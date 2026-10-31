// ------------------------------------------------------------------
// element-preserving compute smoothing for the 2D optical field
// ------------------------------------------------------------------

struct FluidFilterFrame {
    uint width, height;
    uint reserved0, reserved1;
};
ConstantBuffer<FluidFilterFrame> g_fluid_filter;
Texture2D<uint> g_fluid_material;
Texture2D<float> g_fluid_raw;
RWTexture2D<float> g_fluid_filtered;

groupshared float g_values[144];
groupshared uint g_materials[144];

[shader("compute")]
[numthreads(8,8,1)]
void filterLiquid(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {
    uint lane = local.y * 8u + local.x;
    int2 extent = int2(g_fluid_filter.width, g_fluid_filter.height);
    for (uint index = lane; index < 144u; index += 64u) {
        int2 pixel = clamp(int2(group.xy * 8u) + int2(index % 12u, index / 12u) - 2, int2(0), extent - 1);
        g_values[index] = g_fluid_raw.Load(int3(pixel, 0));
        g_materials[index] = g_fluid_material.Load(int3(pixel, 0));
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= uint2(extent))) return;

    uint center_index = (local.y + 2u) * 12u + local.x + 2u;
    uint material = g_materials[center_index];
    if (material == 0u) {
        g_fluid_filtered[id.xy] = 0.0;
        return;
    }

    float center = g_values[center_index];
    float total = 0.0;
    float weight = 0.0;
    // Small bounded 2D filter, kept non-separable to avoid diagonal edge bias.
    // Range restriction preserves droplets and sharp interfaces; ID guidance
    // prevents oil, water and other authored elements bleeding into one another.
    float range = max(0.15, center * 0.5);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            uint index = uint(int(center_index) + y * 12 + x);
            if (g_materials[index] != material) continue;
            float value = g_values[index];
            float difference = clamp(value, center - range, center + range) - center;
            float w = exp(-0.5 * float(x*x + y*y)) * exp(-difference*difference / (range*range));
            total += value * w;
            weight += w;
        }
    }
    g_fluid_filtered[id.xy] = total / max(weight, 1.0e-6);
}
