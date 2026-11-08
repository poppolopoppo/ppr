#ifndef PPR_GRID_WORLD_RADIANCE_SLANG
#define PPR_GRID_WORLD_RADIANCE_SLANG
#include "world_radiance_types.slang"

// ------------------------------------------------------------------
// immutable native reference field, camera-independent Grid binding
// ------------------------------------------------------------------

struct GridWorldRadianceParameters {
    float originX, originY; uint width, height;
    int minX, minY; uint countX, countY;
    uint spacing, enabled, response, materialControl;
};
ConstantBuffer<GridWorldRadianceParameters> g_grid_world_radiance;
StructuredBuffer<WorldCoefficients> g_grid_world_coefficients;

bool gridWorldInside(float2 point) {
    float2 local=point-float2(g_grid_world_radiance.originX,g_grid_world_radiance.originY);
    return all(isfinite(local)) && all(local>=0) &&
        all(local<=float2(g_grid_world_radiance.width,g_grid_world_radiance.height));
}

WorldCoefficients gridWorldProbe(int2 cell) {
    // Host coverage admission protects normal draws. A missing in-world row
    // remains visibly invalid rather than indexing outside the private window.
    int2 index=cell-int2(g_grid_world_radiance.minX,g_grid_world_radiance.minY);
    if (any(index<0) || any(index>=int2(g_grid_world_radiance.countX,g_grid_world_radiance.countY))) {
        WorldCoefficients invalid={};invalid.status=WORLD_INVALID;return invalid;
    }
    return g_grid_world_coefficients[uint(index.y)*g_grid_world_radiance.countX+uint(index.x)];
}

float2 gridWorldCoordinate(float2 local) {
    float spacing=float(g_grid_world_radiance.spacing);
    if (g_grid_world_radiance.spacing==1u)
        return clamp(local-0.5,float2(0),float2(g_grid_world_radiance.width-1u,g_grid_world_radiance.height-1u));

    // Temporal fields retain padded full-world support at their actual spacing.
    return local/spacing-0.5;
}

// Blend signed coefficients before either material convolution. This is used
// by homogeneous terrain and moving matter at their actual world position.
WorldCoefficients gridWorldSample(float2 point) {
    WorldCoefficients result={};
    if (!all(isfinite(point))) { result.status=WORLD_INVALID;return result; }
    if (!gridWorldInside(point)) return result;

    float2 local=point-float2(g_grid_world_radiance.originX,g_grid_world_radiance.originY);
    float2 coordinate=gridWorldCoordinate(local);
    int2 lo=int2(floor(coordinate)),hi=int2(ceil(coordinate));float2 f=coordinate-float2(lo);
    for (uint sample=0u;sample<4u;++sample) {
        float weight=((sample&1u)!=0u?f.x:1-f.x)*((sample&2u)!=0u?f.y:1-f.y);
        if (weight==0) continue;

        int2 cell=int2((sample&1u)!=0u?hi.x:lo.x,(sample&2u)!=0u?hi.y:lo.y);
        WorldCoefficients value=gridWorldProbe(cell);result.status|=value.status;
        for (uint index=0u;index<9u;++index) {
            result.coefficient[index].r+=value.coefficient[index].r*weight;
            result.coefficient[index].g+=value.coefficient[index].g*weight;
            result.coefficient[index].b+=value.coefficient[index].b*weight;
        }
    }
    return result;
}

// Directional far-field input for the default-off screen near-segment pilot.
// Preserve signed harmonics and interpolation; only final diffuse output clamps.
float3 gridWorldDirectionalAt(float2 point,float angle,out uint status) {
    status=0u;
    if (!all(isfinite(point))) { status=WORLD_INVALID; return float3(0); }
    if (!gridWorldInside(point)) return float3(0); // Declared crop exterior is far void.

    float2 local=point-float2(g_grid_world_radiance.originX,g_grid_world_radiance.originY);
    float2 coordinate=gridWorldCoordinate(local);
    int2 lo=int2(floor(coordinate)),hi=int2(ceil(coordinate));float2 f=coordinate-float2(lo);
    float3 result=0;
    for (uint sample=0u;sample<4u;++sample) {
        float weight=((sample&1u)!=0u?f.x:1-f.x)*((sample&2u)!=0u?f.y:1-f.y);
        if (weight==0) continue;

        int2 cell=int2((sample&1u)!=0u?hi.x:lo.x,(sample&2u)!=0u?hi.y:lo.y);
        WorldCoefficients row=gridWorldProbe(cell);
        status|=row.status;
        float3 radiance=worldCoefficient(row,0u);
        for (uint order=1u;order<=4u;++order) {
            radiance+=worldCoefficient(row,2u*order-1u)*cos(float(order)*angle)+
                worldCoefficient(row,2u*order)*sin(float(order)*angle);
        }

        result+=radiance*weight;
    }

    if (!worldFinite3(result)) status|=WORLD_INVALID;
    return result;
}

float3 gridWorldAt(float2 point,float3 normal) {
    if (!gridWorldInside(point)) return float3(0); // Explicit outside receiving plane = far void.

    float2 local=point-float2(g_grid_world_radiance.originX,g_grid_world_radiance.originY);
    // Only positions INSIDE the crop use its nearest centre in the outer
    // half-cell strip. No receiving wall/query is invented beyond the crop.
    float2 coordinate=gridWorldCoordinate(local);
    int2 lo=int2(floor(coordinate)),hi=int2(ceil(coordinate));float2 f=coordinate-float2(lo);
    float3 result=0;
    for (uint sample=0u;sample<4u;++sample) {
        float weight=((sample&1u)!=0u?f.x:1-f.x)*((sample&2u)!=0u?f.y:1-f.y);
        if (weight==0) continue;
        int2 cell=int2((sample&1u)!=0u?hi.x:lo.x,(sample&2u)!=0u?hi.y:lo.y);
        uint status;float3 value=worldDiffuseResponse(gridWorldProbe(cell),normal,status);
        if (status!=0u) return float3(1,0,1); // Missing/invalid rows are never an implicit black field.
        result+=value*weight;
    }
    return max(result,float3(0)); // Physical clamp AFTER complete signed convolution/interpolation.
}
#endif
