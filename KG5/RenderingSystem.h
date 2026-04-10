#pragma once
#include "Renderer.h"
#include "GBuffer.h"
#include "LightingContract.h"
#include <array>
#include <deque>
#include <random>
#include <vector>

class RenderingSystem
{
public:
    bool Init(HWND hwnd, int width, int height);
    void BeginFrame(const float clearColor[4]);
    void DrawScene(float totalTime, float deltaTime);
    void EndFrame() { m_renderer.EndFrame(); }
    void OnResize(int width, int height);
    bool LoadObj(const std::string& path) { return m_renderer.LoadObj(path); }

    void SetTexTiling(float x, float y) { m_texTiling = { x, y }; }
    void SetTexScroll(float x, float y) { m_texScroll = { x, y }; }

    void OnKeyDown(WPARAM key);
    void OnKeyUp(WPARAM key);
    void OnMouseDown(int x, int y);
    void OnMouseUp();
    void OnMouseMove(int x, int y);

private:
    struct RainPointLight
    {
        XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 Velocity = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
        float Range = 450.0f;
        float Intensity = 1.9f;
        bool Landed = false;
        uint64_t SpawnIndex = 0;
    };

    struct RainDebugStats
    {
        UINT FallingCount = 0;
        UINT GroundedCount = 0;
        UINT TotalSimulatedCount = 0;
        UINT TotalSelectedForGpu = 0;
        UINT TotalUploadedToGpu = 0;
        UINT TotalVisibleProxiesRendered = 0;
        UINT ClippedDuringGpuSelection = 0;
        UINT ClippedDuringGpuUpload = 0;
        UINT GroundedTrimmedThisFrame = 0;
    };

    bool m_initialized = false;

    void CreateRootSignatures();
    void CreatePSOs();
    void SetupSceneLights();

    void GeometryPass();
    void LightingPassDirectional();
    void LightingPassLocal();
    void RainLightProxyPass();

    void InitializeRainLightSystem();
    RainPointLight GenerateRainLightParameters();
    void SpawnRainLight();
    void UpdateRainLights(float dt);
    void BuildActivePointLightsForGpu();
    void TrimGroundedLightsIfNeeded();

    void UpdateFrameConstants();
    void UpdateLocalLightConstants();
    void UploadPointLightsToGpu();
    void UpdateCamera(float dt);
    void UpdateViewMatrix();

private:
    static constexpr UINT PointLightsSrvIndex = 5;

    Renderer m_renderer;
    GBuffer m_gbuffer;

    XMFLOAT2 m_texTiling = { 1, 1 };
    XMFLOAT2 m_texScroll = { 0, 0 };

    ComPtr<ID3D12RootSignature> m_geometryRS;
    ComPtr<ID3D12RootSignature> m_lightingDirectionalRS;
    ComPtr<ID3D12RootSignature> m_lightingLocalRS;
    ComPtr<ID3D12RootSignature> m_rainProxyRS;

    ComPtr<ID3D12PipelineState> m_geometryPSO;
    ComPtr<ID3D12PipelineState> m_psoDirectional;
    ComPtr<ID3D12PipelineState> m_psoLocal;
    ComPtr<ID3D12PipelineState> m_psoRainProxy;

    ComPtr<ID3DBlob> m_geoVS;
    ComPtr<ID3DBlob> m_geoHS;
    ComPtr<ID3DBlob> m_geoDS;
    ComPtr<ID3DBlob> m_geoPS;
    ComPtr<ID3DBlob> m_lightFullscreenVS;
    ComPtr<ID3DBlob> m_rainProxyVS;

    ComPtr<ID3D12Resource> m_objectTransformCB;
    ComPtr<ID3D12Resource> m_geometryFrameCB;
    ComPtr<ID3D12Resource> m_materialCB;
    ComPtr<ID3D12Resource> m_frameCB;
    ComPtr<ID3D12Resource> m_localLightsCB;
    ComPtr<ID3D12Resource> m_rainProxyFrameCB;
    ComPtr<ID3D12Resource> m_pointLightsUploadBuffer;
    ComPtr<ID3D12Resource> m_pointLightsDefaultBuffer;
    UINT m_objectTransformCbStride = 0;
    UINT m_materialCbStride = 0;
    UINT m_maxObjectCbCount = 0;

    XMFLOAT4X4 m_view{};
    XMFLOAT4X4 m_proj{};
    XMFLOAT3 m_cameraPos = { 0.0f, 120.0f, -300.0f };
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_moveSpeed = 350.0f;
    float m_mouseSensitivity = 0.0035f;
    float m_tessMinFactor = 1.0f;
    float m_tessMaxFactor = 20.0f;
    float m_tessMinDistance = 5.0f;
    float m_tessMaxDistance = 80.0f;

    bool m_moveForward = false;
    bool m_moveBackward = false;
    bool m_moveLeft = false;
    bool m_moveRight = false;

    bool m_mouseLookActive = false;
    bool m_hasLastMouse = false;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;

    UINT m_debugMode = 0;
    // Geometry debug modes:
    // 0 = regular shaded output
    // 1 = transformed normal visualization
    // 2 = displacement value visualization
    // 3 = tessellation factor visualization
    UINT m_geometryDebugMode = 0;
    UINT m_debugStrongDisplacement = 1;

    std::vector<LightingContract::PointLightData> m_activePointLightsForGpu;
    std::array<LightingContract::SpotLightData, LightingContract::MaxSpotLights> m_spotLights{};
    UINT m_activePointLights = 0;
    UINT m_activeSpotLights = 0;
    RainDebugStats m_rainDebugStats{};
    bool m_rainDebugOutputEnabled = true;
    UINT m_rainDebugOutputIntervalFrames = 60;
    UINT m_rainDebugFrameCounter = 0;

    std::deque<RainPointLight> m_fallingRainLights;
    std::deque<RainPointLight> m_groundedRainLights;
    // Seconds between spawn attempts.
    // Lower value => denser visible rain stream.
    float m_rainSpawnInterval = 0.0055f;
    float m_rainSpawnAccumulator = 0.0f;

    // Base downward speed in world units per second.
    // Slightly slower fall keeps more lights visible in-air at once.
    float m_rainFallSpeed = 205.0f;

    // Sponza-aligned spawn/landing region (X/Z) around the central walkable volume.
    // These are intentionally exposed for easy scene-specific tuning.
    XMFLOAT2 m_rainSpawnMinXZ = { -420.0f, -300.0f };
    XMFLOAT2 m_rainSpawnMaxXZ = { 420.0f, 520.0f };

    // Spawn high above the scene so falling is clearly visible.
    float m_rainSpawnY = 460.0f;

    // Sponza floor in this project is near world Y=0; landed lights are clamped here.
    float m_rainFloorY = 0.0f;
    UINT m_rainMinGroundedLights = 500;
    UINT m_rainMaxGroundedLights = 900;
    UINT m_rainMaxFallingLights = 420;
    UINT m_rainMaxRenderablePointLights = LightingContract::MaxPointLights;
    UINT m_rainReservedRenderableFallingLights = 300;

    // Lighting contribution per drop (kept moderate to avoid overexposure).
    float m_rainLightRangeMin = 140.0f;
    float m_rainLightRangeMax = 220.0f;
    float m_rainLightIntensityMin = 0.22f;
    float m_rainLightIntensityMax = 0.52f;

    // Visual proxy size can be larger than actual light range for readability.
    float m_rainProxyRadius = 7.0f;
    float m_rainProxySoftness = 2.25f;
    uint64_t m_rainNextSpawnIndex = 1;
    std::mt19937 m_rainRng{ 1337u };
    std::uniform_real_distribution<float> m_rainUnitDist{ 0.0f, 1.0f };
};
