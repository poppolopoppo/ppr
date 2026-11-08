// Bounded native-cell cascade diagnostic and straight256 small-scene reference.
// Neither mode establishes full-world quality, storage or performance admission.
// No contour/certificate/FP64 dependency. All coordinates are local native cells.
#include "world_radiance_types.slang"

#include "world_matter_types.slang"
struct WorldCell { WorldMaterial material;float coverage; };
struct WorldInterval { float r, g, b; uint status; float tr, tg, tb; uint segments; };
struct WorldHierarchyLevel { uint offset, width, height, blockSize; };
struct WorldLevel {
    int minX, minY; uint countX, countY;
    uint spacing, directions, directionOffset, records;
    float nearCells, farCells; uint reserved0, reserved1;
};
struct WorldParameters {
    uint width, height, paletteCount, hierarchyLevels;
    float boundaryX, boundaryY, boundaryZ; uint maxSegments;
    int refinedMinX, refinedMinY; uint refinedCountX, refinedCountY;
    float maxSourceR, maxSourceG, maxSourceB, maximumResponse;
    float cellMetres,farBoundR,farBoundG,farBoundB;
    uint overlayCount,overlayCapacity,overlayAncestorWords,skyHourPlusOne;
};
// Refine level selects transport: 0 footprint, 1 straight256, 2 receiver window.
// Coarse solve retains its original level index; the scalar dispatch ABI is32B.
struct WorldDispatch {
    uint level,parentLevel,count,refinedSpacing;
    int receiverMinX,receiverMinY;uint receiverCountX,receiverCountY;
};
struct WorldQuery { float ox, oy, dx, dy, nearCells, farCells; uint terminal, reserved; };
struct WorldWork { uint queries, completed, failed, segments, skipped; };
struct WorldDiscoveryDispatch {
    uint level,parentLevel,firstRecord,count;
    uint refinedSpacing,keyCapacity,bitsetWords,reserved;
};
struct WorldBounceDispatch { uint faceCount,lookupCapacity,firstRecord,reserved; };
struct WorldTemporalDispatch {
    uint phase,firstRecord,phaseWidth,refinedSpacing;
    float historyWeight;uint feedbackAvailable,skipTable,regionOffset;
    uint workQueue,workGroupSize;uint2 sampleTimeNs,sourceVersion;uint2 reserved;
};

ConstantBuffer<WorldParameters> g_world;
ConstantBuffer<WorldDispatch> g_dispatch;
ConstantBuffer<WorldBounceDispatch> g_bounce;
ConstantBuffer<WorldTemporalDispatch> g_temporal;
struct WorldProbeState {
    uint2 sourceVersion,sampleTimeNs;
    float variation;uint samples;
};
StructuredBuffer<WorldProbeState> g_previous_probe_state;
RWStructuredBuffer<WorldProbeState> g_probe_state;
StructuredBuffer<WorldCoefficients> g_previous_field;
StructuredBuffer<uint> g_temporal_work;
RWStructuredBuffer<uint> g_temporal_work_output;
RWStructuredBuffer<uint> g_temporal_arguments;
Texture2D<uint> g_skip_table;
[format("r8ui")]
RWTexture2D<uint> g_skip_table_output;
StructuredBuffer<uint> g_face_keys;
RWStructuredBuffer<uint> g_face_lookup;
RWStructuredBuffer<WorldRgb> g_face_moments;
StructuredBuffer<WorldCoefficients> g_bounce_direct;
StructuredBuffer<WorldCoefficients> g_bounce_indirect;
StructuredBuffer<uint> g_cell_ids; // even ID low16, odd ID high16; no Int16 capability
StructuredBuffer<WorldMaterial> g_materials;
StructuredBuffer<WorldOverlay> g_overlay;
StructuredBuffer<uint> g_overlay_lookup;
StructuredBuffer<uint> g_overlay_ancestors;
StructuredBuffer<uint> g_vacuum;
StructuredBuffer<WorldHierarchyLevel> g_hierarchy;
StructuredBuffer<WorldLevel> g_levels;
StructuredBuffer<float2> g_directions;
StructuredBuffer<uint> g_page_masks;
StructuredBuffer<WorldInterval> g_parent;
StructuredBuffer<WorldQuery> g_queries; // qualification entry only
RWStructuredBuffer<WorldInterval> g_current;
RWStructuredBuffer<WorldCoefficients> g_refined;
RWStructuredBuffer<uint> g_counts;
ConstantBuffer<WorldDiscoveryDispatch> g_discovery;
RWStructuredBuffer<uint> g_discovery_bits;
RWStructuredBuffer<uint> g_discovery_keys;
RWStructuredBuffer<uint> g_discovery_state;

// ------------------------------------------------------------------
// compact mandatory work in stable spatial/directional order
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64,1,1)]
void buildWorldTemporalWork(uint3 tid:SV_DispatchThreadID) {
    if (tid.x==0u) {
        g_temporal_work_output[0]=g_dispatch.count;
        g_temporal_arguments[0]=(g_dispatch.count+g_temporal.workGroupSize-1u)/g_temporal.workGroupSize;
        g_temporal_arguments[1]=1u;
        g_temporal_arguments[2]=1u;
    }
    if (tid.x>=g_dispatch.count) return;

    // Every item in this initial bin is mandatory. No visibility/variance shortcut
    // can remove a dependency; later optional bins must retain that invariant.
    bool refine=g_dispatch.level==0u;
    uint directions=refine?1u:g_levels[g_dispatch.level].directions;
    uint width=refine?g_world.refinedCountX:g_levels[g_dispatch.level].countX;
    uint record=g_temporal.firstRecord+tid.x;
    uint spatial=record/directions;
    uint2 selected=uint2(spatial%g_temporal.phaseWidth,spatial/g_temporal.phaseWidth);
    selected+=uint2(g_temporal.regionOffset%width,g_temporal.regionOffset/width);
    g_temporal_work_output[tid.x+1u]=(selected.y*width+selected.x)*directions+record%directions;
}

// One group compacts at most 256 optional refined probes into three
// locality-preserving bins. The CPU still budgets the full candidate cost.
groupshared uint worldPriorityMask[24];
groupshared uint worldPriorityBase[24];

uint worldRefinedCandidate(uint candidate) {
    uint record=g_temporal.firstRecord+candidate;
    return g_temporal.regionOffset+(record/g_temporal.phaseWidth)*g_world.refinedCountX+record%g_temporal.phaseWidth;
}

uint worldRefinementBin(uint index) {
    if (g_temporal.feedbackAvailable==0u) return 0u;
    WorldProbeState previous=g_previous_probe_state[index];
    if (previous.samples==0u || any(previous.sourceVersion!=g_temporal.sourceVersion)) return 0u;

    uint low=g_temporal.sampleTimeNs.x-previous.sampleTimeNs.x;
    uint high=g_temporal.sampleTimeNs.y-previous.sampleTimeNs.y-uint(g_temporal.sampleTimeNs.x<previous.sampleTimeNs.x);
    if (high!=0u || low>=250000000u) return 0u;

    // Variation is squared normalized RGB/directional change. At observed
    // change >= 1, a sample is due after one 60 Hz interval. Quiet rows age
    // into the first bin after 250 ms, regardless of brightness or visibility.
    float change=saturate(sqrt(max(0.0,previous.variation)));
    if (float(low)*(1.0+14.0*change)<250000000.0) return 3u;
    return change>=0.5 ? 1u : 2u;
}

[shader("compute")]
[numthreads(64,1,1)]
void buildWorldConvergenceWork(uint lane:SV_GroupIndex) {
    if (lane<24u) worldPriorityMask[lane]=0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint i=lane;i<g_dispatch.count;i+=64u) {
        uint bin=worldRefinementBin(worldRefinedCandidate(i));
        if (bin<3u) InterlockedOr(worldPriorityMask[bin*8u+i/32u],1u<<(i%32u));
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane==0u) {
        uint total=0u;
        for (uint word=0u;word<24u;++word) {
            worldPriorityBase[word]=total;
            total+=countbits(worldPriorityMask[word]);
        }
        g_temporal_work_output[0]=total;
        g_temporal_arguments[0]=total;
        g_temporal_arguments[1]=1u;
        g_temporal_arguments[2]=1u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint i=lane;i<g_dispatch.count;i+=64u) {
        uint bit=1u<<(i%32u);
        for (uint bin=0u;bin<3u;++bin) {
            uint word=bin*8u+i/32u;
            uint mask=worldPriorityMask[word];
            if ((mask&bit)!=0u) {
                uint slot=worldPriorityBase[word]+countbits(mask&(bit-1u));
                g_temporal_work_output[slot+1u]=worldRefinedCandidate(i);
                break;
            }
        }
    }
}

static const uint WORLD_PROGRESS = 2u;
static const uint WORLD_SOURCE = 4u;
static const uint WORLD_PARENT = 8u;
static const float WORLD_INF = 3.402823466e38;

groupshared uint worldGroupCounts[8];

// Direct-window rays occupy adjacent component slots within each64-lane pass.
// RGB and status hold exactly4096 logical bytes, independent of world extent.
groupshared float worldRayR[256];
groupshared float worldRayG[256];
groupshared float worldRayB[256];
groupshared uint worldRayStatus[256];

WorldInterval worldIdentity() {
    WorldInterval value = {0,0,0,0,1,1,1,0};
    return value;
}

float3 worldL(WorldInterval value) { return float3(value.r, value.g, value.b); }
float3 worldT(WorldInterval value) { return float3(value.tr, value.tg, value.tb); }

// A narrow coordinate/ownership representability guard, not an intersection
// certificate. Transport products use the separately admitted practical FP32
// domain, including its explicitly bounded loss when attenuation flushes to zero.
bool worldNormalOrZero(float value) {
    uint magnitude=asuint(value)&0x7fffffffu;
    return magnitude==0u || (magnitude>=0x00800000u && magnitude<0x7f800000u);
}

float3 worldMultiply(float3 a, float3 b, inout uint status) {
    float3 result=a*b;
    // Practical FP32 transport: finite subnormals/zero products are allowed.
    // Source admission bounds the lost contribution from permitted FTZ once,
    // rather than treating deep opaque attenuation as a failed certificate.
    for (uint channel=0u;channel<3u;++channel) {
        if (!isfinite(a[channel]) || !isfinite(b[channel]) || !isfinite(result[channel])) status|=WORLD_INVALID;
    }
    return result;
}

WorldInterval worldCompose(WorldInterval nearValue, WorldInterval farValue) {
    WorldInterval result = nearValue;
    float3 L = worldL(nearValue) + worldMultiply(worldT(nearValue),worldL(farValue),result.status);
    float3 T = worldMultiply(worldT(nearValue),worldT(farValue),result.status);
    result.r=L.x; result.g=L.y; result.b=L.z;
    result.tr=T.x; result.tg=T.y; result.tb=T.z;
    result.status |= farValue.status;
    if (!worldFinite3(L) || !worldFinite3(T) || any(L < 0) || any(T < 0) || any(T > 1))
        result.status |= WORLD_INVALID;
    return result;
}

// Every lane, including out-of-domain lanes, reaches both group barriers.
// Global atomics are bounded per workgroup, never per connector/query.
void worldBeginGroup(uint lane) {
    if (lane < 8u) worldGroupCounts[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
}

void worldFinishGroup(uint lane, WorldWork work, bool wrote, uint status) {
    InterlockedAdd(worldGroupCounts[0], work.queries);
    InterlockedAdd(worldGroupCounts[1], work.completed);
    InterlockedAdd(worldGroupCounts[2], work.failed);
    InterlockedAdd(worldGroupCounts[3], wrote ? 1u : 0u);
    InterlockedAdd(worldGroupCounts[4], wrote && status != 0u ? 1u : 0u);
    InterlockedAdd(worldGroupCounts[5], work.segments);
    InterlockedAdd(worldGroupCounts[6], work.skipped);
    InterlockedOr(worldGroupCounts[7], status);
    GroupMemoryBarrierWithGroupSync();
    if (lane < 8u) {
        if (lane == 7u) InterlockedOr(g_counts[lane], worldGroupCounts[lane]);
        else InterlockedAdd(g_counts[lane], worldGroupCounts[lane]);
    }
}

uint worldMaterialId(int2 cell) {
    uint index = uint(cell.y) * g_world.width + uint(cell.x);
    uint packed = g_cell_ids[index >> 1u];
    return (packed >> ((index & 1u) * 16u)) & 65535u;
}

bool worldInside(int2 cell) {
    return cell.x >= 0 && cell.y >= 0 && cell.x < int(g_world.width) && cell.y < int(g_world.height);
}

// One immutable terrain/committed-cell projection for ALL native walkers.
// Terrain opaque wins; partial surface coverage never becomes volumetric j.
WorldCell worldCell(int2 cell,inout uint status) {
    WorldCell value;uint id=worldMaterialId(cell);
    if (id>=g_world.paletteCount) {
        status|=WORLD_SOURCE;value.material=(WorldMaterial)0;value.coverage=0;return value;
    }

    value.material=g_materials[id];
    value.coverage=value.material.kind==1u?1:0;
    if (value.coverage==1 || g_world.overlayCount==0u) return value;
    if (g_world.overlayCapacity==0u) {status|=WORLD_SOURCE;return value;}

    uint key=uint(cell.y)*g_world.width+uint(cell.x);
    uint slot=(key*2654435761u)&(g_world.overlayCapacity-1u);
    for (uint probe=0u;probe<g_world.overlayCapacity;++probe) {
        uint ordinal=g_overlay_lookup[slot];
        if (ordinal==0u) return value;
        if (ordinal>g_world.overlayCount) {status|=WORLD_SOURCE;return value;}

        WorldOverlay row=g_overlay[ordinal-1u];
        if (row.cell==key) {
            value.coverage=row.coverage;
            value.material.r=row.r;value.material.g=row.g;value.material.b=row.b;
            value.material.albedoR=row.albedoR;value.material.albedoG=row.albedoG;value.material.albedoB=row.albedoB;
            value.material.sigmaR+=row.sigmaR;value.material.sigmaG+=row.sigmaG;value.material.sigmaB+=row.sigmaB;
            value.material.jR+=row.jR;value.material.jG+=row.jG;value.material.jB+=row.jB;
            if (value.coverage==1) value.material.kind=1u;
            else if (value.coverage>0 || any(float3(value.material.sigmaR,value.material.sigmaG,value.material.sigmaB)>0) ||
                     any(float3(value.material.jR,value.material.jG,value.material.jB)>0)) value.material.kind=2u;
            return value;
        }

        slot=(slot+1u)&(g_world.overlayCapacity-1u);
    }

    status|=WORLD_SOURCE;
    return value;
}

// Source-version-local conservative empty-block certificates. Zero means no skip.
[shader("compute")]
[numthreads(8,8,1)]
void buildWorldSkipTable(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_world.width || id.y>=g_world.height) return;
    uint status=0u;
    WorldCell value=worldCell(int2(id.xy),status);
    uint size=0u;
    if (status==0u && value.material.kind==0u && value.coverage==0) {
        size=1u;
        for (uint level=1u;level<g_world.hierarchyLevels;++level) {
            WorldHierarchyLevel h=g_hierarchy[level];
            if (h.blockSize>32u) break;
            uint2 index=id.xy/h.blockSize;
            uint nativeIndex=h.offset+index.y*h.width+index.x;
            uint ancestorIndex=nativeIndex-g_world.width*g_world.height;
            bool overlayVacuum=g_world.overlayAncestorWords==0u ||
                (ancestorIndex/32u<g_world.overlayAncestorWords &&
                 (g_overlay_ancestors[ancestorIndex/32u]&(1u<<(ancestorIndex&31u)))==0u);
            if (g_vacuum[nativeIndex]!=0u && overlayVacuum) size=h.blockSize;
        }
    }
    g_skip_table_output[id.xy]=size;
}

// Half-open cells with ray-forward ownership at exact integer faces. A negative
// component owns the cell below that face; an axis-zero component retains floor.
int worldForwardCell(float coordinate, float direction) {
    float base = floor(coordinate);
    return int(base) - ((direction < 0 && coordinate == base) ? 1 : 0);
}

// Local ownership exception only when a rounded initial coordinate is integer.
// These helpers prove exact SMALL quantities; the general noninteger path remains
// the declared FP32 native-cell approximation, not a ray-certificate framework.
bool worldFaceOffset(float plane, float origin, out float offset) {
    offset=0;
    if (!worldNormalOrZero(plane) || !worldNormalOrZero(origin)) return false;
    bool integers=abs(plane)<=4194304 && abs(origin)<=4194304 &&
        plane==floor(plane) && origin==floor(origin);
    bool sterbenz=(plane>0 && origin>0) || (plane<0 && origin<0);
    sterbenz=sterbenz && abs(plane)<=4194304 && abs(origin)<=4194304 &&
        abs(plane)<=2*abs(origin) && abs(origin)<=2*abs(plane);
    if (!(integers || sterbenz || plane==0 || origin==0 || plane==origin)) return false;
    offset=plane-origin;
    return worldNormalOrZero(offset) && (plane==origin || offset!=0);
}

bool worldFaceProduct(float a, float b, out float product) {
    product=0;
    if (!worldNormalOrZero(a) || !worldNormalOrZero(b)) return false;
    if (a==0 || b==0) return true;
    uint ma=asuint(a)&0x7fffffffu,mb=asuint(b)&0x7fffffffu;
    bool powerA=(ma&0x007fffffu)==0u,powerB=(mb&0x007fffffu)==0u;
    bool smallIntegers=abs(a)<=4096 && abs(b)<=4096 && a==floor(a) && b==floor(b);
    if (!(powerA || powerB || smallIntegers)) return false;
    // Away from the underflow boundary a normal finite power-of-two scale
    // is exact. At min-normal an inexact subnormal product may round UP to a
    // normal value (0x00ffffff * .5); conservatively reject that boundary too.
    // Bounded integer products are <=2^24 and exactly representable.
    product=a*b;
    uint magnitude=asuint(product)&0x7fffffffu;
    return worldNormalOrZero(product) && magnitude>0x00800000u;
}

bool worldClipSide(float2 origin, float2 direction, uint enteringAxis, float enteringFace,
                   uint otherAxis, float otherPlane, out int side) {
    side=0;
    float enteringOffset,otherOffset,left,right;
    if (!worldFaceOffset(enteringFace,origin[enteringAxis],enteringOffset) ||
        !worldFaceOffset(otherPlane,origin[otherAxis],otherOffset) ||
        !worldFaceProduct(enteringOffset,direction[otherAxis],left) ||
        !worldFaceProduct(otherOffset,direction[enteringAxis],right)) return false;
    side=(left>right?1:0)-(left<right?1:0);
    if (direction[enteringAxis]<0) side=-side;
    return true; // Compare exact products directly; no residual subtraction/FMA.
}

// ------------------------------------------------------------------
// native entered-face ownership shared by discovery and later lookup
// ------------------------------------------------------------------

bool worldTransmissiveCell(int2 cell) {
    if (!worldInside(cell)) return true; // Admitted source exterior is vacuum.
    uint status=0u;WorldCell value=worldCell(cell,status);
    return status==0u && value.coverage<1 && (value.material.kind==0u || value.material.kind==2u);
}

uint worldFaceKey(int2 hitCell,int2 predecessor,uint crossed,float2 direction,inout uint status) {
    if (crossed==0u) return 0xffffffffu;
    WorldCell hit=worldCell(hitCell,status);
    bool partial=hit.coverage>0 && hit.coverage<1;
    bool fromTransmissive=worldTransmissiveCell(predecessor);
    if (!fromTransmissive && !partial) return 0xffffffffu;

    int2 step=int2(direction.x>0?1:-1,direction.y>0?1:-1);
    bool exposedX=(crossed&1u)!=0u && worldTransmissiveCell(hitCell-int2(step.x,0));
    bool exposedY=(crossed&2u)!=0u && worldTransmissiveCell(hitCell-int2(0,step.y));
    uint axis=0u;
    int2 owner=hitCell;
    if (exposedX || exposedY) {
        axis=exposedX && (!exposedY || abs(direction.x)>=abs(direction.y))?0u:1u;
    } else if (crossed==3u && fromTransmissive) {
        // The diagonal opaque hit remains the DIRECT source/attenuation owner.
        // At a concave corner the first opaque axial neighbour owns reflection.
        axis=abs(direction.x)>=abs(direction.y)?0u:1u;
        owner=predecessor+(axis==0u?int2(step.x,0):int2(0,step.y));
        uint ownerStatus=0u;WorldCell hardOwner=hit;
        if (worldInside(owner)) hardOwner=worldCell(owner,ownerStatus);
        if (!worldInside(owner) || ownerStatus!=0u || hardOwner.coverage!=1) {
            status|=WORLD_INVALID;
            return 0xffffffffu;
        }
    } else if (partial) {
        // A nonempty exact boundary start owns this partial virtual face even
        // with hard backing. Strict partial interior starts have crossed==0.
        axis=(crossed&1u)!=0u && ((crossed&2u)==0u || abs(direction.x)>=abs(direction.y))?0u:1u;
    } else {
        status|=WORLD_INVALID;
        return 0xffffffffu;
    }

    uint side=2u*axis+(direction[axis]>0?0u:1u);
    return ((uint(owner.y)*g_world.width+uint(owner.x))<<2u)|side;
}

// A generation-local concrete table. Ordinal+1 reserves zero for empty; full
// key comparison prevents collisions from silently selecting another face.
float3 worldReflectedFace(uint key,inout uint status) {
    if (key==0xffffffffu) return float3(0); // Strict opaque start has no incident face.
    if (g_bounce.faceCount==0u || g_bounce.lookupCapacity==0u) {status|=WORLD_SOURCE;return float3(0);}

    uint slot=(key*2654435761u)&(g_bounce.lookupCapacity-1u);
    for (uint probe=0u;probe<g_bounce.lookupCapacity;++probe) {
        uint value=g_face_lookup[slot];
        if (value==0u || value>g_bounce.faceCount) {status|=WORLD_SOURCE;return float3(0);}
        if (g_face_keys[value-1u]==key) {
            uint cellIndex=key>>2u;
            int2 cell=int2(cellIndex%g_world.width,cellIndex/g_world.width);
            WorldCell resolved=worldCell(cell,status);
            if (status!=0u || !(resolved.coverage>0)) {status|=WORLD_SOURCE;return float3(0);}

            WorldMaterial owner=resolved.material;
            WorldRgb incident=g_face_moments[value-1u];
            float3 result=float3(owner.albedoR,owner.albedoG,owner.albedoB)*float3(incident.r,incident.g,incident.b);
            if (!worldFinite3(result) || any(result<0)) status|=WORLD_INVALID;
            return result;
        }
        slot=(slot+1u)&(g_bounce.lookupCapacity-1u);
    }
    status|=WORLD_SOURCE;
    return float3(0);
}

void worldDiscoverKey(uint key,inout uint status);

// ------------------------------------------------------------------
// previous completed world-field feedback, independent of image history
// ------------------------------------------------------------------

float3 worldPreviousReflection(uint key,WorldMaterial material,inout uint status) {
    if (g_temporal.feedbackAvailable==0u || key==0xffffffffu) return float3(0);

    uint cellIndex=key>>2u,side=key&3u;
    float2 normal=side==0u?float2(-1,0):side==1u?float2(1,0):side==2u?float2(0,-1):float2(0,1);
    float2 point=float2(cellIndex%g_world.width,cellIndex/g_world.width)+0.5+normal*0.5;
    float2 position=point/float(g_temporal.refinedSpacing)-0.5;
    int2 lower=int2(floor(position));
    float2 fraction=position-float2(lower);
    WorldCoefficients previous=(WorldCoefficients)0;
    for (uint sample=0u;sample<4u;++sample) {
        uint2 offset=uint2(sample&1u,sample>>1u);
        float weight=(offset.x!=0u?fraction.x:1-fraction.x)*(offset.y!=0u?fraction.y:1-fraction.y);
        if (weight==0) continue;

        int2 index=lower+int2(offset)-int2(g_world.refinedMinX,g_world.refinedMinY);
        if (any(index<0) || index.x>=int(g_world.refinedCountX) || index.y>=int(g_world.refinedCountY)) {
            status|=WORLD_PARENT;continue;
        }

        WorldCoefficients value=g_previous_field[uint(index.y)*g_world.refinedCountX+uint(index.x)];
        previous.status|=value.status;
        for (uint order=0u;order<9u;++order) {
            previous.coefficient[order].r+=value.coefficient[order].r*weight;
            previous.coefficient[order].g+=value.coefficient[order].g*weight;
            previous.coefficient[order].b+=value.coefficient[order].b*weight;
        }
    }

    uint responseStatus;
    float3 incident=worldDiffuseResponse(previous,float3(normal,0),responseStatus);
    status|=responseStatus;
    // Declared feedback loss bounds the high-albedo iterative approximation.
    // Material emission is added once by the caller, never fed through this gain.
    float3 reflected=max(incident,0)*float3(material.albedoR,material.albedoG,material.albedoB)*0.95;
    if (!worldFinite3(reflected)) {status|=WORLD_INVALID;return float3(0);}
    return reflected;
}

// A bounded environment lobe follows the solar hour around the 2D world.
// Source captures retain their authored constant boundary when no hour is supplied.
float3 worldTerminalSky(float2 direction) {
    float3 boundary=float3(g_world.boundaryX,g_world.boundaryY,g_world.boundaryZ);
    if (g_world.skyHourPlusOne==0u) return boundary;
    if (boundary.z==0.0) return 0.0;

    direction=normalize(direction);
    float cosine=saturate(dot(direction,boundary.xy));
    float square=cosine*cosine;
    float fourth=square*square;
    float eighth=fourth*fourth;
    return boundary.z*lerp(float3(0.2,0.5,1.0)*(0.5+0.5*saturate(direction.y)),
        float3(1,0.75,0.4),eighth*eighth);
}

WorldInterval worldTraceHit(float2 origin, float2 direction, float nearValue, float farValue,
                           bool terminal, inout WorldWork work,out uint faceKey,bool secondary=false,
                           bool absorbingBoundary=false,bool collectFaces=false,bool feedback=false,bool qualifySkip=false) {
    faceKey=0xffffffffu;
    ++work.queries;
    WorldInterval result = worldIdentity();
    if (!worldNormalOrZero(origin.x) || !worldNormalOrZero(origin.y) ||
        !worldNormalOrZero(direction.x) || !worldNormalOrZero(direction.y) ||
        !worldNormalOrZero(nearValue) || !worldNormalOrZero(farValue) ||
        nearValue < 0 || farValue < nearValue || all(direction == 0)) {
        result.status = WORLD_INVALID;
        ++work.failed;
        return result;
    }

    float entry = -WORLD_INF, exit = WORLD_INF;
    bool intersects = true;
    uint enteringAxis=0u,entryMask=0u;
    float2 enteringFace=0;
    float2 bounds = float2(g_world.width, g_world.height);
    bool stopAtWorld=secondary || absorbingBoundary;
    if (stopAtWorld) {
        float2 intervalOrigin=origin+direction*nearValue;
        if (!worldFinite3(float3(intervalOrigin,0))) {
            result.status=WORLD_INVALID;++work.failed;return result;
        }

        bool outside=any(intervalOrigin<0) || any(intervalOrigin>bounds);
        if (!outside) {
            int2 forward=int2(worldForwardCell(intervalOrigin.x,direction.x),worldForwardCell(intervalOrigin.y,direction.y));
            outside=!worldInside(forward);
        }

        if (outside) {
            // Reflected intervals cannot leave the black exterior and later
            // reenter around an opaque partition. Exact boundary starts use
            // the SAME native ray-forward cell ownership as interior traversal.
            result.tr=0;result.tg=0;result.tb=0;
            ++work.completed;
            return result;
        }
    }

    for (uint axis=0; axis<2; ++axis) {
        if (direction[axis] == 0) {
            if (origin[axis] < 0 || origin[axis] >= bounds[axis]) intersects = false;
        } else {
            float a = -origin[axis] / direction[axis];
            float b = (bounds[axis] - origin[axis]) / direction[axis];
            float candidate=min(a,b);
            enteringFace[axis]=direction[axis]>0?0:bounds[axis];
            if (candidate>entry) {entry=candidate;enteringAxis=axis;entryMask=1u<<axis;}
            else if (candidate==entry) entryMask|=1u<<axis;
            exit = min(exit, max(a,b));
        }
    }
    float cursor = max(nearValue, entry);
    float stop = min(farValue, exit);
    if (!intersects || !(cursor < stop)) {
        // An ordinary finite miss is vacuum identity. Only a declared terminal
        // query may close the world ray with its environment boundary value.
        if (terminal && intersects && entry < exit && exit > nearValue && farValue < exit) {
            result.status=WORLD_PROGRESS; // A too-short terminal interval is NOT escape.
            ++work.failed;
            return result;
        }
        if (terminal) {
            float3 sky=secondary?float3(0):worldTerminalSky(direction);
            result.r=sky.r; result.g=sky.g; result.b=sky.b;
            result.tr=0; result.tg=0; result.tb=0;
        }
        ++work.completed;
        return result;
    }

    // Equal rounded entry quotients do not prove a corner or select a face.
    // Resolve only the bounded exact cross-event case; otherwise fail visibly.
    if (entry>=nearValue && entryMask==3u) {
        uint other=1u-enteringAxis;int side;
        if (!worldClipSide(origin,direction,enteringAxis,enteringFace[enteringAxis],other,enteringFace[other],side)) {
            result.status=WORLD_PROGRESS;++work.failed;return result;
        }
        if (side*(direction[other]>0?1:-1)<0) enteringAxis=other;
        entryMask=side==0?3u:1u<<enteringAxis;
    }
    if (entry==nearValue) {
        float delta,offset;
        if (!worldFaceProduct(direction[enteringAxis],nearValue,delta) ||
            !worldFaceOffset(enteringFace[enteringAxis],origin[enteringAxis],offset) || delta!=offset) {
            result.status=WORLD_PROGRESS;++work.failed;return result;
        }
    }

    float2 start = origin + direction * cursor;
    int2 cell;
    uint crossed=0u;
    bool clipped=entry>nearValue;
    for (uint axis=0u;axis<2u;++axis) {
        if (direction[axis]==0 || (!clipped && cursor==0)) {
            cell[axis]=worldForwardCell(origin[axis],direction[axis]);
            if (direction[axis]!=0 && origin[axis]==floor(origin[axis])) crossed|=1u<<axis;
            continue;
        }
        if (clipped && axis==enteringAxis) {
            cell[axis]=direction[axis]>0?0:int(bounds[axis])-1;
            crossed|=entryMask;
            continue;
        }
        float plane=floor(start[axis]);
        cell[axis]=worldForwardCell(start[axis],direction[axis]);
        if (start[axis]!=plane) continue;

        int side=0;
        if (clipped) {
            if (!worldClipSide(origin,direction,enteringAxis,enteringFace[enteringAxis],axis,plane,side)) {
                result.status=WORLD_PROGRESS;++work.failed;return result;
            }
        } else {
            float delta,offset;
            if (!worldFaceProduct(direction[axis],cursor,delta) || !worldFaceOffset(plane,origin[axis],offset)) {
                result.status=WORLD_PROGRESS;++work.failed;return result;
            }
            side=(delta>offset?1:0)-(delta<offset?1:0);
        }
        cell[axis]=side<0?int(plane)-1:(side>0?int(plane):worldForwardCell(plane,direction[axis]));
        if (side==0) crossed|=1u<<axis;
    }
    int2 predecessor=cell-int2((crossed&1u)!=0u?(direction.x>0?1:-1):0,
        (crossed&2u)!=0u?(direction.y>0?1:-1):0);
    bool reached = false,opaqueHit=false;
    float directionLength=length(direction);
    if (!(g_world.cellMetres>0) || !isfinite(g_world.cellMetres) ||
        !(directionLength>0) || !isfinite(directionLength)) {result.status=WORLD_INVALID;++work.failed;return result;}

    for (uint visit=0; visit<g_world.maxSegments; ++visit) {
        if (!worldInside(cell)) { result.status |= WORLD_PROGRESS; break; }
        ++result.segments;
        ++work.segments;
        WorldCell resolved=worldCell(cell,result.status);
        if (result.status!=0u) break;

        WorldMaterial material=resolved.material;
        float coverage=resolved.coverage;
        // Strict partial starts have no incoming surface. Unity fill retains
        // the existing full-opaque primary S/T0 and secondary black/T0 rule.
        if (coverage==1 || (coverage>0 && crossed!=0u)) {
            faceKey=worldFaceKey(cell,predecessor,crossed,direction,result.status);
            if (faceKey!=0xffffffffu) {
                // The existing concave HARD corner is one complete surface;
                // never mix partial entered-cell C/S with axial hard-owner A.
                uint ownerIndex=faceKey>>2u;
                int2 owner=int2(ownerIndex%g_world.width,ownerIndex/g_world.width);
                if (coverage<1 && any(owner!=cell)) {
                    WorldCell hard=worldCell(owner,result.status);
                    if (hard.coverage!=1) result.status|=WORLD_SOURCE;
                    coverage=hard.coverage;material=hard.material;
                }

                if (collectFaces) worldDiscoverKey(faceKey,result.status);
            }

            WorldInterval surface=worldIdentity();
            float3 reflected=secondary?worldReflectedFace(faceKey,result.status):float3(material.r,material.g,material.b);
            if (feedback) reflected+=worldPreviousReflection(faceKey,material,result.status);
            float3 source=worldMultiply(float3(coverage),reflected,result.status);
            surface.r=source.x;surface.g=source.y;surface.b=source.z;
            surface.tr=1-coverage;surface.tg=1-coverage;surface.tb=1-coverage;
            result=worldCompose(result,surface);
            if (result.status!=0u) break;
            if (coverage==1) {
                reached=true;opaqueHit=true;
                break;
            }
        }
        if (material.kind != 0u && material.kind != 2u) { result.status |= WORLD_SOURCE; break; }

        int2 blockMin=cell, blockMax=cell+1;
        if ((qualifySkip || (feedback && g_temporal.skipTable!=0u)) && material.kind==0u) {
            uint size=g_skip_table.Load(int3(cell,0));
            if (size>1u) {
                blockMin=(cell/int(size))*int(size);
                blockMax=blockMin+int(size);
            }
        } else for (uint level=1u; material.kind==0u && level<g_world.hierarchyLevels; ++level) {
            WorldHierarchyLevel h = g_hierarchy[level];
            uint2 index=uint2(cell)/h.blockSize;
            if (index.x>=h.width || index.y>=h.height) { result.status |= WORLD_PROGRESS; break; }
            uint nativeIndex=h.offset+index.y*h.width+index.x;
            uint ancestorIndex=nativeIndex-g_world.width*g_world.height;
            bool overlayVacuum=g_world.overlayAncestorWords==0u ||
                (ancestorIndex/32u<g_world.overlayAncestorWords &&
                 (g_overlay_ancestors[ancestorIndex/32u]&(1u<<(ancestorIndex&31u)))==0u);
            if (g_vacuum[nativeIndex] != 0u && overlayVacuum) {
                blockMin=int2(index*h.blockSize);
                blockMax=blockMin+int(h.blockSize);
            }
        }
        if (result.status != 0u) break;
        if (any(blockMax-blockMin > 1)) ++work.skipped;

        float2 event=float2(WORLD_INF,WORLD_INF);
        for (uint axis=0; axis<2; ++axis) {
            if (direction[axis] != 0)
                event[axis]=(float(direction[axis]>0 ? blockMax[axis] : blockMin[axis])-origin[axis])/direction[axis];
        }
        float next=min(stop,min(event.x,event.y));
        if (!isfinite(next) || !(next > cursor)) { result.status |= WORLD_PROGRESS; break; }
        if (material.kind==2u) {
            float metres=(next-cursor)*directionLength*g_world.cellMetres;
            WorldInterval local=worldIdentity();
            float3 sigma=float3(material.sigmaR,material.sigmaG,material.sigmaB);
            float3 emission=secondary?float3(0):float3(material.jR,material.jG,material.jB);
            float3 T=0,L=0;
            if (!isfinite(metres) || !(metres>0) || !worldFinite3(sigma) || any(sigma<0) ||
                !worldFinite3(emission) || any(emission<0)) local.status|=WORLD_INVALID;
            for (uint channel=0u;channel<3u;++channel) {
                float depth=sigma[channel]*metres;
                if (!isfinite(depth) || depth<0) {local.status|=WORLD_INVALID;continue;}

                T[channel]=exp(-depth);
                // Canonical sourceIntegral from radiance_cascades.slang:
                // stable weak-depth polynomial, including sigma==0.
                if (depth<0.001) {
                    float integral=metres*(1-depth*(0.5-depth*(1.0/6.0-depth/24.0)));
                    L[channel]=emission[channel]*integral;
                } else {
                    float absorbed=1-T[channel];
                    if (emission[channel]==0) { L[channel]=0;continue; }

                    // Source admission permits zero or positive NORMAL inputs.
                    // Divide only reconstructed [1,2) significands, so neither
                    // reciprocal nor equilibrium requires a subnormal operand.
                    uint sigmaBits=asuint(sigma[channel]),jBits=asuint(emission[channel]);
                    uint sigmaExponent=(sigmaBits>>23u)&255u,jExponent=(jBits>>23u)&255u;
                    if (sigmaExponent==0u || sigmaExponent==255u || jExponent==0u || jExponent==255u) {
                        local.status|=WORLD_INVALID;
                        continue;
                    }

                    float sigmaSignificand=asfloat((sigmaBits&0x007fffffu)|0x3f800000u);
                    float jSignificand=asfloat((jBits&0x007fffffu)|0x3f800000u);
                    float weighted=(jSignificand/sigmaSignificand)*absorbed;
                    int exponent=int(jExponent)-int(sigmaExponent);

                    // Exponent difference lies in [-253,253]. Each normal
                    // power-of-two chunk moves monotonically toward the final
                    // positive value: a representable result cannot overflow
                    // an intermediate. FTZ during decreasing chunks implies
                    // subnormal final radiance, covered by the already admitted
                    // absolute local-radiance loss bound.
                    for (uint chunk=0u;chunk<2u;++chunk) {
                        if (exponent>127) {
                            weighted*=asfloat(254u<<23u);
                            exponent-=127;
                        } else if (exponent< -126) {
                            weighted*=asfloat(1u<<23u);
                            exponent+=126;
                        }
                    }

                    L[channel]=weighted*asfloat(uint(exponent+127)<<23u);
                }
            }

            local.r=L.x;local.g=L.y;local.b=L.z;
            local.tr=T.x;local.tg=T.y;local.tb=T.z;
            result=worldCompose(result,local);
            if (result.status!=0u) break;
        }

        if (next >= stop) { reached=true; cursor=stop; break; }

        // Advance crossed axes by INTEGER block faces, including both exact
        // corner ties. Never refloor a rounded negative-face point back into
        // the block just left. Noncrossed coordinates use ray-forward ownership.
        float2 point=origin+direction*next;
        cell.x=event.x <= event.y ? (direction.x>0 ? blockMax.x : blockMin.x-1)
                                    : worldForwardCell(point.x,direction.x);
        cell.y=event.y <= event.x ? (direction.y>0 ? blockMax.y : blockMin.y-1)
                                    : worldForwardCell(point.y,direction.y);
        crossed=(event.x<=event.y?1u:0u)|(event.y<=event.x?2u:0u);
        // Native plane events own BOTH direct transport and discovered faces.
        // Reconstruction can round to either adjacent cell: compare that cell's
        // entry AND exit plane times before the next material/opacity lookup.
        for (uint axis=0u;axis<2u;++axis) {
            if ((crossed&(1u<<axis))!=0u || direction[axis]==0) continue;

            float entryPlane=float(direction[axis]>0?cell[axis]:cell[axis]+1);
            float exitPlane=float(direction[axis]>0?cell[axis]+1:cell[axis]);
            float entryEvent=(entryPlane-origin[axis])/direction[axis];
            float exitEvent=(exitPlane-origin[axis])/direction[axis];
            if (entryEvent==next) {
                cell[axis]=worldForwardCell(entryPlane,direction[axis]);
                crossed|=1u<<axis;
            } else if (exitEvent==next) {
                cell[axis]=worldForwardCell(exitPlane,direction[axis]);
                crossed|=1u<<axis;
            }
        }

        predecessor=cell-int2((crossed&1u)!=0u?(direction.x>0?1:-1):0,
            (crossed&2u)!=0u?(direction.y>0?1:-1):0);
        cursor=next;
    }
    if (!reached && result.status==0u) result.status |= WORLD_PROGRESS;
    if (stopAtWorld && reached && !opaqueHit && stop==exit) {
        // The first physical exit terminates even a finite connector. Preserve
        // any accumulated L; no parent beyond the boundary may continue it.
        result.tr=0;result.tg=0;result.tb=0;
    }
    if (terminal && reached && !opaqueHit) {
        if (farValue < exit) result.status |= WORLD_PROGRESS; // Even attenuated paths retain actual escape admission.
        else {
            WorldInterval sky=worldIdentity();
            float3 incoming=secondary?float3(0):worldTerminalSky(direction);
            sky.r=incoming.r;sky.g=incoming.g;sky.b=incoming.b;
            sky.tr=0;sky.tg=0;sky.tb=0;
            result=worldCompose(result,sky);
        }
    }
    if (!worldFinite3(worldL(result)) || any(worldL(result)<0) ||
        !worldFinite3(worldT(result)) || any(worldT(result)<0) || any(worldT(result)>1)) result.status |= WORLD_INVALID;
    if (result.status==0u) ++work.completed; else ++work.failed;
    return result;
}

WorldInterval worldTraceRaw(float2 origin,float2 direction,float nearValue,float farValue,
    bool terminal,inout WorldWork work,bool secondary=false,bool feedback=false) {
    uint ignored;
    return worldTraceHit(origin,direction,nearValue,farValue,terminal,work,ignored,secondary,false,false,feedback);
}

WorldInterval worldConnectorHit(float2 from, float2 to, inout WorldWork work,out uint faceKey,bool secondary=false,
    bool absorbingBoundary=false,bool collectFaces=false,bool feedback=false) {
    faceKey=0xffffffffu;
    if (!worldNormalOrZero(from.x) || !worldNormalOrZero(from.y) ||
        !worldNormalOrZero(to.x) || !worldNormalOrZero(to.y)) {
        WorldInterval failed=worldIdentity(); failed.status=WORLD_INVALID;
        ++work.queries; ++work.failed;
        return failed;
    }
    if (all(from==to)) return worldIdentity(); // Only finite, proved equal encoded endpoints.
    float2 delta=to-from;
    bool collapsed=false;
    for (uint axis=0u;axis<2u;++axis) {
        collapsed=collapsed || !worldNormalOrZero(delta[axis]) ||
            (from[axis]!=to[axis] && (asuint(delta[axis])&0x7fffffffu)==0u);
    }
    float lengthSquared=dot(delta,delta);
    if (collapsed || !(lengthSquared>0) || !isfinite(lengthSquared)) {
        WorldInterval failed=worldIdentity(); failed.status=WORLD_INVALID;
        ++work.queries; ++work.failed;
        return failed;
    }
    float distance=sqrt(lengthSquared);
    return worldTraceHit(from,delta/distance,0,distance,false,work,faceKey,secondary,absorbingBoundary,collectFaces,feedback);
}

WorldInterval worldConnector(float2 from,float2 to,inout WorldWork work,bool secondary=false,bool feedback=false) {
    uint ignored;
    return worldConnectorHit(from,to,work,ignored,secondary,false,false,feedback);
}

float2 worldProbe(WorldLevel level, int2 index) { return (float2(index)+0.5)*float(level.spacing); }

bool worldRecordIndex(WorldLevel level, int2 probe, uint direction, out uint index) {
    int2 local=probe-int2(level.minX,level.minY);
    index=0u;
    if (any(local<0) || local.x>=int(level.countX) || local.y>=int(level.countY) || direction>=level.directions) return false;
    index=(uint(local.y)*level.countX+uint(local.x))*level.directions+direction;
    return index<level.records;
}

// One dispatch per level, coarsest to C1, with a UAV barrier and disjoint
// parity SRV/UAV views between levels. Parent field phase is generation-owned.
// ------------------------------------------------------------------
// shared cascade/refinement query geometry, independent of radiance values
// ------------------------------------------------------------------

void worldParentSample(float2 point,WorldLevel parent,uint sample,out int2 k,out float weight) {
    float2 relative=point/float(parent.spacing)-0.5;
    int2 lo=int2(floor(relative)),hi=int2(ceil(relative));
    float2 fraction=relative-float2(lo);
    k=int2((sample&1u)!=0u?hi.x:lo.x,(sample&2u)!=0u?hi.y:lo.y);
    weight=((sample&1u)!=0u?fraction.x:1-fraction.x)*((sample&2u)!=0u?fraction.y:1-fraction.y);
}

float2 worldIntervalOrigin(float2 p,float2 direction,WorldLevel level) {
    return p+direction*level.nearCells;
}

// Four adjacent angular children share the endpoint of their incoming
// connector. Start each child exactly there before it fans out. A first-level
// record connects to refinement at its own angular midpoint.
float2 worldTemporalStart(float2 p,float2 d,WorldLevel level,uint directionIndex) {
    if (g_dispatch.level>1u) {
        WorldLevel lower=g_levels[g_dispatch.level-1u];
        d=g_directions[lower.directionOffset+directionIndex/4u];
    }
    return worldIntervalOrigin(p,d,level);
}

bool worldBounceCascadeGeometry(float2 p,float2 d,WorldLevel level,WorldLevel parent,int2 k,uint child,
    out float2 from,out float2 to,out uint parentIndex) {
    from=worldIntervalOrigin(p,d,level);
    to=worldIntervalOrigin(worldProbe(parent,k),g_directions[parent.directionOffset+child],parent);
    return worldRecordIndex(parent,k,child,parentIndex);
}

bool worldRefinementGeometry(float2 q,WorldLevel parent,int2 k,uint phi,
    out float2 endpoint,out float2 actual,out uint parentIndex,bool endpointConnected=false,float footprint=4) {
    float2 p=worldProbe(parent,k);
    float2 d=g_directions[parent.directionOffset+phi];
    float exit=1;
    if (endpointConnected) endpoint=worldIntervalOrigin(p,d,parent);
    else {
        float2 distances=float2(WORLD_INF,WORLD_INF);
        for (uint axis=0;axis<2u;++axis) {
            if (d[axis]!=0) distances[axis]=((q[axis]+(d[axis]>0?footprint:-footprint))-p[axis])/d[axis];
        }

        exit=min(distances.x,distances.y);
        endpoint=p+d*exit;
    }

    float2 delta=endpoint-q;
    float square=dot(delta,delta);
    actual=0;parentIndex=0;
    if (!(exit>0) || !isfinite(exit) || !(square>0) || !isfinite(square) ||
        !worldRecordIndex(parent,k,phi,parentIndex)) return false;

    actual=delta/sqrt(square);
    return true;
}

void worldSolveCascade(uint3 tid,uint lane,uint first,bool secondary,bool temporal=false) {
    worldBeginGroup(lane);
    WorldWork work={}; uint status=0u; bool wrote=false;
    bool queued=temporal && g_temporal.workQueue!=0u;
    uint count=queued?g_temporal_work[0]:g_dispatch.count;
    if (tid.x<count) {
        uint record=queued?g_temporal_work[tid.x+1u]:tid.x+first;
        WorldLevel level=g_levels[g_dispatch.level];
        uint directionIndex=record%level.directions;
        uint spatial=record/level.directions;
        int2 probe=int2(int(spatial%level.countX)+level.minX,int(spatial/level.countX)+level.minY);
        if (temporal && !queued) {
            uint2 offset=uint2(g_temporal.phase&1u,(g_temporal.phase>>1u)&1u);
            uint2 selected=uint2(spatial%g_temporal.phaseWidth,spatial/g_temporal.phaseWidth)*2u+offset;
            selected+=uint2(g_temporal.regionOffset%level.countX,g_temporal.regionOffset/level.countX);
            probe=int2(selected)+int2(level.minX,level.minY);
            record=(selected.y*level.countX+selected.x)*level.directions+directionIndex;
        }
        float2 p=worldProbe(level,probe);
        float2 d=g_directions[level.directionOffset+directionIndex];
        WorldInterval result=worldIdentity();
        if (g_dispatch.level==g_dispatch.parentLevel) {
            if (secondary)
                result=worldTraceRaw(worldIntervalOrigin(p,d,level),d,0,level.farCells-level.nearCells,true,work,true);
            else if (temporal)
                result=worldTraceRaw(worldTemporalStart(p,d,level,directionIndex),d,0,level.farCells-level.nearCells,true,work,false,true);
            else result=worldTraceRaw(p,d,level.nearCells,level.farCells,true,work);
        } else {
            WorldLevel parent=g_levels[g_dispatch.parentLevel];
            float3 L=0,T=0;
            for (uint sample=0; sample<4u; ++sample) {
                int2 k;float weight;
                worldParentSample(p,parent,sample,k,weight);
                if (weight==0) continue; // No zero-weight out-of-range read.

                WorldInterval local=worldIdentity();
                if (!secondary) {
                    float2 from=temporal ? worldTemporalStart(p,d,level,directionIndex) : worldIntervalOrigin(p,d,level);
                    float2 to=worldIntervalOrigin(worldProbe(parent,k),d,parent);
                    local=worldConnector(from,to,work,false,temporal);
                }

                for (uint child=0; child<4u; ++child) {
                    uint parentIndex;
                    if (secondary) {
                        float2 from,to;
                        if (!worldBounceCascadeGeometry(p,d,level,parent,k,4u*directionIndex+child,from,to,parentIndex)) {
                            result.status|=WORLD_PARENT;continue;
                        }

                        local=worldConnector(from,to,work,true);
                    } else if (!worldRecordIndex(parent,k,4u*directionIndex+child,parentIndex)) {
                        result.status|=WORLD_PARENT;continue;
                    }

                    WorldInterval composed=worldCompose(local,g_parent[parentIndex]);
                    result.status|=composed.status;
                    L+=worldMultiply(worldL(composed),float3(weight*0.25),result.status);
                    T+=worldMultiply(worldT(composed),float3(weight*0.25),result.status);
                }
            }
            result.r=L.x;result.g=L.y;result.b=L.z;
            result.tr=T.x;result.tg=T.y;result.tb=T.z;
            if (!worldFinite3(L) || !worldFinite3(T) || any(L<0) || any(T<0) || any(T>1)) result.status|=WORLD_INVALID;
        }
        g_current[record]=result;status=result.status;wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void solveWorldCascade(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldSolveCascade(tid,lane,0u,false);
}

[shader("compute")]
[numthreads(64,1,1)]
void solveWorldTemporalCascade(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldSolveCascade(tid,lane,g_temporal.firstRecord,false,true);
}

// Four adjacent lanes trace one record's connectors. The owner still folds all
// sixteen parent contributions in sample/child order, identical to the control.
groupshared WorldInterval worldCascadeConnectors[64];
groupshared int2 worldCascadeParents[64];
groupshared float worldCascadeWeights[64];

[shader("compute")]
[numthreads(64,1,1)]
void solveWorldTemporalCascadeParallel(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    uint ordinal=group.x*16u+lane/4u;
    bool queued=g_temporal.workQueue!=0u;
    uint count=queued?g_temporal_work[0]:g_dispatch.count;
    bool active=ordinal<count;
    WorldLevel level=g_levels[g_dispatch.level];
    WorldLevel parent=g_levels[g_dispatch.parentLevel];
    uint record=0u,directionIndex=0u;
    int2 k=0;float weight=0;
    WorldInterval local=worldIdentity();
    if (active) {
        record=queued?g_temporal_work[ordinal+1u]:ordinal+g_temporal.firstRecord;
        directionIndex=record%level.directions;
        uint spatial=record/level.directions;
        int2 probe=int2(int(spatial%level.countX)+level.minX,int(spatial/level.countX)+level.minY);
        if (!queued) {
            uint2 offset=uint2(g_temporal.phase&1u,(g_temporal.phase>>1u)&1u);
            uint2 selected=uint2(spatial%g_temporal.phaseWidth,spatial/g_temporal.phaseWidth)*2u+offset;
            selected+=uint2(g_temporal.regionOffset%level.countX,g_temporal.regionOffset/level.countX);
            probe=int2(selected)+int2(level.minX,level.minY);
            record=(selected.y*level.countX+selected.x)*level.directions+directionIndex;
        }
        float2 p=worldProbe(level,probe);
        float2 d=g_directions[level.directionOffset+directionIndex];
        worldParentSample(p,parent,lane%4u,k,weight);
        if (weight!=0) {
            float2 from=worldTemporalStart(p,d,level,directionIndex);
            float2 to=worldIntervalOrigin(worldProbe(parent,k),d,parent);
            local=worldConnector(from,to,work,false,true);
        }
    }

    worldCascadeConnectors[lane]=local;
    worldCascadeParents[lane]=k;
    worldCascadeWeights[lane]=weight;
    GroupMemoryBarrierWithGroupSync();

    if (active && lane%4u==0u) {
        WorldInterval result=worldIdentity();
        float3 L=0,T=0;
        for (uint sample=0u;sample<4u;++sample) {
            uint slot=lane+sample;
            float w=worldCascadeWeights[slot];
            if (w==0) continue;

            WorldInterval connector=worldCascadeConnectors[slot];
            for (uint child=0u;child<4u;++child) {
                uint parentIndex;
                if (!worldRecordIndex(parent,worldCascadeParents[slot],4u*directionIndex+child,parentIndex)) {
                    result.status|=WORLD_PARENT;continue;
                }
                WorldInterval composed=worldCompose(connector,g_parent[parentIndex]);
                result.status|=composed.status;
                L+=worldMultiply(worldL(composed),float3(w*0.25),result.status);
                T+=worldMultiply(worldT(composed),float3(w*0.25),result.status);
            }
        }
        result.r=L.x;result.g=L.y;result.b=L.z;
        result.tr=T.x;result.tg=T.y;result.tb=T.z;
        if (!worldFinite3(L) || !worldFinite3(T) || any(L<0) || any(T<0) || any(T>1)) result.status|=WORLD_INVALID;
        g_current[record]=result;status=result.status;wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void solveWorldBounceCascade(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldSolveCascade(tid,lane,g_bounce.firstRecord,true);
}

// Primary diagnostics retain the +/-4 C1 footprint. Reflected refinement uses
// exact selected C1 interval origins, with actual connector directions for CH.
// In the cascade branch every C1 parent lies strictly inside this box. Its ray exits,
// and a NEW local connector from q reaches that exit before far composition.
void worldAdmitCoefficients(WorldCoefficients output,inout uint status) {
    // K16<=1+pi/2<3. Precheck finite magnitudes BEFORE adding or
    // multiplying: overflow may saturate on a permitted rounding mode.
    // Nine coefficients each <=FLT_MAX/256 give H<=9*FLT_MAX/256.
    // S<=FLT_MAX/8 makes 8H+2S<=.532*FLT_MAX, leaving ample FP32
    // rounding headroom. Grid reflectance is bounded by1. This is a
    // conservative reference-domain rejection, never a clamp/fallback.
    const float coefficientLimit=asfloat(0x7b7fffffu); // FLT_MAX/256
    const float sourceLimit=asfloat(0x7dffffffu); // FLT_MAX/8
    float3 source=float3(g_world.maxSourceR,g_world.maxSourceG,g_world.maxSourceB);
    bool bounded=worldFinite3(source) && all(source<=sourceLimit);
    float3 magnitude=0;
    for (uint coefficient=0u;coefficient<9u;++coefficient) {
        float3 term=abs(worldCoefficient(output,coefficient));
        bool valid=worldFinite3(term) && all(term<=coefficientLimit);
        bounded=bounded && valid;
        if (valid) magnitude+=term;
    }
    if (!bounded) status|=WORLD_INVALID;
    else {
        // The loose bound also covers <=1 texture/tint reflectance,
        // convex spatial/background mixing and separate surface S.
        // A requested half-format ceiling is an admission limit only.
        float3 responseBound=8*magnitude+2*source;
        if (!worldFinite3(responseBound) || any(responseBound>g_world.maximumResponse)) status|=WORLD_INVALID;
    }
}

// ------------------------------------------------------------------
// ordered direct and cascade coefficient projection
// ------------------------------------------------------------------

struct WorldProjection {
    float3 sum;float total;
    float3 projected[8];float normalization[8];
    float3 first;bool common,haveFirst;
};

WorldProjection worldProjectionIdentity() {
    WorldProjection projection;
    projection.sum=0;projection.total=0;
    for (uint n=0;n<8u;++n) {projection.projected[n]=0;projection.normalization[n]=0;}

    projection.common=true;projection.haveFirst=false;projection.first=0;
    return projection;
}

void worldObserveSample(inout WorldProjection projection,float3 L) {
    if (!projection.haveFirst) {projection.first=L;projection.haveFirst=true;}
    else projection.common=projection.common && all(asuint(L)==asuint(projection.first));
}

void worldProjectSample(inout WorldProjection projection,float3 L,float angle,float w,inout uint status) {
    projection.sum+=worldMultiply(L,float3(w),status);projection.total+=w;

    for (uint order=1u;order<=4u;++order) {
        float c=cos(float(order)*angle),s=sin(float(order)*angle);
        projection.projected[2u*order-2u]+=L*(w*c);projection.normalization[2u*order-2u]+=w*c;
        projection.projected[2u*order-1u]+=L*(w*s);projection.normalization[2u*order-1u]+=w*s;
    }
}

WorldCoefficients worldProjectionResult(WorldProjection projection,inout uint status) {
    WorldCoefficients output;
    float3 average=projection.common && projection.haveFirst ? projection.first :
                   (projection.total>0 ? projection.sum/projection.total : float3(0));
    if (!(projection.total>0) || !isfinite(projection.total) || !worldFinite3(average)) status|=WORLD_INVALID;

    output.coefficient[0].r=average.x;output.coefficient[0].g=average.y;output.coefficient[0].b=average.z;

    for (uint order=1;order<=4u;++order) {
        float x=float(order)*WORLD_PI/5;
        float window=sin(x)/x;

        for (uint component=0;component<2u;++component) {
            uint slot=2u*order-2u+component;
            float3 value=projection.common || !(projection.total>0) ? float3(0) :
                         2*window*(projection.projected[slot]-average*projection.normalization[slot])/projection.total;
            if (!worldFinite3(value)) status|=WORLD_INVALID;

            output.coefficient[slot+1u].r=value.x;output.coefficient[slot+1u].g=value.y;output.coefficient[slot+1u].b=value.z;
        }
    }

    return output;
}

// A bounded summary certifies native demand support without CPU per-probe readback.
[shader("compute")]
[numthreads(8,8,1)]
void checkWorldTemporalCoverage(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    if (tid.x<g_dispatch.receiverCountX && tid.y<g_dispatch.receiverCountY) {
        int2 probe=int2(g_dispatch.receiverMinX,g_dispatch.receiverMinY)+int2(tid.xy);
        uint index=uint(probe.y-g_world.refinedMinY)*g_world.refinedCountX+uint(probe.x-g_world.refinedMinX);
        WorldProbeState state=g_previous_probe_state[index];
        if (state.samples==0u || any(state.sourceVersion!=g_temporal.sourceVersion))
            InterlockedAdd(worldGroupCounts[0],1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane==0u && worldGroupCounts[0]!=0u)
        InterlockedAdd(g_counts[0],worldGroupCounts[0]);
}

// Retention is specified per 60 Hz interval. Subtract the split clock words
// before converting to float so a short interval survives a long-running world.
float worldTemporalRetention(uint2 previousTimeNs, float retentionPerFrame) {
    if (retentionPerFrame == 0.0) return 0.0;

    uint2 currentTimeNs = g_temporal.sampleTimeNs;
    uint low = currentTimeNs.x - previousTimeNs.x;
    uint high = currentTimeNs.y - previousTimeNs.y - uint(currentTimeNs.x < previousTimeNs.x);
    if ((high | low) == 0u) return 1.0;

    float intervals = (float(high) * 4294967296.0 + float(low)) * (60.0 / 1.0e9);
    return pow(retentionPerFrame, intervals);
}

// Per-probe transport validity and temporal change are independent of old-field
// feedback. Unknown retained probes keep version/sample count zero.
void worldRecordTemporalState(uint index,WorldCoefficients output,uint status) {
    WorldProbeState state={};
    if (status==0u) {
        state.sourceVersion=g_temporal.sourceVersion;
        state.sampleTimeNs=g_temporal.sampleTimeNs;
        state.samples=1u;
        state.variation=1.0;
        if (g_temporal.feedbackAvailable!=0u) {
            WorldProbeState previous=g_previous_probe_state[index];
            if (previous.samples!=0u) {
                bool sameSource=all(previous.sourceVersion==g_temporal.sourceVersion);
                state.samples=sameSource?min(previous.samples,0xfffffffeu)+1u:1u;
                WorldCoefficients old=g_previous_field[index];
                float change=0;
                for (uint coefficient=0u;coefficient<9u;++coefficient) {
                    float3 current=worldCoefficient(output,coefficient),prior=worldCoefficient(old,coefficient);
                    // Scale before subtraction to avoid overflow for finite extreme HDR values.
                    float3 scale=max(float3(0.01),max(abs(current),abs(prior)));
                    float3 difference=abs(current/scale-prior/scale);
                    change=max(change,max(difference.x,max(difference.y,difference.z)));
                }
                float squared=change*change;
                float retention = sameSource ? worldTemporalRetention(previous.sampleTimeNs, 0.5) : 0.0;
                state.variation = retention == 1.0 ? previous.variation : lerp(squared, previous.variation, retention);
            }
        }
    }
    g_probe_state[index]=state;
}

float worldTemporalHistoryRetention(uint index) {
    if (g_temporal.feedbackAvailable == 0u) return 0.0;

    WorldProbeState previous = g_previous_probe_state[index];
    if (previous.samples == 0u || any(previous.sourceVersion != g_temporal.sourceVersion)) return 0.0;
    return worldTemporalRetention(previous.sampleTimeNs, g_temporal.historyWeight);
}

void worldRefine(uint3 tid,uint lane,uint firstRecord,bool secondary,bool temporal=false) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;

    bool queued=temporal && g_temporal.workQueue!=0u;
    uint count=queued?g_temporal_work[0]:g_dispatch.count;
    if (tid.x<count) {
        uint record=queued?g_temporal_work[tid.x+1u]:tid.x+firstRecord;
        bool windowed=g_dispatch.level==2u;
        uint countX=windowed?g_dispatch.receiverCountX:g_world.refinedCountX;
        int2 index=windowed ? int2(g_dispatch.receiverMinX,g_dispatch.receiverMinY)+
                                 int2(int(record%countX),int(record/countX))
                           : int2(int(record%countX)+g_world.refinedMinX,
                                  int(record/countX)+g_world.refinedMinY);
        if (temporal && !queued) {
            uint2 offset=uint2(g_temporal.phase&1u,(g_temporal.phase>>1u)&1u);
            uint2 selected=uint2(record%g_temporal.phaseWidth,record/g_temporal.phaseWidth)*2u+offset;
            selected+=uint2(g_temporal.regionOffset%g_world.refinedCountX,g_temporal.regionOffset/g_world.refinedCountX);
            index=int2(selected)+int2(g_world.refinedMinX,g_world.refinedMinY);
        }
        uint outputIndex=uint(index.y-g_world.refinedMinY)*g_world.refinedCountX+uint(index.x-g_world.refinedMinX);

        if (!temporal && g_dispatch.refinedSpacing!=1u && g_dispatch.refinedSpacing!=2u) status|=WORLD_INVALID;
        float2 q=(float2(index)+0.5)*float(g_dispatch.refinedSpacing);
        bool straight=g_dispatch.level==1u || windowed;
        if (g_dispatch.level>2u || (straight && g_dispatch.refinedSpacing!=1u)) status|=WORLD_INVALID;

        WorldLevel parent=g_levels[straight?0u:1u];
        WorldProjection projection=worldProjectionIdentity();

        if (straight) {
            // Fixed midpoint quadrature, uploaded once as binary32 directions.
            // Trace from q to FIRST opaque cell or actual world escape; no
            // footprint, parent interpolation, history or ordinary-miss sky.
            if (parent.directions!=256u) status|=WORLD_INVALID;

            for (uint phi=0u;phi<256u;++phi) {
                float2 direction=g_directions[phi];
                bool included=!windowed || (g_page_masks[8u*g_dispatch.parentLevel+phi/32u]&(1u<<(phi%32u)))!=0u;
                WorldInterval radiance=worldIdentity();
                if (included) radiance=worldTraceRaw(q,direction,0,parent.farCells,true,work);
                // Excluded black-sky bins are exactly zero radiance. They STILL
                // contribute their original weight/basis/constant observation.
                status|=radiance.status;
                if (radiance.status!=0u) continue;

                float3 L=worldL(radiance);
                worldObserveSample(projection,L);
                float angle=atan2(direction.y,direction.x);
                const float w=1.0/256.0;
                worldProjectSample(projection,L,angle,w,status);
            }
        } else {
            for (uint sample=0;sample<4u;++sample) {
                int2 k;float weight;
                worldParentSample(q,parent,sample,k,weight);
                if (weight==0) continue;

                for (uint phi=0;phi<parent.directions;++phi) {
                    float2 endpoint,actual;
                    uint parentIndex;WorldInterval radiance=worldIdentity();
                    if (!worldRefinementGeometry(q,parent,k,phi,endpoint,actual,parentIndex,secondary || temporal,
                        temporal?float(parent.spacing):4)) radiance.status=WORLD_INVALID;
                    else radiance=worldCompose(worldConnector(q,endpoint,work,secondary,temporal),g_parent[parentIndex]);
                    status|=radiance.status;
                    // Failed positive-weight inputs fail the entire row. Do not
                    // normalize them away or let an invalid direction reach atan2.
                    if (radiance.status != 0u) continue;

                    float3 L=worldL(radiance);
                    worldObserveSample(projection,L);
                    float angle=atan2(actual.y,actual.x);
                    float w=weight/float(parent.directions);
                    worldProjectSample(projection,L,angle,w,status);
                }
            }
        }

        WorldCoefficients output=worldProjectionResult(projection,status);
        float retention = temporal ? worldTemporalHistoryRetention(outputIndex) : 0.0;
        if (retention > 0.0) {
            WorldCoefficients previous=g_previous_field[outputIndex];
            status|=previous.status;
            for (uint coefficient=0u;coefficient<9u;++coefficient) {
                float3 prior = worldCoefficient(previous,coefficient);
                float3 blended = retention == 1.0 ? prior : lerp(worldCoefficient(output,coefficient), prior, retention);
                if (!worldFinite3(blended)) status|=WORLD_INVALID;
                output.coefficient[coefficient].r=blended.x;
                output.coefficient[coefficient].g=blended.y;
                output.coefficient[coefficient].b=blended.z;
            }
        }
        if (straight || temporal) worldAdmitCoefficients(output,status);
        if (temporal) worldRecordTemporalState(outputIndex,output,status);
        output.status=status;g_refined[outputIndex]=output;wrote=true;
    }

    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void refineWorldRadiance(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldRefine(tid,lane,0u,false);
}

[shader("compute")]
[numthreads(64,1,1)]
void refineWorldTemporalSerial(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldRefine(tid,lane,g_temporal.firstRecord,false,true);
}

// One group owns one probe. Shared slots preserve parent-sample/phi order;
// only the ray work is parallel, with no floating-point wave reduction.
groupshared float worldTemporalAngle[256];
groupshared float worldTemporalWeight[256];

[shader("compute")]
[numthreads(64,1,1)]
void refineWorldTemporal(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    bool queued=g_temporal.workQueue!=0u;
    bool active=group.x<(queued?g_temporal_work[0]:g_dispatch.count);
    WorldLevel parent=g_levels[1u];
    uint record=queued?(active?g_temporal_work[group.x+1u]:0u):group.x+g_temporal.firstRecord;
    uint2 offset=uint2(g_temporal.phase&1u,(g_temporal.phase>>1u)&1u);
    uint2 selected=uint2(record%g_temporal.phaseWidth,record/g_temporal.phaseWidth)*2u+offset;
    selected+=uint2(g_temporal.regionOffset%g_world.refinedCountX,g_temporal.regionOffset/g_world.refinedCountX);
    if (queued) selected=uint2(record%g_world.refinedCountX,record/g_world.refinedCountX);
    int2 index=int2(selected)+int2(g_world.refinedMinX,g_world.refinedMinY);
    float2 q=(float2(index)+0.5)*float(g_dispatch.refinedSpacing);
    uint sampleCount=4u*parent.directions;
    if (sampleCount>256u) {status|=WORLD_INVALID;active=false;sampleCount=256u;}

    for (uint item=lane;item<sampleCount;item+=64u) {
        uint sample=item/parent.directions,phi=item%parent.directions;
        float weight=0,angle=0;WorldInterval radiance=worldIdentity();
        if (active) {
            int2 k;
            worldParentSample(q,parent,sample,k,weight);
            if (weight!=0) {
                float2 endpoint,actual;uint parentIndex;
                if (!worldRefinementGeometry(q,parent,k,phi,endpoint,actual,parentIndex,true,float(parent.spacing)))
                    radiance.status=WORLD_INVALID;
                else {
                    radiance=worldCompose(worldConnector(q,endpoint,work,false,true),g_parent[parentIndex]);
                    angle=atan2(actual.y,actual.x);
                }
            }
        }
        worldRayR[item]=radiance.r;worldRayG[item]=radiance.g;worldRayB[item]=radiance.b;
        worldRayStatus[item]=radiance.status;
        worldTemporalAngle[item]=angle;worldTemporalWeight[item]=weight;
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane==0u && active) {
        WorldProjection projection=worldProjectionIdentity();
        for (uint item=0u;item<sampleCount;++item) {
            float weight=worldTemporalWeight[item];
            if (weight==0) continue;
            status|=worldRayStatus[item];
            if (worldRayStatus[item]!=0u) continue;

            float3 L=float3(worldRayR[item],worldRayG[item],worldRayB[item]);
            worldObserveSample(projection,L);
            worldProjectSample(projection,L,worldTemporalAngle[item],weight/float(parent.directions),status);
        }
        uint outputIndex=selected.y*g_world.refinedCountX+selected.x;
        WorldCoefficients output=worldProjectionResult(projection,status);
        float retention = worldTemporalHistoryRetention(outputIndex);
        if (retention > 0.0) {
            WorldCoefficients previous=g_previous_field[outputIndex];
            status|=previous.status;
            for (uint coefficient=0u;coefficient<9u;++coefficient) {
                float3 prior = worldCoefficient(previous,coefficient);
                float3 blended = retention == 1.0 ? prior : lerp(worldCoefficient(output,coefficient), prior, retention);
                if (!worldFinite3(blended)) status|=WORLD_INVALID;
                output.coefficient[coefficient].r=blended.x;
                output.coefficient[coefficient].g=blended.y;
                output.coefficient[coefficient].b=blended.z;
            }
        }
        worldAdmitCoefficients(output,status);
        worldRecordTemporalState(outputIndex,output,status);
        output.status=status;g_refined[outputIndex]=output;wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

// ------------------------------------------------------------------
// parallel direct-window rays with ordered coefficient projection
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64,1,1)]
void refineWorldRadianceParallel(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    bool active=group.x<g_dispatch.count;
    WorldLevel parent=g_levels[0u];
    int2 index=0;float2 q=0;
    if (active) {
        uint countX=g_dispatch.receiverCountX;
        index=int2(g_dispatch.receiverMinX,g_dispatch.receiverMinY)+
              int2(int(group.x%countX),int(group.x/countX));
        q=(float2(index)+0.5)*float(g_dispatch.refinedSpacing);
    }

    // GroupIndex, not a wave-local index, owns four distinct phi bins. Every
    // bin is initialized on every group, including masked and inactive bins.
    for (uint phi=lane;phi<256u;phi+=64u) {
        WorldInterval radiance=worldIdentity();
        if (active) {
            bool included=(g_page_masks[8u*g_dispatch.parentLevel+phi/32u]&(1u<<(phi%32u)))!=0u;
            if (included) radiance=worldTraceRaw(q,g_directions[phi],0,parent.farCells,true,work);
        }

        worldRayR[phi]=radiance.r;
        worldRayG[phi]=radiance.g;
        worldRayB[phi]=radiance.b;
        worldRayStatus[phi]=radiance.status;
    }

    GroupMemoryBarrierWithGroupSync();

    if (lane==0u && active) {
        if (g_dispatch.level!=2u || g_dispatch.refinedSpacing!=1u || parent.directions!=256u) status|=WORLD_INVALID;
        uint outputIndex=uint(index.y-g_world.refinedMinY)*g_world.refinedCountX+uint(index.x-g_world.refinedMinX);
        WorldProjection projection=worldProjectionIdentity();

        // Preserve worldRefine's phi order and every projection expression.
        // Integer work was accumulated per lane; no floating reduction occurs.
        for (uint phi=0u;phi<256u;++phi) {
            float2 direction=g_directions[phi];
            uint rayStatus=worldRayStatus[phi];
            status|=rayStatus;
            if (rayStatus!=0u) continue;

            float3 L=float3(worldRayR[phi],worldRayG[phi],worldRayB[phi]);
            worldObserveSample(projection,L);
            float angle=atan2(direction.y,direction.x);
            const float w=1.0/256.0;
            worldProjectSample(projection,L,angle,w,status);
        }

        WorldCoefficients output=worldProjectionResult(projection,status);
        worldAdmitCoefficients(output,status);
        output.status=status;g_refined[outputIndex]=output;wrote=true;
    }

    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void refineWorldBounce(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldRefine(tid,lane,g_bounce.firstRecord,true);
}

// ------------------------------------------------------------------
// one generation-local face table, primary incident moments and final combination
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64,1,1)]
void buildWorldFaceLookup(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    if (tid.x<g_dispatch.count) {
        uint ordinal=g_bounce.firstRecord+tid.x;
        uint key=g_face_keys[ordinal],cellIndex=key>>2u,side=key&3u;
        int2 cell=int2(cellIndex%g_world.width,cellIndex/g_world.width);
        int2 offset=side==0u?int2(-1,0):side==1u?int2(1,0):side==2u?int2(0,-1):int2(0,1);
        if (ordinal>=g_bounce.faceCount || !worldInside(cell)) status|=WORLD_SOURCE;
        else {
            WorldCell owner=worldCell(cell,status);
            if (!(owner.coverage>0) || (owner.coverage==1 && !worldTransmissiveCell(cell+offset))) status|=WORLD_SOURCE;
        }

        if (status==0u) {
            uint slot=(key*2654435761u)&(g_bounce.lookupCapacity-1u);
            bool inserted=false;
            for (uint probe=0u;probe<g_bounce.lookupCapacity;++probe) {
                uint previous;
                InterlockedCompareExchange(g_face_lookup[slot],0u,ordinal+1u,previous);
                if (previous==0u) {inserted=true;break;}
                if (previous>g_bounce.faceCount || g_face_keys[previous-1u]==key) {status|=WORLD_SOURCE;break;}
                slot=(slot+1u)&(g_bounce.lookupCapacity-1u);
            }
            if (!inserted) status|=WORLD_SOURCE;
        }
        wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void bakeWorldFaceMoments(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    if (tid.x<g_dispatch.count) {
        uint ordinal=g_bounce.firstRecord+tid.x;
        uint key=g_face_keys[ordinal],cellIndex=key>>2u,side=key&3u;
        int2 cell=int2(cellIndex%g_world.width,cellIndex/g_world.width);
        float2 normal=side==0u?float2(-1,0):side==1u?float2(1,0):side==2u?float2(0,-1):float2(0,1);
        float2 point=float2(cell)+0.5+normal*0.5;
        float3 moment=0;
        if (ordinal>=g_bounce.faceCount || !worldInside(cell)) status|=WORLD_SOURCE;
        else {
            for (uint phi=0u;phi<256u;++phi) {
                float2 direction=g_directions[phi];
                if (dot(normal,direction)<=0) continue; // Inward bins keep zero weight /256.

                WorldInterval primary=worldTraceRaw(point,direction,0,float(2u*(g_world.width+g_world.height)),true,work);
                status|=primary.status;
                // This normalized cardinal K sum is <1 for the fixed256 bins,
                // so the existing source far-radiance/FTZ-loss bound also bounds
                // any one reflected source when0<=A<=1. No second/pi or S term.
                moment+=worldL(primary)*(terrainReceiverDiffuseLift(float3(normal,0),direction)/256);
            }
        }
        if (!worldFinite3(moment) || any(moment<0)) status|=WORLD_INVALID;
        WorldRgb output;output.r=moment.x;output.g=moment.y;output.b=moment.z;
        g_face_moments[ordinal]=output;wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

// ------------------------------------------------------------------
// parallel face rays with ordered diffuse moments
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64,1,1)]
void bakeWorldFaceMomentsParallel(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    bool active=group.x<g_dispatch.count;
    uint ordinal=g_bounce.firstRecord+group.x;
    float2 normal=0,point=0;
    bool valid=false;
    if (active) {
        uint key=g_face_keys[ordinal],cellIndex=key>>2u,side=key&3u;
        int2 cell=int2(cellIndex%g_world.width,cellIndex/g_world.width);
        normal=side==0u?float2(-1,0):side==1u?float2(1,0):side==2u?float2(0,-1):float2(0,1);
        point=float2(cell)+0.5+normal*0.5;
        valid=ordinal<g_bounce.faceCount && worldInside(cell);
    }

    // All bins are initialized, including inward bins and inactive groups.
    // Shared values are raw radiance; weighting remains in the serial order.
    for (uint phi=lane;phi<256u;phi+=64u) {
        WorldInterval primary=worldIdentity();
        if (active && valid && dot(normal,g_directions[phi])>0)
            primary=worldTraceRaw(point,g_directions[phi],0,float(2u*(g_world.width+g_world.height)),true,work);

        worldRayR[phi]=primary.r;
        worldRayG[phi]=primary.g;
        worldRayB[phi]=primary.b;
        worldRayStatus[phi]=primary.status;
    }

    GroupMemoryBarrierWithGroupSync();

    if (lane==0u && active) {
        float3 moment=0;
        if (!valid) status|=WORLD_SOURCE;
        else {
            for (uint phi=0u;phi<256u;++phi) {
                float2 direction=g_directions[phi];
                if (dot(normal,direction)<=0) continue;

                status|=worldRayStatus[phi];
                float3 radiance=float3(worldRayR[phi],worldRayG[phi],worldRayB[phi]);
                moment+=radiance*(terrainReceiverDiffuseLift(float3(normal,0),direction)/256);
            }
        }

        if (!worldFinite3(moment) || any(moment<0)) status|=WORLD_INVALID;
        WorldRgb output;output.r=moment.x;output.g=moment.y;output.b=moment.z;
        g_face_moments[ordinal]=output;wrote=true;
    }

    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void combineWorldBounce(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    if (tid.x<g_dispatch.count) {
        uint row=g_bounce.firstRecord+tid.x;
        uint x=row%g_world.refinedCountX,y=row/g_world.refinedCountX;
        uint directRow=uint(int(y)+g_world.refinedMinY-g_dispatch.receiverMinY)*g_dispatch.receiverCountX+
            uint(int(x)+g_world.refinedMinX-g_dispatch.receiverMinX);
        WorldCoefficients direct=g_bounce_direct[directRow],indirect=g_bounce_indirect[row],output;
        status=direct.status|indirect.status;
        for (uint coefficient=0u;coefficient<9u;++coefficient) {
            float3 incoming=worldCoefficient(indirect,coefficient);
            float3 value=all(incoming==0)?worldCoefficient(direct,coefficient):worldCoefficient(direct,coefficient)+incoming;
            output.coefficient[coefficient].r=value.x;output.coefficient[coefficient].g=value.y;output.coefficient[coefficient].b=value.z;
        }
        if (any(worldCoefficient(output,0u)<0)) status|=WORLD_INVALID;
        worldAdmitCoefficients(output,status);
        output.status=status;g_refined[row]=output;wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

// ------------------------------------------------------------------
// complete generation-scoped contributor discovery
// ------------------------------------------------------------------

void worldDiscoverKey(uint key,inout uint status) {
    if (key==0xffffffffu) return; // Escape or a strict inside-opaque start.
    uint word=key>>5u;
    if (word>=g_discovery.bitsetWords) { status|=WORLD_INVALID;return; }

    // Per-quantum occurrences fit trace_calls*max_native_segments in u32;
    // the host sums them in u64. Distinct offscreen faces use first-set wins.
    InterlockedAdd(g_discovery_state[2],1u);

    uint bit=1u<<(key&31u),before;
    InterlockedOr(g_discovery_bits[word],bit,before);
    if ((before&bit)!=0u) return;

    uint slot;
    InterlockedAdd(g_discovery_state[0],1u,slot);
    if (slot>=g_discovery.keyCapacity) {
        InterlockedOr(g_discovery_state[1],1u);
        status|=WORLD_INVALID;
        return;
    }

    g_discovery_keys[slot]=key;

    uint cell=key>>2u;
    int2 face=int2(int(cell%g_world.width),int(cell/g_world.width));
    int2 minimum=int2(g_world.refinedMinX,g_world.refinedMinY);
    int2 maximum=minimum+int2(g_world.refinedCountX,g_world.refinedCountY);
    if (any(face<minimum) || any(face>=maximum)) InterlockedAdd(g_discovery_state[3],1u);
}

[shader("compute")]
[numthreads(64,1,1)]
void discoverWorldFaces(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0u;bool wrote=false;
    if (tid.x<g_discovery.count) {
        uint record=g_discovery.firstRecord+tid.x;
        WorldLevel parent=g_levels[g_discovery.parentLevel];
        if (g_discovery.level==0u) {
            int2 index=int2(int(record%g_world.refinedCountX)+g_world.refinedMinX,
                int(record/g_world.refinedCountX)+g_world.refinedMinY);
            float2 q=(float2(index)+0.5)*float(g_discovery.refinedSpacing);
            for (uint sample=0u;sample<4u;++sample) {
                int2 k;float weight;
                worldParentSample(q,parent,sample,k,weight);
                if (weight==0) continue;

                for (uint phi=0u;phi<parent.directions;++phi) {
                    float2 endpoint,actual;uint parentIndex,key;
                    if (!worldRefinementGeometry(q,parent,k,phi,endpoint,actual,parentIndex,true)) {
                        status|=WORLD_INVALID;
                        continue;
                    }

                    WorldInterval value=worldConnectorHit(q,endpoint,work,key,false,true,true);
                    status|=value.status;
                }
            }
        } else {
            WorldLevel level=g_levels[g_discovery.level];
            uint directionIndex=record%level.directions;
            uint spatial=record/level.directions;
            int2 probe=int2(int(spatial%level.countX)+level.minX,int(spatial/level.countX)+level.minY);
            float2 p=worldProbe(level,probe);
            float2 d=g_directions[level.directionOffset+directionIndex];
            if (g_discovery.level==g_discovery.parentLevel) {
                uint key;
                WorldInterval value=worldTraceHit(worldIntervalOrigin(p,d,level),d,0,
                    level.farCells-level.nearCells,true,work,key,false,true,true);
                status|=value.status;
            } else {
                for (uint sample=0u;sample<4u;++sample) {
                    int2 k;float weight;
                    worldParentSample(p,parent,sample,k,weight);
                    if (weight==0) continue;

                    for (uint child=0u;child<4u;++child) {
                        float2 from,to;uint key,parentIndex;
                        if (!worldBounceCascadeGeometry(p,d,level,parent,k,4u*directionIndex+child,from,to,parentIndex)) {
                            status|=WORLD_PARENT;continue;
                        }

                        WorldInterval value=worldConnectorHit(from,to,work,key,false,true,true);
                        status|=value.status;
                        }
                }
            }
        }
        wrote=true;
    }
    worldFinishGroup(lane,work,wrote,status);
}

// ------------------------------------------------------------------
// native trace and entered-face qualification
// ------------------------------------------------------------------

void worldQualifyQueries(uint3 tid,uint lane,bool qualifySkip) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0;bool wrote=false;
    if (tid.x<g_dispatch.count) {
        WorldQuery query=g_queries[tid.x];
        if (g_dispatch.level==0u) {
            uint key;
            WorldInterval value=worldTraceHit(float2(query.ox,query.oy),float2(query.dx,query.dy),
                query.nearCells,query.farCells,query.terminal!=0u,work,key,false,false,query.reserved==2u,false,qualifySkip);
            if (query.reserved==1u) value.segments=key;
            g_current[tid.x]=value;status=value.status;wrote=true;
        } else if (g_dispatch.level==1u) {
            // Qualification only: the fixture supplies its complete native-ray
            // key union and uses the existing lookup/moment kernels unchanged.
            uint key;
            WorldInterval value=worldTraceHit(float2(query.ox,query.oy),float2(query.dx,query.dy),
                query.nearCells,query.farCells,query.terminal!=0u,work,key,true,false,query.reserved==2u);
            g_current[tid.x]=value;status=value.status;wrote=true;
        } else status=WORLD_INVALID;
    }
    worldFinishGroup(lane,work,wrote,status);
}

[shader("compute")]
[numthreads(64,1,1)]
void traceWorldQueries(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldQualifyQueries(tid,lane,false);
}

[shader("compute")]
[numthreads(64,1,1)]
void traceWorldSkipQueries(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldQualifyQueries(tid,lane,true);
}

// ------------------------------------------------------------------
// shared signed-CH diffuse and native material qualification
// ------------------------------------------------------------------

struct WorldNormalQuery { float x,y,z;uint coefficient; };
struct WorldNormalResult { float r,g,b;uint status;float displayR,displayG,displayB;uint reserved; };
StructuredBuffer<WorldCoefficients> g_normal_coefficients;
StructuredBuffer<WorldNormalQuery> g_normal_queries;
RWStructuredBuffer<WorldNormalResult> g_normal_results;

[shader("compute")]
[numthreads(64,1,1)]
void qualifyWorldDiffuse(uint3 tid:SV_DispatchThreadID,uint lane:SV_GroupIndex) {
    worldBeginGroup(lane);
    WorldWork work={};uint status=0;bool wrote=false;
    if (tid.x<g_dispatch.count) {
        WorldNormalQuery query=g_normal_queries[tid.x];
        uint count,stride;g_normal_coefficients.GetDimensions(count,stride);
        float3 value=0;

        if (g_dispatch.level==0u) {
            if (query.coefficient>=count) status=WORLD_INVALID;
            else value=worldDiffuseResponse(g_normal_coefficients[query.coefficient],float3(query.x,query.y,query.z),status);
        } else if (g_dispatch.level==1u) {
            // Qualification-only presets reuse the existing query/result ABI
            // and pipeline. Low16 selects coefficients, high16 selects a
            // declared material control; no new production uniform or buffer.
            uint index=query.coefficient&0xffffu,preset=query.coefficient>>16u;
            float3 normal=float3(query.x,query.y,query.z);
            if (index>=count || preset>12u) status=WORLD_INVALID;
            else {
                WorldCoefficients coefficients=g_normal_coefficients[index];
                float roughness=preset==1u ? 0.5: preset==2u ? 1.0:0.0;
                float3 albedo=float3(0.5,0.25,0.125);
                float metallic=preset==4u ? 1.0:0.0;
                float ior=preset==5u ? 1.0:1.5;

                if (preset==7u) roughness=asfloat(0x7fc00000u);

                if (preset==8u) metallic=asfloat(0x7fc00000u);

                if (preset==9u) ior=asfloat(0x7fc00000u);

                if (preset==10u) albedo.x=asfloat(0x7fc00000u);

                if (preset==11u) { metallic=1.0;roughness=1.1; }

                if (preset<=2u) value=worldSpecularResponse(coefficients,normal,roughness,status);
                else value=worldMaterialResponse(coefficients,normal,albedo,metallic,roughness,ior,status);

                if (preset==6u || preset==12u) {
                    value+=worldCutFaceResponse(albedo,normal,0.04,float3(2,1,0.5));
                }
            }
        } else status=WORLD_INVALID;

        if (!worldFinite3(value)) status|=WORLD_INVALID;

        float3 displayed=max(value,float3(0)); // ONLY after the complete normal convolution.
        WorldNormalResult result={value.x,value.y,value.z,status,displayed.x,displayed.y,displayed.z,0u};
        g_normal_results[tid.x]=result;wrote=true;
    }

    worldFinishGroup(lane,work,wrote,status);
}
