// Copyright (c) 2024 Electronic Arts Inc. All rights reserved.
// Adapted from electronicarts/pbmpm, commit 5a2f438b7115741c41ed429a2a0360d0e5f4b5a9.
// BSD-3-Clause terms are reproduced in LICENSE.md beside this file.

// ------------------------------------------------------------------
// explicit storage protocol
// ------------------------------------------------------------------

// These are source COLUMN components, not a Slang matrix. Scalar storage makes
// the source algebra independent of Slang's row-major engine matrix convention.
struct SourceMatrix {
    float c0x;
    float c0y;
    float c1x;
    float c1y;
};

// Local solver aggregate for the shared helpers. This is NOT a structured-buffer
// element: the device stores the split ParticleWrite/ParticleReadonly row pair.
struct Particle {
    float positionX;
    float positionY;
    float displacementX;
    float displacementY;
    SourceMatrix deformationGradient;
    SourceMatrix deformationDisplacement;
    float liquidDensity;
    float mass;
    float material;
    float volume;
    float lambda;
    float logJp;
    float colorR;
    float colorG;
    float colorB;
    float enabled;
};

// Mutable 64-byte device row: position, displacement, F, D and the liquid
// scalars, in the solver's hot write order.
struct ParticleWrite {
    float positionX;
    float positionY;
    float displacementX;
    float displacementY;
    SourceMatrix deformationGradient;
    SourceMatrix deformationDisplacement;
    float liquidDensity;
    float logJp;
    float lambda;
    float enabled;
};

// Immutable 32-byte device row. Both reserved words are uploaded as zero and are
// never loaded or stored by the solver.
struct ParticleReadonly {
    float mass;
    float volume;
    float material;
    float reserved0;
    float colorR;
    float colorG;
    float colorB;
    float reserved1;
};

// Only admitted collider functionality; browser emitters/drains are absent.
struct StaticShape {
    float positionX;
    float positionY;
    float halfSizeX;
    float halfSizeY;
    float radius;
    float rotationDegrees;
    uint shapeType;
    uint reserved;
};

// Four complete 16-byte constant-buffer rows. Counts replace browser counters;
// external uploads preserve stable IDs in the owner's separate metadata ledger.
struct Parameters {
    uint gridWidth;
    uint gridHeight;
    uint particleCount;
    uint shapeCount;
    float deltaTime;
    float elasticityRatio;
    float gravityStrength;
    float liquidRelaxation;
    float elasticRelaxation;
    float liquidViscosity;
    uint fixedPointMultiplier;
    uint useGridVolumeForLiquid;
    float frictionAngle;
    float plasticity;
    uint reserved0;
    uint reserved1;
};

// ------------------------------------------------------------------
// contour terrain and predicted-body extension protocol
// ------------------------------------------------------------------

struct ContourPoint {
    float x;
    float y;
};

struct ContourSegment {
    uint begin;
    uint end;
};

struct ContourTriangle {
    uint a;
    uint b;
    uint c;
    uint solidCorners;
};

struct ContourCase {
    uint segmentCount;
    uint triangleCount;
    ContourSegment segments[2];
    ContourTriangle triangles[3];
};

struct BodyBoundary {
    StaticShape shape;
    float comX;
    float comY;
    float velocityX;
    float velocityY;
    float angularVelocity;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

struct Environment {
    uint terrainWidth;
    uint terrainHeight;
    uint bodyCount;
    uint bodyBase;
    float terrainOriginX;
    float terrainOriginY;
    uint hasTerrain;
    uint reserved;
};

static const float MaterialLiquid = 0.0;
static const float MaterialElastic = 1.0;
static const float MaterialSand = 2.0;
static const float MaterialVisco = 3.0;
static const uint ShapeTypeBox = 0;
static const uint ShapeTypeCircle = 1;

// ------------------------------------------------------------------
// complete particle publication contract
// ------------------------------------------------------------------

// Matches the CPU upload admission oracle. Every evolving and carried scalar is
// checked; kernels never reorder rows, append records, or change stable metadata.
uint invalidParticleField(Particle p, Parameters parameters) {
    float fields[22] = {
        p.positionX, p.positionY, p.displacementX, p.displacementY,
        p.deformationGradient.c0x, p.deformationGradient.c0y, p.deformationGradient.c1x, p.deformationGradient.c1y,
        p.deformationDisplacement.c0x, p.deformationDisplacement.c0y,
        p.deformationDisplacement.c1x, p.deformationDisplacement.c1y,
        p.liquidDensity, p.mass, p.material, p.volume, p.lambda, p.logJp,
        p.colorR, p.colorG, p.colorB, p.enabled};
    for (uint field = 0u; field < 22u; ++field) {
        if (!isfinite(fields[field])) return field;
    }

    if (p.positionX < 3.0 || p.positionX > float(parameters.gridWidth) - 4.0) return 0u;
    if (p.positionY < 3.0 || p.positionY > float(parameters.gridHeight) - 4.0) return 1u;
    if (p.liquidDensity <= 0.0) return 12u;
    if (p.mass <= 0.0) return 13u;
    if (p.material != MaterialLiquid && p.material != MaterialElastic && p.material != MaterialSand && p.material != MaterialVisco) return 14u;
    if (p.volume <= 0.0) return 15u;
    if (p.enabled != 1.0) return 21u;

    return 0xffffffffu;
}

// ------------------------------------------------------------------
// source column algebra
// ------------------------------------------------------------------

SourceMatrix matrix(float c0x, float c0y, float c1x, float c1y) {
    SourceMatrix r = {c0x, c0y, c1x, c1y};
    return r;
}

SourceMatrix identity() { return matrix(1.0, 0.0, 0.0, 1.0); }

SourceMatrix zeroMatrix() { return matrix(0.0, 0.0, 0.0, 0.0); }

SourceMatrix add(SourceMatrix a, SourceMatrix b) {
    return matrix(a.c0x + b.c0x, a.c0y + b.c0y, a.c1x + b.c1x, a.c1y + b.c1y);
}

SourceMatrix scale(SourceMatrix a, float s) {
    return matrix(a.c0x * s, a.c0y * s, a.c1x * s, a.c1y * s);
}

SourceMatrix subtract(SourceMatrix a, SourceMatrix b) { return add(a, scale(b, -1.0)); }

float2 transform(SourceMatrix a, float2 v) {
    return float2(a.c0x * v.x + a.c1x * v.y, a.c0y * v.x + a.c1y * v.y);
}

SourceMatrix product(SourceMatrix a, SourceMatrix b) {
    float2 c0 = transform(a, float2(b.c0x, b.c0y));
    float2 c1 = transform(a, float2(b.c1x, b.c1y));
    return matrix(c0.x, c0.y, c1.x, c1.y);
}

SourceMatrix transposed(SourceMatrix a) { return matrix(a.c0x, a.c1x, a.c0y, a.c1y); }

float determinant(SourceMatrix a) { return a.c0x * a.c1y - a.c0y * a.c1x; }

float trace(SourceMatrix a) { return a.c0x + a.c1y; }

SourceMatrix inverted(SourceMatrix a) {
    return scale(matrix(a.c1y, -a.c0y, -a.c1x, a.c0x), 1.0 / determinant(a));
}

SourceMatrix rotation(float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return matrix(c, s, -s, c);
}

SourceMatrix diagonal(float2 d) { return matrix(d.x, 0.0, 0.0, d.y); }

SourceMatrix outer(float2 x, float2 y) {
    return matrix(x.x * y.x, x.y * y.x, x.x * y.y, x.y * y.y);
}

struct SvdResult {
    SourceMatrix U;
    float2 Sigma;
    SourceMatrix Vt;
};

// ------------------------------------------------------------------
// source shape and interpolation helpers
// ------------------------------------------------------------------

struct Contact {
    bool collides;
    float penetration;
    float2 normal;
    float2 point;
};

Contact collide(StaticShape shape, float2 position) {
    float2 center = float2(shape.positionX, shape.positionY);
    Contact result = {false, 0.0, float2(0.0), float2(0.0)};
    if (shape.shapeType == ShapeTypeCircle) {
        float2 offset = center - position;
        float distance = length(offset);
        float2 normal = offset * (distance == 0.0 ? 0.0 : 1.0 / distance);
        result.collides = distance <= shape.radius;
        result.penetration = -(distance - shape.radius);
        result.normal = normal;
        result.point = center + normal * shape.radius;
        return result;
    }

    if (shape.shapeType == ShapeTypeBox) {
        SourceMatrix R = rotation(shape.rotationDegrees / 180.0 * 3.14159);
        float2 local = transform(R, position - center);
        float2 halfSize = float2(shape.halfSizeX, shape.halfSizeY);
        float2 penetration = -(abs(local) - halfSize);
        float2 normal = penetration.y < penetration.x ? float2(0.0, sign(local.y)) : float2(sign(local.x), 0.0);
        result.collides = min(penetration.x, penetration.y) > 0.0;
        result.penetration = min(penetration.x, penetration.y);
        result.normal = -transform(transposed(R), normal);
        result.point = center + transform(transposed(R), clamp(local, -halfSize, halfSize));
    }

    return result;
}

float2 guardian(float2 p, uint2 size, float padding) {
    return clamp(p, float2(padding), float2(size) - float2(padding) - float2(1.0));
}

struct Weights {
    float2 w[3];
    int2 base;
};

Weights quadraticWeights(float2 p) {
    float2 down = floor(p);
    float2 offset = (p - down) - 0.5;
    float2 lower = 0.5 - offset;
    float2 upper = 0.5 + offset;
    Weights result;
    result.w[0] = 0.5 * (lower * lower);
    result.w[1] = 0.75 - offset * offset;
    result.w[2] = 0.5 * (upper * upper);
    result.base = int2(down - 1.0);
    return result;
}
