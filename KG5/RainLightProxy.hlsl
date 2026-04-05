#include "LightingContract.hlsli"

StructuredBuffer<PointLightData> gPointLights : register(t0);

cbuffer RainProxyFrameCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4 gCameraRightAndRadius;
    float4 gCameraUpAndSoftness;
    uint gPointLightCount;
    float3 gPadding;
};

struct VSOut
{
    float4 PositionH : SV_POSITION;
    float2 LocalUV : TEXCOORD0;
    float3 Color : TEXCOORD1;
    float Intensity : TEXCOORD2;
};

VSOut VSProxy(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOut o;

    if (instanceID >= gPointLightCount)
    {
        o.PositionH = float4(0.0f, 0.0f, 0.0f, 0.0f);
        o.LocalUV = float2(0.0f, 0.0f);
        o.Color = float3(0.0f, 0.0f, 0.0f);
        o.Intensity = 0.0f;
        return o;
    }

    const float2 corners[6] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f)
    };

    PointLightData light = gPointLights[instanceID];
    float2 corner = corners[vertexID];

    float3 cameraRight = normalize(gCameraRightAndRadius.xyz);
    float3 cameraUp = normalize(gCameraUpAndSoftness.xyz);
    float radius = gCameraRightAndRadius.w;

    float3 positionW = light.Position + (corner.x * cameraRight + corner.y * cameraUp) * radius;
    float4 positionH = mul(mul(float4(positionW, 1.0f), gView), gProj);

    o.PositionH = positionH;
    o.LocalUV = corner;
    o.Color = light.Color;
    o.Intensity = light.Intensity;
    return o;
}

float4 PSProxy(VSOut pin) : SV_Target
{
    const float dist = length(pin.LocalUV);
    if (dist > 1.0f)
        discard;

    const float radial = saturate(1.0f - dist);
    const float softness = max(gCameraUpAndSoftness.w, 1.0f);
    const float glow = pow(radial, softness);
    // Proxy brightness is intentionally less dependent on physical light intensity:
    // we want dense visible "rain spheres" without over-lighting the whole scene.
    const float brightness = (0.90f + pin.Intensity * 0.55f) * glow;

    return float4(pin.Color * brightness, 1.0f);
}
