// Grid tile pass: flat tinted or per-cell quads, one instance per visible chunk-sized
// tile. Row-major, row-vector convention: transforms call
// mul(vector, matrix), never mul(matrix, vector). View-projection remains
// view * projection with [0,1] depth, no transpose, no Y flip. No dynamic lighting,
// world-space catalog texturing, no GI inputs of any kind. Catalog sessions use the shared
// rounded quadratic-Bezier contour surface; explicit legacy fixtures keep square cells.
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
// CPU GridTerrainVisual: 16 bytes, offsets 0/4/8/12. Texture index here is
// the resident descriptor slot, resolved from the catalog's source index.
struct TerrainVisual {
    uint m_material_id;
    uint m_color_rgba;
    uint m_texture_index;
    float m_texture_period_m;
};
StructuredBuffer<TerrainVisual> g_terrain_visuals;
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

// Inverse quadratic coordinates turn shared diagonal midpoint cuts into
// quadratic Bezier arcs with the dual-cell centre as control point. Crossings,
// connectivity and axis-aligned endpoint tangents agree across neighboring cells.
float2 contourWarp(float2 point) {
    float2 offset = point - 0.5;
    return 0.5 + 0.5 * sign(offset) * sqrt(abs(2.0 * offset));
}

float roundedSegmentDistance(float2 point, uint2 segment, out float2 nearest, out float2 normal) {
    float2 a = g_contour_points[segment.x];
    float2 b = g_contour_points[segment.y];
    float2 delta = b - a;
    float t = saturate(dot(point - a, delta) / dot(delta, delta));
    nearest = a + t * delta;
    normal = float2(-delta.y, delta.x);
    if (delta.x != 0.0 && delta.y != 0.0) {
        float2 control = float2(0.5);
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

uint componentMaterial(float2 point, uint materials[4], uint component, out uint masks[4]) {
    for (uint index = 0u; index < 4u; ++index) { masks[index] = 0u; }
    uint visited = 0u;
    uint dominant = 0u;
    uint largest = 0u;
    for (uint corner = 0u; corner < 4u; ++corner) {
        if ((component & (1u << corner)) == 0u || (visited & (1u << corner)) != 0u) {
            continue;
        }
        uint mask = 0u;
        for (uint other = 0u; other < 4u; ++other) {
            if ((component & (1u << other)) != 0u && materials[corner] == materials[other]) {
                mask |= 1u << other;
            }
        }
        if (mask == component) {
            return materials[corner];
        }
        masks[corner] = mask;
        visited |= mask;
        uint count = countbits(mask);
        float2 coordinate = g_contour_points[corner];
        float2 previous = g_contour_points[dominant];
        bool before = coordinate.y < previous.y ||
            (coordinate.y == previous.y && coordinate.x < previous.x);
        if (count > largest || (count == largest && before)) {
            largest = count;
            dominant = corner;
        }
    }
    // Minority labels use the shared marching-square polygons. The dominant
    // label fills their complement, including the centre at 3/4-label junctions.
    // Equal counts prefer the lower-y/lower-x sample, never the numeric ID.
    // In a fully occupied alternating saddle this connects the dominant label;
    // keeping both labels disconnected would leave an unpainted centre.
    masks[dominant] = 0u;
    uint material = materials[dominant];
    for (uint corner = 0u; corner < 4u; ++corner) {
        if (masks[corner] != 0u && contourContains(masks[corner], point)) {
            material = materials[corner];
        }
    }
    return material;
}

uint contourMaterial(float2 position, float width, out float coverage, out int2 winner) {
    winner = int2(4096);
    int2 base = int2(floor(position - 0.5));
    float2 point = position - (float2(base) + 0.5);
    float2 warped = contourWarp(point);
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
            contourCross(b - a, warped - a) / length(b - a),
            min(contourCross(c - b, warped - b) / length(c - b),
                contourCross(a - c, warped - c) / length(a - c)));
        if (signed_distance > best_triangle) {
            best_triangle = signed_distance;
            component = triangle.m_solid_corners;
        }
    }
    if (component == 0u) {
        coverage = 0.0;
        return 0u;
    }
    // AA uses only actual contour segments, never the internal fan edges.
    // Coverage is the union of occupied components, independent of material.
    float edge_distance = 1.0e30;
    for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
        float2 nearest, normal;
        edge_distance = min(edge_distance, roundedSegmentDistance(point, shape.m_segments[edge], nearest, normal));
    }
    coverage = shape.m_segment_count == 0u ? 1.0 :
        saturate(0.5 + (best_triangle >= 0.0 ? edge_distance : -edge_distance) / width);
    if (coverage == 0.0) {
        return 0u;
    }
    float closest = 1.0e30;
    uint masks[4];
    uint material = componentMaterial(warped, materials, component, masks);
    for (uint corner = 0u; corner < 4u; ++corner) {
        if ((component & (1u << corner)) == 0u || materials[corner] != material) {
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
        }
    }
    return material;
}

// Styles search only dual cells within their capped support, including adjacent
// chunks. This prevents shadows/outlines from stopping at a dual-cell boundary.
float2 contourDecoration(float2 position, float2 pixel_x, float2 pixel_y) {
    int2 base = int2(floor(position - 0.5));
    float2 local = position - (float2(base) + 0.5);
    float radius = min(6.5 * length(float2(length(pixel_x), length(pixel_y))), 1.0);
    float outline = 0.0;
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 point = local - float2(x, y);
            float2 box_distance = max(max(-point, point - 1.0), 0.0);
            if (length(box_distance) >= radius) { continue; }
            uint materials[4];
            uint occupied = 0u;
            for (uint corner = 0u; corner < 4u; ++corner) {
                materials[corner] = terrainSample(base + int2(x, y) + int2(g_contour_points[corner]));
                if (materials[corner] != 0u && materials[corner] != 0xffffu) { occupied |= 1u << corner; }
            }
            ContourCase shape = g_contour_cases[occupied];
            for (uint edge = 0u; edge < shape.m_segment_count; ++edge) {
                float2 nearest, normal;
                float distance = roundedSegmentDistance(point, shape.m_segments[edge], nearest, normal);
                float depth = min(6.0 * contourPixelSize(normal, pixel_x, pixel_y), 1.0);
                // The caller clips this to the non-solid coverage, including matter.
                shadow = max(shadow, 1.0 - smoothstep(0.0, depth, distance));
            }
            uint visited = 0u;
            for (uint triangle = 0u; triangle < shape.m_triangle_count; ++triangle) {
                uint component = shape.m_triangles[triangle].m_solid_corners;
                if ((visited & component) != 0u) { continue; }
                visited |= component;
                uint masks[4];
                componentMaterial(contourWarp(saturate(point)), materials, component, masks);
                for (uint corner = 0u; corner < 4u; ++corner) {
                    ContourCase material_shape = g_contour_cases[masks[corner]];
                    for (uint edge = 0u; edge < material_shape.m_segment_count; ++edge) {
                        float2 nearest, normal;
                        float distance = roundedSegmentDistance(point, material_shape.m_segments[edge], nearest, normal);
                        if (!contourContains(occupied, contourWarp(nearest))) { continue; }
                        float pixel = contourPixelSize(normal, pixel_x, pixel_y);
                        // Including its AA fringe, total outline support <= one cell.
                        float half_width = min(pixel, 1.0 / 3.0);
                        float aa = min(0.5 * pixel, 1.0 / 6.0);
                        outline = max(outline, 1.0 - smoothstep(half_width - aa, half_width + aa, distance));
                    }
                }
            }
        }
    }
    return float2(outline, shadow);
}

float3 decorateTerrain(float3 color, float2 decoration) {
    // An opaque outline core hides the discontinuous material switch; its
    // smooth one-pixel fringe blends into the independently sampled terrain.
    return lerp(color, float3(0.025), decoration.x);
}

// Terrain and initial matter cover complementary parts of one contour pixel.
float4 compositeMatter(float4 terrain, float4 matter) {
    float alpha = terrain.a + matter.a;
    return alpha > 0.0 ? float4((terrain.rgb * terrain.a + matter.rgb * matter.a) / alpha, alpha) : float4(0.0);
}

[shader("fragment")]
float4 fragmentGridMain(
    GridVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    // Evaluate derivatives before coverage branches/discards so neighboring
    // helper invocations still provide a defined edge width.
    float2 pixel_x = ddx(input.fine_position);
    float2 pixel_y = ddy(input.fine_position);
    float width = clamp(length(fwidth(input.fine_position)), 1.0e-6, 1.0);
    // Beyond a world-sized footprint all admitted textures already select
    // their coarsest LOD. Bound gradients before period division to keep the
    // smallest supported positive period finite under extreme minification.
    float2 gradient_x = clamp(ddx(input.fine_position) * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    float2 gradient_y = clamp(ddy(input.fine_position) * g_grid_frame.m_cell_size_m, -2048.0, 2048.0);
    uint material = input.material;
    float coverage = 1.0;
    uint diagnostic_mode = g_grid_frame.m_surface_mode >> 2u;
    int2 sample = int2(floor(input.fine_position));
    if (diagnostic_mode == 2u) {
        uint index = diagnosticIndex(sample);
        if (index == 0u) { discard; }
        return diagnosticTint(g_diagnostic_colors[index].m_color_rgba, 1.0);
    }
    if ((input.flags & 1u) != 0u) {
        if ((g_grid_frame.m_surface_mode & 1u) != 0u) {
            material = contourMaterial(input.fine_position, width, coverage, sample);
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
        contourDecoration(input.fine_position, pixel_x, pixel_y) : float2(0.0);
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
    return compositeMatter(float4(decorateTerrain(tint.rgb * albedo.rgb, decoration), tint.a * albedo.a * coverage), matter);
}
