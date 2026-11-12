// ------------------------------------------------------------------
// shared committed-row coordinates and half-open native ownership
// ------------------------------------------------------------------
#ifndef PPR_GRID_PARTICLE_NATIVE_SLANG
#define PPR_GRID_PARTICLE_NATIVE_SLANG

// Mutable 64-byte solver row, field-for-field with the CPU twin
// `matter::ParticleWrite` (lib/engine/matter/Matter.Gpu.Types.cppm): position,
// per-step displacement, the APIC F and D matrices (four floats each there),
// the liquid scalars in solver write order, and `enabled` last. layout=4 and
// stride 64; no field may be reordered without changing both sides.
struct NumericalRow {
    float positionX;
    float positionY;
    float displacementX;
    float displacementY;
    float4 deformationGradient;
    float4 deformationDisplacement;
    float liquidDensity;
    float logJp;
    float lambda;
    float enabled;
};
struct MetadataRow {
    uint idLow,idHigh,sourceRow,materialTemperature;
    float gasX,gasY,radius;
    uint physicalFillBits;
};
struct ParticleNativeCell { uint x,y,status; };

// The caller proves the sourceRow before loading Hot. Gas never uses a Hot row.
// This is shared by census and immutable optical snapshot, before any culling.
ParticleNativeCell particleNativeCell(MetadataRow metadata,float2 movingPosition,float enabled,bool sourceValid,
    float cellMetres,float origin,float2 minimum,uint2 extent) {
    ParticleNativeCell result={0xffffffffu,0xffffffffu,0u};
    float2 native;
    if (metadata.sourceRow!=0xffffffffu) {
        if (!sourceValid) {result.status=1u;return result;}

        native=movingPosition-origin-minimum;
        if (!isfinite(enabled)) result.status|=1u;
        else if (enabled<=0) result.status|=4u;
    } else {
        native=float2(metadata.gasX,metadata.gasY)/cellMetres-minimum;
    }

    if (!all(isfinite(native))) result.status|=1u;
    else if (any(native<0) || any(native>=float2(extent))) result.status|=2u;
    else {
        uint2 cell=uint2(floor(native));
        result.x=cell.x;result.y=cell.y;
    }
    return result;
}
#endif
