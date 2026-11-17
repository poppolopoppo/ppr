// Test-only corrupted-table probe of the production guard and witness sink.
#include "solver.slang"

// ------------------------------------------------------------------
// device property admission
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(1, 1, 1)]
void inspectPropertyAdmission() {
    particlePropertiesUsable(0u);
}

// ------------------------------------------------------------------
// actual calibration-space return, independent of transfer quantization
// ------------------------------------------------------------------

StructuredBuffer<Particle> g_law_input;
RWStructuredBuffer<Particle> g_law_output;

[shader("compute")]
[numthreads(1, 1, 1)]
void inspectCalibratedReturns() {
    uint count, stride;
    g_law_input.GetDimensions(count, stride);
    for (uint index = 0u; index < count; ++index) {
        Particle p = g_law_input[index];
        float2 sigma = float2(p.deformationGradient.c0x, p.deformationGradient.c1y);
        float2 frozenSigma = sigma;
        float frozenC = p.compaction;
        float frozenPhi = p.solidFraction;
        uint frozenRejection = calibratedReturn(frozenSigma, frozenC, frozenPhi, p.logJp, g_properties[index], false);
        p.deformationDisplacement = diagonal(frozenSigma);
        // Display-only witness in this isolated probe's readback buffer. These
        // rows are never committed to the solver; production uses the error field
        // through rejectCalibrated and preserves the liquid-density state word.
        p.liquidDensity = float(frozenRejection);

        bool accepted = calibratedReturn(sigma, p.compaction, p.solidFraction, p.logJp, g_properties[index]) == 0u;
        p.deformationGradient = diagonal(sigma);
        p.enabled = accepted ? 1.0 : 0.0;
        g_law_output[index] = p;
    }
}
