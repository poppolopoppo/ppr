// Preserved test-only diagnostic entry using the actual production terrain query.
// Not invoked by ordinary leaves. Source/probe/manifest and native explanation:
// .slim/deepwork/unified-matter-v2/gpu-integration1-first-terrain-failure/.
#include "solver.slang"

RWStructuredBuffer<float> g_probe;

// ------------------------------------------------------------------
// new query/arithmetic evaluations, not original dispatch intermediates
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(1, 1, 1)]
void inspectTerrainContact(uint3 id : SV_DispatchThreadID) {
    Contact contact = terrainContact(float2(12.5, 12.5), 0xffffffffu, 0u);
    float gravity = -float(g_parameters.gridHeight) * g_parameters.gravityStrength *
        g_parameters.deltaTime * g_parameters.deltaTime;
    float2 correction = contact.penetration * contact.normal;
    float2 d = float2(0.0);
    d.y -= float(g_parameters.gridHeight) * g_parameters.gravityStrength *
        g_parameters.deltaTime * g_parameters.deltaTime;
    if (contact.collides) d -= contact.penetration * contact.normal;

    g_probe[0] = contact.collides ? 1.0 : 0.0;
    g_probe[1] = contact.penetration;
    g_probe[2] = contact.normal.x;
    g_probe[3] = contact.normal.y;
    g_probe[4] = contact.point.x;
    g_probe[5] = contact.point.y;
    g_probe[6] = gravity;
    g_probe[7] = correction.x;
    g_probe[8] = correction.y;
    g_probe[9] = d.x;
    g_probe[10] = d.y;
    g_probe[11] = gravity + 7.5;
}
