#include "grid_particle_native.slang"
#include "world_matter_types.slang"

// ------------------------------------------------------------------
// pass-private immutable projection and stable native-cell grouping
// ------------------------------------------------------------------
struct MatterRecord { uint cell,material;float fill;uint tag; };
struct MatterHierarchy { uint offset,width,height,blockSize; };
struct MatterFrame {
    uint count,movingCount,width,height;
    float cellMetres,origin,minimumX,minimumY;
    uint shift,blockCount,lookupCapacity,ancestorWords;
    uint paletteCount,terrainPaletteCount,oldCount,oldLookupCapacity;
    uint hierarchyLevels,phase,reserved0,reserved1;
};
ConstantBuffer<MatterFrame> g_matter;
StructuredBuffer<NumericalRow> g_numerical;
StructuredBuffer<MetadataRow> g_metadata;
StructuredBuffer<WorldParticleMaterial> g_matter_materials;
StructuredBuffer<uint> g_matter_terrain_ids;
StructuredBuffer<WorldMaterial> g_matter_terrain_materials;
StructuredBuffer<MatterHierarchy> g_matter_hierarchy;
StructuredBuffer<MatterRecord> g_matter_input;
RWStructuredBuffer<MatterRecord> g_matter_output;
RWStructuredBuffer<uint> g_matter_ranks;
RWStructuredBuffer<uint> g_matter_counts;
RWStructuredBuffer<uint> g_matter_prefix;
RWStructuredBuffer<WorldOverlay> g_matter_overlay;
RWStructuredBuffer<uint> g_matter_lookup;
RWStructuredBuffer<uint> g_matter_ancestors;
RWStructuredBuffer<uint> g_matter_summary;
StructuredBuffer<WorldOverlay> g_matter_old_overlay;
StructuredBuffer<uint> g_matter_old_lookup;

static const uint MATTER_DROP=0xffffffffu;
static const uint MATTER_SUMMARY_WORDS=40u;
static const uint MATTER_ROW_MASK=0xfffffu;
groupshared uint matterDigits[256],matterHistogram[16],matterTotals[16];
groupshared uint matterScan[256];

uint matterMaterialIndex(uint id) {
    uint lo=0u,hi=g_matter.paletteCount;
    while (lo<hi) {
        uint mid=lo+(hi-lo)/2u;
        uint candidate=g_matter_materials[mid].id;
        if (candidate<id) lo=mid+1u;else hi=mid;
    }
    if (lo>=g_matter.paletteCount) return MATTER_DROP;
    return g_matter_materials[lo].id==id?lo:MATTER_DROP;
}

WorldMaterial matterTerrain(uint cell) {
    uint packed=g_matter_terrain_ids[cell/2u];
    uint id=(packed>>((cell&1u)*16u))&65535u;
    WorldMaterial value=(WorldMaterial)0;
    if (id>=g_matter.terrainPaletteCount) InterlockedOr(g_matter_summary[0],64u);
    else value=g_matter_terrain_materials[id];
    return value;
}

// Same accepted binary32 conditioning as the shared medium kernel: divide
// normal significands, then restore the exponent in bounded normal chunks.
// A subnormal weighted numerator can be lost only within the separately charged
// once-per-source projection FTZ bound; no unbounded reciprocal is evaluated.
float matterRatio(float numerator,float denominator) {
    if (numerator==0) return 0;
    uint n=asuint(numerator),d=asuint(denominator);
    uint ne=(n>>23u)&255u,de=(d>>23u)&255u;
    if (ne==0u) return 0;
    if (ne==255u || de==0u || de==255u) {InterlockedOr(g_matter_summary[0],64u);return 0;}

    float value=asfloat((n&0x7fffffu)|0x3f800000u)/asfloat((d&0x7fffffu)|0x3f800000u);
    int exponent=int(ne)-int(de);
    for (uint chunk=0u;chunk<2u;++chunk) {
        if (exponent>127) {value*=asfloat(254u<<23u);exponent-=127;}
        else if (exponent< -126) {value*=asfloat(1u<<23u);exponent+=126;}
    }
    return value*asfloat(uint(exponent+127)<<23u);
}

[shader("compute")][numthreads(256,1,1)]
void snapshotWorldMatter(uint3 tid:SV_DispatchThreadID) {
    uint row=tid.x;
    if (row>=g_matter.count) return;
    MetadataRow metadata=g_metadata[row];
    bool moving=metadata.sourceRow!=MATTER_DROP;
    bool validSource=!moving || metadata.sourceRow<g_matter.movingCount;
    NumericalRow hot=(NumericalRow)0;
    if (moving && validSource) hot=g_numerical[metadata.sourceRow];

    ParticleNativeCell cell=particleNativeCell(metadata,float2(hot.positionX,hot.positionY),hot.carried[13],
        validSource,g_matter.cellMetres,g_matter.origin,float2(g_matter.minimumX,g_matter.minimumY),
        uint2(g_matter.width,g_matter.height));
    uint index=matterMaterialIndex(metadata.material);
    uint status=cell.status;
    if (index==MATTER_DROP) status|=32u;
    else if (g_matter_materials[index].value.kind==0u) status|=8u;
    else if (g_matter_materials[index].value.kind!=1u && g_matter_materials[index].value.kind!=2u) status|=32u;

    bool participating=(status&(4u|8u))==0u;
    float fill=asfloat(metadata.physicalFillBits);
    if (participating && (metadata.physicalFillBits<0x00800000u || metadata.physicalFillBits>=0x7f800000u)) status|=1u;
    if ((status&1u)!=0u) InterlockedAdd(g_matter_summary[5],1u);
    if ((status&2u)!=0u) InterlockedAdd(g_matter_summary[6],1u);
    if ((status&4u)!=0u) InterlockedAdd(g_matter_summary[4],1u);
    if ((status&8u)!=0u) InterlockedAdd(g_matter_summary[3],1u);
    if ((status&32u)!=0u) InterlockedAdd(g_matter_summary[7],1u);

    MatterRecord output;
    output.cell=MATTER_DROP;output.material=metadata.material;output.fill=fill;output.tag=row|(status<<20u);
    if (!participating) InterlockedAdd(g_matter_summary[36],1u);
    if (participating) {
        InterlockedAdd(g_matter_summary[2],1u);
        if ((status&(1u|2u|32u))!=0u) InterlockedOr(g_matter_summary[0],status);
        else output.cell=cell.y*g_matter.width+cell.x;
    }
    g_matter_output[row]=output;
}

[shader("compute")][numthreads(256,1,1)]
void rankWorldMatter(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint row=tid.x;
    if (lane<16u) matterHistogram[lane]=0u;
    bool valid=row<g_matter.count;
    MatterRecord value=(MatterRecord)0;
    if (valid) value=g_matter_input[row];
    uint digit=valid?((value.cell>>min(g_matter.shift,28u))&15u):16u;
    matterDigits[lane]=digit;
    GroupMemoryBarrierWithGroupSync();

    if (g_matter.shift==32u) {
        bool head=false;
        if (valid && value.cell!=MATTER_DROP) {
            if (row==0u) head=true;
            else head=g_matter_input[row-1u].cell!=value.cell;
        }
        matterScan[lane]=head?1u:0u;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset=1u;offset<256u;offset*=2u) {
            uint prior=lane>=offset?matterScan[lane-offset]:0u;
            GroupMemoryBarrierWithGroupSync();
            matterScan[lane]+=prior;
            GroupMemoryBarrierWithGroupSync();
        }
        if (valid) g_matter_ranks[row]=matterScan[lane]-(head?1u:0u);
        if (lane<16u) g_matter_counts[group.x*16u+lane]=lane==0u?matterScan[255u]:0u;
        return;
    }

    uint rank=0u;
    for (uint prior=0u;prior<lane;++prior) if (matterDigits[prior]==digit) ++rank;
    if (valid) {
        g_matter_ranks[row]=rank;
        InterlockedAdd(matterHistogram[digit],1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane<16u) g_matter_counts[group.x*16u+lane]=matterHistogram[lane];
}

[shader("compute")][numthreads(16,1,1)]
void prefixWorldMatter(uint lane:SV_GroupIndex) {
    if (g_matter.phase==0u) {
        for (uint word=lane;word<MATTER_SUMMARY_WORDS;word+=16u) {
            uint value=0u;
            if (word==1u) value=g_matter.count;
            if (word==12u) value=g_matter.width;
            if (word==13u) value=g_matter.height;
            if (word>=25u && word<28u) value=0x7f7fffffu;
            g_matter_summary[word]=value;
        }
        return;
    }

    uint total=0u;
    for (uint block=0u;block<g_matter.blockCount;++block) {
        uint index=block*16u+lane;
        g_matter_prefix[index]=total;
        total+=g_matter_counts[index];
    }
    matterTotals[lane]=total;
    GroupMemoryBarrierWithGroupSync();
    uint base=0u;
    for (uint digit=0u;digit<lane;++digit) base+=matterTotals[digit];
    for (uint block=0u;block<g_matter.blockCount;++block) g_matter_prefix[block*16u+lane]+=base;
}

[shader("compute")][numthreads(256,1,1)]
void scatterWorldMatter(uint3 tid:SV_DispatchThreadID,uint3 group:SV_GroupID) {
    uint row=tid.x;
    if (row>=g_matter.count) return;
    MatterRecord value=g_matter_input[row];
    uint digit=(value.cell>>g_matter.shift)&15u;
    uint output=g_matter_prefix[group.x*16u+digit]+g_matter_ranks[row];
    if (output>=g_matter.count) {InterlockedOr(g_matter_summary[0],64u);return;}
    g_matter_output[output]=value;
}

[shader("compute")][numthreads(256,1,1)]
void reduceWorldMatter(uint3 tid:SV_DispatchThreadID,uint3 group:SV_GroupID) {
    uint first=tid.x;
    if (first>=g_matter.count) return;
    uint cell=g_matter_input[first].cell;
    if (cell==MATTER_DROP) return;
    if (first!=0u) {
        if (g_matter_input[first-1u].cell==cell) return;
    }

    float opaque=0;
    float3 source=0,albedo=0,sigma=0,emission=0;
    for (uint row=first;row<g_matter.count;++row) {
        MatterRecord record=g_matter_input[row];
        if (record.cell!=cell) break;
        uint index=matterMaterialIndex(record.material);
        if (index==MATTER_DROP) {InterlockedOr(g_matter_summary[0],32u);return;}
        WorldMaterial material=g_matter_materials[index].value;
        if (material.kind==1u) {
            opaque+=record.fill;
            source+=record.fill*float3(material.r,material.g,material.b);
            albedo+=record.fill*float3(material.albedoR,material.albedoG,material.albedoB);
        } else if (material.kind==2u) {
            sigma+=record.fill*float3(material.sigmaR,material.sigmaG,material.sigmaB);
            emission+=record.fill*float3(material.jR,material.jG,material.jB);
        } else {InterlockedOr(g_matter_summary[0],32u);return;}
    }

    if (!isfinite(opaque) || !all(isfinite(source)) || !all(isfinite(albedo)) ||
        !all(isfinite(sigma)) || !all(isfinite(emission))) {InterlockedOr(g_matter_summary[0],64u);return;}
    if (opaque>0) {
        for (uint channel=0u;channel<3u;++channel) {
            source[channel]=matterRatio(source[channel],opaque);
            albedo[channel]=matterRatio(albedo[channel],opaque);
        }
    }
    if (!all(isfinite(source)) || !all(isfinite(albedo)) || any(albedo<0) || any(albedo>1)) {
        InterlockedOr(g_matter_summary[0],64u);return;
    }

    uint ordinal=g_matter_prefix[group.x*16u]+g_matter_ranks[first];
    if (ordinal>=g_matter.count) {InterlockedOr(g_matter_summary[0],64u);return;}
    WorldOverlay output;
    output.cell=cell;output.coverage=min(1.0,opaque);
    output.r=source.x;output.g=source.y;output.b=source.z;
    output.albedoR=albedo.x;output.albedoG=albedo.y;output.albedoB=albedo.z;
    output.sigmaR=sigma.x;output.sigmaG=sigma.y;output.sigmaB=sigma.z;
    output.jR=emission.x;output.jG=emission.y;output.jB=emission.z;
    g_matter_overlay[ordinal]=output;
    InterlockedMax(g_matter_summary[8],ordinal+1u);

    WorldMaterial terrain=matterTerrain(cell);
    if (terrain.kind!=1u) {
        if (output.coverage>0 && output.coverage<1) InterlockedAdd(g_matter_summary[9],1u);
        if (output.coverage==1) InterlockedAdd(g_matter_summary[10],1u);
        sigma+=float3(terrain.sigmaR,terrain.sigmaG,terrain.sigmaB);
        emission+=float3(terrain.jR,terrain.jG,terrain.jB);
        if (!all(isfinite(sigma)) || !all(isfinite(emission))) {InterlockedOr(g_matter_summary[0],64u);return;}
        for (uint channel=0u;channel<3u;++channel) {
            if (output.coverage>0) InterlockedMax(g_matter_summary[16u+channel],asuint(source[channel]));
            InterlockedMax(g_matter_summary[19u+channel],asuint(sigma[channel]));
            InterlockedMax(g_matter_summary[22u+channel],asuint(emission[channel]));
            if (sigma[channel]>0) {
                InterlockedMin(g_matter_summary[25u+channel],asuint(sigma[channel]));
                InterlockedMax(g_matter_summary[28u+channel],asuint(emission[channel]));
            } else InterlockedMax(g_matter_summary[31u+channel],asuint(emission[channel]));
        }
        if ((output.coverage>0 && any(source>0)) || any(emission>0)) {
            uint x=cell%g_matter.width,y=cell/g_matter.width;
            InterlockedMin(g_matter_summary[12],x);InterlockedMin(g_matter_summary[13],y);
            InterlockedMax(g_matter_summary[14],x+1u);InterlockedMax(g_matter_summary[15],y+1u);
        }
    }
    if (output.coverage>0 && output.coverage<1) InterlockedAdd(g_matter_summary[34],1u);
    if (output.coverage==1) InterlockedAdd(g_matter_summary[35],1u);
}

uint matterLookup(uint cell,bool old) {
    uint count=old?g_matter.oldCount:g_matter_summary[8];
    uint capacity=old?g_matter.oldLookupCapacity:g_matter.lookupCapacity;
    uint slot=(cell*2654435761u)&(capacity-1u);
    for (uint probe=0u;probe<capacity;++probe) {
        uint value=old?g_matter_old_lookup[slot]:g_matter_lookup[slot];
        if (value==0u) return MATTER_DROP;
        if (value>count) {InterlockedOr(g_matter_summary[0],64u);return MATTER_DROP;}
        uint key=old?g_matter_old_overlay[value-1u].cell:g_matter_overlay[value-1u].cell;
        if (key==cell) return value-1u;
        slot=(slot+1u)&(capacity-1u);
    }
    InterlockedOr(g_matter_summary[0],64u);return MATTER_DROP;
}

[shader("compute")][numthreads(256,1,1)]
void finishWorldMatter(uint3 tid:SV_DispatchThreadID) {
    uint ordinal=tid.x;
    if (g_matter.phase==0u) {
        if (ordinal>=g_matter_summary[8]) return;
        WorldOverlay row=g_matter_overlay[ordinal];
        uint slot=(row.cell*2654435761u)&(g_matter.lookupCapacity-1u);
        bool inserted=false;
        for (uint probe=0u;probe<g_matter.lookupCapacity;++probe) {
            uint previous;
            InterlockedCompareExchange(g_matter_lookup[slot],0u,ordinal+1u,previous);
            if (previous==0u) {inserted=true;break;}
            if (previous>g_matter_summary[8] || g_matter_overlay[previous-1u].cell==row.cell) break;
            slot=(slot+1u)&(g_matter.lookupCapacity-1u);
        }
        if (!inserted) {InterlockedOr(g_matter_summary[0],64u);return;}

        if (row.coverage>0 || any(float3(row.sigmaR,row.sigmaG,row.sigmaB)>0) || any(float3(row.jR,row.jG,row.jB)>0)) {
            uint2 cell=uint2(row.cell%g_matter.width,row.cell/g_matter.width);
            for (uint level=1u;level<g_matter.hierarchyLevels;++level) {
                MatterHierarchy h=g_matter_hierarchy[level];
                uint2 index=cell/h.blockSize;
                uint bit=h.offset+index.y*h.width+index.x-g_matter.width*g_matter.height;
                if (bit/32u>=g_matter.ancestorWords) {InterlockedOr(g_matter_summary[0],64u);return;}
                InterlockedOr(g_matter_ancestors[bit/32u],1u<<(bit&31u));
            }
        }
        return;
    }

    uint count=g_matter_summary[8];
    if (ordinal<count) {
        WorldOverlay row=g_matter_overlay[ordinal];
        uint prior=matterLookup(row.cell,true);
        bool changed=prior==MATTER_DROP;
        if (!changed) {
            WorldOverlay old=g_matter_old_overlay[prior];
            changed=asuint(row.coverage)!=asuint(old.coverage) ||
                any(asuint(float3(row.r,row.g,row.b))!=asuint(float3(old.r,old.g,old.b))) ||
                any(asuint(float3(row.albedoR,row.albedoG,row.albedoB))!=asuint(float3(old.albedoR,old.albedoG,old.albedoB))) ||
                any(asuint(float3(row.sigmaR,row.sigmaG,row.sigmaB))!=asuint(float3(old.sigmaR,old.sigmaG,old.sigmaB))) ||
                any(asuint(float3(row.jR,row.jG,row.jB))!=asuint(float3(old.jR,old.jG,old.jB)));
        }
        if (changed) InterlockedAdd(g_matter_summary[11],1u);
    }
    if (ordinal<g_matter.oldCount && matterLookup(g_matter_old_overlay[ordinal].cell,false)==MATTER_DROP)
        InterlockedAdd(g_matter_summary[11],1u);
}
