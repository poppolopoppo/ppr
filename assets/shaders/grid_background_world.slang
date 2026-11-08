// Shared actual receiving-plane mapping. Row-vector camera snapshots, +Y-up;
// display sampling does not alter physical terrain or transport coordinates.
float2 backgroundWorldAt(float2 uv, float4x4 inverseViewProjection) {
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 a = mul(float4(ndc, 0.0, 1.0), inverseViewProjection);
    float4 b = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    float3 nearPoint = a.xyz / a.w;
    float3 farPoint = b.xyz / b.w;
    float3 direction = farPoint - nearPoint;
    float t = abs(direction.z) > 1.0e-8 ? -nearPoint.z / direction.z : 0.0;
    return (nearPoint + direction * t).xy;
}

// Lit views never use the legacy near-plane fallback. Host admission restricts
// the camera to affine orthographic +Z rays; each actual sample still validates
// both homogeneous divisions and the finite forward plane intersection.
bool backgroundReceivingPointAt(float2 uv, float4x4 inverseViewProjection, out float2 world) {
    world = 0.0;
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 a = mul(float4(ndc, 0.0, 1.0), inverseViewProjection);
    float4 b = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    uint aw = asuint(a.w) & 0x7fffffffu;
    uint bw = asuint(b.w) & 0x7fffffffu;
    if (!all(isfinite(a)) || !all(isfinite(b)) || aw < 0x00800000u || bw < 0x00800000u) return false;

    float3 nearPoint = a.xyz / a.w;
    float3 farPoint = b.xyz / b.w;
    float3 direction = farPoint - nearPoint;
    uint dz = asuint(direction.z) & 0x7fffffffu;
    if (!all(isfinite(nearPoint)) || !all(isfinite(farPoint)) || !all(isfinite(direction)) ||
        direction.x != 0.0 || direction.y != 0.0 || dz < 0x00800000u || !(direction.z > 0.0)) return false;

    float t = -nearPoint.z / direction.z;
    if (!isfinite(t) || t < 0.0 || t > 1.0) return false;

    world = (nearPoint + direction * t).xy;
    return all(isfinite(world));
}
