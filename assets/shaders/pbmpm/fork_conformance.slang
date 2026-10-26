// Test-only native conformance port of ErikSom/pbmpm 859c4490c749c1cc364d099543dbf5aa0bd9be1b.
// Original BSD-3-Clause notice: LICENSE.md. Shared arithmetic is also consumed by production GpuSolver.
#include "types.slang"

// ------------------------------------------------------------------
// split storage, bucket protocol and owned error status
// ------------------------------------------------------------------
#include "fork_core.slang"

void fail(uint flag, uint row, uint field) {
    InterlockedOr(g_errors[0], flag);
    uint previous;
    InterlockedCompareExchange(g_errors[1], 0xffffffffu, row, previous);
    if (previous == 0xffffffffu) g_errors[2] = field;
}

void failNode(uint node, uint field) {
    fail(2u,0xffffffffu,field);
}

bool validParticle(Particle p, uint row) {
    float values[22] = {p.positionX,p.positionY,p.displacementX,p.displacementY,
        p.deformationGradient.c0x,p.deformationGradient.c0y,p.deformationGradient.c1x,p.deformationGradient.c1y,
        p.deformationDisplacement.c0x,p.deformationDisplacement.c0y,p.deformationDisplacement.c1x,p.deformationDisplacement.c1y,
        p.liquidDensity,p.mass,p.material,p.volume,p.lambda,p.logJp,p.colorR,p.colorG,p.colorB,p.enabled};
    for (uint i=0; i<22; ++i) { if (!isfinite(values[i])) { fail(8u,row,i); return false; } }

    if (p.positionX<3 || p.positionY<3 || p.positionX>float(g_parameters.gridWidth)-4 ||
        p.positionY>float(g_parameters.gridHeight)-4 || p.liquidDensity<=0 || p.mass<=0 || p.volume<=0 ||
        p.material<0 || p.material>3 || floor(p.material)!=p.material || (p.enabled!=0 && p.enabled!=1)) {
        fail(16u,row,0xffffffffu); return false;
    }

    return true;
}

// ------------------------------------------------------------------
// actual source count/allocation/insertion with explicit bounds rejection
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(64,1,1)]
void countBuckets(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_parameters.particleCount) return;

    Particle p=loadParticle(id.x);
    if (!validParticle(p,id.x) || p.enabled==0) return;

    uint2 b=uint2(float2(p.positionX,p.positionY)/6.0);
    InterlockedAdd(g_counts[b.y*((g_parameters.gridWidth+5)/6)+b.x],1u);
}

[shader("compute")]
[numthreads(8,8,1)]
void allocateBuckets(uint3 id:SV_DispatchThreadID) {
    uint width=(g_parameters.gridWidth+5)/6, height=(g_parameters.gridHeight+5)/6;
    if (id.x>=width || id.y>=height) return;

    uint count=g_counts[id.y*width+id.x];
    if (count==0) return;

    uint groups=(count+63)/64, groupStart, rowStart, capacity, stride;
    InterlockedAdd(g_args[0],groups,groupStart);
    InterlockedAdd(g_allocator[0],count,rowStart);
    g_buckets.GetDimensions(capacity,stride);
    if (groupStart+groups>capacity || rowStart+count>g_parameters.particleCount) { fail(32u,0xffffffffu,0u); return; }

    g_starts[id.y*width+id.x]=rowStart;

    for (uint i=0;i<groups;++i) {
        Bucket bucket={rowStart+i*64,min(64u,count-i*64),id.x,id.y};
        g_buckets[groupStart+i]=bucket;
    }
}

[shader("compute")]
[numthreads(64,1,1)]
void insertBuckets(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_parameters.particleCount) return;

    Particle p=loadParticle(id.x);
    if (!validParticle(p,id.x) || p.enabled==0) return;

    uint2 b=uint2(float2(p.positionX,p.positionY)/6.0);
    uint index=b.y*((g_parameters.gridWidth+5)/6)+b.x, offset;
    InterlockedAdd(g_insertCounts[index],1u,offset);
    uint target=g_starts[index]+offset;
    if (target>=g_parameters.particleCount) { fail(32u,id.x,0u); return; }

    g_rows[target]=id.x;
}

// ------------------------------------------------------------------
// selected fork matrix/constraint/integration semantics
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// fused selected source stage; all lanes cross shared barriers on rejection
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(64,1,1)]
void fused(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    for (uint i=lane;i<256;i+=64) { s_src[i]=0; s_dst[i]=0; }
    GroupMemoryBarrierWithGroupSync();

    Bucket bucket=g_buckets[group.x];
    int2 origin=int2(bucket.x,bucket.y)*6-1;
    int2 local=int2(lane%8,lane/8), node=origin+local;
    uint tile=(uint(local.y)*8+uint(local.x))*4;
    bool valid=all(node>=0) && all(node<=int2(g_parameters.gridWidth,g_parameters.gridHeight));
    uint base=uint(node.y)*g_parameters.gridWidth*4+uint(node.x)*4;
    uint gridWords, gridStride;
    g_gridSrc.GetDimensions(gridWords,gridStride);
    valid=valid && base+3<gridWords;
    if (valid) {
        float2 d=float2(decodeWord(g_gridSrc[base]),decodeWord(g_gridSrc[base+1]));
        float mass=decodeWord(g_gridSrc[base+2]);
        d=mass<1e-5 ? float2(0) : d/mass;
        for (uint i=0;i<g_parameters.shapeCount;++i) {
            Contact contact=collide(g_shapes[i],float2(node)+d);
            if (contact.collides) d-=max(dot(contact.normal,d)-min(0.0,dot(contact.normal,contact.point-float2(node))),0.0)*contact.normal;
        }

        float2 endpoint=float2(node)+d;
        float2 correction=guardian(endpoint,uint2(g_parameters.gridWidth,g_parameters.gridHeight),4.0)-endpoint;
        if (correction.x!=0) { d.x=0; d.y=lerp(d.y,0.0,0.5); }
        if (correction.y!=0) { d.y=0; d.x=lerp(d.x,0.0,0.5); }
        s_src[tile]=checkedEncodeNode(d.x,base/4u,0u);
        s_src[tile+1]=checkedEncodeNode(d.y,base/4u,1u);
        s_src[tile+2]=checkedEncodeNode(mass,base/4u,2u);
        s_src[tile+3]=checkedEncodeNode(decodeWord(g_gridSrc[base+3]),base/4u,3u);
    }

    GroupMemoryBarrierWithGroupSync();

    if (lane<bucket.count) {
        uint row=g_rows[bucket.start+lane];
        Particle p=loadParticle(row);
        float2 oldPosition=float2(p.positionX,p.positionY);
        Weights weights=quadraticWeights(oldPosition);
        if (g_parameters.reserved0!=0) {
            SourceMatrix B=zeroMatrix();
            float2 d=float2(0); float volume=0;
            for (int i=0;i<3;++i) {
                for (int j=0;j<3;++j) {
                    float weight=weights.w[i].x*weights.w[j].y;
                    int2 neighbour=weights.base+int2(i,j), localNeighbour=neighbour-origin;
                    uint at=(uint(localNeighbour.y)*8+uint(localNeighbour.x))*4;
                    float2 weighted=weight*float2(decodeWord(s_src[at]),decodeWord(s_src[at+1]));
                    float2 gatherOffset=float2(neighbour)-oldPosition+0.5;
                    float dbx0=weighted.x*gatherOffset.x, dby0=weighted.y*gatherOffset.x;
                    float dbx1=weighted.x*gatherOffset.y, dby1=weighted.y*gatherOffset.y;
                    B=matrix(B.c0x+dbx0,B.c0y+dby0,B.c1x+dbx1,B.c1y+dby1); d+=weighted;
                    if (g_parameters.useGridVolumeForLiquid!=0) volume+=weight*decodeWord(s_src[at+3]);
                }
            }

            if (g_parameters.useGridVolumeForLiquid!=0) {
                volume=1.0/volume;
                if (volume<1.0) p.liquidDensity=lerp(p.liquidDensity,volume,0.1);
            }
            p.deformationDisplacement=scale(B,4.0); p.displacementX=d.x; p.displacementY=d.y;
            if (g_parameters.reserved0==1) p=integration(p,row);
            if (validParticle(p,row)) saveParticle(row,p);
        }

        Particle updated=constraint(p);
        for (int i=0;i<3;++i) {
            for (int j=0;j<3;++j) {
                float weight=weights.w[i].x*weights.w[j].y;
                int2 neighbour=weights.base+int2(i,j), localNeighbour=neighbour-origin;
                uint at=(uint(localNeighbour.y)*8+uint(localNeighbour.x))*4;
                uint globalAt=(uint(neighbour.y)*g_parameters.gridWidth+uint(neighbour.x))*4;
                float weightedMass=weight*updated.mass;
                float2 momentum=weightedMass*(float2(updated.displacementX,updated.displacementY)+
                    transform(updated.deformationDisplacement,float2(neighbour)-oldPosition+0.5));
                scatterChecked(at,globalAt,momentum.x,row,0);
                scatterChecked(at+1,globalAt+1,momentum.y,row,1);
                scatterChecked(at+2,globalAt+2,weightedMass,row,2);
                if (g_parameters.useGridVolumeForLiquid!=0) scatterChecked(at+3,globalAt+3,updated.volume*weight,row,3);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (valid) {
        for (uint field=0;field<4;++field) InterlockedAdd(g_gridDst[base+field],s_dst[tile+field]);
    }
}
