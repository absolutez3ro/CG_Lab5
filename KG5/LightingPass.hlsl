Texture2D gAlbedoTex   : register(t0);
Texture2D gNormalTex   : register(t1);
Texture2D gPositionTex : register(t2);
Texture2D gMaterialTex : register(t3);
SamplerState gSampler  : register(s0);

cbuffer LightingFrameCB : register(b0)
{
    float4 gEyePos;
    float2 gScreenSize;
    float2 gInvScreenSize;
    float4 gAmbientColor;
    float4 gDirLightDirection;
    float4 gDirLightColorIntensity;
};

cbuffer LightVolumeCB : register(b1)
{
    float4x4 gWorldViewProj;
    float4 gPositionRange;   // xyz, range
    float4 gDirectionCos;    // xyz, cosAngle
    float4 gColorIntensity;  // rgb, intensity
};

struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

VSOutput VSVolume(VSInput vin)
{
    VSOutput vout;
    vout.PositionH = mul(float4(vin.Position, 1.0f), gWorldViewProj);
    return vout;
}

float2 GetUV(float4 positionH)
{
    return positionH.xy * gInvScreenSize;
}

float3 DecodeNormal(float3 n)
{
    return normalize(n * 2.0f - 1.0f);
}

float3 CalcLighting(float3 P, float3 N, float3 V, float3 albedo, float3 L, float3 lightCol, float atten)
{
    float3 H = normalize(L + V);
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);
    return (albedo * diff + spec.xxx) * lightCol * atten;
}

float4 PSDirectional(VSOutput pin) : SV_Target
{
    float2 uv = GetUV(pin.PositionH);

    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float3 normal = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);

    float3 L = normalize(-gDirLightDirection.xyz);
    float diff = max(dot(normal, L), 0.0);

    float3 color = albedo * gAmbientColor.rgb +
                   albedo * gDirLightColorIntensity.rgb * gDirLightColorIntensity.a * diff;

    return float4(color, 1.0f);
}

float4 PSPoint(VSOutput pin) : SV_Target
{
    float2 uv = GetUV(pin.PositionH);

    float3 P = gPositionTex.Sample(gSampler, uv).xyz;
    float3 N = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);
    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float3 V = normalize(gEyePos.xyz - P);

    float3 lightVec = gPositionRange.xyz - P;
    float dist = length(lightVec);
    float range = gPositionRange.w;

    if (dist > range || dist <= 0.0001f)
        discard;

    float atten = saturate(1.0f - dist / range);
    float3 L = lightVec / dist;

    float3 result = CalcLighting(
        P, N, V, albedo, L,
        gColorIntensity.rgb,
        gColorIntensity.a * atten * atten);

    return float4(result, 1.0f);
}

float4 PSSpot(VSOutput pin) : SV_Target
{
    float2 uv = GetUV(pin.PositionH);

    float3 P = gPositionTex.Sample(gSampler, uv).xyz;
    float3 N = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);
    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float3 V = normalize(gEyePos.xyz - P);

    float3 lightVec = gPositionRange.xyz - P;
    float dist = length(lightVec);
    float range = gPositionRange.w;

    if (dist > range || dist <= 0.0001f)
        discard;

    float3 L = lightVec / dist;

    float theta = dot(L, normalize(-gDirectionCos.xyz));
    if (theta < gDirectionCos.w)
        discard;

    float cone = smoothstep(gDirectionCos.w, min(gDirectionCos.w + 0.08f, 1.0f), theta);
    float atten = saturate(1.0f - dist / range);

    float3 result = CalcLighting(
        P, N, V, albedo, L,
        gColorIntensity.rgb,
        gColorIntensity.a * atten * cone);

    return float4(result, 1.0f);
}