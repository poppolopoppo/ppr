// ------------------------------------------------------------------
// temporal eye-adaptation state (1x1 offset around manual exposure)
// ------------------------------------------------------------------
// One thread reads the previous adapted EV offset and the histogram's target EV,
// applies an exponential approach toward the target, clamps the result to the
// manual-exposure window, and writes the new offset to a 1x1 texture. A disabled
// override writes the zero offset (fixed exposure) so the present multiply stays
// a no-op.

struct AutoExposureParameters {
    float adaptationAlpha; // 1 - exp(-dt / tau)
    float evMin;           // clamp lower bound (offset around manual exposure)
    float evMax;           // clamp upper bound
    uint enabled;          // 0 => fixed exposure (offset 0)
};
ConstantBuffer<AutoExposureParameters> g_auto_exposure;

RWStructuredBuffer<float> g_target_ev; // reduced target EV from the histogram
Texture2D<float4> g_previous_exposure; // previous adapted offset in .x
RWTexture2D<float4> g_exposure;        // 1x1 adapted offset in .x

[numthreads(1, 1, 1)]
void adaptExposure(uint3 id : SV_DispatchThreadID) {
    if (g_auto_exposure.enabled == 0u) {
        g_exposure[uint2(0, 0)] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float target = g_target_ev[0];
    float previous = g_previous_exposure.Load(int3(0, 0, 0)).x;
    float next = previous + (target - previous) * g_auto_exposure.adaptationAlpha;
    next = clamp(next, g_auto_exposure.evMin, g_auto_exposure.evMax);

    g_exposure[uint2(0, 0)] = float4(next, 0.0, 0.0, 1.0);
}
