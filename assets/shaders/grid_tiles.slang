// Grid tile pass: flat tinted or per-cell quads, one instance per visible chunk-sized
// tile. Row-major, row-vector convention: transforms call
// mul(vector, matrix), never mul(matrix, vector). View-projection remains
// view * projection with [0,1] depth, no transpose, no Y flip. No lighting,
// world-space catalog texturing, no GI inputs of any kind. Catalog sessions use the shared
// piecewise-linear contour surface; explicit legacy fixtures keep square cells.
struct GridFrame {
    // Unjittered view * projection, followed by the per-slot visibility mode.
    float4x4 m_view_projection;
    uint m_use_visible_ids;
    float m_cell_size_m;
    uint m_surface_mode;
    uint m_boundary_material_id;
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
// fails program creation. Per-draw ordinary values must therefore arrive as
// entry-point `uniform` parameters, as g_tile_base does below.
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
// CPU GridTerrainVisual: 16 bytes, offsets 0/4/8/12. Texture index here is
// the resident descriptor slot, resolved from the catalog's source index.
struct TerrainVisual {
    uint m_material_id;
    uint m_color_rgba;
    uint m_texture_index;
    float m_texture_period_m;
};
StructuredBuffer<TerrainVisual> g_terrain_visuals;

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

// g_tile_base is an entry-point `uniform` (not a global) because
// global-scope ordinary data makes D3D12SerializeRootSignature fail with an
// overlapping globalParams/g_grid_frame CBV. C++ binds it by the flat
// cursor["g_tile_base"].setData(&base, sizeof(base)); it is plain data, so
// it needs no getDereferenced (unlike g_grid_frame).
//
// The direct instanced lane starts at instance 0 and binds this 4-byte base
// to the first payload. The indirect lane supplies startInstanceLocation in
// each argument; its SV_InstanceID behavior still needs GPU parity tests on
// every backend, not an assumption from the argument values.
[shader("vertex")]
GridVertexOutput vertexGridMain(
    uint sv_vertex_id : SV_VertexID,
    uint sv_instance_id : SV_InstanceID,
    uniform uint g_tile_base) {
    uint tile_id = (g_grid_frame.m_use_visible_ids != 0u) ? g_visible_ids[sv_instance_id] : sv_instance_id;
    GridTilePayload tile = g_tiles[g_tile_base + tile_id];
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

uint contourMaterial(float2 position, float width, out float coverage) {
    int2 base = int2(floor(position - 0.5));
    float2 point = position - (float2(base) + 0.5);
    uint materials[4];
    uint mask = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        materials[corner] = terrainSample(base + int2(g_contour_points[corner]));
        if (materials[corner] != 0u && materials[corner] != 0xffffu) {
            mask |= 1u << corner;
        }
    }
    ContourCase shape = g_contour_cases[mask];
    uint component = 0u;
    float best_triangle = -1.0e30;
    for (uint index = 0u; index < shape.m_triangle_count; ++index) {
        ContourTriangle triangle = shape.m_triangles[index];
        float2 a = g_contour_points[triangle.m_a];
        float2 b = g_contour_points[triangle.m_b];
        float2 c = g_contour_points[triangle.m_c];
        float signed_distance = min(
            contourCross(b - a, point - a) / length(b - a),
            min(contourCross(c - b, point - b) / length(c - b),
                contourCross(a - c, point - c) / length(a - c)));
        if (signed_distance > best_triangle) {
            best_triangle = signed_distance;
            component = triangle.m_solid_corners;
        }
    }
    if (component == 0u) {
        discard;
    }
    // AA uses only actual contour segments, never the internal fan edges.
    // Coverage is the union of occupied components, independent of material.
    float edge_distance = 1.0e30;
    for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
        uint2 segment = shape.m_segments[edge];
        float2 a = g_contour_points[segment.x];
        float2 delta = g_contour_points[segment.y] - a;
        float t = saturate(dot(point - a, delta) / dot(delta, delta));
        edge_distance = min(edge_distance, length(point - (a + t * delta)));
    }
    coverage = shape.m_segment_count == 0u ? 1.0 :
        saturate(0.5 + (best_triangle >= 0.0 ? edge_distance : -edge_distance) / width);
    if (coverage == 0.0) {
        discard;
    }
    float closest = 1.0e30;
    int2 winner = int2(4096);
    uint material = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        if ((component & (1u << corner)) == 0u) {
            continue;
        }
        float2 delta = point - g_contour_points[corner];
        float distance = dot(delta, delta);
        int2 coordinate = base + int2(g_contour_points[corner]);
        bool before = coordinate.y < winner.y ||
            (coordinate.y == winner.y && coordinate.x < winner.x);
        if (distance < closest || (distance == closest && before)) {
            closest = distance;
            winner = coordinate;
            material = materials[corner];
        }
    }
    return material;
}

[shader("fragment")]
float4 fragmentGridMain(
    GridVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    // Evaluate derivatives before coverage branches/discards so neighboring
    // helper invocations still provide a defined edge width.
    float width = max(length(fwidth(input.fine_position)), 1.0e-6);
    // Beyond a world-sized footprint all admitted textures already select
    // their coarsest LOD. Bound gradients before period division to keep the
    // smallest supported positive period finite under extreme minification.
    float2 gradient_x = clamp(ddx(input.fine_position) * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    float2 gradient_y = clamp(ddy(input.fine_position) * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    uint material = input.material;
    float coverage = 1.0;
    if ((input.flags & 1u) != 0u) {
        if ((g_grid_frame.m_surface_mode & 1u) != 0u) {
            material = contourMaterial(input.fine_position, width, coverage);
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
    if ((g_grid_frame.m_surface_mode & 2u) == 0u) {
        return float4(gridTint(material), coverage);
    }
    if (material == 0u || material >= 0xffffu) {
        discard;
    }
    TerrainVisual visual = g_terrain_visuals[min(material, 65535u)];
    if (visual.m_material_id != material) {
        discard;
    }
    uint rgba = visual.m_color_rgba;
    float4 tint = float4((rgba >> 24u) & 255u, (rgba >> 16u) & 255u,
        (rgba >> 8u) & 255u, rgba & 255u) / 255.0;
    // Authored colors are sRGB; color texture views perform their own decode.
    tint.rgb = select(tint.rgb <= 0.04045, tint.rgb / 12.92,
        pow((tint.rgb + 0.055) / 1.055, float3(2.4)));
    float period = visual.m_texture_period_m;
    float2 uv = input.fine_position * (g_grid_frame.m_cell_size_m / period);
    float4 albedo = nonuniform(g_textures[visual.m_texture_index]).SampleGrad(
        g_sampler, uv, gradient_x / period, gradient_y / period);
    return float4(tint.rgb * albedo.rgb, tint.a * albedo.a * coverage);
}
