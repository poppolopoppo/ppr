// ------------------------------------------------------------------
// scalar FP32 optical transport qualification
// ------------------------------------------------------------------

// Scalar fields deliberately match the host's structured-buffer protocol;
// float3/array padding is not inferred across backend storage layouts.
struct OpticalCell
{
    float extinctionR, extinctionG, extinctionB;
    float volumeR, volumeG, volumeB;
    float surfaceR, surfaceG, surfaceB;
    uint kind; // 0 vacuum, 1 opaque surface, 2 absorbing/emitting medium
};

struct OpticalRay
{
    float originX, originY; // Local fine cells; world origin removed in CPU double.
    float directionX, directionY;
    float nearMetres, farMetres;
};

struct OpticalResult
{
    float radianceR, radianceG, radianceB;
    float transmissionR, transmissionG, transmissionB;
    uint status; // Unchanged low-byte errors; terrain high-bit reason lanes are declared in radiance_terrain.
    uint segments;
};

// Six independent binary16 channels, low/high halves in order, plus status.
// Interval stride stays 16 bytes; local connector spans use g_segments separately.
struct PackedInterval
{
    uint radianceRG, radianceBtransmissionR, transmissionGB, status;
};

RWStructuredBuffer<PackedInterval> g_packed_results;
StructuredBuffer<PackedInterval> g_packed_upper;
RWStructuredBuffer<uint> g_segments;

bool validInterval(OpticalResult value)
{
    float3 radiance = float3(value.radianceR, value.radianceG, value.radianceB);
    float3 transmission = float3(value.transmissionR, value.transmissionG, value.transmissionB);
    return all(isfinite(radiance)) && all(isfinite(transmission)) && all(radiance >= 0.0) &&
        all(transmission >= 0.0) && all(transmission <= 1.0);
}

// ------------------------------------------------------------------
// interval-only deterministic binary16 RTE storage codec
// ------------------------------------------------------------------

// Called only after packInterval validates finite, nonnegative, <=65504 input.
// Integer rounding makes the storage contract independent of native conversion
// mode. All physical transport/composition arithmetic remains FP32.
uint packStoredHalfRte(float value)
{
    uint bits = asuint(value);
    uint sign = (bits >> 16) & 32768;
    uint exponent = (bits >> 23) & 255;
    uint fraction = bits & 8388607;
    if (exponent < 102)
        return sign;

    if (exponent < 113)
    {
        // Subnormal quantum is 2^-24. The guarded exponent bounds the shift to
        // [14,24], including the 2^-25 tie and carry into minimum normal 0x0400.
        uint shift = 126 - exponent;
        uint significand = 8388608 | fraction;
        uint retained = significand >> shift;
        uint remainder = significand & ((1u << shift) - 1);
        uint midpoint = 1u << (shift - 1);
        if (remainder > midpoint || (remainder == midpoint && (retained & 1) != 0))
            ++retained;

        return sign | retained;
    }

    uint retained = ((exponent - 112) << 10) | (fraction >> 13);
    uint remainder = fraction & 8191;
    if (remainder > 4096 || (remainder == 4096 && (retained & 1) != 0))
        ++retained;

    // The validated finite-half range prevents a carry into infinity.
    return sign | retained;
}

// Preserve sign, infinities and NaN payloads so invalid/corrupt optical values
// are rejected by validInterval rather than disguised as zero.
float unpackStoredHalf(uint halfBits)
{
    uint sign = (halfBits & 32768) << 16;
    uint exponent = (halfBits >> 10) & 31;
    uint fraction = halfBits & 1023;
    if (exponent == 31)
        return asfloat(sign | 2139095040u | (fraction << 13));

    if (exponent == 0)
    {
        if (fraction == 0)
            return asfloat(sign);

        uint floatExponent = 113;
        // A nonzero 10-bit fraction needs at most ten exact normalization shifts.
        while ((fraction & 1024) == 0)
        {
            fraction <<= 1;
            --floatExponent;
        }

        return asfloat(sign | (floatExponent << 23) | ((fraction & 1023) << 13));
    }

    return asfloat(sign | ((exponent + 112) << 23) | (fraction << 13));
}

PackedInterval packInterval(OpticalResult value)
{
    PackedInterval packed = {0, 0, 0, value.status};
    if (!validInterval(value) || any(float3(value.radianceR, value.radianceG, value.radianceB) > 65504.0))
    {
        packed.status |= 16;
        return packed;
    }

    packed.radianceRG = packStoredHalfRte(value.radianceR) | (packStoredHalfRte(value.radianceG) << 16);
    packed.radianceBtransmissionR = packStoredHalfRte(value.radianceB) | (packStoredHalfRte(value.transmissionR) << 16);
    packed.transmissionGB = packStoredHalfRte(value.transmissionG) | (packStoredHalfRte(value.transmissionB) << 16);
    return packed;
}

OpticalResult unpackInterval(PackedInterval packed)
{
    OpticalResult result = {unpackStoredHalf(packed.radianceRG & 65535), unpackStoredHalf(packed.radianceRG >> 16),
        unpackStoredHalf(packed.radianceBtransmissionR & 65535), unpackStoredHalf(packed.radianceBtransmissionR >> 16),
        unpackStoredHalf(packed.transmissionGB & 65535), unpackStoredHalf(packed.transmissionGB >> 16), packed.status, 0};
    if (!validInterval(result))
        result.status |= 16;

    return result;
}

struct TraceParameters
{
    uint width, height, rayCount, opaqueBoundary;
    float cellMetres;
    uint hierarchyLevels, reserved1, reserved2;
};

ConstantBuffer<TraceParameters> g_trace;
StructuredBuffer<OpticalCell> g_cells;
StructuredBuffer<OpticalRay> g_rays;
RWStructuredBuffer<OpticalResult> g_results;

// ------------------------------------------------------------------
// immutable-source aligned vacuum hierarchy
// ------------------------------------------------------------------

struct HierarchyLevel
{
    uint width, height, offset, blockShift;
};

struct HierarchyBuild
{
    uint level, dispatchRowThreads, nodes, reserved;
};

ConstantBuffer<HierarchyBuild> g_build;
StructuredBuffer<HierarchyLevel> g_hierarchy;
RWStructuredBuffer<uint> g_flags_write;
StructuredBuffer<uint> g_flags;

[shader("compute")]
[numthreads(64, 1, 1)]
void buildVacuumHierarchy(uint3 thread : SV_DispatchThreadID)
{
    uint record = thread.x + thread.y * g_build.dispatchRowThreads;
    if (record >= g_build.nodes)
        return;

    HierarchyLevel level = g_hierarchy[g_build.level];
    if (g_build.level == 0)
    {
        g_flags_write[level.offset + record] = g_cells[record].kind == 0 ? 1 : 0;
        return;
    }

    HierarchyLevel child = g_hierarchy[g_build.level - 1];
    uint x = 2 * (record % level.width);
    uint y = 2 * (record / level.width);
    // Padding is not optical vacuum. Even a degenerate axis needs all four
    // children, matching the independently authored scalar CPU hierarchy.
    bool vacuum = x + 1 < child.width && y + 1 < child.height;
    if (vacuum)
    {
        uint first = child.offset + y * child.width + x;
        vacuum = g_flags_write[first] == 1 && g_flags_write[first + 1] == 1 &&
            g_flags_write[first + child.width] == 1 && g_flags_write[first + child.width + 1] == 1;
    }

    // Each dispatch writes a new disjoint range. Host inter-dispatch barriers
    // complete all child writes before parent reads; no LDS reduction is used.
    g_flags_write[level.offset + record] = vacuum ? 1 : 0;
}

// ------------------------------------------------------------------
// generic-cell normal-arithmetic qualification domain
// ------------------------------------------------------------------

// Raw representations are inspected before floating identities so permitted
// FTZ cannot disguise a nonzero operand as an exact zero. Half subnormals such
// as 2^-24 are normal FP32 values and remain valid storage/transport inputs.
bool cellNormalOrZero(float value)
{
    uint magnitude = asuint(value) & 0x7fffffffu;
    return magnitude == 0 || (magnitude >= 0x00800000u && magnitude < 0x7f800000u);
}

bool cellPositiveNormal(float value)
{
    uint bits = asuint(value);
    return bits >= 0x00800000u && bits < 0x7f800000u;
}

bool cellNonnegativeNormalOrZero(float value)
{
    uint bits = asuint(value);
    return (bits & 0x7fffffffu) == 0 || (bits >= 0x00800000u && bits < 0x7f800000u);
}

float cellPositiveProduct(float a, float b, inout uint status)
{
    if (!cellNonnegativeNormalOrZero(a) || !cellNonnegativeNormalOrZero(b))
    {
        status |= 1;
        return 0.0;
    }

    if ((asuint(a) & 0x7fffffffu) == 0 || (asuint(b) & 0x7fffffffu) == 0)
        return 0.0;

    // The bit-observed intermediate is part of the supported arithmetic order;
    // a later large factor must not hide its positive underflow or overflow.
    float product = a * b;
    if (!cellPositiveNormal(product))
        status |= 1;

    return product;
}

float3 cellPositiveProducts(float3 a, float3 b, inout uint status)
{
    float x = cellPositiveProduct(a.x, b.x, status);
    float y = cellPositiveProduct(a.y, b.y, status);
    float z = cellPositiveProduct(a.z, b.z, status);
    return float3(x, y, z);
}

// ------------------------------------------------------------------
// half-open slab and ray-forward cell ownership
// ------------------------------------------------------------------

bool clipAxis(float origin, float direction, float extent, inout float nearDistance, inout float farDistance)
{
    if (direction == 0.0)
        return origin >= 0.0 && origin < extent;

    float a = -origin / direction;
    float b = (extent - origin) / direction;
    nearDistance = max(nearDistance, min(a, b));
    farDistance = min(farDistance, max(a, b));
    return nearDistance < farDistance;
}

bool cellAfter(float cursor, float origin, float direction, uint extent, out uint cell)
{
    uint low = 0;
    uint high = extent + 1;
    while (low < high)
    {
        uint middle = low + (high - low) / 2;
        float event = direction == 0.0 ? float(middle) : (float(middle) - origin) / direction;
        bool passed = direction == 0.0 ? event <= origin : event <= cursor;
        if (direction < 0.0 ? !passed : passed)
            low = middle + 1;
        else
            high = middle;
    }

    cell = low > 0 ? low - 1 : 0;
    return low > 0 && low <= extent;
}

float sourceIntegral(float extinction, float distanceMetres, float depth)
{
    if (depth < 0.001)
    {
        // Stable (1-exp(-x))/x near zero, including exact zero extinction.
        // The first omitted relative term is x^4/120, below FP32 rounding here.
        return distanceMetres * (1.0 - depth * (0.5 - depth * (1.0 / 6.0 - depth / 24.0)));
    }

    return (1.0 - exp(-depth)) / extinction;
}

// ------------------------------------------------------------------
// complete bounded scalar ray traversal
// ------------------------------------------------------------------

OpticalResult traceRay(OpticalRay ray)
{
    OpticalResult result = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0, 0};
    if (!cellNormalOrZero(ray.originX) || !cellNormalOrZero(ray.originY) ||
        !cellNormalOrZero(ray.directionX) || !cellNormalOrZero(ray.directionY) ||
        !cellNonnegativeNormalOrZero(ray.nearMetres) || !cellNonnegativeNormalOrZero(ray.farMetres) ||
        !cellPositiveNormal(g_trace.cellMetres))
    {
        result.status = 1;
        return result;
    }

    float2 origin = float2(ray.originX, ray.originY);
    float2 rawDirection = float2(ray.directionX, ray.directionY);
    float2 direction = normalize(rawDirection);
    float requestedNear = ray.nearMetres / g_trace.cellMetres;
    float requestedFar = ray.farMetres / g_trace.cellMetres;
    if (!cellNormalOrZero(direction.x) || !cellNormalOrZero(direction.y) ||
        (rawDirection.x != 0.0 && direction.x == 0.0) || (rawDirection.y != 0.0 && direction.y == 0.0) ||
        !cellNonnegativeNormalOrZero(requestedNear) || !cellNonnegativeNormalOrZero(requestedFar) ||
        (ray.nearMetres != 0.0 && requestedNear == 0.0) || (ray.farMetres != 0.0 && requestedFar == 0.0))
    {
        result.status = 1;
        return result;
    }

    if (ray.nearMetres == ray.farMetres)
        return result;

    if (!(requestedNear < requestedFar))
    {
        result.status = 1;
        return result;
    }

    float worldNear = requestedNear;
    float worldFar = requestedFar;
    bool hits = clipAxis(origin.x, direction.x, float(g_trace.width), worldNear, worldFar);
    hits = clipAxis(origin.y, direction.y, float(g_trace.height), worldNear, worldFar) && hits;
    hits = hits && worldNear < worldFar;
    if (g_trace.opaqueBoundary != 0 && (!hits || worldNear > requestedNear))
    {
        result.transmissionR = result.transmissionG = result.transmissionB = 0.0;
        return result;
    }

    if (!hits)
        return result;

    float3 radiance = 0.0;
    float3 transmission = 1.0;
    float cursor = worldNear;
    uint maximumSegments = g_trace.width + g_trace.height;
    for (uint step = 0; step < maximumSegments && cursor < worldFar; ++step)
    {
        uint x, y;
        if (!cellAfter(cursor, origin.x, direction.x, g_trace.width, x) ||
            !cellAfter(cursor, origin.y, direction.y, g_trace.height, y))
        {
            result.status = 2;
            return result;
        }

        uint blockSize = 1;
        bool vacuum = false;
        for (uint candidate = g_trace.hierarchyLevels; candidate > 0; --candidate)
        {
            HierarchyLevel level = g_hierarchy[candidate - 1];
            uint blockX = x >> level.blockShift;
            uint blockY = y >> level.blockShift;
            if (g_flags[level.offset + blockY * level.width + blockX] == 1)
            {
                blockSize = 1u << level.blockShift;
                vacuum = true;
                break;
            }
        }

        uint blockX = (x / blockSize) * blockSize;
        uint blockY = (y / blockSize) * blockSize;
        float next = worldFar;
        if (direction.x != 0.0)
        {
            float exitX = float(direction.x > 0.0 ? blockX + blockSize : blockX);
            next = min(next, (exitX - origin.x) / direction.x);
        }
        if (direction.y != 0.0)
        {
            float exitY = float(direction.y > 0.0 ? blockY + blockSize : blockY);
            next = min(next, (exitY - origin.y) / direction.y);
        }

        if (!(next > cursor))
        {
            result.status = 3;
            return result;
        }

        ++result.segments;
        if (vacuum)
        {
            cursor = next;
            continue;
        }

        OpticalCell cell = g_cells[y * g_trace.width + x];
        if (cell.kind == 1)
        {
            float3 contribution = cellPositiveProducts(transmission,
                float3(cell.surfaceR, cell.surfaceG, cell.surfaceB), result.status);
            if (result.status != 0)
                return result;

            radiance += contribution;
            transmission = 0.0;
            cursor = worldFar;
            break;
        }

        if (cell.kind == 2)
        {
            float spanCells = next - cursor;
            if (!cellPositiveNormal(spanCells))
            {
                result.status = 1;
                return result;
            }

            float distanceMetres = cellPositiveProduct(spanCells, g_trace.cellMetres, result.status);
            float3 extinction = float3(cell.extinctionR, cell.extinctionG, cell.extinctionB);
            float3 source = float3(cell.volumeR, cell.volumeG, cell.volumeB);
            float3 attenuation;
            float3 integral = 0.0;
            for (uint channel = 0; channel < 3; ++channel)
            {
                float depth = cellPositiveProduct(extinction[channel], distanceMetres, result.status);
                attenuation[channel] = extinction[channel] == 0.0 ? 1.0 : exp(-depth);
                if (!cellPositiveNormal(attenuation[channel]) || attenuation[channel] > 1.0)
                    result.status |= 1;

                // An unneeded integral cannot affect radiance. Every needed
                // positive source integral must itself remain normal and finite.
                if (source[channel] != 0.0)
                {
                    integral[channel] = sourceIntegral(extinction[channel], distanceMetres, depth);
                    if (!cellPositiveNormal(integral[channel]))
                        result.status |= 1;
                }
            }

            if (result.status != 0)
                return result;

            float3 weightedSource = cellPositiveProducts(transmission, source, result.status);
            float3 contribution = cellPositiveProducts(weightedSource, integral, result.status);
            float3 nextTransmission = cellPositiveProducts(transmission, attenuation, result.status);
            if (result.status != 0)
                return result;

            radiance += contribution;
            transmission = nextTransmission;
        }

        cursor = next;
    }

    if (cursor < worldFar)
    {
        result.status = 4;
        return result;
    }

    if (g_trace.opaqueBoundary != 0 && worldFar < requestedFar)
        transmission = 0.0;

    if (!all(isfinite(radiance)) || !all(isfinite(transmission)) || any(radiance < 0.0) ||
        any(transmission < 0.0) || any(transmission > 1.0))
    {
        result.status = 1;
        return result;
    }

    result.radianceR = radiance.x;
    result.radianceG = radiance.y;
    result.radianceB = radiance.z;
    result.transmissionR = transmission.x;
    result.transmissionG = transmission.y;
    result.transmissionB = transmission.z;
    return result;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void traceOptical(uint3 dispatchThread : SV_DispatchThreadID)
{
    if (dispatchThread.x >= g_trace.rayCount)
        return;

    g_results[dispatchThread.x] = traceRay(g_rays[dispatchThread.x]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void traceOpticalPacked(uint3 dispatchThread : SV_DispatchThreadID)
{
    if (dispatchThread.x >= g_trace.rayCount)
        return;

    OpticalResult result = traceRay(g_rays[dispatchThread.x]);
    g_packed_results[dispatchThread.x] = packInterval(result);
    g_segments[dispatchThread.x] = result.segments;
}

#include "radiance_terrain.slang"

// ------------------------------------------------------------------
// fixed-lattice FP32 cascade qualification
// ------------------------------------------------------------------

struct Direction
{
    float x, y;
};

struct CascadeParameters
{
    uint probeWidth, probeHeight, directionCount, level;
    float spacing, nearCells, terminalFarCells;
    uint parentWidth;
    uint parentHeight, parentDirectionOffset, directionOffset, dispatchRowThreads;
    float parentSpacing, parentNearCells;
    uint reserved0, reserved1;
};

ConstantBuffer<CascadeParameters> g_cascade;
StructuredBuffer<Direction> g_directions;
StructuredBuffer<OpticalResult> g_upper;

OpticalResult traceConnector<let terrain : bool>(float2 start, float2 end)
{
    OpticalResult invalid = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1, 0};
    if (!terrain)
    {
        if (!cellNormalOrZero(start.x) || !cellNormalOrZero(start.y) ||
            !cellNormalOrZero(end.x) || !cellNormalOrZero(end.y) || !cellPositiveNormal(g_trace.cellMetres))
            return invalid;

        // Only equality of the validated endpoints proves a zero connector.
        // A computed length that flushes/collapses must remain a visible error.
        if (all(start == end))
        {
            OpticalResult identity = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0, 0};
            return identity;
        }
    }

    float2 delta = end - start;
    float distanceCells = length(delta);
    if (!terrain)
    {
        if (!cellNormalOrZero(delta.x) || !cellNormalOrZero(delta.y) || !cellPositiveNormal(distanceCells) ||
            (start.x != end.x && delta.x == 0.0) || (start.y != end.y && delta.y == 0.0))
            return invalid;
    }
    else if (distanceCells == 0.0)
    {
        OpticalResult identity = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0, 0};
        return identity;
    }

    OpticalRay ray = {start.x, start.y, delta.x / distanceCells, delta.y / distanceCells,
        0.0, distanceCells * g_trace.cellMetres};
    if (!terrain)
    {
        if (!cellNormalOrZero(ray.directionX) || !cellNormalOrZero(ray.directionY) ||
            (delta.x != 0.0 && ray.directionX == 0.0) || (delta.y != 0.0 && ray.directionY == 0.0) ||
            !cellPositiveNormal(ray.farMetres))
            return invalid;
    }

    return terrain ? traceTerrainRay(ray) : traceRay(ray);
}

OpticalResult evaluateCascadeTop<let terrain : bool>(uint record)
{
    uint bin = record % g_cascade.directionCount;
    uint probe = record / g_cascade.directionCount;
    float2 position = 0.5 + float2(probe % g_cascade.probeWidth, probe / g_cascade.probeWidth) * g_cascade.spacing;
    Direction direction = g_directions[g_cascade.directionOffset + bin];
    if (terrain)
    {
        TerrainPoint origin = {position, 0.0, 0.0};
        TerrainLine line = {origin, origin, float2(direction.x, direction.y), 0};
        return traceTerrainParametric(line, float2(g_cascade.nearCells), float2(g_cascade.terminalFarCells), 0);
    }

    OpticalRay ray = {position.x, position.y, direction.x, direction.y,
        g_cascade.nearCells * g_trace.cellMetres, g_cascade.terminalFarCells * g_trace.cellMetres};
    return traceRay(ray);
}

OpticalResult evaluateCascadeMerge<let terrain : bool>(uint record, bool packedStorage)
{
    uint bin = record % g_cascade.directionCount;
    uint probe = record / g_cascade.directionCount;
    uint2 coordinate = uint2(probe % g_cascade.probeWidth, probe / g_cascade.probeWidth);
    float2 position = 0.5 + float2(coordinate) * g_cascade.spacing;
    Direction parentDirection = g_directions[g_cascade.directionOffset + bin];
    // Each stored slot has ONE near origin, independent of which angular child
    // or upper probe is used to reconstruct the far continuation.
    float2 start = 0.0;
    if (!terrain)
        start = position + g_cascade.nearCells * float2(parentDirection.x, parentDirection.y);

    uint2 floorParent = coordinate / 2;
    uint2 ceilParent = floorParent + coordinate % 2;
    float2 fraction = float2(coordinate % 2) * 0.5;
    float3 radiance = 0.0;
    float3 transmission = 0.0;
    uint status = 0;
    uint segments = 0;
    bool selectedUpperOrder = false;
    uint upperOrderSelection = 0;

    for (uint child = 0; child < 2; ++child)
    {
        uint upperBin = 2 * bin + child;
        Direction childDirection = g_directions[g_cascade.parentDirectionOffset + upperBin];
        for (uint y = 0; y < 2; ++y)
        {
            for (uint x = 0; x < 2; ++x)
            {
                float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                    (y == 0 ? 1.0 - fraction.y : fraction.y);
                if (weight == 0.0)
                    continue;

                // True floor/ceil support; zero-weight dependencies never form
                // an address, including one-probe degenerate world axes.
                uint2 upperCoordinate = uint2(x == 0 ? floorParent.x : ceilParent.x,
                    y == 0 ? floorParent.y : ceilParent.y);
                if (upperCoordinate.x >= g_cascade.parentWidth || upperCoordinate.y >= g_cascade.parentHeight)
                {
                    status = 8;
                    if (terrain) status |= kTerrainReasonArithmetic;
                    continue;
                }

                uint upperRecord = (upperCoordinate.y * g_cascade.parentWidth + upperCoordinate.x) *
                    (2 * g_cascade.directionCount) + upperBin;
                OpticalResult upper;
                if (packedStorage)
                    upper = unpackInterval(g_packed_upper[upperRecord]);
                else
                    upper = g_upper[upperRecord];
                float2 upperPosition = 0.5 + float2(upperCoordinate) * g_cascade.parentSpacing;
                OpticalResult connector;
                if (terrain)
                {
                    TerrainPoint lower = {position, float2(parentDirection.x, parentDirection.y), g_cascade.nearCells};
                    TerrainPoint upperStart = {upperPosition, float2(childDirection.x, childDirection.y), g_cascade.parentNearCells};
                    TerrainLine line = {lower, upperStart, 0.0, 1};
                    connector = traceTerrainParametric(line, float2(0.0), float2(1.0), 0);
                }
                else
                {
                    float2 end = upperPosition + g_cascade.parentNearCells * float2(childDirection.x, childDirection.y);
                    connector = traceConnector<false>(start, end);
                }

                if (terrain)
                {
                    // Preserve low-byte OR exactly. Parent reasons describe an
                    // earlier invocation, even if they were local at that level.
                    status |= (connector.status | upper.status) & 0xffu;
                    status |= connector.status & kTerrainLocalReasonMask;
                    status |= ((upper.status & kTerrainLocalReasonMask) << 8) |
                        (upper.status & kTerrainInheritedReasonMask);
                    status |= (connector.status | upper.status) & kTerrainOrderSiteMask;

                    // C0 selects the first positive-weight upper with ORDER,
                    // not the first arbitrary failure. This metadata is local
                    // to C0; the site-bit union above remains independent.
                    if (g_cascade.nearCells == 0.0 && !selectedUpperOrder &&
                        (upper.status & (kTerrainReasonOrder | (kTerrainReasonOrder << 8))) != 0)
                    {
                        upperOrderSelection = (child * 4u + y * 2u + x) << 28;
                        if ((upper.status & (kTerrainReasonOrder << 8)) != 0)
                            upperOrderSelection |= 1u << 31;

                        selectedUpperOrder = true;
                    }
                }
                else
                    status |= connector.status | upper.status;
                segments += connector.segments;

                float3 connectorL = float3(connector.radianceR, connector.radianceG, connector.radianceB);
                float3 connectorT = float3(connector.transmissionR, connector.transmissionG, connector.transmissionB);
                float3 upperL = float3(upper.radianceR, upper.radianceG, upper.radianceB);
                float3 upperT = float3(upper.transmissionR, upper.transmissionG, upper.transmissionB);
                // Compose each connector/upper pair BEFORE one bilinear weight,
                // then average equal-width angular children with measure 1/2.
                if (!terrain)
                {
                    float3 farRadiance = cellPositiveProducts(connectorT, upperL, status);
                    float3 pairRadiance = connectorL + farRadiance;

                    float measure = cellPositiveProduct(0.5, weight, status);
                    float3 weightedRadiance = cellPositiveProducts(float3(measure), pairRadiance, status);
                    float3 weightedTransmission = cellPositiveProducts(float3(measure), connectorT, status);
                    float3 pairTransmission = cellPositiveProducts(weightedTransmission, upperT, status);

                    radiance += weightedRadiance;
                    transmission += pairTransmission;
                }
                else
                {
                    radiance += 0.5 * weight * (connectorL + connectorT * upperL);
                    transmission += 0.5 * weight * connectorT * upperT;
                }
            }
        }
    }

    if (terrain) status |= upperOrderSelection;

    if (!all(isfinite(radiance)) || !all(isfinite(transmission)) || any(radiance < 0.0) ||
        any(transmission < 0.0) || any(transmission > 1.0))
        status |= 1 | (terrain ? kTerrainReasonArithmetic : 0u);

    OpticalResult result = {radiance.x, radiance.y, radiance.z,
        transmission.x, transmission.y, transmission.z, status, segments};
    return result;
}

// ------------------------------------------------------------------
// explicitly selected storage wrappers over the same FP32 transport
// ------------------------------------------------------------------

bool cascadeRecord(uint3 thread, out uint record)
{
    record = thread.x + thread.y * g_cascade.dispatchRowThreads;
    return record < g_cascade.probeWidth * g_cascade.probeHeight * g_cascade.directionCount;
}

void storePackedCascade(uint record, OpticalResult result)
{
    g_packed_results[record] = packInterval(result);
    // Counts describe this slot's local connectors, never ancestor traversal.
    // Only the bounded C0 tile and selected C1 diagnostic have count storage.
    if (g_cascade.level == 0)
        g_segments[record] = result.segments;
    else if (g_cascade.level == 1 && record == 0)
        g_segments[g_trace.rayCount] = result.segments;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeTop(uint3 thread : SV_DispatchThreadID)
{
    uint record;
    if (cascadeRecord(thread, record))
        g_results[record] = evaluateCascadeTop<false>(record);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeMerge(uint3 thread : SV_DispatchThreadID)
{
    uint record;
    if (cascadeRecord(thread, record))
        g_results[record] = evaluateCascadeMerge<false>(record, false);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeTopPacked(uint3 thread : SV_DispatchThreadID)
{
    uint record;
    if (cascadeRecord(thread, record))
        storePackedCascade(record, evaluateCascadeTop<false>(record));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeMergePacked(uint3 thread : SV_DispatchThreadID)
{
    uint record;
    if (cascadeRecord(thread, record))
        storePackedCascade(record, evaluateCascadeMerge<false>(record, true));
}

// ------------------------------------------------------------------
// PRECISE-linked noise-free terrain geometry qualification
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void traceTerrain(uint3 id : SV_DispatchThreadID) {
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    terrainRequireRte64();
#endif
    if (id.x >= g_trace.rayCount) return;
    g_results[id.x] = traceTerrainRay(g_rays[id.x]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeTerrainTop(uint3 id : SV_DispatchThreadID) {
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    terrainRequireRte64();
#endif
    uint record;
    if (!cascadeRecord(id, record)) return;
    g_results[record] = evaluateCascadeTop<true>(record);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cascadeTerrainMerge(uint3 id : SV_DispatchThreadID) {
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    terrainRequireRte64();
#endif
    uint record;
    if (!cascadeRecord(id, record)) return;
    g_results[record] = evaluateCascadeMerge<true>(record, false);
}

// ------------------------------------------------------------------
// owned exterior midpoint receivers over the same terrain C1 field
// ------------------------------------------------------------------

#include "terrain_receiver_types.slang"
struct TerrainReceiverParameters {
    uint count, parentWidth, parentHeight, parentDirectionOffset;
    float parentSpacing, parentNearCells;
    uint angularOutput, reserved1;
};
ConstantBuffer<TerrainReceiverParameters> g_receivers;
StructuredBuffer<TerrainReceiverFeature> g_receiver_features;
RWStructuredBuffer<TerrainReceiverResult> g_receiver_results;
RWStructuredBuffer<TerrainReceiverAngular> g_receiver_angular;

bool terrainReceiverPoint(TerrainReceiverFeature feature, out TerrainPoint point, out TerrainWitness witness,
    inout uint status) {
    point.probe = 0.0;
    point.rawDirection = 0.0;
    point.nearCells = 0.0;
    witness.base = int2(feature.baseX, feature.baseY);
    witness.curve = feature.curve;
    witness.enabled = 1;
    if (feature.baseX < 0 || feature.baseY < 0 || feature.baseX + 1 >= int(g_trace.width) ||
        feature.baseY + 1 >= int(g_trace.height)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; return false;
    }

    uint labels[4]; float roundness[4]; float3 sources[4];
    terrainCorners(witness.base, labels, roundness, sources);
    TerrainCurve curves[2]; bool complement;
    uint count = terrainCurves(labels, roundness, curves, complement);
    if (feature.curve >= count) { status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; return false; }
    TerrainCurve curve = curves[feature.curve];
    if (curve.lineAxis != 2) {
        point.probe = float2(witness.base) + 1.0;
        return true;
    }

    if (!terrainNormal(curve.roundness)) { status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return false; }
    precise float offset = curve.roundness * 0.125;
    if (!terrainNormal(offset) || (curve.roundness != 0.0 && offset == 0.0)) {
        status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic; return false;
    }
    bool reflectedX = curve.corner == 1 || curve.corner == 2;
    bool reflectedY = curve.corner == 2 || curve.corner == 3;
    point.probe = float2(witness.base) + float2(reflectedX ? 1.25 : 0.75, reflectedY ? 1.25 : 0.75);
    point.rawDirection = float2(reflectedX ? -offset : offset, reflectedY ? -offset : offset);
    point.nearCells = 1.0;
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void terrainReceivers(uint3 id : SV_DispatchThreadID) {
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    terrainRequireRte64();
#endif
    uint record = id.x;
    if (record >= g_receivers.count) return;

    uint status = 0;
    TerrainPoint point; TerrainWitness witness;
    if (!terrainReceiverPoint(g_receiver_features[record], point, witness, status)) {
        TerrainReceiverResult failed = {0.0, 0.0, 0.0, status};
        if (g_receivers.angularOutput != 0u) {
            for (uint bin = 0u; bin < 16u; ++bin) {
                TerrainReceiverAngular invalid = {0.0, 0.0, 0.0, status};
                g_receiver_angular[record * 16u + bin] = invalid;
            }
        }
        g_receiver_results[record] = failed; return;
    }

    uint2 floorParent, ceilParent;
    float2 fraction;
    for (uint axis = 0; axis < 2; ++axis) {
        float2 coordinate = terrainMul(terrainPointCoordinate(point, axis, 0.5, status), float2(0.5), status);
        if (status != 0 || coordinate.x < 0.0 || coordinate.y >= float(axis == 0 ? g_receivers.parentWidth : g_receivers.parentHeight)) {
            status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; break;
        }

        uint low = uint(floor(coordinate.x)), high = uint(floor(coordinate.y));
        if (low != high) { status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; break; }
        floorParent[axis] = low;
        bool integerPoint = coordinate.x == coordinate.y && coordinate.x == float(low);
        ceilParent[axis] = low + (integerPoint ? 0u : 1u);
        if (ceilParent[axis] >= (axis == 0 ? g_receivers.parentWidth : g_receivers.parentHeight)) {
            status |= kTerrainNumericAmbiguity | kTerrainReasonOwnership; break;
        }

        // Local half-grid anchor cancellation precedes the authored offset.
        // Physical q remains symbolic; these are only its interpolation weights.
        precise float anchor = point.probe[axis] - (0.5 + 2.0 * float(low));
        precise float offset = point.nearCells * point.rawDirection[axis];
        fraction[axis] = 0.5 * anchor + 0.5 * offset;
        if (!terrainNormal(fraction[axis]) || fraction[axis] < 0.125 || fraction[axis] > 0.875)
            status |= kTerrainNumericAmbiguity | kTerrainReasonArithmetic;
    }

    float3 mean = 0.0;
    float3 angular[16];
    uint angularStatus = 0u;
    if (g_receivers.angularOutput != 0u) {
        for (uint bin = 0u; bin < 16u; ++bin) angular[bin] = 0.0;
    }

    if (status == 0) {
        // Sixteen C1 angular children are the two equal-width children of each
        // C0 bin. Each connector is composed before its single spatial weight.
        for (uint bin = 0; bin < 16; ++bin) {
            Direction direction = g_directions[g_receivers.parentDirectionOffset + bin];
            for (uint y = 0; y < 2; ++y) {
                for (uint x = 0; x < 2; ++x) {
                    float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
                    if (weight == 0.0) continue;
                    uint2 coordinate = uint2(x == 0 ? floorParent.x : ceilParent.x, y == 0 ? floorParent.y : ceilParent.y);
                    uint upperRecord = (coordinate.y * g_receivers.parentWidth + coordinate.x) * 16u + bin;
                    OpticalResult upper = g_upper[upperRecord];
                    TerrainPoint target = {0.5 + float2(coordinate) * g_receivers.parentSpacing,
                        float2(direction.x, direction.y), g_receivers.parentNearCells};
                    TerrainLine line = {point, target, 0.0, 1};
                    OpticalResult connector = traceTerrainParametricWitness(line, float2(0.0), float2(1.0), 0, witness);
                    status |= connector.status | upper.status;
                    float3 nearL = float3(connector.radianceR, connector.radianceG, connector.radianceB);
                    float3 nearT = float3(connector.transmissionR, connector.transmissionG, connector.transmissionB);
                    float3 farL = float3(upper.radianceR, upper.radianceG, upper.radianceB);
                    float3 farTerm = cellPositiveProducts(nearT, farL, status);
                    float measure = cellPositiveProduct(1.0 / 16.0, weight, status);
                    mean += cellPositiveProducts(float3(measure), nearL + farTerm, status);

                    if (g_receivers.angularOutput != 0u)
                        angular[bin] += cellPositiveProducts(float3(weight), nearL + farTerm, angularStatus);
                }
            }
        }
    }

    if (!all(isfinite(mean)) || any(mean < 0.0)) status |= 1 | kTerrainReasonArithmetic;
    TerrainReceiverResult result = {mean.x, mean.y, mean.z, status};
    g_receiver_results[record] = result;

    if (g_receivers.angularOutput != 0u) {
        for (uint bin = 0u; bin < 16u; ++bin) {
            if (!all(isfinite(angular[bin])) || any(angular[bin] < 0.0)) angularStatus |= 1u | kTerrainReasonArithmetic;
        }

        for (uint bin = 0u; bin < 16u; ++bin) {
            TerrainReceiverAngular value = {angular[bin].x, angular[bin].y, angular[bin].z, status | angularStatus};
            g_receiver_angular[record * 16u + bin] = value;
        }
    }
}

// ------------------------------------------------------------------
// optional world-fixed C0 incident mean for opaque cavern backwalls
// ------------------------------------------------------------------

RWStructuredBuffer<TerrainReceiverResult> g_background_means;

[shader("compute")]
[numthreads(64, 1, 1)]
void terrainBackgroundMeans(uint3 id : SV_DispatchThreadID) {
    uint probe = id.x;
    if (probe >= g_trace.width * g_trace.height) return;

    float3 mean = 0.0;
    uint status = 0u;
    for (uint bin = 0u; bin < 8u; ++bin) {
        OpticalResult interval = g_upper[probe * 8u + bin];
        status |= interval.status;
        float3 value = float3(interval.radianceR, interval.radianceG, interval.radianceB);
        if (!all(isfinite(value)) || any(value < 0.0)) status |= 1u;
        mean += cellPositiveProducts(float3(0.125), value, status);
    }

    if (!all(isfinite(mean)) || any(mean < 0.0)) status |= 1u;
    TerrainReceiverResult result = {mean.x, mean.y, mean.z, status};
    g_background_means[probe] = result;
}

// ------------------------------------------------------------------
// exact-view backwall incident lighting, centre plus admitted MSAA4 samples
// ------------------------------------------------------------------

#include "grid_background_world.slang"
struct TerrainBackgroundViewParameters {
    float4x4 inverseViewProjection;
    uint width, height, sourceWidth, sourceHeight;
    float minX, minY, cellMetres;
    uint reserved;
};
ConstantBuffer<TerrainBackgroundViewParameters> g_background_view;
StructuredBuffer<TerrainReceiverResult> g_background_source;
RWStructuredBuffer<float4> g_background_view_results;
// attempted, qualified, unresolved, arithmetic-bound errors, outside-plane,
// sampled positions, invalid values, reserved. No per-pixel CPU readback.
RWStructuredBuffer<uint> g_background_view_counts;

void backgroundCount(uint index) {
    uint previous;
    InterlockedAdd(g_background_view_counts[index], 1u, previous);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void terrainBackgroundView(uint3 id : SV_DispatchThreadID) {
#if defined(PPR_TERRAIN_FP64_REFINEMENT)
    terrainRequireRte64();
#endif
    if (id.x >= g_background_view.width || id.y >= g_background_view.height) return;

    // Vulkan's admitted standard indexed4 pattern, shared by the existing
    // spatial capability recipe. Slot0 is cached1x centre, slots1..4 match SV_SampleIndex.
    static const float2 positions[5] = {float2(0.5, 0.5), float2(0.375, 0.125),
        float2(0.875, 0.375), float2(0.125, 0.625), float2(0.625, 0.875)};
    for (uint sample = 0u; sample < 5u; ++sample) {
        uint record = (id.y * g_background_view.width + id.x) * 5u + sample;
        float2 uv = (float2(id.xy) + positions[sample]) / float2(g_background_view.width, g_background_view.height);
        backgroundCount(5u);
        float2 world;
        if (!backgroundReceivingPointAt(uv, g_background_view.inverseViewProjection, world)) {
            backgroundCount(6u);
            g_background_view_results[record] = 0.0;
            continue;
        }

        float2 q = (world - float2(g_background_view.minX, g_background_view.minY)) / g_background_view.cellMetres;
        uint status = 0u;
        float3 mean = 0.0;
        if (!all(isfinite(q)) || !terrainNormal(q.x) || !terrainNormal(q.y)) {
            backgroundCount(6u);
            g_background_view_results[record] = 0.0;
            continue;
        }

        // The finite receiving plane is EXACTLY the C0 convex hull. Beyond it
        // there is no receiving wall: this is declared far void, not a query fallback.
        float2 high = float2(g_background_view.sourceWidth, g_background_view.sourceHeight) - 0.5;
        if (any(q < 0.5) || any(q > high)) {
            backgroundCount(4u);
            g_background_view_results[record] = float4(0.0, 0.0, 0.0, 1.0);
            continue;
        }

        float2 lattice = q - 0.5;
        uint2 floorProbe = uint2(floor(lattice));
        uint2 ceilProbe = uint2(ceil(lattice));
        float2 fraction = lattice - float2(floorProbe);
        // At the high edge only, ownership uses the adjacent in-domain closed
        // dual cell. Parent support/weights remain true floor/ceil, never clamped.
        int2 base = int2(min(floorProbe, uint2(g_background_view.sourceWidth, g_background_view.sourceHeight) - 2u));
        TerrainPoint origin = {q, 0.0, 0.0};
        for (uint y = 0u; y < 2u; ++y) {
            for (uint x = 0u; x < 2u; ++x) {
                float weight = (x == 0u ? 1.0 - fraction.x : fraction.x) * (y == 0u ? 1.0 - fraction.y : fraction.y);
                if (weight == 0.0) continue;

                uint2 probe = uint2(x == 0u ? floorProbe.x : ceilProbe.x, y == 0u ? floorProbe.y : ceilProbe.y);
                TerrainPoint target = {0.5 + float2(probe), 0.0, 0.0};
                TerrainLine line = {origin, target, 0.0, 1u};
                OpticalResult connector = traceTerrainBackgroundConnector(line, base);
                TerrainReceiverResult far = g_background_source[probe.y * g_background_view.sourceWidth + probe.x];
                status |= connector.status | far.status;
                backgroundCount(0u);
                if (connector.status == 0u && far.status == 0u) backgroundCount(1u);
                else {
                    backgroundCount(2u);
                    if (((connector.status | far.status) & kTerrainReasonArithmetic) != 0u) backgroundCount(3u);
                }

                float3 nearL = float3(connector.radianceR, connector.radianceG, connector.radianceB);
                float3 nearT = float3(connector.transmissionR, connector.transmissionG, connector.transmissionB);
                float3 farL = float3(far.meanR, far.meanG, far.meanB);
                float3 pair = nearL + cellPositiveProducts(nearT, farL, status);
                mean += cellPositiveProducts(float3(weight), pair, status);
            }
        }

        if (status != 0u || !all(isfinite(mean)) || any(mean < 0.0)) backgroundCount(6u);
        g_background_view_results[record] = float4(mean, 1.0);
    }
}
