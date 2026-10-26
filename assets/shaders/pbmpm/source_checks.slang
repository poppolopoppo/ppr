// Focused executable checks of the actual port helpers; not a second solver.
#include "types.slang"
#include "fork_core.slang"

void fail(uint flag, uint row, uint field) { InterlockedOr(g_errors[0],flag); }

void failNode(uint node, uint field) { fail(2u,0xffffffffu,field); }

RWStructuredBuffer<float> g_probe;

// ------------------------------------------------------------------
// publication validation probes
// ------------------------------------------------------------------

StructuredBuffer<Particle> g_validation_rows;
RWStructuredBuffer<uint> g_validation_fields;
ConstantBuffer<Parameters> g_validation_parameters;

[shader("compute")]
[numthreads(64, 1, 1)]
void inspectParticleValidation(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_validation_parameters.particleCount) return;

    g_validation_fields[id.x] = invalidParticleField(g_validation_rows[id.x], g_validation_parameters);
}

// ------------------------------------------------------------------
// captured dispatch parameters
// ------------------------------------------------------------------

// Dispatch-time ordinary-data capture, using one reused root shader object.
[shader("compute")]
[numthreads(1, 1, 1)]
void captureParameters(uniform uint g_slot, uniform float g_value) {
    g_probe[g_slot] = g_value;
}

// ------------------------------------------------------------------
// source column algebra and contact microgates
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(1, 1, 1)]
void inspectSource(uint3 id : SV_DispatchThreadID) {
    SourceMatrix A = matrix(1.0, 2.0, 3.0, 4.0);
    float2 transformed = transform(A, float2(5.0, 6.0));
    SourceMatrix P = product(A, matrix(2.0, -1.0, 0.5, 3.0));
    g_probe[0] = transformed.x;
    g_probe[1] = transformed.y;
    g_probe[2] = P.c0x;
    g_probe[3] = P.c0y;
    g_probe[4] = P.c1x;
    g_probe[5] = P.c1y;

    SvdResult decomposition = forkSvd(matrix(0.0, 2.0, -1.0, 0.0));
    SourceMatrix reconstructed = product(product(decomposition.U, diagonal(decomposition.Sigma)), decomposition.Vt);
    g_probe[6] = decomposition.Sigma.x;
    g_probe[7] = decomposition.Sigma.y;
    g_probe[8] = reconstructed.c0x;
    g_probe[9] = reconstructed.c0y;
    g_probe[10] = reconstructed.c1x;
    g_probe[11] = reconstructed.c1y;

    StaticShape box = {0.0, 0.0, 1.0, 1.0, 0.0, 0.0, ShapeTypeBox, 0u};
    Contact face = collide(box, float2(1.0, 0.0));
    Contact tie = collide(box, float2(0.5, 0.5));
    Contact center = collide(box, float2(0.0));
    g_probe[12] = face.collides ? 1.0 : 0.0;
    g_probe[13] = tie.normal.x;
    g_probe[14] = tie.normal.y;
    g_probe[15] = center.normal.x;
    g_probe[16] = center.normal.y;
    box.rotationDegrees = 90.0;
    Contact rotated = collide(box, float2(0.0, 0.5));
    g_probe[17] = rotated.normal.x;
    g_probe[18] = rotated.normal.y;

    StaticShape circle = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0, ShapeTypeCircle, 0u};
    Contact circleFace = collide(circle, float2(1.0, 0.0));
    Contact circleCenter = collide(circle, float2(0.0));
    g_probe[19] = circleFace.collides ? 1.0 : 0.0;
    g_probe[20] = circleFace.point.x;
    g_probe[21] = circleCenter.normal.x;
    g_probe[22] = circleCenter.normal.y;
}
