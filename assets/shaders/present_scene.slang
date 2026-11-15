// ------------------------------------------------------------------
// fixed-pixelization nearest presentation with display-referred CAS
// ------------------------------------------------------------------
// Final fixed-P enlargement: each window pixel loads the exact integer resolved
// pixel at floor(pixel/P); the R*P-W right/bottom excess is simply cropped by
// not sampling it. The contrast-adaptive sharpen runs on resolved texels: its
// cross taps are the four resolved neighbours one texel apart, so the nearest
// enlargement replicates each sharpened texel to its whole P x P window block.
// Sharpening in window space instead would make every tap coincide with the
// centre for P > 1 and degenerate to the identity. The sharpen runs on the
// display-linear post-AgX values, before the single sRGB transfer (only when the
// target asks for one). UI is composed natively after this draw.

struct PresentSceneParameters {
    uint m_resolved_width;
    uint m_resolved_height;
    uint m_pixel_scale;
    uint m_encode_srgb;
    float m_sharpen_strength;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};
ConstantBuffer<PresentSceneParameters> g_present;

Texture2D<float4> g_output;

// Mirrors PostProcessPass's sharpen constants. kSharpenPeakMin/Max are the
// cross-tap coefficient magnitude at the low/high end of the strength ramp;
// the contrast reciprocal's floor and ceiling keep the kernel well conditioned.
static const float kSharpenPeakMin = 0.125;
static const float kSharpenPeakMax = 0.25;
static const float kSharpenContrastEps = 1.0e-3;
static const float kSharpenMaxWeight = 2.0;

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

// One resolved texel, clamped to the content extent so the picture border
// replicates instead of wrapping.
float3 resolvedTexel(int2 resolved_pixel, int2 extent_last) {
    return g_output.Load(int3(clamp(resolved_pixel, int2(0, 0), extent_last), 0)).rgb;
}

// AMD FidelityFX-style CAS: the four resolved cross taps fix the local contrast
// weight 1/sqrt(min + max + eps); the negative cross coefficient grows from 1/8 to
// 1/4 with strength while the centre keeps 1 - 4 * coefficient, so the five taps
// sum to one and a flat patch is reproduced up to a rounding step.
float3 contrastAdaptiveSharpen(
    float3 center, float3 left, float3 right, float3 up, float3 down, float strength) {
    float3 mn = min(min(left, right), min(up, down));
    float3 mx = max(max(left, right), max(up, down));
    float3 weight = min(rsqrt(mn + mx + kSharpenContrastEps), float3(kSharpenMaxWeight));
    float peak = strength * (kSharpenPeakMin + (kSharpenPeakMax - kSharpenPeakMin) * strength);
    float3 cross = -peak * weight;
    return saturate(center * (1.0 - 4.0 * cross) + cross * (left + right + up + down));
}

float3 presentLinearToSrgb(float3 linear_color) {
    return select(linear_color <= 0.0031308, linear_color * 12.92,
        1.055 * pow(linear_color, float3(1.0 / 2.4)) - 0.055);
}

[shader("fragment")]
float4 fragmentPresentScene(PresentSceneVertexOutput input) : SV_Target {
    uint2 extent = max(uint2(g_present.m_resolved_width, g_present.m_resolved_height), uint2(1u, 1u));
    uint scale = max(g_present.m_pixel_scale, 1u);
    int2 extent_last = int2(extent) - int2(1, 1);

    // The resolved texel this window pixel enlarges; the crop clamp drops the
    // R*P-W right/bottom excess.
    int2 resolved = clamp(int2(input.position.xy) / int2(int(scale), int(scale)), int2(0, 0), extent_last);
    float3 display_color = resolvedTexel(resolved, extent_last);

    // A zero strength (disabled or slider at zero) is the exact unsharpened
    // presentation: no cross tap is read.
    if (g_present.m_sharpen_strength > 0.0) {
        display_color = contrastAdaptiveSharpen(
            display_color,
            resolvedTexel(resolved + int2(-1, 0), extent_last),
            resolvedTexel(resolved + int2(1, 0), extent_last),
            resolvedTexel(resolved + int2(0, -1), extent_last),
            resolvedTexel(resolved + int2(0, 1), extent_last),
            g_present.m_sharpen_strength);
    }

    // An sRGB attachment encodes in hardware; an SDR UNorm attachment asks the
    // final shader for the single sRGB encoding. Float targets stay linear.
    if (g_present.m_encode_srgb != 0u) {
        display_color = presentLinearToSrgb(display_color);
    }

    return float4(display_color, 1.0);
}
