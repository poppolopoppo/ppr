// One geometry/control-flow body, instantiated with FP32 and qualified FP64 policies.
// ------------------------------------------------------------------
// precision-selected outward arithmetic
// ------------------------------------------------------------------

[noinline]
TerrainInterval terrainExpand(TerrainScalar x, uint steps, inout uint status) {
    if (!terrainNormal(x)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }
    TerrainScalar low = x, high = x;
    for (uint i = 0; i < steps; ++i) {
        low = terrainNext(low, false);
        high = terrainNext(high, true);
    }
    if (!terrainNormal(low) || !terrainNormal(high)) status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
    return TerrainInterval(low, high);
}

TerrainInterval terrainAdd(TerrainInterval a, TerrainInterval b, inout uint status) {
    if (!terrainNormal(a.x) || !terrainNormal(a.y) || !terrainNormal(b.x) || !terrainNormal(b.y)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }

    if (all(a == 0.0)) return b;
    if (all(b == 0.0)) return a;

    precise TerrainScalar low = a.x + b.x;
    precise TerrainScalar high = a.y + b.y;
    if ((low == 0.0 && a.x != -b.x) || (high == 0.0 && a.y != -b.y)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }

    uint expansionStatus = status;
    TerrainInterval lower = low == 0.0 ? TerrainInterval(0.0) : terrainExpand(low, 1, expansionStatus);
    TerrainInterval upper = high == 0.0 ? TerrainInterval(0.0) : terrainExpand(high, 1, expansionStatus);

    status = expansionStatus;
    return TerrainInterval(lower.x, upper.y);
}

TerrainInterval terrainSub(TerrainInterval a, TerrainInterval b, inout uint status) { return terrainAdd(a, terrainNeg(b), status); }

TerrainInterval terrainMul(TerrainInterval a, TerrainInterval b, inout uint status) {
    if (!terrainNormal(a.x) || !terrainNormal(a.y) || !terrainNormal(b.x) || !terrainNormal(b.y)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }

    if (all(a == 0.0) || all(b == 0.0)) return 0.0;
    if (all(a == 1.0)) return b;
    if (all(b == 1.0)) return a;

    precise TerrainScalar p0 = a.x * b.x;
    precise TerrainScalar p1 = a.x * b.y;
    precise TerrainScalar p2 = a.y * b.x;
    precise TerrainScalar p3 = a.y * b.y;
    if ((p0 == 0.0 && a.x != 0.0 && b.x != 0.0) || (p1 == 0.0 && a.x != 0.0 && b.y != 0.0) ||
        (p2 == 0.0 && a.y != 0.0 && b.x != 0.0) || (p3 == 0.0 && a.y != 0.0 && b.y != 0.0)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }
    TerrainScalar low = min(min(p0, p1), min(p2, p3)), high = max(max(p0, p1), max(p2, p3));

    uint expansionStatus = status;
    TerrainScalar lower = low == 0.0 ? 0.0 : terrainExpand(low, 1, expansionStatus).x;
    TerrainScalar upper = high == 0.0 ? 0.0 : terrainExpand(high, 1, expansionStatus).y;

    status = expansionStatus;
    return TerrainInterval(lower, upper);
}

TerrainInterval terrainDiv(TerrainInterval a, TerrainInterval b, inout uint status) {
    if (!terrainNormal(a.x) || !terrainNormal(a.y) || !terrainNormal(b.x) || !terrainNormal(b.y) ||
        max(abs(b.x), abs(b.y)) > terrainDivMaximum()) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }

    if (b.x <= 0.0 && b.y >= 0.0) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }
    if (all(a == 0.0)) return 0.0;
    if (all(b == 1.0)) return a;

    precise TerrainScalar p0 = a.x / b.x;
    precise TerrainScalar p1 = a.x / b.y;
    precise TerrainScalar p2 = a.y / b.x;
    precise TerrainScalar p3 = a.y / b.y;
    if ((p0 == 0.0 && a.x != 0.0) || (p1 == 0.0 && a.x != 0.0) ||
        (p2 == 0.0 && a.y != 0.0) || (p3 == 0.0 && a.y != 0.0)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }
    // The FP32 policy retains its existing eight-neighbor division envelope.
    // FP64 uses that width as a bounded hardware-qualified engineering assumption;
    // no universal Vulkan binary64 bound or output admission is claimed.
    TerrainScalar low = min(min(p0, p1), min(p2, p3)), high = max(max(p0, p1), max(p2, p3));

    uint expansionStatus = status;
    TerrainScalar lower = low == 0.0 ? 0.0 : terrainExpand(low, 8, expansionStatus).x;
    TerrainScalar upper = high == 0.0 ? 0.0 : terrainExpand(high, 8, expansionStatus).y;

    status = expansionStatus;
    return TerrainInterval(lower, upper);
}

// ------------------------------------------------------------------
// complete implicit occupancy and symbolic line
// ------------------------------------------------------------------

TerrainInterval terrainField(TerrainCurve curve, TerrainInterval x, TerrainInterval y, inout uint status) {
    if (curve.lineAxis == 2 && !terrainCanonicalNormal(curve.roundness)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0;
    }

    if (curve.lineAxis != 2) {
        TerrainInterval coordinate = curve.lineAxis == 0 ? x : y;
        if (curve.corner != 0) coordinate = terrainSub(TerrainInterval(1.0), coordinate, status);
        return terrainSub(coordinate, TerrainInterval(0.5), status);
    }
    if (curve.corner == 1 || curve.corner == 2) x = terrainSub(TerrainInterval(1.0), x, status);
    if (curve.corner == 2 || curve.corner == 3) y = terrainSub(TerrainInterval(1.0), y, status);
    TerrainInterval difference = terrainSub(x, y, status);
    TerrainInterval square = terrainMul(difference, difference, status);
    TerrainInterval sum = terrainSub(terrainAdd(x, y, status), terrainAdd(TerrainInterval(0.5), terrainMul(TerrainInterval(0.25), TerrainInterval(curve.roundness), status), status), status);
    return terrainAdd(sum, terrainMul(TerrainInterval(curve.roundness), square, status), status);
}

bool terrainInside(uint mask, TerrainCurve curves[2], uint count, bool complement, TerrainInterval x, TerrainInterval y, inout uint status) {
    if (mask == 0 || mask == 15) return mask == 15;
    bool anyInside = false, uncertain = false;
    for (uint i = 0; i < count; ++i) {
        TerrainInterval value = terrainField(curves[i], x, y, status);
        anyInside = anyInside || value.y < 0.0;
        uncertain = uncertain || (value.x <= 0.0 && value.y >= 0.0);
    }
    if (!anyInside && uncertain) status |= kTerrainNumericAmbiguity | kTerrainReasonRoots;
    return complement ? !anyInside : anyInside;
}

bool terrainValidDifference(TerrainScalar a, TerrainScalar b, TerrainScalar difference, inout uint status) {
    bool magnitudeZero = terrainZeroBits(difference);
    if (!terrainNormal(difference) || (magnitudeZero && a != b)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return false;
    }
    return true;
}

TerrainInterval terrainAnchorDifference(TerrainScalar a, TerrainScalar b, inout uint status) {
    if (!terrainNormal(a) || !terrainNormal(b)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }
    if (terrainZeroBits(a)) return terrainNeg(TerrainInterval(b));
    if (terrainZeroBits(b)) return TerrainInterval(a);
    if (terrainEqualBits(a, b)) return 0.0;

    precise TerrainScalar difference = a - b;
    if (!terrainValidDifference(a, b, difference, status)) return 0.0;

    // Existing half-grid subtraction is exact within this bounded range.
    if (abs(a) <= 65536.0 && abs(b) <= 65536.0 &&
        a * 2.0 == floor(a * 2.0) && b * 2.0 == floor(b * 2.0)) return TerrainInterval(difference);
    if (terrainSterbenzOperands(a, b)) return TerrainInterval(difference);

    // A reverse Sterbenz subtraction is itself exact. Thus a-r=b proves
    // r=a-b, without an input grid or a rounded residual equality assumption.
    if (terrainSterbenzOperands(a, difference)) {
        precise TerrainScalar reverse = a - difference;
        if (!terrainValidDifference(a, difference, reverse, status)) return 0.0;
        if (terrainEqualBits(reverse, b)) return TerrainInterval(difference);
    }

    TerrainScalar negativeDifference = terrainSignNeg(difference);
    if (terrainSterbenzOperands(b, negativeDifference)) {
        precise TerrainScalar reverse = b - negativeDifference;
        if (!terrainValidDifference(b, negativeDifference, reverse, status)) return 0.0;
        if (terrainEqualBits(reverse, a)) return TerrainInterval(difference);
    }
    return terrainSub(TerrainInterval(a), TerrainInterval(b), status);
}

TerrainInterval terrainPointCoordinate(TerrainPoint point, uint axis, TerrainScalar reference, inout uint status) {
    if (!terrainValidPoint(point) || !terrainNormal(reference)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }

    TerrainInterval anchor = terrainAnchorDifference(point.probe[axis], reference, status);
    TerrainInterval offset = terrainMul(TerrainInterval(point.nearCells), TerrainInterval(point.rawDirection[axis]), status);
    return terrainAdd(anchor, offset, status);
}

TerrainInterval terrainPointDifference(TerrainPoint a, TerrainPoint b, uint axis, inout uint status) {
    if (!terrainValidPoint(a) || !terrainValidPoint(b)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }

    TerrainInterval anchor = terrainAnchorDifference(a.probe[axis], b.probe[axis], status);
    if (a.nearCells == b.nearCells && a.rawDirection[axis] == b.rawDirection[axis]) return anchor;

    TerrainInterval offset;
    if (a.rawDirection[axis] == b.rawDirection[axis]) {
        TerrainInterval parameter = terrainAnchorDifference(a.nearCells, b.nearCells, status);
        offset = terrainMul(parameter, TerrainInterval(a.rawDirection[axis]), status);
    } else {
        TerrainInterval first = terrainMul(TerrainInterval(a.nearCells), TerrainInterval(a.rawDirection[axis]), status);
        TerrainInterval second = terrainMul(TerrainInterval(b.nearCells), TerrainInterval(b.rawDirection[axis]), status);
        offset = terrainSub(first, second, status);
    }
    return terrainAdd(anchor, offset, status);
}

bool terrainPointEqualAxes(TerrainPoint point) {
    return point.probe.x == point.probe.y &&
        (point.nearCells == 0.0 || point.rawDirection.x == point.rawDirection.y);
}

void terrainLineDirection(TerrainLine line, out TerrainInterval dx, out TerrainInterval dy, inout uint status) {
    if (line.connector != 0) {
        dx = terrainPointDifference(line.target, line.origin, 0, status);
        dy = terrainPointDifference(line.target, line.origin, 1, status);
    } else {
        dx = TerrainInterval(line.rawDirection.x);
        dy = TerrainInterval(line.rawDirection.y);
    }
}

TerrainInterval terrainLineCoordinate(TerrainLine line, uint axis, TerrainInterval parameter, TerrainScalar reference,
    TerrainInterval direction, inout uint status) {
    if (!terrainNormal(parameter.x) || !terrainNormal(parameter.y)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0.0; }

    if (all(parameter == 0.0)) return terrainPointCoordinate(line.origin, axis, reference, status);
    if (line.connector != 0 && all(parameter == 1.0))
        return terrainPointCoordinate(line.target, axis, reference, status);

    TerrainInterval origin = terrainPointCoordinate(line.origin, axis, reference, status);
    return terrainAdd(origin, terrainMul(direction, parameter, status), status);
}

TerrainInterval terrainLinePlane(TerrainLine line, uint axis, TerrainScalar plane, TerrainInterval direction, inout uint status) {
    TerrainInterval origin = terrainPointCoordinate(line.origin, axis, plane, status);
    if (all(origin == 0.0)) return 0.0;

    if (line.connector != 0) {
        TerrainInterval target = terrainPointCoordinate(line.target, axis, plane, status);
        if (all(target == 0.0)) return 1.0;
    }
    return terrainDiv(terrainNeg(origin), direction, status);
}

int terrainPlaneEventOrder(TerrainLine line, TerrainInterval dx, TerrainInterval dy,
    uint axisA, TerrainScalar planeA, uint axisB, TerrainScalar planeB, inout uint status) {
    if (axisA > 1 || axisB > 1 || !terrainNormal(planeA) || !terrainNormal(planeB)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0;
    }

    TerrainInterval directionA = axisA == 0 ? dx : dy, directionB = axisB == 0 ? dx : dy;
    if (!terrainNormal(directionA.x) || !terrainNormal(directionA.y) ||
        !terrainNormal(directionB.x) || !terrainNormal(directionB.y) ||
        (!all(directionA > 0.0) && !all(directionA < 0.0)) ||
        (!all(directionB > 0.0) && !all(directionB < 0.0))) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0;
    }

    TerrainInterval originA = terrainPointCoordinate(line.origin, axisA, planeA, status);
    TerrainInterval originB = terrainPointCoordinate(line.origin, axisB, planeB, status);
    TerrainInterval numeratorA = directionA.x > 0.0 ? terrainNeg(originA) : originA;
    TerrainInterval numeratorB = directionB.x > 0.0 ? terrainNeg(originB) : originB;
    TerrainInterval denominatorA = directionA.x > 0.0 ? directionA : terrainNeg(directionA);
    TerrainInterval denominatorB = directionB.x > 0.0 ? directionB : terrainNeg(directionB);
    TerrainInterval left = terrainMul(numeratorA, denominatorB, status);
    TerrainInterval right = terrainMul(numeratorB, denominatorA, status);
    if (status != 0) return 0;

    if (left.y < right.x) return -1;
    if (right.y < left.x) return 1;
    return 0;
}

// ------------------------------------------------------------------
// complete roots and source ownership
// ------------------------------------------------------------------

uint terrainRoots(TerrainCurve curve, TerrainInterval ox, TerrainInterval oy, TerrainInterval dx, TerrainInterval dy,
    bool equalDirection, bool oppositeDirection, bool midpointWitness, out TerrainInterval roots[2], inout uint status) {
    if (curve.lineAxis == 2 && !terrainCanonicalNormal(curve.roundness)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return 0;
    }

    TerrainInterval a = 0.0, b, c;
    if (curve.lineAxis != 2) {
        TerrainInterval d = curve.lineAxis == 0 ? dx : dy;
        if (curve.corner != 0) d = terrainNeg(d);
        b = d;
        c = midpointWitness ? TerrainInterval(0.0) : terrainField(curve, ox, oy, status);
    } else {
        if (curve.corner == 1 || curve.corner == 2) { ox = terrainSub(TerrainInterval(1.0), ox, status); dx = terrainNeg(dx); }
        if (curve.corner == 2 || curve.corner == 3) { oy = terrainSub(TerrainInterval(1.0), oy, status); dy = terrainNeg(dy); }
        // At the owned midpoint the reflected coordinates are symbolically
        // equal and F(q)=0. Do not evaluate a rounded free-point residual.
        TerrainInterval difference = midpointWitness ? TerrainInterval(0.0) : terrainSub(ox, oy, status);
        TerrainInterval direction = terrainSub(dx, dy, status);
        // These source-level identities prove a zero quadratic coefficient.
        // A rounded/FTZ zero from arbitrary arithmetic never selects this case.
        bool reflectedEqual = (curve.corner == 1 || curve.corner == 3) ? oppositeDirection : equalDirection;
        bool linear = curve.roundness == 0.0 || reflectedEqual;
        if (!linear) a = terrainMul(TerrainInterval(curve.roundness), terrainMul(direction, direction, status), status);
        b = terrainAdd(terrainAdd(dx, dy, status),
            terrainMul(TerrainInterval(2.0 * curve.roundness), terrainMul(difference, direction, status), status), status);
        if (midpointWitness) c = 0.0;
        else c = terrainAdd(terrainSub(terrainAdd(ox, oy, status),
            terrainAdd(TerrainInterval(0.5), terrainMul(TerrainInterval(0.25), TerrainInterval(curve.roundness), status), status), status),
            terrainMul(TerrainInterval(curve.roundness), terrainMul(difference, difference, status), status), status);
        if (linear) a = 0.0;
    }
    if (status != 0) return 0;
    if (all(a == 0.0)) {
        if (b.x <= 0.0 && b.y >= 0.0) {
            // Parallel/coincident cases are classified by the certified field.
            // Unproven cancellation is ambiguity, never a tangent/miss repair.
            if (!all(b == 0.0) || (c.x <= 0.0 && c.y >= 0.0)) status |= kTerrainNumericAmbiguity | kTerrainReasonRoots;
            return 0;
        }
        roots[0] = terrainDiv(terrainNeg(c), b, status);
        return 1;
    }
    if (!(a.x > 0.0)) { status |= kTerrainNumericAmbiguity | kTerrainReasonRoots; return 0; }
    TerrainInterval discriminant = terrainSub(terrainMul(b, b, status), terrainMul(TerrainInterval(4.0), terrainMul(a, c, status), status), status);
    if (discriminant.y < 0.0) return 0;
    if (!(discriminant.x > 0.0)) { status |= kTerrainNumericAmbiguity | kTerrainReasonRoots; return 0; }
    TerrainInterval squareRoot = terrainSqrt(discriminant, status);

    // This bound comparison chooses conditioning, not a certified sign of b.
    // Either fixed branch defines one root q/a and its companion c/q. With
    // a>0, the minus-sqrt numerator gives the lower root; plus-sqrt gives upper.
    // Existing Div rejects a q enclosure containing zero before a c==0 identity;
    // all normal-domain, repeated-root and strict event-order guards remain.
    bool upperViaQ = b.x < -b.y;
    if (upperViaQ) {
        TerrainInterval q = terrainMul(TerrainInterval(-0.5), terrainSub(b, squareRoot, status), status);
        roots[0] = terrainDiv(c, q, status);
        roots[1] = terrainDiv(q, a, status);
    } else {
        TerrainInterval q = terrainMul(TerrainInterval(-0.5), terrainAdd(b, squareRoot, status), status);
        roots[0] = terrainDiv(q, a, status);
        roots[1] = terrainDiv(c, q, status);
    }

    if (!(roots[0].y < roots[1].x)) status |= kTerrainNumericAmbiguity | kTerrainReasonRoots;
    return 2;
}

bool terrainSourceWarp(TerrainInterval coordinate, TerrainScalar roundness, out TerrainInterval warped) {
    warped = 0.0;
    if (!terrainNormal(coordinate.x) || !terrainNormal(coordinate.y) || !terrainNormal(roundness) ||
        coordinate.x < 0.0 || coordinate.y > 1.0 || roundness < 0.0 || roundness > 1.0) return false;

    bool reflected = coordinate.x > 0.5;
    if (!reflected && coordinate.y > 0.5) return false;

    uint status = 0;
    TerrainInterval lower = reflected ? terrainSub(TerrainInterval(1.0), coordinate, status) : coordinate;
    TerrainInterval scale = terrainAdd(TerrainInterval(1.0), TerrainInterval(roundness), status);
    TerrainInterval discriminant = terrainSub(terrainMul(scale, scale, status),
        terrainMul(terrainMul(TerrainInterval(8.0), TerrainInterval(roundness), status), lower, status), status);
    if (status != 0 || !(discriminant.x > 0.0) || discriminant.y > 8.0) return false;

    // Enclose the existing native contourWarp sqrt without assuming IEEE RTE:
    // for a positive-normal input <=8, Vulkan's 2-ULP inverse-sqrt and inherited
    // division bounds place sqrt in [0,4]. Zero/subnormal uncertainty is rejected.
    TerrainInterval denominator = terrainAdd(scale, TerrainInterval(0.0, 4.0), status);
    warped = terrainDiv(terrainMul(TerrainInterval(2.0), lower, status), denominator, status);
    if (reflected) warped = terrainSub(TerrainInterval(1.0), warped, status);
    return status == 0;
}

TerrainInterval terrainSourceCross(TerrainInterval a, TerrainInterval b, TerrainInterval x, TerrainInterval y, inout uint status) {
    // Uploaded contour-table vertices are exact half-grid values in [0,1].
    TerrainInterval edge = b - a;
    TerrainInterval px = terrainSub(x, TerrainInterval(a.x), status);
    TerrainInterval py = terrainSub(y, TerrainInterval(a.y), status);
    return terrainSub(terrainMul(TerrainInterval(edge.x), py, status), terrainMul(TerrainInterval(edge.y), px, status), status);
}

bool terrainSourceContains(uint mask, TerrainInterval x, TerrainInterval y, out bool inside, out bool outside) {
    inside = false;
    outside = true;
    uint status = 0;
    ContourCase shape = g_contour_cases[mask];
    for (uint index = 0; index < shape.m_triangle_count; ++index) {
        ContourTriangle triangle = shape.m_triangles[index];
        TerrainInterval a = g_contour_points[triangle.m_a], b = g_contour_points[triangle.m_b], c = g_contour_points[triangle.m_c];
        TerrainInterval first = terrainSourceCross(a, b, x, y, status);
        TerrainInterval second = terrainSourceCross(b, c, x, y, status);
        TerrainInterval third = terrainSourceCross(c, a, x, y, status);
        inside = inside || (first.x > 0.0 && second.x > 0.0 && third.x > 0.0);
        outside = outside && (first.y < 0.0 || second.y < 0.0 || third.y < 0.0);
    }

    return status == 0;
}

bool terrainStraightSource(uint labels[4], float roundness[4], float3 sources[4],
    TerrainInterval x, TerrainInterval y, out float3 source) {
    source = 0.0;
    if (occupiedMask(labels) != 15) return false;

    uint firstMask = materialMask(labels, 0);
    if (firstMask != 3 && firstMask != 9) return false;

    uint secondCorner = 0;
    for (uint corner = 1; corner < 4; ++corner) if (labels[corner] != labels[0]) {
        if (secondCorner != 0 && labels[corner] != labels[secondCorner]) return false;
        secondCorner = corner;
    }

    if (secondCorner == 0) return false;

    // Exactly the two-label shared roundness and triangle-containment rule used
    // by terrainHitLabel. A strict inside/outside certificate makes the selected
    // signed score positive and its competitor nonpositive; no SDF is substituted.
    float canonicalSharedRoundness = terrainSharedSourceRoundness(labels, roundness, labels[secondCorner]);
    if (!terrainCanonicalNormal(canonicalSharedRoundness)) return false;

    TerrainScalar sharedRoundness = canonicalSharedRoundness;
    TerrainInterval warpedX, warpedY;
    if (!terrainSourceWarp(x, sharedRoundness, warpedX) || !terrainSourceWarp(y, sharedRoundness, warpedY)) return false;
    bool firstInside, firstOutside, secondInside, secondOutside;
    if (!terrainSourceContains(firstMask, warpedX, warpedY, firstInside, firstOutside) ||
        !terrainSourceContains(15u ^ firstMask, warpedX, warpedY, secondInside, secondOutside)) return false;

    uint status = 0;
    TerrainInterval perpendicular = terrainSub(firstMask == 3 ? y : x, TerrainInterval(0.5), status);
    TerrainInterval squared = terrainMul(perpendicular, perpendicular, status);
    // The straight segment's perpendicular coordinate is exactly 1/2. A normal
    // positive squared separation keeps its native Length strictly positive
    // under the inherited dot/sqrt bounds and permitted denormal flushing.
    if (status != 0 || !(squared.x >= asfloat(0x00800000u))) return false;
    if (firstInside && secondOutside) { source = sources[0]; return true; }
    if (secondInside && firstOutside) { source = sources[secondCorner]; return true; }
    return false;
}

bool terrainOpaque(uint labels[4], float roundness[4], float3 sources[4], int2 base,
    TerrainLine line, TerrainInterval dx, TerrainInterval dy, TerrainInterval hitEvent, inout OpticalResult result) {
    if (!terrainNormal(hitEvent.x) || !terrainNormal(hitEvent.y) || hitEvent.x > hitEvent.y) {
        result.status |= kTerrainNumericAmbiguity | kTerrainReasonSource;
        return false;
    }

    // Label identity need not be unique when every possible nonvacuum winner
    // carries the same source. Preserve all labels and the full first event.
    uint firstCorner = 4;
    bool common = true;
    for (uint corner = 0; corner < 4; ++corner) if (labels[corner] != 0) {
        if (firstCorner == 4) firstCorner = corner;
        else common = common && all(asuint(sources[corner]) == asuint(sources[firstCorner]));
    }

    if (firstCorner == 4) { result.status |= 2 | kTerrainReasonSource; return false; }

    float3 source = sources[firstCorner];
    if (!common) {
        TerrainInterval x = terrainLineCoordinate(line, 0, hitEvent, TerrainScalar(base.x) + 0.5, dx, result.status);
        TerrainInterval y = terrainLineCoordinate(line, 1, hitEvent, TerrainScalar(base.y) + 0.5, dy, result.status);
        if (result.status != 0) return false;

        if (x.x == x.y && y.x == y.y) {
            // There is only one possible point. Evaluate the unchanged declared
            // score/tie rule at that point, never at an event/position midpoint.
            float2 nativePoint;
            if (!terrainNativePoint(TerrainInterval(x.x, y.x), nativePoint)) {
                result.status |= kTerrainNumericAmbiguity | kTerrainReasonSource;
                return false;
            }

            uint label = terrainHitLabel(labels, roundness, nativePoint, result.status);
            if (result.status != 0) return false;

            uint selected = 4;
            for (uint corner = 0; corner < 4; ++corner) if (labels[corner] == label) { selected = corner; break; }

            if (selected == 4 || label == 0) { result.status |= 2 | kTerrainReasonSource; return false; }

            source = sources[selected];
        } else if (!terrainSourceCertificate(labels, roundness, sources, x, y, source)) {
            // Intermediate qualifier: general curved differing-source ownership
            // still needs certification before ordinary-world/emitter integration.
            result.status |= kTerrainNumericAmbiguity | kTerrainReasonSource;
            return false;
        }
    }

    result.radianceR = source.x; result.radianceG = source.y; result.radianceB = source.z;
    result.transmissionR = 0.0; result.transmissionG = 0.0; result.transmissionB = 0.0;
    return true;
}

// ------------------------------------------------------------------
// first occupied prefix and complete clip/DDA traversal
// ------------------------------------------------------------------

bool terrainCell(int2 base, TerrainLine line, TerrainInterval dx, TerrainInterval dy, bool equalDirection, bool oppositeDirection,
    TerrainInterval nearBound, TerrainInterval farBound, TerrainWitness witness, bool certificateOnly, inout OpticalResult result) {
    uint labels[4]; float roundness[4]; float3 sources[4];
    terrainCorners(base, labels, roundness, sources);
    uint mask = occupiedMask(labels);
    ++result.segments;
    if (mask == 0) return false;

    TerrainInterval ox = terrainPointCoordinate(line.origin, 0, TerrainScalar(base.x) + 0.5, result.status);
    TerrainInterval oy = terrainPointCoordinate(line.origin, 1, TerrainScalar(base.y) + 0.5, result.status);
    TerrainCurve curves[2]; bool complement;
    uint count = terrainCurves(labels, roundness, curves, complement);

    // A whole-span field certificate can prove uniform coverage without solving
    // irrelevant roots outside this cell interval. Uncertain broad bounds are
    // refined by the complete root path below; no uncertain miss is accepted.
    uint coverageStatus = 0;
    TerrainInterval domain = TerrainInterval(nearBound.x, farBound.y);
    TerrainInterval domainX = terrainLineCoordinate(line, 0, domain, TerrainScalar(base.x) + 0.5, dx, coverageStatus);
    TerrainInterval domainY = terrainLineCoordinate(line, 1, domain, TerrainScalar(base.y) + 0.5, dy, coverageStatus);
    bool wholeInside = terrainInside(mask, curves, count, complement, domainX, domainY, coverageStatus);
    if (coverageStatus == 0) {
        if (!wholeInside) return false;

        return terrainOpaque(labels, roundness, sources, base, line, dx, dy, nearBound, result);
    }

    // Descriptor ordering can prove a positive thin span while its original
    // time boxes overlap. Only whole-domain coverage may settle that span.
    if (certificateOnly) {
        result.status |= coverageStatus | kTerrainNumericAmbiguity | kTerrainReasonDda;
        return false;
    }

    TerrainInterval cuts[6]; uint cutCount = 2;
    cuts[0] = nearBound; cuts[1] = farBound;
    uint farSentinel[6]; farSentinel[0] = 0; farSentinel[1] = 1;
    bool deferredFarOverlap = false;
    for (uint i = 0; i < count; ++i) {
        TerrainInterval roots[2];
        bool ownMidpoint = witness.enabled != 0 && all(base == witness.base) && witness.curve == i;
        precise TerrainScalar rootReference = 0.0;
        TerrainInterval rootOx = ox, rootOy = oy;
        if (!ownMidpoint) {
            if (!terrainNormal(nearBound.y) || !terrainNormal(farBound.x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return false; }

            // This validated FP32 literal changes only the parameter origin.
            // Its position stays an enclosure of the ORIGINAL symbolic line.
            rootReference = nearBound.y + (farBound.x - nearBound.y) * 0.5;
            if (!terrainNormal(rootReference)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return false; }
            if (!(rootReference > nearBound.y && rootReference < farBound.x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderMiddle; return false; }

            rootOx = terrainLineCoordinate(line, 0, TerrainInterval(rootReference), TerrainScalar(base.x) + 0.5, dx, result.status);
            rootOy = terrainLineCoordinate(line, 1, TerrainInterval(rootReference), TerrainScalar(base.y) + 0.5, dy, result.status);
        }

        // The owned feature alone retains reference0 and its exact c=0 proof.
        // Other roots use signed local tau, then return to global parameters.
        uint rootCount = terrainRoots(curves[i], rootOx, rootOy, dx, dy, equalDirection, oppositeDirection, ownMidpoint, roots, result.status);
        for (uint root = 0; root < rootCount; ++root) roots[root] = terrainAdd(TerrainInterval(rootReference), roots[root], result.status);
        if (rootCount == 2 && !(roots[0].y < roots[1].x)) result.status |= kTerrainNumericAmbiguity | kTerrainReasonRoots;

        for (uint root = 0; root < rootCount; ++root) {
            TerrainInterval event = roots[root];
            // Only the algebraically proved contact at this owned feature is
            // excluded. Classification below still certifies the complete first
            // positive occupied region, including every other curve/saddle.
            if (ownMidpoint && all(nearBound == 0.0) && all(event == 0.0)) continue;
            if (event.y < nearBound.x || event.x > farBound.y) continue;
            if (!(event.x > nearBound.y)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderRootSpan; return false; }

            // A later exit may meet the far boundary without invalidating a
            // strictly ordered first occupied prefix. Keep its complete box.
            deferredFarOverlap = deferredFarOverlap || !(event.y < farBound.x);
            cuts[cutCount] = event;
            farSentinel[cutCount++] = 0;
        }
    }

    if (result.status != 0) return false;

    for (uint i = 1; i < cutCount; ++i) {
        TerrainInterval value = cuts[i]; uint sentinel = farSentinel[i]; uint j = i;
        while (j > 0 && value.x < cuts[j - 1].x) { cuts[j] = cuts[j - 1]; farSentinel[j] = farSentinel[j - 1]; --j; }
        cuts[j] = value; farSentinel[j] = sentinel;
    }

    for (uint i = 1; i < cutCount; ++i) {
        if (!(cuts[i - 1].y < cuts[i].x)) {
            result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderCutSeparation;
            if (deferredFarOverlap && (farSentinel[i - 1] != 0 || farSentinel[i] != 0)) result.status |= kTerrainOrderRootSpan;
            return false;
        }

        // The classified open gap must be entirely before every possible query
        // end. The far sentinel is never bypassed to manufacture a later hit.
        if (!(cuts[i].x <= farBound.x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderRootSpan; return false; }

        precise TerrainScalar middle = cuts[i - 1].y + (cuts[i].x - cuts[i - 1].y) * 0.5;
        if (!(middle > cuts[i - 1].y && middle < cuts[i].x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderMiddle; return false; }
        TerrainInterval px = terrainLineCoordinate(line, 0, TerrainInterval(middle), TerrainScalar(base.x) + 0.5, dx, result.status);
        TerrainInterval py = terrainLineCoordinate(line, 1, TerrainInterval(middle), TerrainScalar(base.y) + 0.5, dy, result.status);
        bool inside = terrainInside(mask, curves, count, complement, px, py, result.status);
        if (result.status != 0) return false;
        if (!inside) continue;

        return terrainOpaque(labels, roundness, sources, base, line, dx, dy, cuts[i - 1], result);
    }

    if (deferredFarOverlap) result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderRootSpan;
    return false;
}

// The caller selects one actual dual cell and literal endpoints contained in
// its closed square. Four C0 corner connectors never leave that square; this
// shares the existing first-prefix/root/source rules without a long-ray DDA.
TERRAIN_COMPLETE_QUERY_BOUNDARY
OpticalResult traceTerrainCellConnector(TerrainLine line, int2 base) {
    OpticalResult result = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0u, 0u};
    if (!terrainValidPoint(line.origin) || !terrainValidPoint(line.target) || line.connector != 1u ||
        line.origin.nearCells != 0.0 || line.target.nearCells != 0.0 ||
        any(base < 0) || base.x + 1 >= int(g_trace.width) || base.y + 1 >= int(g_trace.height)) {
        result.status = kTerrainNumericAmbiguity | kTerrainReasonOwnership;
        return result;
    }

    float2 low = float2(base) + 0.5;
    if (any(line.origin.probe < low) || any(line.origin.probe > low + 1.0) ||
        any(line.target.probe < low) || any(line.target.probe > low + 1.0)) {
        result.status = kTerrainNumericAmbiguity | kTerrainReasonOwnership;
        return result;
    }
    if (all(line.origin.probe == line.target.probe)) return result;

    uint labels[4]; float roundness[4]; float3 sources[4];
    terrainCorners(base, labels, roundness, sources);
    if (occupiedMask(labels) == 0u) return result;

    TerrainInterval dx, dy;
    terrainLineDirection(line, dx, dy, result.status);
    if (result.status != 0) return result;
    bool zeroX = all(dx == 0.0), zeroY = all(dy == 0.0);
    if ((zeroX && zeroY) || (!zeroX && !(dx.x > 0.0 || dx.y < 0.0)) ||
        (!zeroY && !(dy.x > 0.0 || dy.y < 0.0))) {
        result.status = kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return result;
    }

    bool equalDirection = dx.x == dx.y && dy.x == dy.y && dx.x == dy.x;
    bool oppositeDirection = dx.x == dx.y && dy.x == dy.y && dx.x == -dy.x;
    TerrainWitness none = {int2(0), 0u, 0u};
    terrainCell(base, line, dx, dy, equalDirection, oppositeDirection,
        TerrainInterval(0.0), TerrainInterval(1.0), none, false, result);
    return result;
}

TERRAIN_COMPLETE_QUERY_BOUNDARY
OpticalResult traceTerrainParametricWitness(TerrainLine line, TerrainInterval requestedNear, TerrainInterval requestedFar, uint initialStatus, TerrainWitness witness) {
    OpticalResult result = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, initialStatus, 0};
    if (!terrainNormal(g_trace.cellMetres) || !(g_trace.cellMetres > 0.0) ||
        !terrainValidPoint(line.origin) || (line.connector != 0 && !terrainValidPoint(line.target)) ||
        !terrainNormal(line.rawDirection.x) || !terrainNormal(line.rawDirection.y) ||
        !terrainNormal(requestedNear.x) || !terrainNormal(requestedNear.y) ||
        !terrainNormal(requestedFar.x) || !terrainNormal(requestedFar.y) || requestedNear.x < 0.0 ||
        requestedNear.x > requestedNear.y || requestedFar.x > requestedFar.y) {
        result.status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return result;
    }
    if (result.status != 0) return result;

    TerrainInterval dx, dy;
    terrainLineDirection(line, dx, dy, result.status);
    bool zeroX = all(dx == 0.0), zeroY = all(dy == 0.0);
    if (result.status != 0) return result;
    if (zeroX && zeroY) {
        // Singleton zero direction bounds prove identical symbolic endpoints.
        if (line.connector != 0) return result;
        result.status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return result;
    }
    if ((!zeroX && !(dx.x > 0.0 || dx.y < 0.0)) || (!zeroY && !(dy.x > 0.0 || dy.y < 0.0))) {
        result.status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
        return result;
    }
    if (all(requestedNear == requestedFar) && requestedNear.x == requestedNear.y) return result;
    if (!(requestedNear.y < requestedFar.x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOrder | kTerrainOrderRequestedSpan; return result; }

    bool equalDirection = dx.x == dx.y && dy.x == dy.y && dx.x == dy.x;
    bool oppositeDirection = dx.x == dx.y && dy.x == dy.y && dx.x == -dy.x;
    if (line.connector == 0) {
        equalDirection = line.rawDirection.x == line.rawDirection.y;
        oppositeDirection = line.rawDirection.x == -line.rawDirection.y;
    }
    bool equalOriginAxes = terrainPointEqualAxes(line.origin);
    if (line.connector != 0 && equalOriginAxes && terrainPointEqualAxes(line.target)) equalDirection = true;

    TerrainInterval entry = requestedNear, exit = requestedFar;
    bool outsideEntry = false, outsideExit = false;
    for (uint axis = 0; axis < 2; ++axis) {
        TerrainScalar extent = TerrainScalar(axis == 0 ? g_trace.width : g_trace.height);
        TerrainInterval direction = axis == 0 ? dx : dy;
        bool zero = axis == 0 ? zeroX : zeroY;
        if (zero) {
            TerrainInterval origin = terrainPointCoordinate(line.origin, axis, 0.0, result.status);
            if (origin.y < 0.0 || origin.x >= extent) {
                if (g_trace.opaqueBoundary != 0) result.transmissionR = result.transmissionG = result.transmissionB = 0.0;
                return result;
            }
            if (!(origin.x >= 0.0 && origin.y < extent)) result.status |= kTerrainNumericAmbiguity | kTerrainReasonClip;
            continue;
        }

        TerrainInterval first = terrainLinePlane(line, axis, 0.0, direction, result.status);
        TerrainInterval last = terrainLinePlane(line, axis, extent, direction, result.status);
        if (direction.y < 0.0) { TerrainInterval swap = first; first = last; last = swap; }
        if (first.x > entry.y) { entry = first; outsideEntry = true; }
        else if (first.y >= entry.x) {
            bool exactTie = all(first == entry) && first.x == first.y;
            if (!exactTie) result.status |= kTerrainNumericAmbiguity | kTerrainReasonClip;
        }
        if (last.y < exit.x) { exit = last; outsideExit = true; }
        else if (last.x <= exit.y) {
            bool exactTie = all(last == exit) && last.x == last.y;
            if (!exactTie) result.status |= kTerrainNumericAmbiguity | kTerrainReasonClip;
        }
    }
    if (result.status != 0) return result;
    if (g_trace.opaqueBoundary != 0 && outsideEntry) {
        result.transmissionR = result.transmissionG = result.transmissionB = 0.0; return result;
    }
    if (entry.x >= exit.y) {
        if (g_trace.opaqueBoundary != 0) result.transmissionR = result.transmissionG = result.transmissionB = 0.0;
        return result;
    }
    if (!(entry.y < exit.x)) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonClip; return result; }

    int2 base;
    for (uint axis = 0; axis < 2; ++axis) {
        TerrainInterval direction = axis == 0 ? dx : dy;
        TerrainInterval p = terrainLineCoordinate(line, axis, entry, 0.0, direction, result.status);
        TerrainScalar extent = TerrainScalar(axis == 0 ? g_trace.width : g_trace.height);
        if (!all(isfinite(p)) || p.x < -0.5 || p.y > extent + 0.5) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; return result; }

        // A singleton position at a literal dual plane proves ray-forward
        // ownership. Bounds/range checks above precede every integer conversion.
        if (p.x == p.y) {
            TerrainScalar originFloor = floor(p.x);
            if (p.x == originFloor + 0.5) {
                base[axis] = int(originFloor) - (direction.y < 0.0 ? 1 : 0);
                continue;
            }
        }

        TerrainInterval shifted = terrainSub(p, TerrainInterval(0.5), result.status);
        int first = int(floor(shifted.x)), last = int(floor(shifted.y));
        if (first != last) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; return result; }
        base[axis] = first;
    }

    uint entryAxis = 2;
    TerrainScalar entryPlane = 0.0;
    uint maximumSegments = g_trace.width + g_trace.height + 2;
    for (uint segment = 0; segment < maximumSegments; ++segment) {
        if (result.status != 0) return result;
        TerrainInterval next = exit;
        uint stepMask = 0;
        uint nextAxis = 2;
        TerrainScalar nextPlane = 0.0;
        TerrainInterval events[2]; bool active[2];
        for (uint axis = 0; axis < 2; ++axis) {
            TerrainInterval direction = axis == 0 ? dx : dy;
            active[axis] = axis == 0 ? !zeroX : !zeroY;
            if (!active[axis]) continue;

            TerrainScalar plane = TerrainScalar(base[axis]) + (direction.x > 0.0 ? 1.5 : 0.5);
            events[axis] = terrainLinePlane(line, axis, plane, direction, result.status);
            if (events[axis].y < next.x) {
                next = events[axis]; stepMask = 1u << axis;
                nextAxis = axis; nextPlane = plane;
            } else if (events[axis].x <= next.y) {
                bool exactSame = stepMask != 0 && equalOriginAxes && equalDirection && base.x == base.y;
                bool exactExit = stepMask == 0 && all(events[axis] == exit) && exit.x == exit.y;
                if (exactSame) stepMask |= 1u << axis;
                else if (!exactExit) {
                    int order = nextAxis < 2 ? terrainPlaneEventOrder(line, dx, dy,
                        axis, plane, nextAxis, nextPlane, result.status) : 0;
                    if (order < 0) {
                        next = events[axis]; stepMask = 1u << axis;
                        nextAxis = axis; nextPlane = plane;
                    } else if (order == 0) result.status |= kTerrainNumericAmbiguity | kTerrainReasonDda;
                }
            }
        }

        bool certificateOnly = false;
        if (!(entry.y < next.x)) {
            int order = entryAxis < 2 && nextAxis < 2 ? terrainPlaneEventOrder(line, dx, dy,
                entryAxis, entryPlane, nextAxis, nextPlane, result.status) : 0;
            if (order < 0) certificateOnly = true;
            else result.status |= kTerrainNumericAmbiguity | kTerrainReasonDda;
        }

        if (result.status != 0) { result.status |= kTerrainNumericAmbiguity | kTerrainReasonDda; return result; }

        if (terrainCell(base, line, dx, dy, equalDirection, oppositeDirection, entry, next, witness, certificateOnly, result) || result.status != 0) return result;

        if (stepMask == 0) {
            if (g_trace.opaqueBoundary != 0 && outsideExit) result.transmissionR = result.transmissionG = result.transmissionB = 0.0;
            return result;
        }

        for (uint axis = 0; axis < 2; ++axis) if ((stepMask & (1u << axis)) != 0) base[axis] += (axis == 0 ? dx.x : dy.x) > 0.0 ? 1 : -1;

        entry = next;
        entryAxis = nextAxis; entryPlane = nextPlane;
    }
    result.status |= 4 | kTerrainReasonDda;
    return result;
}
