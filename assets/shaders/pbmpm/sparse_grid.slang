#include "types.slang"
#define PBMPM_SPARSE_STORAGE 1
#include "fork_core.slang"

// Six-word transaction sink, identical to the production solver protocol.
void recordError(uint flag, uint node, uint field, uint row) {
    InterlockedOr(g_errors[0],flag);
    uint previous;
    InterlockedCompareExchange(g_errors[1],0u,1u,previous);
    if (previous==0u) {
        g_errors[2]=node; g_errors[3]=field;
        g_errors[4]=row; g_errors[5]=0xffffffffu;
    }
}
void fail(uint flag, uint row, uint field) { recordError(flag==2u ? 2u : 128u,0xffffffffu,field,row); }
void failNode(uint key, uint field) { recordError(2u,key,field,0xffffffffu); }
#include "sparse_address.slang"

bool sparseParticle(uint row, out Particle particle) {
    particle=loadParticle(row);
    uint field=invalidParticleField(particle,g_parameters);
    if (field!=0xffffffffu) {
        recordError(8u,0xffffffffu,field,row);
        return false;
    }
    return particle.enabled!=0;
}

uint bucketKey(Particle p) {
    // Admission guarantees finite nonnegative positions. Quantize the cell before
    // integer division so a rounded reciprocal cannot select the next bucket.
    uint2 bucket=uint2(float2(p.positionX,p.positionY))/6u;
    return bucket.y*g_sparse.bucketColumns+bucket.x;
}

[shader("compute")][numthreads(64,1,1)]
void discoverBuckets(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint row=sparseGroup(group)*64u+lane;
    if (row>=g_sparse.rows) return;
    Particle particle;
    if (!sparseParticle(row,particle)) return;

    uint key=bucketKey(particle), previous;
    InterlockedExchange(g_bucketIndex[key].generation,g_sparse.generation,previous);
    if (previous==g_sparse.generation) return;

    uint slot;
    InterlockedAdd(g_sparseCounters[0],1u,slot);
    if (slot>=g_sparse.bucketCapacity) {
        recordError(128u,0xffffffffu,0u,row);
        return;
    }
    // No consumer reads this publication in this dispatch.
    g_activeBuckets[slot].key=key;
    g_bucketIndex[key].slot=slot;
}

[shader("compute")][numthreads(1,1,1)]
void finalizeSparseArguments() {
    uint counts[3]={g_sparseCounters[0],g_sparseCounters[1],g_sparseCounters[2]};
    bool valid=g_errors[0]==0u && counts[0]<=g_sparse.bucketCapacity &&
        counts[1]<=g_sparse.pageCapacity && counts[2]<=g_sparse.groupCapacity &&
        g_sparseCounters[3]<=g_sparse.rows;
    if (!valid && g_errors[0]==0u) recordError(128u,0xffffffffu,0xffffffffu,0xffffffffu);
    for (uint i=0u;i<3u;++i) {
        uint count=valid ? counts[i] : 0u;
        g_sparseArgs[i*4u]=min(count,g_sparse.dispatchWidth);
        g_sparseArgs[i*4u+1u]=count==0u ? 1u : (count-1u)/g_sparse.dispatchWidth+1u;
        g_sparseArgs[i*4u+2u]=1u;
        g_sparseArgs[i*4u+3u]=0u;
    }
}

[shader("compute")][numthreads(64,1,1)]
void initializeBucketsAndDiscoverPages(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint slot=sparseGroup(group);
    if (slot>=g_sparseCounters[0] || g_errors[0]!=0u) return;
    uint key=g_activeBuckets[slot].key;
    if (lane==0u) {
        g_activeBuckets[slot].count=0u;
        g_activeBuckets[slot].rowStart=0u;
        g_activeBuckets[slot].inserted=0u;
    }

    int2 node=int2(key%g_sparse.bucketColumns,key/g_sparse.bucketColumns)*6-1+int2(lane%8u,lane/8u);
    uint flat;
    if (!sparseFlatKey(node,flat)) return;
    uint page=sparsePageKey(flat), previous;
    InterlockedExchange(g_pageIndexB[page].generation,g_sparse.generation,previous);
    if (previous==g_sparse.generation) return;

    uint pageSlot;
    InterlockedAdd(g_sparseCounters[1],1u,pageSlot);
    if (pageSlot>=g_sparse.pageCapacity) {
        recordError(128u,flat,0u,0xffffffffu);
        return;
    }
    g_pageKeys[pageSlot]=page;
    g_pageIndexB[page].slot=pageSlot;
}

[shader("compute")][numthreads(64,1,1)]
void countSparseParticles(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint row=sparseGroup(group)*64u+lane;
    if (row>=g_sparse.rows || g_errors[0]!=0u) return;
    Particle particle;
    if (!sparseParticle(row,particle)) return;
    SparseIndex entry=g_bucketIndex[bucketKey(particle)];
    if (entry.generation!=g_sparse.generation || entry.slot>=g_sparse.bucketCapacity) {
        recordError(128u,0xffffffffu,1u,row);
        return;
    }
    InterlockedAdd(g_activeBuckets[entry.slot].count,1u);
}

[shader("compute")][numthreads(1,1,1)]
void allocateSparseGroups(uint3 group:SV_GroupID) {
    uint slot=sparseGroup(group);
    if (slot>=g_sparseCounters[0] || g_errors[0]!=0u) return;
    SparseBucket active=g_activeBuckets[slot];
    uint groups=(active.count+63u)/64u, first, rowStart;
    InterlockedAdd(g_sparseCounters[2],groups,first);
    InterlockedAdd(g_sparseCounters[3],active.count,rowStart);
    if (first>g_sparse.groupCapacity || groups>g_sparse.groupCapacity-first ||
        rowStart>g_sparse.rows || active.count>g_sparse.rows-rowStart) {
        recordError(128u,0xffffffffu,2u,0xffffffffu);
        return;
    }
    g_activeBuckets[slot].rowStart=rowStart;
    for (uint index=0u;index<groups;++index) {
        Bucket bucket={rowStart+index*64u,min(64u,active.count-index*64u),
            active.key%g_sparse.bucketColumns,active.key/g_sparse.bucketColumns};
        g_buckets[first+index]=bucket;
    }
}

[shader("compute")][numthreads(64,1,1)]
void insertSparseParticles(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint row=sparseGroup(group)*64u+lane;
    if (row>=g_sparse.rows || g_errors[0]!=0u) return;
    Particle particle;
    if (!sparseParticle(row,particle)) return;
    SparseIndex entry=g_bucketIndex[bucketKey(particle)];
    if (entry.generation!=g_sparse.generation || entry.slot>=g_sparse.bucketCapacity) {
        recordError(128u,0xffffffffu,3u,row);
        return;
    }
    uint offset;
    InterlockedAdd(g_activeBuckets[entry.slot].inserted,1u,offset);
    uint target=g_activeBuckets[entry.slot].rowStart+offset;
    if (target>=g_sparse.rows || offset>=g_activeBuckets[entry.slot].count) {
        recordError(128u,0xffffffffu,3u,row);
        return;
    }
    g_rows[target]=row;
}

[shader("compute")][numthreads(64,1,1)]
void initializeSparseDestination(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint slot=sparseGroup(group);
    if (slot>=g_sparseCounters[1] || g_errors[0]!=0u) return;
    uint page=g_pageKeys[slot];
    if (slot>=g_sparse.pageCapacity || page>=g_sparse.pageKeys) {
        recordError(128u,0xffffffffu,4u,0xffffffffu);
        return;
    }
    if (g_sparse.destinationA!=0u && lane==0u) {
        SparseIndex entry={g_sparse.generation,slot};
        g_pageIndexA[page]=entry;
    }

    // The destination is immutable for this dispatch. Keep its choice outside
    // the word loop: DXC1.9 otherwise creates a two-entry cycle after publication.
    if (g_sparse.destinationA!=0u) {
        for (uint field=0u;field<4u;++field) {
            uint word=(slot*64u+lane)*4u+field;
            g_payloadA[word]=0;
            g_sparseBudget[word]=0u;
        }
    }
    else {
        for (uint field=0u;field<4u;++field) {
            uint word=(slot*64u+lane)*4u+field;
            g_payloadB[word]=0;
            g_sparseBudget[word]=0u;
        }
    }
}
