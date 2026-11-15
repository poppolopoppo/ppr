// Copyright (c) 2024 Electronic Arts Inc. All rights reserved.
// Selected fork ErikSom/pbmpm 859c449; BSD-3-Clause notice: LICENSE.md.
#include "types.slang"
#define PBMPM_ENGINE_ADAPTERS 1
#define PBMPM_SPARSE_STORAGE 1
#include "fork_core.slang"

// ------------------------------------------------------------------
// engine transaction error and contact adapters
// ------------------------------------------------------------------
void recordError(uint flag, uint node, uint field, uint particle, uint body) {
    InterlockedOr(g_errors[0], flag);
    uint previous;
    InterlockedCompareExchange(g_errors[1], 0u, 1u, previous);
    if (previous == 0u) {
        g_errors[2] = node;
        g_errors[3] = field;
        g_errors[4] = particle;
        g_errors[5] = body;
    }
}

void recordError(uint flag, uint node, uint field, uint particle) {
    recordError(flag, node, field, particle, 0xffffffffu);
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

// forkSvd squares E,F,G,H, each a half-sum or half-difference of two gradient
// entries, so a squared term is at most max_entry^2 and the radicand E^2 + H^2
// (or F^2 + G^2) at most 2 * max_entry^2. At 1.3e19 that radicand is
// 2 * 1.69e38 = 3.38e38, below FLT_MAX = 3.4028e38, so the squares and both
// sqrts stay finite; a magnitude above it has no finite decomposition input.
// Deliberate singular-value limiting happens after this point, on finite values,
// so it is never re-inspected here.
static const float kMaxDecompositionEntry = 1.3e19;

bool usableMagnitude(SourceMatrix m) {
    float largest = max(max(abs(m.c0x), abs(m.c0y)), max(abs(m.c1x), abs(m.c1y)));
    return largest < kMaxDecompositionEntry;
}

bool usableDeformation(SourceMatrix m) {
    return finiteMatrix(m) && usableMagnitude(m) && usableInverse(m);
}

// Records the first unusable constitutive operand for this row and reports
// whether the production constraint/integration may run. Stored F is field 4 and
// the trial (I + D)F is field 8 of the validation record. Liquids read neither.
bool deformationOperandsUsable(Particle p, uint row) {
    if (p.material == MaterialLiquid) return true;

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
void fusedIteration<let Iteration : uint, let Cooperative : bool>(uint lane,uint3 group) {
    uint groupIndex=sparseGroup(group);
    if (groupIndex>=g_sparseCounters[2]) return;

    for (uint i=lane;i<256;i+=64) {
        if (Iteration != 0u) s_src[i]=0;
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
    if (Iteration == 0u) bootstrap = bootstrapGridCertified();

    int4 source=int4(0);
    if (valid && (!bootstrap || (g_environment.hasTerrain != 0u && (g_environment.reserved & 1u) == 0u))) {
        bool sourceA=g_sparse.destinationA==0u;
        uint generation=sourceA ? g_sparse.sourceGeneration : g_sparse.generation;
        source=sparseSource(key,sourceA,generation);
    }

    // The certificate depends only on uniform constants/body rows. Every lane
    // therefore takes the same branch and the fallback wave remains converged.
    if (bootstrap) {
        checkBootstrapGridCoverage(node, key, source, valid);
    } else {
        float2 sandDisplacement = float2(0.0);
        float4 state;
        if (Iteration == 0u) state = contactGridState<Cooperative>(node, key, source, valid);
        else state = projectGridContacts<Cooperative, true>(node, key, source, valid, sandDisplacement);
        if (valid) {
            float2 d=state.xy;
            float mass=state.z;
            s_src[tile]=checkedEncodeNode(d.x,key,0u);
            s_src[tile+1]=checkedEncodeNode(d.y,key,1u);
            s_src[tile+2]=checkedEncodeNode(mass,key,2u);
            s_src[tile+3]=checkedEncodeNode(decodeWord(source.w),key,3u);
            if (Iteration != 0u) {
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
        } else if (Iteration != 0u) {
            s_sandVelocity[lane] = float2(0.0);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (lane<bucket.count) {
        uint row=g_rows[bucket.start+lane];
        Particle p=loadParticle(row);
        float2 oldPosition=float2(p.positionX,p.positionY);
        Weights weights=quadraticWeights(oldPosition);
        if (Iteration!=0) {
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
                volume=1.0/volume;
                if (volume<1.0) p.liquidDensity=lerp(p.liquidDensity,volume,0.1);
            }
            p.deformationDisplacement=scale(B,4.0); p.displacementX=d.x; p.displacementY=d.y;
            if (Iteration==1) {
                // Unusable constitutive operands are a per-particle rejection:
                // no integration, no engine pressure feedback, no early return.
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
        if (deformationOperandsUsable(p,row)) updated=constraint(p);
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
// fixed K2 iteration entries
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(64,1,1)]
void fused0(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<0u, false>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused1(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<1u, false>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused0Wave(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<0u, true>(lane,group);
}

[shader("compute")]
[numthreads(64,1,1)]
void fused1Wave(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    fusedIteration<1u, true>(lane,group);
}
