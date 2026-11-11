// Test-only thermal lookup probe. The production helper and table binding are
// in ../thermal_emission.slang; this file supplies one entry point so the real
// GPU sampling path can be compared against the binary64 reference.

#include "../thermal_emission.slang"
#include "../world_lighting_codec.slang"

struct ThermalProbeParameters {
    uint m_count;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};

ConstantBuffer<ThermalProbeParameters> g_thermal_probe;
StructuredBuffer<uint> g_thermal_half_bits;
RWStructuredBuffer<float4> g_thermal_output;

[shader("compute")]
[numthreads(64, 1, 1)]
void sampleThermal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_thermal_probe.m_count) return;

    float kelvin = unpackHalf(g_thermal_half_bits[id.x] & 0xffffu);
    g_thermal_output[id.x] = float4(thermalRGB(kelvin), 0.0);
}
