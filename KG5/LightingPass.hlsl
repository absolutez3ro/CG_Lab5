// Albedo from GBuffer (written in geometry pass)
Texture2D gAlbedoTex   : register(t0);
Texture2D gNormalTex   : register(t1);
Texture2D gMaterialTex : register(t2);
Texture2D gDepthTex    : register(t3);
SamplerState gSampler  : register(s0);

cbuffer LightingFrameCB : register(b0)
{
    float4 gEyePos;
    float2 gScreenSize;
    float2 gInvScreenSize;
    float4 gAmbientColor;
    float4 gDirLightDirection;
    float4 gDirLightColorIntensity;
    float4x4 gInvViewProj;
};

cbuffer LightVolumeCB : register(b1)
{
    float4x4 gWorldViewProj;
    float4 gPositionRange;
    float4 gDirectionCos;
    float4 gColorIntensity;
};

struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 PositionH : SV_POSITION;
};

struct VSFullscreenOutput
{
    float4 PositionH : SV_POSITION;
    float2 UV : TEXCOORD0;
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

    const float2 texcoords[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    o.PositionH = float4(positions[vertexID], 0.0f, 1.0f);
    o.UV = texcoords[vertexID];
    return o;
}

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

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = (1.0f - uv.y) * 2.0f - 1.0f;

    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, gInvViewProj);

    return worldPos.xyz / worldPos.w;
}

float3 CalcLighting(float3 P, float3 N, float3 V, float3 albedo, float3 specColor, float specPower, float3 L, float3 lightCol, float atten)
{
    float3 H = normalize(L + V);
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(specPower, 1.0));
    return (albedo * diff + specColor * spec) * lightCol * atten;
}

// Directional lighting shader (reconstructs world position from depth)
float4 PSDirectional(VSFullscreenOutput pin) : SV_Target
{
    float2 uv = saturate(pin.UV);

    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float3 normal = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);
    float depth = gDepthTex.Sample(gSampler, uv).r;
    float3 posW = ReconstructWorldPosition(uv, depth);
    float4 mat = gMaterialTex.Sample(gSampler, uv);
    float3 specColor = mat.rgb;
    float specPower = saturate(mat.a) * 255.0f;

    float3 V = normalize(gEyePos.xyz - posW);
    float3 L = normalize(-gDirLightDirection.xyz);
    float3 lit = CalcLighting(posW, normal, V, albedo, specColor, specPower, L, gDirLightColorIntensity.rgb, gDirLightColorIntensity.a);
    float3 color = albedo * gAmbientColor.rgb + lit;

    return float4(color, 1.0f);
}

// Point-light shader (reads albedo from gAlbedoTex)
float4 PSPoint(VSOutput pin) : SV_Target
{
    float2 uv = GetUV(pin.PositionH);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        discard;

    float depth = gDepthTex.Sample(gSampler, uv).r;
    float3 P = ReconstructWorldPosition(uv, depth);
    float3 N = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);
    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float4 mat = gMaterialTex.Sample(gSampler, uv);
    float3 specColor = mat.rgb;
    float specPower = saturate(mat.a) * 255.0f;
    float3 V = normalize(gEyePos.xyz - P);

    float3 lightVec = gPositionRange.xyz - P;
    float dist = length(lightVec);
    float range = gPositionRange.w;

    if (dist > range || dist <= 0.0001f)
        discard;

    float atten = saturate(1.0f - dist / range);
    atten *= atten;
    float3 L = lightVec / dist;

    float3 result = CalcLighting(P, N, V, albedo, specColor, specPower, L, gColorIntensity.rgb, gColorIntensity.a * atten);
    return float4(result, 1.0f);
}

// Spot-light shader (reads albedo from gAlbedoTex)
float4 PSSpot(VSOutput pin) : SV_Target
{
    float2 uv = GetUV(pin.PositionH);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        discard;

    float depth = gDepthTex.Sample(gSampler, uv).r;
    float3 P = ReconstructWorldPosition(uv, depth);
    float3 N = DecodeNormal(gNormalTex.Sample(gSampler, uv).xyz);
    float3 albedo = gAlbedoTex.Sample(gSampler, uv).rgb;
    float4 mat = gMaterialTex.Sample(gSampler, uv);
    float3 specColor = mat.rgb;
    float specPower = saturate(mat.a) * 255.0f;
    float3 V = normalize(gEyePos.xyz - P);

    float3 lightVec = gPositionRange.xyz - P;
    float dist = length(lightVec);
    float range = gPositionRange.w;

    if (dist > range || dist <= 0.0001f)
        discard;

    float3 L = lightVec / dist;
    float3 spotDir = normalize(gDirectionCos.xyz);
    float3 lightToPoint = normalize(P - gPositionRange.xyz);
    float theta = dot(lightToPoint, spotDir);
    float outerCos = saturate(gDirectionCos.w);
    if (theta < outerCos)
        discard;

    float innerCos = saturate(lerp(outerCos, 1.0f, 0.20f));
    float cone = smoothstep(outerCos, innerCos, theta);
    float atten = saturate(1.0f - dist / range);
    atten *= atten;

    float3 result = CalcLighting(P, N, V, albedo, specColor, specPower, L, gColorIntensity.rgb, gColorIntensity.a * atten * cone);
    return float4(result, 1.0f);
}
