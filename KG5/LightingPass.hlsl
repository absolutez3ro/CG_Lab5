#include "LightingContract.hlsli"
#include "DeferredLightingCommon.hlsli"

// GBuffer textures from geometry pass.
Texture2D gAlbedoTex   : register(t0);
Texture2D gNormalTex   : register(t1);
Texture2D gMaterialTex : register(t2);
Texture2D gDepthTex    : register(t3);
StructuredBuffer<PointLightData> gPointLights : register(t4);
SamplerState gSampler  : register(s0);

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
    float3 SpecColor;
    float SpecPower;
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
    s.SpecColor = material.rgb;
    s.SpecPower = saturate(material.a) * 255.0f;

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

float3 EvaluateDirectionalLight(SurfaceData s)
{
    float3 L = normalize(-gFrame.DirectionalLight.Direction);
    float3 brdf = ComputeBlinnPhong(normalize(s.Normal), normalize(s.ViewDir), L, s.Albedo, s.SpecColor, s.SpecPower);
    return brdf * gFrame.DirectionalLight.Color * gFrame.DirectionalLight.Intensity;
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
        float3 brdf = ComputeBlinnPhong(normalize(s.Normal), normalize(s.ViewDir), L, s.Albedo, s.SpecColor, s.SpecPower);
        sum += brdf * light.Color * (light.Intensity * attenuation);
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

        float3 brdf = ComputeBlinnPhong(normalize(s.Normal), normalize(s.ViewDir), L, s.Albedo, s.SpecColor, s.SpecPower);
        sum += brdf * gLocalLights.SpotLights[i].Color * (gLocalLights.SpotLights[i].Intensity * attenuation * cone);
    }
    return sum;
}

float VisualizeDepth(float depth)
{
    if (!HasValidSurface(depth))
        return 0.0f;

    // Readable depth visualization for non-linear depth buffer.
    return pow(saturate(1.0f - depth), 0.35f);
}

float4 PSDirectional(VSFullscreenOutput pin) : SV_Target
{
    const float2 uv = GetScreenUV(pin.PositionH);
    SurfaceData s = LoadSurface(uv);

    if (gFrame.DebugMode == 1) return float4(s.Albedo, 1.0f);
    if (gFrame.DebugMode == 2) return float4(s.Normal * 0.5f + 0.5f, 1.0f);
    if (gFrame.DebugMode == 3) return gMaterialTex.Sample(gSampler, uv);
    if (gFrame.DebugMode == 4) return float4(VisualizeDepth(s.Depth).xxx, 1.0f);

    if (!s.HasSurface)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (gFrame.DebugMode == 6 || gFrame.DebugMode == 7 || gFrame.DebugMode == 8)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 directional = EvaluateDirectionalLight(s);
    float3 base = (gFrame.DebugMode == 5) ? directional : (s.Albedo * gFrame.AmbientColor.rgb + directional);
    return float4(base, 1.0f);
}

float4 PSLocalLights(VSFullscreenOutput pin) : SV_Target
{
    const float2 uv = GetScreenUV(pin.PositionH);
    SurfaceData s = LoadSurface(uv);

    if (!s.HasSurface)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 pointContribution = EvaluatePointLights(s);
    float3 spotContribution = EvaluateSpotLights(s);

    if (gFrame.DebugMode == 6 || gFrame.DebugMode == 8)
        return float4(pointContribution, 1.0f);

    if (gFrame.DebugMode == 7)
        return float4(spotContribution, 1.0f);

    if (gFrame.DebugMode == 5 || gFrame.DebugMode == 0)
        return float4(pointContribution + spotContribution, 1.0f);

    return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
