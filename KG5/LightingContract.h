#pragma once
#include <DirectXMath.h>

using namespace DirectX;

namespace LightingContract
{
    // MUST MATCH LightingContract.hlsli
    constexpr UINT MaxPointLights = 1024;
    constexpr UINT MaxSpotLights = 3;

    struct alignas(16) DirectionalLightData
    {
        XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f };
        float Intensity = 1.0f;
        XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
        float Padding = 0.0f;
    };

    struct alignas(16) PointLightData
    {
        XMFLOAT3 Position;
        float Range;
        XMFLOAT3 Color;
        float Intensity;
    };

    struct alignas(16) SpotLightData
    {
        XMFLOAT3 Position;
        float Range;
        // Direction points outward from the spotlight along cone axis.
        XMFLOAT3 Direction;
        float InnerCos;
        XMFLOAT3 Color;
        float OuterCos;
        float Intensity;
        XMFLOAT3 Padding = { 0.0f, 0.0f, 0.0f };
    };

    struct alignas(256) LightingFrameConstants
    {
        XMFLOAT4 EyePos;
        XMFLOAT2 ScreenSize;
        XMFLOAT2 InvScreenSize;
        XMFLOAT4 AmbientColor;
        XMFLOAT4X4 InvViewProj;
        DirectionalLightData DirectionalLight;
        UINT PointLightCount;
        UINT SpotLightCount;
        UINT DebugMode;
        UINT Padding = 0;
    };

    struct alignas(256) LocalLightConstants
    {
        SpotLightData SpotLights[MaxSpotLights];
    };

    static_assert(sizeof(DirectionalLightData) % 16 == 0, "DirectionalLightData must be 16-byte aligned.");
    static_assert(sizeof(PointLightData) % 16 == 0, "PointLightData must be 16-byte aligned.");
    static_assert(sizeof(SpotLightData) % 16 == 0, "SpotLightData must be 16-byte aligned.");
    static_assert(sizeof(LightingFrameConstants) % 16 == 0, "LightingFrameConstants must be 16-byte aligned.");
    static_assert(sizeof(LocalLightConstants) % 16 == 0, "LocalLightConstants must be 16-byte aligned.");

    static_assert(sizeof(DirectionalLightData) == 32, "DirectionalLightData layout mismatch.");
    static_assert(sizeof(PointLightData) == 32, "PointLightData layout mismatch.");
    static_assert(sizeof(SpotLightData) == 64, "SpotLightData layout mismatch.");
    static_assert(sizeof(LightingFrameConstants) % 256 == 0, "LightingFrameConstants must be CBV-aligned (256 bytes).");
    static_assert(sizeof(LocalLightConstants) % 256 == 0, "LocalLightConstants must be CBV-aligned (256 bytes).");
}
