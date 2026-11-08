// Included only by radiance_cascades; geometry entry programs are linked PRECISE.
#include "grid_contour_math.slang"

struct TerrainVertex { float roundness; uint material; };
struct TerrainParameters {
    uint outsideMaterial;
    float outsideRoundness, outsideR, outsideG;
    float outsideB;
    uint reserved0, reserved1, reserved2;
};
ConstantBuffer<TerrainParameters> g_terrain;
StructuredBuffer<TerrainVertex> g_terrain_vertices;

// ------------------------------------------------------------------
// local intersection certificates for normal FP32 arithmetic
// ------------------------------------------------------------------

static const uint kTerrainNumericAmbiguity = 32;

// The low byte is the unchanged operational status. Reason bits are unions,
// not a first-fault index: local bits 8..14, inherited parent bits 16..22.
// A direct/top trace uses only the local lane. Merge folds both parent lanes
// into inherited while preserving this invocation's connector lane.
static const uint kTerrainReasonClip = 1u << 8;
static const uint kTerrainReasonOwnership = 1u << 9;
static const uint kTerrainReasonDda = 1u << 10;
static const uint kTerrainReasonRoots = 1u << 11;
static const uint kTerrainReasonOrder = 1u << 12;
static const uint kTerrainReasonSource = 1u << 13;
static const uint kTerrainReasonArithmetic = 1u << 14;
static const uint kTerrainLocalReasonMask = 0x00007f00u;
static const uint kTerrainInheritedReasonMask = 0x007f0000u;

// ORDER sites union across all dependencies, independently of C0's selected
// first upper-ORDER dependency. They do not uniquely identify that dependency.
static const uint kTerrainOrderRequestedSpan = 1u << 24;
static const uint kTerrainOrderRootSpan = 1u << 25;
static const uint kTerrainOrderCutSeparation = 1u << 26;
static const uint kTerrainOrderMiddle = 1u << 27;
static const uint kTerrainOrderSiteMask = 0x0f000000u;

bool terrainNormal(float x) {
    uint bits = asuint(x) & 0x7fffffffu;
    return bits == 0 || (bits >= 0x00800000u && bits < 0x7f800000u);
}

// ------------------------------------------------------------------
// implicit forms of the existing quadratic corner arcs
// ------------------------------------------------------------------

struct TerrainCurve { uint corner; float roundness; uint lineAxis; uint reserved; };

void terrainCorners(int2 base, out uint labels[4], out float roundness[4], out float3 sources[4]) {
    for (uint corner = 0; corner < 4; ++corner) {
        int2 sample = base + int2(g_contour_points[corner]);
        if (any(sample < 0) || sample.x >= int(g_trace.width) || sample.y >= int(g_trace.height)) {
            labels[corner] = g_terrain.outsideMaterial;
            roundness[corner] = g_terrain.outsideRoundness;
            sources[corner] = float3(g_terrain.outsideR, g_terrain.outsideG, g_terrain.outsideB);
        } else {
            uint index = uint(sample.y) * g_trace.width + uint(sample.x);
            TerrainVertex vertex = g_terrain_vertices[index];
            OpticalCell cell = g_cells[index];
            labels[corner] = vertex.material;
            roundness[corner] = vertex.roundness;
            sources[corner] = float3(cell.surfaceR, cell.surfaceG, cell.surfaceB);
        }
    }
}

float terrainRoundness(uint label, uint labels[4], float roundness[4]) {
    for (uint i = 0; i < 4; ++i) if (labels[i] == label) return roundness[i];
    return 0.0;
}

float terrainSharedSourceRoundness(uint labels[4], float roundness[4], uint second) {
    return contourMeanRoundness(roundness[0], terrainRoundness(second, labels, roundness));
}

float terrainEndpoint(uint labels[4], float roundness[4], float2 point) {
    uint i = point.y == 0.0 ? 0u : point.x == 1.0 ? 1u : point.y == 1.0 ? 2u : 3u;
    return labels[i] != 0 ? roundness[i] : roundness[(i + 1u) & 3u];
}

uint terrainCurves(uint labels[4], float roundness[4], out TerrainCurve curves[2], out bool complement) {
    uint mask = occupiedMask(labels);
    complement = false;
    if (mask == 0 || mask == 15) return 0;
    bool bridge = occupiedSaddleConnected(labels, mask);
    if ((mask == 5 || mask == 10) && !bridge) {
        uint count = 0;
        for (uint corner = 0; corner < 4; ++corner) if (labels[corner] != 0) {
            TerrainCurve curve = {corner, roundness[corner], 2, 0}; curves[count++] = curve;
        }
        return count;
    }
    uint shapeMask = bridge ? 15u ^ mask : mask;
    ContourCase shape = g_contour_cases[shapeMask];
    complement = bridge || shapeMask == 7 || shapeMask == 11 || shapeMask == 13 || shapeMask == 14;
    for (uint i = 0; i < shape.m_segment_count; ++i) {
        uint2 edge = shape.m_segments[i];
        float2 a = g_contour_points[edge.x], b = g_contour_points[edge.y];
        TerrainCurve curve = {0, 0.0, 2, 0};
        if (a.x == b.x) { curve.lineAxis = 0; curve.corner = (mask & 1u) != 0 ? 0 : 1; }
        else if (a.y == b.y) { curve.lineAxis = 1; curve.corner = (mask & 1u) != 0 ? 0 : 2; }
        else {
            uint cornerMask = shapeMask;
            if (shapeMask == 5 || shapeMask == 10) cornerMask = shape.m_triangles[i].m_solid_corners;
            if (shapeMask == 7 || shapeMask == 11 || shapeMask == 13 || shapeMask == 14) cornerMask = 15u ^ shapeMask;
            for (uint c = 0; c < 4; ++c) if ((cornerMask & (1u << c)) != 0) { curve.corner = c; break; }
            curve.roundness = contourMeanRoundness(terrainEndpoint(labels, roundness, a), terrainEndpoint(labels, roundness, b));
        }
        curves[i] = curve;
    }
    return shape.m_segment_count;
}

uint terrainHitLabel(uint labels[4], float roundness[4], float2 point, inout uint status) {
    if (labels[0] == labels[1] && labels[0] == labels[2] && labels[0] == labels[3]) return labels[0];
    bool shared = occupiedMask(labels) == 15;
    uint second = 0;
    for (uint i = 0; i < 4; ++i) if (labels[i] != labels[0]) {
        if (second != 0 && labels[i] != second) shared = false;
        second = labels[i];
    }
    shared = shared && second != 0;
    float sharedRoundness = terrainSharedSourceRoundness(labels, roundness, second);
    uint visited = 0, winner = 0;
    float best = -1.0e30;
    for (uint corner = 0; corner < 4; ++corner) {
        uint label = labels[corner];
        if (label == 0 || (visited & (1u << corner)) != 0) continue;
        uint mask = materialMask(labels, corner);
        visited |= mask;
        bool bridge = connectedSaddle(labels, corner, mask);

        float scoreRoundness = shared ? sharedRoundness : roundness[corner];
        if (!terrainNormal(scoreRoundness)) {
            status |= kTerrainNumericAmbiguity | kTerrainReasonSource;
            return 0;
        }

        float2 gradient;
        float score = contourScore(point, bridge ? 15u ^ mask : mask, scoreRoundness, bridge, gradient);
        if (winner != 0 && score <= best) continue;
        winner = label; best = score;
    }
    return winner;
}

// ------------------------------------------------------------------
// symbolic RC endpoints and parametric line enclosures
// ------------------------------------------------------------------

// A point means probe+nearCells*rawDirection. These are local function values,
// not a new uploaded ABI. Parent targets and the next level's starts reuse this
// expression; no rounded endpoint or independent normalization is introduced.
struct TerrainPoint { float2 probe; float2 rawDirection; float nearCells; };
struct TerrainLine { TerrainPoint origin; TerrainPoint target; float2 rawDirection; uint connector; };
struct TerrainWitness { int2 base; uint curve; uint enabled; };

bool terrainValidPoint(TerrainPoint point) {
    if (!terrainNormal(point.probe.x) || !terrainNormal(point.probe.y) ||
        !terrainNormal(point.rawDirection.x) || !terrainNormal(point.rawDirection.y) ||
        !terrainNormal(point.nearCells) || point.nearCells < 0.0) return false;

    // Literal physical-ray origins keep their existing finite range. Nonliteral
    // RC expressions stay in the admitted half-grid domain, so even cancelled
    // common products denote bounded finite points before identity is selected.
    return point.nearCells == 0.0 || (point.nearCells <= 65536.0 &&
        all(abs(point.probe) <= 65536.0) && all(abs(point.rawDirection) <= 1.0));
}

// ------------------------------------------------------------------
// precision policies over one complete geometry query
// ------------------------------------------------------------------

namespace Terrain32 {
#define TERRAIN_COMPLETE_QUERY_BOUNDARY
#include "terrain_precision32.slang"
#include "terrain_query_shared.slang"
#undef TERRAIN_COMPLETE_QUERY_BOUNDARY
}

#if defined(PPR_TERRAIN_FP64_REFINEMENT)
bool terrainCanonicalInputNormal(float value) { return terrainNormal(value); }

namespace Terrain64 {
#define TERRAIN_COMPLETE_QUERY_BOUNDARY [noinline]
#include "terrain_precision64.slang"
#include "terrain_query_shared.slang"
#undef TERRAIN_COMPLETE_QUERY_BOUNDARY
}
#endif

// These thin seams retain the existing FP32 receiver-weight and metric arithmetic.
float2 terrainAdd(float2 a, float2 b, inout uint status) { return Terrain32::terrainAdd(a, b, status); }
float2 terrainMul(float2 a, float2 b, inout uint status) { return Terrain32::terrainMul(a, b, status); }
float2 terrainDiv(float2 a, float2 b, inout uint status) { return Terrain32::terrainDiv(a, b, status); }
float2 terrainSqrt(float2 a, inout uint status) { return Terrain32::terrainSqrt(a, status); }
float2 terrainPointCoordinate(TerrainPoint p, uint axis, float reference, inout uint status) {
    return Terrain32::terrainPointCoordinate(p, axis, reference, status);
}

#if defined(PPR_TERRAIN_FP64_REFINEMENT)
#include "terrain_precision_refinement.slang"
#endif

OpticalResult traceTerrainParametricWitness(TerrainLine line, float2 requestedNear,
    float2 requestedFar, uint initialStatus, TerrainWitness witness) {
    OpticalResult first = Terrain32::traceTerrainParametricWitness(line, requestedNear, requestedFar, initialStatus, witness);
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    if (terrainRefinementEligible(first.status, initialStatus)) {
        OpticalResult refined = Terrain64::traceTerrainParametricWitness(line,
            double2(requestedNear), double2(requestedFar), 0, witness);
        return terrainSettleRefinement(first, refined);
    }
#endif
    return first;
}

OpticalResult traceTerrainParametric(TerrainLine line, float2 requestedNear, float2 requestedFar, uint initialStatus) {
    TerrainWitness none = {int2(0), 0, 0};
    return traceTerrainParametricWitness(line, requestedNear, requestedFar, initialStatus, none);
}

OpticalResult traceTerrainRay(OpticalRay ray) {
    OpticalResult invalid = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, kTerrainNumericAmbiguity | kTerrainReasonArithmetic, 0};
    // Preserve the physical-metre wrapper, including raw bit validation before
    // any empty/axis identity and metric conversion by the actual raw length.
    if (!terrainNormal(ray.originX) || !terrainNormal(ray.originY) ||
        !terrainNormal(ray.directionX) || !terrainNormal(ray.directionY) ||
        !terrainNormal(ray.nearMetres) || !terrainNormal(ray.farMetres) || !terrainNormal(g_trace.cellMetres)) return invalid;
    if (ray.nearMetres == ray.farMetres) {
        OpticalResult identity = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0, 0};
        return identity;
    }

    uint status = 0;
    float2 direction = float2(ray.directionX, ray.directionY);
    float2 lengthSquared = terrainAdd(terrainMul(float2(direction.x), float2(direction.x), status),
        terrainMul(float2(direction.y), float2(direction.y), status), status);
    float2 metric = terrainMul(float2(g_trace.cellMetres), terrainSqrt(lengthSquared, status), status);
    float2 near = terrainDiv(float2(ray.nearMetres), metric, status);
    float2 far = terrainDiv(float2(ray.farMetres), metric, status);
    TerrainPoint origin = {float2(ray.originX, ray.originY), 0.0, 0.0};
    TerrainLine line = {origin, origin, direction, 0};
    TerrainWitness none = {int2(0), 0, 0};
    OpticalResult first = Terrain32::traceTerrainParametricWitness(line, near, far, status, none);
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    if (terrainRefinementEligible(first.status, status)) {
        uint refinedStatus = 0;
        double2 rawDirection = double2(ray.directionX, ray.directionY);
        double2 lengthSquared64 = Terrain64::terrainAdd(
            Terrain64::terrainMul(double2(rawDirection.x), double2(rawDirection.x), refinedStatus),
            Terrain64::terrainMul(double2(rawDirection.y), double2(rawDirection.y), refinedStatus), refinedStatus);
        double2 metric64 = Terrain64::terrainMul(double2(g_trace.cellMetres),
            Terrain64::terrainSqrt(lengthSquared64, refinedStatus), refinedStatus);
        double2 near64 = Terrain64::terrainDiv(double2(ray.nearMetres), metric64, refinedStatus);
        double2 far64 = Terrain64::terrainDiv(double2(ray.farMetres), metric64, refinedStatus);
        OpticalResult refined = Terrain64::traceTerrainParametricWitness(line, near64, far64, refinedStatus, none);
        return terrainSettleRefinement(first, refined);
    }
#endif
    return first;
}

// ------------------------------------------------------------------
// isolated coefficient qualification through actual terrain consumers
// ------------------------------------------------------------------

struct TerrainRoundnessProbeParameters { uint count, reserved0, reserved1, reserved2; };
ConstantBuffer<TerrainRoundnessProbeParameters> g_contour_roundness_probe_params;
StructuredBuffer<uint2> g_contour_roundness_pairs;
RWStructuredBuffer<uint4> g_contour_roundness_probe;

[shader("compute")]
[numthreads(64, 1, 1)]
void qualifyTerrainContourRoundness(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_contour_roundness_probe_params.count) return;

    uint2 pair = g_contour_roundness_pairs[id.x];
    TerrainVertex a = g_terrain_vertices[pair.x], b = g_terrain_vertices[pair.y];
    uint outerLabels[4] = {a.material, b.material, b.material, 0};
    float outerRoundness[4] = {a.roundness, b.roundness, b.roundness, 0.0};
    TerrainCurve curves[2]; bool complement;
    uint count = terrainCurves(outerLabels, outerRoundness, curves, complement);

    uint sharedLabels[4] = {a.material, b.material, a.material, b.material};
    float sharedRoundness[4] = {a.roundness, b.roundness, a.roundness, b.roundness};
    float shared = terrainSharedSourceRoundness(sharedLabels, sharedRoundness, b.material);
    uint coefficient = count == 1 ? asuint(curves[0].roundness) : 0x7fc00000u;
    // Encoding coverage is separate from transport admission. These bits survive
    // even when an endpoint/coefficient is outside the normal arithmetic domain.
    uint status = !terrainNormal(a.roundness) || !terrainNormal(b.roundness) ||
        !terrainNormal(asfloat(coefficient)) || !terrainNormal(shared) ? kTerrainNumericAmbiguity : 0u;
    g_contour_roundness_probe[id.x] = uint4(coefficient, asuint(shared), count, status);
}

// Exact-view reconstruction uses this same complete cell query and the existing
// eligible local refinement policy. Source failures remain ineligible/visible.
OpticalResult traceTerrainBackgroundConnector(TerrainLine line, int2 base) {
    OpticalResult first = Terrain32::traceTerrainCellConnector(line, base);
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    if (terrainRefinementEligible(first.status, 0u)) {
        OpticalResult refined = Terrain64::traceTerrainCellConnector(line, base);
        return terrainSettleRefinement(first, refined);
    }
#endif
    return first;
}
