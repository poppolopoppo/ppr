// Reader for the WorldLightingPass::bind contract: the same resolved-cell and
// numeric-status names a material/inspection shader consumes.
struct BindProbe {
    uint cell;
    uint reserved[3];
};

ConstantBuffer<BindProbe> g_bind_probe;
StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<uint> g_numeric_status;
RWStructuredBuffer<uint> g_bind_out;

[shader("compute")]
[numthreads(256, 1, 1)]
void probeBindings(uint3 id: SV_DispatchThreadID) {
    if (id.x != 0u) return;

    g_bind_out[0] = g_resolved_cells[g_bind_probe.cell];
    g_bind_out[1] = g_numeric_status[0];
    g_bind_out[2] = g_numeric_status[1];
    g_bind_out[3] = g_numeric_status[2];
}
