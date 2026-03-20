#ifndef DEFERRED_LIGHTING_COMMON_HLSLI
#define DEFERRED_LIGHTING_COMMON_HLSLI

// Normal is stored in [0,1], remap to [-1,1] and normalize.
float3 DecodeNormal(float3 encodedNormal)
{
    return normalize(encodedNormal * 2.0f - 1.0f);
}

// Treat depth values close to far plane as background (no scene geometry).
bool HasValidSurface(float depth)
{
    return depth < 0.999999f;
}

// Reconstruct world position from sampled depth in D3D clip-space convention.
// Matrices are uploaded transposed from C++, so we use row-vector mul in HLSL.
float3 ReconstructWorldPosition(float2 uv, float depth, float4x4 invViewProj)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    const float4 clipPos = float4(ndc, depth, 1.0f);
    const float4 worldPos = mul(clipPos, invViewProj);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

float3 ComputeBlinnPhong(float3 N, float3 V, float3 L, float3 albedo, float3 specColor, float specPower)
{
    const float3 H = normalize(L + V);
    const float NdotL = max(dot(N, L), 0.0f);
    const float specular = pow(max(dot(N, H), 0.0f), max(specPower, 1.0f));
    return albedo * NdotL + specColor * specular;
}

// Educational attenuation model:
// - smooth range fade to guarantee zero at range limit
// - mild distance term to avoid unrealistically flat intensity near the source
float ComputeRangeAttenuation(float distanceToLight, float range)
{
    const float d = distanceToLight / max(range, 1e-4f);

    const float rangeFade = saturate(1.0f - d);
    const float smoothRange = rangeFade * rangeFade * (3.0f - 2.0f * rangeFade);

    const float distanceTerm = 1.0f / (1.0f + 0.5f * d + 1.5f * d * d);
    return max(smoothRange * distanceTerm, 0.0f);
}

// Convention: spotDirection points outward from light position along cone axis.
// L points from shaded point towards the light, therefore compare axis with -L.
float ComputeSpotConeAttenuation(float3 L, float3 spotDirection, float innerCos, float outerCos)
{
    const float cosTheta = dot(-L, normalize(spotDirection));
    const float denom = max(innerCos - outerCos, 1e-5f);
    return saturate((cosTheta - outerCos) / denom);
}

#endif
