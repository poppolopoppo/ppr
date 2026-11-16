#include "world_lighting_codec.slang"
// The one particle shape constant set, shared with the prepare cull.
#include "grid_particle_native.slang"
// Screen-space density kernel and composite mathematics. The 1x accumulation
// and sample-rate composite entries near the end of this file consume it; the
// direct entries below are unchanged.
#include "grid_particle_density.slang"
// The shared receiving-plane mapping backgroundWorldAt: the sample-rate density
// composite reconstructs the same z = 0 plane point the background and the
// terrain do, with the same homogeneous validation.
#include "grid_background_world.slang"

// Grid tile pass: flat tinted or per-cell quads, one instance per visible chunk-sized
// tile. Row-major, row-vector convention: transforms call
// mul(vector, matrix), never mul(matrix, vector). View-projection remains
// view * projection with [0,1] depth, no transpose, no Y flip. Optional immutable
// world radiance and catalog texturing remain content-owned explicit inputs. Catalog sessions use the shared
// rounded quadratic-Bezier contour surface; explicit legacy fixtures keep square cells.
struct BackgroundLayer {
    uint m_texture_index;
    uint m_order;
    uint m_color_rgba;
    float m_repeat_period_m;
    float m_parallax_factor;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};
struct GridFrame {
    // Unjittered view * projection, followed by the per-slot visibility mode.
    float4x4 m_view_projection;
    uint m_use_visible_ids;
    float m_cell_size_m;
    uint m_surface_mode;
    uint m_boundary_material_id;
    float4x4 m_inverse_view_projection;
    float4 m_viewport;
    float2 m_camera_origin;
    uint m_has_background;
    uint m_has_surface;
    BackgroundLayer m_layers[4];
    float m_camera_world_x;
    float m_camera_world_y;
    float m_camera_world_z;
    float m_camera_forward_x;
    float m_camera_forward_y;
    float m_camera_forward_z;
    uint m_camera_ortho;
    // Availability bits for the categorical publication. Bit 0 is the resolved
    // cell and fine-CH publication this terrain pass shades from. Bit 1 is the
    // committed particle-ownership grid (g_best_row) bound for that same
    // submission's projection attempt; it is never set while the zero fallback
    // is bound, so it alone authorizes reading the 1-Mi-cell grid. Consumers
    // that only need the categorical publication test the word as nonzero.
    uint m_has_resolved_cells;
};
static const uint kGridFrameHasResolvedCells = 1u;
static const uint kGridFrameHasParticleOwnership = 2u;

// CPU mirror GridTilePayload: 32 B = 16 rect + 4 material + 4 base
// + 4 flags + 4 pad,
// 16-aligned. m_rect is min_x, min_y, max_x, max_y in world-cell units on
// the z = 0 plane; m_material is the element/material tint index. Field
// order, widths, signedness, and offsets are asserted on the CPU side.
struct GridTilePayload {
    float4 m_rect;
    uint m_material;
    uint m_cell_word_base;
    uint m_flags;
    uint m_pad;
};

// Global scope carries NO ordinary data: a ConstantBuffer nested beside
// $Globals ordinary data is misbound (D3D12SerializeRootSignature then
// reports the generated globalParams b0 overlapping g_grid_frame b1), which
// fails program creation. Frame values belong in the explicit constant buffer.
ConstantBuffer<GridFrame> g_grid_frame;
StructuredBuffer<GridTilePayload> g_tiles;
StructuredBuffer<uint> g_visible_ids;
// Physical slot n occupies 8192 words at n*8192; low halfword = even cell,
// high halfword = odd cell. Only read when payload flag bit 0 is set.
StructuredBuffer<uint> g_cell_materials;
// Uploaded verbatim from engine.math; no separate shader case table.
#include "grid_contour_math.slang"
StructuredBuffer<uint> g_chunk_slots;
// CPU GridTerrainVisual: 96 bytes. Scalar mean components occupy offsets 20/24/28.
// Texture index here is
// the resident descriptor slot, resolved from the catalog's source index.
struct GridVisual {
    uint m_material_id;
    uint m_color_rgba;
    uint m_texture_index;
    float m_texture_period_m;
    float m_roundness;
    // Scalar padding keeps SPIR-V std430 array stride identical to the CPU mirror.
    float m_filtered_mean_r;
    float m_filtered_mean_g;
    float m_filtered_mean_b;
    uint m_normal_texture_index;
    float m_metallic;
    float m_roughness;
    float m_opacity;
    float3 m_emissive_rgb;
    float m_emissive_strength;
    float m_ior;
    float m_normal_scale;
    float m_refraction_depth_m;
    uint m_surface_flags;
    uint m_transport_model;
    float m_sigma_r_per_m, m_sigma_g_per_m, m_sigma_b_per_m;
};
static const uint kSurfaceOpticalBlend = 1u;
static const uint kSurfaceTextureRepeat = 2u;
static const uint kSurfaceParticleSolid = 4u;
static const uint kSurfaceParticleLiquid = 8u;
StructuredBuffer<GridVisual> g_terrain_visuals;
// Terrain pass selector: 0 draws every material in one pass (the legacy
// reference lanes), 1 discards optical-blend (translucent) cells for the
// depth-writing opaque pass, 2 discards opaque cells for the blended
// translucent pass. The same 128x128 chunk quad geometry is drawn twice.
//
// `particle_class` is the particle draw class of the current draw: 0 gas,
// 1 opaque (depth-writing), 2 optical (blended). The class selects depth and
// blend only; the material's own phase flags select the description family.
// The particle vertex entry rejects every instance whose packed class bits are
// not the class this draw owns, before rasterisation, so one draw list serves
// all three classes.
struct GridPassSwitch {
    uint pass;
    uint particle_class;
    // Admitted phase mask of the surface reconstruction. Bit 0 is the liquid
    // phase (kParticleSurfacePhaseLiquid). The direct restored classes reject
    // every non-gas row whose own palette phase is in this mask, so the surface
    // splat and a direct draw never present the same particle. Every valid
    // liquid material of the publication is admitted together, so this is a
    // phase test against each row's own material, never a material-id compare.
    // A gas row keeps its own class path even when its material is a liquid.
    uint surface_phase_mask;
    uint pad2;
};
ConstantBuffer<GridPassSwitch> g_grid_pass;
Texture2D<float4> g_boundary_noise;
// Written before the surface pass; this is never the active color attachment.
Texture2D<float4> g_backdrop;
// Renderer-owned packed 24-byte particle presentation row; the writer is
// PresentationRow in grid_particle_prepare.slang and the CPU twin is
// pP::ParticleGpu in App.Renderer.GridPass.cppm. Position and radius are world
// metres (not cells and not quantised); the material and motion fields are
// packed and the fourth word is the stable-identity UV seed. Every field name
// and offset reflects against the CPU twin.
struct ParticleGpu {
    float2 m_position;                    // 0:  world metres
    float m_radius_m;                     // 8:  metres
    uint m_material_flags_temperature;    // 12: material 12 | class 4 << 12 | binary16 K << 16
    uint m_uv_seed;                       // 16: stable-identity material-UV seed
    half2 m_motion;                       // 20: native cells per solver step
};
static const uint kParticleMaterialMask = 0xfffu;
// Class nibble of material_flags_temperature. It sits beside the material id
// because a gas row and a solid particle can share one material id. Bits 2..3
// are reserved; solid versus liquid is selected by the material's own surface
// flags at shading time.
static const uint kParticleClassGas = 1u;
static const uint kParticleClassOptical = 2u;
// Particle draw class, the per-draw constant in g_grid_pass.particle_class.
// The class is a depth/blend state; it never selects the description family.
static const uint kParticleDrawGas = 0u;
static const uint kParticleDrawOpaque = 1u;
static const uint kParticleDrawOptical = 2u;

// The row's palette tint in the linear r/g/b byte order the packer used, with
// no sRGB decode: the particle response multiplies this by the lighting/cue term
// and never by the palette alpha (that supplies the material opacity separately).
// Deliberately not `diagnosticTint`, whose authored-sRGB decode and coverage
// scale belong to the terrain path.
float3 particleTint(uint rgba) {
    return float3(float((rgba>>24u)&255u),float((rgba>>16u)&255u),float((rgba>>8u)&255u))/255.0;
}

// ------------------------------------------------------------------
// stable-identity seeded particle UV
// ------------------------------------------------------------------
// The row's fourth word is the seed derived at pack time from the existing stable
// 64-bit metadata id (grid_particle_prepare.slang). It is not a palette copy and
// not a per-frame token, so a checkpoint/replay that restores the same ids
// recreates the same offsets with no saved seed state.
//
// The two 16-bit halves plus a half step are the particle's constant offset in
// texture periods. Every value is exactly representable in binary32, and the
// hash bits are portable, so the offset itself is bit-identical on both backends.
float2 particleUvOffset(uint seed) {
    return (float2(float(seed&0xffffu),float(seed>>16u))+0.5)/65536.0;
}

// The particle-attached texture coordinate and its unwrapped gradients: a frame
// translated with the particle centre p, aligned to world XY and never rotated or
// stretched by the conic.
//
//   uv_unwrapped = offset(seed) + (x - p) / texture_period_m
//   uv_dx, uv_dy = d(x - p)/d(raster) / texture_period_m
//
// The offset is a pure translation: it leaves every gradient untouched (a
// constant has zero derivative) and is applied before the sampler's own
// wrap/patch derivation, so a repeating material still samples
// frac(uv_unwrapped) through SampleGrad with these unwrapped gradients and a
// stochastic material still feeds texturePatches(uv_unwrapped) without a prior
// frac. At x == p the sample is the seed offset itself, so the patch is anchored
// to the particle rather than to the viewport or the world origin.
//
// This derivation is not consumed by the draw yet: the direct and splat paths
// still sample the world-space UV, so exposing the seed changes no output.
float2 particleUvUnwrapped(uint seed,float2 local_metres,float period) {
    return particleUvOffset(seed)+local_metres/period;
}Texture2D<float4> g_particle_mask;
SamplerState g_particle_mask_sampler;
StructuredBuffer<ParticleGpu> g_particles;
// Renderer-owned per-particle orientation history, one 8-byte record per packed
// row. The prepare pass writes it fused into rank; this draw only decodes the
// stored world-plane axis and the two amplitudes. Writer: OrientationRow in
// grid_particle_native.slang; CPU twin: pP::ParticleOrientationGpu.
StructuredBuffer<OrientationRow> g_particle_orientation;
// Per-row six-byte conic draw records, pair-packed into three uints per two rows
// by the same prepare rank that advances the canonical history. Writer: the
// pair-packed layout documented in grid_particle_native.slang; the density splat
// vertex decodes one record with two aligned uint loads. A structured buffer of
// uints, exactly like g_particle_ids: the packing is explicit, and the slot
// buffer carries a stride but no format. The direct classes keep their
// orientation silhouette decode and never read this buffer.
StructuredBuffer<uint> g_particle_conic;
// The admitted phase bits of the surface reconstruction. There is one shared
// method for both phases, so the mask names phases, not draws or materials; the
// liquid bit is the only one admitted until the solid migration.
static const uint kParticleSurfacePhaseLiquid = 1u;

// The single admitted surface-reconstruction configuration. The splat vertex
// below reads it for the admitted phase mask and the one-pixel admission
// padding; the selection, accumulation and composite entries read the phase
// mask and the composite class. The direct classes read only
// `g_grid_pass.surface_phase_mask`, which carries the same phase bits.
struct ParticleSurfaceFrame {
    // One density pixel in world-plane metres at the camera's z = 0 plane.
    float m_pixel_world;
    // Phase bits whose rows this surface draw owns.
    uint m_phase_mask;
    // The composite class of the current composite draw (kParticleDrawOpaque or
    // kParticleDrawOptical); zero for the selection and accumulation passes,
    // which serve every admitted class in one draw and let the label choose.
    uint m_composite_class;
    uint m_pad;
};
ConstantBuffer<ParticleSurfaceFrame> g_particle_surface;

// ------------------------------------------------------------------
// particle phase and class admission
// ------------------------------------------------------------------
// One rule for the phase of a packed row's own material, shared by the surface
// vertex (which admits it) and the direct classes (which exclude it). The
// palette identity guard means an unknown id has no phase rather than inheriting
// entry zero. A material authored as both phases is solid, exactly as the
// existing description selection reads it; gas is a draw class, not a phase, and
// keeps its direct mask/geometry path.
uint particleMaterialPhase(uint material) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    if (visual.m_material_id != material) return 0u;
    bool liquid = (visual.m_surface_flags & kSurfaceParticleLiquid) != 0u;
    bool solid = (visual.m_surface_flags & kSurfaceParticleSolid) != 0u;
    return liquid && !solid ? kParticleSurfacePhaseLiquid : 0u;
}

// The composite class a label's own palette record selects. Opaque and optical
// liquid composites read the same label and the same reconstructed attributes;
// exactly one of them owns the sample.
uint particleSurfaceClass(uint material) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    return (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u ? kParticleDrawOptical : kParticleDrawOpaque;
}
struct DiagnosticColor {
    uint m_color_rgba;
    float m_fill_height_fine;
    uint2 m_reserved;
};
StructuredBuffer<uint> g_diagnostic_indices;
StructuredBuffer<DiagnosticColor> g_diagnostic_colors;

// ------------------------------------------------------------------
// visible categorical material and directional lighting
// ------------------------------------------------------------------
// The world-lighting owner publishes one packed material+temperature word per
// native cell, the decoded fine CH field, the derived source-material palette
// and the device's one thermal table. GridPass owns the immutable BRDF table.
static const uint kResolvedEdge = kParticleNativeEdge;
StructuredBuffer<uint> g_resolved_cells;
// Committed per-native-cell winner grid from the same projection attempt, one
// word per cell; 0xffffffff means no particle winner. Terrain reads it only to
// retire the categorical base of particle-owned cells (plan §3), the particle
// draw to occlude. It is read-only here and never identifies ownership by
// material. The Q16 fill accumulation of the same attempt is not read here: the
// prepare pass's fused target is its only consumer now.
StructuredBuffer<uint> g_best_row;
StructuredBuffer<PackedCH> g_fine_ch;
#include "world_lighting_ch.slang"
#include "thermal_emission.slang"
#include "world_lighting_material.slang"
StructuredBuffer<SourceMaterial> g_source_materials;
Texture2D<float2> g_brdf_lut;
SamplerState g_brdf_sampler;

uint diagnosticIndex(int2 sample) {
    if (any(sample < int2(0)) || any(sample >= int2(4096))) { return 0u; }
    uint2 cell = uint2(sample);
    uint chunk = (cell.y / 128u) * 32u + cell.x / 128u;
    uint index = (cell.y % 128u) * 128u + cell.x % 128u;
    uint packed = g_diagnostic_indices[chunk * 8192u + index / 2u];
    return ((index & 1u) != 0u) ? (packed >> 16u) : (packed & 0xffffu);
}

float4 diagnosticTint(uint rgba, float coverage) {
    float4 tint = float4((rgba >> 24u) & 255u, (rgba >> 16u) & 255u,
        (rgba >> 8u) & 255u, rgba & 255u) / 255.0;
    tint.rgb = select(tint.rgb <= 0.04045, tint.rgb / 12.92,
        pow((tint.rgb + 0.055) / 1.055, float3(2.4)));

    // The normalized RGB endpoint is exactly1. Shader pow approximation can
    // otherwise exceed it and violate the admitted material reflectance domain.
    tint.rgb = select(uint3((rgba >> 24u) & 255u, (rgba >> 16u) & 255u, (rgba >> 8u) & 255u) == 255u,
        float3(1.0), tint.rgb);
    return float4(tint.rgb, tint.a * coverage);
}

struct GridVertexOutput {
    float4 position : SV_Position;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint cell_word_base : TEXCOORD1;
    nointerpolation uint flags : TEXCOORD2;
    float2 local_cell : TEXCOORD0;
    float2 fine_position : TEXCOORD3;
};

// Deterministic tint from the low 3 bits: the 8 corners of a dim/bright
// cube, so adjacent element ids never alias. No palette table, no texture.
float3 gridTint(uint material) {
    uint index = material % 8u;
    float red = ((index & 1u) != 0u) ? 0.85 : 0.15;
    float green = ((index & 2u) != 0u) ? 0.85 : 0.15;
    float blue = ((index & 4u) != 0u) ? 0.85 : 0.15;
    return float3(red, green, blue);
}

// Every payload table begins at zero. GPU visibility supplies compact indices;
// the reference indirect lane supplies instance offsets in its draw arguments.
// No vertex ordinary-data range is needed alongside the fragment sampler handle.
[shader("vertex")]
GridVertexOutput vertexGridMain(
    uint sv_vertex_id : SV_VertexID,
    uint sv_instance_id : SV_InstanceID) {
    uint tile_id = (g_grid_frame.m_use_visible_ids != 0u) ? g_visible_ids[sv_instance_id] : sv_instance_id;
    GridTilePayload tile = g_tiles[tile_id];
    float2 min_xy = tile.m_rect.xy;
    float2 max_xy = tile.m_rect.zw;
    // Six non-indexed verts: two triangles over the tile rect. Rasterizer
    // culling is off for this pipeline, so the winding is don't-care; it
    // stays consistent anyway.
    float2 corner = min_xy;
    if (sv_vertex_id == 1u) {
        corner = float2(max_xy.x, min_xy.y);
    } else if (sv_vertex_id == 2u) {
        corner = float2(min_xy.x, max_xy.y);
    } else if (sv_vertex_id == 3u) {
        corner = float2(min_xy.x, max_xy.y);
    } else if (sv_vertex_id == 4u) {
        corner = float2(max_xy.x, min_xy.y);
    } else if (sv_vertex_id == 5u) {
        corner = max_xy;
    }
    GridVertexOutput output;
    // Row-vector convention (mirrors the CPU Mango transform): translation
    // in row 3, positive clip.w for +Z view-space points. Tiles live on the
    // z = 0 plane.
    float4 world = float4(corner * g_grid_frame.m_cell_size_m, 0.0, 1.0);
    output.position = mul(world, g_grid_frame.m_view_projection);
    output.material = tile.m_material;
    output.cell_word_base = tile.m_cell_word_base;
    output.flags = tile.m_flags;
    output.local_cell = corner - min_xy;
    output.fine_position = corner;
    return output;
}

// Global sample addressing includes diagonal neighbors. Outside samples are
// protected solid and never form a buffer address. Missing inside residency is
// empty during incremental candidate preparation.
uint terrainSample(int2 sample) {
    if (any(sample < int2(0)) || any(sample >= int2(4096))) {
        return g_grid_frame.m_boundary_material_id;
    }
    uint2 cell = uint2(sample);
    uint chunk = (cell.y / 128u) * 32u + cell.x / 128u;
    uint slot = g_chunk_slots[chunk];
    if (slot == 0xffffffffu) {
        return 0u;
    }
    uint index = (cell.y % 128u) * 128u + cell.x % 128u;
    uint packed = g_cell_materials[slot * 8192u + index / 2u];
    return ((index & 1u) != 0u) ? (packed >> 16u) : (packed & 0xffffu);
}

float materialRoundness(uint material) {
    return (g_grid_frame.m_surface_mode & 2u) != 0u ? g_terrain_visuals[material].m_roundness : 1.0;
}

// Two solid labels share one curve, including complementary saddle cases.
// Other junctions use signed-distance partitioning inside the solid union.
bool sharedSolidRoundness(uint materials[4], out float roundness) {
    roundness = 0.0;
    uint first = materials[0];
    uint second = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        uint label = materials[corner];
        if (label == 0u || label == 0xffffu) { return false; }
        if (label == first) { continue; }
        if (second != 0u && label != second) { return false; }
        second = label;
    }
    if (second == 0u) { return false; }
    roundness = contourMeanRoundness(materialRoundness(first), materialRoundness(second));
    return true;
}

// Smooth materials use quintic texel interpolation; sharp materials retain
// angular ramps with broad plateaus. The world-space lattice never follows zoom.
float2 boundaryNoise(float2 position, float period, float roundness,
    float footprint, SamplerState.Handle sampler) {
    float2 texel = position * (64.0 / period) - 0.5;
    float2 fraction = frac(texel);
    float2 angular = saturate(2.0 * fraction - 0.5);
    float2 smooth = fraction * fraction * fraction * (fraction * (6.0 * fraction - 15.0) + 10.0);
    float2 uv = (floor(texel) + lerp(angular, smooth, roundness) + 0.5) / 64.0;
    // Both interpolation profiles have slope at most two. The Frobenius
    // footprint conservatively filters even angular plateaus under minification.
    float lod = clamp(log2(max(128.0 * footprint / period, 1.0)), 0.0, 6.0);
    float lower = floor(lod);
    float blend = smoothstep(0.0, 1.0, frac(lod));
    // The snapped window changes filtering only: both mips sample identical UVs,
    // so zooming cannot slide, rescale or replace the underlying spatial field.
    float2 fine = g_boundary_noise.SampleLevel(sampler, uv, lower).rg;
    float2 coarse = g_boundary_noise.SampleLevel(sampler, uv, min(lower + 1.0, 6.0)).rg;
    return lerp(fine, coarse, blend) * 2.0 - 1.0;
}

// Persistent macro and detail bands add together, rather than morphing between
// spatial octaves as zoom changes. One shared style keeps material sides joined.
float2 boundaryDisplacement(float2 position, float2 pixel_x, float2 pixel_y, SamplerState.Handle sampler) {
    int2 base = int2(floor(position - 0.5));
    float roundness = 0.0;
    float solids = 0.0;
    for (uint corner = 0u; corner < 4u; ++corner) {
        uint label = terrainSample(base + int2(g_contour_points[corner]));
        if (label == 0u || label == 0xffffu) { continue; }
        roundness += materialRoundness(label);
        solids += 1.0;
    }
    roundness = solids > 0.0 ? roundness / solids : 1.0;
    float footprint = max(sqrt(dot(pixel_x, pixel_x) + dot(pixel_y, pixel_y)), 1.0e-6);
    float2 macro = boundaryNoise(position, 512.0, roundness, footprint, sampler);
    float2 detail = boundaryNoise(position, 16.0, roundness, footprint, sampler);
    // Fixed gains preserve displacement at every roundness. Fine detail is
    // 32 times the macro frequency; their combined component bound is 0.086 cells.
    float2 displacement = 0.075 * macro + 0.011 * detail;
    float2 local = frac(position - 0.5);
    float2 envelope = 16.0 * local * local * (1.0 - local) * (1.0 - local);
    // The harmonic mean preserves edge anchoring without multiplying away
    // visible displacement in the interior. Both components still vanish C1
    // on every cell edge, even where neighboring material mixes differ.
    float shared_envelope = 2.0 * envelope.x * envelope.y / (envelope.x + envelope.y + 1.0e-6);
    float fade = 1.0 - smoothstep(0.25, 0.75, footprint);
    return displacement * shared_envelope * fade;
}

float contourPixelSize(float2 normal, float2 pixel_x, float2 pixel_y) {
    return max(length(float2(dot(normal, pixel_x), dot(normal, pixel_y))), 1.0e-6);
}

// A contour endpoint crosses one dual-cell edge. Only the solid on that edge
// contributes roundness, so a remote third label cannot deform this outer arc.
float outerEndpointRoundness(uint materials[4], float2 endpoint) {
    uint corner = endpoint.y == 0.0 ? 0u : endpoint.x == 1.0 ? 1u : endpoint.y == 1.0 ? 2u : 3u;
    uint label = materials[corner];
    if (label == 0u || label == 0xffffu) { label = materials[(corner + 1u) & 3u]; }
    return materialRoundness(label);
}

float outerSegmentRoundness(uint materials[4], uint2 segment) {
    return contourMeanRoundness(outerEndpointRoundness(materials, g_contour_points[segment.x]),
        outerEndpointRoundness(materials, g_contour_points[segment.y]));
}

float occupiedCoverage(uint materials[4], float2 point, float width) {
    uint mask = occupiedMask(materials);
    if (mask == 0u) { return 0.0; }
    if (mask == 15u) { return 1.0; }
    bool connected = occupiedSaddleConnected(materials, mask);
    uint shape_mask = connected ? 15u ^ mask : mask;
    ContourCase shape = g_contour_cases[shape_mask];
    float coverage = 0.0;
    if ((mask == 5u || mask == 10u) && !connected) {
        // Different diagonal solids separated by empty corners remain separate.
        for (uint corner = 0u; corner < 4u; ++corner) {
            if ((mask & (1u << corner)) == 0u) { continue; }
            float2 gradient;
            float score = contourScore(point, 1u << corner, materialRoundness(materials[corner]), false, gradient);
            coverage += saturate(0.5 + score / width);
        }
    } else {
        float2 gradient;
        float roundness = outerSegmentRoundness(materials, shape.m_segments[0]);
        coverage = saturate(0.5 + contourScore(point, shape_mask, roundness, connected, gradient) / width);
    }
    return saturate(coverage);
}

// Point occupancy for the selected MSAA sample. Keep the same authored contour
// and saddle topology; smooth AA ramps are not geometry and must not be summed
// or multiplied into alpha after hardware sample coverage has been selected.
float occupiedPoint(uint materials[4], float2 point) {
    uint mask = occupiedMask(materials);
    if (mask == 0u) { return 0.0; }
    if (mask == 15u) { return 1.0; }
    bool connected = occupiedSaddleConnected(materials, mask);
    uint shape_mask = connected ? 15u ^ mask : mask;
    ContourCase shape = g_contour_cases[shape_mask];

    if ((mask == 5u || mask == 10u) && !connected) {
        for (uint corner = 0u; corner < 4u; ++corner) {
            if ((mask & (1u << corner)) == 0u) { continue; }
            float2 gradient;
            if (contourScore(point, 1u << corner, materialRoundness(materials[corner]), false, gradient) >= 0.0) {
                return 1.0;
            }
        }
        return 0.0;
    }

    float2 gradient;
    float roundness = outerSegmentRoundness(materials, shape.m_segments[0]);
    return contourScore(point, shape_mask, roundness, connected, gradient) >= 0.0 ? 1.0 : 0.0;
}

struct TerrainStencil {
    int2 base;
    uint labels[16];
};

TerrainStencil terrainStencil(float2 position) {
    TerrainStencil stencil;
    stencil.base = int2(floor(position - 0.5));
    // Decoration reaches at most one neighboring dual cell. All its corners
    // and the central contour fit this exact 4x4 fragment-local stencil.
    for (uint y = 0u; y < 4u; ++y) {
        for (uint x = 0u; x < 4u; ++x) {
            stencil.labels[y * 4u + x] = terrainSample(stencil.base + int2(x, y) - 1);
        }
    }
    return stencil;
}

uint stencilSample(TerrainStencil stencil, int2 offset) {
    return stencil.labels[(offset.y + 1) * 4 + offset.x + 1];
}

uint contourMaterial(float2 position, float2 pixel_x, float2 pixel_y, float width, TerrainStencil stencil,
    out float coverage, out int2 winner, out float internal_outline, bool point_sample = false) {
    winner = int2(4096);
    internal_outline = 0.0;
    int2 base = stencil.base;
    float2 point = position - (float2(base) + 0.5);
    uint materials[4];
    float scores[4];
    float2 gradients[4];
    for (uint corner = 0u; corner < 4u; ++corner) {
        materials[corner] = stencilSample(stencil, int2(g_contour_points[corner]));
        scores[corner] = -1.0e30;
        gradients[corner] = float2(0.0);
    }
    // Identical labels have neither a material interface nor partial occupancy.
    // Decoration still examines neighbors: an exterior rim can enter this quad.
    if (materials[0] == materials[1] && materials[0] == materials[2] && materials[0] == materials[3]) {
        uint label = materials[0];
        coverage = label == 0u || label == 0xffffu ? 0.0 : 1.0;
        if (coverage == 0.0) { return 0u; }
        // Nearest-corner diagnostics retain lower-Y, then lower-X ties.
        winner = base + int2(point.x > 0.5 ? 1 : 0, point.y > 0.5 ? 1 : 0);
        return label;
    }
    coverage = point_sample ? occupiedPoint(materials, point) : occupiedCoverage(materials, point, width);
    float interface_roundness;
    bool shared_interface = sharedSolidRoundness(materials, interface_roundness);
    uint visited = 0u;
    uint candidates = 0u;
    uint selected = 0u;
    uint material = 0u;
    float best = -1.0e30;
    for (uint corner = 0u; corner < 4u; ++corner) {
        uint label = materials[corner];
        if (label == 0u || label == 0xffffu || (visited & (1u << corner)) != 0u) { continue; }
        uint mask = materialMask(materials, corner);
        visited |= mask;
        candidates |= 1u << corner;
        bool connected = connectedSaddle(materials, corner, mask);
        uint shape_mask = connected ? 15u ^ mask : mask;
        float roundness = shared_interface ? interface_roundness : materialRoundness(label);
        scores[corner] = contourScore(point, shape_mask, roundness, connected, gradients[corner]);
        if (material != 0u && scores[corner] <= best) { continue; }
        best = scores[corner];
        selected = corner;
        material = label;
    }
    // A single centered two-pixel stroke: one-pixel half-width on either side.
    // Fade still uses its full width, matching the authored outer-rim tuning.
    for (uint corner = 0u; corner < 4u; ++corner) {
        if ((candidates & (1u << corner)) == 0u || corner == selected) { continue; }
        float2 gradient = gradients[selected] - gradients[corner];
        float gradient_length = length(gradient);
        if (gradient_length < 1.0e-6) { continue; }
        float pixel = contourPixelSize(gradient / gradient_length, pixel_x, pixel_y);
        float distance_pixels = (best - scores[corner]) / (gradient_length * pixel);
        float fade = 1.0 - smoothstep(0.2, 0.3, 2.0 * pixel);
        internal_outline = max(internal_outline, fade * (1.0 - smoothstep(0.5, 1.5, distance_pixels)));
    }
    float closest = 1.0e30;
    for (uint corner = 0u; corner < 4u; ++corner) {
        if (materials[corner] != material || material == 0u) { continue; }
        float2 delta = point - g_contour_points[corner];
        float distance = dot(delta, delta);
        int2 coordinate = base + int2(g_contour_points[corner]);
        bool before = coordinate.y < winner.y || (coordinate.y == winner.y && coordinate.x < winner.x);
        if (distance < closest || (distance == closest && before)) {
            closest = distance;
            winner = coordinate;
        }
    }
    return material;
}

// Only occupied-vs-empty curves produce exterior rims and shadows. Internal
// label curves are scores, not decoration geometry, so stitched joins stay clean.
float2 contourDecoration(float2 position, float2 pixel_x, float2 pixel_y, uint material, bool need_shadow,
    TerrainStencil stencil) {
    int2 base = stencil.base;
    float2 local = position - (float2(base) + 0.5);
    float pixel_bound = length(float2(length(pixel_x), length(pixel_y)));
    float pixel_min = sqrt(max(min(dot(pixel_x, pixel_x), dot(pixel_y, pixel_y)) -
        abs(dot(pixel_x, pixel_y)), 0.0));
    bool need_outline = material != 0u && material != 0xffffu && pixel_min < 0.15;
    float radius = max(need_outline ? min(2.5 * pixel_bound, 0.375) : 0.0,
        need_shadow ? min(12.0 * pixel_bound, 1.0) : 0.0);
    if (radius == 0.0) { return float2(0.0); }
    int extent = int(ceil(radius));
    float outline = 0.0;
    float shadow = 0.0;
    for (int y = -extent; y <= extent; ++y) {
        for (int x = -extent; x <= extent; ++x) {
            float2 point = local - float2(x, y);
            float2 box_distance = max(max(-point, point - 1.0), 0.0);
            if (dot(box_distance, box_distance) >= radius * radius) { continue; }
            uint materials[4];
            for (uint corner = 0u; corner < 4u; ++corner) {
                materials[corner] = stencilSample(stencil, int2(x, y) + int2(g_contour_points[corner]));
            }
            uint mask = occupiedMask(materials);
            if (mask == 0u || mask == 15u) { continue; }
            bool connected = occupiedSaddleConnected(materials, mask);
            ContourCase shape = g_contour_cases[connected ? 15u ^ mask : mask];
            for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
                float2 a = g_contour_points[shape.m_segments[edge].x];
                float2 b = g_contour_points[shape.m_segments[edge].y];
                float roundness = outerSegmentRoundness(materials, shape.m_segments[edge]);
                float2 control = lerp((a + b) * 0.5, float2(0.5), roundness);
                float2 low = min(a, b), high = max(a, b);
                if (a.x != b.x && a.y != b.y) {
                    low = min(low, control); high = max(high, control);
                }
                float2 box = max(max(low - point, point - high), 0.0);
                if (dot(box, box) >= radius * radius) { continue; }
                float2 nearest, normal;
                float distance = roundedSegmentDistance(point, shape.m_segments[edge], roundness, nearest, normal);
                float pixel = contourPixelSize(normal, pixel_x, pixel_y);
                if (need_shadow) {
                    float depth = min(12.0 * pixel, 1.0);
                    shadow = max(shadow, 1.0 - smoothstep(0.0, depth, distance));
                }
                if (need_outline) {
                    float width = 2.0 * pixel;
                    float fade = 1.0 - smoothstep(0.2, 0.3, width);
                    float aa = 0.5 * pixel;
                    outline = max(outline, fade * (1.0 - smoothstep(width - aa, width + aa, distance)));
                }
            }
        }
    }
    return float2(outline, shadow);
}

float3 decorateTerrain(float3 color, float2 decoration) {
    // Exterior rims and centered material borders share a two-pixel total width.
    return lerp(color, float3(0.025), decoration.x);
}

// Terrain and initial matter cover complementary parts of one contour pixel.
float4 compositeMatter(float4 terrain, float4 matter) {
    float alpha = terrain.a + matter.a;
    return alpha > 0.0 ? float4((terrain.rgb * terrain.a + matter.rgb * matter.a) / alpha, alpha) : float4(0.0);
}

float2 backdropUv(float2 screen) {
    float2 extent = max(g_grid_frame.m_viewport.zw, float2(1.0));
    return clamp((screen - g_grid_frame.m_viewport.xy) / extent,
        0.5 / extent, 1.0 - 0.5 / extent);
}

float3 backdropColor(float2 screen, SamplerState.Handle sampler) {
    return g_backdrop.SampleLevel(sampler, backdropUv(screen), 0.0).rgb;
}

struct TexturePatches {
    float2 uv0;
    float2 uv1;
    float2 uv2;
    float3 weights;
};

float2 patchTranslation(int2 vertex) {
    uint2 bounded = uint2(vertex) & 0xffffu;
    uint value = bounded.x * 0x9e3779b9u ^ bounded.y * 0x85ebca6bu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return float2(value & 0xffffu, value >> 16u) / 65536.0;
}

TexturePatches texturePatches(float2 uv) {
    // Equilateral triangular lattice: neighboring triangles share translated
    // patches and barycentric weights vanish continuously at their edges.
    // Half-scale arithmetic cannot overflow for admitted nonnegative world UVs.
    // Wrap the hash lattice before integer conversion; modulo vertex hashing
    // keeps shared triangles continuous at that bounded domain's seam.
    float2 half_lattice = float2(uv.x * 0.5 - uv.y * 0.288675135, uv.y * 0.577350269);
    float2 lattice = fmod(half_lattice, 32768.0) * 2.0;
    int2 cell = int2(floor(lattice));
    float2 fraction = frac(lattice);
    int2 vertices[3];
    TexturePatches patches;
    if (fraction.x + fraction.y <= 1.0) {
        vertices[0] = cell;
        vertices[1] = cell + int2(1, 0);
        vertices[2] = cell + int2(0, 1);
        patches.weights = float3(1.0 - fraction.x - fraction.y, fraction.x, fraction.y);
    } else {
        vertices[0] = cell + int2(1, 1);
        vertices[1] = cell + int2(0, 1);
        vertices[2] = cell + int2(1, 0);
        patches.weights = float3(fraction.x + fraction.y - 1.0, 1.0 - fraction.x, 1.0 - fraction.y);
    }
    // Translation only preserves authored strata direction and the normal frame.
    float2 repeat_uv = frac(uv);
    patches.uv0 = repeat_uv + patchTranslation(vertices[0]);
    patches.uv1 = repeat_uv + patchTranslation(vertices[1]);
    patches.uv2 = repeat_uv + patchTranslation(vertices[2]);
    return patches;
}

float3 decodePatchNormal(float3 value) {
    float3 decoded = value * 2.0 - 1.0;
    decoded.xy = select(abs(decoded.xy) <= (1.0 / 255.0 + 1.0e-6), float2(0.0), decoded.xy);
    return decoded;
}

float4 decorateSurface(float4 surface, float2 decoration) {
    // A solid boundary has its own opacity, independent of optical opacity.
    // Normalize premultiplied contributions for the pass's straight-alpha blend.
    float rim = saturate(decoration.x);
    float optical = surface.a * (1.0 - rim);
    float alpha = optical + rim;
    float3 color = alpha > 0.0 ? (surface.rgb * optical + float3(0.025) * rim) / alpha : surface.rgb;
    return float4(color, alpha);
}

// ------------------------------------------------------------------
// visible categorical material response
// ------------------------------------------------------------------

// Native-cell index of the continuous world-plane sample, or 0xffffffff when
// it lies outside the published domain. The resolved word and the committed
// ownership word are both addressed by this same index.
uint resolvedCellIndex(float2 fine_position) {
    int2 cell = int2(floor(fine_position));
    if (any(cell < int2(0)) || any(cell >= int2(int(kResolvedEdge)))) return 0xffffffffu;
    return uint(cell.y) * kResolvedEdge + uint(cell.x);
}

// Decode all nine coefficients at each bilinear tap, then interpolate them at
// the continuous shading location before any directional evaluation.
CH sampleFineCH(float2 world_cell_position) {
    float2 fine = world_cell_position * 0.5 - 0.5;
    int2 base = int2(floor(fine));
    float2 fraction = fine - float2(base);
    int2 last = int2(int(kFineCHEdge) - 1);
    CH result;
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
        result.m_coefficients[coefficient] = float3(0.0);
    }

    for (uint tap = 0u; tap < 4u; ++tap) {
        int2 offset = int2(int(tap % 2u), int(tap / 2u));
        float weight =
            (offset.x == 0 ? 1.0 - fraction.x : fraction.x) *
            (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        if (weight <= 0.0) continue;

        int2 index = clamp(base + offset, int2(0), last);
        CH decoded = unpackCH(g_fine_ch[index.y * int(kFineCHEdge) + index.x]);
        for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
            result.m_coefficients[coefficient] += decoded.m_coefficients[coefficient] * weight;
        }
    }

    return result;
}

// V points from the world-plane sample to the camera eye. The shared helper
// applies the same formula the reviewed material probes exercise.
float3 surfaceToCamera(float2 fine_position) {
    float3 camera_origin = float3(
        g_grid_frame.m_camera_world_x, g_grid_frame.m_camera_world_y, g_grid_frame.m_camera_world_z);
    float3 camera_forward = float3(
        g_grid_frame.m_camera_forward_x, g_grid_frame.m_camera_forward_y,
        g_grid_frame.m_camera_forward_z);
    return planeSurfaceToCamera(
        fine_position, g_grid_frame.m_cell_size_m, camera_origin, camera_forward,
        g_grid_frame.m_camera_ortho != 0u);
}

// Raw weighted texture albedo before tint and filtered-mean normalization.
float4 sampleVisibleAlbedo(GridVisual visual, TexturePatches patches, bool repeat_texture,
    float2 uv_gradient_x, float2 uv_gradient_y,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    float4 a0 = nonuniform(textures[visual.m_texture_index]).SampleGrad(
        sampler, patches.uv0, uv_gradient_x, uv_gradient_y);
    float4 a1 = float4(0.0), a2 = float4(0.0);
    if (!repeat_texture) {
        a1 = nonuniform(textures[visual.m_texture_index]).SampleGrad(
            sampler, patches.uv1, uv_gradient_x, uv_gradient_y);
        a2 = nonuniform(textures[visual.m_texture_index]).SampleGrad(
            sampler, patches.uv2, uv_gradient_x, uv_gradient_y);
    }

    return a0 * patches.weights.x + a1 * patches.weights.y + a2 * patches.weights.z;
}

// The approved filtered-mean variance normalization for stochastic (non-repeat)
// materials; alpha keeps ordinary barycentric blending. Kept separate from the
// tint so the direct gas path can reuse it without adopting the terrain tint's
// authored-sRGB decode.
float4 albedoVarianceNormalized(GridVisual visual, float4 albedo, float3 weights, bool repeat_texture) {
    if (!repeat_texture) {
        float3 mean = float3(visual.m_filtered_mean_r, visual.m_filtered_mean_g, visual.m_filtered_mean_b);
        albedo.rgb = saturate(mean + (albedo.rgb - mean) * rsqrt(dot(weights, weights)));
    }

    return albedo;
}

// Linear tint times the visible albedo.
float4 normalizeVisibleAlbedo(
    GridVisual visual, float4 albedo, float3 weights, bool repeat_texture) {
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    float4 normalized = albedoVarianceNormalized(visual, albedo, weights, repeat_texture);
    return float4(tint.rgb * normalized.rgb, normalized.a);
}

float4 visibleBaseColor(GridVisual visual, TexturePatches patches, bool repeat_texture,
    float2 uv_gradient_x, float2 uv_gradient_y,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    float4 albedo = sampleVisibleAlbedo(
        visual, patches, repeat_texture, uv_gradient_x, uv_gradient_y, textures, sampler);
    return normalizeVisibleAlbedo(visual, albedo, patches.weights, repeat_texture);
}

// Authored normal-map decode, orientation and scale in the same world XY basis
// as the CH field.
float3 sampleVisibleNormal(GridVisual visual, TexturePatches patches, bool repeat_texture,
    float2 uv_gradient_x, float2 uv_gradient_y,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    float3 normal = float3(0.0, 0.0, 1.0);
    if (visual.m_normal_texture_index == 0xffffffffu) return normal;

    float3 n0 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
        sampler, patches.uv0, uv_gradient_x, uv_gradient_y).xyz);
    float3 n1 = float3(0.0), n2 = float3(0.0);
    if (!repeat_texture) {
        n1 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
            sampler, patches.uv1, uv_gradient_x, uv_gradient_y).xyz);
        n2 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
            sampler, patches.uv2, uv_gradient_x, uv_gradient_y).xyz);
    }

    float3 decoded = n0 * patches.weights.x + n1 * patches.weights.y + n2 * patches.weights.z;
    decoded.xy *= visual.m_normal_scale;
    if (all(isfinite(decoded)) && decoded.z > 0.0 && dot(decoded, decoded) > 1.0e-8) {
        normal = normalize(decoded);
    }

    return normal;
}

// Occupancy is opaque classification, not brightness or a nonzero material ID:
// material zero is empty, a non-emissive receive-only (optical blend) surface is
// transparent, and every other resolved material is opaque regardless of its
// emission. This mirrors the shared source classification without reusing the
// codec's identically named mirror struct.
bool categoricalCellOccupied(uint resolved, GridVisual visual) {
    if ((resolved & 0xffffu) == 0u) return false;
    bool receive_only =
        (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u && visual.m_emissive_strength == 0.0;
    return !receive_only;
}

// ------------------------------------------------------------------
// attribute-taking visible material response
// ------------------------------------------------------------------
// One surface's own sampled material attributes, before any lighting: the
// linear tint*albedo base, the decoded unit normal, the palette authoring alpha
// and the canonical-temperature thermal radiance the categorical response
// consumes. The response helpers below take exactly these plus the physical
// sample position, so the same response can shade a reconstruction of several
// particles' attributes instead of one particle's texture sample.
struct VisibleSurfaceSample {
    // rgb: linear tint * sampled albedo (reflectance); a: sampled texture alpha.
    float4 m_base;
    // Unit world-plane normal after the element's decode, scale and patch mix.
    float3 m_normal;
    // thermalRGB of the row's canonical half-temperature, or zero when the
    // material's response is not emission-eligible. Unused by the liquid policy.
    float3 m_thermal;
    // Palette tint alpha: the authored factor of the optical opacity rule.
    float m_tint_alpha;
    // Reconstructed surface alpha. The deferred composite already formed
    // alphaBar = sum(w*alpha_p)/sum(w) from G.a, which includes the palette tint
    // alpha and authored opacity exactly once, so `m_alpha_reconstructed` tells
    // the response to use this value instead of re-deriving it. Every direct
    // sampling stage leaves the flag false.
    float m_alpha;
    bool m_alpha_reconstructed;
};

// The one surface-alpha rule: opaque classes are exactly one, an optical surface
// is the authored palette tint alpha times the sampled albedo alpha times the
// authored opacity. The direct responses and the deferred attribute accumulation
// both read this, so alpha_p is formed once and never re-applied.
float visibleSurfaceAlpha(GridVisual visual,VisibleSurfaceSample sample) {
    bool optical_blend=(visual.m_surface_flags&kSurfaceOpticalBlend)!=0u;
    return optical_blend ? saturate(sample.m_tint_alpha*sample.m_base.a*visual.m_opacity) : 1.0;
}

// The physical inputs a response reads beyond position and attributes: the
// raster position the refraction offset is expressed in, the caller's
// decoration, and the retained pre-render background copy.
struct VisibleSurfaceBackdrop {
    float2 m_screen;
    float2 m_decoration;
    bool m_reuse_background;
    float3 m_unshifted_background;
};

// Current categorical/solid response: the CH diffuse/specular evaluation, the
// BRDF pair, eligible authored emission and the (already converted) thermal
// term. Material parameters stay per-label palette constants.
float4 categoricalSurfaceResponse(uint material,uint resolved,float2 fine_position,
    CH coefficients,VisibleSurfaceSample sample,bool premultiplied) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float metallic = visual.m_metallic;
    float roughness = visual.m_roughness;
    float eta = visual.m_ior;

    float3 view = surfaceToCamera(fine_position);
    float n_dot_v = visibleNoV(sample.m_normal, view);
    float2 environment = brdfLutPair(g_brdf_lut, g_brdf_sampler, n_dot_v, roughness);
    // `resolved` is the canonical material|temperature word the caller resolved
    // for this sample, so occupancy (and therefore authored emission) is decided
    // from that word and its matched palette record, never from brightness.
    // The approved transport surface scope also bounds visible emission:
    // receive-only transparent cells and gas never acquire a thermal or authored
    // glow, while opaque surfaces and the approved emissive-liquid class do.
    bool occupied = categoricalCellOccupied(resolved, visual);
    float3 authored = occupied ? unpackSourceEmission(g_source_materials[material]) : float3(0.0);
    float3 hdr = visibleMaterialResponse(
        sample.m_base.rgb, metallic, roughness, eta, sample.m_normal, view, coefficients, environment, authored,
        sample.m_thermal);

    bool optical_blend = (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u;
    float opacity = sample.m_alpha_reconstructed
        ? (optical_blend ? saturate(sample.m_alpha) : 1.0) : visibleSurfaceAlpha(visual, sample);
    if (premultiplied) hdr *= opacity;
    return float4(hdr, opacity);
}

// Current liquid response: the fixed local relief/facing term, bounded palette
// emission and the existing true-position refraction/absorption against the
// retained background copy, then one premultiplication. It has neither CH nor
// canonical-particle thermal input.
float4 liquidSurfaceResponse(uint material,float2 fine_position,VisibleSurfaceSample sample,
    VisibleSurfaceBackdrop backdrop,SamplerState.Handle sampler,bool premultiplied) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float2 world = fine_position * g_grid_frame.m_cell_size_m;
    float3 normal = sample.m_normal;
    float3 color = sample.m_base.rgb;
    // Fixed local relief response uses PBR parameters without a world light/GI
    // dependency. Roughness widens the facing lobe; metallic colors its F0.
    float facing = saturate(dot(normal, normalize(float3(-0.35, 0.45, 1.0))));
    float3 f0 = lerp(float3(0.04), color, visual.m_metallic);
    if (visual.m_normal_texture_index != 0xffffffffu) {
        color = color * (0.72 + 0.28 * facing) + f0 * pow(facing, lerp(48.0, 2.0, visual.m_roughness)) * 0.12;
    }
    // Self color only, bounded without overflowing large finite strengths.
    color += 1.0 - exp(-visual.m_emissive_rgb * min(visual.m_emissive_strength, 80.0));
    bool optical_blend = (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u;
    float opacity = sample.m_alpha_reconstructed
        ? (optical_blend ? saturate(sample.m_alpha) : 1.0) : visibleSurfaceAlpha(visual, sample);
    if (g_grid_frame.m_has_background == 0u) {
        float4 surface=decorateSurface(float4(color,opacity),backdrop.m_decoration);
        if (premultiplied) surface.rgb*=surface.a;
        return surface;
    }
    if (opacity >= 1.0) { return decorateSurface(float4(color, 1.0), backdrop.m_decoration); }
    float2 offset_pixels = float2(0.0);
    if (optical_blend && visual.m_ior > 1.0 && visual.m_refraction_depth_m > 0.0 && any(normal.xy != 0.0)) {
        float2 displacement = normal.xy / max(normal.z, 0.25) *
            ((1.0 - 1.0 / visual.m_ior) * visual.m_refraction_depth_m);
        float4 start = mul(float4(world, 0.0, 1.0), g_grid_frame.m_view_projection);
        float4 shifted = mul(float4(world + displacement, 0.0, 1.0), g_grid_frame.m_view_projection);
        if (abs(start.w) > 1.0e-6 && abs(shifted.w) > 1.0e-6) {
            float2 delta_ndc = shifted.xy / shifted.w - start.xy / start.w;
            offset_pixels = delta_ndc * float2(0.5, -0.5) * g_grid_frame.m_viewport.zw;
            offset_pixels *= min(1.0, 8.0 / max(length(offset_pixels), 1.0e-6));
        }
    }
    // The refracted backdrop comes from the background copy (never the scene).
    // Beer-Lambert absorption tints the transmitted light over the fixed optical
    // thickness. This runs unconditionally, including the zero-offset and
    // premultiplied paths, so a flat/IOR<=1 absorbing cell still tints.
    float3 background = backdrop.m_reuse_background && all(offset_pixels == 0.0)
        ? backdrop.m_unshifted_background : backdropColor(backdrop.m_screen + offset_pixels, sampler);
    float3 sigma = float3(visual.m_sigma_r_per_m, visual.m_sigma_g_per_m, visual.m_sigma_b_per_m);
    if (any(sigma > 0.0)) {
        background *= exp(-sigma * visual.m_refraction_depth_m);
    }

    // Blend over the destination (particles already drawn) with the material
    // opacity, never a full replace: the refracted backdrop folds into the
    // transmitted (1-opacity) term while the destination keeps its particles.
    float4 surface = decorateSurface(float4(lerp(background, color, opacity), opacity), backdrop.m_decoration);
    if (premultiplied) surface.rgb *= surface.a;
    return surface;
}

// The one response entry every path uses. `liquid` is the explicit material
// response policy (the current liquid particle response versus the current
// categorical/solid one); it is never re-derived from a draw class.
float4 visibleSurfaceResponse(bool liquid,bool premultiplied,uint material,uint resolved,float2 fine_position,
    VisibleSurfaceSample sample,CH coefficients,VisibleSurfaceBackdrop backdrop,SamplerState.Handle sampler) {
    if (liquid) return liquidSurfaceResponse(material,fine_position,sample,backdrop,sampler,premultiplied);
    return categoricalSurfaceResponse(material,resolved,fine_position,coefficients,sample,premultiplied);
}

// The texture-coordinate context of one particle sample, split from the physical
// position the response reads. `m_unwrapped` is the particle-centred seeded
// coordinate of design section 2.6 and the gradients are its unwrapped
// derivatives, so a texture sample never needs a fake position and lighting,
// refraction, CH and terrain keep the real one.
struct VisibleUvContext {
    float2 m_unwrapped;
    float2 m_gradient_x;
    float2 m_gradient_y;
};

// The patch set of one element sample in a supplied texture context. The repeat
// path wraps the unwrapped coordinate with frac and keeps the unwrapped gradient;
// the stochastic path feeds texturePatches the unwrapped coordinate, never a prior
// frac. Every stage below shares this one construction.
TexturePatches visibleTexturePatches(GridVisual visual, float2 unwrapped) {
    TexturePatches patches;
    if ((visual.m_surface_flags & kSurfaceTextureRepeat) != 0u) {
        patches.uv0 = frac(unwrapped);
        patches.uv1 = patches.uv0;
        patches.uv2 = patches.uv0;
        patches.weights = float3(1.0, 0.0, 0.0);
    } else {
        patches = texturePatches(unwrapped);
    }

    return patches;
}

// World-space texture context of a native-cell sample: the terrain translucent
// path and any caller without a particle seed keeps its original coordinates.
VisibleUvContext worldUvContext(float2 fine_position, float2 gradient_x, float2 gradient_y, float period) {
    VisibleUvContext context;
    float2 world = fine_position * g_grid_frame.m_cell_size_m;
    context.m_unwrapped = world / period;
    context.m_gradient_x = gradient_x / period;
    context.m_gradient_y = gradient_y / period;
    return context;
}

// Particle-centred seeded context of one surface splat: the stable-identity
// offset plus the interpolated world-plane offset, and the unwrapped world
// gradients of that same offset over the texture period. The seed translates the
// patch and leaves every derivative untouched.
VisibleUvContext particleUvContext(uint seed, float2 local_metres, float2 local_gradient_x,
    float2 local_gradient_y, float period) {
    VisibleUvContext context;
    context.m_unwrapped = particleUvUnwrapped(seed, local_metres, period);
    context.m_gradient_x = local_gradient_x / period;
    context.m_gradient_y = local_gradient_y / period;
    return context;
}

// The categorical/solid sampling stage in a supplied texture context: the
// element's own tinted albedo, decoded normal and canonical-temperature thermal.
// The context carries the particle-attached seeded frame for a splat and the
// world-space frame for terrain, so both use the same texture helpers.
VisibleSurfaceSample categoricalSurfaceSampleAt(uint material, uint resolved, VisibleUvContext context,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches = visibleTexturePatches(visual, context.m_unwrapped);

    // Visible base color is sampled before metallic reduction selects kD.
    VisibleSurfaceSample sample;
    sample.m_base = visibleBaseColor(
        visual, patches, repeat_texture, context.m_gradient_x, context.m_gradient_y, textures, sampler);
    sample.m_normal = sampleVisibleNormal(
        visual, patches, repeat_texture, context.m_gradient_x, context.m_gradient_y, textures, sampler);
    sample.m_tint_alpha = tint.a;
    sample.m_thermal = categoricalCellOccupied(resolved, visual)
        ? thermalRGB(unpackCellTemperature(resolved)) : float3(0.0);
    sample.m_alpha = 1.0;
    sample.m_alpha_reconstructed = false;
    return sample;
}

// The element's own albedo (variance-normalized) and sampled alpha in a supplied
// texture context, WITHOUT a tint: the direct gas path multiplies its palette tint
// itself, keeping the existing byte tint rather than the terrain sRGB decode.
float4 visibleElementAlbedo(GridVisual visual, VisibleUvContext context,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches = visibleTexturePatches(visual, context.m_unwrapped);
    float4 albedo = sampleVisibleAlbedo(
        visual, patches, repeat_texture, context.m_gradient_x, context.m_gradient_y, textures, sampler);
    return albedoVarianceNormalized(visual, albedo, patches.weights, repeat_texture);
}

// World-space context of a native-cell sample: the terrain paths keep their
// original coordinates.
VisibleSurfaceSample categoricalSurfaceSample(uint material, uint resolved, float2 fine_position,
    float2 gradient_x, float2 gradient_y,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    return categoricalSurfaceSampleAt(
        material, resolved,
        worldUvContext(fine_position, gradient_x, gradient_y, visual.m_texture_period_m), textures, sampler);
}

float4 categoricalMaterialSurface(uint material, uint resolved, float2 fine_position,
    float2 gradient_x, float2 gradient_y, CH coefficients,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler,
    bool premultiplied) {
    VisibleSurfaceSample sample = categoricalSurfaceSample(
        material, resolved, fine_position, gradient_x, gradient_y, textures, sampler);
    VisibleSurfaceBackdrop backdrop = {float2(0.0), float2(0.0), false, float3(0.0)};
    return visibleSurfaceResponse(
        false, premultiplied, material, resolved, fine_position, sample, coefficients, backdrop, sampler);
}

// Optical opacity and geometric coverage are separate. Native zero-offset RGB
// stays premultiplied so actual destination samples supply transmission.
// Callers blend coverage once.
//
// The liquid sampling stage: the element's own albedo (with the filtered-mean
// variance normalization), normal and authoring alpha in the supplied texture
// context. The repeat path wraps the unwrapped coordinate with frac and keeps
// the unwrapped gradients; the stochastic path feeds texturePatches the
// unwrapped coordinate, never a prior frac. All albedo and normal patches share
// these gradients and translations.
VisibleSurfaceSample liquidSurfaceSampleAt(uint material, VisibleUvContext context,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches = visibleTexturePatches(visual, context.m_unwrapped);

    float2 uv_gradient_x = context.m_gradient_x;
    float2 uv_gradient_y = context.m_gradient_y;
    float4 a0 = nonuniform(textures[visual.m_texture_index]).SampleGrad(sampler, patches.uv0, uv_gradient_x, uv_gradient_y);
    float4 a1 = float4(0.0), a2 = float4(0.0);
    if (!repeat_texture) {
        a1 = nonuniform(textures[visual.m_texture_index]).SampleGrad(sampler, patches.uv1, uv_gradient_x, uv_gradient_y);
        a2 = nonuniform(textures[visual.m_texture_index]).SampleGrad(sampler, patches.uv2, uv_gradient_x, uv_gradient_y);
    }
    float3 weights = patches.weights;
    float4 albedo = a0 * weights.x + a1 * weights.y + a2 * weights.z;
    float3 mean = float3(visual.m_filtered_mean_r, visual.m_filtered_mean_g, visual.m_filtered_mean_b);
    // Heitz/Neyret-style variance normalization in linear RGB, using the admitted
    // filtered mean. This is an approximation, not Gaussian histogram transport.
    // Bound reflectance overshoot; alpha retains ordinary barycentric blending.
    if (!repeat_texture) {
        albedo.rgb = saturate(mean + (albedo.rgb - mean) * rsqrt(dot(weights, weights)));
    }

    float3 normal = float3(0.0, 0.0, 1.0);
    if (visual.m_normal_texture_index != 0xffffffffu) {
        float3 n0 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
            sampler, patches.uv0, uv_gradient_x, uv_gradient_y).xyz);
        float3 n1 = float3(0.0), n2 = float3(0.0);
        if (!repeat_texture) {
            n1 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
                sampler, patches.uv1, uv_gradient_x, uv_gradient_y).xyz);
            n2 = decodePatchNormal(nonuniform(textures[visual.m_normal_texture_index]).SampleGrad(
                sampler, patches.uv2, uv_gradient_x, uv_gradient_y).xyz);
        }
        float3 decoded = n0 * weights.x + n1 * weights.y + n2 * weights.z;
        decoded.xy *= visual.m_normal_scale;
        if (all(isfinite(decoded)) && decoded.z > 0.0 && dot(decoded, decoded) > 1.0e-8) {
            normal = normalize(decoded);
        }
    }

    VisibleSurfaceSample sample;
    sample.m_base = float4(tint.rgb * albedo.rgb, albedo.a);
    sample.m_normal = normal;
    sample.m_thermal = float3(0.0);
    sample.m_tint_alpha = tint.a;
    sample.m_alpha = 1.0;
    sample.m_alpha_reconstructed = false;
    return sample;
}

VisibleSurfaceSample liquidSurfaceSample(uint material, float2 fine_position, float2 gradient_x,
    float2 gradient_y, StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures,
    SamplerState.Handle sampler) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    return liquidSurfaceSampleAt(
        material, worldUvContext(fine_position, gradient_x, gradient_y, visual.m_texture_period_m), textures,
        sampler);
}

float4 materialSurface(uint material, float2 fine_position, float2 gradient_x,
    float2 gradient_y, float2 screen, float2 decoration, bool reuse_background, float3 unshifted_background,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler, float2 receiver_position, bool moving_surface=false, bool premultiplied=false) {
    VisibleSurfaceSample sample = liquidSurfaceSample(
        material, fine_position, gradient_x, gradient_y, textures, sampler);
    VisibleSurfaceBackdrop backdrop = {screen, decoration, reuse_background, unshifted_background};
    // The liquid response reads no indirect light; the shared entry still takes
    // the coefficient set so both policies have one call shape.
    CH no_coefficients={};
    return visibleSurfaceResponse(
        true, premultiplied, material, 0u, fine_position, sample, no_coefficients, backdrop, sampler);
}

float4 shadeGridReactive(
    GridVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler, bool point_sample, out float reactive_coverage) {
    reactive_coverage = 0;
    float2 world_pixel_x = ddx(input.fine_position);
    float2 world_pixel_y = ddy(input.fine_position);
    // Beyond a world-sized footprint all admitted textures already select
    // their coarsest LOD. Bound gradients before period division to keep the
    // smallest supported positive period finite under extreme minification.
    float2 gradient_x = clamp(world_pixel_x * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    float2 gradient_y = clamp(world_pixel_y * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    uint diagnostic_mode = g_grid_frame.m_surface_mode >> 2u;
    if (diagnostic_mode != 0u) reactive_coverage = 1;

    // ------------------------------------------------------------------
    // categorical resolved-cell path
    // ------------------------------------------------------------------
    // Each raster sample reconstructs its own continuous world-plane position
    // and selects exactly one native packed cell. This runs before the retired
    // contour/material selection so MSAA4 does not pay for work it discards.
    if (g_grid_frame.m_has_resolved_cells != 0u && diagnostic_mode == 0u) {
        uint resolved_index = resolvedCellIndex(input.fine_position);
        uint resolved = resolved_index == 0xffffffffu ? 0u : g_resolved_cells[resolved_index];
        uint resolved_material = unpackCellMaterial(resolved);
        if (resolved_material == 0u) discard;

        // Base retirement (plan §3): a native cell the committed projection
        // assigned to a particle presents only that particle union, never the
        // categorical cell fill at the winner's material. Ownership is the
        // committed winner grid of the same cell, never the resolved material,
        // so terrain-owned cells (including terrain sharing a dug particle's
        // material id) and empty cells are unchanged. This covers both the
        // opaque and the optical terrain pass below, which share this branch.
        // The bit is clear when no committed ownership grid is bound (a fixture
        // publication with no particle attempt), which keeps that surface.
        if ((g_grid_frame.m_has_resolved_cells & kGridFrameHasParticleOwnership) != 0u &&
            particleOwnsCell(resolved_index, g_best_row)) {
            discard;
        }

        GridVisual active = g_terrain_visuals[min(resolved_material, 65535u)];
        if (active.m_material_id != resolved_material) discard;
        const bool active_optical = (active.m_surface_flags & kSurfaceOpticalBlend) != 0u;
        if ((g_grid_pass.pass == 1u && active_optical) || (g_grid_pass.pass == 2u && !active_optical)) {
            discard;
        }

        if (active_optical) reactive_coverage = 1;
        return categoricalMaterialSurface(
            resolved_material, resolved, input.fine_position, gradient_x, gradient_y,
            sampleFineCH(input.fine_position), g_textures, g_sampler, point_sample);
    }

    // ------------------------------------------------------------------
    // retained cold-start, test-fixture and authoring-preview path
    // ------------------------------------------------------------------
    // All rendered 1024 production worlds publish resolved cells and take the
    // categorical path above. This contour path remains for explicit
    // test-fixture grids with no lighting publication, the first-publication
    // window before the lighting owner has bound cells, and a publication
    // failure that keeps the last valid categorical field unavailable. The same
    // diagnostic branch serves world-generation authoring previews
    // (material/biome/temperature/initial matter). It is not a flat/ambient
    // substitute for a supported rendered world. Task 19 owns the final
    // acceptance decision on whether this fallback can be removed; retained
    // lighting inspection no longer uses it.
    float2 contour_position = input.fine_position;
    if ((g_grid_frame.m_surface_mode & 3u) == 3u) {
        contour_position += boundaryDisplacement(input.fine_position, world_pixel_x, world_pixel_y, g_sampler);
    }
    // Evaluate derivatives before coverage branches/discards so neighboring
    // helper invocations still provide a defined edge width.
    float2 pixel_x = ddx(contour_position);
    float2 pixel_y = ddy(contour_position);
    float width = clamp(length(fwidth(contour_position)), 1.0e-6, 1.0);
    uint material = input.material;
    float coverage = 1.0;
    float internal_outline = 0.0;
    int2 sample = int2(floor(input.fine_position));
    TerrainStencil stencil = {};
    if (diagnostic_mode == 2u) {
        uint index = diagnosticIndex(sample);
        if (index == 0u) { discard; }
        return diagnosticTint(g_diagnostic_colors[index].m_color_rgba, 1.0);
    }
    if ((input.flags & 1u) != 0u) {
        if ((g_grid_frame.m_surface_mode & 1u) != 0u) {
            stencil = terrainStencil(contour_position);
            material = contourMaterial(contour_position, pixel_x, pixel_y, width, stencil, coverage, sample,
                internal_outline, point_sample);
        } else {
            // Rasterized quad edges may land exactly at 128 after interpolation;
            // clamp them into the final cell rather than reading the next chunk.
            uint2 cell = uint2(clamp(input.local_cell, float2(0.0), float2(127.999)));
            uint index = cell.y * 128u + cell.x;
            uint packed = g_cell_materials[input.cell_word_base + index / 2u];
            material = ((index & 1u) != 0u) ? (packed >> 16u) : (packed & 0xffffu);
            if (material == 0xffffu || material == 0u) {
                discard;
            }
        }
    }

    // The pilot's centre mask uses this actual displaced material selector,
    // before texturing/lighting. It does not infer occupancy from scene depth.

    float4 matter = float4(0.0);
    if (diagnostic_mode == 4u) {
        uint index = diagnosticIndex(int2(floor(input.fine_position)));
        if (index != 0u) {
            DiagnosticColor entry = g_diagnostic_colors[index];
            float fill = saturate(0.5 + (entry.m_fill_height_fine - input.fine_position.y) / width);
            if (fill > 0.0 && coverage < 1.0) {
                matter = diagnosticTint(entry.m_color_rgba, fill * (1.0 - coverage));
            }
        }
    }
    if (diagnostic_mode == 1u || diagnostic_mode == 3u) {
        if (coverage == 0.0 || material == 0u || material >= 0xffffu) { discard; }
        uint index = diagnostic_mode == 1u ? material : diagnosticIndex(sample);
        float4 diagnostic=diagnosticTint(g_diagnostic_colors[index].m_color_rgba,coverage);
        if (point_sample) diagnostic.rgb*=diagnostic.a;
        return diagnostic;
    }

    // Lit empty samples cannot consume either decoration component: the exterior
    // shadow is suppressed below, and no terrain surface receives an outline.
    bool consumes_decoration = coverage > 0.0 && material != 0u && material < 0xffffu;
    float2 decoration = consumes_decoration && (input.flags & 1u) != 0u && (g_grid_frame.m_surface_mode & 1u) != 0u ?
        contourDecoration(contour_position, pixel_x, pixel_y, material, coverage < 1.0, stencil) : float2(0.0);
    decoration.x = max(decoration.x, internal_outline);

    // Preserve contributor provenance before static decoration shares this accumulator.
    bool reactive_matter = matter.a > 0.0;

    // Shadow lives entirely on the non-solid side. Its coverage complements the
    // terrain AA and composites over matter before the final background blend.
    float shadow = 0.45 * decoration.y;
    matter = compositeMatter(float4(float3(0.015), shadow * (1.0 - coverage)),
        float4(matter.rgb, matter.a * (1.0 - shadow)));
    if (coverage == 0.0 || material == 0u || material >= 0xffffu) {
        if (matter.a > 0.0) {
            if (reactive_matter) reactive_coverage = 1;
            if (point_sample) matter.rgb*=matter.a;
            return matter;
        }
        discard;
    }
    if ((g_grid_frame.m_surface_mode & 2u) == 0u) {
        float4 flat=compositeMatter(float4(decorateTerrain(gridTint(material),decoration),coverage),matter);
        if (point_sample) flat.rgb*=flat.a;
        return flat;
    }
    if (material == 0u || material >= 0xffffu) {
        discard;
    }
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    if (visual.m_material_id != material) {
        discard;
    }
    // Terrain pass selection: the opaque pass skips optical-blend (translucent)
    // cells and the translucent pass skips opaque cells. Both draw the same
    // chunk quad; the pipeline depth states separate their ordering.
    const bool optical = (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u;
    if ((g_grid_pass.pass == 1u && optical) || (g_grid_pass.pass == 2u && !optical)) {
        discard;
    }
    // Classify the ACTUAL selected contour before optical opacity/emission.
    if ((visual.m_surface_flags & kSurfaceOpticalBlend) != 0u || coverage != 1.0 || reactive_matter)
        reactive_coverage = 1;

    float4 surface=materialSurface(material,input.fine_position,gradient_x,gradient_y,
        input.position.xy,decoration,false,float3(0),g_textures,g_sampler,contour_position,false,point_sample);
    if (point_sample) {
        float alpha=surface.a*coverage;
        return float4(surface.rgb*coverage+matter.rgb*matter.a*(1-alpha),alpha+matter.a*(1-alpha));
    }
    return compositeMatter(float4(surface.rgb, surface.a * coverage), matter);
}

float4 shadeGrid(GridVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler, bool point_sample) {
    float unused_reactive;
    return shadeGridReactive(input,g_textures,g_sampler,point_sample,unused_reactive);
}

struct GridReactiveOutput {
    float4 color : SV_Target0;
    float reactive : SV_Target1;
};

[shader("fragment")]
float4 fragmentGridMain(GridVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    return shadeGrid(input, g_textures, g_sampler, false);
}

// This separate entry is selected by owned spatial policy, never sample count
// alone (legacy temporal also renders 4x). SampleId forces per-sample invocation;
// explicit interpolation evaluates the actual plane, including perspective.
[shader("fragment")]
float4 fragmentGridSpatial(GridVertexOutput input, uint sample_index : SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 fine = EvaluateAttributeAtSample(input.fine_position, sample_index);
    float2 local = EvaluateAttributeAtSample(input.local_cell, sample_index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.local_cell = local;
    input.position.xy = floor(input.position.xy) + positions[sample_index];
    return shadeGrid(input, g_textures, g_sampler, true);
}

[shader("fragment")]
GridReactiveOutput fragmentGridReactive(GridVertexOutput input, uint sample_index : SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) {
    float2 fine = EvaluateAttributeAtSample(input.fine_position, sample_index);
    float2 local = EvaluateAttributeAtSample(input.local_cell, sample_index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.local_cell = local;
    input.position.xy = floor(input.position.xy) + positions[sample_index];
    GridReactiveOutput output;
    output.color=shadeGridReactive(input,g_textures,g_sampler,true,output.reactive);
    return output;
}

// ------------------------------------------------------------------
// stored per-particle orientation
// ------------------------------------------------------------------
// Task 11 increment 2 moved the whole cue solve out of the draw. The per-cell
// winner-motion field and the committed per-cell Q16 fill field used to be walked
// once per vertex to build an independent flow direction and surface tangent;
// that is now one target per accepted publication, computed in the prepare rank
// invocation and stored as the per-particle history. The vertex stage below
// decodes that record instead: the packed world-plane axis plus the bias and
// stretch amplitudes. No fill or winner read remains here, and a stored axis is
// never recomputed from the fields this draw once sampled.

// Unit drawing direction of the stored world-plane axis, projected into the
// quad's (screen-axis) frame, or (0,0) when that projection is degenerate (the
// surface is edge-on, so it has no in-plane axis to align to). Both cue
// strengths fade with the projected length over the same 1e-5..1e-4 band and fall
// back to the round response inside it.
float2 particleOrientation(float3 right,float3 up,uint instance,
    out float bias_fraction,out float stretch_fraction) {
    bias_fraction=0.0;
    stretch_fraction=0.0;
    OrientationRow row=g_particle_orientation[instance];
    uint angle=uint(row.angle_bias_stretch&0xffffu);
    uint bias=uint((row.angle_bias_stretch>>16u)&0xffu);
    uint stretch=uint((row.angle_bias_stretch>>24u)&0xffu);
    if (bias==0u && stretch==0u) return float2(0.0,0.0);

    // Decode the canonical unoriented angle: one trig pair per drawn vertex. The
    // decoded pair is for drawing only and is never stored back.
    float radians=float(angle)*kParticleOrientationPi/float(kParticleOrientationAngleCount);
    float2 world_axis=float2(cos(radians),sin(radians));
    float2 screen_axis=float2(dot(float3(world_axis,0.0),right),dot(float3(world_axis,0.0),up));
    float projected=length(screen_axis);
    float fade=smoothstep(1.0e-5,1.0e-4,projected);
    if (!(projected>1.0e-5)) return float2(0.0,0.0);

    bias_fraction=float(bias)/255.0*fade;
    stretch_fraction=float(stretch)/255.0*fade;
    return screen_axis/projected;
}

// One decoded conic record. Two aligned uint loads cover the six logical bytes:
// even row 2j starts at word 3j (a = low, b = high) and spans word 3j+1 for c;
// odd row 2j+1 starts at word 3j+1 (a = high) and spans word 3j+2 for b and c.
// The two loads always lie inside the 12*ceil(capacity/2)-byte allocation.
ParticleConicMetric particleConic(uint row) {
    uint base=3u*(row>>1u)+(row&1u);
    uint2 words=uint2(g_particle_conic[base],g_particle_conic[base+1u]);
    if ((row&1u)==0u) return particleConicMetric(words.x&0xffffu,words.x>>16u,words.y&0xffffu);
    return particleConicMetric(words.x>>16u,words.y&0xffffu,words.y>>16u);
}

struct ParticleVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 uv:TEXCOORD1;
    nointerpolation uint material:TEXCOORD2;
    // Stable-identity material-UV seed from the row and the particle-centred world
    // offset of this vertex. They carry the seeded sampling context of the
    // particle's own texture patch; the frame is translated with the particle,
    // not rotated or stretched by the conic, so neither follows the draw class.
    // A sample-rate entry must evaluate local_m at the sample exactly as it does
    // fine_position and uv, never reuse the pixel-centre value.
    nointerpolation uint uv_seed:TEXCOORD9;
    float2 local_m:TEXCOORD10;
    // Quad corner in units of the row's physical radius r: 64/62 for the gas
    // mask, and the shared support 2.0 for both restored classes (the
    // fragment's phase SDF and falloff produce the actual silhouette). A row
    // with published motion carries the flow cue's k here, so the quad encloses
    // the stretched shape.
    nointerpolation float shape_scale:TEXCOORD3;
    // Canonical packCell word rebuilt from the row: material | temperature << 16.
    nointerpolation uint canonical:TEXCOORD4;
    // Shared stored orientation axis in the quad's (screen-axis) frame and the
    // stretch amplitude's elongation k >= 1. Both are uniform per instance; k == 1
    // is the un-stretched shape and the fragment evaluates today's local
    // coordinates unchanged.
    nointerpolation float2 flow:TEXCOORD5;
    nointerpolation float flow_stretch:TEXCOORD6;
    // The same shared axis carried to the fragment's tangent contour, with the
    // stored bias strength (B/255, faded toward the round contour when the
    // projected axis is degenerate). Both are uniform per instance.
    nointerpolation float2 surface_tangent:TEXCOORD7;
    nointerpolation float surface_strength:TEXCOORD8;
    // Density-lane conic metric, uniform per instance: xyz is the corrected
    // record scaled by 1/R^2 (so the fragment multiplies the world-plane offset
    // directly) and w is sqrt(det) of the unscaled record, the kernel's volume
    // normalisation. Only the splat entry reads it; the direct entries leave the
    // neutral identity.
    nointerpolation float4 conic:TEXCOORD11;
};

// Shared particle quad expansion. `surface` selects the surface reconstruction
// entry: every non-gas row whose own palette phase is in the admitted phase mask
// is accepted, one density pixel of admission padding is added to the quad, and
// the class-based depth description is not consulted. The admitted phase mask
// and padding are explicit arguments so each program declares only the constant
// buffers its own entry reads. The direct entries pass false and keep their
// byte-identical geometry, class rejection and admitted-phase exclusion: a
// routed row is never drawn both directly and reconstructed.
//
// `minimum_extent` is a world-space floor on the quad's own half extent. The
// presence marker passes its kParticleMarkerQuadRadius bounds here, because at far
// zoom the reconstruction support is narrower than the pixel the marker must still
// cover; every other entry passes zero and keeps its exact previous geometry.
ParticleVertexOutput particleVertexCommon(uint vertex,uint instance,bool surface,uint phase_mask,float pad,
    float minimum_extent) {
    static const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),
        float2(-1,-1),float2(1,1),float2(-1,1)};
    ParticleGpu particle=g_particles[instance];
    ParticleVertexOutput output=(ParticleVertexOutput)0;
    // The row does not keep the canonical packCell word: material (12 bits),
    // class flags (4 bits) and the binary16 temperature share one word, so
    // shading reconstructs canonical = material | (temperature << 16). Alpha is
    // not stored either; the material's own alpha supplies opacity.
    uint material=particle.m_material_flags_temperature&kParticleMaterialMask;
    uint class_bits=(particle.m_material_flags_temperature>>12u)&0xfu;
    bool is_gas=(class_bits&kParticleClassGas)!=0u;
    bool is_optical=(class_bits&kParticleClassOptical)!=0u;
    uint draw_class=g_grid_pass.particle_class;

    // One branch, no new buffer and no second compaction: a draw rejects the
    // classes it does not own before rasterisation. The gas draw therefore
    // rejects every moving row explicitly, which is what the retired fragment
    // discard used to do. The admitted phases are rejected by the direct classes
    // (held frames included) so nothing is drawn twice: admission is the row's
    // own palette phase against the phase mask, so every admitted liquid
    // material routes together and an unauthored/unlisted material stays direct.
    // A gas row keeps the gas path, which is a different representation.
    uint phase=particleMaterialPhase(material);
    bool routed_elsewhere=(phase&phase_mask)!=0u&&!is_gas;
    bool owned=surface ? routed_elsewhere
        : (draw_class==kParticleDrawGas ? is_gas
            : draw_class==kParticleDrawOpaque ? (!is_gas&&!is_optical)
            : (draw_class==kParticleDrawOptical&&!is_gas&&is_optical))&&!routed_elsewhere;
    if (!owned) {
        output.position=float4(2.0,2.0,2.0,1.0);
        return output;
    }

    // Billboard basis: the world directions of the screen axes. The stored
    // world-plane axis is projected into this basis below.
    float3 right=normalize(mul(float4(1,0,0,0),g_grid_frame.m_inverse_view_projection).xyz);
    float3 up=normalize(mul(float4(0,1,0,0),g_grid_frame.m_inverse_view_projection).xyz);

    // Task 11 increment 2: both cues are decoded from the stored per-particle
    // history instead of being recomputed from the committed fill and winner
    // grids. One shared world-plane axis carries flow and tangent; the stored
    // amplitudes stay independent (b = 0.3999 B along the surface, stretch =
    // 1 + 0.5 S along the flow). A repeated or held publication rewrites nothing,
    // so a paused frame decodes the same bytes. There is no dt.
    //
    // The density lane replaces that projected silhouette decode with the
    // per-row conic record: its shape is a camera-independent world-plane
    // metric, so a camera change rescales the raster footprint without
    // re-deriving the shape and without a second source of truth for the cue.
    //
    // Gas keeps its mask texture and its exact previous geometry: it decodes no
    // attribute and stays round.
    float2 axis=float2(1.0,0.0);
    float stretch=1.0;
    float surface_strength=0.0;
    if (!surface&&draw_class!=kParticleDrawGas) {
        float bias_fraction=0.0;
        float stretch_fraction=0.0;
        const float2 stored_axis=particleOrientation(right,up,instance,bias_fraction,stretch_fraction);
        if (dot(stored_axis,stored_axis)>0.5) axis=stored_axis;
        stretch=1.0+(kParticleStretchCap-1.0)*stretch_fraction;
        surface_strength=bias_fraction;
    }

    // The surface reconstruction's own metric: the same record the draw binds,
    // decoded once per instance into the corrected coefficients scaled by
    // 1/R_{surface}^2 and the kernel normalisation sqrt(det). `conic_support` is
    // its major semiaxis in units of R_{surface} (<= 1.5 by the decoded
    // eigenvalue floor) and therefore sizes the quad exactly like the silhouette
    // stretch does for the direct classes.
    float4 conic=float4(1.0,0.0,1.0,1.0);
    float conic_support=1.0;
    if (surface) {
        ParticleConicMetric metric=particleConic(instance);
        float support_radius=particle.m_radius_m*kParticleSurfaceRadiusScale;
        float inverse_area=1.0/(support_radius*support_radius);
        conic=float4(metric.m_a*inverse_area,metric.m_b*inverse_area,metric.m_c*inverse_area,
            metric.m_sqrt_determinant);
        conic_support=metric.m_support;
    }

    // The flow metric and the tangent contour now share that one axis; the
    // fragment scales the contour bias by the stored tangent strength.
    float2 flow=axis;
    float2 surface_tangent=axis;

    // Gas keeps the shared mask texture and its two-texel transparent border; the
    // direct solid keeps its rounded square inscribed in R = 2.0 r. Those are the
    // direct silhouettes until the solid migration, so their quads keep the
    // direct support scale; the surface reconstruction's quad follows its own
    // smaller shared conic radius instead, multiplied by the decoded major
    // semiaxis so a stretched row still fits.
    //
    // The direct quad is the un-stretched support scaled by the same k, so it
    // encloses the stretched shape: that shape's support in any direction is at
    // most k R = 3.000 r, inside the 3.0625 r cull margin, and the fragment
    // applies the same bound to the minor axis, which stays at R. With stretch ==
    // 1 every factor here is exactly the previous expression. The surface entry
    // adds one density pixel of admission padding so a pixel whose footprint
    // intersects the support is tested even when its centre lies outside; the
    // kernel weight itself still vanishes at rho >= 1. Its quad follows the
    // decoded conic's own major semiaxis, which stays correct when the projected
    // silhouette decode is degenerate: the conic metric is world XY and needs no
    // projection.
    float extent=surface
        ? particle.m_radius_m*(kParticleSurfaceRadiusScale*conic_support)+pad
        : (draw_class==kParticleDrawGas ? particle.m_radius_m*(64.0/62.0)
            : particle.m_radius_m*(kParticleSupportScale*stretch));
    extent=max(extent,minimum_extent);
    float3 world=float3(particle.m_position,0)+(right*corners[vertex].x+up*corners[vertex].y)*extent;
    output.position=mul(float4(world,1),g_grid_frame.m_view_projection);
    output.fine_position=world.xy/g_grid_frame.m_cell_size_m;
    output.uv=corners[vertex]*0.5+0.5;
    output.material=material;
    output.uv_seed=particle.m_uv_seed;
    output.local_m=world.xy-particle.m_position;
    output.shape_scale=extent/particle.m_radius_m;
    output.canonical=material|(particle.m_material_flags_temperature&0xffff0000u);
    output.flow=flow;
    output.flow_stretch=stretch;
    output.surface_tangent=surface_tangent;
    output.surface_strength=surface_strength;
    output.conic=conic;
    return output;
}

StructuredBuffer<uint> g_particle_ids;

// The three direct class draws (standard, reactive and spatial) all read the
// clip position, the native-cell offset, the material word, the seeded texture
// context and the quad-local offset, and nothing else. Their shared vertex entry
// therefore declares only those: every output of the stage interface is consumed
// by the fragment, so no write is discarded and the unwritten fields cost no
// interpolation. The quad itself is still the one shared expansion.
struct ParticleDirectVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 uv:TEXCOORD1;
    nointerpolation uint material:TEXCOORD2;
    nointerpolation uint uv_seed:TEXCOORD3;
    float2 local_m:TEXCOORD4;
};

ParticleDirectVertexOutput particleVertex(uint vertex,uint instance) {
    ParticleVertexOutput expanded=particleVertexCommon(vertex,instance,false,g_grid_pass.surface_phase_mask,0.0,0.0);
    ParticleDirectVertexOutput output;
    output.position=expanded.position;
    output.fine_position=expanded.fine_position;
    output.uv=expanded.uv;
    output.material=expanded.material;
    output.uv_seed=expanded.uv_seed;
    output.local_m=expanded.local_m;
    return output;
}

[shader("vertex")]
ParticleDirectVertexOutput vertexParticleIndirectMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    return particleVertex(vertex,g_particle_ids[instance]);
}

// The solid and liquid fragments read the whole attribute set, so their pipeline
// needs a vertex entry that declares all of it. Sharing the reduced direct output
// above would leave those fragments reading locations the vertex never writes,
// which the Vulkan interface rule forbids outright rather than warning about.
[shader("vertex")]
ParticleVertexOutput vertexParticleIndirectFull(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    return particleVertexCommon(vertex,instance,false,g_grid_pass.surface_phase_mask,0.0,0.0);
}

// ------------------------------------------------------------------
// particle occlusion and element shading
// ------------------------------------------------------------------

struct ParticleTerrainFootprint {
    float2 contour;
    float2 pixel_x;
    float2 pixel_y;
    float width;
};

// Prepare all derivatives coherently, including the displaced contour, before
// rejecting empty pixels. The expensive terrain stencil can then be deferred.
ParticleTerrainFootprint particleTerrainFootprint(float2 fine_position, SamplerState.Handle g_sampler) {
    float2 contour = fine_position;
    float2 world_pixel_x = ddx(contour), world_pixel_y = ddy(contour);
    if ((g_grid_frame.m_surface_mode & 3u) == 3u) {
        contour += boundaryDisplacement(fine_position, world_pixel_x, world_pixel_y, g_sampler);
    }

    ParticleTerrainFootprint footprint;
    footprint.contour = contour;
    footprint.pixel_x = ddx(contour);
    footprint.pixel_y = ddy(contour);
    footprint.width = clamp(length(fwidth(contour)), 1.0e-6, 1.0);
    return footprint;
}

// The native cell at `cell` is opaque terrain: present, non-transparent, and
// not on the optical-blend (translucent) path. Returns the material id for an
// opaque cell and zero for vacuum, transparent or translucent cells.
uint nativeOpaqueMaterial(int2 cell) {
    uint material = terrainSample(cell);
    if (material == 0u || material == 0xffffu) return 0u;
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    if (visual.m_material_id != material) return 0u;
    return (visual.m_surface_flags & kSurfaceOpticalBlend) == 0u ? material : 0u;
}

// Opaque-terrain coverage for the particle/fluid hard-clip contract. Translucent
// cells contribute zero so a particle blends under water/glass rather than
// fading on an occupied (but transparent) native cell; antialiasing survives
// only on the opaque geometric boundary.
float particleOpaqueCoverage(ParticleTerrainFootprint footprint, bool point_sample=false) {
    int2 cell = int2(floor(footprint.contour));
    if ((g_grid_frame.m_surface_mode & 1u) != 0u) {
        TerrainStencil stencil = terrainStencil(footprint.contour);
        uint materials[4];
        for (uint corner = 0u; corner < 4u; ++corner) {
            materials[corner] = nativeOpaqueMaterial(stencil.base + int2(g_contour_points[corner]));
        }
        float2 point = footprint.contour - (float2(stencil.base) + 0.5);
        return point_sample ? occupiedPoint(materials, point) : occupiedCoverage(materials, point, footprint.width);
    }
    return nativeOpaqueMaterial(cell) != 0u ? 1.0 : 0.0;
}

float4 shadeParticleReactive(ParticleDirectVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample,out float geometric_support) {
    geometric_support=0;
    // Coherent derivatives first (design §4.2): the element albedo/gradients below
    // are evaluated before the mask/terrain discards that follow.
    float2 local_gradient_x=ddx(input.local_m),local_gradient_y=ddy(input.local_m);

    // D9: the material id is the shading key and is never substituted. A row
    // whose id the palette does not carry discards here instead of reading the
    // empty entry 0; the host counts rejected materials once per publication.
    GridVisual visual = g_terrain_visuals[input.material];
    if (visual.m_material_id != input.material) discard;

    ParticleTerrainFootprint footprint = particleTerrainFootprint(input.fine_position, g_sampler);
    // Actual alpha texture, with filtered mip levels. No analytic circle mask.
    float alpha=g_particle_mask.Sample(g_particle_mask_sampler,input.uv).a;
    if (alpha==0) discard;

    float coverage = particleOpaqueCoverage(footprint,point_sample);
    if (coverage == 1.0) discard;

    // Geometric support survives independently of material alpha and source.
    geometric_support=1;

    float materialAlpha=float(visual.m_color_rgba&255u)/255.0;
    // The tint is the row's own palette element, the same authored RGBA8 word the
    // packer derived its tint from: the row's fourth word now carries the UV seed,
    // so this decodes the palette bytes directly. Alpha is not part of it; the
    // palette alpha above is the separate opacity factor.
    float3 tint=particleTint(visual.m_color_rgba);
    // Gas is alpha-composited over the already shaded scene. Its color uses the
    // current decoded CH angular mean; it never relights the background and
    // adds no authored/thermal transport emission or ambient floor. Without a
    // published fine-CH field (legacy fixtures) the fallback buffer is too small
    // to index, so the contribution is explicitly zero.
    float3 lighting = float3(0.0);
    if (g_grid_frame.m_has_resolved_cells != 0u) {
        lighting = angularMean(input.fine_position);
    }

    // The element's own albedo and alpha, sampled in the particle-attached seeded
    // frame (design §2.7). Gas keeps its mask UV above and gains no normal, no
    // emission and no density field; the palette tint still multiplies it, so an
    // untextured element's neutral albedo reproduces the previous tint-only color
    // up to the seeded patch.
    float period = visual.m_texture_period_m;
    float4 element = float4(1.0);
    if (period > 0.0 && isfinite(period)) {
        element = visibleElementAlbedo(visual,
            particleUvContext(input.uv_seed,input.local_m,local_gradient_x,local_gradient_y,period),
            g_textures,g_sampler);
    }

    float opacity=alpha*materialAlpha*element.a*visual.m_opacity*(1.0-coverage);
    float3 color=tint*element.rgb*lighting;
    return float4(point_sample?color*opacity:color,opacity);
}

float4 shadeParticle(ParticleDirectVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample) {
    float unused_reactive;
    return shadeParticleReactive(input,g_textures,g_sampler,point_sample,unused_reactive);
}

[shader("fragment")]
float4 fragmentParticleMain(ParticleDirectVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler):SV_Target {
    return shadeParticle(input,g_textures,g_sampler,false);
}

[shader("fragment")]
float4 fragmentParticleSpatial(ParticleDirectVertexOutput input,uint index:SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler):SV_Target {
    // Evaluate the entry's actual varyings before mutating either member.
    float2 fine = EvaluateAttributeAtSample(input.fine_position, index);
    float2 uv = EvaluateAttributeAtSample(input.uv, index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.uv = uv;
    input.position.xy = floor(input.position.xy) + positions[index];
    return shadeParticle(input,g_textures,g_sampler,true);
}

[shader("fragment")]
GridReactiveOutput fragmentParticleReactive(ParticleDirectVertexOutput input,uint index:SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler){
    // Evaluate the entry's actual varyings before mutating either member.
    float2 fine = EvaluateAttributeAtSample(input.fine_position, index);
    float2 uv = EvaluateAttributeAtSample(input.uv, index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.uv = uv;
    input.position.xy = floor(input.position.xy) + positions[index];
    GridReactiveOutput output;
    output.color=shadeParticleReactive(input,g_textures,g_sampler,true,output.reactive);
    return output;
}

// ------------------------------------------------------------------
// per-instance solid and liquid descriptions
// ------------------------------------------------------------------
// The restored instanced classes shade from the instance's own packed material
// field. The phase flags on that same material choose the description family:
// solid -> the material's PBR surface, liquid -> its optical surface. That
// choice is independent of the draw class, which selects only depth/blend state
// and whether authored opacity participates in the edge composite: an opaque
// liquid (liquid phase without kSurfaceOpticalBlend) is drawn by the opaque
// class and still shades through the liquid description, and an optical solid
// is drawn by the optical class and still shades through the solid one.

// Signed distance to the solid rounded-square boundary, in units of the row's
// physical radius: distance to the inset rectangle (half-extent a-c) plus the
// corner arc c. It is a distance measurement, never a mask texture fetch; the
// fragment turns it into a one-pixel coverage ramp. The liquid silhouette is the
// radial falloff below, so it needs no SDF.
float particleSolidDistance(float2 local) {
    float2 offset=abs(local)-float2(kParticleSolidInnerHalfExtentScale,kParticleSolidInnerHalfExtentScale);
    return length(max(offset,float2(0.0)))+min(max(offset.x,offset.y),0.0)-kParticleSolidCornerScale;
}

// C1 radial falloff of the presentation support, shared by both phases. With
// rho = |local| / R it is 1 inside R_in = (2/3) R, falls as smoothstep(1, 2/3,
// rho) to 0 at R and is 0 beyond. t is written increasing so this is the ordinary
// smoothstep polynomial, and both joins have zero slope. Both restored classes
// composite this falloff premultiplied as their edge coverage, and a solid
// multiplies it by the analytic SDF rim below, so the silhouette is the coverage
// itself rather than a cut at alpha = 0.5; its resting half-coverage contour is
// rho = 5/6, and the optional tangent cue below moves that contour per angle
// inside [2/3, 1] without touching either join. The radial plateau (rho <= 2/3)
// is exactly 1 as a falloff; final solid coverage multiplies the rim and the
// terrain complement, so it is exactly 1 only where those leave it untouched.
float particleFalloff(float2 local) {
    float rho=length(local)/kParticleSupportScale;
    float t=saturate((1.0-rho)/(1.0-kParticleFalloffInnerFraction));
    return t*t*(3.0-2.0*t);
}

// Anisotropic metric of the optional flow cue: the sample point is compressed
// along the flow axis by 1/k, so the un-stretched disk/rounded-square distance
// and the falloff above, evaluated at the result, describe the shape stretched
// by k along that axis. The stretch is a metric change rather than a rotation of
// the shape's own frame, so k == 1 is the exact identity for every direction: a
// solid's rounded square does not spin when a world starts stepping, and no
// direction has to be chosen for zero motion. Since k >= 1 the stretched shape
// contains the un-stretched one, and the cross-flow semi-axis stays R.
float2 particleFlowLocal(float2 local,float2 flow,float stretch) {
    if (!(stretch>1.0)) return local;
    return local-(1.0-1.0/stretch)*dot(local,flow)*flow;
}

// Tangent-aligned contour of the optional Task 11 cue, in the same r units and
// the same frame as particleFalloff. Only the band between the plateau and the
// support is warped: the half-coverage contour becomes the angular edge
// k(theta) = k0*(1 + b*(cos^2(theta) - 1/2)) with theta measured from the surface
// tangent, so the premultiplied silhouette is longer along the surface and
// shorter across it.
// The warp is a Mobius map of the band coordinate with fixed endpoints and a
// 0.5 crossing at k(theta), and the same C1 smoothstep polynomial is evaluated on
// it, so both joins keep zero slope. The plateau and the support are returned
// explicitly, so the function is total on its own domain: alpha is exactly 1 for
// rho <= 2/3 (including |local| == 0) and exactly 0 at and beyond R, and no
// support or cull change can result. Both joins are closed by
// kParticleTangentJoinClosure, because |local| is rounded: a sample constructed
// exactly at the support radius can arrive a few ulps inside it, and the warp
// would then return a denormal alpha instead of 0. The contour itself is clamped
// one hair above the plateau and to the support, so the warp can never divide by
// a zero band offset (k_min > 2/3, the finite-band requirement) and no orientation
// can place the contour at or beyond R (k_max <= 1, the support requirement).
float particleTangentFalloff(float2 local,float2 tangent,float strength) {
    // Compile-time endpoint guard, so a future constant edit fails the build
    // instead of producing a NaN alpha at the normal direction. This Slang
    // version rejects the declaration form of static_assert at module scope
    // (E20001), so the assertion sits at the statement scope of its consumer; it
    // is still evaluated at compile time (a failing b = 0.40 fails shader
    // compilation). The CPU twin carries the same pair in App.Renderer.GridPass.cppm.
    // The effective bias is b = kParticleTangentContourBias * strength with
    // 0 <= strength <= 1, so proving the maximum constant bounds every bias used.
    static_assert(kParticleTangentContourScale*(1.0-0.5*kParticleTangentContourBias)>
        kParticleFalloffInnerFraction,
        "the contour's minor radius must stay above the plateau: the warp divides by the band offset");
    static_assert(kParticleTangentContourScale*(1.0+0.5*kParticleTangentContourBias)<=1.0,
        "the contour's major radius must stay inside the support radius R");

    float length_local=length(local);
    float rho=length_local/kParticleSupportScale;
    if (rho<=kParticleFalloffInnerFraction+kParticleTangentJoinClosure) return 1.0;
    if (rho>=1.0-kParticleTangentJoinClosure) return 0.0;

    float2 direction=local/length_local;
    float cosine=dot(direction,tangent);
    // b_eff = 0 gives k(theta) = k0 exactly: the resting circular contour, which
    // is the intended behaviour for a field the energy gate does not trust.
    float effective_bias=kParticleTangentContourBias*strength;
    float contour=kParticleTangentContourScale*(1.0+effective_bias*(cosine*cosine-0.5));
    contour=clamp(contour,kParticleTangentContourMinimum,1.0);

    float u=(rho-kParticleFalloffInnerFraction)/(1.0-kParticleFalloffInnerFraction);
    float u_contour=(contour-kParticleFalloffInnerFraction)/(1.0-kParticleFalloffInnerFraction);
    float warp=2.0-1.0/u_contour;
    float w=u*(1.0-warp)/(1.0-warp*u);
    float t=1.0-w;
    return t*t*(3.0-2.0*t);
}

// Pure full-cell occlusion decision. A particle is hidden by its destination
// native cell only when the cell is owned by opaque terrain: it carries a
// material, the committed projection recorded no particle winner for it, the
// resolved material matches the palette record (identity guard), and the
// material is not optical-blend. Ownership is the committed winner grid, not the
// material id, so terrain sharing a dug particle's material still occludes;
// optical terrain keeps the existing blend-under behaviour. Shared with the
// nonvisual material probe so the asserted decision is this one.
bool terrainOccludesParticle(uint resolved_material, uint terrain_material_id, bool terrain_optical,
    uint best_row) {
    return resolved_material!=0u && best_row==0xffffffffu &&
        terrain_material_id==resolved_material && !terrain_optical;
}

// ------------------------------------------------------------------
// pre-edge phase surface shared by direct and density entries
// ------------------------------------------------------------------
// The particle's own material surface before geometric edge coverage: linear HDR
// premultiplied RGB in .rgb and the authored optical alpha in .a (1 for the
// opaque class, which ignores authored opacity). This is the one pre-edge
// evaluation the direct entries and the density splat entry read. It is split at
// the attribute boundary: `*SurfaceSample` evaluates this particle's own material
// textures, while `visibleSurfaceResponse` performs the response from those
// attributes and the physical sample position. The deferred composite will call
// the same response with reconstructed attributes.
//
// A liquid shades through the optical surface (its albedo/normal, IOR, optical
// thickness and absorption sigmas, refracting the backdrop copy); a solid
// shades through the PBR surface (albedo, normal, tint, metallic, roughness,
// authored emission and the temperature classification carried in the canonical
// word). `blend_class` is the optical draw class: it premultiplies the authored
// opacity and is never re-derived from the phase.
//
// The particle's texture coordinates are the seeded, particle-attached frame of
// design §2.5–2.7, never the world UV: this is the same `VisibleUvContext` the
// surface lane accumulates, so a temporary direct solid, a direct liquid and a
// reconstructed splat of one element sample the same patch at the same point.
// The physical position (fine_position) still drives CH, the view, refraction,
// terrain and depth; only the texture coordinate is attached to the particle.
float4 shadeParticleSurfaceBeforeEdge(bool liquid,bool blend_class,uint material,uint canonical,
    float2 fine_position,float2 screen,uint seed,float2 local_m,float2 local_gradient_x,
    float2 local_gradient_y,StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler) {
    // Admitted positive finite period; a malformed record keeps a finite context
    // instead of an infinite coordinate (the palette admission already validates it).
    float period=g_terrain_visuals[min(material,65535u)].m_texture_period_m;
    if (!(period>0.0)||!isfinite(period)) period=1.0;
    VisibleUvContext context=particleUvContext(seed,local_m,local_gradient_x,local_gradient_y,period);

    if (liquid) {
        VisibleSurfaceSample sample=liquidSurfaceSampleAt(material,context,g_textures,g_sampler);
        VisibleSurfaceBackdrop backdrop={screen,float2(0.0),false,float3(0.0)};
        CH no_coefficients={};
        return visibleSurfaceResponse(
            true,blend_class,material,0u,fine_position,sample,no_coefficients,backdrop,g_sampler);
    }

    // A world with no published fine CH shades from an explicit zero field
    // instead of indexing the one-element fallback.
    CH coefficients={};
    if (g_grid_frame.m_has_resolved_cells!=0u) coefficients=sampleFineCH(fine_position);
    VisibleSurfaceSample sample=categoricalSurfaceSampleAt(material,canonical,context,g_textures,g_sampler);
    VisibleSurfaceBackdrop backdrop={screen,float2(0.0),false,float3(0.0)};
    return visibleSurfaceResponse(
        false,blend_class,material,canonical,fine_position,sample,coefficients,backdrop,g_sampler);
}

float4 shadeSolidLiquidReactive(ParticleVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample,out float geometric_support) {
    geometric_support=0;
    // The draw class is the depth/blend state only; it never selects the
    // description family.
    bool blend_class=g_grid_pass.particle_class==kParticleDrawOptical;

    // Coherent derivatives first (design §4.2): the terrain footprint and the
    // seeded texture gradients are evaluated before any per-pixel reject, so a
    // discard below cannot leave them undefined across a fragment quad.
    float2 local_gradient_x=ddx(input.local_m),local_gradient_y=ddy(input.local_m);
    ParticleTerrainFootprint footprint=particleTerrainFootprint(input.fine_position,g_sampler);

    // D9: the material is the row's own id in its canonical packCell word. An
    // absent id discards this instance and is counted on the host; it never
    // falls back to entry 0 or to the resolved destination cell's material.
    uint material=input.canonical&kParticleMaterialMask;
    GridVisual visual=g_terrain_visuals[material];
    if (visual.m_material_id!=material) discard;

    // The description family is authored on this material's own phase flags, so
    // a liquid is never shaded through the solid description and a solid never
    // through the liquid one. An unauthored phase keeps the solid description,
    // so every committed particle is drawn.
    bool authored_liquid=(visual.m_surface_flags&kSurfaceParticleLiquid)!=0u;
    bool authored_solid=(visual.m_surface_flags&kSurfaceParticleSolid)!=0u;
    bool liquid=authored_liquid&&!authored_solid;

    float2 local=(input.uv*2.0-1.0)*input.shape_scale;
    // The optional flow cue only reshapes the local coordinates; at k == 1 this
    // is the unchanged value, so a zero-motion row draws today's disk/rounded
    // square bit for bit.
    local=particleFlowLocal(local,input.flow,input.flow_stretch);
    float falloff=particleFalloff(local);
    // The optional tangent cue is an additional outer-edge term, not a
    // replacement for the flow stretch above: Task 4 reshapes the local metric
    // and this warps only the band radius, so both compose on one silhouette.
    // It is applied only inside the open band (0 < alpha < 1), which keeps the
    // radial falloff plateau, the support and a flat field byte-identical to
    // today; input.surface_strength carries the stored bias strength B/255 (the
    // same 0..1 gate the energy rule used to supply) and scales the warp toward
    // the round k0 contour.
    if (falloff>0.0 && falloff<1.0 && dot(input.surface_tangent,input.surface_tangent)>0.5)
        falloff=particleTangentFalloff(local,input.surface_tangent,input.surface_strength);
    // One coverage term now carries both restored classes: the C1 falloff, the
    // solid's analytic rounded-square rim, and partial terrain coverage. Both
    // classes composite it premultiplied; they differ only in depth ownership
    // (the opaque class writes depth) and in whether authored opacity
    // participates (the optical class) or not (the opaque class). The old hard
    // cuts at alpha = 0.5 and at SDF <= 0 are gone: a solid's flats and corners
    // both fade over about one pixel through the same pixel-footprint ramp. This
    // is also the qualification on the "plateau is opaque" claim: final coverage
    // is exactly 1 only where the rim and the terrain complement leave the radial
    // plateau untouched (fine zoom, no terrain under the sample).
    float shape_distance=particleSolidDistance(local);
    // Pixel footprint in the same local (r) units: the mean of the two raster
    // axis lengths. It is derived from the derivatives themselves, never a
    // hardcoded pixel size, so the rim stays one pixel wide at any zoom.
    float rim_width=0.5*(length(ddx(local))+length(ddy(local)));
    // A liquid is the falloff disk alone. A solid's rim is the SDF coverage ramp:
    // 1 in the interior, 0.5 on the boundary, 0 half a footprint outside it.
    float shape_coverage=liquid ? 1.0 : saturate(0.5-shape_distance/max(rim_width,1.0e-6));
    float terrain_coverage=particleOpaqueCoverage(footprint,point_sample);
    float edge_coverage=falloff*shape_coverage*(1.0-terrain_coverage);
    if (edge_coverage<=kParticleCoverageFloor) discard;

    // Full-cell terrain occlusion: an opaque terrain-owned destination cell
    // hides the particle. The per-pixel hard clip below refines contour edges;
    // this is the whole-cell guarantee for quads whose sample lands on the cell
    // interior. The ownership bit authorizes the committed winner-grid read, so
    // a fallback/stale attempt never reads out of range. Particle-owned (dug)
    // cells and empty cells draw, and optical terrain keeps blending under.
    if ((g_grid_frame.m_has_resolved_cells&kGridFrameHasParticleOwnership)!=0u) {
        uint occluder_cell=resolvedCellIndex(input.fine_position);
        if (occluder_cell!=0xffffffffu) {
            uint resolved=g_resolved_cells[occluder_cell];
            uint occluder_material=unpackCellMaterial(resolved);
            if (occluder_material!=0u) {
                GridVisual occluder=g_terrain_visuals[min(occluder_material,65535u)];
                if (terrainOccludesParticle(occluder_material,occluder.m_material_id,
                    (occluder.m_surface_flags&kSurfaceOpticalBlend)!=0u,g_best_row[occluder_cell])) {
                    discard;
                }
            }
        }
    }

    // The per-pixel terrain hard clip is kept as the second line of defence for
    // pixels where draw order alone is not enough. It is redundant with the
    // coverage floor above (full terrain coverage drives edge coverage to zero)
    // and stays as the explicit whole-cell guarantee.
    if (terrain_coverage==1.0) discard;

    geometric_support=1;

    // The phase surface before edge coverage, shared with the density entry. It
    // is still the material's own description (the liquid optical surface or the
    // solid PBR surface), so lighting, texture, normal, temperature/emissive and
    // backdrop evaluation are unchanged. The optical class premultiplies its
    // authored opacity by the edge coverage; the opaque class ignores authored
    // opacity and premultiplies only the coverage, so its interior is fully
    // opaque where the rim and terrain complement leave coverage at 1.
    float4 surface=shadeParticleSurfaceBeforeEdge(liquid,blend_class,material,input.canonical,
        input.fine_position,input.position.xy,input.uv_seed,input.local_m,local_gradient_x,local_gradient_y,
        g_textures,g_sampler);
    if (blend_class) return float4(surface.rgb*edge_coverage,surface.a*edge_coverage);
    return float4(surface.rgb*edge_coverage,edge_coverage);
}

[shader("fragment")]
float4 fragmentSolidLiquidMain(ParticleVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler):SV_Target {
    float unused_reactive;
    return shadeSolidLiquidReactive(input,g_textures,g_sampler,false,unused_reactive);
}

[shader("fragment")]
float4 fragmentSolidLiquidSpatial(ParticleVertexOutput input,uint index:SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler):SV_Target {
    // Evaluate the entry's actual varyings before mutating either member.
    float2 fine = EvaluateAttributeAtSample(input.fine_position, index);
    float2 uv = EvaluateAttributeAtSample(input.uv, index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.uv = uv;
    input.position.xy = floor(input.position.xy) + positions[index];
    float unused_reactive;
    return shadeSolidLiquidReactive(input,g_textures,g_sampler,true,unused_reactive);
}

[shader("fragment")]
GridReactiveOutput fragmentSolidLiquidReactive(ParticleVertexOutput input,uint index:SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) {
    // Evaluate the entry's actual varyings before mutating either member.
    float2 fine = EvaluateAttributeAtSample(input.fine_position, index);
    float2 uv = EvaluateAttributeAtSample(input.uv, index);
    static const float2 positions[4] = {
        float2(0.375, 0.125), float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};

    input.fine_position = fine;
    input.uv = uv;
    input.position.xy = floor(input.position.xy) + positions[index];
    GridReactiveOutput output;
    output.color=shadeSolidLiquidReactive(input,g_textures,g_sampler,true,output.reactive);
    return output;
}

// ------------------------------------------------------------------
// screen-space particle surface: marker, label selection, attributes, composite
// ------------------------------------------------------------------
// One reconstruction method for the admitted phases (rev. 4 section 9, rev. 5
// radius, rev. 7 world-space smoothing, rev. 8 presence marker). Per rendered
// frame and phase bundle:
//
//   1. marker            M = RGBA16Unorm nearest liquid centre (pixel offset from
//                        the texel centre) + its material, Z = nearest pixel
//                        distance. Blend off, strict Less, depth writes, one
//                        candidate quad of 1.7071 raster samples per admitted row.
//   2. label selection   L = R32Uint winning material, Z = D32Float minimum
//                        eligible rho^2. Blend off, strict Less, depth writes.
//   3. accumulation      Q = (sum w*base.rgb, sum w) and
//                        G = (sum w*unit normal.xyz, sum w*alpha)
//                        One/One/Add on every channel, no depth attachment,
//                        foreign-label and terrain rejection per splat.
//   4. smoothing         two separable tiled label-masked Gaussian passes,
//                        sigma = 0.15 world cell, +/-3 sigma, horizontal into
//                        one scratch L/Q/G bundle and vertical into the next.
//   5. composite         categorical four-tap reconstruction of the FILTERED
//                        L/Q/G, a BINARY occupancy decision per scene sample
//                        (D > theta), one deferred response evaluation per
//                        occupied sample, and the union C = max(B, F) that keeps
//                        a particle visible where the density field cannot
//                        resolve it.
//
// The kernel, quadrature and occupancy mathematics live in
// grid_particle_density.slang. There is no temporal history, no blur of lit
// colour and no hardware resolve before the reconstruction; D is a linear HDR
// scalar and the attributes are linear reflectance/normal sums throughout. The
// silhouette is antialiased by the four scene samples alone: the composite writes
// one binary decision per sample and the reconstruction pass reads those samples
// individually, so no per-sample coverage ramp pre-filters the edge.

// The winning label of the current frame's selection pass, one texel per raster
// pixel. Never sampled with a filter: the composite integer-loads exactly four
// taps, so a material id is never interpolated. Outside the smoothing passes
// these three resources are the ACCUMULATION field; the smoothing entries read
// them (and their own scratch bundles) and the composite reads the final bundle.
Texture2D<uint> g_particle_label;
// Q = weighted base reflectance + density; G = weighted normal + alpha. Both are
// read by integer tap, never filtered, so the composite reconstructs exactly the
// four texels it differentiates.
Texture2D<float4> g_particle_base;
Texture2D<float4> g_particle_normal;
// The minimum-presence winner record of the current frame's marker pass, one
// texel per raster pixel: .xy is the nearest liquid particle's screen-space
// centre as an offset from THIS texel's centre in pixels (unorm, +/-4 px), .z is
// that particle's palette material id (unorm /65535) and .w is unused. A zero
// material is the empty record. The composite completes the distance with its own
// sample position, so the marker is never a stored coverage value.
Texture2D<float4> g_particle_marker;

// The authoritative opaque-terrain exclusion at one world-plane position, shared
// by the four splat quadrature points and the per-sample composite. A position is
// excluded only by valid opaque terrain ownership: nonzero matching resolved
// material, no particle winner for that cell, and not optical terrain. This is
// the same rule the direct particle entry applies as its whole-cell guarantee;
// the surface field is the density of visible, non-occluded particle samples.
bool terrainExcludesDensity(float2 fine_position) {
    if ((g_grid_frame.m_has_resolved_cells&kGridFrameHasParticleOwnership)==0u) return false;
    uint cell=resolvedCellIndex(fine_position);
    if (cell==0xffffffffu) return false;
    uint resolved=g_resolved_cells[cell];
    uint material=unpackCellMaterial(resolved);
    if (material==0u) return false;
    GridVisual visual=g_terrain_visuals[min(material,65535u)];
    return terrainOccludesParticle(material,visual.m_material_id,
        (visual.m_surface_flags&kSurfaceOpticalBlend)!=0u,g_best_row[cell]);
}

// ------------------------------------------------------------------
// minimum-presence marker selection
// ------------------------------------------------------------------

// One candidate quad of kParticleMarkerQuadRadius (1.7071) raster samples per
// admitted liquid row, over the same prepared instances and indirect arguments the
// splat passes use. The geometry is the shared splat quad with the marker's own
// screen-space floor, because the reconstruction support is narrower than a pixel
// as soon as the field can no longer resolve the row: a candidate must exist even
// where no density texel does.

// The marker fragment reads the centre offset, the material word and the
// reference radius, so its vertex entry declares exactly those and SV_Position.
// Every declared output is consumed: the stage interface carries no attribute the
// fragment would discard. The geometry is still the one shared expansion.
struct ParticleSurfaceMarkerVertexOutput {
    float4 position:SV_Position;
    nointerpolation uint material:TEXCOORD0;
    float2 local_m:TEXCOORD1;
    nointerpolation float radius_m:TEXCOORD2;
};

[shader("vertex")]
ParticleSurfaceMarkerVertexOutput vertexParticleSurfaceMarker(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    ParticleVertexOutput expanded=particleVertexCommon(vertex,g_particle_ids[instance],true,
        g_particle_surface.m_phase_mask,g_particle_surface.m_pixel_world,
        kParticleMarkerQuadRadius*g_particle_surface.m_pixel_world);
    ParticleSurfaceMarkerVertexOutput output;
    output.position=expanded.position;
    output.material=expanded.material;
    output.local_m=expanded.local_m;
    output.radius_m=g_particles[g_particle_ids[instance]].m_radius_m;
    return output;
}

struct ParticleSurfaceMarkerOutput {
    float4 m_record:SV_Target0;
    // The nearest candidate wins, exactly like the label selection: the synthetic
    // distance is the pixel distance to the particle centre, scaled by 1/4 so
    // every in-quad distance stays strictly below the 1.0 clear value. An exact
    // tie keeps the first stable instance the rasteriser delivers.
    float m_depth:SV_Depth;
};

// One fragment per raster texel. It always writes the record: unlike the label
// selection there is no eligibility test, because the marker's whole purpose is
// the row whose quadrature points are all outside the texel. Terrain occlusion is
// NOT tested here — the composite applies the same per-sample exclusion to the
// marker that it applies to the field, so an occluded dot cannot cross an opaque
// cell, and a marker never influences the label or the density.
[shader("fragment")]
ParticleSurfaceMarkerOutput fragmentParticleSurfaceMarker(ParticleSurfaceMarkerVertexOutput input) {
    ParticleSurfaceMarkerOutput output;
    output.m_record=float4(0.0);
    output.m_depth=1.0;

    // The particle centre is the quad point whose local_m is zero. local_m is
    // affine in the raster position over one candidate quad, so the two screen
    // derivatives invert to the pixel offset of the centre from this fragment:
    // local_m = [ddx(local_m) ddy(local_m)] * offset_px. The projection is a
    // homography, so this is the exact first-order offset at this pixel and the
    // same interpolation the splat fragment already differentiates.
    float2 local_x=ddx(input.local_m);
    float2 local_y=ddy(input.local_m);
    float determinant=local_x.x*local_y.y-local_x.y*local_y.x;
    if (!(abs(determinant)>0.0)) discard;

    float2 offset=float2(local_y.y*input.local_m.x-local_y.x*input.local_m.y,
        local_x.x*input.local_m.y-local_x.y*input.local_m.x)/determinant;
    float distance_px=length(offset);
    // Only a pixel whose CENTRE is inside the candidate radius can hold a sample
    // with positive coverage, so nothing farther is recorded. That also keeps the
    // unorm offset inside its +/-4-pixel range when the density support quad is the
    // larger of the two.
    if (!(distance_px<=kParticleMarkerQuadRadius)) discard;

    // The record's fourth channel is this sample's ENERGY, not a free slot: the dot
    // exists so a row the field cannot resolve keeps a mark, but presence is not
    // radiance. The row's reference area is projected through this fragment's own
    // world-per-raster Jacobian - the determinant that already inverts to the
    // centre offset above - and converted to content pixels, so the factor follows
    // the camera exactly and no per-draw pixel scale is involved. The size of the
    // record does not change and no coverage is stored: the composite still
    // recomputes F from the decoded centre distance.
    float reference_area_content_px2=particleMarkerReferenceAreaContentPx2(input.radius_m,abs(determinant));
    float energy=particleMarkerEnergy(reference_area_content_px2);
    output.m_record=float4(particleMarkerEncodeCentre(offset),particleMarkerEncodeMaterial(input.material),energy);
    output.m_depth=min(distance_px*0.25,kParticleDensityDepthCeiling);
    return output;
}

// ------------------------------------------------------------------
// label selection
// ------------------------------------------------------------------

// The selection and accumulation passes rasterise exactly the same rows, so the
// label the second pass reads is the winner the first pass stored for that same
// footprint. They read different attributes, though, so each declares only its
// own: an auxiliary pass must not publish an output its fragment discards.
struct ParticleSurfaceSelectVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 local_m:TEXCOORD1;
    nointerpolation uint material:TEXCOORD2;
    nointerpolation float4 conic:TEXCOORD3;
};

[shader("vertex")]
ParticleSurfaceSelectVertexOutput vertexParticleSurfaceSelect(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    ParticleVertexOutput expanded=particleVertexCommon(vertex,g_particle_ids[instance],true,
        g_particle_surface.m_phase_mask,g_particle_surface.m_pixel_world,0.0);
    ParticleSurfaceSelectVertexOutput output;
    output.position=expanded.position;
    output.fine_position=expanded.fine_position;
    output.local_m=expanded.local_m;
    output.material=expanded.material;
    output.conic=expanded.conic;
    return output;
}

struct ParticleSurfaceAccumulateVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 local_m:TEXCOORD1;
    nointerpolation uint uv_seed:TEXCOORD2;
    nointerpolation uint canonical:TEXCOORD3;
    nointerpolation float4 conic:TEXCOORD4;
};

[shader("vertex")]
ParticleSurfaceAccumulateVertexOutput vertexParticleSurfaceSplat(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    ParticleVertexOutput expanded=particleVertexCommon(vertex,g_particle_ids[instance],true,
        g_particle_surface.m_phase_mask,g_particle_surface.m_pixel_world,0.0);
    ParticleSurfaceAccumulateVertexOutput output;
    output.position=expanded.position;
    output.fine_position=expanded.fine_position;
    output.local_m=expanded.local_m;
    output.uv_seed=expanded.uv_seed;
    output.canonical=expanded.canonical;
    output.conic=expanded.conic;
    return output;
}

// One fragment per raster texel. A candidate is eligible only if at least one of
// the four quadrature points is unexcluded and inside the conic support; the
// stored synthetic distance is the minimum eligible rho^2, so the nearest
// footprint wins under strict Less and exact ties keep the first instance in
// stable compacted order (the depth test never advances an equal value). A
// completely excluded footprint writes nothing, letting the next eligible label
// of the phase win. The clamp keeps a positive-support depth that rounds to the
// 1.0 clear value writable under Less; it is not a tie epsilon.
struct ParticleSurfaceSelectionOutput {
    uint m_label:SV_Target0;
    float m_depth:SV_Depth;
};

[shader("fragment")]
ParticleSurfaceSelectionOutput fragmentParticleSurfaceSelect(ParticleSurfaceSelectVertexOutput input) {
    ParticleSurfaceSelectionOutput output;
    output.m_label=input.material;
    output.m_depth=1.0;

    float2 local_x=ddx(input.local_m),local_y=ddy(input.local_m);
    float2 fine_x=ddx(input.fine_position),fine_y=ddy(input.fine_position);
    float3 conic=input.conic.xyz;

    bool eligible=false;
    float nearest=1.0;
    for (uint corner=0u;corner<4u;++corner) {
        float2 step=particleDensityQuadratureOffset(corner);
        float2 quadrature=input.local_m+step.x*local_x+step.y*local_y;
        float2 quadrature_fine=input.fine_position+step.x*fine_x+step.y*fine_y;
        if (terrainExcludesDensity(quadrature_fine)) continue;
        float rho2=particleDensityRhoSquared(quadrature,conic);
        if (!(rho2<1.0)) continue;
        nearest=min(nearest,rho2);
        eligible=true;
    }

    if (!eligible) discard;
    output.m_depth=min(nearest,kParticleDensityDepthCeiling);
    return output;
}

// ------------------------------------------------------------------
// attribute accumulation
// ------------------------------------------------------------------

struct ParticleSurfaceAttributeOutput {
    // Q: rgb = sum w * base reflectance, a = sum w (density).
    float4 m_base_density:SV_Target0;
    // G: rgb = sum w * unit normal, a = sum w * surface alpha.
    float4 m_normal_alpha:SV_Target1;
};

// Coherent per-pixel derivatives of one particle sample. ddx/ddy are
// primitive-coherent state (design §4.2): every lane of the 2x2 fragment quad must
// reach them, so they are evaluated by the entry point BEFORE any per-texel
// rejection. Evaluating them inside the accumulation after a label-driven early
// return left them undefined exactly at a material boundary, where adjacent texels
// have different winning labels, and both the quadrature offsets and the texture
// gradients are then unspecified.
struct ParticleSurfaceGradients {
    // d(world-plane offset)/d(pixel) in metres; this particle's raster Jacobian.
    float2 m_local_x;
    float2 m_local_y;
    // d(native-cell position)/d(pixel), for the terrain predicate.
    float2 m_fine_x;
    float2 m_fine_y;
};

// One fragment per raster texel of one accepted same-label splat. The weight is
// the four-point terrain-masked kernel average; the material attributes are
// sampled ONCE at this texel's centre in the particle's own seeded texture
// context, never at the quadrature points (those integrate weight only) and never
// through the response (lighting, emission and refraction are deferred to the
// composite). No CH, BRDF or backdrop input is read here.
ParticleSurfaceAttributeOutput particleSurfaceAccumulate(ParticleSurfaceAccumulateVertexOutput input,uint label,
    ParticleSurfaceGradients gradients,StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler) {
    ParticleSurfaceAttributeOutput output;
    output.m_base_density=float4(0.0);
    output.m_normal_alpha=float4(0.0);

    // D9: only a FOREIGN same-phase label and an unknown palette id write nothing
    // (§4.2 and §9.3 reject a foreign material; the label partition is a material
    // partition, never a presence test). Selection and accumulation rasterise the
    // same quad, use the same coherent gradients and evaluate the same four
    // terrain-masked quadrature points, so positive kernel weight (rho < 1 - 1e-6)
    // always implies selection eligibility (rho^2 < 1) at that same texel. A
    // positive Q at an empty L beyond rounding is therefore a selection or
    // derivative discrepancy to diagnose, not an expected operator property.
    uint material=input.canonical&kParticleMaterialMask;
    if (label!=0u&&label!=material) return output;

    GridVisual visual=g_terrain_visuals[min(material,65535u)];
    if (visual.m_material_id!=material) return output;
    // The admitted positive finite texture period; a malformed period writes no
    // contribution instead of an infinite coordinate.
    float period=visual.m_texture_period_m;
    if (!(period>0.0)||!isfinite(period)) return output;

    // The kernel metric is the stored conic: world XY, camera-independent, with
    // the vertex's eigenvalue correction and the record's own det normalisation.
    float3 conic=input.conic.xyz;
    float sqrt_determinant=input.conic.w;

    float weight=0.0;
    for (uint corner=0u;corner<4u;++corner) {
        float2 step=particleDensityQuadratureOffset(corner);
        float2 quadrature=input.local_m+step.x*gradients.m_local_x+step.y*gradients.m_local_y;
        float2 quadrature_fine=input.fine_position+step.x*gradients.m_fine_x+step.y*gradients.m_fine_y;
        if (terrainExcludesDensity(quadrature_fine)) continue;
        weight+=particleDensityWeight(quadrature,conic,sqrt_determinant);
    }
    weight*=0.25;
    if (!(weight>0.0)) return output;

    // This particle's own element attributes at the texel centre, in the
    // particle-attached seeded frame. The response never runs here.
    VisibleSurfaceSample sample=liquidSurfaceSampleAt(material,
        particleUvContext(input.uv_seed,input.local_m,gradients.m_local_x,gradients.m_local_y,period),
        g_textures,g_sampler);
    float alpha=visibleSurfaceAlpha(visual,sample);

    output.m_base_density=float4(sample.m_base.rgb*weight,weight);
    output.m_normal_alpha=float4(sample.m_normal*weight,alpha*weight);
    return output;
}

[shader("fragment")]
ParticleSurfaceAttributeOutput fragmentParticleSurfaceAccumulate(ParticleSurfaceAccumulateVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) {
    // Derivatives first, unconditionally: the label below is per texel and may
    // differ from a neighbouring fragment's, so no divergent guard may precede
    // ddx/ddy.
    ParticleSurfaceGradients gradients;
    gradients.m_local_x=ddx(input.local_m);
    gradients.m_local_y=ddy(input.local_m);
    gradients.m_fine_x=ddx(input.fine_position);
    gradients.m_fine_y=ddy(input.fine_position);
    int2 texel=int2(floor(input.position.xy));
    return particleSurfaceAccumulate(
        input,g_particle_label.Load(int3(texel.x,texel.y,0)),gradients,g_textures,g_sampler);
}

// ------------------------------------------------------------------
// per-scene-sample deferred composite
// ------------------------------------------------------------------

struct ParticleSurfaceCompositeVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 uv:TEXCOORD1;
};

// The full-raster triangle reconstructs the z = 0 scene plane at each corner with
// the shared receiving-plane mapping, so the composite carries the same clip
// depth the coplanar particles and terrain use rather than a fixed NDC depth.
[shader("vertex")]
ParticleSurfaceCompositeVertexOutput vertexParticleSurfaceComposite(uint vertex:SV_VertexID) {
    float2 uv=float2((vertex<<1u)&2u,vertex&2u);
    float2 world=backgroundWorldAt(uv,g_grid_frame.m_inverse_view_projection);
    ParticleSurfaceCompositeVertexOutput output;
    output.position=mul(float4(world,0.0,1.0),g_grid_frame.m_view_projection);
    output.fine_position=world/g_grid_frame.m_cell_size_m;
    output.uv=uv;
    return output;
}

// One reconstructed attribute set for a scene sample: the winning palette
// material, the mean base reflectance, the normalized mean normal and the mean
// surface alpha.
struct ParticleSurfaceReconstruction {
    uint m_material;
    // The scene sample in target pixels, with texel i centred at i + .5. It is
    // the same coordinate the categorical taps were taken at, so the deferred
    // response's refraction offset is expressed in the raster it was built
    // from.
    float2 m_raster;
    float m_density;
    float3 m_base;
    float3 m_normal;
    float m_alpha;
};

// Categorical four-tap reconstruction at one scene sample. Integer taps only:
// the label is never filtered. `raster` is in target pixels with texel i centred
// at i + .5, the four taps are the bilinear cell around it, and the nearest texel
// centre is the tap at floor(raster), so an exact halfway tie selects the higher
// index. Matching labels contribute values; empty labels keep their weight as
// air, so the mean is attenuated by true vacuum but never by a foreign material;
// foreign labels are removed and the weights renormalised by the same Z. The
// quotient derivative holds the label selection fixed, so the contour width
// follows the reconstructed density and never the discontinuous id.
bool particleSurfaceReconstruct(float2 uv,out ParticleSurfaceReconstruction result,out float derivative_x,
    out float derivative_y) {
    result=(ParticleSurfaceReconstruction)0;
    derivative_x=0.0;
    derivative_y=0.0;

    uint width,height;
    g_particle_label.GetDimensions(width,height);
    int2 extent=int2(int(width),int(height));
    if (width==0u||height==0u) return false;

    // The actual sample position in target pixels, never clamped: clamping to the
    // last texel centre would hold the right/bottom edge at that centre instead of
    // zero-extending it like the left/top edges, and would make the quotient
    // derivative the derivative of the clamped reconstruction. A tap that falls
    // outside the target is air (below), so the nearest label is the only
    // coordinate that needs an explicit bounds check.
    float2 raster=uv*float2(extent);
    if (any(raster<0.0)||any(raster>=float2(extent))) return false;

    int2 base=int2(floor(raster-0.5));
    float2 fraction=raster-0.5-float2(base);
    int2 nearest=int2(floor(raster));
    if (any(nearest<int2(0))||any(nearest>=extent)) return false;

    uint nearest_label=g_particle_label.Load(int3(nearest.x,nearest.y,0));
    if (nearest_label==0u) return false;
    GridVisual visual=g_terrain_visuals[min(nearest_label,65535u)];
    if (visual.m_material_id!=nearest_label) return false;
    // Exactly one liquid class owns each sample. Both composites read the same
    // nearest label, so the class that does not own it writes nothing.
    if (particleSurfaceClass(nearest_label)!=g_particle_surface.m_composite_class) return false;

    float beta[4]={(1.0-fraction.x)*(1.0-fraction.y),fraction.x*(1.0-fraction.y),
        (1.0-fraction.x)*fraction.y,fraction.x*fraction.y};
    float beta_x[4]={-(1.0-fraction.y),1.0-fraction.y,-fraction.y,fraction.y};
    float beta_y[4]={-(1.0-fraction.x),-fraction.x,1.0-fraction.x,fraction.x};

    // Z = sum(beta * e) with e = matching or empty, U = sum(beta * matching
    // value) for the three attribute channels the composite consumes. Only the
    // DENSITY channel needs a derivative (the contour width), so its weighted sum
    // and first derivatives are scalars; the base/normal derivatives are not
    // consumed and are not accumulated.
    float air=0.0,air_x=0.0,air_y=0.0;
    float4 base_sum=float4(0.0);
    float4 normal_sum=float4(0.0);
    float density_sum=0.0,density_x=0.0,density_y=0.0;
    for (uint tap=0u;tap<4u;++tap) {
        int2 texel=base+int2(int(tap&1u),int(tap>>1u));
        // A tap outside the target is zero in every channel: it is air, never a
        // replicated edge texel.
        uint label=0u;
        float4 q=float4(0.0),g=float4(0.0);
        if (all(texel>=int2(0))&&all(texel<extent)) {
            label=g_particle_label.Load(int3(texel.x,texel.y,0));
            if (label==nearest_label) {
                q=g_particle_base.Load(int3(texel.x,texel.y,0));
                g=g_particle_normal.Load(int3(texel.x,texel.y,0));
            }
        }

        float keep=(label==nearest_label||label==0u) ? 1.0 : 0.0;
        air+=beta[tap]*keep;
        air_x+=beta_x[tap]*keep;
        air_y+=beta_y[tap]*keep;
        base_sum+=beta[tap]*q;
        normal_sum+=beta[tap]*g;
        density_sum+=beta[tap]*q.a;
        density_x+=beta_x[tap]*q.a;
        density_y+=beta_y[tap]*q.a;
    }

    if (!(air>0.0)) return false;

    // F = U/Z and the quotient derivative of its density channel, label selection
    // held fixed: d(U.D/Z) = (Ux.D*Z - U.D*Zx)/Z^2.
    derivative_x=(density_x*air-density_sum*air_x)/(air*air);
    derivative_y=(density_y*air-density_sum*air_y)/(air*air);

    result.m_material=nearest_label;
    result.m_raster=raster;
    result.m_density=density_sum/air;
    float density_denominator=max(density_sum,kParticleDensityEpsilon);
    result.m_base=base_sum.rgb/density_denominator;
    float3 normal_vector=normal_sum.rgb/density_denominator;
    result.m_normal=dot(normal_vector,normal_vector)>1.0e-12&&all(isfinite(normal_vector))&&
        normal_vector.z>0.0 ? normalize(normal_vector) : float3(0.0,0.0,1.0);
    result.m_alpha=normal_sum.a/density_denominator;
    return true;
}

// The presence marker of one scene sample: the nearest liquid particle's coverage,
// energy and the material that owns it, all read from the winner record at this
// sample's own raster pixel. The stored centre is a pixel offset from that texel's
// centre, so the sample's sub-pixel position completes the distance; nothing is
// filtered and no coverage value is stored, which is what a half-resolution presence
// image could not preserve. The energy is stored, because it is a property of the
// row and not of the sample's sub-pixel phase.
struct ParticleSurfaceMarker {
    float m_coverage;
    // The winner's linear energy factor, [0,1]: one at or above one content pixel of
    // projected reference area, the area ratio below it. Already the factor the
    // composite applies, so the unorm decode is the value itself.
    float m_energy;
    uint m_material;
    float2 m_raster;
};

// F at this sample, or zero when no candidate owns the pixel. The record is only
// a candidate: it still belongs to the class and ownership rules the field uses,
// so a marker never shades through the other composite and never paints a sample
// the field already assigns to the other class. Both the stored offset and the
// sample position are RESOLVED RASTER SAMPLES, the same unit as the marker radii,
// so the distance needs no conversion here even though the design contract is
// stated in content pixels.
bool particleSurfaceMarkerAt(float2 uv,out ParticleSurfaceMarker result) {
    result=(ParticleSurfaceMarker)0;

    uint width,height;
    g_particle_marker.GetDimensions(width,height);
    if (width==0u||height==0u) return false;

    float2 raster=uv*float2(width,height);
    if (any(raster<0.0)||any(raster>=float2(width,height))) return false;
    int2 texel=int2(floor(raster));

    float4 record=g_particle_marker.Load(int3(texel.x,texel.y,0));
    uint material=particleMarkerDecodeMaterial(record.z);
    if (material==0u) return false;

    // The marker carries an id, never attributes: an id the palette does not carry
    // and an id of the other class write nothing, exactly like the label rule.
    GridVisual visual=g_terrain_visuals[min(material,65535u)];
    if (visual.m_material_id!=material) return false;
    if (particleSurfaceClass(material)!=g_particle_surface.m_composite_class) return false;
    uint nearest=g_particle_label.Load(int3(texel.x,texel.y,0));
    if (nearest!=0u&&particleSurfaceClass(nearest)!=g_particle_surface.m_composite_class) return false;

    float2 sample_offset=raster-(float2(texel)+0.5);
    float distance_px=length(particleMarkerDecodeCentre(record.xy)-sample_offset);
    result.m_coverage=particleMarkerCoverage(distance_px);
    // Already a linear [0,1] factor, so the unorm channel decodes to the value the
    // composite multiplies by. The empty record returned above, so an unstored
    // energy can never be applied.
    result.m_energy=record.w;
    result.m_material=material;
    result.m_raster=raster;
    return result.m_coverage>0.0;
}

// The marker's own material at a resolution the field cannot reach: the element's
// mean authored reflectance, a flat facing normal and the palette's own opacity
// rule. No element texture is sampled, because a dot whose geometry is a lower
// bound on presence has no reliable surface to texture; its coverage is what the
// marker guarantees, not its albedo. An optical material keeps its authored
// opacity, so the dot can never become a fake opaque surface.
VisibleSurfaceSample particleSurfaceMarkerSample(uint material) {
    GridVisual visual=g_terrain_visuals[min(material,65535u)];
    float4 tint=diagnosticTint(visual.m_color_rgba,1.0);
    // The element's own mean authored albedo, so the dot carries the material's
    // colour at subpixel scale. A record without a filtered mean (an untextured
    // element) keeps its authored tint instead of shading the dot black.
    float3 mean=float3(visual.m_filtered_mean_r,visual.m_filtered_mean_g,visual.m_filtered_mean_b);
    if (!any(mean>0.0)) mean=float3(1.0);
    VisibleSurfaceSample sample=(VisibleSurfaceSample)0;
    sample.m_base=float4(tint.rgb*mean,1.0);
    sample.m_normal=float3(0.0,0.0,1.0);
    sample.m_thermal=float3(0.0);
    sample.m_tint_alpha=tint.a;
    sample.m_alpha=1.0;
    sample.m_alpha_reconstructed=false;
    return sample;
}

// One invocation per scene sample. The categorical reconstruction supplies D and
// the same-label attribute means; the density threshold alone decides B, so a
// sample is either occupied or empty and never partially covered. The presence
// marker F is unioned once as C = max(B, F), and B wins an exact tie, so a resolved
// body keeps its own coverage and attributes bit for bit and the marker only ever
// adds the presence a vanished field cannot express. Where the marker is the sole
// source of a sample its own energy factor scales the shaded radiance, so a
// sub-pixel row fades with its projected area instead of burning a hot dot; a
// resolved sample is the field's own body data and is never scaled by it. The
// winning sample's base, normal and alpha then shade ONCE through the shared liquid
// response at this sample's real world position and palette material, and the
// result is premultiplied exactly once. No element texture, seed or UV is sampled
// here. The negligible-coverage floor discards before lighting, which also
// suppresses the opaque class's plane-depth write, so a body sample writes depth on
// exactly the same decision that made it occupied. The authoritative terrain
// exclusion is rechecked at the actual sample, so interpolation can never paint
// fluid or a marker over opaque terrain.
// `uv` and `fine` are evaluated by the entry point: a sample-rate interpolation
// must read the entry's own varyings, so it cannot be performed inside a helper.
float4 particleSurfaceComposite(float2 uv,float2 fine,SamplerState.Handle g_sampler) {
    if (terrainExcludesDensity(fine)) discard;

    // The quotient derivative is still reported by the reconstruction (its own
    // categorical contract, pinned by its probe) but occupancy reads D alone.
    ParticleSurfaceReconstruction reconstruction;
    float derivative_x,derivative_y;
    const bool body=particleSurfaceReconstruct(uv,reconstruction,derivative_x,derivative_y);
    float coverage=body&&reconstruction.m_density>kParticleDensityEpsilon
        ? particleDensityOccupancy(reconstruction.m_density) : 0.0;

    ParticleSurfaceMarker marker;
    const bool marked=particleSurfaceMarkerAt(uv,marker);
    const bool use_marker=marked&&marker.m_coverage>coverage;
    if (use_marker) coverage=marker.m_coverage;
    if (!(coverage>kParticleCoverageFloor)) discard;

    // The field's own attributes are the source whenever they belong to the winning
    // particle: either the body won, or the marker won at a sample the field already
    // resolves with that same material. Only a marker the field lost entirely falls
    // back to the material's mean.
    const bool resolved=!use_marker||(body&&reconstruction.m_material==marker.m_material);
    VisibleSurfaceSample sample=(VisibleSurfaceSample)0;
    uint material=reconstruction.m_material;
    float2 raster=reconstruction.m_raster;
    if (resolved) {
        // The winning label supplied this sample's parameters; Q/G supplied the
        // attributes. The mean alpha is already the authored opacity applied once,
        // so the response must not re-derive it.
        sample.m_base=float4(reconstruction.m_base,1.0);
        sample.m_normal=reconstruction.m_normal;
        sample.m_thermal=float3(0.0);
        sample.m_tint_alpha=1.0;
        sample.m_alpha=reconstruction.m_alpha;
        sample.m_alpha_reconstructed=true;
    } else {
        material=marker.m_material;
        raster=marker.m_raster;
        sample=particleSurfaceMarkerSample(material);
    }

    VisibleSurfaceBackdrop backdrop={raster,float2(0.0),false,float3(0.0)};
    CH no_coefficients={};
    float4 surface=visibleSurfaceResponse(true,false,material,0u,fine,sample,no_coefficients,backdrop,g_sampler);

    // The marker's energy is applied ONLY where the marker is the sole source of the
    // sample (use_marker && !resolved): there the shaded radiance is the marker's own
    // full-radiance stand-in, and a row whose projected reference area is below a
    // content pixel must fade with that area rather than render a hot dot at full
    // material radiance. A resolved sample is the field's own body data the marker
    // merely raised the coverage of, so it keeps its own premultiplied value - the
    // factor is exactly one there, and the expression below reduces to the previous
    // one. That is the documented contract: a resolved body keeps its own coverage
    // and attributes bit for bit, and dimming it would darken a body the field does
    // legitimately resolve. !use_marker is always resolved and its record may not
    // even exist, so the same guard covers it.
    float energy=use_marker&&!resolved ? marker.m_energy : 1.0;
    return float4(energy*coverage*surface.rgb*surface.a,energy*coverage*surface.a);
}

[shader("fragment")]
float4 fragmentParticleSurfaceComposite(ParticleSurfaceCompositeVertexOutput input,uint index:SV_SampleIndex,
    uniform SamplerState.Handle g_sampler):SV_Target {
    float2 uv=EvaluateAttributeAtSample(input.uv,index);
    float2 fine=EvaluateAttributeAtSample(input.fine_position,index);
    return particleSurfaceComposite(uv,fine,g_sampler);
}

// Both liquid classes draw this one program: the class is the pipeline's depth
// state only, and each draw rejects the samples the other class owns through the
// lane's composite-class constant. There is no class-specific shading code.

// ------------------------------------------------------------------
// world-space surface smoothing: two separable tiled passes
// ------------------------------------------------------------------
// The reconstruction influence is a Gaussian of sigma world cells, truncated at
// +/-3 sigma and clamped by the shared tile's widest halo. Both the width and the
// tap count are world-sized: the per-axis scale is this frame's own raster
// pixels-per-cell, so a closer camera grows the kernel in samples and a coarse
// one shrinks it. That is the whole point of the revision - a pixel-fixed stencil
// silently changes the world width with the zoom.
//
// Two passes, in this order: horizontal writes scratch bundle A from the
// accumulation field, vertical reads A and writes the final bundle B. Every
// output texel loads a 16x16 logical tile plus a one-axis halo into shared memory
// (no subgroup intrinsic, so the shape is portable), filters from shared and
// writes exactly one output texel. One group barrier per pass.
//
// The mask is categorical and shared by both passes, so the sequential result is
// deterministic: a tap contributes only when its label equals the mask label.
// Air contributes zero but keeps its weight in the denominator, so the contour
// may extend into vacuum; a foreign nonempty label leaves numerator AND
// denominator, so two materials never bleed and a border never opens a black
// seam. Terrain is never renormalised through: a terrain-blocked source texel is
// loaded as the empty texel, which is exactly the rule's zero-numerator,
// kept-weight treatment, and a blocked OUTPUT texel emits the empty texel.
static const float kParticleSurfaceSmoothingSigma=0.15;
// Logical outputs per group edge. One output per thread: 256 threads per group.
static const uint kParticleSurfaceFilterTile=16u;
// Widest loaded halo along the filtered axis. 16 + 2*8 source texels of 36 bytes
// (uint label + two float4) is 18,432 shared bytes, the design's bound. A wider
// required halo is reported by the host and truncated here, never silently
// claimed as the same smoothing width.
static const uint kParticleSurfaceFilterMaxHalo=8u;
static const uint kParticleSurfaceFilterColumns=kParticleSurfaceFilterTile+2u*kParticleSurfaceFilterMaxHalo;

struct ParticleSurfaceFilterFrame {
    // Raster samples per world native cell for this frame.
    float m_samples_per_cell;
    // The smoothing sigma in world native cells (kParticleSurfaceSmoothingSigma).
    float m_sigma_cell;
    // Loaded halo per axis, in raster samples.
    uint m_halo;
    uint m_pad;
    // The field's raster extent.
    uint m_extent_x;
    uint m_extent_y;
    uint m_pad2;
    uint m_pad3;
};
ConstantBuffer<ParticleSurfaceFilterFrame> g_particle_filter;

// One axis of the separable pair. Both programs declare the same names and each
// entry keeps only what it uses, so the horizontal program binds the accumulation
// field and the vertical program binds the horizontal scratch bundle.
Texture2D<uint> g_particle_filter_source_label;
Texture2D<float4> g_particle_filter_source_base;
Texture2D<float4> g_particle_filter_source_normal;
RWTexture2D<uint> g_particle_filter_target_label;
RWTexture2D<float4> g_particle_filter_target_base;
RWTexture2D<float4> g_particle_filter_target_normal;

// The loaded tile, flat with a runtime row stride: `row` is the non-halo axis and
// `column` runs -halo..tile+halo along the filtered one. 36 bytes per source
// texel.
groupshared uint s_filter_label[kParticleSurfaceFilterTile*kParticleSurfaceFilterColumns];
groupshared float4 s_filter_base[kParticleSurfaceFilterTile*kParticleSurfaceFilterColumns];
groupshared float4 s_filter_normal[kParticleSurfaceFilterTile*kParticleSurfaceFilterColumns];
// The raw Gaussian taps for this frame's scale, evaluated once per group in world
// cells. They are NOT normalized: each output divides by the sum of the weights
// it actually admitted, which is the shared denominator of every channel and of
// the categorical renormalization at a border.
groupshared float s_filter_weight[2u*kParticleSurfaceFilterMaxHalo+1u];

// World-plane native-cell position of one raster texel: the same mapping the
// composite and the background use, so the terrain predicate answers about the
// same plane point the field describes.
float2 particleSurfaceFilterFine(uint2 texel) {
    float2 extent=float2(g_particle_filter.m_extent_x,g_particle_filter.m_extent_y);
    float2 uv=(float2(texel)+0.5)/extent;
    return backgroundWorldAt(uv,g_grid_frame.m_inverse_view_projection)/g_grid_frame.m_cell_size_m;
}

// One axis of the separable filter. `horizontal` selects which axis carries the
// halo. Everything else - the tile shape, the tap weights, the label mask, the
// terrain rule and the output contract - is shared, so the two passes are the same
// operator applied twice.
void particleSurfaceFilterTile(uint3 thread:SV_GroupThreadID,uint3 group:SV_GroupID,bool horizontal) {
    // The extreme group edge and a degenerate frame are the only guards needed:
    // halo, stride and the extent are read once so the two entry points cannot
    // disagree about the kernel they are applying.
    uint halo=min(g_particle_filter.m_halo,kParticleSurfaceFilterMaxHalo);
    uint stride=kParticleSurfaceFilterTile+2u*halo;
    uint2 extent=uint2(g_particle_filter.m_extent_x,g_particle_filter.m_extent_y);
    uint2 origin=uint2(group.x,group.y)*kParticleSurfaceFilterTile;
    uint2 local=uint2(thread.x,thread.y);
    uint lanes=kParticleSurfaceFilterTile*kParticleSurfaceFilterTile;
    uint lane=thread.y*kParticleSurfaceFilterTile+thread.x;

    // One lane per tap evaluates the exponential in WORLD CELLS; every output of
    // the group reuses the table. The scale guard only keeps a degenerate frame
    // from dividing by zero.
    if (lane<2u*halo+1u) {
        float cells=float(int(lane)-int(halo))/max(g_particle_filter.m_samples_per_cell,1.0e-6);
        float width=cells/g_particle_filter.m_sigma_cell;
        s_filter_weight[lane]=exp(-0.5*width*width);
    }

    // Cooperative load of the 16 x (16 + 2*halo) source tile. Out of bounds is
    // air, never a replicated edge texel.
    uint count=kParticleSurfaceFilterTile*stride;
    for (uint index=lane;index<count;index+=lanes) {
        uint row=index/stride;
        uint column=index%stride;
        int2 texel=horizontal
            ? int2(int(origin.x)+int(column)-int(halo),int(origin.y)+int(row))
            : int2(int(origin.x)+int(row),int(origin.y)+int(column)-int(halo));

        uint label=0u;
        float4 base=float4(0.0),normal=float4(0.0);
        if (all(texel>=int2(0))&&all(texel<int2(int(extent.x),int(extent.y)))) {
            uint2 inside=uint2(texel);
            label=g_particle_filter_source_label.Load(int3(int2(inside),0));
            base=g_particle_filter_source_base.Load(int3(int2(inside),0));
            normal=g_particle_filter_source_normal.Load(int3(int2(inside),0));
            // A terrain-blocked source texel loads as the empty texel: zero
            // numerator, weight still in the denominator, exactly the specified
            // treatment of a blocked tap (including a blocked foreign one).
            if (terrainExcludesDensity(particleSurfaceFilterFine(inside))) {
                label=0u;
                base=float4(0.0);
                normal=float4(0.0);
            }
        }

        uint slot=row*stride+column;
        s_filter_label[slot]=label;
        s_filter_base[slot]=base;
        s_filter_normal[slot]=normal;
    }

    // Every lane reaches the barrier, including the whole out-of-range edge group.
    GroupMemoryBarrierWithGroupSync();

    uint2 texel=origin+local;
    if (texel.x>=extent.x||texel.y>=extent.y) return;

    // A terrain-blocked output is empty: the filter never paints over opaque
    // terrain, and the next pass then reads it as air.
    if (terrainExcludesDensity(particleSurfaceFilterFine(texel))) {
        g_particle_filter_target_label[texel]=0u;
        g_particle_filter_target_base[texel]=float4(0.0);
        g_particle_filter_target_normal[texel]=float4(0.0);
        return;
    }

    uint row=horizontal ? local.y : local.x;
    int centre=int(row*stride+halo+(horizontal ? local.x : local.y));

    // The mask label. A nonempty centre keeps its own label, so the output can
    // never acquire a foreign material's attributes. An empty centre adopts the
    // nearest nonzero tap label (ties: smaller unsigned label, then the smaller
    // signed offset), which is the deterministic halo rule of BOTH passes; its
    // only effect is to let an existing surface extend into air, never to create
    // density on its own and never selected by density.
    uint mask=s_filter_label[centre];
    if (mask==0u) {
        for (uint distance=1u;distance<=halo;++distance) {
            uint negative=s_filter_label[centre-int(distance)];
            uint positive=s_filter_label[centre+int(distance)];
            if (negative==0u&&positive==0u) continue;
            if (negative==0u) { mask=positive; break; }
            if (positive==0u) { mask=negative; break; }
            mask=negative<=positive ? negative : positive;
            break;
        }
    }

    if (mask==0u) {
        g_particle_filter_target_label[texel]=0u;
        g_particle_filter_target_base[texel]=float4(0.0);
        g_particle_filter_target_normal[texel]=float4(0.0);
        return;
    }

    // Coherent masked accumulation over offsets -halo..halo. Q.rgb, Q.a, G.rgb and
    // G.a all use these weights and this one denominator, so the attributes stay
    // premultiplied and the composite's ratio is unchanged by filtering.
    float4 base=float4(0.0),normal=float4(0.0);
    float denominator=0.0;
    for (uint tap=0u;tap<2u*halo+1u;++tap) {
        int neighbour=centre+int(tap)-int(halo);
        uint label=s_filter_label[neighbour];
        if (label!=0u&&label!=mask) continue;

        float weight=s_filter_weight[tap];
        denominator+=weight;
        if (label==mask) {
            base+=weight*s_filter_base[neighbour];
            normal+=weight*s_filter_normal[neighbour];
        }
    }

    float inverse=denominator>0.0 ? 1.0/denominator : 0.0;
    g_particle_filter_target_label[texel]=mask;
    g_particle_filter_target_base[texel]=base*inverse;
    g_particle_filter_target_normal[texel]=normal*inverse;
}

// Horizontal: halo along x, accumulation field in, first scratch bundle out.
[shader("compute")]
[numthreads(16,16,1)]
void smoothParticleSurfaceHorizontal(uint3 thread:SV_GroupThreadID,uint3 group:SV_GroupID) {
    particleSurfaceFilterTile(thread,group,true);
}

// Vertical: halo along y, first scratch bundle in, final field out.
[shader("compute")]
[numthreads(16,16,1)]
void smoothParticleSurfaceVertical(uint3 thread:SV_GroupThreadID,uint3 group:SV_GroupID) {
    particleSurfaceFilterTile(thread,group,false);
}

// ------------------------------------------------------------------
// isolated qualification probe for the actual material palette ABI
// ------------------------------------------------------------------

struct MaterialOpticsProbeParameters {
    uint count, reserved0, reserved1, reserved2;
};
struct MaterialOpticsProbe {
    uint model;
    float sigmaR, sigmaG, sigmaB;
    float sourceR, sourceG, sourceB;
    uint status;
    float volumeR, volumeG, volumeB;
    uint reserved;
};
ConstantBuffer<MaterialOpticsProbeParameters> g_material_optics_probe_params;
RWStructuredBuffer<MaterialOpticsProbe> g_material_optics_probe;

[shader("compute")]
[numthreads(64,1,1)]
void qualifyMaterialOptics(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= g_material_optics_probe_params.count)
        return;

    GridVisual visual = g_terrain_visuals[thread.x];
    float3 sigma = float3(visual.m_sigma_r_per_m, visual.m_sigma_g_per_m, visual.m_sigma_b_per_m);
    float3 source = visual.m_emissive_rgb * visual.m_emissive_strength;
    float3 volume = sigma * source;
    uint model = visual.m_transport_model;
    uint status = (model == 0x101u || model == 0x102u || model == 0x103u) ? 0 : 1;
    if (!all(isfinite(visual.m_emissive_rgb)) || any(visual.m_emissive_rgb < 0.0) || any(visual.m_emissive_rgb > 1.0) ||
        !isfinite(visual.m_emissive_strength) || visual.m_emissive_strength < 0.0 ||
        !all(isfinite(sigma)) || any(sigma < 0.0) || !all(isfinite(source)) || any(source < 0.0) ||
        !all(isfinite(volume)) || any(volume < 0.0) ||
        any((visual.m_emissive_rgb > 0.0) && (visual.m_emissive_strength > 0.0) && (source == 0.0)) ||
        any((sigma > 0.0) && (source > 0.0) && (volume == 0.0)) ||
        (model != 0x103u && any(sigma != 0.0)) || (model == 0x101u && any(source != 0.0)))
        status |= 2;

    MaterialOpticsProbe result = {model, sigma.x, sigma.y, sigma.z, source.x, source.y, source.z,
        status, volume.x, volume.y, volume.z, 0};
    g_material_optics_probe[thread.x] = result;
}

// ------------------------------------------------------------------
// isolated bit contract for the actual raster roundness call paths
// ------------------------------------------------------------------

struct ContourRoundnessProbeParameters {
    uint count, reserved0, reserved1, reserved2;
};
ConstantBuffer<ContourRoundnessProbeParameters> g_contour_roundness_probe_params;
StructuredBuffer<uint2> g_contour_roundness_pairs;
RWStructuredBuffer<uint4> g_contour_roundness_probe;

// Bind the ordinary 304-byte GridFrame with surface_mode mask2 (numeric value2)
// set, the real 96-byte terrain palette, and the shared points/cases. Distinct
// nonzero pair IDs may carry equal roundness values. No production draw binds
// these probe resources or invokes this entry.
[shader("compute")]
[numthreads(64,1,1)]
void qualifyRasterContourRoundness(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= g_contour_roundness_probe_params.count) { return; }

    uint2 pair = g_contour_roundness_pairs[thread.x];
    uint shared_materials[4] = {pair.x, pair.y, pair.x, pair.y};
    float shared;
    bool has_shared = sharedSolidRoundness(shared_materials, shared);

    uint outer_materials[4] = {pair.x, pair.y, pair.y, 0u};
    float outer = outerSegmentRoundness(outer_materials, g_contour_cases[7u].m_segments[0]);
    g_contour_roundness_probe[thread.x] = uint4(asuint(outer), asuint(shared), has_shared ? 1u : 0u, 0u);
}

