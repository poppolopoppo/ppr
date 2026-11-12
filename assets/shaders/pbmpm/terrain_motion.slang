// Production point transport against the unchanged canonical terrain contour.
// Included after environment.slang; no additional device resources or ABI.

// ------------------------------------------------------------------
// directed boundary features and local free-space cones
// ------------------------------------------------------------------

struct TerrainFeature {
    int2 cell;
    uint segment;
    float fraction;
    float2 point;
};

struct TerrainCone {
    float2 incoming;
    float2 outgoing;
    float turn;
    bool paired;
};

float2 terrainEdgeNormal(float2 edge) {
    // Canonical edges are axis aligned or 45 degrees. Unnormalized signs
    // retain exact tangent projections without sqrt/dot residuals.
    return float2(-sign(edge.y), sign(edge.x));
}

void terrainFeatureEdge(TerrainFeature feature, out float2 a, out float2 edge) {
    ContourCase shape = terrainCase(feature.cell);
    ContourSegment segment = shape.segments[feature.segment];
    a = float2(g_environment.terrainOriginX, g_environment.terrainOriginY) +
        float2(feature.cell) + contourPoint(segment.begin);
    edge = contourPoint(segment.end) - contourPoint(segment.begin);
}

TerrainCone terrainFeatureCone(TerrainFeature feature, uint row) {
    float2 a, edge;
    terrainFeatureEdge(feature, a, edge);
    TerrainCone cone = {terrainEdgeNormal(edge), float2(0.0), 0.0, false};
    if (feature.fraction > 0.0 && feature.fraction < 1.0) return cone;

    float2 vertex = feature.fraction == 0.0 ? a : a + edge;
    float2 local = vertex - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    int2 home = int2(floor(local));
    float2 incoming = float2(0.0), outgoing = float2(0.0);
    uint incomingCount = 0u, outgoingCount = 0u;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 cell = home + int2(x,y);
            if (!terrainCellValid(cell)) continue;

            ContourCase shape = terrainCase(cell);
            for (uint index = 0u; index < shape.segmentCount; ++index) {
                ContourSegment segment = shape.segments[index];
                float2 begin = float2(cell) + contourPoint(segment.begin);
                float2 end = float2(cell) + contourPoint(segment.end);
                if (all(end == local)) { incoming = end - begin; ++incomingCount; }
                if (all(begin == local)) { outgoing = end - begin; ++outgoingCount; }
            }
        }
    }

    if (incomingCount == 1u && outgoingCount == 1u) {
        cone.incoming = terrainEdgeNormal(incoming);
        cone.outgoing = terrainEdgeNormal(outgoing);
        cone.turn = cross2(incoming, outgoing);
        cone.paired = true;
        if (cone.turn == 0.0 && dot(incoming, outgoing) <= 0.0)
            recordError(256u, 0xffffffffu, 1u, row);
    } else {
        // An open contour may end only at the finite terrain-domain boundary.
        bool domainEdge = local.x == 0.0 || local.y == 0.0 ||
            local.x == float(g_environment.terrainWidth) || local.y == float(g_environment.terrainHeight);
        if (!domainEdge || incomingCount + outgoingCount != 1u)
            recordError(256u, 0xffffffffu, 2u, row);
    }

    return cone;
}

bool terrainDirectionEnters(TerrainCone cone, float2 direction) {
    bool first = dot(cone.incoming, direction) > 0.0;
    if (!cone.paired) return first;

    bool second = dot(cone.outgoing, direction) > 0.0;
    // Solid is on the left of both directed edges. Convex solid corners have
    // a union of free half-planes; concave solid corners have an intersection.
    return cone.turn > 0.0 ? first && second : first || second;
}

float2 terrainTangent(float2 direction, float2 normal) {
    if (normal.x == 0.0) return float2(direction.x, 0.0);
    if (normal.y == 0.0) return float2(0.0, direction.y);

    bool opposing = normal.x * normal.y < 0.0;
    float tangent = opposing ? direction.x * 0.5 + direction.y * 0.5 : direction.x * 0.5 - direction.y * 0.5;
    return float2(tangent, opposing ? tangent : -tangent);
}

float2 terrainConstrain(TerrainCone cone, float2 direction) {
    if (!terrainDirectionEnters(cone, direction)) return direction;

    float magnitude = max(abs(direction.x), abs(direction.y));
    float2 scaled = direction / magnitude;
    float2 best = float2(0.0);
    float distanceSquared = dot(scaled, scaled);
    for (uint index = 0u; index < (cone.paired ? 2u : 1u); ++index) {
        float2 normal = index == 0u ? cone.incoming : cone.outgoing;
        float2 candidate = terrainTangent(direction, normal);
        float2 change = candidate / magnitude - scaled;
        float candidateSquared = dot(change, change);
        if (!terrainDirectionEnters(cone, candidate) && candidateSquared < distanceSquared) {
            best = candidate;
            distanceSquared = candidateSquared;
        }
    }

    return best;
}

// Coulomb carry response to one normal projection. Geometry and recovery
// distances never enter this calculation; scaling preserves the free cone.
float2 terrainCarryFriction(float2 direction, float2 projected, float friction) {
    if (friction == 0.0 || all(direction == projected)) return projected;

    float magnitude = max(max(abs(direction.x), abs(direction.y)), max(abs(projected.x), abs(projected.y)));
    if (magnitude == 0.0) return projected;

    float2 scaled = projected / magnitude;
    float tangent = length(scaled);
    if (tangent == 0.0) return float2(0.0);

    float normalRemoval = length(direction / magnitude - scaled);
    return projected * max(0.0, 1.0 - friction * normalRemoval / tangent);
}

float terrainNextFloat(float value, float direction) {
    if (direction == 0.0) return value;
    if (value == 0.0) return asfloat(direction > 0.0 ? 1u : 0x80000001u);

    uint bits = asuint(value);
    bool increaseBits = (value > 0.0) == (direction > 0.0);
    return asfloat(increaseBits ? bits + 1u : bits - 1u);
}

bool terrainOnBoundary(float2 local) {
    int2 home = int2(floor(local));
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 cell = home + int2(x,y);
            if (!terrainCellValid(cell)) continue;

            ContourCase shape = terrainCase(cell);
            for (uint index = 0u; index < shape.segmentCount; ++index) {
                ContourSegment segment = shape.segments[index];
                float2 a = float2(cell) + contourPoint(segment.begin);
                float2 edge = contourPoint(segment.end) - contourPoint(segment.begin);
                float2 relative = local - a;
                float along = dot(relative, edge);
                if (cross2(edge, relative) == 0.0 && along >= 0.0 && along <= dot(edge, edge)) return true;
            }
        }
    }

    return false;
}

bool terrainCoveredClosed(float2 local) {
    int2 home = int2(floor(local));
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 cell = home + int2(x,y);
            if (!terrainCellValid(cell)) continue;

            float2 fraction = local - float2(cell);
            if (all(fraction >= float2(0.0)) && all(fraction <= float2(1.0)) &&
                containsTerrain(terrainCase(cell), fraction)) return true;
        }
    }

    return false;
}

bool terrainStrictInterior(float2 point) {
    float2 local = point - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    if (any(local < float2(0.0)) || local.x >= float(g_environment.terrainWidth) ||
        local.y >= float(g_environment.terrainHeight)) return false;

    int2 home = int2(floor(local));
    if (!containsTerrain(terrainCase(home), local - float2(home))) return false;

    // Triangle containment is closed; actual contour segments are admissible
    // zero-depth contact rather than solid interior.
    return !terrainOnBoundary(local);
}

bool terrainPositionCertified(float2 point) {
    if (!all(isfinite(point))) return false;

    return !terrainStrictInterior(point);
}

void terrainValidatePosition(float2 point, uint row) {
    if (g_environment.hasTerrain != 0u && !terrainPositionCertified(point))
        recordError(256u, 0xffffffffu, 6u, row);
}

float2 terrainExteriorPoint(TerrainFeature feature, TerrainCone cone, float2 preferred, uint row) {
    // A closed boundary is already admissible. Equal ULP counts are not equal
    // geometric steps: at a rounded concave vertex, nudging a valid point can
    // cross the neighboring edge when the coordinate magnitudes differ.
    if (terrainPositionCertified(feature.point)) return feature.point;

    float2 outward = -cone.incoming;
    if (cone.paired && cone.turn <= 0.0) outward -= cone.outgoing;
    else if (cone.paired && dot(cone.incoming, preferred) > 0.0 && dot(cone.outgoing, preferred) <= 0.0)
        outward = -cone.outgoing;

    // Repair uncertified rounding with representable steps, not a fixed skin.
    // Recovery distance never becomes velocity or constitutive deformation.
    float2 point = feature.point;
    for (uint attempt = 0u; attempt < 3u; ++attempt) {
        point = float2(terrainNextFloat(point.x, outward.x), terrainNextFloat(point.y, outward.y));
        if (terrainPositionCertified(point)) return point;
    }

    recordError(256u, 0xffffffffu, 3u, row);
    return point;
}

TerrainFeature terrainNearestFeature(float2 point, uint row) {
    float2 origin = float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    float2 local = point - origin;
    int2 home = int2(floor(local));
    TerrainFeature best = {int2(0), 0u, 0.0, point};
    float bestSquared = 3.402823466e38;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            int2 cell = home + int2(x,y);
            if (!terrainCellValid(cell)) continue;

            ContourCase shape = terrainCase(cell);
            for (uint index = 0u; index < shape.segmentCount; ++index) {
                ContourSegment segment = shape.segments[index];
                float2 a = float2(cell) + contourPoint(segment.begin);
                float2 edge = contourPoint(segment.end) - contourPoint(segment.begin);
                float fraction = clamp(dot(local - a, edge) / dot(edge, edge), 0.0, 1.0);
                float2 nearest = a + fraction * edge;
                float2 difference = nearest - local;
                float squared = dot(difference, difference);
                if (squared < bestSquared) {
                    bestSquared = squared;
                    best.cell = cell;
                    best.segment = index;
                    best.fraction = fraction;
                    best.point = origin + nearest;
                }
            }
        }
    }

    // The complete nearest query supplied a boundary point, so its feature is
    // in this neighborhood even if constructing the world point rounded it.
    if (bestSquared == 3.402823466e38) recordError(256u, 0xffffffffu, 4u, row);
    return best;
}

// ------------------------------------------------------------------
// clipped supercover traversal and first entering segment
// ------------------------------------------------------------------

bool terrainClipAxis(float start, float delta, float extent, inout float entry, inout float exit) {
    if (delta == 0.0) return start >= 0.0 && start <= extent;

    float a = -start / delta;
    float b = (extent - start) / delta;
    entry = max(entry, min(a,b));
    exit = min(exit, max(a,b));
    return entry <= exit;
}

void terrainSweepCell(int2 cell, float2 localStart, float2 motion, uint row,
    inout bool found, inout float first, inout TerrainFeature feature) {
    if (!terrainTileHasBoundary(cell)) return;

    ContourCase shape = terrainCase(cell);
    for (uint index = 0u; index < shape.segmentCount; ++index) {
        ContourSegment segment = shape.segments[index];
        float2 pointA = contourPoint(segment.begin);
        float2 edge = contourPoint(segment.end) - pointA;
        if (cross2(edge, motion) <= 0.0) continue;

        float2 relative = (float2(cell) + pointA) - localStart;
        float denominator = cross2(motion, edge);
        float time = cross2(relative, edge) / denominator;
        float fraction = cross2(relative, motion) / denominator;
        if (time < 0.0 || time > 1.0 || fraction < 0.0 || fraction > 1.0) continue;
        if (found && (time > first || (time == first &&
            (cell.y > feature.cell.y || (cell.y == feature.cell.y &&
                (cell.x > feature.cell.x || (cell.x == feature.cell.x && index >= feature.segment))))))) continue;

        TerrainFeature candidate;
        candidate.cell = cell;
        candidate.segment = index;
        candidate.fraction = fraction;
        candidate.point = float2(g_environment.terrainOriginX, g_environment.terrainOriginY) +
            float2(cell) + pointA + fraction * edge;
        TerrainCone cone = terrainFeatureCone(candidate, row);
        if (!terrainDirectionEnters(cone, motion)) continue;

        found = true;
        first = time;
        feature = candidate;
    }
}

// A finite view does not synthesize collision edges at its rectangle. Entering
// occupied coverage there without a canonical segment is incomplete geometry.
bool terrainCertifyEntry(float2 local, float2 motion, float entry, float exit,
    float2 extent, uint row, out bool requiresHit) {
    requiresHit = false;
    if (entry >= exit || (entry == 0.0 && all(local >= float2(0.0)) && all(local < extent))) return true;

    float2 point = clamp(local + entry * motion, float2(0.0), extent);
    bool vertical = false, horizontal = false;
    if (motion.x != 0.0) {
        float face = motion.x > 0.0 ? 0.0 : extent.x;
        vertical = entry == (face - local.x) / motion.x;
        if (vertical) point.x = face;
    }

    if (motion.y != 0.0) {
        float face = motion.y > 0.0 ? 0.0 : extent.y;
        horizontal = entry == (face - local.y) / motion.y;
        if (horizontal) point.y = face;
    }

    if (!vertical && !horizontal) {
        recordError(256u, 0xffffffffu, 8u, row);
        return false;
    }

    if (terrainOnBoundary(point)) {
        TerrainFeature feature = terrainNearestFeature(
            point + float2(g_environment.terrainOriginX, g_environment.terrainOriginY), row);
        requiresHit = terrainDirectionEnters(terrainFeatureCone(feature, row), motion);
        return true;
    }

    if (terrainCoveredClosed(point)) {
        recordError(16u, 0xffffffffu, 0xffffffffu, row);
        return false;
    }

    float2 error = 8.0 * 1.192092896e-7 * (abs(local) + abs(entry * motion) + 1.0);
    float uncertainty = vertical ? error.y : error.x;
    // Side topology changes on the half grid. An interval no longer than
    // half a cell cannot hide a complete occupied span between free samples.
    if (!all(isfinite(error)) || uncertainty > 0.25) {
        recordError(256u, 0xffffffffu, 8u, row);
        return false;
    }

    float2 tangent = vertical ? float2(0.0, uncertainty) : float2(uncertainty, 0.0);
    if (terrainCoveredClosed(clamp(point - tangent, float2(0.0), extent)) ||
        terrainCoveredClosed(clamp(point + tangent, float2(0.0), extent))) {
        recordError(256u, 0xffffffffu, 8u, row);
        return false;
    }

    return true;
}

bool terrainSweepComplete(bool found, float first, float entry, bool requiresHit, uint row) {
    if (requiresHit && (!found || first > entry)) recordError(256u, 0xffffffffu, 8u, row);
    return found;
}

bool terrainSweep(float2 start, float2 motion, uint row, out float first, out TerrainFeature feature) {
    first = 1.0;
    feature = {int2(0), 0u, 0.0, start};
    float2 local = start - float2(g_environment.terrainOriginX, g_environment.terrainOriginY);
    float entry = 0.0, exit = 1.0;
    if (!terrainClipAxis(local.x, motion.x, float(g_environment.terrainWidth), entry, exit) ||
        !terrainClipAxis(local.y, motion.y, float(g_environment.terrainHeight), entry, exit)) return false;

    float2 extent = float2(g_environment.terrainWidth, g_environment.terrainHeight);
    bool requiresHit;
    if (!terrainCertifyEntry(local, motion, entry, exit, extent, row, requiresHit)) return false;

    float2 clipped = clamp(local + entry * motion, float2(0.0), extent);
    int2 cell = clamp(int2(floor(clipped)), int2(0), int2(g_environment.terrainWidth, g_environment.terrainHeight) - 1);
    int2 step = int2(sign(motion));
    bool found = false;
    uint bound = g_environment.terrainWidth + g_environment.terrainHeight + 4u;
    for (uint visit = 0u; visit < bound; ++visit) {
        // The 3x3 supercover includes cell-edge rays, vertex crossings and
        // rounded DDA ties. Whole-ray intersection tests reject extra cells.
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x)
                terrainSweepCell(cell + int2(x,y), local, motion, row, found, first, feature);
        }

        float nextX = step.x == 0 ? 3.402823466e38 :
            (float(cell.x + (step.x > 0 ? 1 : 0)) - local.x) / motion.x;
        float nextY = step.y == 0 ? 3.402823466e38 :
            (float(cell.y + (step.y > 0 ? 1 : 0)) - local.y) / motion.y;
        float next = min(nextX, nextY);
        if (next > exit || (found && next > first)) return terrainSweepComplete(found, first, entry, requiresHit, row);
        if (step.x == 0 && step.y == 0) return terrainSweepComplete(found, first, entry, requiresHit, row);

        if (nextX <= nextY) cell.x += step.x;
        if (nextY <= nextX) cell.y += step.y;
        if (!terrainCellValid(cell)) return terrainSweepComplete(found, first, entry, requiresHit, row);
    }

    recordError(256u, 0xffffffffu, 5u, row);
    return found;
}

// ------------------------------------------------------------------
// separate positional recovery, transport and retained motion
// ------------------------------------------------------------------

// Slang2026.17.1 does not emit SPIR-V NoContraction for local `precise`
// (upstream #12198/#12208). Protect only these two geometric operations;
// global FP mode and the selected-fork constitutive arithmetic stay unchanged.
// This is the pinned compiler's inline-SPIR-V syntax; recheck on upgrades.
float4 terrainRoundedLeg(float2 start, float2 delta) {
    __target_switch {
    case spirv:
        return spirv_asm {
            OpFAdd $$float2 %target $start $delta;
            OpDecorate %target NoContraction;
            OpFSub $$float2 %motion %target $start;
            OpDecorate %motion NoContraction;
            OpCompositeConstruct $$float4 result %target %motion;
        };
    default:
        precise float2 target = start + delta;
        precise float2 motion = target - start;
        return float4(target, motion);
    }
}

// A transport exit publishes the point it chose. If that point is not certified,
// project it to the nearest canonical boundary first, using the same bounded
// recovery the contact path already uses (nearest 3x3 feature, at most three
// representable outward steps). The rejection stays the last resort: field 6 is
// recorded only when the bounded repair also fails. A passing position is
// returned untouched, so a successful tick is bit-identical.
float2 terrainRepairPublishedPosition(float2 position, uint row) {
    if (g_environment.hasTerrain != 0u && !terrainPositionCertified(position)) {
        TerrainFeature feature = terrainNearestFeature(position, row);
        TerrainCone cone = terrainFeatureCone(feature, row);
        position = terrainExteriorPoint(feature, cone, position, row);
    }

    // An unrepairable neighbourhood already recorded field 3 or field 4, so the
    // final-position field remains the last-resort reason.
    terrainValidatePosition(position, row);
    return position;
}

void terrainTransportLimited<let ContactLimit : uint>(
    float2 oldPosition, float2 transport, inout float2 position, inout float2 carry, uint row, float friction) {
    if (g_environment.hasTerrain == 0u) return;

    // In particular, reject gravity/body overflow before a projection could
    // replace an invalid carry with a finite zero and hide its original cause.
    if (!all(isfinite(oldPosition)) || !all(isfinite(transport)) ||
        !all(isfinite(position)) || !all(isfinite(carry))) {
        recordError(8u, 0xffffffffu, 0xffffffffu, row);
        return;
    }

    float2 start = oldPosition;
    Contact overlap = terrainContact(start, 0xffffffffu, row);
    if (overlap.collides) {
        TerrainFeature feature = terrainNearestFeature(overlap.point, row);
        TerrainCone cone = terrainFeatureCone(feature, row);
        transport = terrainConstrain(cone, transport);
        float2 projected = terrainConstrain(cone, carry);
        start = terrainExteriorPoint(feature, cone, any(transport != float2(0.0)) ? transport : projected, row);
        carry = terrainCarryFriction(carry, projected, friction);
    }

    // Each contact removes only motion entering the local solid cone. A
    // bounded failure rejects the working transaction instead of publishing a
    // partial path or spinning indefinitely on a numerical contact cycle.
    for (uint contact = 0u; contact <= ContactLimit; ++contact) {
        // Sweep the target actually representable in the published FP32 row.
        // Sweeping the unrounded carry can miss an endpoint that rounds across
        // a diagonal at large world coordinates. Carry itself is independent.
        float4 leg = terrainRoundedLeg(start, transport);
        float2 target = leg.xy;
        if (!all(isfinite(target))) {
            recordError(8u, 0xffffffffu, 0xffffffffu, row);
            return;
        }

        float2 motion = leg.zw;
        if (all(motion == float2(0.0))) {
            position = terrainRepairPublishedPosition(start, row);
            return;
        }

        float time;
        TerrainFeature feature;
        if (!terrainSweep(start, motion, row, time, feature)) {
            position = terrainRepairPublishedPosition(target, row);
            return;
        }

        // The limit counts resolved contact events, not sweeps: a final
        // no-hit query above may still consume the remaining tangent motion.
        if (contact == ContactLimit) {
            recordError(256u, 0xffffffffu, 7u, row);
            return;
        }

        TerrainCone cone = terrainFeatureCone(feature, row);
        transport = terrainConstrain(cone, (1.0 - time) * motion);
        float2 projected = terrainConstrain(cone, carry);
        start = terrainExteriorPoint(feature, cone, any(transport != float2(0.0)) ? transport : projected, row);
        carry = terrainCarryFriction(carry, projected, friction);
    }
}

void terrainTransportLimited<let ContactLimit : uint>(
    float2 oldPosition, float2 transport, inout float2 position, inout float2 carry, uint row) {
    terrainTransportLimited<ContactLimit>(oldPosition, transport, position, carry, row, 0.0);
}

void terrainTransport(float2 oldPosition, float2 transport, inout float2 position, inout float2 carry, uint row,
    float friction) {
    terrainTransportLimited<32u>(oldPosition, transport, position, carry, row, friction);
}

void terrainTransport(float2 oldPosition, float2 transport, inout float2 position, inout float2 carry, uint row) {
    terrainTransport(oldPosition, transport, position, carry, row, 0.0);
}
