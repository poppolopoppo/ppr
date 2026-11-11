// Test-only sample writer: one deterministic colour per source pixel/sample.
struct ProbeParameters {
    int2 m_extent;
    uint m_mode;
    uint m_reserved;
    float4 m_base;
};
ConstantBuffer<ProbeParameters> g_probe;
Texture2DMS<float4, 4> g_scene;
RWTexture2D<float4> g_dump;

struct ProbeOutput {
    float4 m_position : SV_Position;
    float2 m_uv : TEXCOORD0;
};

[shader("vertex")]
ProbeOutput vertexReconstructionProbe(uint id : SV_VertexID) {
    float2 uv = float2((id << 1u) & 2u, id & 2u);
    ProbeOutput output;
    output.m_position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.m_uv = uv;
    return output;
}

[shader("fragment")]
float4 fragmentReconstructionProbe(ProbeOutput input, uint sample : SV_SampleIndex) : SV_Target {
    float2 pixel = EvaluateAttributeAtSample(input.m_uv, sample) * float2(g_probe.m_extent);
    int2 source = int2(floor(pixel));
    if (g_probe.m_mode == 0u) {
        uint r = uint(source.x * 13 + source.y * 7 + int(sample) * 3) & 15u;
        uint g = uint(source.x * 5 + source.y * 11 + int(sample) * 7 + 3) & 15u;
        uint b = uint(source.x * 3 + source.y * 2 + int(sample) * 13 + 7) & 15u;
        float tint = g_probe.m_base.w;
        return float4(float(r) / 16.0 * tint, float(g) / 16.0 * tint, float(b) / 16.0 * tint, 1.0);
    }
    return float4(g_probe.m_base.rgb, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dumpReconstructionProbe(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(g_probe.m_extent))) { return; }
    for (uint sample = 0u; sample < 4u; ++sample) {
        g_dump[uint2(id.x * 4u + sample, id.y)] = g_scene.Load(int2(id.xy), sample);
    }
}
