// ------------------------------------------------------------------
// fixed-pixelization nearest presentation
// ------------------------------------------------------------------
// Final fixed-P enlargement: each window pixel loads the exact integer resolved
// pixel at floor(pixel/P); the R*P-W right/bottom excess is simply cropped by
// not sampling it. UI is composed natively after this draw.

struct PresentSceneParameters {
    uint m_resolved_width;
    uint m_resolved_height;
    uint m_pixel_scale;
    uint m_encode_srgb;
};
ConstantBuffer<PresentSceneParameters> g_present;

Texture2D<float4> g_output;

struct PresentSceneVertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
PresentSceneVertexOutput vertexPresentScene(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    PresentSceneVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float3 presentLinearToSrgb(float3 linear_color) {
    return select(linear_color <= 0.0031308, linear_color * 12.92,
        1.055 * pow(linear_color, float3(1.0 / 2.4)) - 0.055);
}

[shader("fragment")]
float4 fragmentPresentScene(PresentSceneVertexOutput input) : SV_Target {
    uint2 extent = max(uint2(g_present.m_resolved_width, g_present.m_resolved_height), uint2(1u, 1u));
    uint scale = max(g_present.m_pixel_scale, 1u);
    uint2 pixel = min(uint2(input.position.xy) / scale, extent - 1u);
    float3 linear_color = g_output.Load(int3(pixel, 0)).rgb;
    // An sRGB attachment encodes in hardware; an SDR UNorm attachment asks the
    // final shader for the single sRGB encoding. Float targets stay linear.
    if (g_present.m_encode_srgb != 0u) {
        linear_color = presentLinearToSrgb(linear_color);
    }

    return float4(linear_color, 1.0);
}
