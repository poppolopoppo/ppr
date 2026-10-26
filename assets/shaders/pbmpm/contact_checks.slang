// Test-only inspection of the production grid-contact operation.
#include "solver.slang"

RWStructuredBuffer<int> g_contact_result;
// Explicitly test-only dense input to the actual production contact operation.
StructuredBuffer<int> g_gridSrc;

// ------------------------------------------------------------------
// analytical grid-contact probes
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(1,1,1)]
void inspectContact() {
    uint base=(7u*32u+6u)*4u;
    int4 source=int4(g_gridSrc[base],g_gridSrc[base+1],g_gridSrc[base+2],g_gridSrc[base+3]);
    float4 state=contactGridState(int2(6,7),base/4u,source);
    g_contact_result[0]=checkedEncodeNode(state.x,base/4u,0u);
    g_contact_result[1]=checkedEncodeNode(state.y,base/4u,1u);
    g_contact_result[2]=checkedEncodeNode(state.z,base/4u,2u);
    g_contact_result[3]=checkedEncodeNode(state.w,base/4u,3u);
}

// ------------------------------------------------------------------
// exact node conversion and unchanged scatter-conversion rejection
// ------------------------------------------------------------------
StructuredBuffer<uint> g_conversion_inputs;
RWStructuredBuffer<uint> g_conversion_results;

[shader("compute")]
[numthreads(1,1,1)]
void inspectNodeConversions(uniform uint g_count) {
    // One invocation isolates each case's ordinary production error sink.
    for (uint index=0u;index<g_count;++index) {
        for (uint word=0u;word<6u;++word) g_errors[word]=0u;

        float value=asfloat(g_conversion_inputs[index]);
        uint at=index*12u;
        g_conversion_results[at]=asuint(checkedEncodeNode(value,index,0u));
        for (uint word=0u;word<6u;++word) g_conversion_results[at+1u+word]=g_errors[word];

        for (uint word=0u;word<6u;++word) g_errors[word]=0u;

        g_conversion_results[at+7u]=asuint(checkedEncode(value,index,0u));
        g_conversion_results[at+8u]=g_errors[0];
        g_conversion_results[at+9u]=g_errors[1];
        g_conversion_results[at+10u]=g_errors[2];
        g_conversion_results[at+11u]=g_errors[4];
    }
}
