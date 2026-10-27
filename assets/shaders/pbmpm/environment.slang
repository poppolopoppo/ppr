// Engine contour/body extensions. The upstream static-shape path stays separate.
// Included after the source types and checked integer/error helpers.

ConstantBuffer<Environment> g_environment;
StructuredBuffer<uint> g_terrain_masks;
StructuredBuffer<uint> g_terrain_boundary_tiles;
StructuredBuffer<uint> g_contour_points;
StructuredBuffer<uint2> g_contour_cases;
StructuredBuffer<BodyBoundary> g_bodies;

// ------------------------------------------------------------------
// complete contour boundary search in implicit dual-cell bins
// ------------------------------------------------------------------

float2 contourPoint(uint index) {
    uint code = g_contour_points[0] >> (4u * index);
    return float2(code & 3u, (code >> 2u) & 3u) * 0.5;
}

uint contourBits(uint2 packed, uint offset, uint mask) {
    if (offset >= 32u) return (packed.y >> (offset - 32u)) & mask;

    uint low = packed.x >> offset;
    // No shift by 32, and no native 64-bit shader integer is required.
    if (offset != 0u) low |= packed.y << (32u - offset);
    return low & mask;
}

ContourCase contourCase(uint index) {
    uint2 packed = g_contour_cases[index];
    ContourCase shape;
    shape.segmentCount = packed.x & 3u;
    shape.triangleCount = (packed.x >> 2u) & 3u;
    for (uint segment = 0u; segment < 2u; ++segment) {
        uint code = contourBits(packed, 4u + 6u * segment, 63u);
        shape.segments[segment].begin = code & 7u;
        shape.segments[segment].end = (code >> 3u) & 7u;
    }

    for (uint triangle = 0u; triangle < 3u; ++triangle) {
        uint code = contourBits(packed, 16u + 13u * triangle, 8191u);
        shape.triangles[triangle].a = code & 7u;
        shape.triangles[triangle].b = (code >> 3u) & 7u;
        shape.triangles[triangle].c = (code >> 6u) & 7u;
        shape.triangles[triangle].solidCorners = (code >> 9u) & 15u;
    }

    return shape;
}

float cross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

bool terrainCellValid(int2 cell) {
    return all(cell >= int2(0)) && cell.x < int(g_environment.terrainWidth) && cell.y < int(g_environment.terrainHeight);
}

bool terrainTileHasBoundary(int2 cell) {
    if (!terrainCellValid(cell)) return false;

    uint columns = (g_environment.terrainWidth + 7u) / 8u;
    uint tile = (uint(cell.y) / 8u) * columns + uint(cell.x) / 8u;
    return ((g_terrain_boundary_tiles[tile >> 5u] >> (tile & 31u)) & 1u) != 0u;
}

ContourCase terrainCase(int2 cell) {
    uint cellIndex = uint(cell.y) * g_environment.terrainWidth + uint(cell.x);
    uint mask = (g_terrain_masks[cellIndex >> 3u] >> (4u * (cellIndex & 7u))) & 15u;
    return contourCase(mask);
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

// The owner derives reserved bit 0 from the validated canonical boundary tiles
// in the same replacement transaction as these masks. With no segment anywhere,
// an interior query is exactly the existing complete-search failure predicate.
void checkTerrainCoverage(float2 p, uint node) {
    if (g_environment.hasTerrain == 0u || (g_environment.reserved & 1u) != 0u) return;

    float2 local = p - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    if (!all(isfinite(local)) || any(local < float2(0.0)) ||
        local.x >= float(g_environment.terrainWidth) || local.y >= float(g_environment.terrainHeight)) return;

    int2 home = int2(floor(local));
    if (containsTerrain(terrainCase(home), local - float2(home)))
        recordError(16u, node, 0xffffffffu, 0xffffffffu);
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
        int right = min(home.x + radius, int(g_environment.terrainWidth) - 1);
        for (int x = max(home.x - radius, 0); x <= right;) {
            int end = min(right, x | 7);
            // Skip a span only when both sides are empty. Retain the original
            // top/bottom interleave and strict nearest comparison for ties.
            if (terrainTileHasBoundary(int2(x, home.y - radius)) ||
                terrainTileHasBoundary(int2(x, home.y + radius))) {
                for (int cellX = x; cellX <= end; ++cellX) {
                    nearestInCell(int2(cellX, home.y - radius), home, fraction, bestSquared, bestVector);
                    if (radius != 0) nearestInCell(int2(cellX, home.y + radius), home, fraction, bestSquared, bestVector);
                }
            }

            x = end + 1;
        }

        int bottom = min(home.y + radius - 1, int(g_environment.terrainHeight) - 1);
        for (int y = max(home.y - radius + 1, 0); y <= bottom;) {
            int end = min(bottom, y | 7);
            if (terrainTileHasBoundary(int2(home.x - radius, y)) ||
                terrainTileHasBoundary(int2(home.x + radius, y))) {
                for (int cellY = y; cellY <= end; ++cellY) {
                    nearestInCell(int2(home.x - radius, cellY), home, fraction, bestSquared, bestVector);
                    nearestInCell(int2(home.x + radius, cellY), home, fraction, bestSquared, bestVector);
                }
            }

            y = end + 1;
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
// converged grid-query waves; endpoint queries retain the serial search
// ------------------------------------------------------------------

Contact terrainContactWave(float2 p, bool enabled, uint node, uint particle) {
    Contact result = {false, 0.0, float2(0.0), float2(0.0)};
    bool pending = enabled && g_environment.hasTerrain != 0u;
    int2 home = int2(0);
    float2 fraction = float2(0.0);
    if (pending) {
        float2 local = p - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
        pending = all(isfinite(local)) && all(local >= float2(0.0)) &&
            local.x < float(g_environment.terrainWidth) && local.y < float(g_environment.terrainHeight);
        if (pending) {
            home = int2(floor(local));
            fraction = local - float2(home);
            pending = containsTerrain(terrainCase(home), fraction);
        }
    }

    uint lane = WaveGetLaneIndex();
    uint rank = WavePrefixCountBits(true);
    uint count = WaveActiveCountBits(true);
    uint extent = max(g_environment.terrainWidth, g_environment.terrainHeight);
    for (;;) {
        uint owner = WaveActiveMin(pending ? lane : 0xffffffffu);
        if (owner == 0xffffffffu) break;

        // The reduced owner is active and uniform across this wave. Every
        // participating lane stays in the loop until all queries are complete.
        int2 queryHome = WaveReadLaneAt(home, owner);
        float2 queryFraction = WaveReadLaneAt(fraction, owner);
        float2 queryPoint = WaveReadLaneAt(p, owner);
        float bestSquared = 3.402823466e38;
        float2 bestVector = float2(0.0);
        for (int radius = 0; radius <= int(extent); ++radius) {
            uint ringCells = radius == 0 ? 1u : 8u * uint(radius);
            uint horizontal = 4u * uint(radius) + 2u;
            float laneSquared = 3.402823466e38;
            float2 laneVector = float2(0.0);
            uint laneOrdinal = 0xffffffffu;
            for (uint ordinal = rank; ordinal < ringCells; ordinal += count) {
                int2 cell = queryHome;
                if (radius != 0) {
                    if (ordinal < horizontal) {
                        cell.x += int(ordinal / 2u) - radius;
                        cell.y += (ordinal & 1u) == 0u ? -radius : radius;
                    } else {
                        uint vertical = ordinal - horizontal;
                        cell.x += (vertical & 1u) == 0u ? -radius : radius;
                        cell.y += int(vertical / 2u) - radius + 1;
                    }
                }

                if (!terrainTileHasBoundary(cell)) continue;

                float cellSquared = 3.402823466e38;
                float2 cellVector = float2(0.0);
                // This retains the original per-segment expressions and the
                // first segment on equal distance within a cell.
                nearestInCell(cell, queryHome, queryFraction, cellSquared, cellVector);
                if (cellSquared == 3.402823466e38) continue;

                if (cellSquared < laneSquared || (cellSquared == laneSquared && ordinal < laneOrdinal)) {
                    laneSquared = cellSquared;
                    laneVector = cellVector;
                    laneOrdinal = ordinal;
                }
            }

            // Squared distances are finite and nonnegative in the admitted
            // domain. Their uint encodings order exactly, without float sums.
            uint minimum = WaveActiveMin(asuint(laneSquared));
            if (minimum < asuint(bestSquared)) {
                uint ordinal = WaveActiveMin(asuint(laneSquared) == minimum ? laneOrdinal : 0xffffffffu);
                uint winner = WaveActiveMin(laneOrdinal == ordinal && asuint(laneSquared) == minimum ? lane : 0xffffffffu);
                bestSquared = asfloat(minimum);
                bestVector = WaveReadLaneAt(laneVector, winner);
            }

            // A complete ring is reduced before the unchanged termination test.
            if (bestSquared != 3.402823466e38) {
                float innerFraction = min(min(queryFraction.x, 1.0 - queryFraction.x), min(queryFraction.y, 1.0 - queryFraction.y));
                float lower = downPositive(downPositive(float(radius) + innerFraction));
                float allowance = 8.0 * 1.192092896e-7 * (float(extent) + 3.0);
                float upper = abs(bestVector.x) + abs(bestVector.y);
                upper = asfloat(asuint(upper) + 1u);
                upper += allowance;
                upper = asfloat(asuint(upper) + 1u);
                if (upper <= lower) break;
            }
        }

        if (lane == owner) {
            if (bestSquared == 3.402823466e38) {
                recordError(16u, node, 0xffffffffu, particle);
            } else {
                result.collides = true;
                result.penetration = sqrt(bestSquared);
                result.normal = result.penetration == 0.0 ? float2(0.0) : -bestVector / result.penetration;
                result.point = queryPoint + bestVector;
            }

            pending = false;
        }
    }

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
