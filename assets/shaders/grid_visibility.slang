// ABI mirrors GridTilePayload and the pass-local VisibilityFrame.
struct Tile {
    float4 rect;
    uint material;
    uint cell_word_base;
    uint flags;
    uint pad;
};
struct VisibilityFrame {
    float4 planes[6];
    uint count;
    float cell_size_m;
    uint2 padding;
};
ConstantBuffer<VisibilityFrame> g_visibility;
StructuredBuffer<Tile> g_tiles;
RWStructuredBuffer<uint> g_visible_ids;
RWStructuredBuffer<uint4> g_args;

[shader("compute")]
[numthreads(1, 1, 1)]
void resetVisibility() {
    // One fixed non-indexed draw: vertex count, instance count, first vertex,
    // first instance. Reset even when no tiles are resident.
    g_args[0] = uint4(6u, 0u, 0u, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cullVisibility(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_visibility.count) return;
    float4 rect = g_tiles[id.x].rect;
    float2 centre = (rect.xy + rect.zw) * 0.5 * g_visibility.cell_size_m;
    float half_extent = (max(max((rect.z - rect.x) * 0.5, (rect.w - rect.y) * 0.5), 0.5) + 0.5) * g_visibility.cell_size_m;
    // Match the CPU reference's conservative cubic box and exact planes.
    for (uint plane = 0u; plane < 6u; ++plane) {
        float4 p = g_visibility.planes[plane];
        float3 corner = float3(centre, 0.0) + float3(
            p.x > 0.0 ? half_extent : -half_extent,
            p.y > 0.0 ? half_extent : -half_extent,
            p.z > 0.0 ? half_extent : -half_extent);
        if (dot(float4(corner, 1.0), p) < 0.0) return;
    }
    uint destination;
    InterlockedAdd(g_args[0].y, 1u, destination);
    g_visible_ids[destination] = id.x;
}
