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

void engineEndpointContacts(float2 position, inout float2 d, uint row) {
    Contact terrain=terrainContact(position,0xffffffffu,row);
    if (terrain.collides) d-=terrain.penetration*terrain.normal;

    for (uint body=0u;body<g_environment.bodyCount;++body) {
        BodyBoundary boundary=g_bodies[g_environment.bodyBase+body];
        Contact contact=collide(boundary.shape,position);
        if (!contact.collides) continue;

        d-=contact.penetration*contact.normal;
    }
}

// Grid contact is one production operation, also inspected by the test-only
// contact entry. Source shapes, engine terrain/bodies, then guardian, in order.
float4 contactGridState<let Cooperative : bool>(int2 node, uint key, int4 source, bool valid) {
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

    float2 terrainEndpoint = float2(node) + d;
    Contact terrain = {false, 0.0, float2(0.0), float2(0.0)};
    if (Cooperative) {
        terrain = terrainContactWave(terrainEndpoint, valid && mass > 0.0, key, 0xffffffffu);
    } else if (valid && mass > 0.0) {
        terrain = terrainContact(terrainEndpoint, key, 0xffffffffu);
    }

    // Extensions apply only where encoded mass represents material. Empty
    // unsupported nodes neither query terrain nor query moving body boundaries.
    if (valid && mass > 0) {
        if (terrain.collides) {
            // Explicit contour adaptation: the response point is the queried
            // endpoint, matching the source unrotated box's interior semantics.
            // The geometric closest point is retained separately by the query.
            float gap = min(0.0, dot(terrain.normal, terrainEndpoint - float2(node)));
            float penetration = dot(terrain.normal, d) - gap;
            d -= max(penetration, 0.0) * terrain.normal;
        }

        for (uint body = 0u; body < g_environment.bodyCount; ++body) {
            BodyBoundary boundary = g_bodies[g_environment.bodyBase + body];
            Contact contact = collide(boundary.shape, float2(node) + d);
            if (!contact.collides) continue;

            float2 db = bodyDisplacement(boundary, contact.point);
            float2 relative = d - db;
            float gap = min(0.0, dot(contact.normal, contact.point - (float2(node) + db)));
            float penetration = dot(contact.normal, relative) - gap;
            relative -= max(penetration, 0.0) * contact.normal;
            d = relative + db;
        }
    }

    if (valid) {
        float2 endpoint=float2(node)+d;
        float2 correction=guardian(endpoint,uint2(g_parameters.gridWidth,g_parameters.gridHeight),4.0)-endpoint;
        if (correction.x!=0) { d.x=0; d.y=lerp(d.y,0.0,0.5); }
        if (correction.y!=0) { d.y=0; d.x=lerp(d.x,0.0,0.5); }
    }

    return float4(d,mass,decodeWord(source.w));
}

float4 contactGridState(int2 node, uint key, int4 source) {
    return contactGridState<false>(node, key, source, true);
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
    checkTerrainCoverage(float2(node) + d, key);
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
        float4 state=contactGridState<Cooperative>(node,key,source,valid);
        if (valid) {
            float2 d=state.xy;
            float mass=state.z;
            s_src[tile]=checkedEncodeNode(d.x,key,0u);
            s_src[tile+1]=checkedEncodeNode(d.y,key,1u);
            s_src[tile+2]=checkedEncodeNode(mass,key,2u);
            s_src[tile+3]=checkedEncodeNode(decodeWord(source.w),key,3u);
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
            if (Iteration==1) p=integration(p,row);
            if (validParticle(p,row)) saveParticle(row,p);
        }

        Particle updated=constraint(p);
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
