// Grid tile pass: flat tinted or per-cell quads, one instance per visible chunk-sized
// tile. Row-major, row-vector convention: transforms call
// mul(vector, matrix), never mul(matrix, vector). View-projection remains
// view * projection with [0,1] depth, no transpose, no Y flip. No dynamic lighting,
// world-space catalog texturing, no GI inputs of any kind. Catalog sessions use the shared
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
};

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
struct ContourTriangle {
    uint m_a;
    uint m_b;
    uint m_c;
    uint m_solid_corners;
};
struct ContourCase {
    uint m_segment_count;
    uint m_triangle_count;
    uint2 m_segments[2];
    ContourTriangle m_triangles[3];
};
StructuredBuffer<float2> g_contour_points;
StructuredBuffer<ContourCase> g_contour_cases;
StructuredBuffer<uint> g_chunk_slots;
// CPU GridTerrainVisual: 80 bytes. Scalar mean components occupy offsets 20/24/28.
// Texture index here is
// the resident descriptor slot, resolved from the catalog's source index.
struct TerrainVisual {
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
};
static const uint kSurfaceOpticalBlend = 1u;
static const uint kSurfaceTextureRepeat = 2u;
StructuredBuffer<TerrainVisual> g_terrain_visuals;
Texture2D<float4> g_boundary_noise;
// Written before the surface pass; this is never the active color attachment.
Texture2D<float4> g_backdrop;
struct ParticleGpu {
    float2 m_position;
    float m_radius_m;
    float m_fill;
    uint m_material_id;
    uint m_phase;
    uint2 m_owner;
};
StructuredBuffer<ParticleGpu> g_particles;
struct DiagnosticColor {
    uint m_color_rgba;
    float m_fill_height_fine;
    uint2 m_reserved;
};
StructuredBuffer<uint> g_diagnostic_indices;
StructuredBuffer<DiagnosticColor> g_diagnostic_colors;

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

float contourCross(float2 a, float2 b) {
    return a.x * b.y - a.y * b.x;
}

// Inverse of the per-axis quadratic map. Zero roundness keeps straight
// midpoint cuts; one reproduces the original rounded Bezier boundary.
float2 contourWarp(float2 point, float roundness) {
    float2 lower = min(point, 1.0 - point);
    float scale = 1.0 + roundness;
    float2 mapped = 2.0 * lower / (scale + sqrt(max(scale * scale - 8.0 * roundness * lower, 0.0)));
    return select(point <= 0.5, mapped, 1.0 - mapped);
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
    roundness = 0.5 * (materialRoundness(first) + materialRoundness(second));
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

float roundedSegmentDistance(float2 point, uint2 segment, float roundness, out float2 nearest, out float2 normal) {
    float2 a = g_contour_points[segment.x];
    float2 b = g_contour_points[segment.y];
    float2 delta = b - a;
    float t = saturate(dot(point - a, delta) / dot(delta, delta));
    nearest = a + t * delta;
    normal = float2(-delta.y, delta.x);
    if (roundness != 0.0 && delta.x != 0.0 && delta.y != 0.0) {
        float2 control = lerp((a + b) * 0.5, float2(0.5), roundness);
        float2 acceleration = 2.0 * (a - 2.0 * control + b);
        float best = min(dot(point - a, point - a), dot(point - b, point - b));
        float best_t = dot(point - a, point - a) < dot(point - b, point - b) ? 0.0 : 1.0;
        // Bounded closest-point solves from both ends and the middle avoid
        // selecting a local maximum on the inside of a convex arc.
        for (uint seed = 0u; seed < 3u; ++seed) {
            t = 0.5 * float(seed);
            for (uint iteration = 0u; iteration < 6u; ++iteration) {
                float2 curve = lerp(lerp(a, control, t), lerp(control, b, t), t);
                float2 tangent = 2.0 * lerp(control - a, b - control, t);
                float numerator = dot(curve - point, tangent);
                float denominator = dot(tangent, tangent) + dot(curve - point, acceleration);
                if (abs(denominator) > 1.0e-8) {
                    t = saturate(t - clamp(numerator / denominator, -0.25, 0.25));
                }
            }
            float2 curve = lerp(lerp(a, control, t), lerp(control, b, t), t);
            float distance = dot(point - curve, point - curve);
            if (distance < best) { best = distance; best_t = t; }
        }
        nearest = lerp(lerp(a, control, best_t), lerp(control, b, best_t), best_t);
        float2 tangent = lerp(control - a, b - control, best_t);
        normal = float2(-tangent.y, tangent.x);
    }
    normal /= max(length(normal), 1.0e-6);
    return length(point - nearest);
}

float contourPixelSize(float2 normal, float2 pixel_x, float2 pixel_y) {
    return max(length(float2(dot(normal, pixel_x), dot(normal, pixel_y))), 1.0e-6);
}

bool contourContains(uint mask, float2 point) {
    ContourCase shape = g_contour_cases[mask];
    for (uint index = 0u; index < shape.m_triangle_count; ++index) {
        ContourTriangle triangle = shape.m_triangles[index];
        float2 a = g_contour_points[triangle.m_a];
        float2 b = g_contour_points[triangle.m_b];
        float2 c = g_contour_points[triangle.m_c];
        if (contourCross(b - a, point - a) >= 0.0 &&
            contourCross(c - b, point - b) >= 0.0 &&
            contourCross(a - c, point - c) >= 0.0) {
            return true;
        }
    }
    return false;
}

// Label shapes provide continuous scores; their maximum partitions the entire
// occupied region, including gaps between independently rounded label shapes.
uint materialMask(uint materials[4], uint corner) {
    uint mask = 0u;
    for (uint other = 0u; other < 4u; ++other) {
        if (materials[other] == materials[corner]) { mask |= 1u << other; }
    }
    return mask;
}

// A saddle cannot connect both opposing materials in the plane. The lower
// solid ID wins that tie; a repeated solid always connects across empty corners.
// Use the opposite case's boundary and complement only for that saddle label.
bool connectedSaddle(uint materials[4], uint corner, uint mask) {
    if (mask != 5u && mask != 10u) { return false; }
    uint other = materials[(corner + 1u) & 3u];
    uint opposite = materials[(corner + 3u) & 3u];
    return other != opposite || other == 0u || other == 0xffffu || materials[corner] < other;
}

// Signed scores preserve the existing rounded label contours while making
// their bisectors shared interfaces, even outside every individual region.
float contourScore(float2 point, uint mask, float roundness, bool connected, out float2 gradient) {
    ContourCase shape = g_contour_cases[mask];
    float distance = 1.0e30;
    float2 closest = point;
    float2 closest_normal = float2(0.0);
    for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
        float2 nearest, normal;
        float candidate = roundedSegmentDistance(point, shape.m_segments[edge], roundness, nearest, normal);
        if (candidate < distance) {
            distance = candidate;
            closest = nearest;
            closest_normal = normal;
        }
    }
    bool inside = contourContains(mask, contourWarp(point, roundness)) != connected;
    if (distance > 1.0e-6) {
        gradient = (inside ? 1.0 : -1.0) * (point - closest) / distance;
    } else {
        bool normal_inside = contourContains(mask, contourWarp(closest + 1.0e-4 * closest_normal, roundness)) != connected;
        gradient = normal_inside ? closest_normal : -closest_normal;
    }
    return inside ? distance : -distance;
}

uint occupiedMask(uint materials[4]) {
    uint mask = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        if (materials[corner] != 0u && materials[corner] != 0xffffu) { mask |= 1u << corner; }
    }
    return mask;
}

bool occupiedSaddleConnected(uint materials[4], uint mask) {
    return (mask == 5u && materials[0] == materials[2]) ||
        (mask == 10u && materials[1] == materials[3]);
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
    return 0.5 * (outerEndpointRoundness(materials, g_contour_points[segment.x]) +
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
    out float coverage, out int2 winner, out float internal_outline) {
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
    coverage = occupiedCoverage(materials, point, width);
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

// Optical opacity and geometric coverage are separate. The returned RGB is
// already composed with the immutable backdrop; callers blend coverage once.
float4 materialSurface(uint material, float2 fine_position, float2 gradient_x,
    float2 gradient_y, float2 screen, float2 decoration, bool reuse_background, float3 unshifted_background,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> textures, SamplerState.Handle sampler) {
    TerrainVisual visual = g_terrain_visuals[min(material, 65535u)];
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
    if (g_grid_frame.m_has_background == 0u) { return decorateSurface(float4(color, opacity), decoration); }
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
    float3 background = reuse_background && all(offset_pixels == 0.0)
        ? unshifted_background : backdropColor(screen + offset_pixels, sampler);
    return decorateSurface(float4(lerp(background, color, opacity), 1.0), decoration);
}

[shader("fragment")]
float4 fragmentGridMain(
    GridVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 world_pixel_x = ddx(input.fine_position);
    float2 world_pixel_y = ddy(input.fine_position);
    float2 contour_position = input.fine_position;
    if ((g_grid_frame.m_surface_mode & 3u) == 3u) {
        contour_position += boundaryDisplacement(input.fine_position, world_pixel_x, world_pixel_y, g_sampler);
    }
    // Evaluate derivatives before coverage branches/discards so neighboring
    // helper invocations still provide a defined edge width.
    float2 pixel_x = ddx(contour_position);
    float2 pixel_y = ddy(contour_position);
    float width = clamp(length(fwidth(contour_position)), 1.0e-6, 1.0);
    // Beyond a world-sized footprint all admitted textures already select
    // their coarsest LOD. Bound gradients before period division to keep the
    // smallest supported positive period finite under extreme minification.
    float2 gradient_x = clamp(world_pixel_x * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    float2 gradient_y = clamp(world_pixel_y * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    uint material = input.material;
    float coverage = 1.0;
    float internal_outline = 0.0;
    uint diagnostic_mode = g_grid_frame.m_surface_mode >> 2u;
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
            material = contourMaterial(contour_position, pixel_x, pixel_y, width, stencil, coverage, sample, internal_outline);
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
        return diagnosticTint(g_diagnostic_colors[index].m_color_rgba, coverage);
    }
    float2 decoration = (input.flags & 1u) != 0u && (g_grid_frame.m_surface_mode & 1u) != 0u ?
        contourDecoration(contour_position, pixel_x, pixel_y, material, coverage < 1.0, stencil) : float2(0.0);
    decoration.x = max(decoration.x, internal_outline);
    // Shadow lives entirely on the non-solid side. Its coverage complements the
    // terrain AA and composites over matter before the final background blend.
    float shadow = 0.45 * decoration.y;
    matter = compositeMatter(float4(float3(0.015), shadow * (1.0 - coverage)),
        float4(matter.rgb, matter.a * (1.0 - shadow)));
    if (coverage == 0.0 || material == 0u || material >= 0xffffu) {
        if (matter.a > 0.0) { return matter; }
        discard;
    }
    if ((g_grid_frame.m_surface_mode & 2u) == 0u) {
        return compositeMatter(float4(decorateTerrain(gridTint(material), decoration), coverage), matter);
    }
    if (material == 0u || material >= 0xffffu) {
        discard;
    }
    TerrainVisual visual = g_terrain_visuals[min(material, 65535u)];
    if (visual.m_material_id != material) {
        discard;
    }
    float4 surface = materialSurface(material, input.fine_position, gradient_x, gradient_y,
        input.position.xy, decoration, false, float3(0.0), g_textures, g_sampler);
    return compositeMatter(float4(surface.rgb, surface.a * coverage), matter);
}

struct ParticleVertexOutput {
    float4 position : SV_Position;
    float2 fine_position : TEXCOORD0;
    nointerpolation uint material : TEXCOORD1;
    nointerpolation float fill : TEXCOORD2;
    nointerpolation uint2 owner : TEXCOORD3;
};

[shader("vertex")]
ParticleVertexOutput vertexParticleMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    static const float2 corners[6] = {float2(-0.5,-0.5), float2(0.5,-0.5), float2(0.5,0.5),
        float2(-0.5,-0.5), float2(0.5,0.5), float2(-0.5,0.5)};
    ParticleGpu particle = g_particles[instance];
    float2 owner_center = (float2(particle.m_owner) + 0.5) * g_grid_frame.m_cell_size_m;
    // Conservative source-volume support: translate with the continuous center
    // and expand to cover its unique owner region. Clipping cannot discard mass
    // when a restored center approaches an owner edge. Radius is physical 2D
    // sqrt(volume/thickness/pi); the cell proxy also covers contour-cut volumes.
    float2 half_extent = max(float2(particle.m_radius_m),
        0.5 * g_grid_frame.m_cell_size_m + abs(particle.m_position - owner_center));
    float2 world = particle.m_position + corners[vertex] * (2.0 * half_extent);
    ParticleVertexOutput output;
    output.position = mul(float4(world, 0.0, 1.0), g_grid_frame.m_view_projection);
    output.fine_position = world / g_grid_frame.m_cell_size_m;
    output.material = particle.m_material_id;
    output.fill = particle.m_fill;
    output.owner = particle.m_owner;
    return output;
}

[shader("fragment")]
float4 fragmentParticleMain(ParticleVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 world_pixel_x = ddx(input.fine_position);
    float2 world_pixel_y = ddy(input.fine_position);
    float2 contour = input.fine_position;
    if ((g_grid_frame.m_surface_mode & 3u) == 3u) {
        contour += boundaryDisplacement(input.fine_position, world_pixel_x, world_pixel_y, g_sampler);
    }
    float2 pixel_x = ddx(contour), pixel_y = ddy(contour);
    float width = clamp(length(fwidth(contour)), 1.0e-6, 1.0);
    float2 gradient_x = clamp(world_pixel_x * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    float2 gradient_y = clamp(world_pixel_y * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    // Half-open ownership eliminates double opacity at every world/chunk seam.
    // Expanded continuous support conserves each source record's owner volume.
    if (any(input.fine_position < float2(input.owner)) || any(input.fine_position >= float2(input.owner + 1u))) { discard; }
    float coverage = 0.0, outline = 0.0;
    int2 sample = int2(input.owner);
    TerrainStencil stencil = terrainStencil(contour);
    uint terrain = contourMaterial(contour, pixel_x, pixel_y, width, stencil, coverage, sample, outline);
    // The terrain draw already owns fully covered pixels. Do not shade hidden
    // particle material or reconstruct a terrain surface a second time here.
    if (coverage == 1.0) { discard; }
    float3 background = g_grid_frame.m_has_background != 0u ? backdropColor(input.position.xy, g_sampler) : float3(0.0);
    float4 particle = materialSurface(input.material, input.fine_position, gradient_x, gradient_y,
        input.position.xy, float2(0.0), true, background, g_textures, g_sampler);
    float fill = saturate(input.fill);
    if (g_grid_frame.m_has_background == 0u) {
        return float4(particle.rgb, particle.a * fill * (1.0 - coverage));
    }
    float3 color = lerp(background, particle.rgb, fill);
    float2 decoration = contourDecoration(contour, pixel_x, pixel_y, terrain, coverage < 1.0, stencil);
    color = lerp(color, float3(0.015), 0.45 * decoration.y);
    // Reconstruct the shared contour's complete pixel, avoiding a second
    // multiplication of terrain AA against particle AA at the same boundary.
    if (coverage > 0.0 && terrain > 0u && terrain < 0xffffu) {
        decoration.x = max(decoration.x, outline);
        float4 surface = materialSurface(terrain, input.fine_position, gradient_x, gradient_y,
            input.position.xy, decoration, true, background, g_textures, g_sampler);
        color = lerp(color, surface.rgb, coverage);
    }
    return float4(color, 1.0);
}
