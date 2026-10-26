// Canonical ownership shared by sparse maintenance and the production adapter.
struct SparseIndex { uint generation, slot; };
struct SparseBucket { uint key, count, rowStart, inserted; };
struct SparseConstants {
    uint width, height, rows, bucketColumns;
    uint pageColumns, bucketCapacity, pageCapacity, groupCapacity;
    uint generation, sourceGeneration, dispatchWidth, destinationA;
    uint bucketKeys, pageKeys, reserved0, reserved1;
};
ConstantBuffer<SparseConstants> g_sparse;
RWStructuredBuffer<SparseIndex> g_bucketIndex;
RWStructuredBuffer<SparseIndex> g_pageIndexA;
RWStructuredBuffer<SparseIndex> g_pageIndexB;
RWStructuredBuffer<SparseBucket> g_activeBuckets;
RWStructuredBuffer<uint> g_pageKeys;
RWStructuredBuffer<int> g_payloadA;
RWStructuredBuffer<int> g_payloadB;
RWStructuredBuffer<uint> g_sparseBudget;
RWStructuredBuffer<uint> g_sparseCounters;
RWStructuredBuffer<uint> g_sparseArgs;

uint sparseGroup(uint3 group) { return group.x+group.y*g_sparse.dispatchWidth; }

bool sparseFlatKey(int2 node, out uint key) {
    key=0u;
    // Preserve the old inclusive tuple predicate and the subsequent flat bound.
    if (any(node<0) || any(node>int2(g_sparse.width,g_sparse.height))) return false;
    key=uint(node.y)*g_sparse.width+uint(node.x);
    return key<g_sparse.width*g_sparse.height;
}

uint sparsePageKey(uint key) {
    uint2 canonical=uint2(key%g_sparse.width,key/g_sparse.width);
    return (canonical.y/8u)*g_sparse.pageColumns+canonical.x/8u;
}

uint sparseLane(uint key) {
    return ((key/g_sparse.width)%8u)*8u+(key%g_sparse.width)%8u;
}

bool sparseWord(uint key, bool sourceA, uint generation, bool destination, out uint word) {
    word=0u;
    if (generation==0u) return false;
    uint page=sparsePageKey(key);
    SparseIndex entry=sourceA ? g_pageIndexA[page] : g_pageIndexB[page];
    if (entry.generation!=generation || entry.slot>=g_sparse.pageCapacity) {
        if (destination) recordError(128u,key,0xffffffffu,0xffffffffu);
        return false;
    }

    word=(entry.slot*64u+sparseLane(key))*4u;
    return true;
}

int4 sparseSource(uint key, bool sourceA, uint generation) {
    uint word;
    if (!sparseWord(key,sourceA,generation,false,word)) return int4(0);

    if (sourceA) return int4(g_payloadA[word],g_payloadA[word+1u],g_payloadA[word+2u],g_payloadA[word+3u]);
    return int4(g_payloadB[word],g_payloadB[word+1u],g_payloadB[word+2u],g_payloadB[word+3u]);
}

bool reserveSparseBudget(uint word, uint key, uint magnitude, uint row, uint field) {
    uint before;
    InterlockedAdd(g_sparseBudget[word],0u,before);
    for (;;) {
        if (before>0x7fffffffu || magnitude>0x7fffffffu-before) {
            recordError(4u,key,field,row);
            return false;
        }

        uint observed;
        InterlockedCompareExchange(g_sparseBudget[word],before,before+magnitude,observed);
        if (observed==before) return true;
        before=observed;
    }
}
