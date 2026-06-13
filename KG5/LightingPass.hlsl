#include "LightingContract.hlsli"
#include "DeferredLightingCommon.hlsli"

Texture2D gAlbedoTex   : register(t0);
Texture2D gNormalTex   : register(t1);
Texture2D gMaterialTex : register(t2);
Texture2D gDepthTex    : register(t3);
StructuredBuffer<PointLightData> gPointLights : register(t4);
Texture2DArray<float> gShadowMap : register(t6);
TextureCube gIrradianceMap : register(t8);
Texture2D gBRDFLUT : register(t9);
TextureCube gPrefilterMap : register(t10);
SamplerState gSampler  : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gIBLSampler : register(s2);

cbuffer LightingFrameCB : register(b0)
{
    LightingFrameConstants gFrame;
};

cbuffer LocalLightsCB : register(b1)
{
    LocalLightConstants gLocalLights;
};

struct VSFullscreenOutput
{
    float4 PositionH : SV_POSITION;
};

VSFullscreenOutput VSFullscreen(uint vertexID : SV_VertexID)
{
    VSFullscreenOutput o;

    const float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    o.PositionH = float4(positions[vertexID], 0.0f, 1.0f);
    return o;
}

float2 GetScreenUV(float4 positionH)
{
    return positionH.xy * gFrame.InvScreenSize;
}

struct SurfaceData
{
    float3 Albedo;
    float3 Normal;
    float Metallic;
    float Roughness;
    float AO;
    float3 WorldPos;
    float3 ViewDir;
    float Depth;
    bool HasSurface;
};

SurfaceData LoadSurface(float2 uv)
{
    SurfaceData s;
    s.Albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    s.Normal = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);

    float4 material = gMaterialTex.Sample(gSampler, uv);
    s.Metallic = saturate(material.r);
    s.Roughness = clamp(material.g, 0.04f, 1.0f);
    s.AO = saturate(material.b);

    s.Depth = gDepthTex.Sample(gSampler, uv).r;
    s.HasSurface = HasValidSurface(s.Depth);

    if (s.HasSurface)
    {
        s.WorldPos = ReconstructWorldPosition(uv, s.Depth, gFrame.InvViewProj);
        s.ViewDir = normalize(gFrame.EyePos.xyz - s.WorldPos);
    }
    else
    {
        s.WorldPos = 0.0f;
        s.ViewDir = float3(0.0f, 0.0f, 1.0f);
    }

    return s;
}

float3 GetIBLSampleDirection(float3 dir)
{
    return dir;
}

float3 GetSkyboxRayDirection(float2 uv)
{
    const float2 ndc = uv * 2.0f - 1.0f;
    const float4 nearH = mul(float4(ndc.x, -ndc.y, 0.0f, 1.0f), gFrame.InvViewProj);
    const float4 farH = mul(float4(ndc.x, -ndc.y, 1.0f, 1.0f), gFrame.InvViewProj);
    const float3 nearWorld = nearH.xyz / max(nearH.w, 1.0e-6f);
    const float3 farWorld = farH.xyz / max(farH.w, 1.0e-6f);
    return normalize(farWorld - nearWorld);
}

uint GetCascadeIndex(float viewDepth)
{
    uint cascadeIndex = 0;
    if (viewDepth > gFrame.CascadeSplits.x) cascadeIndex = 1;
    if (viewDepth > gFrame.CascadeSplits.y) cascadeIndex = 2;
    if (viewDepth > gFrame.CascadeSplits.z) cascadeIndex = 3;
    return min(cascadeIndex, max(gFrame.CascadeCount, 1u) - 1u);
}

float GetShadowPCF(float3 worldPos, uint cascadeIndex)
{
    float4 shadowH = mul(float4(worldPos, 1.0f), gFrame.ShadowViewProj[cascadeIndex]);
    shadowH.xyz /= max(shadowH.w, 1e-6f);

    float2 uv;
    uv.x = shadowH.x * 0.5f + 0.5f;
    uv.y = -shadowH.y * 0.5f + 0.5f;
    const float depth = shadowH.z;

    if (uv.x < 0.0f || uv.x > 1.0f ||
        uv.y < 0.0f || uv.y > 1.0f ||
        depth < 0.0f || depth > 1.0f)
    {
        return 1.0f;
    }

    const float texelSize = 1.0f / max(gFrame.ShadowMapSize, 1.0f);
    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(
                gShadowSampler,
                float3(uv + offset, cascadeIndex),
                depth);
        }
    }

    return shadow / 9.0f;
}

float GetShadowFactorRaw(float3 worldPos)
{
    if (gFrame.EnableShadows == 0 || gFrame.CascadeCount == 0)
        return 1.0f;

    const float4 viewPos = mul(float4(worldPos, 1.0f), gFrame.View);
    const uint cascadeIndex = GetCascadeIndex(abs(viewPos.z));
    return GetShadowPCF(worldPos, cascadeIndex);
}

float ApplyShadowVisibility(float rawShadow)
{
    const float minLitInShadow = 0.28f;
    return lerp(minLitInShadow, 1.0f, saturate(rawShadow));
}

float GetShadowFactor(float3 worldPos)
{
    return ApplyShadowVisibility(GetShadowFactorRaw(worldPos));
}

float3 GetCascadeDebugColor(float3 worldPos)
{
    const float4 viewPos = mul(float4(worldPos, 1.0f), gFrame.View);
    const uint cascadeIndex = GetCascadeIndex(abs(viewPos.z));
    const float3 colors[4] =
    {
        float3(1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f),
        float3(0.0f, 0.35f, 1.0f),
        float3(1.0f, 1.0f, 0.0f)
    };
    return colors[cascadeIndex];
}

float3 EvaluateDirectionalLight(SurfaceData s)
{
    float3 L = normalize(-gFrame.DirectionalLight.Direction);
    float3 radiance = gFrame.DirectionalLight.Color * gFrame.DirectionalLight.Intensity;
    return ComputePBRDirectLight(s.Normal, s.ViewDir, L, s.Albedo, s.Metallic, s.Roughness, radiance, gFrame.MicrofacetDistribution);
}

float3 EvaluatePointLights(SurfaceData s)
{
    if (!s.HasSurface)
        return 0.0f;

    float3 sum = 0.0f;
    for (uint i = 0; i < min(gFrame.PointLightCount, MAX_POINT_LIGHTS); ++i)
    {
        PointLightData light = gPointLights[i];
        float3 lightVec = light.Position - s.WorldPos;
        float dist = length(lightVec);
        if (dist > light.Range || dist <= 1e-4f)
            continue;

        float3 L = lightVec / dist;
        float attenuation = max(ComputeRangeAttenuation(dist, light.Range), 0.0f);
        float3 radiance = light.Color * (light.Intensity * attenuation);
        sum += ComputePBRDirectLight(s.Normal, s.ViewDir, L, s.Albedo, s.Metallic, s.Roughness, radiance, gFrame.MicrofacetDistribution);
    }
    return sum;
}

float3 EvaluateSpotLights(SurfaceData s)
{
    if (!s.HasSurface)
        return 0.0f;

    float3 sum = 0.0f;
    for (uint i = 0; i < min(gFrame.SpotLightCount, MAX_SPOT_LIGHTS); ++i)
    {
        float3 lightVec = gLocalLights.SpotLights[i].Position - s.WorldPos;
        float dist = length(lightVec);
        if (dist > gLocalLights.SpotLights[i].Range || dist <= 1e-4f)
            continue;

        float3 L = lightVec / dist;
        float attenuation = max(ComputeRangeAttenuation(dist, gLocalLights.SpotLights[i].Range), 0.0f);
        float cone = ComputeSpotConeAttenuation(L, gLocalLights.SpotLights[i].Direction, gLocalLights.SpotLights[i].InnerCos, gLocalLights.SpotLights[i].OuterCos);
        if (cone <= 0.0f)
            continue;

        float3 radiance = gLocalLights.SpotLights[i].Color * (gLocalLights.SpotLights[i].Intensity * attenuation * cone);
        sum += ComputePBRDirectLight(s.Normal, s.ViewDir, L, s.Albedo, s.Metallic, s.Roughness, radiance, gFrame.MicrofacetDistribution);
    }
    return sum;
}

float3 EvaluateIBL(SurfaceData s)
{
    float3 N = normalize(s.Normal);
    float3 V = normalize(s.ViewDir);
    float3 R = GetIBLSampleDirection(reflect(-V, N));
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), s.Albedo, s.Metallic);

    float NdotV = max(dot(N, V), 0.0f);
    float3 F = FresnelSchlickRoughness(NdotV, F0, s.Roughness);
    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - s.Metallic);

    float3 irradiance = gIrradianceMap.Sample(gIBLSampler, N).rgb;
    float3 diffuse = irradiance * s.Albedo;

    const float MAX_REFLECTION_LOD = 11.0f;
    float3 prefilteredColor = gPrefilterMap.SampleLevel(gIBLSampler, R, s.Roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = gBRDFLUT.Sample(gIBLSampler, float2(NdotV, s.Roughness)).rg;
    float3 specular = prefilteredColor * (F * brdf.x + brdf.y);

    float3 diffuseIBL = kD * diffuse * gFrame.IBLDiffuseStrength;
    float3 specularIBL = specular * gFrame.IBLSpecularStrength;
    return (diffuseIBL + specularIBL) * s.AO;
}

float VisualizeDepth(float depth)
{
    if (!HasValidSurface(depth))
        return 0.0f;

    return pow(saturate(1.0f - depth), 0.35f);
}

float4 PSDirectional(VSFullscreenOutput pin) : SV_Target
{
    const float2 uv = GetScreenUV(pin.PositionH);
    SurfaceData s = LoadSurface(uv);

    if (gFrame.ForceMirrorMaterial != 0)
    {
        s.Albedo = float3(0.95f, 0.95f, 0.95f);
        s.Metallic = 1.0f;
        s.Roughness = 0.02f;
        s.AO = 1.0f;
    }

    if (gFrame.DebugMode == 1) return float4(s.Albedo, 1.0f);
    if (gFrame.DebugMode == 2) return float4(s.Normal * 0.5f + 0.5f, 1.0f);
    if (gFrame.DebugMode == 3) return gMaterialTex.Sample(gSampler, uv);
    if (gFrame.DebugMode == 4) return float4(VisualizeDepth(s.Depth).xxx, 1.0f);

    if (!s.HasSurface)
    {
        if (gFrame.ShowIBLSkybox != 0)
        {
            const float3 rayDir = GetIBLSampleDirection(GetSkyboxRayDirection(uv));
            return float4(gPrefilterMap.SampleLevel(gIBLSampler, rayDir, 0.0f).rgb, 1.0f);
        }
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    if (gFrame.DebugMode == 6 || gFrame.DebugMode == 7)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (gFrame.DebugMode == 8)
    {
        const float shadowFactor = GetShadowFactorRaw(s.WorldPos);
        return float4(shadowFactor.xxx, 1.0f);
    }
    if (gFrame.DebugMode == 9)
    {
        return float4(GetCascadeDebugColor(s.WorldPos), 1.0f);
    }
    if (gFrame.DebugMode == 10)
    {
        const float3 N = normalize(s.Normal);
        const float3 irradiance = gIrradianceMap.Sample(gIBLSampler, N).rgb;
        const float3 diffuse = irradiance * s.Albedo;
        return float4(diffuse * s.AO, 1.0f);
    }
    if (gFrame.DebugMode == 11)
    {
        const float3 N = normalize(s.Normal);
        const float3 V = normalize(s.ViewDir);
        const float3 R = GetIBLSampleDirection(reflect(-V, N));
        const float NdotV = max(dot(N, V), 0.0f);
        const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), s.Albedo, s.Metallic);
        const float MAX_REFLECTION_LOD = 11.0f;
        const float3 prefilteredColor = gPrefilterMap.SampleLevel(gIBLSampler, R, s.Roughness * MAX_REFLECTION_LOD).rgb;
        const float2 brdf = gBRDFLUT.Sample(gIBLSampler, float2(NdotV, s.Roughness)).rg;
        const float3 F = FresnelSchlickRoughness(NdotV, F0, s.Roughness);
        const float3 specular = prefilteredColor * (F * brdf.x + brdf.y);
        return float4(specular, 1.0f);
    }
    if (gFrame.DebugMode == 12)
    {
        const float3 N = normalize(s.Normal);
        const float3 V = normalize(s.ViewDir);
        const float3 R = GetIBLSampleDirection(reflect(-V, N));
        const float MAX_REFLECTION_LOD = 11.0f;
        const float lod = s.Roughness * MAX_REFLECTION_LOD;
        const float3 prefilteredColor = gPrefilterMap.SampleLevel(gIBLSampler, R, lod).rgb;
        return float4(prefilteredColor, 1.0f);
    }
    if (gFrame.DebugMode == 13)
    {
        return float4(s.Metallic, s.Roughness, s.AO, 1.0f);
    }
    if (gFrame.DebugMode == 14)
    {
        const float3 L = normalize(-gFrame.DirectionalLight.Direction);
        const float3 radiance = gFrame.DirectionalLight.Color * gFrame.DirectionalLight.Intensity;
        const float3 ggx = ComputePBRDirectLight(s.Normal, s.ViewDir, L, s.Albedo, s.Metallic, s.Roughness, radiance, 0u);
        const float3 beckmann = ComputePBRDirectLight(s.Normal, s.ViewDir, L, s.Albedo, s.Metallic, s.Roughness, radiance, 1u);
        return float4(abs(ggx - beckmann) * 8.0f, 1.0f);
    }

    const float shadowFactor = GetShadowFactor(s.WorldPos);
    const float3 directDirectional = EvaluateDirectionalLight(s);
    const float3 directional = directDirectional * shadowFactor;
    const float3 finalLighting = (gFrame.DebugMode == 5) ? directional : (EvaluateIBL(s) + directional);
    return float4(finalLighting, 1.0f);
}

float4 PSLocalLights(VSFullscreenOutput pin) : SV_Target
{
    const float2 uv = GetScreenUV(pin.PositionH);
    SurfaceData s = LoadSurface(uv);

    if (gFrame.ForceMirrorMaterial != 0)
    {
        s.Albedo = float3(0.95f, 0.95f, 0.95f);
        s.Metallic = 1.0f;
        s.Roughness = 0.02f;
        s.AO = 1.0f;
    }

    if (!s.HasSurface)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 pointContribution = EvaluatePointLights(s);
    float3 spotContribution = EvaluateSpotLights(s);

    if (gFrame.DebugMode == 6)
        return float4(pointContribution, 1.0f);

    if (gFrame.DebugMode == 7)
        return float4(spotContribution, 1.0f);

    if (gFrame.DebugMode == 5 || gFrame.DebugMode == 0)
        return float4(pointContribution + spotContribution, 1.0f);

    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
