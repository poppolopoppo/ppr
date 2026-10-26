// Engine contour/body extensions. The upstream static-shape path stays separate.
// Included after the source types and checked integer/error helpers.

ConstantBuffer<Environment> g_environment;
StructuredBuffer<uint> g_terrain_masks;
StructuredBuffer<ContourPoint> g_contour_points;
StructuredBuffer<ContourCase> g_contour_cases;
StructuredBuffer<BodyBoundary> g_bodies;

// ------------------------------------------------------------------
// complete contour boundary search in implicit dual-cell bins
// ------------------------------------------------------------------

float2 contourPoint(uint index) {
    ContourPoint p = g_contour_points[index];
    return float2(p.x, p.y);
}

float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

bool terrainCellValid(int2 cell) {
    return all(cell >= int2(0)) && cell.x < int(g_environment.terrainWidth) && cell.y < int(g_environment.terrainHeight);
}

ContourCase terrainCase(int2 cell) {
    return g_contour_cases[g_terrain_masks[uint(cell.y) * g_environment.terrainWidth + uint(cell.x)]];
}

bool containsTerrain(ContourCase shape, float2 fraction) {
    for (uint index = 0u; index < shape.triangleCount; ++index) {
        ContourTriangle triangle = shape.triangles[index];
        float2 a = contourPoint(triangle.a);
        float2 b = contourPoint(triangle.b);
        float2 c = contourPoint(triangle.c);
        float ab = cross2(b - a, fraction - a);
        float bc = cross2(c - b, fraction - b);
        float ca = cross2(a - c, fraction - c);
        if ((ab >= 0.0 && bc >= 0.0 && ca >= 0.0) || (ab <= 0.0 && bc <= 0.0 && ca <= 0.0)) return true;
    }

    return false;
}

void nearestInCell(int2 cell, int2 home, float2 fraction, inout float bestSquared, inout float2 bestVector) {
    if (!terrainCellValid(cell)) return;

    ContourCase shape = terrainCase(cell);
    for (uint index = 0u; index < shape.segmentCount; ++index) {
        ContourSegment segment = shape.segments[index];
        float2 pointA = contourPoint(segment.begin);
        float2 direction = contourPoint(segment.end) - pointA;
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

float downPositive(float value) { return value > 0.0 ? asfloat(asuint(value) - 1u) : 0.0; }

Contact terrainContact(float2 p, uint node, uint particle) {
    Contact result = {false, 0.0, float2(0.0), float2(0.0)};
    if (g_environment.hasTerrain == 0u) return result;

    float2 local = p - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    // Establish the finite integer conversion domain before floor/cast.
    if (!all(isfinite(local)) || any(local < float2(0.0)) ||
        local.x >= float(g_environment.terrainWidth) || local.y >= float(g_environment.terrainHeight)) return result;

    int2 home = int2(floor(local));
    float2 fraction = local - float2(home);
    if (!containsTerrain(terrainCase(home), fraction)) return result;

    float bestSquared = 3.402823466e38;
    float2 bestVector = float2(0.0);
    uint extent = max(g_environment.terrainWidth, g_environment.terrainHeight);
    for (int radius = 0; radius <= int(extent); ++radius) {
        // Only the new ring is visited. Segments stay inside their owning unit
        // cells, so distance to the unvisited square complement is a lower bound.
        for (int x = home.x - radius; x <= home.x + radius; ++x) {
            nearestInCell(int2(x, home.y - radius), home, fraction, bestSquared, bestVector);
            if (radius != 0) nearestInCell(int2(x, home.y + radius), home, fraction, bestSquared, bestVector);
        }

        for (int y = home.y - radius + 1; y < home.y + radius; ++y) {
            nearestInCell(int2(home.x - radius, y), home, fraction, bestSquared, bestVector);
            nearestInCell(int2(home.x + radius, y), home, fraction, bestSquared, bestVector);
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

// ------------------------------------------------------------------
// one-way predicted body displacement
// ------------------------------------------------------------------

float2 bodyDisplacement(BodyBoundary boundary, float2 contact) {
    float2 lever = contact - float2(boundary.comX, boundary.comY);
    float2 velocity = float2(boundary.velocityX, boundary.velocityY) + boundary.angularVelocity * float2(-lever.y, lever.x);
    return g_parameters.deltaTime * velocity;
}
