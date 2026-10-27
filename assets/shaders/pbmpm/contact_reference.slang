// Test-only frozen ring traversal before coarse boundary-tile acceleration.
// The wide canonical resources independently check the compact production decode.

StructuredBuffer<uint> g_reference_masks;
StructuredBuffer<ContourPoint> g_reference_points;
StructuredBuffer<ContourCase> g_reference_cases;

float2 referenceContourPoint(uint index) {
    ContourPoint p = g_reference_points[index];
    return float2(p.x, p.y);
}

ContourCase referenceTerrainCase(int2 cell) {
    return g_reference_cases[g_reference_masks[uint(cell.y) * g_environment.terrainWidth + uint(cell.x)]];
}

bool referenceContainsTerrain(ContourCase shape, float2 fraction) {
    for (uint index = 0u; index < shape.triangleCount; ++index) {
        ContourTriangle triangle = shape.triangles[index];
        float2 a = referenceContourPoint(triangle.a);
        float2 b = referenceContourPoint(triangle.b);
        float2 c = referenceContourPoint(triangle.c);
        float ab = cross2(b - a, fraction - a);
        float bc = cross2(c - b, fraction - b);
        float ca = cross2(a - c, fraction - c);
        if ((ab >= 0.0 && bc >= 0.0 && ca >= 0.0) || (ab <= 0.0 && bc <= 0.0 && ca <= 0.0)) return true;
    }

    return false;
}

void referenceNearestInCell(int2 cell, int2 home, float2 fraction, inout float bestSquared, inout float2 bestVector) {
    if (!terrainCellValid(cell)) return;

    ContourCase shape = referenceTerrainCase(cell);
    for (uint index = 0u; index < shape.segmentCount; ++index) {
        ContourSegment segment = shape.segments[index];
        float2 pointA = referenceContourPoint(segment.begin);
        float2 direction = referenceContourPoint(segment.end) - pointA;
        float2 a = (float2(cell - home) + pointA) - fraction;
        float t = clamp(dot(-a, direction) / dot(direction, direction), 0.0, 1.0);
        float2 nearest = a + t * direction;
        float distanceSquared = dot(nearest, nearest);
        if (distanceSquared < bestSquared) {
            bestSquared = distanceSquared;
            bestVector = nearest;
        }
    }
}

// ------------------------------------------------------------------
// frozen complete contour ring traversal
// ------------------------------------------------------------------

Contact referenceTerrainContact(float2 p, uint node, uint particle) {
    Contact result = {false, 0.0, float2(0.0), float2(0.0)};
    if (g_environment.hasTerrain == 0u) return result;

    float2 local = p - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    // Establish the finite integer conversion domain before floor/cast.
    if (!all(isfinite(local)) || any(local < float2(0.0)) ||
        local.x >= float(g_environment.terrainWidth) || local.y >= float(g_environment.terrainHeight)) return result;

    int2 home = int2(floor(local));
    float2 fraction = local - float2(home);
    if (!referenceContainsTerrain(referenceTerrainCase(home), fraction)) return result;

    float bestSquared = 3.402823466e38;
    float2 bestVector = float2(0.0);
    uint extent = max(g_environment.terrainWidth, g_environment.terrainHeight);
    for (int radius = 0; radius <= int(extent); ++radius) {
        // Only the new ring is visited. Segments stay inside their owning unit
        // cells, so distance to the unvisited square complement is a lower bound.
        for (int x = home.x - radius; x <= home.x + radius; ++x) {
            referenceNearestInCell(int2(x, home.y - radius), home, fraction, bestSquared, bestVector);
            if (radius != 0) referenceNearestInCell(int2(x, home.y + radius), home, fraction, bestSquared, bestVector);
        }

        for (int y = home.y - radius + 1; y < home.y + radius; ++y) {
            referenceNearestInCell(int2(home.x - radius, y), home, fraction, bestSquared, bestVector);
            referenceNearestInCell(int2(home.x + radius, y), home, fraction, bestSquared, bestVector);
        }

        if (bestSquared != 3.402823466e38) {
            float innerFraction = min(min(fraction.x, 1.0 - fraction.x), min(fraction.y, 1.0 - fraction.y));
            float lower = downPositive(downPositive(float(radius) + innerFraction));
            // Search termination only. L1 bounds the distance to this valid
            // point on the segment, without a sqrt/division accuracy premise.
            // Admitted half-integral origin/extent <=2^22 makes integer+half
            // exact; two component sub/add roundings contribute <=u*(4W+10).
            float allowance = 8.0 * 1.192092896e-7 * (float(extent) + 3.0);
            float upper = abs(bestVector.x) + abs(bestVector.y);
            upper = asfloat(asuint(upper) + 1u);
            upper += allowance;
            upper = asfloat(asuint(upper) + 1u);
            if (upper <= lower) break;
        }
    }

    if (bestSquared == 3.402823466e38) {
        recordError(16u, node, 0xffffffffu, particle);
        return result;
    }

    result.collides = true;
    result.penetration = sqrt(bestSquared);
    result.normal = result.penetration == 0.0 ? float2(0.0) : -bestVector / result.penetration;
    result.point = p + bestVector;
    return result;
}

