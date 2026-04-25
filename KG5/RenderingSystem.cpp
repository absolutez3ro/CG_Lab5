#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <vector>

using namespace DirectX;

static void RS_ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("RenderingSystem DX call failed");
}

static float RS_Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static DirectX::XMFLOAT4 RS_NormalizePlane(const DirectX::XMFLOAT4& p)
{
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (len <= 0.000001f)
        return p;
    const float invLen = 1.0f / len;
    return DirectX::XMFLOAT4(p.x * invLen, p.y * invLen, p.z * invLen, p.w * invLen);
}

struct alignas(256) RainProxyFrameConstants
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4 CameraRightAndRadius;
    XMFLOAT4 CameraUpAndSoftness;
    UINT PointLightCount = 0;
    XMFLOAT3 Padding = { 0.0f, 0.0f, 0.0f };
};

bool RenderingSystem::Init(HWND hwnd, int width, int height)
{
    try
    {
        m_hwnd = hwnd;

        if (!m_renderer.Init(hwnd, width, height))
            return false;

    m_gbuffer.Initialize(
        m_renderer.GetDevice(),
        width,
        height,
        m_renderer.GetGbufferRtvStart(),
        m_renderer.GetRtvDescriptorSize(),
        m_renderer.GetGbufferSrvCpuStart(),
        m_renderer.GetGbufferSrvGpuStart(),
        m_renderer.GetSrvDescriptorSize());

    ApplyDirtySceneSettings();

    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&m_cameraPos),
        XMVectorSet(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z + 1.0f, 1.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(width) / static_cast<float>(height), 1.0f, 5000.0f);
    XMStoreFloat4x4(&m_view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));

    CreateRootSignatures();
    CreatePSOs();
    CreateDebugLineResources();
    CreateDebugLinePSO();
    SetupSceneLights();

    m_objectTransformCbStride = (sizeof(ObjectTransformConstants) + 255u) & ~255u;
    m_materialCbStride = (sizeof(MaterialConstants) + 255u) & ~255u;
    m_maxObjectCbCount = 8192;

    m_renderer.CreateBuffer(nullptr, m_objectTransformCbStride * m_maxObjectCbCount, &m_objectTransformCB);
    m_renderer.CreateBuffer(nullptr, m_materialCbStride * m_maxObjectCbCount, &m_materialCB);
    m_renderer.CreateBuffer(nullptr, sizeof(GeometryFrameConstants), &m_geometryFrameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LightingFrameConstants), &m_frameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LocalLightConstants), &m_localLightsCB);
    m_renderer.CreateBuffer(nullptr, sizeof(RainProxyFrameConstants), &m_rainProxyFrameCB);

    const UINT pointLightsBufferSize = static_cast<UINT>(sizeof(LightingContract::PointLightData) * LightingContract::MaxPointLights);
    m_renderer.CreateBuffer(nullptr, pointLightsBufferSize, &m_pointLightsUploadBuffer);

    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(pointLightsBufferSize);
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_pointLightsDefaultBuffer)));

    D3D12_SHADER_RESOURCE_VIEW_DESC pointLightsSrvDesc{};
    pointLightsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pointLightsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    pointLightsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    pointLightsSrvDesc.Buffer.FirstElement = 0;
    pointLightsSrvDesc.Buffer.NumElements = LightingContract::MaxPointLights;
    pointLightsSrvDesc.Buffer.StructureByteStride = sizeof(LightingContract::PointLightData);
    pointLightsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE pointLightsSrvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_renderer.GetSrvHeap()->GetCPUDescriptorHandleForHeapStart(),
        PointLightsSrvIndex,
        m_renderer.GetSrvDescriptorSize());
    m_renderer.GetDevice()->CreateShaderResourceView(m_pointLightsDefaultBuffer.Get(), &pointLightsSrvDesc, pointLightsSrvCpuHandle);

        if (!m_particles.Initialize(&m_renderer))
            return false;

        // Start directly in Sponza for Lab6 smoke check. If load fails, fall back to Dirty scene.
        m_activeSceneKind = DemoSceneKind::DirtyInstancing;
        if (!SwitchToSponzaScene())
        {
            MessageBoxA(
                hwnd,
                "Failed to load Sponza on startup. Falling back to Dirty scene.",
                "Startup Scene Warning",
                MB_OK | MB_ICONWARNING);

            // Force full Dirty scene switch path (do not early-return on same-scene guard).
            m_activeSceneKind = DemoSceneKind::Sponza;
            if (!SwitchToDirtyScene())
                return false;
        }

        m_initialized = true;
        UpdateWindowTitle();
        return true;
    }
    catch (const std::exception& ex)
    {
        std::string msg = std::string("RenderingSystem::Init failed:\n") + ex.what();
        OutputDebugStringA((msg + "\n").c_str());
        MessageBoxA(nullptr, msg.c_str(), "Rendering Init Error", MB_OK | MB_ICONERROR);
        return false;
    }
}

std::string RenderingSystem::GetExeDir() const
{
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    size_t p = path.find_last_of("\\/");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

bool RenderingSystem::TryLoadSponzaWithFallbacks()
{
    const std::string exeDir = GetExeDir();
    const char* candidates[] =
    {
        "assets/sponza/sponza.obj",
        "..\\assets\\sponza\\sponza.obj",
        "..\\..\\assets\\sponza\\sponza.obj",
        "..\\..\\..\\assets\\sponza\\sponza.obj",
    };

    for (const char* rel : candidates)
    {
        const std::string fullPath = exeDir + rel;
        std::string msg = std::string("[SceneSwitch][Sponza] Loading OBJ: ") + fullPath + "\n";
        OutputDebugStringA(msg.c_str());
        if (m_renderer.LoadObj(fullPath))
        {
            OutputDebugStringA("[SceneSwitch][Sponza] LoadObj success\n");
            return true;
        }
    }

    OutputDebugStringA("[SceneSwitch][Sponza] LoadObj failed for all fallback candidates\n");

    std::wstring msg =
        L"Failed to load Sponza scene.\n\n"
        L"Expected file:\n"
        L"assets\\sponza\\sponza.obj\n\n"
        L"Copy the assets/sponza folder from Lab5_clean to Lab5_dirt if it is missing.\n\n"
        L"Exe dir:\n";
    msg += std::wstring(exeDir.begin(), exeDir.end());
    MessageBoxW(nullptr, msg.c_str(), L"Sponza Load Error", MB_OK | MB_ICONERROR);
    return false;
}

void RenderingSystem::ApplySponzaSceneSettings()
{
    m_cameraPos = XMFLOAT3(0.0f, 120.0f, -300.0f);
    m_yaw = 0.0f;
    m_pitch = 0.0f;
    m_moveSpeed = 350.0f;
    m_tessMinFactor = 1.0f;
    m_tessMaxFactor = 20.0f;
    m_tessMinDistance = 5.0f;
    m_tessMaxDistance = 80.0f;
    m_ambientColor = XMFLOAT4(0.16f, 0.16f, 0.18f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.20f, -1.0f, 0.10f);
    m_directionalLightColor = XMFLOAT3(0.95f, 0.97f, 1.00f);
    m_directionalLightIntensity = 1.35f;
}

void RenderingSystem::ApplyDirtySceneSettings()
{
    // Dirty scene defaults.
    m_cameraPos = XMFLOAT3(-235.0f, 54.0f, -235.0f);
    m_yaw = 0.78f;
    m_pitch = -0.28f;
    m_moveSpeed = 350.0f;
    m_tessMinFactor = 1.0f;
    m_tessMaxFactor = 1.0f;
    m_tessMinDistance = 5.0f;
    m_tessMaxDistance = 80.0f;
    m_ambientColor = XMFLOAT4(0.28f, 0.28f, 0.30f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.30f, -1.0f, 0.25f);
    m_directionalLightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
    m_directionalLightIntensity = 2.20f;
}

bool RenderingSystem::SwitchToSponzaScene()
{
    if (m_activeSceneKind == DemoSceneKind::Sponza)
        return true;

    OutputDebugStringA("[SceneSwitch][Sponza] Begin\n");
    LogSceneState("[SceneSwitch][Sponza] Pre");
    OutputDebugStringA("[SceneSwitch][Sponza] WaitForIdle before\n");
    m_renderer.WaitForIdle();

    OutputDebugStringA("[SceneSwitch][Sponza] Set flags begin\n");
    if (!TryLoadSponzaWithFallbacks())
        return false;

    m_activeSceneKind = DemoSceneKind::Sponza;
    ApplySponzaSceneSettings();
    m_renderMainSceneModel = true;
    m_useTessellationForScene = true;
    m_enableFallingLights = true;
    m_enableGroundPlane = false;
    m_showCullingDebugGrid = false;
    m_debugLineVertices.clear();
    OutputDebugStringA("[SceneSwitch][Sponza] Set flags done\n");
    LogSceneState("[SceneSwitch][Sponza] AfterFlags");

    OutputDebugStringA("[SceneSwitch][Sponza] BuildSingleMainSceneObject begin\n");
    BuildSingleMainSceneObject();
    OutputDebugStringA("[SceneSwitch][Sponza] BuildSingleMainSceneObject done\n");
    LogSceneState("[SceneSwitch][Sponza] AfterSingleObject");

    OutputDebugStringA("[SceneSwitch][Sponza] SetupSceneLights begin\n");
    SetupSceneLights();
    OutputDebugStringA("[SceneSwitch][Sponza] SetupSceneLights done\n");
    // Restart particles so the Sponza fountain appears immediately at the demo emitter location.
    m_particlesReinitRequested = true;
    UpdateViewMatrix();
    UpdateWindowTitle();
    OutputDebugStringA("[SceneSwitch][Sponza] WaitForIdle after\n");
    m_renderer.WaitForIdle();
    LogSceneState("[SceneSwitch][Sponza] EndState");
    OutputDebugStringA("[SceneSwitch][Sponza] End\n");
    return true;
}

bool RenderingSystem::SwitchToDirtyScene()
{
    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
        return true;

    OutputDebugStringA("[SceneSwitch][Dirty] Begin\n");
    LogSceneState("[SceneSwitch][Dirty] Pre");
    m_renderer.WaitForIdle();

    m_activeSceneKind = DemoSceneKind::DirtyInstancing;
    m_renderMainSceneModel = false;
    m_useTessellationForScene = false;
    m_enableFallingLights = false;
    m_enableGroundPlane = true;
    m_debugStrongDisplacement = 0;
    m_geometryDebugMode = 0;
    m_sceneObjectCount = 1000;
    m_massPlacementMode = MassPlacementMode::Grid;
    m_showCullingDebugGrid = m_enableCulling;
    ApplyDirtySceneSettings();
    m_activePointLightsForGpu.clear();
    m_activePointLights = 0;
    m_rainDebugStats = RainDebugStats{};
    if (!LoadMassPrimitiveScene())
    {
        OutputDebugStringA("[SceneSwitch] Failed to load dirty primitive cube scene\n");
        return false;
    }
    SetupSceneLights();
    RebuildCullingDebugLines();
    // Restart particles so old Sponza fountain particles do not remain in the Dirty scene.
    m_particlesReinitRequested = true;
    UpdateViewMatrix();
    UpdateWindowTitle();
    m_renderer.WaitForIdle();
    LogSceneState("[SceneSwitch][Dirty] EndState");
    OutputDebugStringA("[SceneSwitch][Dirty] End\n");
    return true;
}

void RenderingSystem::UpdateWindowTitle() const
{
    if (!m_hwnd)
        return;

    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        wchar_t title[256];
        swprintf_s(
            title,
            L"[SPONZA] Deferred Renderer | Particles: %u %s %s",
            m_particles.GetAliveCountForDraw(),
            m_particles.IsEnabled() ? L"ON" : L"OFF",
            m_particles.IsSortEnabled() ? L"SORT" : L"NOSORT");
        SetWindowTextW(m_hwnd, title);
    }
    else
    {
        wchar_t title[320];
        const wchar_t* modeLabel = L"[NO CULLING]";
        if (m_enableCulling)
            modeLabel = m_useOctreeMode ? L"[OCTREE + GRID]" : L"[FRUSTUM + GRID]";
        swprintf_s(
            title,
            L"%s INSTANCING: %u / %u cubes visible | Particles: %u %s %s",
            modeLabel,
            m_visibleObjectCount,
            m_sceneObjectCount,
            m_particles.GetAliveCountForDraw(),
            m_particles.IsEnabled() ? L"ON" : L"OFF",
            m_particles.IsSortEnabled() ? L"SORT" : L"NOSORT");
        SetWindowTextW(m_hwnd, title);
    }
}

XMFLOAT3 RenderingSystem::GetParticleEmitterPosition() const
{
    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        // Demo fountain inside Sponza, in front of the default camera.
        // This position is low enough to read as a fountain instead of floating particles.
        return XMFLOAT3(0.0f, 24.0f, -85.0f);
    }

    return XMFLOAT3(0.0f, 40.0f, 0.0f);
}

ParticleSystemGPU::FountainSettings RenderingSystem::GetParticleFountainSettings() const
{
    ParticleSystemGPU::FountainSettings settings{};

    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        // Smoke fountain setup for Lab6 demonstration in Sponza.
        settings.EmitPerFrame = 14;
        settings.BaseVelocity = XMFLOAT3(0.0f, 8.5f, 0.0f);
        settings.VelocityRandomness = XMFLOAT3(16.0f, 6.0f, 16.0f);
        settings.Gravity = XMFLOAT3(0.0f, 0.45f, 0.0f);
        settings.MinLifeSpan = 4.0f;
        settings.MaxLifeSpan = 7.0f;
        settings.MinSize = 14.0f;
        settings.MaxSize = 34.0f;
        settings.EmitterRadius = 18.0f;
        settings.GroundY = 0.0f;
        settings.EnableGroundCollision = 1;
        settings.StartColorA = XMFLOAT4(0.58f, 0.58f, 0.58f, 0.085f);
        settings.StartColorB = XMFLOAT4(0.25f, 0.25f, 0.25f, 0.025f);
        return settings;
    }

    // Smaller fountain for the Dirty cubes scene.
    settings.EmitPerFrame = 32;
    settings.BaseVelocity = XMFLOAT3(0.0f, 30.0f, 0.0f);
    settings.VelocityRandomness = XMFLOAT3(12.0f, 8.0f, 12.0f);
    settings.Gravity = XMFLOAT3(0.0f, -9.8f, 0.0f);
    settings.MinLifeSpan = 2.0f;
    settings.MaxLifeSpan = 3.2f;
    settings.MinSize = 1.6f;
    settings.MaxSize = 4.0f;
    settings.GroundY = -10.0f;
    settings.EnableGroundCollision = 1;
    settings.StartColorA = XMFLOAT4(1.0f, 0.65f, 0.15f, 1.0f);
    settings.StartColorB = XMFLOAT4(0.35f, 0.55f, 1.0f, 1.0f);
    return settings;
}


void RenderingSystem::RequestSceneSwitch(DemoSceneKind scene)
{
    if (scene == m_activeSceneKind)
        return;
    if (m_pendingSceneSwitch.has_value() && *m_pendingSceneSwitch == scene)
        return;

    if (scene == DemoSceneKind::Sponza)
        OutputDebugStringA("[SceneSwitch] Request Sponza\n");
    else
        OutputDebugStringA("[SceneSwitch] Request Dirty\n");

    m_pendingSceneSwitch = scene;
}

bool RenderingSystem::ApplyPendingSceneSwitchIfNeeded()
{
    if (!m_pendingSceneSwitch.has_value())
        return true;

    const DemoSceneKind requested = *m_pendingSceneSwitch;
    m_pendingSceneSwitch.reset();

    if (requested == m_activeSceneKind)
        return true;

    if (requested == DemoSceneKind::Sponza)
        return SwitchToSponzaScene();

    return SwitchToDirtyScene();
}

void RenderingSystem::BuildSingleMainSceneObject()
{
    m_sceneObjects.clear();
    m_sceneObjects.resize(1);

    const XMMATRIX identity = XMMatrixIdentity();
    XMStoreFloat4x4(&m_sceneObjects[0].World, XMMatrixTranspose(identity));
    XMStoreFloat4x4(&m_sceneObjects[0].WorldInvTranspose, XMMatrixTranspose(identity));
    m_sceneObjects[0].BoundsCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_sceneObjects[0].BoundsRadius = 1.0f;
    m_sceneObjects[0].ColorTint = XMFLOAT4(1, 1, 1, 1);
    m_sceneObjects[0].Visible = true;

    m_visibleObjectCount = 1;
}

void RenderingSystem::RegenerateSceneObjects()
{
    const UINT requestedCount = (std::max)(1u, m_sceneObjectCount);
    const UINT subsetCount = static_cast<UINT>((std::max)(size_t(1), m_renderer.GetSubsets().size()));
    const UINT maxCountByDrawBudget = (std::max)(1u, m_sceneMaxDrawCallsBudget / subsetCount);
    const UINT objectCount = (std::min)(requestedCount, maxCountByDrawBudget);

    if (objectCount < requestedCount)
    {
        char msg[256];
        std::snprintf(
            msg,
            sizeof(msg),
            "[SceneCull] Requested %u objects clipped to %u (subsetCount=%u, drawBudget=%u) to avoid GPU timeout.\n",
            requestedCount,
            objectCount,
            subsetCount,
            m_sceneMaxDrawCallsBudget);
        OutputDebugStringA(msg);
    }

    m_sceneObjects.clear();
    m_sceneObjects.reserve(objectCount);

    const float width = m_massPlacementMaxXZ.x - m_massPlacementMinXZ.x;
    const float depth = m_massPlacementMaxXZ.y - m_massPlacementMinXZ.y;

    UINT gridCols = static_cast<UINT>(std::ceil(std::sqrt(static_cast<float>(objectCount))));
    gridCols = (std::max)(1u, gridCols);
    const UINT gridRows = static_cast<UINT>(std::ceil(static_cast<float>(objectCount) / static_cast<float>(gridCols)));
    const float gridStepX = (gridCols > 1u) ? (width / static_cast<float>(gridCols - 1u)) : width;
    const float gridStepZ = (gridRows > 1u) ? (depth / static_cast<float>(gridRows - 1u)) : depth;
    const float jitterX = 0.28f * gridStepX;
    const float jitterZ = 0.28f * gridStepZ;
    const uint32_t sceneSeed = 0x00C0FFEEu ^ (objectCount * 131u) ^ (static_cast<uint32_t>(m_massPlacementMode) * 977u);
    std::mt19937 sceneRng(sceneSeed);
    std::uniform_real_distribution<float> sceneUnitDist(0.0f, 1.0f);

    for (UINT i = 0; i < objectCount; ++i)
    {
        float worldX = 0.0f;
        float worldZ = 0.0f;
        const float objectScale = m_massScaleMin + (m_massScaleMax - m_massScaleMin) * sceneUnitDist(sceneRng);
        const float yOffset = m_massYOffsetMin + (m_massYOffsetMax - m_massYOffsetMin) * sceneUnitDist(sceneRng);
        const float worldY = m_massPlacementY + 0.5f * objectScale + yOffset;

        if (m_massPlacementMode == MassPlacementMode::Random)
        {
            worldX = m_massPlacementMinXZ.x + width * sceneUnitDist(sceneRng);
            worldZ = m_massPlacementMinXZ.y + depth * sceneUnitDist(sceneRng);
        }
        else
        {
            const UINT row = i / gridCols;
            const UINT col = i % gridCols;
            const float tx = (gridCols > 1u) ? static_cast<float>(col) / static_cast<float>(gridCols - 1u) : 0.5f;
            const float tz = (gridRows > 1u) ? static_cast<float>(row) / static_cast<float>(gridRows - 1u) : 0.5f;
            worldX = m_massPlacementMinXZ.x + tx * width;
            worldZ = m_massPlacementMinXZ.y + tz * depth;

            const float offsetX = -jitterX + 2.0f * jitterX * sceneUnitDist(sceneRng);
            const float offsetZ = -jitterZ + 2.0f * jitterZ * sceneUnitDist(sceneRng);
            worldX = std::clamp(worldX + offsetX, m_massPlacementMinXZ.x, m_massPlacementMaxXZ.x);
            worldZ = std::clamp(worldZ + offsetZ, m_massPlacementMinXZ.y, m_massPlacementMaxXZ.y);
        }

        const XMMATRIX world = XMMatrixScaling(objectScale, objectScale, objectScale) * XMMatrixTranslation(worldX, worldY, worldZ);

        SceneObject object{};
        XMStoreFloat4x4(&object.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&object.WorldInvTranspose, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
        object.BoundsCenter = XMFLOAT3(
            worldX + m_massObjectBoundsCenter.x * objectScale,
            worldY + m_massObjectBoundsCenter.y * objectScale,
            worldZ + m_massObjectBoundsCenter.z * objectScale);
        object.BoundsRadius = m_massObjectBoundsRadius * objectScale;
        object.ColorTint = XMFLOAT4(
            0.70f + 0.50f * sceneUnitDist(sceneRng),
            0.70f + 0.50f * sceneUnitDist(sceneRng),
            0.70f + 0.50f * sceneUnitDist(sceneRng),
            1.0f);
        object.Visible = true;
        m_sceneObjects.push_back(object);
    }

    m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
    RebuildCullingDebugLines();
    UpdateObjectVisibility();
    UpdateWindowTitle();
}

bool RenderingSystem::LoadMassPrimitiveScene()
{
    if (!m_renderer.LoadPrimitiveCubeScene())
        return false;

    m_useTessellationForScene = false;
    m_renderMainSceneModel = false;
    RegenerateSceneObjects();
    OutputDirtySceneStats();
    return true;
}

RenderingSystem::FrustumPlanes RenderingSystem::BuildFrustumPlanes() const
{
    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));
    const XMMATRIX vp = view * proj;

    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, vp);

    FrustumPlanes planes{};
    planes.Left = RS_NormalizePlane(XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41));
    planes.Right = RS_NormalizePlane(XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41));
    planes.Top = RS_NormalizePlane(XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42));
    planes.Bottom = RS_NormalizePlane(XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42));
    planes.Near = RS_NormalizePlane(XMFLOAT4(m._13, m._23, m._33, m._43));
    planes.Far = RS_NormalizePlane(XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43));
    return planes;
}

bool RenderingSystem::IsSphereVisible(const XMFLOAT3& center, float radius, const FrustumPlanes& frustum) const
{
    const XMFLOAT4 planes[] = { frustum.Left, frustum.Right, frustum.Top, frustum.Bottom, frustum.Near, frustum.Far };
    for (const XMFLOAT4& p : planes)
    {
        const float distance = p.x * center.x + p.y * center.y + p.z * center.z + p.w;
        if (distance < -radius)
            return false;
    }
    return true;
}

void RenderingSystem::UpdateObjectVisibility()
{
    if (m_activeSceneKind != DemoSceneKind::DirtyInstancing)
    {
        for (SceneObject& object : m_sceneObjects)
            object.Visible = true;
        m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
        return;
    }

    if (!m_enableCulling)
    {
        for (SceneObject& object : m_sceneObjects)
            object.Visible = true;
        m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
        return;
    }

    const FrustumPlanes frustum = BuildFrustumPlanes();
    UINT visible = 0;
    for (SceneObject& object : m_sceneObjects)
    {
        object.Visible = IsSphereVisible(object.BoundsCenter, object.BoundsRadius, frustum);
        if (object.Visible)
            ++visible;
    }
    m_visibleObjectCount = visible;
}

void RenderingSystem::OutputDirtySceneStats() const
{
    const auto& subsets = m_renderer.GetSubsets();
    char msg[256];
    std::snprintf(
        msg,
        sizeof(msg),
        "[SceneSwitch] Dirty stats: objects=%u subsets=%zu visible=%u tess=%d mainModel=%d\n",
        m_sceneObjectCount,
        subsets.size(),
        m_visibleObjectCount,
        m_useTessellationForScene ? 1 : 0,
        m_renderMainSceneModel ? 1 : 0);
    OutputDebugStringA(msg);
}

void RenderingSystem::LogSceneState(const char* stageTag) const
{
    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();
    char msg[512];
    std::snprintf(
        msg,
        sizeof(msg),
        "%s scene=%d mainModel=%d tess=%d falling=%d sceneObjects=%zu subsets=%zu materials=%zu indices=%u vertices=%u topology=%s\n",
        stageTag,
        static_cast<int>(m_activeSceneKind),
        m_renderMainSceneModel ? 1 : 0,
        m_useTessellationForScene ? 1 : 0,
        m_enableFallingLights ? 1 : 0,
        m_sceneObjects.size(),
        subsets.size(),
        materials.size(),
        m_renderer.GetIndexCount(),
        m_renderer.GetVertexCount(),
        m_useTessellationForScene ? "patch" : "triangle");
    OutputDebugStringA(msg);
}

void RenderingSystem::CreateDebugLineResources()
{
    m_renderer.CreateBuffer(nullptr, sizeof(DebugLineConstants), &m_debugLineCB);
}

void RenderingSystem::CreateDebugLinePSO()
{
    // Created inside CreatePSOs(); kept for API symmetry.
}

void RenderingSystem::AddDebugLine(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT4& color)
{
    m_debugLineVertices.push_back(DebugLineVertex{ a, color });
    m_debugLineVertices.push_back(DebugLineVertex{ b, color });
}

void RenderingSystem::AddDebugBox(const XMFLOAT3& min, const XMFLOAT3& max, const XMFLOAT4& color)
{
    const XMFLOAT3 p000{ min.x, min.y, min.z };
    const XMFLOAT3 p001{ min.x, min.y, max.z };
    const XMFLOAT3 p010{ min.x, max.y, min.z };
    const XMFLOAT3 p011{ min.x, max.y, max.z };
    const XMFLOAT3 p100{ max.x, min.y, min.z };
    const XMFLOAT3 p101{ max.x, min.y, max.z };
    const XMFLOAT3 p110{ max.x, max.y, min.z };
    const XMFLOAT3 p111{ max.x, max.y, max.z };

    AddDebugLine(p000, p001, color); AddDebugLine(p001, p011, color); AddDebugLine(p011, p010, color); AddDebugLine(p010, p000, color);
    AddDebugLine(p100, p101, color); AddDebugLine(p101, p111, color); AddDebugLine(p111, p110, color); AddDebugLine(p110, p100, color);
    AddDebugLine(p000, p100, color); AddDebugLine(p001, p101, color); AddDebugLine(p010, p110, color); AddDebugLine(p011, p111, color);
}

void RenderingSystem::RebuildCullingDebugLines()
{
    m_debugLineVertices.clear();
    if (m_activeSceneKind != DemoSceneKind::DirtyInstancing || !m_enableCulling || !m_showCullingDebugGrid)
        return;

    const float minX = m_massPlacementMinXZ.x;
    const float minZ = m_massPlacementMinXZ.y;
    const float maxX = m_massPlacementMaxXZ.x;
    const float maxZ = m_massPlacementMaxXZ.y;

    if (!m_useOctreeMode)
    {
        AddDebugBox(XMFLOAT3(minX, 0.0f, minZ), XMFLOAT3(maxX, 80.0f, maxZ), XMFLOAT4(0.0f, 1.0f, 0.7f, 1.0f));
        constexpr int gridN = 10;
        for (int i = 0; i <= gridN; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(gridN);
            const float x = RS_Lerp(minX, maxX, t);
            const float z = RS_Lerp(minZ, maxZ, t);
            AddDebugLine(XMFLOAT3(x, 0.0f, minZ), XMFLOAT3(x, 80.0f, minZ), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f));
            AddDebugLine(XMFLOAT3(x, 0.0f, minZ), XMFLOAT3(x, 0.0f, maxZ), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f));
            AddDebugLine(XMFLOAT3(minX, 0.0f, z), XMFLOAT3(maxX, 0.0f, z), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f));
        }
    }
    else
    {
        const XMFLOAT3 bmin(minX - 20.0f, 0.0f, minZ - 20.0f);
        const XMFLOAT3 bmax(maxX + 20.0f, 120.0f, maxZ + 20.0f);
        AddDebugBox(bmin, bmax, XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f));
        const int levels = 3;
        for (int level = 1; level <= levels; ++level)
        {
            const int div = 1 << level;
            const XMFLOAT4 color = (level == 1) ? XMFLOAT4(1.0f, 0.6f, 0.1f, 1.0f) :
                (level == 2) ? XMFLOAT4(1.0f, 0.3f, 0.1f, 1.0f) : XMFLOAT4(1.0f, 0.1f, 0.1f, 1.0f);
            for (int i = 0; i < div; ++i)
            {
                for (int j = 0; j < div; ++j)
                {
                    const float x0 = RS_Lerp(bmin.x, bmax.x, static_cast<float>(i) / div);
                    const float x1 = RS_Lerp(bmin.x, bmax.x, static_cast<float>(i + 1) / div);
                    const float z0 = RS_Lerp(bmin.z, bmax.z, static_cast<float>(j) / div);
                    const float z1 = RS_Lerp(bmin.z, bmax.z, static_cast<float>(j + 1) / div);
                    AddDebugBox(XMFLOAT3(x0, 0.0f, z0), XMFLOAT3(x1, 60.0f + level * 20.0f, z1), color);
                }
            }
        }
    }
}

void RenderingSystem::UploadDebugLines()
{
    if (m_debugLineVertices.empty())
        return;

    if (m_debugLineVertices.size() > m_debugLineVertexCapacity || !m_debugLineVertexBuffer)
    {
        m_debugLineVertexCapacity = static_cast<UINT>(m_debugLineVertices.size() + m_debugLineVertices.size() / 2 + 64);
        m_renderer.CreateBuffer(nullptr, static_cast<UINT>(sizeof(DebugLineVertex) * m_debugLineVertexCapacity), &m_debugLineVertexBuffer);
    }

    void* mapped = nullptr;
    m_debugLineVertexBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, m_debugLineVertices.data(), sizeof(DebugLineVertex) * m_debugLineVertices.size());
    m_debugLineVertexBuffer->Unmap(0, nullptr);

    m_debugLineVbView.BufferLocation = m_debugLineVertexBuffer->GetGPUVirtualAddress();
    m_debugLineVbView.StrideInBytes = sizeof(DebugLineVertex);
    m_debugLineVbView.SizeInBytes = static_cast<UINT>(sizeof(DebugLineVertex) * m_debugLineVertices.size());
}

void RenderingSystem::DebugLinePass()
{
    if (m_debugLineVertices.empty() || !m_debugLinePSO || !m_debugLineRS)
        return;

    UploadDebugLines();

    DebugLineConstants cb{};
    const XMMATRIX vp = XMMatrixMultiply(XMMatrixTranspose(XMLoadFloat4x4(&m_view)), XMMatrixTranspose(XMLoadFloat4x4(&m_proj)));
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(vp));

    void* mapped = nullptr;
    m_debugLineCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_debugLineCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_debugLineRS.Get());
    cmdList->SetPipelineState(m_debugLinePSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_debugLineVbView);
    cmdList->SetGraphicsRootConstantBufferView(0, m_debugLineCB->GetGPUVirtualAddress());
    cmdList->DrawInstanced(static_cast<UINT>(m_debugLineVertices.size()), 1, 0, 0);
}

void RenderingSystem::OnKeyDown(WPARAM key)
{
    if (key == 'Z')
    {
        RequestSceneSwitch(DemoSceneKind::Sponza);
        return;
    }
    if (key == 'X')
    {
        RequestSceneSwitch(DemoSceneKind::DirtyInstancing);
        return;
    }

    if (key == 'W') m_moveForward = true;
    if (key == 'S') m_moveBackward = true;
    if (key == 'A') m_moveLeft = true;
    if (key == 'D') m_moveRight = true;
    if (key == 'Q') m_moveUp = true;
    if (key == 'E') m_moveDown = true;
    if (key == 'P')
    {
        m_particles.SetEnabled(!m_particles.IsEnabled());
        UpdateWindowTitle();
        return;
    }
    if (key == 'O')
    {
        m_particles.SetSortEnabled(!m_particles.IsSortEnabled());
        UpdateWindowTitle();
        return;
    }
    if (key == 'I')
    {
        m_particlesReinitRequested = true;
        UpdateWindowTitle();
        return;
    }

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        if (key == '1')
        {
            if (m_enableCulling && !m_useOctreeMode)
            {
                m_enableCulling = false;
                m_showCullingDebugGrid = false;
                m_debugLineVertices.clear();
            }
            else
            {
                m_enableCulling = true;
                m_useOctreeMode = false;
                m_showCullingDebugGrid = true;
                RebuildCullingDebugLines();
            }
            UpdateObjectVisibility();
            UpdateWindowTitle();
            return;
        }
        if (key == '2')
        {
            if (m_enableCulling && m_useOctreeMode)
            {
                m_enableCulling = false;
                m_showCullingDebugGrid = false;
                m_debugLineVertices.clear();
            }
            else
            {
                m_enableCulling = true;
                m_useOctreeMode = true;
                m_showCullingDebugGrid = true;
                RebuildCullingDebugLines();
            }
            UpdateObjectVisibility();
            UpdateWindowTitle();
            return;
        }
    }

    // Debug modes:
    // 0=Final
    // 1=Albedo
    // 2=Normal
    // 3=Material
    // 4=Depth
    // 5=Lighting only
    // 6=Point lights only (all active point lights)
    // 7=Spot lights only
    // 8=Rain point lights debug (alias of point-only for explicit QA)
    if (key >= '0' && key <= '8')
        m_debugMode = static_cast<UINT>(key - '0');
    if (key == '9')
        m_debugMode = 1;

    // Geometry debug visualization (F1..F4):
    // F1 = regular render
    // F2 = transformed normal visualization
    // F3 = displacement value visualization
    // F4 = tessellation factor visualization
    if (key == VK_F1) m_geometryDebugMode = 0;
    if (key == VK_F2) m_geometryDebugMode = 1;
    if (key == VK_F3) m_geometryDebugMode = 2;
    if (key == VK_F4) m_geometryDebugMode = 3;
    if (key == VK_F5) m_debugStrongDisplacement = (m_debugStrongDisplacement == 0) ? 1u : 0u;

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        if (key == 'G')
        {
            m_massPlacementMode = MassPlacementMode::Grid;
            RegenerateSceneObjects();
            OutputDirtySceneStats();
        }
        if (key == 'R')
        {
            m_massPlacementMode = MassPlacementMode::Random;
            RegenerateSceneObjects();
            OutputDirtySceneStats();
        }
        if (key == VK_F6) m_sceneObjectCount = 200;
        if (key == VK_F7) m_sceneObjectCount = 500;
        if (key == VK_F8) m_sceneObjectCount = 1000;
        if (key == VK_F9) m_sceneObjectCount = 2000;
        if (key == VK_F6 || key == VK_F7 || key == VK_F8 || key == VK_F9)
        {
            RegenerateSceneObjects();
            OutputDirtySceneStats();
        }
    }
}

void RenderingSystem::OnKeyUp(WPARAM key)
{
    if (key == 'W') m_moveForward = false;
    if (key == 'S') m_moveBackward = false;
    if (key == 'A') m_moveLeft = false;
    if (key == 'D') m_moveRight = false;
    if (key == 'Q') m_moveUp = false;
    if (key == 'E') m_moveDown = false;
}

void RenderingSystem::OnMouseDown(int x, int y)
{
    m_mouseLookActive = true;
    m_hasLastMouse = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void RenderingSystem::OnMouseUp()
{
    m_mouseLookActive = false;
    m_hasLastMouse = false;
}

void RenderingSystem::OnMouseMove(int x, int y)
{
    if (!m_mouseLookActive)
        return;

    if (!m_hasLastMouse)
    {
        m_hasLastMouse = true;
        m_lastMouseX = x;
        m_lastMouseY = y;
        return;
    }

    const int dx = x - m_lastMouseX;
    const int dy = y - m_lastMouseY;
    m_lastMouseX = x;
    m_lastMouseY = y;

    m_yaw += static_cast<float>(dx) * m_mouseSensitivity;
    m_pitch -= static_cast<float>(dy) * m_mouseSensitivity;

    const float pitchLimit = XM_PIDIV2 - 0.01f;
    if (m_pitch > pitchLimit) m_pitch = pitchLimit;
    if (m_pitch < -pitchLimit) m_pitch = -pitchLimit;
}

void RenderingSystem::UpdateCamera(float dt)
{
    const float sinYaw = std::sin(m_yaw);
    const float cosYaw = std::cos(m_yaw);
    XMVECTOR forwardXZ = XMVector3Normalize(XMVectorSet(sinYaw, 0.0f, cosYaw, 0.0f));
    XMVECTOR rightXZ = XMVector3Normalize(XMVectorSet(cosYaw, 0.0f, -sinYaw, 0.0f));

    XMVECTOR pos = XMLoadFloat3(&m_cameraPos);
    const float step = m_moveSpeed * dt;
    if (m_moveForward) pos = XMVectorAdd(pos, XMVectorScale(forwardXZ, step));
    if (m_moveBackward) pos = XMVectorSubtract(pos, XMVectorScale(forwardXZ, step));
    if (m_moveLeft) pos = XMVectorSubtract(pos, XMVectorScale(rightXZ, step));
    if (m_moveRight) pos = XMVectorAdd(pos, XMVectorScale(rightXZ, step));
    if (m_moveUp) pos = XMVectorAdd(pos, XMVectorSet(0.0f, step, 0.0f, 0.0f));
    if (m_moveDown) pos = XMVectorSubtract(pos, XMVectorSet(0.0f, step, 0.0f, 0.0f));

    XMStoreFloat3(&m_cameraPos, pos);
    UpdateViewMatrix();
}

void RenderingSystem::UpdateViewMatrix()
{
    const float sinYaw = std::sin(m_yaw);
    const float cosYaw = std::cos(m_yaw);
    const float sinPitch = std::sin(m_pitch);
    const float cosPitch = std::cos(m_pitch);

    const XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(cosPitch * sinYaw, sinPitch, cosPitch * cosYaw, 0.0f));
    const XMVECTOR at = XMVectorAdd(eye, forward);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    XMStoreFloat4x4(&m_view, XMMatrixTranspose(view));
}

void RenderingSystem::BeginFrame(const float clearColor[4])
{
    // Must happen before command allocator/list reset in Renderer::BeginFrame().
    ApplyPendingSceneSwitchIfNeeded();

    m_renderer.BeginFrame();

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();
    auto dsv = m_renderer.GetDsvHandle();

    cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(
        dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);
}

void RenderingSystem::CreateRootSignatures()
{
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0); // ObjectTransformConstants
        params[1].InitAsConstantBufferView(1); // GeometryFrameConstants
        params[2].InitAsConstantBufferView(2); // MaterialConstants
        params[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            4,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_geometryRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingDirectionalRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(1); // LocalLightConstants

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            3,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingLocalRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // RainProxyFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rainProxyRS)));
    }

    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstantBufferView(0);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            1,
            params,
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_debugLineRS)));
    }
}

void RenderingSystem::CreatePSOs()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileShader = [&](const wchar_t* file, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob)
    {
        outBlob.Reset();
        const std::string exeDir = GetExeDir();
        const std::wstring exeDirW(exeDir.begin(), exeDir.end());
        const std::wstring fileW(file);

        const std::vector<std::wstring> candidates =
        {
            fileW,
            exeDirW + fileW,
            exeDirW + L"..\\..\\" + fileW,
            exeDirW + L"..\\..\\..\\" + fileW,
            exeDirW + L"..\\..\\..\\KG5\\" + fileW
        };

        HRESULT lastHr = E_FAIL;
        std::wstring lastPath = fileW;
        std::string lastCompilerError;

        for (const std::wstring& candidate : candidates)
        {
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompileFromFile(
                candidate.c_str(),
                nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE,
                entry,
                target,
                flags,
                0,
                &outBlob,
                &errors);

            if (SUCCEEDED(hr) && outBlob)
            {
                break;
            }

            lastHr = hr;
            lastPath = candidate;
            lastCompilerError.clear();
            if (errors && errors->GetBufferPointer())
            {
                lastCompilerError = static_cast<const char*>(errors->GetBufferPointer());
            }
        }

        if (!outBlob)
        {
            std::string fileUtf8(file, file + std::wcslen(file));
            std::string lastPathUtf8(lastPath.begin(), lastPath.end());
            std::ostringstream oss;
            oss << "Shader compilation failed for " << fileUtf8
                << " [entry=" << entry << ", target=" << target
                << ", hr=0x" << std::hex << static_cast<unsigned long>(lastHr) << "]"
                << ". Last tried path: " << lastPathUtf8;
            if (!lastCompilerError.empty())
            {
                oss << ": " << lastCompilerError;
            }
            throw std::runtime_error(oss.str());
        }
    };

    compileShader(L"GeometryPass.hlsl", "VSMain", "vs_5_0", m_geoVS);
    compileShader(L"GeometryPass.hlsl", "HSMain", "hs_5_0", m_geoHS);
    compileShader(L"GeometryPass.hlsl", "DSMain", "ds_5_0", m_geoDS);
    compileShader(L"GeometryPass.hlsl", "PSMain", "ps_5_0", m_geoPS);
    compileShader(L"GeometryPass.hlsl", "VSMainNoTess", "vs_5_0", m_geoNoTessVS);
    compileShader(L"GeometryPass.hlsl", "PSMainNoTess", "ps_5_0", m_geoNoTessPS);
    compileShader(L"LightingPass.hlsl", "VSFullscreen", "vs_5_0", m_lightFullscreenVS);
    compileShader(L"RainLightProxy.hlsl", "VSProxy", "vs_5_0", m_rainProxyVS);
    compileShader(L"DebugLine.hlsl", "VSMain", "vs_5_0", m_debugLineVS);

    if (!m_geoVS || !m_geoHS || !m_geoDS || !m_geoPS || !m_geoNoTessVS || !m_geoNoTessPS || !m_lightFullscreenVS || !m_rainProxyVS || !m_debugLineVS)
    {
        throw std::runtime_error("CreatePSOs: one or more mandatory shader blobs are null after compilation.");
    }

    D3D12_INPUT_ELEMENT_DESC geoLayout[] =
    {
        { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geoDesc{};
    geoDesc.InputLayout = { geoLayout, _countof(geoLayout) };
    geoDesc.pRootSignature = m_geometryRS.Get();
    geoDesc.VS = { m_geoVS->GetBufferPointer(), m_geoVS->GetBufferSize() };
    geoDesc.HS = { m_geoHS->GetBufferPointer(), m_geoHS->GetBufferSize() };
    geoDesc.DS = { m_geoDS->GetBufferPointer(), m_geoDS->GetBufferSize() };
    geoDesc.PS = { m_geoPS->GetBufferPointer(), m_geoPS->GetBufferSize() };
    geoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geoDesc.SampleMask = UINT_MAX;
    geoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    geoDesc.NumRenderTargets = 3;
    geoDesc.RTVFormats[0] = m_gbuffer.GetFormat(GBuffer::Albedo);
    geoDesc.RTVFormats[1] = m_gbuffer.GetFormat(GBuffer::Normal);
    geoDesc.RTVFormats[2] = m_gbuffer.GetFormat(GBuffer::Material);
    geoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geoDesc.SampleDesc.Count = 1;

    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&geoDesc, IID_PPV_ARGS(&m_geometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geoNoTessDesc = geoDesc;
    geoNoTessDesc.VS = { m_geoNoTessVS->GetBufferPointer(), m_geoNoTessVS->GetBufferSize() };
    geoNoTessDesc.HS = {};
    geoNoTessDesc.DS = {};
    geoNoTessDesc.PS = { m_geoNoTessPS->GetBufferPointer(), m_geoNoTessPS->GetBufferSize() };
    geoNoTessDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&geoNoTessDesc, IID_PPV_ARGS(&m_geometryNoTessPSO)));

    auto makeFullscreenLightingPso = [&](const char* psEntry, bool additive, ID3D12RootSignature* rootSignature, ComPtr<ID3D12PipelineState>& outPSO)
    {
        ComPtr<ID3DBlob> psBlob;
        compileShader(L"LightingPass.hlsl", psEntry, "ps_5_0", psBlob);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.InputLayout = { nullptr, 0 };
        desc.pRootSignature = rootSignature;
        desc.VS = { m_lightFullscreenVS->GetBufferPointer(), m_lightFullscreenVS->GetBufferSize() };
        desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        if (additive)
        {
            desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
            desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        }

        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&outPSO)));
    };

    makeFullscreenLightingPso("PSDirectional", false, m_lightingDirectionalRS.Get(), m_psoDirectional);
    makeFullscreenLightingPso("PSLocalLights", true, m_lightingLocalRS.Get(), m_psoLocal);

    {
        ComPtr<ID3DBlob> proxyPsBlob;
        compileShader(L"RainLightProxy.hlsl", "PSProxy", "ps_5_0", proxyPsBlob);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC proxyDesc{};
        proxyDesc.InputLayout = { nullptr, 0 };
        proxyDesc.pRootSignature = m_rainProxyRS.Get();
        proxyDesc.VS = { m_rainProxyVS->GetBufferPointer(), m_rainProxyVS->GetBufferSize() };
        proxyDesc.PS = { proxyPsBlob->GetBufferPointer(), proxyPsBlob->GetBufferSize() };
        proxyDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        proxyDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        proxyDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        proxyDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        proxyDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        proxyDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        proxyDesc.DepthStencilState.DepthEnable = FALSE;
        proxyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        proxyDesc.SampleMask = UINT_MAX;
        proxyDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        proxyDesc.NumRenderTargets = 1;
        proxyDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        proxyDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        proxyDesc.SampleDesc.Count = 1;

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&proxyDesc, IID_PPV_ARGS(&m_psoRainProxy)));
    }

    {
        ComPtr<ID3DBlob> linePsBlob;
        compileShader(L"DebugLine.hlsl", "PSMain", "ps_5_0", linePsBlob);
        D3D12_INPUT_ELEMENT_DESC lineLayout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC lineDesc{};
        lineDesc.InputLayout = { lineLayout, _countof(lineLayout) };
        lineDesc.pRootSignature = m_debugLineRS.Get();
        lineDesc.VS = { m_debugLineVS->GetBufferPointer(), m_debugLineVS->GetBufferSize() };
        lineDesc.PS = { linePsBlob->GetBufferPointer(), linePsBlob->GetBufferSize() };
        lineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        lineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        lineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        lineDesc.DepthStencilState.DepthEnable = FALSE;
        lineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        lineDesc.SampleMask = UINT_MAX;
        lineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        lineDesc.NumRenderTargets = 1;
        lineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        lineDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        lineDesc.SampleDesc.Count = 1;
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&lineDesc, IID_PPV_ARGS(&m_debugLinePSO)));
    }
}

void RenderingSystem::SetupSceneLights()
{
    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        SetupSponzaLights();
    }
    else
    {
        SetupDirtySceneLights();
    }
}

void RenderingSystem::SetupSponzaLights()
{
    InitializeRainLightSystem();
    SeedRainLightsForSponza();

    m_spotLights.fill(LightingContract::SpotLightData{});
    m_activeSpotLights = 0;

    auto setSpotLight = [this](size_t index, const LightingContract::SpotLightData& light)
    {
        if (index < m_spotLights.size())
        {
            m_spotLights[index] = light;
            const UINT usedCount = static_cast<UINT>(index + 1);
            if (usedCount > m_activeSpotLights)
                m_activeSpotLights = usedCount;
        }
    };

    LightingContract::SpotLightData leftSpot{};
    leftSpot.Position = XMFLOAT3(-760.f, 430.f, -180.f);
    leftSpot.Range = 900.f;
    leftSpot.Direction = XMFLOAT3(0.35f, -1.f, 0.12f);
    leftSpot.InnerCos = 0.88f;
    leftSpot.Color = XMFLOAT3(1.00f, 0.20f, 0.20f);
    leftSpot.OuterCos = 0.79f;
    leftSpot.Intensity = 2.40f;
    setSpotLight(0, leftSpot);

    LightingContract::SpotLightData rightSpot{};
    rightSpot.Position = XMFLOAT3(760.f, 430.f, -180.f);
    rightSpot.Range = 900.f;
    rightSpot.Direction = XMFLOAT3(-0.35f, -1.f, 0.12f);
    rightSpot.InnerCos = 0.88f;
    rightSpot.Color = XMFLOAT3(0.20f, 1.00f, 0.30f);
    rightSpot.OuterCos = 0.79f;
    rightSpot.Intensity = 2.40f;
    setSpotLight(1, rightSpot);

    LightingContract::SpotLightData backSpot{};
    backSpot.Position = XMFLOAT3(0.f, 460.f, 980.f);
    backSpot.Range = 980.f;
    backSpot.Direction = XMFLOAT3(0.0f, -1.f, -0.28f);
    backSpot.InnerCos = 0.87f;
    backSpot.Color = XMFLOAT3(0.25f, 0.50f, 1.00f);
    backSpot.OuterCos = 0.78f;
    backSpot.Intensity = 2.20f;
    setSpotLight(2, backSpot);
}

void RenderingSystem::SetupDirtySceneLights()
{
    m_spotLights.fill(LightingContract::SpotLightData{});
    m_activeSpotLights = 0;
    m_activePointLightsForGpu.clear();
    m_activePointLights = 0;
}

void RenderingSystem::InitializeRainLightSystem()
{
    m_fallingRainLights.clear();
    m_groundedRainLights.clear();

    m_activePointLightsForGpu.clear();
    m_activePointLightsForGpu.reserve(LightingContract::MaxPointLights);
    m_activePointLights = 0;

    m_rainSpawnAccumulator = 0.0f;
    m_rainNextSpawnIndex = 1;
    m_rainDebugStats = RainDebugStats{};
    m_rainDebugFrameCounter = 0;
}

void RenderingSystem::SeedRainLightsForSponza()
{
    const UINT seedCount = (std::min)(m_rainReservedRenderableFallingLights, m_rainMaxFallingLights);
    for (UINT i = 0; i < seedCount; ++i)
    {
        SpawnRainLight();
    }

    for (RainPointLight& light : m_fallingRainLights)
    {
        light.Position.y = RS_Lerp(m_rainFloorY, m_rainSpawnY, m_rainUnitDist(m_rainRng));
    }

    BuildActivePointLightsForGpu();
}

RenderingSystem::RainPointLight RenderingSystem::GenerateRainLightParameters()
{
    RainPointLight light{};

    // Recommended cold palette for rain lights:
    // cyan/blue/violet only, with no warm yellow tones.
    static constexpr XMFLOAT3 palette[] =
    {
        XMFLOAT3(0.25f, 0.60f, 1.00f), // cold blue
        XMFLOAT3(0.20f, 0.85f, 1.00f), // cyan
        XMFLOAT3(0.35f, 0.50f, 1.00f), // azure
        XMFLOAT3(0.55f, 0.40f, 0.95f), // violet
        XMFLOAT3(0.45f, 0.70f, 1.00f), // icy blue
    };

    const size_t paletteCount = std::size(palette);
    const float selector = m_rainUnitDist(m_rainRng) * static_cast<float>(paletteCount - 1);
    const size_t idxA = static_cast<size_t>(selector);
    const size_t idxB = (std::min)(idxA + 1, paletteCount - 1);
    const float t = selector - static_cast<float>(idxA);

    light.Color = XMFLOAT3(
        RS_Lerp(palette[idxA].x, palette[idxB].x, t),
        RS_Lerp(palette[idxA].y, palette[idxB].y, t),
        RS_Lerp(palette[idxA].z, palette[idxB].z, t));

    // Keep actual lighting contribution moderate to avoid full-scene overexposure.
    // Visual readability is handled mostly by the proxy pass.
    light.Range = RS_Lerp(m_rainLightRangeMin, m_rainLightRangeMax, m_rainUnitDist(m_rainRng));
    light.Intensity = RS_Lerp(m_rainLightIntensityMin, m_rainLightIntensityMax, m_rainUnitDist(m_rainRng));

    // Jitter keeps motion organic while preserving overall rain density.
    const float speedJitter = RS_Lerp(-16.0f, 16.0f, m_rainUnitDist(m_rainRng));
    light.Velocity = XMFLOAT3(0.0f, -(m_rainFallSpeed + speedJitter), 0.0f);

    return light;
}

void RenderingSystem::SpawnRainLight()
{
    if (m_fallingRainLights.size() >= static_cast<size_t>(m_rainMaxFallingLights))
        return;

    RainPointLight light = GenerateRainLightParameters();
    light.Position.x = RS_Lerp(m_rainSpawnMinXZ.x, m_rainSpawnMaxXZ.x, m_rainUnitDist(m_rainRng));
    light.Position.y = m_rainSpawnY;
    light.Position.z = RS_Lerp(m_rainSpawnMinXZ.y, m_rainSpawnMaxXZ.y, m_rainUnitDist(m_rainRng));

    light.Landed = false;
    light.SpawnIndex = m_rainNextSpawnIndex++;

    m_fallingRainLights.push_back(light);
}

void RenderingSystem::TrimGroundedLightsIfNeeded()
{
    const size_t minKeep = static_cast<size_t>(m_rainMinGroundedLights);
    const size_t maxKeep = static_cast<size_t>((std::max)(m_rainMaxGroundedLights, m_rainMinGroundedLights));

    m_rainDebugStats.GroundedTrimmedThisFrame = 0;

    // Keep at least the guaranteed floor pool, but trim oldest once we exceed upper bound.
    while (m_groundedRainLights.size() > maxKeep && m_groundedRainLights.size() > minKeep)
    {
        m_groundedRainLights.pop_front();
        ++m_rainDebugStats.GroundedTrimmedThisFrame;
    }
}

void RenderingSystem::UpdateRainLights(float dt)
{
    if (dt <= 0.0f)
        return;

    m_rainSpawnAccumulator += dt;
    while (m_rainSpawnAccumulator >= m_rainSpawnInterval)
    {
        m_rainSpawnAccumulator -= m_rainSpawnInterval;
        SpawnRainLight();
    }

    std::deque<RainPointLight> stillFalling;
    stillFalling.clear();

    for (RainPointLight& light : m_fallingRainLights)
    {
        light.Position.y += light.Velocity.y * dt;

        if (light.Position.y <= m_rainFloorY)
        {
            light.Position.y = m_rainFloorY;
            light.Velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
            light.Landed = true;
            m_groundedRainLights.push_back(light);
        }
        else
        {
            stillFalling.push_back(light);
        }
    }

    m_fallingRainLights.swap(stillFalling);
    TrimGroundedLightsIfNeeded();
}

void RenderingSystem::BuildActivePointLightsForGpu()
{
    m_activePointLightsForGpu.clear();

    const size_t maxForGpu = static_cast<size_t>((std::min)(m_rainMaxRenderablePointLights, LightingContract::MaxPointLights));
    const size_t reservedFalling = static_cast<size_t>((std::min)(m_rainReservedRenderableFallingLights, m_rainMaxFallingLights));

    auto appendPointLight = [this, maxForGpu](const LightingContract::PointLightData& pointLight)
    {
        if (m_activePointLightsForGpu.size() >= maxForGpu)
            return;

        m_activePointLightsForGpu.push_back(pointLight);
    };

    auto appendRainLight = [this, maxForGpu](const RainPointLight& rain)
    {
        if (m_activePointLightsForGpu.size() >= maxForGpu)
            return;

        LightingContract::PointLightData gpuLight{};
        gpuLight.Position = rain.Position;
        gpuLight.Range = rain.Range;
        gpuLight.Color = rain.Color;
        gpuLight.Intensity = rain.Intensity;
        m_activePointLightsForGpu.push_back(gpuLight);
    };

    // Sponza uses the complete reference-style light set:
    // directional light (UpdateFrameConstants), static point lights here,
    // colored spot lights (SetupSponzaLights), plus the animated rain-light stream below.
    // Static point lights are inserted before rain so they are never pushed out by the rain pool.
    UINT staticPointLightCount = 0;
    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        const LightingContract::PointLightData sponzaPointLights[] =
        {
            { XMFLOAT3(0.0f, 260.0f, 120.0f), 720.0f, XMFLOAT3(0.45f, 0.75f, 1.00f), 1.65f },
            { XMFLOAT3(-520.0f, 190.0f, -80.0f), 560.0f, XMFLOAT3(1.00f, 0.32f, 0.48f), 1.15f },
            { XMFLOAT3(520.0f, 190.0f, -80.0f), 560.0f, XMFLOAT3(0.30f, 1.00f, 0.62f), 1.15f },
            { XMFLOAT3(0.0f, 210.0f, 760.0f), 680.0f, XMFLOAT3(0.32f, 0.46f, 1.00f), 1.25f },
        };

        for (const LightingContract::PointLightData& pointLight : sponzaPointLights)
        {
            appendPointLight(pointLight);
            ++staticPointLightCount;
        }
    }

    // Keep descending lights visibly active even with a large grounded pool.
    size_t fallingRendered = 0;
    for (const RainPointLight& falling : m_fallingRainLights)
    {
        if (fallingRendered >= reservedFalling)
            break;

        appendRainLight(falling);
        ++fallingRendered;
    }

    for (const RainPointLight& grounded : m_groundedRainLights)
    {
        appendRainLight(grounded);
    }

    // Use any remaining GPU budget for additional falling lights.
    for (size_t i = fallingRendered; i < m_fallingRainLights.size(); ++i)
    {
        appendRainLight(m_fallingRainLights[i]);
    }

    m_activePointLights = static_cast<UINT>(m_activePointLightsForGpu.size());

    m_rainDebugStats.FallingCount = static_cast<UINT>(m_fallingRainLights.size());
    m_rainDebugStats.GroundedCount = static_cast<UINT>(m_groundedRainLights.size());
    m_rainDebugStats.TotalSimulatedCount = m_rainDebugStats.FallingCount + m_rainDebugStats.GroundedCount;
    m_rainDebugStats.TotalSelectedForGpu = m_activePointLights;

    const size_t selected = m_activePointLightsForGpu.size();
    const size_t simulated = m_fallingRainLights.size() + m_groundedRainLights.size() + static_cast<size_t>(staticPointLightCount);
    m_rainDebugStats.ClippedDuringGpuSelection = (simulated > selected)
        ? static_cast<UINT>(simulated - selected)
        : 0;
}

void RenderingSystem::GeometryPass()
{
    auto cmdList = m_renderer.GetCmdList();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] =
    {
        m_gbuffer.GetRtvHandle(GBuffer::Albedo),
        m_gbuffer.GetRtvHandle(GBuffer::Normal),
        m_gbuffer.GetRtvHandle(GBuffer::Material),
    };

    auto dsv = m_renderer.GetDsvHandle();
    cmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_geometryRS.Get());
    cmdList->SetPipelineState(m_useTessellationForScene ? m_geometryPSO.Get() : m_geometryNoTessPSO.Get());
    cmdList->IASetPrimitiveTopology(m_useTessellationForScene
        ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
        : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    GeometryFrameConstants frame{};
    frame.View = m_view;
    frame.Proj = m_proj;
    frame.CameraPos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    frame.TessFactorRange = XMFLOAT2(m_tessMinFactor, m_tessMaxFactor);
    frame.TessDistanceRange = XMFLOAT2(m_tessMinDistance, m_tessMaxDistance);
    frame.GeometryDebugMode = m_geometryDebugMode;
    frame.DebugStrongDisplacement = m_debugStrongDisplacement;

    void* frameMapped = nullptr;
    m_geometryFrameCB->Map(0, nullptr, &frameMapped);
    memcpy(frameMapped, &frame, sizeof(frame));
    m_geometryFrameCB->Unmap(0, nullptr);

    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();

    if (subsets.empty())
        return;

    void* transformMapped = nullptr;
    void* materialMapped = nullptr;
    m_objectTransformCB->Map(0, nullptr, &transformMapped);
    m_materialCB->Map(0, nullptr, &materialMapped);

    std::uint8_t* transformBase = reinterpret_cast<std::uint8_t*>(transformMapped);
    std::uint8_t* materialBase = reinterpret_cast<std::uint8_t*>(materialMapped);

    size_t drawIndex = 0;
    const bool drawMainModel = m_renderMainSceneModel || m_sceneObjects.empty();
    const size_t objectCount = drawMainModel ? 1 : m_sceneObjects.size();
    if (drawMainModel)
        m_visibleObjectCount = static_cast<UINT>(objectCount);

    for (size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        if (!drawMainModel && !m_sceneObjects[objectIndex].Visible)
            continue;

        for (size_t subsetIndex = 0; subsetIndex < subsets.size(); ++subsetIndex)
        {
            if (drawIndex >= m_maxObjectCbCount)
                break;

            const auto& s = subsets[subsetIndex];

            ObjectTransformConstants transform{};
            if (drawMainModel)
            {
                const XMMATRIX identity = XMMatrixIdentity();
                XMStoreFloat4x4(&transform.World, XMMatrixTranspose(identity));
                XMStoreFloat4x4(&transform.WorldInvTranspose, XMMatrixTranspose(identity));
                transform.ColorTint = XMFLOAT4(1, 1, 1, 1);
            }
            else
            {
                const SceneObject& object = m_sceneObjects[objectIndex];
                transform.World = object.World;
                transform.WorldInvTranspose = object.WorldInvTranspose;
                transform.ColorTint = object.ColorTint;
            }

            MaterialConstants material{};
            material.MaterialDiffuse = XMFLOAT4(1, 1, 1, 1);
            material.MaterialSpecular = XMFLOAT4(1, 1, 1, 1);
            material.SpecularPower = 32.0f;
            material.HasTexture = 0;
            material.HasNormalMap = 0;
            material.HasDisplacementMap = 0;
            material.DisplacementScale = 0.0f;
            material.DisplacementBias = 0.0f;

            UINT textureSrv = 0;
            if (s.materialIdx >= 0 && s.materialIdx < static_cast<int>(materials.size()))
            {
                const auto& mat = materials[s.materialIdx];
                material.MaterialDiffuse = mat.diffuse;
                material.MaterialSpecular = mat.specular;
                material.SpecularPower = mat.specPower;
                if (mat.diffuseSrvHeapIndex >= 0)
                {
                    material.HasTexture = 1;
                    textureSrv = static_cast<UINT>(mat.diffuseSrvHeapIndex);
                }
                if (mat.normalSrvHeapIndex >= 0 && mat.hasNormalMap)
                {
                    material.HasNormalMap = 1;
                }
                if (mat.displacementSrvHeapIndex >= 0 && mat.hasDisplacementMap)
                {
                    material.HasDisplacementMap = 1;
                    material.DisplacementScale = mat.displacementScale;
                    material.DisplacementBias = mat.displacementBias;
                }
            }

            const UINT transformOffset = static_cast<UINT>(drawIndex * m_objectTransformCbStride);
            const UINT materialOffset = static_cast<UINT>(drawIndex * m_materialCbStride);

            memcpy(transformBase + transformOffset, &transform, sizeof(transform));
            memcpy(materialBase + materialOffset, &material, sizeof(material));

            cmdList->SetGraphicsRootConstantBufferView(0, m_objectTransformCB->GetGPUVirtualAddress() + transformOffset);
            cmdList->SetGraphicsRootConstantBufferView(1, m_geometryFrameCB->GetGPUVirtualAddress());
            cmdList->SetGraphicsRootConstantBufferView(2, m_materialCB->GetGPUVirtualAddress() + materialOffset);
            cmdList->SetGraphicsRootDescriptorTable(3, m_renderer.GetSrvGpuHandle(textureSrv));
            cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexStart, 0, 0);
            ++drawIndex;
        }
    }

    m_objectTransformCB->Unmap(0, nullptr);
    m_materialCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateFrameConstants()
{
    LightingContract::LightingFrameConstants cb{};
    cb.EyePos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    cb.ScreenSize = XMFLOAT2(static_cast<float>(m_renderer.GetWidth()), static_cast<float>(m_renderer.GetHeight()));
    cb.InvScreenSize = XMFLOAT2(1.0f / cb.ScreenSize.x, 1.0f / cb.ScreenSize.y);
    cb.AmbientColor = m_ambientColor;

    const XMVECTOR dirLight = XMVector3Normalize(XMLoadFloat3(&m_directionalLightDirection));
    XMStoreFloat3(&cb.DirectionalLight.Direction, dirLight);
    cb.DirectionalLight.Color = m_directionalLightColor;
    cb.DirectionalLight.Intensity = m_directionalLightIntensity;

    XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));

    cb.PointLightCount = (std::min)(m_activePointLights, LightingContract::MaxPointLights);
    cb.SpotLightCount = m_activeSpotLights;
    cb.DebugMode = m_debugMode;

    void* mapped = nullptr;
    m_frameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_frameCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateLocalLightConstants()
{
    LightingContract::LocalLightConstants lights{};

    for (UINT i = 0; i < m_activeSpotLights; ++i)
    {
        lights.SpotLights[i] = m_spotLights[i];

        XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&lights.SpotLights[i].Direction));
        XMStoreFloat3(&lights.SpotLights[i].Direction, direction);

        lights.SpotLights[i].OuterCos = std::clamp(lights.SpotLights[i].OuterCos, 0.0f, 0.9999f);
        lights.SpotLights[i].InnerCos = std::clamp(lights.SpotLights[i].InnerCos, lights.SpotLights[i].OuterCos, 0.9999f);
    }

    void* mapped = nullptr;
    m_localLightsCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &lights, sizeof(lights));
    m_localLightsCB->Unmap(0, nullptr);
}


void RenderingSystem::UploadPointLightsToGpu()
{
    const UINT clampedPointLightCount = static_cast<UINT>((std::min)(
        m_activePointLightsForGpu.size(),
        static_cast<size_t>(LightingContract::MaxPointLights)));
    const UINT pointLightDataSize = static_cast<UINT>(sizeof(LightingContract::PointLightData) * clampedPointLightCount);

    if (pointLightDataSize > 0)
    {
        void* mapped = nullptr;
        m_pointLightsUploadBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, m_activePointLightsForGpu.data(), pointLightDataSize);
        m_pointLightsUploadBuffer->Unmap(0, nullptr);
    }

    m_activePointLights = clampedPointLightCount;
    m_rainDebugStats.TotalUploadedToGpu = clampedPointLightCount;
    m_rainDebugStats.ClippedDuringGpuUpload = (m_activePointLightsForGpu.size() > clampedPointLightCount)
        ? static_cast<UINT>(m_activePointLightsForGpu.size() - clampedPointLightCount)
        : 0;

    auto cmdList = m_renderer.GetCmdList();

    auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pointLightsDefaultBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pointLightsDefaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    static bool initializedForShaderRead = false;
    if (initializedForShaderRead)
    {
        cmdList->ResourceBarrier(1, &toCopyDest);
    }

    if (pointLightDataSize > 0)
    {
        cmdList->CopyBufferRegion(
            m_pointLightsDefaultBuffer.Get(),
            0,
            m_pointLightsUploadBuffer.Get(),
            0,
            pointLightDataSize);
    }

    cmdList->ResourceBarrier(1, &toShaderResource);
    initializedForShaderRead = true;
}

void RenderingSystem::LightingPassDirectional()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingDirectionalRS.Get());
    cmdList->SetPipelineState(m_psoDirectional.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSrvGpu());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::LightingPassLocal()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingLocalRS.Get());
    cmdList->SetPipelineState(m_psoLocal.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSrvGpu());
    cmdList->SetGraphicsRootConstantBufferView(2, m_localLightsCB->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::RainLightProxyPass()
{
    if (m_activePointLights == 0)
    {
        m_rainDebugStats.TotalVisibleProxiesRendered = 0;
        return;
    }

    RainProxyFrameConstants cb{};
    cb.View = m_view;
    cb.Proj = m_proj;

    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMVECTOR cameraRight = XMVector3Normalize(XMVectorSet(view.r[0].m128_f32[0], view.r[1].m128_f32[0], view.r[2].m128_f32[0], 0.0f));
    const XMVECTOR cameraUp = XMVector3Normalize(XMVectorSet(view.r[0].m128_f32[1], view.r[1].m128_f32[1], view.r[2].m128_f32[1], 0.0f));

    XMFLOAT3 right3{};
    XMFLOAT3 up3{};
    XMStoreFloat3(&right3, cameraRight);
    XMStoreFloat3(&up3, cameraUp);

    cb.CameraRightAndRadius = XMFLOAT4(right3.x, right3.y, right3.z, m_rainProxyRadius);
    cb.CameraUpAndSoftness = XMFLOAT4(up3.x, up3.y, up3.z, m_rainProxySoftness);
    cb.PointLightCount = m_activePointLights;

    void* mapped = nullptr;
    m_rainProxyFrameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_rainProxyFrameCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_rainProxyRS.Get());
    cmdList->SetPipelineState(m_psoRainProxy.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_rainProxyFrameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_renderer.GetSrvGpuHandle(PointLightsSrvIndex));

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, m_activePointLights, 0, 0);
    m_rainDebugStats.TotalVisibleProxiesRendered = m_activePointLights;
}

void RenderingSystem::DrawScene(float totalTime, float deltaTime)
{
    UpdateCamera(deltaTime);
    if (m_enableFallingLights)
    {
        UpdateRainLights(deltaTime);
        BuildActivePointLightsForGpu();
    }
    else
    {
        m_activePointLightsForGpu.clear();
        m_activePointLights = 0;
        m_rainDebugStats = RainDebugStats{};
    }

    auto cmdList = m_renderer.GetCmdList();
    if (m_particlesReinitRequested)
    {
        m_particles.Reinitialize(cmdList);
        m_particlesReinitRequested = false;
    }
    m_particles.Update(cmdList, deltaTime, totalTime, m_cameraPos, GetParticleEmitterPosition(), GetParticleFountainSettings());

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        UpdateObjectVisibility();
    }

    m_gbuffer.BeginGeometryPass(cmdList);
    m_gbuffer.Clear(cmdList);
    GeometryPass();

    m_renderer.TransitionDepthToShaderResource();
    m_gbuffer.EndGeometryPass(cmdList);

    UpdateFrameConstants();
    UpdateLocalLightConstants();
    UploadPointLightsToGpu();

    LightingPassDirectional();

    // Local lights are only needed in final and lighting debug modes.
    if (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 7 || m_debugMode == 8)
    {
        LightingPassLocal();
    }

    if (m_enableFallingLights && (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 8))
    {
        RainLightProxyPass();
    }

    m_particles.Render(
        cmdList,
        m_renderer.GetBackBufferRtv(),
        m_renderer.GetDsvHandle(),
        m_view,
        m_proj,
        m_cameraPos,
        m_directionalLightDirection,
        m_directionalLightIntensity,
        m_directionalLightColor,
        m_ambientColor);

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing && m_enableCulling && m_showCullingDebugGrid)
    {
        DebugLinePass();
    }

    if (m_rainDebugOutputEnabled)
    {
        ++m_rainDebugFrameCounter;
        if (m_rainDebugFrameCounter % (std::max)(1u, m_rainDebugOutputIntervalFrames) == 0)
        {
            char msg[512];
            std::snprintf(
                msg,
                sizeof(msg),
                "[RainDebug] falling=%u grounded=%u simulated=%u selectedGPU=%u uploadedGPU=%u proxies=%u clipSelect=%u clipUpload=%u groundedTrim=%u\n",
                m_rainDebugStats.FallingCount,
                m_rainDebugStats.GroundedCount,
                m_rainDebugStats.TotalSimulatedCount,
                m_rainDebugStats.TotalSelectedForGpu,
                m_rainDebugStats.TotalUploadedToGpu,
                m_rainDebugStats.TotalVisibleProxiesRendered,
                m_rainDebugStats.ClippedDuringGpuSelection,
                m_rainDebugStats.ClippedDuringGpuUpload,
                m_rainDebugStats.GroundedTrimmedThisFrame);
            OutputDebugStringA(msg);
        }
    }

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        UpdateWindowTitle();
    }
}

void RenderingSystem::OnResize(int width, int height)
{
    if (!m_initialized)
        return;

    if (width <= 0 || height <= 0)
        return;

    if (m_renderer.GetSrvHeap() == nullptr)
        return;

    m_renderer.OnResize(width, height);
    m_gbuffer.Resize(
        m_renderer.GetDevice(),
        width,
        height,
        m_renderer.GetGbufferRtvStart(),
        m_renderer.GetRtvDescriptorSize(),
        m_renderer.GetGbufferSrvCpuStart(),
        m_renderer.GetGbufferSrvGpuStart(),
        m_renderer.GetSrvDescriptorSize());

    if (height > 0)
    {
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(width) / static_cast<float>(height), 1.0f, 5000.0f);
        XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));
    }
}
