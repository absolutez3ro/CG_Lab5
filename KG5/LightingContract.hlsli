#ifndef LIGHTING_CONTRACT_HLSLI
#define LIGHTING_CONTRACT_HLSLI

// MUST MATCH LightingContract.h.
#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 3

struct DirectionalLightData
{
    float3 Direction;
    float Intensity;
    float3 Color;
    float Padding;
};

struct PointLightData
{
    float3 Position;
    float Range;
    float3 Color;
    float Intensity;
};

struct SpotLightData
{
    float3 Position;
    float Range;
    // Direction points outward from the spotlight along cone axis.
    float3 Direction;
    float InnerCos;
    float3 Color;
    float OuterCos;
    float Intensity;
    float3 Padding;
};

struct LightingFrameConstants
{
    float4 EyePos;
    float2 ScreenSize;
    float2 InvScreenSize;
    float4 AmbientColor;
    float4x4 InvViewProj;
    DirectionalLightData DirectionalLight;
    uint PointLightCount;
    uint SpotLightCount;
    uint DebugMode;
    uint Padding;
};

struct LocalLightConstants
{
    PointLightData PointLights[MAX_POINT_LIGHTS];
    SpotLightData SpotLights[MAX_SPOT_LIGHTS];
};

#endif
