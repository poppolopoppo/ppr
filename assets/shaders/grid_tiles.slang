// Grid tile pass: flat tinted or per-cell quads, one instance per visible chunk-sized
// tile. Row-major, row-vector convention: transforms call
// mul(vector, matrix), never mul(matrix, vector). View-projection remains
// view * projection with [0,1] depth, no transpose, no Y flip. No lighting,
// no texturing, no GI inputs of any kind.
struct GridFrame {
    // Only m_view_projection is read by this shader. It is the unjittered
    // view * projection uploaded by uploadFrameConstants_; jitter must never
    // enter the frame upload or the pipeline key.
    float4x4 m_view_projection;
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
// Chunk n occupies 8192 words at n*8192; low halfword = even cell,
// high halfword = odd cell. Only read when payload flag bit 0 is set.
StructuredBuffer<uint> g_cell_materials;

struct GridVertexOutput {
    float4 position : SV_Position;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint cell_word_base : TEXCOORD1;
    nointerpolation uint flags : TEXCOORD2;
    float2 local_cell : TEXCOORD0;
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
    GridTilePayload tile = g_tiles[g_tile_base + sv_instance_id];
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
    float4 world = float4(corner, 0.0, 1.0);
    output.position = mul(world, g_grid_frame.m_view_projection);
    output.material = tile.m_material;
    output.cell_word_base = tile.m_cell_word_base;
    output.flags = tile.m_flags;
    output.local_cell = corner - min_xy;
    return output;
}

[shader("fragment")]
float4 fragmentGridMain(GridVertexOutput input) : SV_Target {
    uint material = input.material;
    if ((input.flags & 1u) != 0u) {
        // Rasterized quad edges may land exactly at 128 after interpolation;
        // clamp them into the final cell rather than reading the next chunk.
        uint2 cell = uint2(clamp(input.local_cell, float2(0.0), float2(127.999)));
        uint index = cell.y * 128u + cell.x;
        uint packed = g_cell_materials[input.cell_word_base + index / 2u];
        material = ((index & 1u) != 0u) ? (packed >> 16u) : (packed & 0xffffu);
        if (material == 0xffffu) {
            discard;
        }
    }
    return float4(gridTint(material), 1.0);
}
