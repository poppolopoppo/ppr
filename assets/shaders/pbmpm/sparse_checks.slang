// Test-only observations of the same canonical addressing/budget implementation.
#include "sparse_grid.slang"
struct SparseProbe { uint count, sourceA, generation, magnitude; };
ConstantBuffer<SparseProbe> g_probe;
StructuredBuffer<int2> g_probeNodes;
RWStructuredBuffer<uint> g_probeResults;

[shader("compute")][numthreads(64,1,1)]
void inspectSparseNodes(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_probe.count) return;
    int2 node=g_probeNodes[id.x];
    uint at=id.x*13u, key=0u, word=0u;
    bool valid=sparseFlatKey(node,key);
    bool found=valid && sparseWord(key,g_probe.sourceA!=0u,g_probe.generation,false,word);
    int4 source=valid ? sparseSource(key,g_probe.sourceA!=0u,g_probe.generation) : int4(0);
    g_probeResults[at]=valid ? 1u : 0u;
    g_probeResults[at+1u]=valid ? key : 0xffffffffu;
    g_probeResults[at+2u]=valid ? sparsePageKey(key) : 0xffffffffu;
    g_probeResults[at+3u]=valid ? sparseLane(key) : 0xffffffffu;
    g_probeResults[at+4u]=found ? word : 0xffffffffu;
    g_probeResults[at+5u]=asuint(source.x);
    g_probeResults[at+6u]=found ? g_sparseBudget[word] : 0u;
    g_probeResults[at+7u]=asuint(node.x);
    g_probeResults[at+8u]=asuint(node.y);
    g_probeResults[at+9u]=found ? 1u : 0u;
    g_probeResults[at+10u]=asuint(source.y);
    g_probeResults[at+11u]=asuint(source.z);
    g_probeResults[at+12u]=asuint(source.w);
}

// Test-only dense logical values seed represented pages; expected values remain
// independently enumerated CPU flat rows rather than a second sparse solver.
StructuredBuffer<uint> g_denseSource;

[shader("compute")][numthreads(64,1,1)]
void seedSparseFromDense(uint3 id:SV_DispatchThreadID) {
    uint key=id.x, word;
    if (key>=g_sparse.width*g_sparse.height) return;
    if (!sparseWord(key,false,g_sparse.generation,false,word)) return;

    for (uint field=0u;field<4u;++field) g_payloadB[word+field]=asint(g_denseSource[key*4u+field]);
}

[shader("compute")][numthreads(1,1,1)]
void reserveSparseAcrossGroups(uint3 group:SV_GroupID) {
    if (group.x>=g_probe.count) return;
    uint key, word;
    bool valid=sparseFlatKey(g_probeNodes[group.x],key);
    if (!valid || !sparseWord(key,g_probe.sourceA!=0u,g_probe.generation,true,word)) return;
    bool accepted=reserveSparseBudget(word,key,g_probe.magnitude,group.x,0u);
    if (accepted) {
        if (g_probe.sourceA!=0u) InterlockedAdd(g_payloadA[word],int(g_probe.magnitude));
        else InterlockedAdd(g_payloadB[word],int(g_probe.magnitude));
    }
    g_probeResults[group.x*13u]=word;
    g_probeResults[group.x*13u+1u]=accepted ? 1u : 0u;
}
