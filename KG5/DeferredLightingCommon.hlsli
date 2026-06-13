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

static const float PI = 3.14159265359f;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float NdotH = max(dot(N, H), 0.0f);
    const float NdotH2 = NdotH * NdotH;
    const float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    return a2 / max(PI * denom * denom, 1e-6f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    // Direct-lighting Smith term from LearnOpenGL: k = (r + 1)^2 / 8.
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    // IBL uses the less aggressive k = roughness^2 / 2 remapping.
    const float k = (roughness * roughness) / 2.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness, bool directLighting)
{
    const float NdotV = max(dot(N, V), 0.0f);
    const float NdotL = max(dot(N, L), 0.0f);
    const float ggxV = directLighting ? GeometrySchlickGGX(NdotV, roughness) : GeometrySchlickGGX_IBL(NdotV, roughness);
    const float ggxL = directLighting ? GeometrySchlickGGX(NdotL, roughness) : GeometrySchlickGGX_IBL(NdotL, roughness);
    return ggxV * ggxL;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 ComputePBRDirectLight(float3 N, float3 V, float3 L, float3 albedo, float metallic, float roughness, float3 radiance)
{
    N = normalize(N);
    V = normalize(V);
    L = normalize(L);
    roughness = clamp(roughness, 0.04f, 1.0f);
    metallic = saturate(metallic);

    const float3 H = normalize(V + L);
    const float NdotV = max(dot(N, V), 0.0f);
    const float NdotL = max(dot(N, L), 0.0f);
    if (NdotL <= 0.0f || NdotV <= 0.0f)
        return 0.0f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float NDF = DistributionGGX(N, H, roughness);
    const float G = GeometrySmith(N, V, L, roughness, true);
    const float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

    const float3 specular = (NDF * G * F) / max(4.0f * NdotV * NdotL, 0.001f);
    const float3 kS = F;
    const float3 kD = (1.0f - kS) * (1.0f - metallic);

    return (kD * albedo / PI + specular) * radiance * NdotL;
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
