#include "grid_particle_native.slang"

// Immutable cold row (24 bytes); the pack shifts volume into a physical radius.
struct ReadonlyRow { float mass, volume, material, r, g, b; };

// Renderer-owned 24-byte presentation row (plan §3 "Instance layout"). CPU twin:
// pP::ParticleGpu in lib/engine/app/renderer/App.Renderer.GridPass.cppm; the
// draw-side mirror is ParticleGpu in grid_tiles.slang. Position and radius stay
// float32 world metres: the float ulp at the 1365 m map corner is 4.1e-3 px at
// span64 where an absolute half2 position would be 8.44 px. Only the material
// word, the stable-identity UV seed and the motion pair are quantised.
struct PresentationRow {
    float2 position;                  // 0:  world metres
    float radius;                     // 8:  metres
    uint material_flags_temperature;  // 12: material 12 | class 4 << 12 | binary16 K << 16
    uint uv_seed;                     // 16: stable-identity material-UV seed
    half2 motion;                     // 20: native cells per solver step
};

// Class nibble of material_flags_temperature. It sits beside the material id
// because a gas row and a particle can share one material id. Bits 2..3 are
// reserved; solid versus liquid is selected by the material's own surface flags.
static const uint kParticleClassGas = 1u;
static const uint kParticleClassOptical = 2u;
static const uint kParticleMaterialMask = 0xfffu;
static const uint kParticleMaterialLimit = 4096u;
// Presentation clamp for the motion proxy. Not a solver limit.
static const float kParticleMotionLimit = 64.0;

// Same 96-byte GridTerrainVisual palette; circle packing consumes only ID/tint.
// Retained carrier words preserve the full authored transport append stride.
struct MaterialColor { uint material, rgba; uint carried[22]; };
struct PrepareFrame {
    float4 planes[6];
    float cellMetres, origin;
    uint count, movingCount;
    uint blockCount, censusWorldEdge;
    // Orientation publication: one candidate update per newly accepted
    // publication, the identity remap only when the metadata owner changed, and
    // this attempt's ownership binding outcome. All four are host tokens, never a
    // clock: there is no dt in this mechanism.
    uint orientationUpdate, orientationRemap;
    uint orientationPreviousCount, orientationOwned;
    // Density-lane admission padding: one density pixel in world metres and the
    // one admitted lane material. The homogeneous guard expands that material's
    // visibility margin only, so a pixel whose centre lies just outside the
    // analytic support still receives its intersecting footprint while no other
    // row's cull changes. Zero material means no lane and no guard.
    float surfaceGuard;
    uint surfaceMaterial;
};
ConstantBuffer<PrepareFrame> g_particle_prepare;
StructuredBuffer<NumericalRow> g_numerical;
StructuredBuffer<ReadonlyRow> g_readonly;
StructuredBuffer<MetadataRow> g_metadata;
StructuredBuffer<MaterialColor> g_particle_palette;
RWStructuredBuffer<PresentationRow> g_packed;
RWStructuredBuffer<uint> g_ranks;
RWStructuredBuffer<uint> g_block_counts;
RWStructuredBuffer<uint> g_block_prefix;
RWStructuredBuffer<uint> g_particle_ids;
RWStructuredBuffer<uint> g_particle_args;
// Orientation history: the accepted role is read, the candidate is written, and
// the two never alias. The committed per-cell grids are bound by the world
// lighting ownership authority, which is also the only source of `orientationOwned`.
StructuredBuffer<OrientationRow> g_orientation_previous;
RWStructuredBuffer<OrientationRow> g_orientation_current;
StructuredBuffer<MetadataRow> g_previous_metadata;
StructuredBuffer<uint> g_best_row;
StructuredBuffer<uint> g_cell_fill;

// ------------------------------------------------------------------
// stable-identity material UV seed
// ------------------------------------------------------------------
// The seed is a pure function of the row's existing stable 64-bit metadata id, so
// it survives checkpoint/replay, repacking, compaction and row reordering with no
// saved seed state. Nothing else enters it: no frame counter, compacted instance
// index, source row, position, conic axis, material id, runtime world-generation
// token or clock. All arithmetic is modulo 2^32 with logical right shifts, and
// the draw converts the two 16-bit halves to exact binary32 texture offsets.
uint mixParticleUvSeed(uint value) {
    value^=value>>16u;
    value*=0x7feb352du;
    value^=value>>15u;
    value*=0x846ca68bu;
    value^=value>>16u;
    return value;
}

uint particleUvSeed(uint id_low,uint id_high) {
    return mixParticleUvSeed(id_low^mixParticleUvSeed(id_high^0x9e3779b9u));
}

// Independent golden vectors (id high:low -> seed): 00000000:00000001 ->
// a07c7a97, 00000000:00000002 -> 0e77ceb6, 00000001:00000000 -> 8e374fe0,
// ffffffff:ffffffff -> 8fad8c62, and the pure arithmetic vector
// 00000000:00000000 -> ae6f80f1. They pin this arithmetic against a second
// implementation; they are not a uniqueness guarantee.

// ------------------------------------------------------------------
// physical circle packing with the committed hot/cold row identity
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(256,1,1)]
void packParticles(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_particle_prepare.count) return;

    MetadataRow metadata=g_metadata[id.x];
    uint material=metadata.materialTemperature & 0xffffu;

    // A rejected or disabled row is written all-zero and never enters the
    // compaction: zero radius cannot pass visible(), so the indirect instance
    // count is the observable rejection count and no stale bits survive.
    PresentationRow particle=(PresentationRow)0;
    uint class_flags=0u;

    if (metadata.sourceRow!=0xffffffffu) {
        NumericalRow hot=g_numerical[metadata.sourceRow];
        ReadonlyRow cold=g_readonly[metadata.sourceRow];
        // Disabled rows are excluded at pack time, before the compaction count.
        if (!(hot.enabled>0.0)) {
            g_packed[id.x]=particle;
            return;
        }

        // The hot row is the authoritative live position. It is written in world
        // metres directly: one origin shift and one cell-to-metre scale, with
        // every binary32 quantum preserved (no cell rounding).
        float2 position=(float2(hot.positionX,hot.positionY)-g_particle_prepare.origin)*g_particle_prepare.cellMetres;
        // Cold volume is in cell^2*reference-thickness units. Its disk area
        // is volume/thickness, so physical radius is h*sqrt(volume/pi).
        float radius=g_particle_prepare.cellMetres*sqrt(cold.volume/3.141592653589793);
        if (!all(isfinite(position)) || !isfinite(radius) || radius<=0.0) {
            g_packed[id.x]=particle;
            return;
        }

        particle.position=position;
        particle.radius=radius;
        // Native cells per solver step, with no dt division: the per-step
        // displacement is the only motion the solver publishes. Non-finite
        // components become 0 and each component is clamped to the presentation
        // limit, so nothing unbounded reaches the vertex stage.
        float2 displacement=float2(hot.displacementX,hot.displacementY);
        float2 motion=float2(
            isfinite(displacement.x) ? clamp(displacement.x,-kParticleMotionLimit,kParticleMotionLimit) : 0.0,
            isfinite(displacement.y) ? clamp(displacement.y,-kParticleMotionLimit,kParticleMotionLimit) : 0.0);
        particle.motion=half2(half(motion.x),half(motion.y));
    } else {
        // Gas keeps its own path and range rule: metadata position and radius
        // are already metres, and a gas row has no solver displacement.
        particle.position=float2(metadata.gasX,metadata.gasY);
        particle.radius=metadata.radius;
        particle.motion=half2(half(0.0),half(0.0));
        class_flags|=kParticleClassGas;
        if (!all(isfinite(particle.position)) || !isfinite(particle.radius) || particle.radius<=0.0) {
            g_packed[id.x]=particle;
            return;
        }
    }

    // The material id rides the row in 12 bits. A larger id is rejected rather
    // than truncated into a different valid material.
    if (material>=kParticleMaterialLimit) {
        g_packed[id.x]=(PresentationRow)0;
        return;
    }

    // MaterialColor mirrors GridTerrainVisual: words 0/1 are material and rgba,
    // so carried[17] is m_surface_flags (byte 76) and carried[18] is
    // m_transport_model (byte 80). Bit 0 of the surface flags is
    // kGridSurfaceOpticalBlend on the CPU and kSurfaceOpticalBlend in
    // grid_tiles.slang.
    uint surface_flags=g_particle_palette[material].carried[17];
    if ((surface_flags&1u)!=0u)
        class_flags|=kParticleClassOptical;

    // The fourth word is this row's stable-identity UV seed, derived from the
    // metadata id before compaction so a rejected publication can never replace
    // it and a different id occupying the row recomputes it. The palette tint is
    // no longer copied into the row: every shading path decodes the tint from the
    // row's own palette element, so the row stays 24 bytes and no tint is stored.
    particle.uv_seed=particleUvSeed(metadata.idLow,metadata.idHigh);
    // The material is not stored in its canonical packCell position: material,
    // class and temperature share one word. Shading reconstructs
    // canonical = material | (temperature << 16).
    particle.material_flags_temperature=
        (material&kParticleMaterialMask)|(class_flags<<12u)|(metadata.materialTemperature&0xffff0000u);

    g_packed[id.x]=particle;
}

// ------------------------------------------------------------------
// capture-only committed native-cell census, before any visual compaction
// ------------------------------------------------------------------
struct CensusRow {
    uint idLow, idHigh, material, status;
    uint cellX, cellY;
};
RWStructuredBuffer<CensusRow> g_particle_census;

[shader("compute")]
[numthreads(256,1,1)]
void projectParticleCensus(uint3 id:SV_DispatchThreadID) {
    if (id.x >= g_particle_prepare.count) return;

    MetadataRow metadata = g_metadata[id.x];
    CensusRow row;
    row.idLow = metadata.idLow;
    row.idHigh = metadata.idHigh;
    row.material = metadata.materialTemperature & 0xffffu;
    // Existing palette word20 is the explicitly authored transport tag. This
    // reports declared vacuum, never infers optics from solver class/opacity.
    row.status = row.material == 0u || g_particle_palette[row.material].carried[18] == 0x101u ? 8u : 0u;
    row.cellX = row.cellY = 0xffffffffu;
    bool moving=metadata.sourceRow!=0xffffffffu;
    bool sourceValid=!moving || metadata.sourceRow<g_particle_prepare.movingCount;
    NumericalRow hot=(NumericalRow)0;
    if (moving && sourceValid) hot=g_numerical[metadata.sourceRow];

    ParticleNativeCell cell=particleNativeCell(metadata,float2(hot.positionX,hot.positionY),hot.enabled,
        sourceValid,g_particle_prepare.cellMetres,g_particle_prepare.origin,float2(0),
        uint2(g_particle_prepare.censusWorldEdge));
    row.status|=cell.status;row.cellX=cell.x;row.cellY=cell.y;

    g_particle_census[id.x] = row;
}

// ------------------------------------------------------------------
// per-particle orientation target and slew
// ------------------------------------------------------------------
// One target per admitted row per accepted publication, computed here in the
// rank invocation that already visits every row: off-screen rows update too,
// because their history is not a function of this frame's visibility. There is
// no dt: the only clock is the accepted-publication token in the constants, and
// a repeated or held publication encodes no update at all.
// The accepted response limits: canonical angle turns at most 5.625 degrees and
// each amplitude at most 32/255 per accepted publication. A full amplitude ramp
// therefore takes eight updates and an orthogonal flip at least sixteen.
static const int kOrientationAngleSlewCodes = 2048;
static const int kOrientationAmplitudeSlew = 32;
static const float kOrientationAmplitudeScale = 255.0;
static const float kOrientationAxisEpsilon = 1.0e-8;

// One lattice value of the winner-motion field. The committed winner grid names a
// metadata row, which is the same packed row the draw indexes.
bool particleCellMotion(int2 cell,out float2 motion) {
    motion=float2(0.0,0.0);
    if (any(cell<int2(0)) || any(cell>=int2(int(kParticleNativeEdge)))) return false;
    uint row=g_best_row[uint(cell.y)*kParticleNativeEdge+uint(cell.x)];
    if (row==0xffffffffu || row>=g_particle_prepare.count) return false;
    motion=float2(float(g_packed[row].motion.x),float(g_packed[row].motion.y));
    return true;
}

// The shipped bilinear (dual-lattice) sample of the per-cell winner-motion field,
// including the W < 1/4 continuous extension toward the row's own motion. This is
// the single remaining production copy: the draw no longer recomputes it.
float2 particleFlowMotion(float2 cell_position,float2 row_motion) {
    float2 p=cell_position-0.5;
    float2 base=floor(p);
    float2 fraction=p-base;
    int2 cell=int2(base);
    float w_00=(1.0-fraction.x)*(1.0-fraction.y);
    float w_10=fraction.x*(1.0-fraction.y);
    float w_01=(1.0-fraction.x)*fraction.y;
    float w_11=fraction.x*fraction.y;

    float2 sum=float2(0.0,0.0);
    float weight=0.0;
    float2 value;
    if (particleCellMotion(cell,value)) {sum+=w_00*value;weight+=w_00;}
    if (particleCellMotion(cell+int2(1,0),value)) {sum+=w_10*value;weight+=w_10;}
    if (particleCellMotion(cell+int2(0,1),value)) {sum+=w_01*value;weight+=w_01;}
    if (particleCellMotion(cell+int2(1,1),value)) {sum+=w_11*value;weight+=w_11;}

    if (weight<0.25) return 4.0*sum+(1.0-4.0*weight)*row_motion;
    return sum/weight;
}

float particleFillTap(int2 cell) {
    if (any(cell<int2(0)) || any(cell>=int2(int(kParticleNativeEdge)))) return 0.0;
    return float(g_cell_fill[uint(cell.y)*kParticleNativeEdge+uint(cell.x)])/kParticleFillQuantum;
}

float4 particleFillRow(int2 cell,int offset_y) {
    return float4(particleFillTap(cell+int2(-1,offset_y)),particleFillTap(cell+int2(0,offset_y)),
        particleFillTap(cell+int2(1,offset_y)),particleFillTap(cell+int2(2,offset_y)));
}

// The shipped fractional 16-tap fill Sobel, in world XY and in its unnormalised
// units: the Sobel difference on one axis and the 1-2-1 smoothing on the other,
// applied to the bilinearly reconstructed field, so the stencil costs one 4x4
// window. The confidences it feeds are the shipped smoothstep(1, 3, |g|) gate.
float2 particleFillGradient(float2 cell_position) {
    float2 p=cell_position-0.5;
    float2 base=floor(p);
    float2 fraction=p-base;
    int2 cell=int2(base);
    float bx0=1.0-fraction.x;
    float bx1=fraction.x;
    float by0=1.0-fraction.y;
    float by1=fraction.y;
    float4 difference_x=float4(-bx0,-bx1,bx0,bx1);
    float4 smooth_x=float4(bx0,2.0*bx0+bx1,bx0+2.0*bx1,bx1);
    float4 difference_y=float4(-by0,-by1,by0,by1);
    float4 smooth_y=float4(by0,2.0*by0+by1,by0+2.0*by1,by1);

    float4 row_0=particleFillRow(cell,-1);
    float4 row_1=particleFillRow(cell,0);
    float4 row_2=particleFillRow(cell,1);
    float4 row_3=particleFillRow(cell,2);
    float gradient_x=dot(row_0,difference_x)*smooth_y.x+dot(row_1,difference_x)*smooth_y.y+
        dot(row_2,difference_x)*smooth_y.z+dot(row_3,difference_x)*smooth_y.w;
    float gradient_y=dot(row_0,smooth_x)*difference_y.x+dot(row_1,smooth_x)*difference_y.y+
        dot(row_2,smooth_x)*difference_y.z+dot(row_3,smooth_x)*difference_y.w;
    return float2(gradient_x,gradient_y);
}

// Doubled unoriented axis D(u) = (ux^2 - uy^2, 2 ux uy). Flow and tangent share it
// so one arbitration decides the single axis the shape may use.
float2 doubledAxis(float2 unit) {
    return float2(unit.x*unit.x-unit.y*unit.y,2.0*unit.x*unit.y);
}

// Identity remap: the accepted metadata owner is unchanged in the ordinary case,
// so a row keeps its state by index with no per-row ID fetch. Only a changed
// owner costs a same-rank ID compare and, on mismatch, a binary search of the old
// sorted metadata (IDs, never source row, instance number, material or position).
uint orientationSourceRow(uint row,out bool has_previous) {
    has_previous=g_particle_prepare.orientationPreviousCount>row;
    if (g_particle_prepare.orientationRemap==0u)
        return row;

    has_previous=false;
    MetadataRow current=g_metadata[row];
    if (g_particle_prepare.orientationPreviousCount>row) {
        MetadataRow same_rank=g_previous_metadata[row];
        if (same_rank.idLow==current.idLow && same_rank.idHigh==current.idHigh) {
            has_previous=true;
            return row;
        }
    }

    uint low=0u;
    uint high=g_particle_prepare.orientationPreviousCount;
    while (low<high) {
        uint middle=(low+high)/2u;
        MetadataRow probe=g_previous_metadata[middle];
        bool less=probe.idHigh<current.idHigh ||
            (probe.idHigh==current.idHigh && probe.idLow<current.idLow);
        if (less) low=middle+1u; else high=middle;
    }

    if (low<g_particle_prepare.orientationPreviousCount) {
        MetadataRow probe=g_previous_metadata[low];
        if (probe.idLow==current.idLow && probe.idHigh==current.idHigh) {
            has_previous=true;
            return low;
        }
    }

    return row;
}

// Candidate row for `row`: the target from this publication's fill and packed
// motion, slewed against the accepted history. Every candidate row is written,
// including deterministic neutral gas/invalid rows, so there are no holes.
OrientationRow updateOrientationRow(uint row) {
    bool has_previous=false;
    uint source_row=orientationSourceRow(row,has_previous);
    OrientationRow previous=(OrientationRow)0;
    if (has_previous) previous=g_orientation_previous[source_row];

    PresentationRow particle=g_packed[row];
    uint class_bits=(particle.material_flags_temperature>>12u)&0xfu;
    bool gas=(class_bits&kParticleClassGas)!=0u;
    bool eligible=!gas && all(isfinite(particle.position)) && particle.radius>0.0;

    float2 axis=float2(0.0,0.0);
    bool has_axis=false;
    float bias_target=0.0;
    float stretch_target=0.0;
    if (eligible) {
        float2 centre=particle.position/g_particle_prepare.cellMetres;
        // Without the committed ownership grids no fill is bound, so the tangent
        // confidence stays zero and the flow cue falls back to the row's own
        // published motion, exactly the draw's no-ownership branch. No unbound
        // grid is ever read: the host sets orientationOwned only when the world
        // lighting authority actually bound this attempt's matching pair.
        float tangent_confidence=0.0;
        float2 tangent=float2(0.0,0.0);
        if (g_particle_prepare.orientationOwned!=0u) {
            float2 gradient=particleFillGradient(centre);
            float magnitude=length(gradient);
            if (magnitude>kParticleTangentGradientEpsilon) {
                tangent_confidence=smoothstep(
                    kParticleTangentConfidenceLow,kParticleTangentConfidenceHigh,magnitude);
                tangent=float2(gradient.y,-gradient.x)/magnitude;
            }
        }

        float2 own_motion=float2(float(particle.motion.x),float(particle.motion.y));
        float2 motion=g_particle_prepare.orientationOwned!=0u
            ? particleFlowMotion(centre,own_motion) : own_motion;
        float motion_length=length(motion);
        float flow_confidence=motion_length>0.0 ? saturate(motion_length/kParticleMotionReference) : 0.0;

        float2 combined=float2(0.0,0.0);
        if (flow_confidence>0.0) combined+=flow_confidence*doubledAxis(motion/motion_length);
        if (tangent_confidence>0.0)
            combined+=(1.0-flow_confidence)*tangent_confidence*doubledAxis(tangent);
        float total=flow_confidence+(1.0-flow_confidence)*tangent_confidence;
        float shared=clamp(length(combined)/max(total,kOrientationAxisEpsilon),0.0,1.0);
        if (length(combined)>kOrientationAxisEpsilon) {
            axis=combined/length(combined);
            has_axis=true;
            // B* = a h and S* = f h: both individual maxima are reachable together
            // when the cues agree, and cancellation fades both.
            bias_target=tangent_confidence*shared;
            stretch_target=flow_confidence*shared;
        }
    }

    uint target_angle=0u;
    if (has_axis) {
        float angle=0.5*atan2(axis.y,axis.x);
        if (angle<0.0) angle+=kParticleOrientationPi;
        target_angle=uint(floor(angle/kParticleOrientationPi*float(kParticleOrientationAngleCount)+0.5))&
            (kParticleOrientationAngleCount-1u);
    }
    uint bias_stop=uint(floor(clamp(bias_target,0.0,1.0)*kOrientationAmplitudeScale+0.5));
    uint stretch_stop=uint(floor(clamp(stretch_target,0.0,1.0)*kOrientationAmplitudeScale+0.5));

    int previous_angle=int(previous.angle_bias_stretch&0xffffu);
    int previous_bias=int((previous.angle_bias_stretch>>16u)&0xffu);
    int previous_stretch=int((previous.angle_bias_stretch>>24u)&0xffu);
    if (!has_previous) {
        // New identity: the target angle (zero if none) and B = S = 0, then ramp.
        previous_angle=int(target_angle);
        previous_bias=0;
        previous_stretch=0;
    } else if (!has_axis) {
        // Zero total confidence holds the angle and slews the amplitudes to zero.
        target_angle=uint(previous_angle);
    }

    int delta=int(target_angle)-previous_angle;
    delta=((delta%int(kParticleOrientationAngleCount))+int(kParticleOrientationAngleCount))%
        int(kParticleOrientationAngleCount);
    // Shortest signed modulo-65536 delta; the exact 32768 tie is positive.
    if (delta>32768) delta-=int(kParticleOrientationAngleCount);
    delta=min(max(delta,-kOrientationAngleSlewCodes),kOrientationAngleSlewCodes);
    int next_angle=(previous_angle+delta)&0xffff;

    int bias_delta=min(max(int(bias_stop)-previous_bias,-kOrientationAmplitudeSlew),kOrientationAmplitudeSlew);
    int stretch_delta=min(max(int(stretch_stop)-previous_stretch,-kOrientationAmplitudeSlew),kOrientationAmplitudeSlew);
    uint next_bias=uint(min(max(previous_bias+bias_delta,0),255));
    uint next_stretch=uint(min(max(previous_stretch+stretch_delta,0),255));

    // The canonical angle is the stored history; the vertex stage decodes it to
    // (cos, sin) and never writes that decoded pair back.
    OrientationRow result;
    result.angle_bias_stretch=uint(next_angle)|(next_bias<<16u)|(next_stretch<<24u);
    return result;
}

// ------------------------------------------------------------------
// stable per-particle frustum compaction and indirect count
// ------------------------------------------------------------------
groupshared uint g_scan[256];

void scanLane(uint lane, uint value) {
    g_scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < 256u; offset *= 2u) {
        uint prior = lane >= offset ? g_scan[lane - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[lane] += prior;
        GroupMemoryBarrierWithGroupSync();
    }
}

bool visible(PresentationRow particle) {
    // A rejected pack writes an all-zero row, and no row may carry a non-finite
    // coordinate into a clip position. Rejecting both here means the compaction
    // count excludes them and the vertex stage never loads such a row.
    if (!all(isfinite(particle.position)) || !(particle.radius>0.0))
        return false;
    // Enclose the drawn support, not only the disk quad: liquid is a disk of
    // radius R = 2.0 r and the solid rounded square is inscribed in the same
    // circle, so both stay inside this shared 3.0625 r cull margin, which also
    // covers the retained 1.5x flow stretch at 3.000 r. The admitted density-lane
    // material adds the one-density-pixel homogeneous guard, which is the same
    // term as expanding each cull plane by that guard.
    float radius=particle.radius*kParticleCullRadiusScale;
    if ((particle.material_flags_temperature&kParticleMaterialMask)==g_particle_prepare.surfaceMaterial)
        radius+=g_particle_prepare.surfaceGuard;
    for (uint plane=0u;plane<6u;++plane) {
        float4 p=g_particle_prepare.planes[plane];
        if (dot(float4(particle.position,0,1),p)+radius*length(p.xyz)<0) return false;
    }

    return true;
}

[shader("compute")]
[numthreads(256,1,1)]
void rankParticles(uint3 id:SV_DispatchThreadID,uint3 local:SV_GroupThreadID,uint3 group:SV_GroupID) {
    bool selected=id.x<g_particle_prepare.count ? visible(g_packed[id.x]) : false;
    // The orientation update is fused here because rank already visits every row:
    // culling must not skip a row's history. It contains no barrier and no early
    // return, so every lane still reaches scanLane's group barriers.
    if (g_particle_prepare.orientationUpdate!=0u && id.x<g_particle_prepare.count)
        g_orientation_current[id.x]=updateOrientationRow(id.x);
    scanLane(local.x,selected ? 1u : 0u);
    if (id.x<g_particle_prepare.count) g_ranks[id.x]=(g_scan[local.x]-(selected ? 1u : 0u))|(selected ? 0x80000000u : 0u);
    if (local.x==255u) g_block_counts[group.x]=g_scan[255u];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void scanParticleBlocks(uint3 local : SV_GroupThreadID) {
    // At most 4096 block totals for the admitted 1M population. All 256 lanes
    // participate; each lane visits at most 16 values, independent of N.
    uint prefixes[16];
    uint sum = 0u;
    for (uint item = 0u; item < 16u; ++item) {
        uint block = local.x * 16u + item;
        prefixes[item] = sum;
        if (block < g_particle_prepare.blockCount) sum += g_block_counts[block];
    }

    scanLane(local.x, sum);
    uint base = g_scan[local.x] - sum;
    for (uint item = 0u; item < 16u; ++item) {
        uint block = local.x * 16u + item;
        if (block < g_particle_prepare.blockCount) g_block_prefix[block] = base + prefixes[item];
    }

    if (local.x == 255u) g_block_prefix[g_particle_prepare.blockCount] = g_scan[255u];
}

[shader("compute")]
[numthreads(256,1,1)]
void scatterParticles(uint3 id:SV_DispatchThreadID) {
    if (id.x==0u) {
        g_particle_args[0]=6u;
        g_particle_args[1]=g_block_prefix[g_particle_prepare.blockCount];
        g_particle_args[2]=0u;
        g_particle_args[3]=0u;
    }

    if (id.x>=g_particle_prepare.count) return;

    uint rank=g_ranks[id.x];
    if ((rank&0x80000000u)!=0u) {
        uint before=g_block_prefix[id.x/256u]+(rank&0x7fffffffu);
        g_particle_ids[before]=id.x;
    }
}
