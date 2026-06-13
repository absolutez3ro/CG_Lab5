#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Renderer.h"
#include "GBuffer.h"
#include "LightingContract.h"
#include "ParticleSystemGPU.h"
#include <array>
#include <deque>
#include <optional>
#include <random>
#include <vector>


struct alignas(256) PostProcessConstants
{
    XMFLOAT2 ScreenSize = { 1.0f, 1.0f };
    XMFLOAT2 InvScreenSize = { 1.0f, 1.0f };
    float Time = 0.0f;
    UINT Mode = 3;
    float EdgeStrength = 1.0f;
    float DepthEdgeScale = 1.25f;
    float NormalEdgeScale = 1.0f;
    float LumaEdgeScale = 0.8f;
    float VcrIntensity = 1.0f;
    float ScanlineStrength = 0.20f;
    float NoiseStrength = 0.08f;
    float ChromaticAberration = 1.0f;
    float ScannerIntensity = 1.0f;
    float ScannerSpeed = 0.65f;
    float ScannerLineWidth = 0.045f;
    float ScannerTickStrength = 0.75f;
    float NauseaIntensity = 0.85f;
    float NauseaSpeed = 0.85f;
    float KaleidoscopeSegments = 6.0f;
    float NauseaChromaticAberration = 1.4f;
    XMFLOAT2 Padding0 = { 0.0f, 0.0f };
    XMFLOAT4X4 InvViewProj{};
    XMFLOAT4 EyePos = { 0.0f, 0.0f, 0.0f, 1.0f };
    float ScannerMaxDistance = 1600.0f;
    float ScannerWorldLineWidth = 35.0f;
    float ScannerTrailLength = 180.0f;
    float ScannerGridScale = 90.0f;
    float Exposure = 1.0f;
    float Gamma = 2.2f;
    UINT ToneMapperMode = 3;
    float ToneMapWhitePoint = 11.2f;
    float Padding[18] = {};
};

namespace RenderingShadowSettings
{
    constexpr UINT CascadeCount = 4;
    constexpr UINT MapResolution = 2048;
}

class RenderingSystem
{
public:
    struct DebugLineVertex
    {
        XMFLOAT3 Position;
        XMFLOAT4 Color;
    };

    struct DebugLineConstants
    {
        XMFLOAT4X4 ViewProj;
    };

    enum class DemoSceneKind
    {
        DirtyInstancing,
        Sponza,
        PerlinPlane,
        AlphaTestShadow,
        PBRModel
    };

    bool Init(HWND hwnd, int width, int height);
    void BeginFrame(const float clearColor[4]);
    void DrawScene(float totalTime, float deltaTime);
    void EndFrame() { m_renderer.EndFrame(); }
    void OnResize(int width, int height);
    bool LoadObj(const std::string& path) { return m_renderer.LoadObj(path); }
    bool SwitchToSponzaScene();
    bool SwitchToDirtyScene();
    bool SwitchToPerlinPlaneScene();
    bool SwitchToAlphaTestShadowScene();
    bool SwitchToPBRModelScene();
    DemoSceneKind GetActiveSceneKind() const { return m_activeSceneKind; }
    void RequestSceneSwitch(DemoSceneKind scene);
    bool ApplyPendingSceneSwitchIfNeeded();

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

    struct FrustumPlanes
    {
        XMFLOAT4 Left = {};
        XMFLOAT4 Right = {};
        XMFLOAT4 Top = {};
        XMFLOAT4 Bottom = {};
        XMFLOAT4 Near = {};
        XMFLOAT4 Far = {};
    };

    bool m_initialized = false;

    void CreateRootSignatures();
    void CreatePSOs();
    void SetupSceneLights();
    void SetupSponzaLights();
    void SetupDirtySceneLights();
    std::string GetExeDir() const;
    bool TryLoadSponzaWithFallbacks();
    bool TryLoadPBRModelWithFallbacks();
    void ApplySponzaSceneSettings();
    void ApplyDirtySceneSettings();
    void ApplyPerlinPlaneSceneSettings();
    void ApplyAlphaTestShadowSceneSettings();

    void GeometryPass();
    void LightingPassDirectional();
    void ShadowPass();
    void LightingPassLocal();
    void RainLightProxyPass();
    void PostProcessPass(float totalTime);
    void CreateOrResizeSceneColorResources(UINT width, UINT height);
    void TransitionSceneColor(D3D12_RESOURCE_STATES newState);
    void LoadIBLResources();
    bool TryLoadDDSTexture(const std::wstring& path, UINT srvIndex, bool requireCube, ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& upload, const char* debugLabel = "DDS");
    bool TryLoadLDRTexture2D(const std::wstring& path, UINT srvIndex, ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& upload);
    bool CreateFallbackIBLTexture(UINT srvIndex, bool cube, const std::vector<uint8_t>& rgba, ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& upload);

    void InitializeRainLightSystem();
    void SeedRainLightsForSponza();
    RainPointLight GenerateRainLightParameters();
    void SpawnRainLight();
    void UpdateRainLights(float dt);
    void BuildActivePointLightsForGpu();
    void TrimGroundedLightsIfNeeded();

    void UpdateFrameConstants();
    void UpdateCascadedShadowMapsData();
    void UpdateLocalLightConstants();
    void UploadPointLightsToGpu();
    void UpdateCamera(float dt);
    void UpdateViewMatrix();
    void UpdateWindowTitle() const;
    bool LoadMassPrimitiveScene();
    void BuildSingleMainSceneObject();
    void RegenerateSceneObjects();
    void UpdateObjectVisibility();
    FrustumPlanes BuildFrustumPlanes() const;
    bool IsSphereVisible(const XMFLOAT3& center, float radius, const FrustumPlanes& frustum) const;
    void OutputDirtySceneStats() const;
    void LogSceneState(const char* stageTag) const;
    bool EnsureBillboardResources();
    void CreateDebugLineResources();
    void CreateDebugLinePSO();
    void RebuildCullingDebugLines();
    void AddDebugLine(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& color);
    void AddDebugBox(const XMFLOAT3& min, const XMFLOAT3& max, const XMFLOAT4& color);
    void UploadDebugLines();
    void DebugLinePass();
    bool ShouldRunParticleFountain() const;
    XMFLOAT3 GetParticleEmitterPosition() const;
    ParticleSystemGPU::FountainSettings GetParticleFountainSettings() const;

private:
    static constexpr UINT PointLightsSrvIndex = 5;
    static constexpr UINT SceneColorRtvIndex = 5;
    static constexpr UINT SceneColorSrvIndex = 8;
    static constexpr UINT IrradianceMapSrvIndex = 9;
    static constexpr UINT BrdfLutSrvIndex = 10;
    static constexpr UINT PrefilterMapSrvIndex = 11;
    static constexpr DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr UINT ShadowMapSrvIndex = 6;
    static constexpr UINT ShadowCascadeCount = RenderingShadowSettings::CascadeCount;
    static constexpr UINT ShadowMapResolution = RenderingShadowSettings::MapResolution;

    Renderer m_renderer;
    GBuffer m_gbuffer;

    XMFLOAT2 m_texTiling = { 1, 1 };
    XMFLOAT2 m_texScroll = { 0, 0 };

    ComPtr<ID3D12RootSignature> m_geometryRS;
    ComPtr<ID3D12RootSignature> m_lightingDirectionalRS;
    ComPtr<ID3D12RootSignature> m_lightingLocalRS;
    ComPtr<ID3D12RootSignature> m_rainProxyRS;
    ComPtr<ID3D12RootSignature> m_debugLineRS;
    ComPtr<ID3D12RootSignature> m_shadowRS;
    ComPtr<ID3D12RootSignature> m_postProcessRS;

    ComPtr<ID3D12PipelineState> m_geometryPSO;
    ComPtr<ID3D12PipelineState> m_geometryNoTessPSO;
    ComPtr<ID3D12PipelineState> m_psoDirectional;
    ComPtr<ID3D12PipelineState> m_shadowPSO;
    ComPtr<ID3D12PipelineState> m_psoLocal;
    ComPtr<ID3D12PipelineState> m_psoRainProxy;
    ComPtr<ID3D12PipelineState> m_debugLinePSO;
    ComPtr<ID3D12PipelineState> m_postProcessPSO;

    ComPtr<ID3DBlob> m_geoVS;
    ComPtr<ID3DBlob> m_geoHS;
    ComPtr<ID3DBlob> m_geoDS;
    ComPtr<ID3DBlob> m_geoPS;
    ComPtr<ID3DBlob> m_geoNoTessVS;
    ComPtr<ID3DBlob> m_geoNoTessPS;
    ComPtr<ID3DBlob> m_lightFullscreenVS;
    ComPtr<ID3DBlob> m_rainProxyVS;
    ComPtr<ID3DBlob> m_debugLineVS;
    ComPtr<ID3DBlob> m_shadowVS;
    ComPtr<ID3DBlob> m_postProcessVS;

    ComPtr<ID3D12Resource> m_objectTransformCB;
    ComPtr<ID3D12Resource> m_geometryFrameCB;
    ComPtr<ID3D12Resource> m_materialCB;
    ComPtr<ID3D12Resource> m_frameCB;
    ComPtr<ID3D12Resource> m_localLightsCB;
    ComPtr<ID3D12Resource> m_rainProxyFrameCB;
    ComPtr<ID3D12Resource> m_pointLightsUploadBuffer;
    ComPtr<ID3D12Resource> m_pointLightsDefaultBuffer;
    ComPtr<ID3D12Resource> m_debugLineVertexBuffer;
    ComPtr<ID3D12Resource> m_debugLineCB;
    ComPtr<ID3D12Resource> m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    ComPtr<ID3D12Resource> m_shadowFrameCB;
    ComPtr<ID3D12Resource> m_shadowObjectTransformCB;
    ComPtr<ID3D12Resource> m_postProcessCB;
    ComPtr<ID3D12Resource> m_sceneColor;
    ComPtr<ID3D12Resource> m_irradianceMap;
    ComPtr<ID3D12Resource> m_irradianceMapUpload;
    ComPtr<ID3D12Resource> m_brdfLut;
    ComPtr<ID3D12Resource> m_brdfLutUpload;
    ComPtr<ID3D12Resource> m_prefilterMap;
    ComPtr<ID3D12Resource> m_prefilterMapUpload;
    UINT m_objectTransformCbStride = 0;
    UINT m_materialCbStride = 0;
    UINT m_shadowFrameCbStride = 0;
    UINT m_maxObjectCbCount = 0;

    HWND m_hwnd = nullptr;
    DemoSceneKind m_activeSceneKind = DemoSceneKind::DirtyInstancing;
    std::optional<DemoSceneKind> m_pendingSceneSwitch;
    bool m_renderMainSceneModel = false;
    bool m_useTessellationForScene = false;
    bool m_enableFallingLights = false;
    bool m_enableGroundPlane = true;
    bool m_showCullingDebugGrid = false;
    D3D12_RESOURCE_STATES m_sceneColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_CPU_DESCRIPTOR_HANDLE m_sceneColorRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_sceneColorSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_sceneColorSrvGpu{};
    int m_postProcessMode = 3;
    bool m_vcrStrongMode = false;
    bool m_nauseaStrongMode = false;
    UINT m_toneMapperMode = 3;
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    XMFLOAT4X4 m_view{};
    XMFLOAT4X4 m_proj{};
    XMFLOAT3 m_cameraPos = { -235.0f, 54.0f, -235.0f };
    float m_yaw = 0.78f;
    float m_pitch = -0.28f;
    float m_moveSpeed = 350.0f;
    float m_mouseSensitivity = 0.0035f;
    float m_tessMinFactor = 1.0f;
    float m_tessMaxFactor = 1.0f;
    float m_tessMinDistance = 5.0f;
    float m_tessMaxDistance = 80.0f;
    float m_perlinNoiseSeed = 1.0f;

    bool m_moveForward = false;
    bool m_moveBackward = false;
    bool m_moveLeft = false;
    bool m_moveRight = false;
    bool m_moveUp = false;
    bool m_moveDown = false;

    bool m_mouseLookActive = false;
    bool m_hasLastMouse = false;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;

    UINT m_debugMode = 0;
    bool m_forceMirrorMaterial = false;
    bool m_showIBLSkybox = false;
    float m_iblDiffuseStrength = 1.0f;
    float m_iblSpecularStrength = 1.0f;
    // Geometry debug modes:
    // 0 = regular shaded output
    // 1 = transformed normal visualization
    // 2 = displacement value visualization
    // 3 = tessellation factor visualization
    UINT m_geometryDebugMode = 0;
    UINT m_debugStrongDisplacement = 1;
    bool m_enableCulling = true;
    bool m_useOctreeMode = true;

    enum class MassPlacementMode
    {
        Grid,
        Random
    };

    struct SceneObject
    {
        XMFLOAT4X4 World{};
        XMFLOAT4X4 WorldInvTranspose{};
        XMFLOAT3 BoundsCenter = { 0.0f, 0.0f, 0.0f };
        float BoundsRadius = 1.0f;
        XMFLOAT4 ColorTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        bool Visible = true;
    };

    MassPlacementMode m_massPlacementMode = MassPlacementMode::Grid;
    UINT m_sceneObjectCount = 1000;
    UINT m_visibleObjectCount = 0;
    UINT m_cubeDrawCount = 0;
    UINT m_billboardDrawCount = 0;
    UINT m_sceneMaxDrawCallsBudget = 60000;
    XMFLOAT2 m_massPlacementMinXZ = { -360.0f, -360.0f };
    XMFLOAT2 m_massPlacementMaxXZ = { 360.0f, 360.0f };
    float m_massPlacementY = 0.0f;
    float m_massYOffsetMin = 2.0f;
    float m_massYOffsetMax = 30.0f;
    XMFLOAT3 m_massObjectBoundsCenter = { 0.0f, 0.0f, 0.0f };
    float m_massObjectBoundsRadius = 0.8660254f;
    float m_massScaleMin = 7.5f;
    float m_massScaleMax = 14.0f;
    std::vector<SceneObject> m_sceneObjects;
    ComPtr<ID3D12Resource> m_billboardVertexBuffer;
    ComPtr<ID3D12Resource> m_billboardIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_billboardVbView{};
    D3D12_INDEX_BUFFER_VIEW m_billboardIbView{};
    int m_billboardTextureSrv = -1;
    int m_billboardMaterialSrvBase = -1;
    float m_billboardSwitchDistance = 220.0f;
    bool m_billboardInitAttempted = false;
    bool m_billboardReady = false;

    std::vector<DebugLineVertex> m_debugLineVertices;
    D3D12_VERTEX_BUFFER_VIEW m_debugLineVbView{};
    UINT m_debugLineVertexCapacity = 0;

    std::vector<LightingContract::PointLightData> m_activePointLightsForGpu;
    std::array<LightingContract::SpotLightData, LightingContract::MaxSpotLights> m_spotLights{};
    UINT m_activePointLights = 0;
    UINT m_activeSpotLights = 0;
    RainDebugStats m_rainDebugStats{};
    bool m_rainDebugOutputEnabled = true;
    UINT m_rainDebugOutputIntervalFrames = 60;
    UINT m_rainDebugFrameCounter = 0;
    ParticleSystemGPU m_particles;
    bool m_particlesReinitRequested = false;

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

    XMFLOAT4 m_ambientColor = XMFLOAT4(0.28f, 0.28f, 0.30f, 1.0f);
    XMFLOAT3 m_directionalLightDirection = XMFLOAT3(0.30f, -1.0f, 0.25f);
    XMFLOAT3 m_directionalLightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
    float m_directionalLightIntensity = 2.20f;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, RenderingShadowSettings::CascadeCount> m_shadowDsvHandles{};
    XMFLOAT4 m_cascadeSplits = { 0,0,0,0 };
    std::array<XMFLOAT4X4, RenderingShadowSettings::CascadeCount> m_shadowViewProj{};
    float m_cameraNear = 0.1f;
    float m_cameraFar = 1200.0f;
    UINT m_enableShadows = 1;
    INT m_shadowDepthBias = 2000;
    float m_shadowSlopeBias = 2.0f;
};
