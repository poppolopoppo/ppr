// Copyright (c) 2024 Electronic Arts Inc. All rights reserved.
// Selected fork ErikSom/pbmpm 859c449; BSD-3-Clause notice: LICENSE.md.
#include "types.slang"
#define PBMPM_ENGINE_PARTICLE_ROWS 1
#define PBMPM_ENGINE_ADAPTERS 1
#define PBMPM_SPARSE_STORAGE 1
#include "fork_core.slang"

// ------------------------------------------------------------------
// engine transaction error and contact adapters
// ------------------------------------------------------------------
bool recordError(uint flag, uint node, uint field, uint particle, uint body) {
    InterlockedOr(g_errors[0], flag);
    uint previous;
    InterlockedCompareExchange(g_errors[1], 0u, 1u, previous);
    if (previous == 0u) {
        g_errors[2] = node;
        g_errors[3] = field;
        g_errors[4] = particle;
        g_errors[5] = body;
    }

    return previous == 0u;
}

void recordError(uint flag, uint node, uint field, uint particle) {
    recordError(flag, node, field, particle, 0xffffffffu);
}

// ------------------------------------------------------------------
// opt-in first-rejection operand evidence
// ------------------------------------------------------------------

// PPR_PBMPM_RETURN_DIAGNOSTIC=1 provisions 31 words instead of six. Only
// the first-error claimant reaches this replay; accepted particles never call it.
// Replay the unchanged arithmetic prefix, stopping at the identified guard.
// NaN means not reached/not applicable, not an invented downstream operand.
// Payload after marker word 6: sigma.xy,t,dev.xy,n,phi,C,D,c,radicand,
// projected.xy,H,nextPhi,nextC,ratio,denominator,returned,increment,edge,
// strengthPhi,voidFraction,updateHistory. Floats use asuint, not conversion.
void captureCalibratedRejection(uint field, float2 sigma, float C, float phi,
                                float H, MaterialProperties a, bool updateHistory) {
    uint words, stride;
    g_errors.GetDimensions(words, stride);
    if (words < 31u) return;

    for (uint i = 7u; i < 31u; ++i) g_errors[i] = 0x7fc00000u;
    g_errors[6] = 1u;
    g_errors[7] = asuint(sigma.x);
    g_errors[8] = asuint(sigma.y);
    g_errors[13] = asuint(phi);
    g_errors[14] = asuint(C);
    g_errors[20] = asuint(H);
    g_errors[30] = asuint(updateHistory ? 1.0 : 0.0);
    if (field == 27u || field == 28u || field == 22u || field == 23u) return;

    float2 e = log(sigma);
    float trial = e.x + e.y + H;
    float2 dev = e - trial*0.5;
    float n = length(dev);
    float sine = sin(a.frictionAngle / 180.0 * 3.14159);
    float denominator = 3.0-sine;
    g_errors[9] = asuint(trial);
    g_errors[10] = asuint(dev.x);
    g_errors[11] = asuint(dev.y);
    g_errors[12] = asuint(n);
    g_errors[24] = asuint(denominator);
    if (field == 29u || field == 30u || field == 31u) return;

    float alpha = sqrt(2.0/3.0)*2.0*sine/denominator;
    float cohesion = a.cohesion;
    float cap = a.cap;
    float phiDense = 1.0-a.denseEpsilon;
    float maxCap = log(phiDense/phi);
    if (field == 33u && (!isfinite(maxCap) || maxCap < 0.0)) {
        g_errors[15] = asuint(maxCap);
        return;
    }

    float nextC = C;
    float nextPhi = phi;
    float returned = min(trial, a.tension);
    bool porous = a.family == uint(MaterialCompacting);
    bool dense = porous && phi >= 1.0-a.denseEpsilon;
    g_errors[15] = asuint(cap);
    g_errors[16] = asuint(cohesion);
    g_errors[21] = asuint(nextPhi);
    g_errors[22] = asuint(nextC);
    g_errors[25] = asuint(returned);
    if (!porous) {
        cap += a.capHardening*C;
        g_errors[15] = asuint(cap);
        if (field == 26u) return;

        cap = min(cap, maxCap);
        returned = max(returned, -cap);
        if (updateHistory) nextC = max(C, -min(returned, 0.0));
        cohesion += a.cohesionHardening*nextC;
    } else {
        if (!dense) {
            cap = maxCap;
            g_errors[15] = asuint(cap);
            if (field == 33u) return;

            returned = max(returned, -cap);
            g_errors[25] = asuint(returned);
            if (updateHistory) {
                float remaining = phiDense-phi;
                float increment = remaining * (1.0-exp(-a.compactionRate*max(0.0, returned-trial)));
                g_errors[26] = asuint(increment);
                if (field == 34u) return;

                nextPhi = clamp(phi+increment, a.solidFraction0, 1.0-a.denseEpsilon);
            }
        }

        dense = nextPhi >= 1.0-a.denseEpsilon;
        float strengthPhi = dense ? 1.0-a.denseEpsilon : nextPhi;
        g_errors[21] = asuint(nextPhi);
        g_errors[28] = asuint(strengthPhi);
        if (field == 35u) return;

        cohesion *= pow(strengthPhi, a.solidExponent);
        g_errors[16] = asuint(cohesion);
        if (!dense) {
            cap = log(phiDense/nextPhi);
            g_errors[15] = asuint(cap);
            if (field == 37u) return;
        } else {
            cap = 0.0;
        }

        returned = max(returned, -cap);
    }

    float edge = alpha*max(-returned, 0.0)+cohesion;
    g_errors[15] = asuint(cap);
    g_errors[16] = asuint(cohesion);
    g_errors[21] = asuint(nextPhi);
    g_errors[22] = asuint(nextC);
    g_errors[25] = asuint(returned);
    g_errors[27] = asuint(edge);
    if (field == 38u) return;

    if (porous && !dense && returned <= 0.0) {
        float ratio = -returned/cap;
        g_errors[23] = asuint(ratio);
        if (!isfinite(ratio)) return;

        float projectedRatio = min(ratio, 1.0);
        float radicand = 1.0-projectedRatio*projectedRatio;
        g_errors[17] = asuint(radicand);
        if (field == 25u) return;

        edge *= sqrt(radicand);
        g_errors[27] = asuint(edge);
    }

    if (n > edge) {
        if (field == 39u) return;

        dev *= edge/n;
    }

    float2 projected = dev + returned*0.5;
    g_errors[18] = asuint(projected.x);
    g_errors[19] = asuint(projected.y);
}

void rejectCalibrated(uint field, uint row, float2 sigma, float C, float phi,
                      float H, MaterialProperties a, bool updateHistory) {
    if (recordError(8u, 0xffffffffu, field, row, 0xffffffffu)) {
        captureCalibratedRejection(field, sigma, C, phi, H, a, updateHistory);
    }
}

void fail(uint flag, uint row, uint field) {
    // Fork-specific flags occupy additional bits; engine flags retain meaning.
    uint mapped = flag==2u ? 2u : flag==4u ? 4u : flag==8u ? 8u : flag==16u ? 64u : 128u;
    recordError(mapped,0xffffffffu,field,row);
}

void failNode(uint node, uint field) {
    recordError(2u,node,field,0xffffffffu);
}

#include "sparse_address.slang"

// Shared absolute totals bound every signed prefix before the global merge.
groupshared uint s_absolute[256];
groupshared uint s_contributor[256];
// Sand gathers its own post-contact velocity, with the same node quantization.
// The original integer source tile remains unchanged for every other material.
groupshared float2 s_sandVelocity[64];

// Logical keys remain the error identity; per-contribution conversion is unchanged.
void scatterChecked(uint localAddress, uint globalAddress, float value, uint row, uint field) {
    int encoded=checkedEncode(value,row,field);
    uint magnitude=uint(encoded<0 ? -encoded : encoded);
    if (magnitude==0u) return;

    uint before;
    InterlockedAdd(s_absolute[localAddress],0u,before);
    for (;;) {
        if (before>0x7fffffffu || magnitude>0x7fffffffu-before) {
            recordError(4u,globalAddress/4u,field,row);
            return;
        }

        uint observed;
        InterlockedCompareExchange(s_absolute[localAddress],before,before+magnitude,observed);
        if (observed==before) break;
        before=observed;
    }

    InterlockedMin(s_contributor[localAddress],row);
    InterlockedAdd(s_dst[localAddress],encoded);
}

bool reserveWorkgroupMerge(uint localAddress, uint globalWord, uint key, uint field) {
    uint magnitude=s_absolute[localAddress];
    if (magnitude==0u) return false;

    // Reserve even when opposing contributions made the signed sum zero.
    return reserveSparseBudget(globalWord,key,magnitude,s_contributor[localAddress],field);
}

bool validParticle(Particle p, uint row) {
    uint field=invalidParticleField(p,g_parameters);
    if (field!=0xffffffffu) {
        recordError(8u,0xffffffffu,field,row);
        return false;
    }

    return true;
}

// Property failures have their own flag (512): node carries the property index,
// field 0 means absent/out-of-range storage, field 1 means an empty/mismatched row.
// UINT_MAX is the explicit global-profile route and never reads the table.
bool particlePropertiesUsable(uint row) {
    uint index = g_cold[row].propertyRow;
    if (index == 0xffffffffu) return true;

    uint count, stride;
    g_properties.GetDimensions(count, stride);
    if (index >= count) {
        recordError(512u, index, 0u, row);
        return false;
    }

    MaterialProperties properties = g_properties[index];
    if (properties.substanceId == 0u || properties.family != uint(g_cold[row].material)) {
        recordError(512u, index, 1u, row);
        return false;
    }

    return true;
}

#include "environment.slang"
#include "terrain_motion.slang"

// Independent terrain Coulomb coefficient, not the guardian's damping factor
// or the constitutive sand friction angle.
static const float kSandTerrainFriction = 0.5;

void engineEndpointContacts(float2 oldPosition, float2 transport, inout float2 position, inout float2 d, uint row) {
    for (uint body=0u;body<g_environment.bodyCount;++body) {
        BodyBoundary boundary=g_bodies[g_environment.bodyBase+body];
        Contact contact=collide(boundary.shape,position);
        if (!contact.collides) continue;

        d-=contact.penetration*contact.normal;
    }

    // Bodies retain their original integrated-endpoint query and carry term.
    // Terrain is the final motion constraint when both adapters are present.
    float friction = g_cold[row].material == MaterialSand ? kSandTerrainFriction : 0.0;
    terrainTransport(oldPosition, transport, position, d, row, friction);
}

// Grid contact is one production operation, also inspected by the test-only
// contact entry. Source shapes, engine terrain/bodies, then guardian, in order.
float4 projectGridContacts<let Cooperative : bool, let SandFriction : bool>(
    int2 node, uint key, int4 source, bool valid, out float2 sandDisplacement) {
    float2 d=float2(0.0);
    float mass=0.0;
    if (valid) {
        d=float2(decodeWord(source.x),decodeWord(source.y));
        mass=decodeWord(source.z);
        d=mass<1e-5 ? float2(0) : d/mass;
        for (uint i=0;i<g_parameters.shapeCount;++i) {
            Contact contact=collide(g_shapes[i],float2(node)+d);
            if (contact.collides) d-=max(dot(contact.normal,d)-min(0.0,dot(contact.normal,contact.point-float2(node))),0.0)*contact.normal;
        }
    }

    // Quadratic weights and APIC offsets represent node centres at index+0.5.
    // Only the terrain adapter changes convention; source/body hooks retain
    // their selected-fork coordinates and response order.
    float2 terrainNode = float2(node) + 0.5;
    float2 terrainEndpoint = terrainNode + d;
    Contact terrain = {false, 0.0, float2(0.0), float2(0.0)};
    if (Cooperative) {
        terrain = terrainContactWave(terrainEndpoint, valid && mass > 0.0, key, 0xffffffffu);
    } else if (valid && mass > 0.0) {
        terrain = terrainContact(terrainEndpoint, key, 0xffffffffu);
    }

    // Extensions apply only where encoded mass represents material. Empty
    // unsupported nodes neither query terrain nor query moving body boundaries.
    float2 sand = d;
    if (valid && mass > 0) {
        if (terrain.collides) {
            // Explicit contour adaptation: the response point is the queried
            // endpoint, matching the source unrotated box's interior semantics.
            // The geometric closest point is retained separately by the query.
            float gap = min(0.0, dot(terrain.normal, terrainEndpoint - terrainNode));
            float penetration = dot(terrain.normal, d) - gap;
            float2 incoming = d;
            d -= max(penetration, 0.0) * terrain.normal;
            sand = d;
            if (SandFriction) sand = terrainCarryFriction(incoming, d, kSandTerrainFriction);
        }

        for (uint body = 0u; body < g_environment.bodyCount; ++body) {
            BodyBoundary boundary = g_bodies[g_environment.bodyBase + body];
            bool distinctSand = SandFriction && any(sand != d);
            if (distinctSand) {
                // Friction precedes bodies. Reproject the distinct sand candidate
                // through the same ordered contacts, including moving-body carry.
                Contact sandContact = collide(boundary.shape, float2(node) + sand);
                if (sandContact.collides) {
                    float2 sandBody = bodyDisplacement(boundary, sandContact.point);
                    float2 sandRelative = sand - sandBody;
                    float sandGap = min(0.0, dot(sandContact.normal, sandContact.point - (float2(node) + sandBody)));
                    float sandPenetration = dot(sandContact.normal, sandRelative) - sandGap;
                    sandRelative -= max(sandPenetration, 0.0) * sandContact.normal;
                    sand = sandRelative + sandBody;
                }
            }

            Contact contact = collide(boundary.shape, float2(node) + d);
            if (!contact.collides) continue;

            float2 db = bodyDisplacement(boundary, contact.point);
            float2 relative = d - db;
            float gap = min(0.0, dot(contact.normal, contact.point - (float2(node) + db)));
            float penetration = dot(contact.normal, relative) - gap;
            relative -= max(penetration, 0.0) * contact.normal;
            d = relative + db;
            if (!distinctSand) sand = d;
        }
    }

    if (valid) {
        bool distinctSand = SandFriction && any(sand != d);
        if (distinctSand) {
            float2 sandEndpoint = float2(node) + sand;
            float2 sandCorrection = guardian(sandEndpoint, uint2(g_parameters.gridWidth, g_parameters.gridHeight), 4.0) - sandEndpoint;
            if (sandCorrection.x != 0) { sand.x = 0; sand.y = lerp(sand.y, 0.0, 0.5); }
            if (sandCorrection.y != 0) { sand.y = 0; sand.x = lerp(sand.x, 0.0, 0.5); }
        }

        float2 endpoint=float2(node)+d;
        float2 correction=guardian(endpoint,uint2(g_parameters.gridWidth,g_parameters.gridHeight),4.0)-endpoint;
        if (correction.x!=0) { d.x=0; d.y=lerp(d.y,0.0,0.5); }
        if (correction.y!=0) { d.y=0; d.x=lerp(d.x,0.0,0.5); }
        if (!distinctSand) sand = d;
    }

    sandDisplacement = sand;
    return float4(d,mass,decodeWord(source.w));
}

float4 contactGridState<let Cooperative : bool>(int2 node, uint key, int4 source, bool valid) {
    float2 unused;
    return projectGridContacts<Cooperative, false>(node, key, source, valid, unused);
}

float4 contactGridState(int2 node, uint key, int4 source) {
    return contactGridState<false>(node, key, source, true);
}

// ------------------------------------------------------------------
// engine sand objective-volume feedback
// ------------------------------------------------------------------

// Dimensionless relaxation for the admitted K2 schedule. This is a unilateral
// affine velocity impulse, not a material modulus or a change to plastic strain.
static const float kSandVolumeRelaxation = 0.1;

void applySandVolumeImpulse(inout Particle p, float referenceVolume) {
    if (p.material != MaterialSand || g_parameters.useGridVolumeForLiquid == 0u || referenceVolume <= 1.0) return;

    float impulse = 0.5 * kSandVolumeRelaxation * max(referenceVolume - 1.0 - trace(p.deformationDisplacement), 0.0);
    if (impulse > 0.0) {
        p.deformationDisplacement = add(p.deformationDisplacement, scale(identity(), impulse));
    }
}

// ------------------------------------------------------------------
// constitutive operand domains
// ------------------------------------------------------------------

bool finiteMatrix(SourceMatrix m) {
    return isfinite(m.c0x) && isfinite(m.c0y) && isfinite(m.c1x) && isfinite(m.c1y);
}

// The determinant, its reciprocal and every inverse component must stay usable.
// A zero or nonfinite determinant has no admitted reciprocal; an overflowing
// reciprocal or product has no admitted inverse. This is the same reciprocal
// constraint forms from inverted(F), and no decomposition is evaluated.
bool usableInverse(SourceMatrix m) {
    float df = determinant(m);
    if (!isfinite(df) || df == 0.0) return false;

    float reciprocal = 1.0 / df;
    if (!isfinite(reciprocal)) return false;

    return finiteMatrix(scale(matrix(m.c1y, -m.c0y, -m.c1x, m.c0x), reciprocal));
}

// forkSvd forms E=(c0x+c1y)/2, F=(c0x-c1y)/2, G=(c0y+c1x)/2, H=(c0y-c1x)/2 and
// evaluates sqrt(E*E + H*H) and sqrt(F*F + G*G). Testing those two radicands
// directly admits exactly the arithmetic domain where the squares and both
// square roots stay finite, instead of a conservative per-entry box: diag(2e19,1)
// is admitted because H and G are zero, while its source decomposition is finite.
// Deliberate singular-value limiting happens after this point, on finite values,
// so it is never re-inspected here.
bool usableRadicands(SourceMatrix m) {
    float e = (m.c0x + m.c1y) * 0.5;
    float f = (m.c0x - m.c1y) * 0.5;
    float g = (m.c0y + m.c1x) * 0.5;
    float h = (m.c0y - m.c1x) * 0.5;

    return isfinite(e * e + h * h) && isfinite(f * f + g * g);
}

bool usableDeformation(SourceMatrix m) {
    return finiteMatrix(m) && usableRadicands(m) && usableInverse(m);
}

// Records the first unusable constitutive operand for this row and reports
// whether the production constraint/integration may run. Stored F is field 4 and
// the trial (I + D)F is field 8 of the validation record; a liquid's density
// reciprocal is field 12. The device may flush a subnormal density to zero, so
// the reciprocal is validated explicitly rather than through that comparison.
bool deformationOperandsUsable(Particle p, uint row) {
    if (p.material == MaterialLiquid) {
        if (!isfinite(1.0 / p.liquidDensity)) {
            recordError(8u, 0xffffffffu, 12u, row);
            return false;
        }

        return true;
    }

    if (!usableDeformation(p.deformationGradient)) {
        recordError(8u, 0xffffffffu, 4u, row);
        return false;
    }

    SourceMatrix trial = product(add(identity(), p.deformationDisplacement), p.deformationGradient);
    if (!usableDeformation(trial)) {
        recordError(8u, 0xffffffffu, 8u, row);
        return false;
    }

    return true;
}

// ------------------------------------------------------------------
// produced-state backstop
// ------------------------------------------------------------------

// A constitutive coefficient product can overflow even when every admitted
// operand was finite. Check the produced D itself for finiteness (field 8),
// leaving encodability to the actual weighted scatter operands. A liquid density
// must be finite, positive and invertible (field 12). No value is repaired.
bool producedStateUsable(Particle p, uint row) {
    if (!finiteMatrix(p.deformationDisplacement)) {
        recordError(8u, 0xffffffffu, 8u, row);
        return false;
    }

    if (p.material == MaterialLiquid &&
        (!isfinite(p.liquidDensity) || p.liquidDensity <= 0.0 || !isfinite(1.0 / p.liquidDensity))) {
        recordError(8u, 0xffffffffu, 12u, row);
        return false;
    }

    return true;
}

// ------------------------------------------------------------------
// bootstrap error acceptance without an unused numerical source tile
// ------------------------------------------------------------------

bool bootstrapGridCertified() {
    if (g_parameters.shapeCount != 0u || g_environment.bodyCount > 1u ||
        g_parameters.fixedPointMultiplier == 0u || !isfinite(g_parameters.deltaTime) ||
        g_parameters.deltaTime <= 0.0 || g_parameters.deltaTime > 1.0) return false;

    if (g_environment.bodyCount != 0u) {
        BodyBoundary body = g_bodies[g_environment.bodyBase];
        if (body.shape.shapeType != ShapeTypeBox) return false;

        float fields[11] = {body.shape.positionX, body.shape.positionY, body.shape.halfSizeX,
            body.shape.halfSizeY, body.shape.radius, body.shape.rotationDegrees, body.comX, body.comY,
            body.velocityX, body.velocityY, body.angularVelocity};
        for (uint index = 0u; index < 11u; ++index) {
            if (!isfinite(fields[index]) || abs(fields[index]) > 1048576.0) return false;
        }
    }

    // Integer payload / positive multiplier and the 1e-5 mass cutoff bound d
    // below 2^48. Canonical terrain has extent <=2^22 and nonzero half-grid
    // segments; its finite normal/response and one bounded box keep arithmetic
    // below 2^64, and scaled node checks below 2^96. Thus conversion cannot
    // reject here. Only terrain coverage can fail; arbitrary colliders and
    // uncertified constants retain the complete original checks below.
    return true;
}

void checkBootstrapGridCoverage(int2 node, uint key, int4 source, bool valid) {
    if (!valid || g_environment.hasTerrain == 0u || (g_environment.reserved & 1u) != 0u) return;

    float mass = decodeWord(source.z);
    if (mass <= 0.0) return;

    float2 d = float2(decodeWord(source.x), decodeWord(source.y));
    d = mass < 1e-5 ? float2(0.0) : d / mass;
    checkTerrainCoverage(float2(node) + 0.5 + d, key);
}

// ------------------------------------------------------------------
// fused selected source stage; all lanes cross shared barriers on rejection
// ------------------------------------------------------------------
// The host encodes exactly two programs: the bootstrap entry runs stage 0 and
// the gather entry every later stage, so K is a runtime count. The final stage
// is the one whose index satisfies reserved0 + 1 >= reserved1, which keeps the
// schedule bounded without a third entry point or per-K specialization.
void fusedIteration<let FirstStage : bool, let Cooperative : bool>(uint lane,uint3 group) {
    uint groupIndex=sparseGroup(group);
    if (groupIndex>=g_sparseCounters[2]) return;

    for (uint i=lane;i<256;i+=64) {
        if (!FirstStage) s_src[i]=0;
        s_dst[i]=0;
        s_absolute[i]=0u; s_contributor[i]=0xffffffffu;
    }
    GroupMemoryBarrierWithGroupSync();

    Bucket bucket=g_buckets[groupIndex];
    int2 origin=int2(bucket.x,bucket.y)*6-1;
    int2 local=int2(lane%8,lane/8), node=origin+local;
    uint tile=(uint(local.y)*8+uint(local.x))*4;
    uint key=0xffffffffu;
    bool valid=sparseFlatKey(node,key);
    bool bootstrap = false;
    if (FirstStage) bootstrap = bootstrapGridCertified();

    int4 source=int4(0);
    if (valid && (!bootstrap || (g_environment.hasTerrain != 0u && (g_environment.reserved & 1u) == 0u))) {
        // This stage writes one ping-pong side and reads the other. The host
        // tracks which side the schedule wrote last and names that side's
        // generation, so no stage parity is implied here.
        bool sourceA=g_sparse.destinationA==0u;
        source=sparseSource(key,sourceA,g_sparse.sourceGeneration);
    }

    // The certificate depends only on uniform constants/body rows. Every lane
    // therefore takes the same branch and the fallback wave remains converged.
    if (bootstrap) {
        checkBootstrapGridCoverage(node, key, source, valid);
    } else {
        float2 sandDisplacement = float2(0.0);
        float4 state;
        if (FirstStage) state = contactGridState<Cooperative>(node, key, source, valid);
        else state = projectGridContacts<Cooperative, true>(node, key, source, valid, sandDisplacement);
        if (valid) {
            float2 d=state.xy;
            float mass=state.z;
            s_src[tile]=checkedEncodeNode(d.x,key,0u);
            s_src[tile+1]=checkedEncodeNode(d.y,key,1u);
            s_src[tile+2]=checkedEncodeNode(mass,key,2u);
            s_src[tile+3]=checkedEncodeNode(decodeWord(source.w),key,3u);
            if (!FirstStage) {
                // Reuse exact source quantization unless terrain friction made
                // the sand candidate distinct. No extra error or rounding path
                // enters sand-free or frictionless execution.
                if (all(sandDisplacement == d)) {
                    s_sandVelocity[lane] = float2(decodeWord(s_src[tile]), decodeWord(s_src[tile+1]));
                } else {
                    s_sandVelocity[lane] = float2(
                        decodeWord(checkedEncodeNode(sandDisplacement.x, key, 0u)),
                        decodeWord(checkedEncodeNode(sandDisplacement.y, key, 1u)));
                }
            }
        } else if (!FirstStage) {
            s_sandVelocity[lane] = float2(0.0);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (lane<bucket.count) {
        uint row=g_rows[bucket.start+lane];
        bool propertiesUsable=particlePropertiesUsable(row);
        Particle p=loadParticle(row);
        float2 oldPosition=float2(p.positionX,p.positionY);
        Weights weights=quadraticWeights(oldPosition);
        if (!FirstStage && propertiesUsable) {
            SourceMatrix B=zeroMatrix();
            float2 d=float2(0); float volume=0;
            for (int i=0;i<3;++i) {
                for (int j=0;j<3;++j) {
                    float weight=weights.w[i].x*weights.w[j].y;
                    int2 neighbour=weights.base+int2(i,j), localNeighbour=neighbour-origin;
                    if (any(localNeighbour<0) || any(localNeighbour>=8)) {
                        uint invalidKey=uint(neighbour.y)*g_parameters.gridWidth+uint(neighbour.x);
                        recordError(128u,invalidKey,0xffffffffu,row);
                        continue;
                    }

                    uint at=(uint(localNeighbour.y)*8+uint(localNeighbour.x))*4;
                    float2 nodeDisplacement = float2(decodeWord(s_src[at]), decodeWord(s_src[at+1]));
                    if (p.material == MaterialSand) nodeDisplacement = s_sandVelocity[at/4u];
                    float2 weighted=weight*nodeDisplacement;
                    float2 gatherOffset=float2(neighbour)-oldPosition+0.5;
                    float dbx0=weighted.x*gatherOffset.x, dby0=weighted.y*gatherOffset.x;
                    float dbx1=weighted.x*gatherOffset.y, dby1=weighted.y*gatherOffset.y;
                    B=matrix(B.c0x+dbx0,B.c0y+dby0,B.c1x+dbx1,B.c1y+dby1); d+=weighted;
                    if (g_parameters.useGridVolumeForLiquid!=0) volume+=weight*decodeWord(s_src[at+3]);
                }
            }

            float referenceVolume = volume;
            if (g_parameters.useGridVolumeForLiquid!=0) {
                // Quantization can leave zero support at this node. An empty
                // reciprocal would otherwise be an accidental infinity that the
                // comparison happens to discard; with no support there is no
                // objective-volume feedback and the density stays unchanged.
                if (volume > 0.0) {
                    volume=1.0/volume;
                    if (volume<1.0) p.liquidDensity=lerp(p.liquidDensity,volume,0.1);
                }
            }
            p.deformationDisplacement=scale(B,4.0); p.displacementX=d.x; p.displacementY=d.y;
            if (g_parameters.reserved0 + 1u >= g_parameters.reserved1) {
                // Integration carries gravity and external acceleration, so it
                // runs exactly once per substep: on the final stage. Unusable
                // constitutive operands are a per-particle rejection: no
                // integration, no engine pressure feedback, no early return.
                if (deformationOperandsUsable(p,row)) {
                    p=integration(p,row);
                    // Like retained gravity translation, pressure acts through the
                    // next transfer. Current position and return-mapped F stay intact.
                    applySandVolumeImpulse(p, referenceVolume);
                }
            }
            if (validParticle(p,row)) saveParticle(row,p);
        }

        Particle updated=p;
        bool scatterable=propertiesUsable && deformationOperandsUsable(p,row);
        if (scatterable) {
            updated=constraint(p,row);
            scatterable=producedStateUsable(updated,row);
        }
        if (scatterable) {
            for (int i=0;i<3;++i) {
                for (int j=0;j<3;++j) {
                    float weight=weights.w[i].x*weights.w[j].y;
                    int2 neighbour=weights.base+int2(i,j), localNeighbour=neighbour-origin;
                    if (any(localNeighbour<0) || any(localNeighbour>=8)) {
                        uint invalidKey=uint(neighbour.y)*g_parameters.gridWidth+uint(neighbour.x);
                        recordError(128u,invalidKey,0xffffffffu,row);
                        continue;
                    }

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
    }

    GroupMemoryBarrierWithGroupSync();

    if (valid) {
        uint word;
        if (sparseWord(key,g_sparse.destinationA!=0u,g_sparse.generation,true,word)) {
            if (g_sparse.destinationA!=0u) {
                for (uint field=0;field<4;++field) {
                    if (reserveWorkgroupMerge(tile+field,word+field,key,field))
                        InterlockedAdd(g_payloadA[word+field],s_dst[tile+field]);
                }
            }
            else {
                for (uint field=0;field<4;++field) {
                    if (reserveWorkgroupMerge(tile+field,word+field,key,field))
                        InterlockedAdd(g_payloadB[word+field],s_dst[tile+field]);
                }
            }
        }
    }
}

// ------------------------------------------------------------------
// two runtime-parameterised stage entries
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(64,1,1)]
void fused0(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<true, false>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused1(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<false, false>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused0Wave(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<true, true>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused1Wave(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<false, true>(lane,group);
}
