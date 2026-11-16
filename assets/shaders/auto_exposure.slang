// ------------------------------------------------------------------
// eye-adaptation state (1x1 offset around manual exposure)
// ------------------------------------------------------------------
// One thread reads the previous adapted EV offset and the histogram's target EV,
// applies an exponential approach toward the target, clamps the result to the
// manual-exposure window, and writes the new offset to a 1x1 texture. A disabled
// override writes the zero offset (fixed exposure) so the total exposure stays
// exp2(manual EV). Adaptation is independent of reconstruction history: settings
// edits and resizes never reset it, while first use and world replacement start
// from zero.

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

void writeAdaptedExposure(float target) {
    // PostProcessPass always runs this entry: manual exposure uses the disabled
    // branch and writes a deterministic zero offset, which is also the value a
    // later auto-exposure re-enable starts from.
    if (g_auto_exposure.enabled == 0u) {
        g_exposure[uint2(0, 0)] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float previous = g_previous_exposure.Load(int3(0, 0, 0)).x;
    float next = previous + (target - previous) * g_auto_exposure.adaptationAlpha;
    next = clamp(next, g_auto_exposure.evMin, g_auto_exposure.evMax);

    g_exposure[uint2(0, 0)] = float4(next, 0.0, 0.0, 1.0);
}

[numthreads(1, 1, 1)]
void adaptExposure(uint3 id : SV_DispatchThreadID) {
    if (g_auto_exposure.enabled == 0u) {
        writeAdaptedExposure(0.0);
        return;
    }

    writeAdaptedExposure(g_target_ev[0]);
}
