// ------------------------------------------------------------------
// resolved-size post-processing: bloom combine and attributed AgX
// ------------------------------------------------------------------
// Runs at the resolved content extent after world reconstruction. It consumes
// the immutable resolved scene view and the pass-owned bloom chain, then writes
// display-linear RGBA16F. Exposure and AgX are applied exactly once here; the
// scene/history values stay unexposed, and presentation never repeats them.

#include "display_agx.slang"

struct PostProcessParameters {
    uint m_width;
    uint m_height;
    float m_bloom_intensity;
    float m_manual_ev;
    uint m_bloom_enabled;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};
ConstantBuffer<PostProcessParameters> g_post;

Texture2D<float4> g_scene;
Texture2D<float4> g_bloom;
// 1x1 adapted EV offset around the manual exposure. Manual mode binds a zero
// offset texture, so the total exposure remains exp2(manual EV).
Texture2D<float4> g_exposure;
SamplerState.Handle g_linear;

struct PostProcessVertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
PostProcessVertexOutput vertexPostProcess(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    PostProcessVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentPostProcess(PostProcessVertexOutput input) : SV_Target {
    uint2 extent = max(uint2(g_post.m_width, g_post.m_height), uint2(1u, 1u));
    uint2 pixel = min(uint2(input.position.xy), extent - 1u);
    float2 uv = (float2(pixel) + 0.5) / float2(extent);

    float3 combined = g_scene.Load(int3(pixel, 0)).rgb;
    if (g_post.m_bloom_enabled != 0u) {
        // One bilinear bloom-0 tap at the resolved picture resolution. The FP32
        // combination preserves signed intermediates until the display boundary.
        combined += g_post.m_bloom_intensity * g_bloom.SampleLevel(g_linear, uv, 0.0).rgb;
    }

    float adapted = g_exposure.Load(int3(0, 0, 0)).x;
    float exposure = exp2(g_post.m_manual_ev + adapted);
    return float4(displayAgxPunchy(combined, exposure), 1.0);
}
