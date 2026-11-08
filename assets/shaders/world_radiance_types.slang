#ifndef PPR_WORLD_RADIANCE_TYPES_SLANG
#define PPR_WORLD_RADIANCE_TYPES_SLANG
#include "terrain_receiver_types.slang" // Existing validated normalized K16 lift.

// ------------------------------------------------------------------
// signed112-byte world field and pure normalized diffuse consumer
// ------------------------------------------------------------------

struct WorldRgb { float r, g, b; };
struct WorldCoefficients { WorldRgb coefficient[9]; uint status; };
static const uint WORLD_INVALID = 1u;
static const float WORLD_PI = 3.14159265358979323846;
bool worldFinite3(float3 value) { return all(isfinite(value)); }

float3 worldCoefficient(WorldCoefficients value, uint index) {
    return float3(value.coefficient[index].r,value.coefficient[index].g,value.coefficient[index].b);
}

// Unit front-facing normals are the same decoded normal-map contract as Grid.
// Front normal is bit-identical DC; tilted response retains the validated K16
// elevation lift and returns E/pi directly. Self-emission is a separate caller term.
float3 worldDiffuseResponse(WorldCoefficients value, float3 normal, out uint status) {
    status=value.status;
    float square=dot(normal,normal);
    if (!worldFinite3(normal) || !(square>0) || !isfinite(square) || normal.z<0) {
        status|=WORLD_INVALID;
        return float3(0);
    }
    for (uint coefficient=0u;coefficient<9u;++coefficient) {
        if (!worldFinite3(worldCoefficient(value,coefficient))) status|=WORLD_INVALID;
    }
    if (any(worldCoefficient(value,0u)<0)) status|=WORLD_INVALID;
    if (normal.x==0 && normal.y==0 && normal.z>0) return worldCoefficient(value,0u);
    normal/=sqrt(square);

    float3 result=0;
    for (uint index=0u;index<16u;++index) {
        float angle=(float(index)+0.5)*(2*WORLD_PI/16);
        float2 direction=float2(cos(angle),sin(angle));
        float3 radiance=worldCoefficient(value,0u);
        for (uint order=1u;order<=4u;++order) {
            radiance+=worldCoefficient(value,2u*order-1u)*cos(float(order)*angle)+
                      worldCoefficient(value,2u*order)*sin(float(order)*angle);
        }
        result+=radiance*(terrainReceiverDiffuseLift(normal,direction)/16);
    }
    // Finite truncation undershoot remains RAW diagnostic data, not a failed
    // field. Physical shading clamps only this final convolution, never AC,
    // directional samples or DC. Self-emission is a separate caller term.
    if (!worldFinite3(result)) status|=WORLD_INVALID;
    return result;
}


// ------------------------------------------------------------------
// bounded native opaque material response, not a full3D GGX integral
// ------------------------------------------------------------------

bool worldMaterialNormal(WorldCoefficients value,float3 normal,out float3 unitNormal) {
    unitNormal=float3(0);
    float square=dot(normal,normal);
    if (value.status!=0u || !worldFinite3(normal) || !isfinite(square) || !(square>0) || normal.z<0) return false;

    for (uint index=0u;index<9u;++index) {
        if (!worldFinite3(worldCoefficient(value,index))) return false;
    }

    if (any(worldCoefficient(value,0u)<0)) return false;

    unitNormal=normal/sqrt(square);
    return true;
}

// N is unit length, V=(0,0,1). Complex powers avoid an undefined pole
// azimuth and preserve signed CH. Only the completed convolution is clamped.
float3 worldPoissonReflection(WorldCoefficients value,float3 normal,float roughness) {
    float3 reflection=2*normal.z*normal-float3(0,0,1);
    float2 z=(1-roughness*roughness)*reflection.xy;
    float2 power=float2(1,0);
    float3 result=worldCoefficient(value,0u);

    for (uint order=1u;order<=4u;++order) {
        power=float2(power.x*z.x-power.y*z.y,power.x*z.y+power.y*z.x);
        result+=worldCoefficient(value,2u*order-1u)*power.x+worldCoefficient(value,2u*order)*power.y;
    }

    return max(result,float3(0))*saturate(reflection.z);
}

float3 worldSpecularResponse(WorldCoefficients value,float3 normal,float roughness,out uint status) {
    status=value.status;
    float3 unitNormal;
    if (!worldMaterialNormal(value,normal,unitNormal) || !isfinite(roughness) || roughness<0 || roughness>1) {
        status|=WORLD_INVALID;
        return float3(0);
    }

    float3 result=worldPoissonReflection(value,unitNormal,roughness);
    if (!worldFinite3(result)) status|=WORLD_INVALID;
    return result;
}

float3 worldMaterialResponse(WorldCoefficients value,float3 normal,float3 albedo,
    float metallic,float roughness,float ior,out uint status) {
    status=value.status;
    float3 unitNormal;
    if (!worldMaterialNormal(value,normal,unitNormal) || !worldFinite3(albedo) || any(albedo<0) || any(albedo>1) ||
        !isfinite(metallic) || metallic<0 || metallic>1 || !isfinite(roughness) || roughness<0 || roughness>1 ||
        !isfinite(ior) || ior<1 || ior>3) {
        status|=WORLD_INVALID;
        return float3(0);
    }

    float ratio=(ior-1)/(ior+1);
    float3 f0=lerp(float3(ratio*ratio),albedo,metallic);
    float grazing=1-saturate(unitNormal.z);
    float grazing2=grazing*grazing;
    float3 fresnel=f0+(1-f0)*(grazing2*grazing2*grazing);

    float3 specular=worldPoissonReflection(value,unitNormal,roughness);

    float3 diffuse=0;
    if (metallic<1) diffuse=max(worldDiffuseResponse(value,unitNormal,status),float3(0));

    // P<=H and D<=3H. With bounded A,m,F this mixture is <=3H;
    // adding S+0.04 retains the existing8H+2S<=65504 half admission.
    float3 result=(1-metallic)*(1-fresnel)*albedo*diffuse+fresnel*specular;
    if (!worldFinite3(result)) status|=WORLD_INVALID;
    return result;
}

// The mapped-normal local key is a render-only term. It never enters CH,
// tracing, feedback, background or gameplay; callers add this source once.
float3 worldCutFaceResponse(float3 albedo,float3 mappedNormal,float keyIncident,float3 source) {
    return albedo*keyIncident*max(dot(mappedNormal,normalize(float3(-0.4,0.5,1))),0)+source;
}

#endif
