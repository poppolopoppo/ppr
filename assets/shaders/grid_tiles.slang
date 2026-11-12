#include "world_lighting_codec.slang"
// The one particle shape constant set, shared with the prepare cull.
#include "grid_particle_native.slang"

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
    uint pad1, pad2;
};
ConstantBuffer<GridPassSwitch> g_grid_pass;
Texture2D<float4> g_boundary_noise;
// Written before the surface pass; this is never the active color attachment.
Texture2D<float4> g_backdrop;
// Renderer-owned packed 24-byte particle presentation row; the writer is
// PresentationRow in grid_particle_prepare.slang and the CPU twin is
// pP::ParticleGpu in App.Renderer.GridPass.cppm. Position and radius are world
// metres (not cells and not quantised); the material, tint and motion fields are
// packed. Every field name and offset reflects against the CPU twin.
struct ParticleGpu {
    float2 m_position;                    // 0:  world metres
    float m_radius_m;                     // 8:  metres
    uint m_material_flags_temperature;    // 12: material 12 | class 4 << 12 | binary16 K << 16
    uint m_color_rgb9e5;                  // 16: RGB9E5 tint; alpha is not stored
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

// Shared-exponent RGB9E5 decode, matching the host lighting::unpackRGB9E5.
float3 unpackParticleTint(uint word) {
    int exponent=int((word>>27u)&0x1fu)-24;
    return float3(float(word&0x1ffu),float((word>>9u)&0x1ffu),float((word>>18u)&0x1ffu))*exp2(float(exponent));
}
Texture2D<float4> g_particle_mask;
SamplerState g_particle_mask_sampler;
StructuredBuffer<ParticleGpu> g_particles;
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
static const uint kResolvedEdge = 1024u;
StructuredBuffer<uint> g_resolved_cells;
// Committed per-native-cell winner grid from the same projection attempt, one
// word per cell; 0xffffffff means no particle winner. Terrain reads it only to
// retire the categorical base of particle-owned cells (plan §3). It is
// read-only here and never identifies ownership by material.
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

// Linear tint times the visible albedo. The filtered-mean variance normalization
// is the approved approximation; alpha keeps ordinary barycentric blending.
float4 normalizeVisibleAlbedo(
    GridVisual visual, float4 albedo, float3 weights, bool repeat_texture) {
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    float3 mean = float3(visual.m_filtered_mean_r, visual.m_filtered_mean_g, visual.m_filtered_mean_b);
    if (!repeat_texture) {
        albedo.rgb = saturate(mean + (albedo.rgb - mean) * rsqrt(dot(weights, weights)));
    }

    return float4(tint.rgb * albedo.rgb, albedo.a);
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

float4 categoricalMaterialSurface(uint material, uint resolved, float2 fine_position,
    float2 gradient_x, float2 gradient_y, CH coefficients,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler,
    bool premultiplied) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    float period = visual.m_texture_period_m;
    float2 world = fine_position * g_grid_frame.m_cell_size_m;
    float2 uv = world / period;
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches;
    if (repeat_texture) {
        patches.uv0 = frac(uv);
        patches.uv1 = patches.uv0;
        patches.uv2 = patches.uv0;
        patches.weights = float3(1.0, 0.0, 0.0);
    } else {
        patches = texturePatches(uv);
    }

    // Derivatives belong to the continuous world UV, before lattice/hash/fract.
    // All albedo and normal patches use these same gradients and translations.
    float2 uv_gradient_x = textureUvGradient(gradient_x, period);
    float2 uv_gradient_y = textureUvGradient(gradient_y, period);

    // Visible base color is sampled before metallic reduction selects kD.
    float4 base = visibleBaseColor(
        visual, patches, repeat_texture, uv_gradient_x, uv_gradient_y, textures, sampler);
    float metallic = visual.m_metallic;
    float roughness = visual.m_roughness;
    float eta = visual.m_ior;

    float3 normal = sampleVisibleNormal(
        visual, patches, repeat_texture, uv_gradient_x, uv_gradient_y, textures, sampler);

    float3 view = surfaceToCamera(fine_position);
    float n_dot_v = visibleNoV(normal, view);
    float2 environment = brdfLutPair(g_brdf_lut, g_brdf_sampler, n_dot_v, roughness);
    // `coefficients` is supplied by the caller: the terrain path requires a
    // published fine CH, while a particle world without one shades from an
    // explicit zero field instead of indexing the one-element fallback.
    // The approved transport surface scope also bounds visible emission:
    // receive-only transparent cells and gas never acquire a thermal or authored
    // glow, while opaque surfaces and the approved emissive-liquid class do.
    bool occupied = categoricalCellOccupied(resolved, visual);
    float3 authored = occupied ? unpackSourceEmission(g_source_materials[material]) : float3(0.0);
    float3 thermal = occupied ? thermalRGB(unpackCellTemperature(resolved)) : float3(0.0);
    float3 hdr = visibleMaterialResponse(
        base.rgb, metallic, roughness, eta, normal, view, coefficients, environment, authored, thermal);

    bool optical_blend = (visual.m_surface_flags & kSurfaceOpticalBlend) != 0u;
    float opacity = optical_blend ? saturate(tint.a * base.a * visual.m_opacity) : 1.0;
    if (premultiplied) hdr *= opacity;
    return float4(hdr, opacity);
}

// Optical opacity and geometric coverage are separate. Native zero-offset RGB
// stays premultiplied so actual destination samples supply transmission.
// Callers blend coverage once.
float4 materialSurface(uint material, float2 fine_position, float2 gradient_x,
    float2 gradient_y, float2 screen, float2 decoration, bool reuse_background, float3 unshifted_background,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler, float2 receiver_position, bool moving_surface=false, bool premultiplied=false) {
    GridVisual visual = g_terrain_visuals[min(material, 65535u)];
    float4 tint = diagnosticTint(visual.m_color_rgba, 1.0);
    float period = visual.m_texture_period_m;
    float2 world = fine_position * g_grid_frame.m_cell_size_m;
    float2 uv = world / period;
    bool repeat_texture = (visual.m_surface_flags & kSurfaceTextureRepeat) != 0u;
    TexturePatches patches;
    if (repeat_texture) {
        patches.uv0 = frac(uv);
        patches.uv1 = patches.uv0;
        patches.uv2 = patches.uv0;
        patches.weights = float3(1.0, 0.0, 0.0);
    } else {
        patches = texturePatches(uv);
    }
    // Derivatives belong to the continuous world UV, before lattice/hash/fract.
    // All albedo and normal patches use these same gradients and translations.
    float2 uv_gradient_x = gradient_x / period;
    float2 uv_gradient_y = gradient_y / period;
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

    float3 color = tint.rgb * albedo.rgb;
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
    float opacity = !optical_blend ? 1.0 : saturate(tint.a * albedo.a * visual.m_opacity);
    if (g_grid_frame.m_has_background == 0u) {
        float4 surface=decorateSurface(float4(color,opacity),decoration);
        if (premultiplied) surface.rgb*=surface.a;
        return surface;
    }
    if (opacity >= 1.0) { return decorateSurface(float4(color, 1.0), decoration); }
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
    float3 background = reuse_background && all(offset_pixels == 0.0)
        ? unshifted_background : backdropColor(screen + offset_pixels, sampler);
    float3 sigma = float3(visual.m_sigma_r_per_m, visual.m_sigma_g_per_m, visual.m_sigma_b_per_m);
    if (any(sigma > 0.0)) {
        background *= exp(-sigma * visual.m_refraction_depth_m);
    }

    // Blend over the destination (particles already drawn) with the material
    // opacity, never a full replace: the refracted backdrop folds into the
    // transmitted (1-opacity) term while the destination keeps its particles.
    float4 surface = decorateSurface(float4(lerp(background, color, opacity), opacity), decoration);
    if (premultiplied) surface.rgb *= surface.a;
    return surface;
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

struct ParticleVertexOutput {
    float4 position:SV_Position;
    float2 fine_position:TEXCOORD0;
    float2 uv:TEXCOORD1;
    nointerpolation uint material:TEXCOORD2;
    nointerpolation float4 color:COLOR0;
    // Quad corner in units of the row's physical radius r: 64/62 for the gas
    // mask, and the shared support 2.0 for both restored classes (the
    // fragment's phase SDF and falloff produce the actual silhouette).
    nointerpolation float shape_scale:TEXCOORD3;
    // Canonical packCell word rebuilt from the row: material | temperature << 16.
    nointerpolation uint canonical:TEXCOORD4;
};

ParticleVertexOutput particleVertex(uint vertex,uint instance) {
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
    // discard used to do.
    bool owned = draw_class==kParticleDrawGas ? is_gas
        : draw_class==kParticleDrawOpaque ? (!is_gas&&!is_optical)
        : (draw_class==kParticleDrawOptical&&!is_gas&&is_optical);
    if (!owned) {
        output.position=float4(2.0,2.0,2.0,1.0);
        return output;
    }

    // Gas keeps the shared mask texture and its two-texel transparent border;
    // the restored classes use the analytic shapes. Both shapes are inscribed in
    // the same support circle R = 2.0 r, so one quad encloses either and the
    // fragment's phase test selects the actual silhouette. Sizing the quad from
    // the draw class instead would clip an opaque liquid's disk.
    float extent=draw_class==kParticleDrawGas ? particle.m_radius_m*(64.0/62.0)
        : particle.m_radius_m*kParticleSupportScale;
    float3 right=normalize(mul(float4(1,0,0,0),g_grid_frame.m_inverse_view_projection).xyz);
    float3 up=normalize(mul(float4(0,1,0,0),g_grid_frame.m_inverse_view_projection).xyz);
    float3 world=float3(particle.m_position,0)+(right*corners[vertex].x+up*corners[vertex].y)*extent;
    output.position=mul(float4(world,1),g_grid_frame.m_view_projection);
    output.fine_position=world.xy/g_grid_frame.m_cell_size_m;
    output.uv=corners[vertex]*0.5+0.5;
    output.material=material;
    output.color=float4(unpackParticleTint(particle.m_color_rgb9e5),1.0);
    output.shape_scale=extent/particle.m_radius_m;
    output.canonical=material|(particle.m_material_flags_temperature&0xffff0000u);
    return output;
}

StructuredBuffer<uint> g_particle_ids;

[shader("vertex")]
ParticleVertexOutput vertexParticleIndirectMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    return particleVertex(vertex,g_particle_ids[instance]);
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

float4 shadeParticleReactive(ParticleVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample,out float geometric_support) {
    geometric_support=0;
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
    // Gas is alpha-composited over the already shaded scene. Its color uses the
    // current decoded CH angular mean; it never relights the background and
    // adds no authored/thermal transport emission or ambient floor. Without a
    // published fine-CH field (legacy fixtures) the fallback buffer is too small
    // to index, so the contribution is explicitly zero.
    float3 lighting = float3(0.0);
    if (g_grid_frame.m_has_resolved_cells != 0u) {
        lighting = angularMean(input.fine_position);
    }

    float opacity=alpha*input.color.a*materialAlpha*visual.m_opacity*(1.0-coverage);
    float3 color=input.color.rgb*lighting;
    return float4(point_sample?color*opacity:color,opacity);
}

float4 shadeParticle(ParticleVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample) {
    float unused_reactive;
    return shadeParticleReactive(input,g_textures,g_sampler,point_sample,unused_reactive);
}

[shader("fragment")]
float4 fragmentParticleMain(ParticleVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler):SV_Target {
    return shadeParticle(input,g_textures,g_sampler,false);
}

[shader("fragment")]
float4 fragmentParticleSpatial(ParticleVertexOutput input,uint index:SV_SampleIndex,
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
GridReactiveOutput fragmentParticleReactive(ParticleVertexOutput input,uint index:SV_SampleIndex,
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
// choice is independent of the draw class, which selects only depth/blend: an
// opaque liquid (liquid phase without kSurfaceOpticalBlend) is drawn by the
// opaque class and still shades through the liquid description, and an optical
// solid is drawn by the optical class and still shades through the solid one.

// Signed distance to the solid rounded-square boundary, in units of the row's
// physical radius: distance to the inset rectangle (half-extent a-c) plus the
// corner arc c. It is a distance comparison, never a mask texture fetch. The
// liquid silhouette is the radial falloff below, so it needs no SDF.
float particleSolidDistance(float2 local) {
    float2 offset=abs(local)-float2(kParticleSolidInnerHalfExtentScale,kParticleSolidInnerHalfExtentScale);
    return length(max(offset,float2(0.0)))+min(max(offset.x,offset.y),0.0)-kParticleSolidCornerScale;
}

// C1 radial falloff of the presentation support, shared by both phases. With
// rho = |local| / R it is 1 inside R_in = (2/3) R, falls as smoothstep(1, 2/3,
// rho) to 0 at R and is 0 beyond. t is written increasing so this is the ordinary
// smoothstep polynomial, and both joins have zero slope.
float particleFalloff(float2 local) {
    float rho=length(local)/kParticleSupportScale;
    float t=saturate((1.0-rho)/(1.0-kParticleFalloffInnerFraction));
    return t*t*(3.0-2.0*t);
}

float4 shadeSolidLiquidReactive(ParticleVertexOutput input,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler,bool point_sample,out float geometric_support) {
    geometric_support=0;
    // The draw class is the depth/blend state only; it never selects the
    // description family.
    bool blend_class=g_grid_pass.particle_class==kParticleDrawOptical;

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

    // Derivatives are prepared before any per-pixel reject so the texture
    // gradients stay coherent across the quad.
    float2 world_gradient_x=clamp(ddx(input.fine_position)*g_grid_frame.m_cell_size_m,-2048.0,2048.0);
    float2 world_gradient_y=clamp(ddy(input.fine_position)*g_grid_frame.m_cell_size_m,-2048.0,2048.0);
    ParticleTerrainFootprint footprint=particleTerrainFootprint(input.fine_position,g_sampler);
    float2 local=(input.uv*2.0-1.0)*input.shape_scale;
    float falloff=particleFalloff(local);
    // A blended class shapes its edge with alpha; a non-blending class cannot,
    // so its silhouette is the alpha = 0.5 contour (the R_s circle, plan §3).
    // Solids also carry their rounded-square mask.
    bool inside_solid_shape=liquid || particleSolidDistance(local)<=0.0;
    if (!inside_solid_shape || falloff<=(blend_class ? 0.0 : kParticleDiscardThreshold)) discard;

    // The per-pixel terrain hard clip is kept as the second line of defence for
    // pixels where draw order alone is not enough.
    float terrain_coverage=particleOpaqueCoverage(footprint,point_sample);
    if (terrain_coverage==1.0) discard;

    geometric_support=1;

    // A blended instance yields to the falloff and to partial terrain coverage;
    // an opaque class writes straight.
    float coverage_scale=blend_class ? falloff*(1.0-terrain_coverage) : 1.0;

    if (liquid) {
        // Liquid description: the material's own albedo/normal, IOR, optical
        // thickness and absorption sigmas through the optical surface, which
        // refracts the backdrop copy and applies Beer-Lambert absorption. The
        // surface is premultiplied only when the draw class blends; an opaque
        // liquid is written straight by the depth-writing opaque class.
        float4 surface=materialSurface(material,input.fine_position,world_gradient_x,world_gradient_y,
            input.position.xy,float2(0.0,0.0),false,float3(0.0),g_textures,g_sampler,
            input.fine_position,false,blend_class);
        if (!blend_class) return surface;
        return float4(surface.rgb*coverage_scale,surface.a*coverage_scale);
    }

    // Solid description: the material's own albedo texture with its period and
    // patching, normal map and scale, tint, metallic, roughness, authored
    // emission and the temperature classification carried in the canonical
    // word, through the PBR surface so the CH/BRDF response is preserved. A
    // world with no published fine CH shades from an explicit zero field. An
    // optical solid is premultiplied for the blending class; the opaque class
    // writes it straight.
    CH coefficients={};
    if (g_grid_frame.m_has_resolved_cells!=0u) coefficients=sampleFineCH(input.fine_position);
    float4 surface=categoricalMaterialSurface(material,input.canonical,input.fine_position,world_gradient_x,
        world_gradient_y,coefficients,g_textures,g_sampler,blend_class);
    if (!blend_class) return surface;
    return float4(surface.rgb*coverage_scale,surface.a*coverage_scale);
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

