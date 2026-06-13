#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <vector>
#include <filesystem>
#include <limits>
#include <initializer_list>

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
    m_cameraNear = 1.0f;
    m_cameraFar = 5000.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(width) / static_cast<float>(height), m_cameraNear, m_cameraFar);
    XMStoreFloat4x4(&m_view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));

    CreateRootSignatures();
    CreatePSOs();
    LoadIBLResources();
    CreateDebugLineResources();
    CreateDebugLinePSO();
    SetupSceneLights();

    m_objectTransformCbStride = (sizeof(ObjectTransformConstants) + 255u) & ~255u;
    m_materialCbStride = (sizeof(MaterialConstants) + 255u) & ~255u;
    m_maxObjectCbCount = 8192;
    m_shadowFrameCbStride = (sizeof(XMFLOAT4X4) + 255u) & ~255u;

    m_renderer.CreateBuffer(nullptr, m_objectTransformCbStride * m_maxObjectCbCount, &m_objectTransformCB);
    m_renderer.CreateBuffer(nullptr, m_objectTransformCbStride * m_maxObjectCbCount, &m_shadowObjectTransformCB);
    m_renderer.CreateBuffer(nullptr, m_materialCbStride * m_maxObjectCbCount, &m_materialCB);
    m_renderer.CreateBuffer(nullptr, sizeof(GeometryFrameConstants), &m_geometryFrameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LightingFrameConstants), &m_frameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LocalLightConstants), &m_localLightsCB);
    m_renderer.CreateBuffer(nullptr, sizeof(RainProxyFrameConstants), &m_rainProxyFrameCB);
    m_renderer.CreateBuffer(nullptr, m_shadowFrameCbStride * ShadowCascadeCount, &m_shadowFrameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(PostProcessConstants), &m_postProcessCB);

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

    // CSM shadow map Texture2DArray: typeless resource + per-slice DSV + array SRV.
    D3D12_RESOURCE_DESC shadowDesc{};
    shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width = ShadowMapResolution;
    shadowDesc.Height = ShadowMapResolution;
    shadowDesc.DepthOrArraySize = ShadowCascadeCount;
    shadowDesc.MipLevels = 1;
    shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.SampleDesc.Count = 1;
    shadowDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE shadowClear{};
    shadowClear.Format = DXGI_FORMAT_D32_FLOAT;
    shadowClear.DepthStencil.Depth = 1.0f;
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &shadowClear, IID_PPV_ARGS(&m_shadowMap)));
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = ShadowCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap)));
    for (UINT i = 0; i < ShadowCascadeCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.FirstArraySlice = i;
        m_shadowDsvHandles[i] = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart(),
            i,
            m_renderer.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
        m_renderer.GetDevice()->CreateDepthStencilView(m_shadowMap.Get(), &dsv, m_shadowDsvHandles[i]);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrv{};
    shadowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrv.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadowSrv.Texture2DArray.ArraySize = ShadowCascadeCount;
    shadowSrv.Texture2DArray.MipLevels = 1;
    auto shadowSrvCpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_renderer.GetSrvHeap()->GetCPUDescriptorHandleForHeapStart(), ShadowMapSrvIndex, m_renderer.GetSrvDescriptorSize());
    m_renderer.GetDevice()->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrv, shadowSrvCpu);
    // Lighting passes bind their descriptor table at the first GBuffer SRV (heap index 1),
    // so HLSL register t6 resolves to heap index 7. Keep an alias there for the CSM Texture2DArray.
    auto shadowSrvT6Cpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_renderer.GetSrvHeap()->GetCPUDescriptorHandleForHeapStart(), ShadowMapSrvIndex + 1, m_renderer.GetSrvDescriptorSize());
    m_renderer.GetDevice()->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrv, shadowSrvT6Cpu);

        if (!m_particles.Initialize(&m_renderer, SceneColorFormat))
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


namespace
{
    std::wstring RS_ToWidePath(const std::string& path)
    {
        return std::wstring(path.begin(), path.end());
    }

    std::string RS_ToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};

        const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
            return {};

        std::string result(static_cast<size_t>(required - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), required, nullptr, nullptr);
        return result;
    }

    std::string RS_ToUtf8(const wchar_t* text)
    {
        return text ? RS_ToUtf8(std::wstring(text)) : std::string{};
    }

    const char* RS_DxgiFormatName(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "DXGI_FORMAT_B8G8R8X8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "DXGI_FORMAT_R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R32G32_FLOAT: return "DXGI_FORMAT_R32G32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "DXGI_FORMAT_R32_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT: return "DXGI_FORMAT_R16G16_FLOAT";
        case DXGI_FORMAT_R16G16_UNORM: return "DXGI_FORMAT_R16G16_UNORM";
        case DXGI_FORMAT_R8G8_UNORM: return "DXGI_FORMAT_R8G8_UNORM";
        case DXGI_FORMAT_BC1_UNORM: return "DXGI_FORMAT_BC1_UNORM";
        case DXGI_FORMAT_BC2_UNORM: return "DXGI_FORMAT_BC2_UNORM";
        case DXGI_FORMAT_BC3_UNORM: return "DXGI_FORMAT_BC3_UNORM";
        case DXGI_FORMAT_BC4_UNORM: return "DXGI_FORMAT_BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM: return "DXGI_FORMAT_BC4_SNORM";
        case DXGI_FORMAT_BC5_UNORM: return "DXGI_FORMAT_BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM: return "DXGI_FORMAT_BC5_SNORM";
        case DXGI_FORMAT_BC6H_UF16: return "DXGI_FORMAT_BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16: return "DXGI_FORMAT_BC6H_SF16";
        case DXGI_FORMAT_BC7_UNORM: return "DXGI_FORMAT_BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "DXGI_FORMAT_BC7_UNORM_SRGB";
        default: return "DXGI_FORMAT_UNKNOWN_OR_OTHER";
        }
    }

    void RS_LogDDSHeader(const TextureLoader::DDSData& dds, bool includeResolvedFormat)
    {
        char header[512]{};
        std::snprintf(
            header,
            sizeof(header),
            "[IBL] DDS header: width=%u height=%u mipMapCount=%u flags=0x%08X caps=0x%08X caps2=0x%08X\n",
            dds.width,
            dds.height,
            dds.headerMipMapCount,
            dds.headerFlags,
            dds.headerCaps,
            dds.headerCaps2);
        OutputDebugStringA(header);

        char pixelFormat[512]{};
        std::snprintf(
            pixelFormat,
            sizeof(pixelFormat),
            "[IBL] DDS pixel format: flags=0x%08X fourCC=0x%08X rgbBitCount=%u rMask=0x%08X gMask=0x%08X bMask=0x%08X aMask=0x%08X\n",
            dds.pixelFormatFlags,
            dds.pixelFormatFourCC,
            dds.pixelFormatRGBBitCount,
            dds.pixelFormatRBitMask,
            dds.pixelFormatGBitMask,
            dds.pixelFormatBBitMask,
            dds.pixelFormatABitMask);
        OutputDebugStringA(pixelFormat);

        if (includeResolvedFormat)
        {
            std::string resolved = std::string("[IBL] DDS resolved format: ") + RS_DxgiFormatName(dds.format) + "\n";
            OutputDebugStringA(resolved.c_str());
        }
    }
}

bool RenderingSystem::TryLoadDDSTexture(
    const std::wstring& path,
    UINT srvIndex,
    bool requireCube,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload,
    const char* debugLabel)
{
    const bool isBrdf = (debugLabel != nullptr && std::strcmp(debugLabel, "BRDF LUT") == 0);
    OutputDebugStringW((std::wstring(isBrdf ? L"[IBL] BRDF LUT file found: " : L"[IBL] DDS file found: ") + path + L"\n").c_str());

    TextureLoader::DDSData dds{};
    if (!TextureLoader::LoadDDSFromFile(path, dds))
    {
        std::string reason = TextureLoader::GetLastError().empty() ? "unsupported DDS format" : TextureLoader::GetLastError();
        if (isBrdf)
        {
            RS_LogDDSHeader(dds, dds.format != DXGI_FORMAT_UNKNOWN);
            std::wstring msg = L"[IBL] BRDF LUT load failed: " + path + L" | reason=";
            msg += std::wstring(reason.begin(), reason.end());
            msg += L"\n";
            OutputDebugStringW(msg.c_str());
        }
        else
        {
            std::wstring msg = L"[IBL] DDS load failed: " + path + L" | ";
            msg += std::wstring(reason.begin(), reason.end());
            msg += L"\n";
            OutputDebugStringW(msg.c_str());
        }
        return false;
    }
    if (isBrdf)
        RS_LogDDSHeader(dds, true);

    {
        std::string details = isBrdf ? "[IBL] BRDF LUT DDS details: format=" : "[IBL] DDS details: format=";
        details += RS_DxgiFormatName(dds.format);
        details += " width=" + std::to_string(dds.width);
        details += " height=" + std::to_string(dds.height);
        details += " mips=" + std::to_string(dds.mipLevels);
        details += " arraySize=" + std::to_string(dds.arraySize);
        details += dds.isCubeMap ? " dimension=TextureCube\n" : " dimension=Texture2D\n";
        OutputDebugStringA(details.c_str());
    }

    if (requireCube && !dds.isCubeMap)
    {
        OutputDebugStringW((std::wstring(L"[IBL] DDS load failed: ") + path + L" | unsupported cubemap: expected TextureCube\n").c_str());
        return false;
    }
    if (!requireCube && dds.isCubeMap)
    {
        OutputDebugStringW((std::wstring(isBrdf ? L"[IBL] BRDF LUT load failed: " : L"[IBL] DDS load failed: ") + path + (isBrdf ? L" | reason=unsupported DDS dimension: expected Texture2D\n" : L" | unsupported DDS dimension: expected Texture2D\n")).c_str());
        return false;
    }
    if (!requireCube && dds.arraySize != 1)
    {
        OutputDebugStringW((std::wstring(isBrdf ? L"[IBL] BRDF LUT load failed: " : L"[IBL] DDS load failed: ") + path + (isBrdf ? L" | reason=unsupported DDS dimension: expected Texture2D arraySize=1\n" : L" | unsupported DDS dimension: expected Texture2D arraySize=1\n")).c_str());
        return false;
    }

    if (!TextureLoader::CreateTextureFromDDS(m_renderer.GetDevice(), m_renderer.GetCmdList(), dds, texture, upload))
    {
        std::string reason = TextureLoader::GetLastError().empty() ? "CreateCommittedResource failed" : TextureLoader::GetLastError();
        std::wstring msg = std::wstring(isBrdf ? L"[IBL] BRDF LUT load failed: " : L"[IBL] DDS GPU upload failed: ") + path + (isBrdf ? L" | reason=" : L" | ");
        msg += std::wstring(reason.begin(), reason.end());
        msg += L"\n";
        OutputDebugStringW(msg.c_str());
        return false;
    }

    if (!TextureLoader::CreateShaderResourceView(
        m_renderer.GetDevice(),
        texture.Get(),
        dds.format,
        dds.isCubeMap,
        dds.mipLevels,
        m_renderer.GetSrvCpuHandle(srvIndex)))
    {
        OutputDebugStringW((std::wstring(isBrdf ? L"[IBL] BRDF LUT load failed: " : L"[IBL] DDS SRV creation failed: ") + path + (isBrdf ? L" | reason=CreateShaderResourceView failed\n" : L" | CreateShaderResourceView failed\n")).c_str());
        return false;
    }

    OutputDebugStringW((std::wstring(isBrdf ? L"[IBL] BRDF LUT loaded successfully: " : L"[IBL] DDS loaded successfully: ") + path + L"\n").c_str());
    return true;
}

bool RenderingSystem::TryLoadLDRTexture2D(
    const std::wstring& path,
    UINT srvIndex,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload)
{
    TextureLoader::TextureData data{};
    if (!TextureLoader::LoadFromFile(path, data))
        return false;
    if (!TextureLoader::CreateTexture(m_renderer.GetDevice(), m_renderer.GetCmdList(), data, texture, upload))
        return false;

    TextureLoader::CreateShaderResourceView(
        m_renderer.GetDevice(),
        texture.Get(),
        data.format,
        false,
        1,
        m_renderer.GetSrvCpuHandle(srvIndex));
    return true;
}

bool RenderingSystem::CreateFallbackIBLTexture(
    UINT srvIndex,
    bool cube,
    const std::vector<uint8_t>& rgba,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& upload)
{
    if (cube)
    {
        // Fallback TextureCube keeps the IBL descriptor table valid when external assets are missing.
        TextureLoader::DDSData data{};
        data.width = 1;
        data.height = 1;
        data.arraySize = 6;
        data.mipLevels = 1;
        data.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        data.isCubeMap = true;
        data.pixels.reserve(6 * 4);
        for (UINT face = 0; face < 6; ++face)
            data.pixels.insert(data.pixels.end(), rgba.begin(), rgba.end());
        for (UINT face = 0; face < 6; ++face)
        {
            D3D12_SUBRESOURCE_DATA sub{};
            sub.pData = data.pixels.data() + face * 4;
            sub.RowPitch = 4;
            sub.SlicePitch = 4;
            data.subresources.push_back(sub);
        }

        if (!TextureLoader::CreateTextureFromDDS(m_renderer.GetDevice(), m_renderer.GetCmdList(), data, texture, upload))
            return false;
        TextureLoader::CreateShaderResourceView(m_renderer.GetDevice(), texture.Get(), data.format, true, 1, m_renderer.GetSrvCpuHandle(srvIndex));
        return true;
    }

    TextureLoader::TextureData data{};
    data.width = 1;
    data.height = 1;
    data.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    data.rowPitch = 4;
    data.pixels = rgba;
    if (!TextureLoader::CreateTexture(m_renderer.GetDevice(), m_renderer.GetCmdList(), data, texture, upload))
        return false;
    TextureLoader::CreateShaderResourceView(m_renderer.GetDevice(), texture.Get(), data.format, false, 1, m_renderer.GetSrvCpuHandle(srvIndex));
    return true;
}

void RenderingSystem::LoadIBLResources()
{
    const std::string exeDir = GetExeDir();
    auto firstExistingPath = [&](std::initializer_list<const char*> relPaths) -> std::wstring
    {
        for (const char* rel : relPaths)
        {
            const std::string full = exeDir + rel;
            if (std::filesystem::exists(full))
                return RS_ToWidePath(full);
        }
        return L"";
    };

    const std::wstring irradiancePath = firstExistingPath({
        "assets/ibl/IrradianceMap_BC6U.dds",
        "..\\assets\\ibl\\IrradianceMap_BC6U.dds",
        "..\\..\\assets\\ibl\\IrradianceMap_BC6U.dds",
        "..\\..\\..\\assets\\ibl\\IrradianceMap_BC6U.dds",
        "assets/ibl/irradiance.dds",
        "..\\assets\\ibl\\irradiance.dds",
        "..\\..\\assets\\ibl\\irradiance.dds",
        "..\\..\\..\\assets\\ibl\\irradiance.dds" });
    const std::wstring prefilterPath = firstExistingPath({
        "assets/ibl/PreFilteredEnvMap_BC6U.dds",
        "..\\assets\\ibl\\PreFilteredEnvMap_BC6U.dds",
        "..\\..\\assets\\ibl\\PreFilteredEnvMap_BC6U.dds",
        "..\\..\\..\\assets\\ibl\\PreFilteredEnvMap_BC6U.dds",
        "assets/ibl/prefilter.dds",
        "..\\assets\\ibl\\prefilter.dds",
        "..\\..\\assets\\ibl\\prefilter.dds",
        "..\\..\\..\\assets\\ibl\\prefilter.dds" });
    const std::wstring brdfDdsPath = firstExistingPath({
        "assets/ibl/IntegrationMap.dds",
        "..\\assets\\ibl\\IntegrationMap.dds",
        "..\\..\\assets\\ibl\\IntegrationMap.dds",
        "..\\..\\..\\assets\\ibl\\IntegrationMap.dds",
        "assets/ibl/brdf_lut.dds",
        "..\\assets\\ibl\\brdf_lut.dds",
        "..\\..\\assets\\ibl\\brdf_lut.dds",
        "..\\..\\..\\assets\\ibl\\brdf_lut.dds" });
    const std::wstring brdfPngPath = firstExistingPath({
        "assets/ibl/brdf_lut.png",
        "..\\assets\\ibl\\brdf_lut.png",
        "..\\..\\assets\\ibl\\brdf_lut.png",
        "..\\..\\..\\assets\\ibl\\brdf_lut.png" });

    bool irradianceLoaded = false;
    bool prefilterLoaded = false;
    bool brdfLoaded = false;

    if (irradiancePath.empty())
        OutputDebugStringA("[IBL] file not found: assets/ibl/IrradianceMap_BC6U.dds\n");
    if (prefilterPath.empty())
        OutputDebugStringA("[IBL] file not found: assets/ibl/PreFilteredEnvMap_BC6U.dds\n");
    if (brdfDdsPath.empty())
        OutputDebugStringA("[IBL] file not found: assets/ibl/IntegrationMap.dds\n");

    try
    {
        m_renderer.BeginUploadCommands();

        if (!irradiancePath.empty())
            irradianceLoaded = TryLoadDDSTexture(irradiancePath, IrradianceMapSrvIndex, true, m_irradianceMap, m_irradianceMapUpload);
        if (irradianceLoaded)
            OutputDebugStringA("[IBL] IrradianceMap_BC6U.dds loaded successfully\n");
        if (!prefilterPath.empty())
            prefilterLoaded = TryLoadDDSTexture(prefilterPath, PrefilterMapSrvIndex, true, m_prefilterMap, m_prefilterMapUpload);
        if (prefilterLoaded)
            OutputDebugStringA("[IBL] PreFilteredEnvMap_BC6U.dds loaded successfully\n");
        if (!brdfDdsPath.empty())
            brdfLoaded = TryLoadDDSTexture(brdfDdsPath, BrdfLutSrvIndex, false, m_brdfLut, m_brdfLutUpload, "BRDF LUT");
        if (brdfLoaded)
            OutputDebugStringA("[IBL] IntegrationMap.dds BRDF LUT loaded successfully\n");
        if (!brdfLoaded && !brdfPngPath.empty())
        {
            OutputDebugStringW((std::wstring(L"[IBL] BRDF LUT PNG fallback found: ") + brdfPngPath + L"\n").c_str());
            brdfLoaded = TryLoadLDRTexture2D(brdfPngPath, BrdfLutSrvIndex, m_brdfLut, m_brdfLutUpload);
            if (brdfLoaded)
                OutputDebugStringW((std::wstring(L"[IBL] BRDF LUT loaded successfully from PNG fallback: ") + brdfPngPath + L"\n").c_str());
        }

        if (!irradianceLoaded)
        {
            OutputDebugStringA("[IBL] Failed to load assets/ibl/IrradianceMap_BC6U.dds or irradiance.dds; using black fallback diffuse IBL cube.\n");
            CreateFallbackIBLTexture(IrradianceMapSrvIndex, true, { 0, 0, 0, 255 }, m_irradianceMap, m_irradianceMapUpload);
        }
        if (!prefilterLoaded)
        {
            OutputDebugStringA("[IBL] Failed to load assets/ibl/PreFilteredEnvMap_BC6U.dds or prefilter.dds; using black fallback specular IBL cube.\n");
            CreateFallbackIBLTexture(PrefilterMapSrvIndex, true, { 0, 0, 0, 255 }, m_prefilterMap, m_prefilterMapUpload);
        }
        if (!brdfLoaded)
        {
            OutputDebugStringA("[IBL] WARNING: using neutral fallback BRDF LUT. Failed to load assets/ibl/IntegrationMap.dds, brdf_lut.dds or brdf_lut.png.\n");
            CreateFallbackIBLTexture(BrdfLutSrvIndex, false, { 255, 0, 0, 255 }, m_brdfLut, m_brdfLutUpload);
        }

        m_renderer.EndUploadCommands();
    }
    catch (...)
    {
        OutputDebugStringA("[IBL] Exception while loading IBL resources; renderer initialization will continue without crashing.\n");
    }
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

    OutputDebugStringA("[SceneSwitch][Sponza] LoadObj failed for all fallback candidates; trying optional PBR model fallbacks\n");
    if (TryLoadPBRModelWithFallbacks())
        return true;

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


bool RenderingSystem::TryLoadPBRModelWithFallbacks()
{
    const std::string exeDir = GetExeDir();
    const char* modelRelPaths[] =
    {
        "PBR_models/Cerberus_by_Andrew_Maximov/Cerberus_LP.obj",
        "PBR_models/wood_root/Asset_wood_root_M_rkswd_LOD0.obj",
        "model.obj",
    };
    const char* prefixes[] =
    {
        "assets/",
        "../assets/",
        "../../assets/",
        "../../../assets/",
    };

    for (const char* modelRel : modelRelPaths)
    {
        for (const char* prefix : prefixes)
        {
            const std::string rel = std::string(prefix) + modelRel;
            const std::string fullPath = exeDir + rel;
            if (!std::filesystem::exists(fullPath))
                continue;

            std::string msg = std::string("[PBR] Loading OBJ: ") + fullPath + "\n";
            OutputDebugStringA(msg.c_str());
            if (m_renderer.LoadObj(fullPath))
            {
                msg = std::string("[PBR] Loaded PBR OBJ: ") + fullPath + "\n";
                OutputDebugStringA(msg.c_str());
                return true;
            }

            msg = std::string("[PBR] Found but failed to load OBJ: ") + fullPath + "\n";
            OutputDebugStringA(msg.c_str());
        }
    }

    OutputDebugStringA("[PBR] No OBJ model found. Expected: assets/PBR_models/Cerberus_by_Andrew_Maximov/Cerberus_LP.obj or assets/PBR_models/wood_root/Asset_wood_root_M_rkswd_LOD0.obj\n");
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
    // Brighter baseline for CSM debugging: the previous values made Sponza too dark
    // when the first shadow pass over-shadowed the directional light.
    m_ambientColor = XMFLOAT4(0.26f, 0.26f, 0.29f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.20f, -1.0f, 0.10f);
    m_directionalLightColor = XMFLOAT3(0.98f, 0.99f, 1.00f);
    m_directionalLightIntensity = 2.40f;
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


void RenderingSystem::ApplyPerlinPlaneSceneSettings()
{
    m_cameraPos = XMFLOAT3(0.0f, 110.0f, -240.0f);
    m_yaw = 0.0f;
    m_pitch = -0.28f;
    m_moveSpeed = 220.0f;
    m_tessMinFactor = 8.0f;
    m_tessMaxFactor = 16.0f;
    m_tessMinDistance = 20.0f;
    m_tessMaxDistance = 500.0f;
    m_ambientColor = XMFLOAT4(0.18f, 0.20f, 0.24f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.18f, -1.0f, 0.15f);
    m_directionalLightColor = XMFLOAT3(0.95f, 0.98f, 1.00f);
    m_directionalLightIntensity = 1.8f;
}

void RenderingSystem::ApplyAlphaTestShadowSceneSettings()
{
    m_cameraPos = XMFLOAT3(0.0f, 90.0f, -210.0f);
    m_yaw = 0.0f;
    m_pitch = -0.33f;
    m_moveSpeed = 180.0f;
    m_tessMinFactor = 1.0f;
    m_tessMaxFactor = 1.0f;
    m_tessMinDistance = 5.0f;
    m_tessMaxDistance = 80.0f;
    m_ambientColor = XMFLOAT4(0.20f, 0.21f, 0.23f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.55f, -1.0f, 0.35f);
    m_directionalLightColor = XMFLOAT3(1.0f, 0.98f, 0.92f);
    m_directionalLightIntensity = 2.35f;
    m_enableShadows = 1;
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
    m_forceMirrorMaterial = false;
    m_showIBLSkybox = false;
    m_iblDiffuseStrength = 1.0f;
    m_iblSpecularStrength = 1.0f;
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
    m_forceMirrorMaterial = false;
    m_showIBLSkybox = false;
    m_iblDiffuseStrength = 1.0f;
    m_iblSpecularStrength = 1.0f;
    m_renderMainSceneModel = false;
    m_useTessellationForScene = false;
    m_enableFallingLights = false;
    m_enableGroundPlane = true;
    m_debugStrongDisplacement = 0;
    m_geometryDebugMode = 0;
    m_sceneObjectCount = 1000;
    m_massPlacementMode = MassPlacementMode::Grid;
    m_showCullingDebugGrid = m_enableCulling;
    m_billboardInitAttempted = false;
    m_billboardReady = false;
    m_billboardTextureSrv = -1;
    m_billboardMaterialSrvBase = -1;
    m_billboardVertexBuffer.Reset();
    m_billboardIndexBuffer.Reset();
    m_billboardVbView = {};
    m_billboardIbView = {};
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
    EnsureBillboardResources();

    m_particlesReinitRequested = false;
    CreateOrResizeSceneColorResources(m_renderer.GetWidth(), m_renderer.GetHeight());
    UpdateViewMatrix();
    UpdateWindowTitle();
    m_renderer.WaitForIdle();
    LogSceneState("[SceneSwitch][Dirty] EndState");
    OutputDebugStringA("[SceneSwitch][Dirty] End\n");
    return true;
}


bool RenderingSystem::SwitchToPerlinPlaneScene()
{
    if (m_activeSceneKind == DemoSceneKind::PerlinPlane)
        return true;

    m_renderer.WaitForIdle();
    m_activeSceneKind = DemoSceneKind::PerlinPlane;
    m_forceMirrorMaterial = false;
    m_showIBLSkybox = false;
    m_iblDiffuseStrength = 1.0f;
    m_iblSpecularStrength = 1.0f;
    m_renderMainSceneModel = false;
    m_useTessellationForScene = true;
    m_enableFallingLights = false;
    m_enableGroundPlane = false;
    m_showCullingDebugGrid = false;
    m_debugStrongDisplacement = 1;
    m_geometryDebugMode = 0;

    if (!m_renderer.LoadPrimitivePlaneScene())
        return false;

    BuildSingleMainSceneObject();
    ApplyPerlinPlaneSceneSettings();
    SetupSceneLights();
    m_particlesReinitRequested = false;
    UpdateViewMatrix();
    UpdateWindowTitle();
    m_renderer.WaitForIdle();
    return true;
}

bool RenderingSystem::SwitchToAlphaTestShadowScene()
{
    if (m_activeSceneKind == DemoSceneKind::AlphaTestShadow)
        return true;

    m_renderer.WaitForIdle();
    m_activeSceneKind = DemoSceneKind::AlphaTestShadow;
    m_forceMirrorMaterial = false;
    m_showIBLSkybox = false;
    m_iblDiffuseStrength = 1.0f;
    m_iblSpecularStrength = 1.0f;
    m_renderMainSceneModel = false;
    m_useTessellationForScene = false;
    m_enableFallingLights = false;
    m_enableGroundPlane = false;
    m_showCullingDebugGrid = false;
    m_debugLineVertices.clear();
    m_debugStrongDisplacement = 0;
    m_geometryDebugMode = 0;

    if (!m_renderer.LoadAlphaTestShadowScene())
        return false;

    BuildSingleMainSceneObject();
    ApplyAlphaTestShadowSceneSettings();
    SetupSceneLights();
    m_particlesReinitRequested = false;
    UpdateViewMatrix();
    UpdateWindowTitle();
    m_renderer.WaitForIdle();
    return true;
}

bool RenderingSystem::SwitchToPBRModelScene()
{
    if (m_activeSceneKind == DemoSceneKind::PBRModel)
        return true;

    OutputDebugStringA("[SceneSwitch][PBR] Begin\n");
    m_renderer.WaitForIdle();

    auto findRelativeAsset = [](const std::filesystem::path& relativePath) -> std::string
    {
        const std::filesystem::path prefixes[] =
        {
            std::filesystem::path("."),
            std::filesystem::path(".."),
            std::filesystem::path("..") / "..",
            std::filesystem::path("..") / ".." / ".."
        };

        for (const auto& prefix : prefixes)
        {
            const std::filesystem::path candidate = prefix / relativePath;
            if (std::filesystem::exists(candidate))
                return candidate.generic_string();
        }
        return {};
    };

    const std::string cerberusPath = findRelativeAsset(
        std::filesystem::path("assets") / "PBR_models" / "Cerberus_by_Andrew_Maximov" / "Cerberus_LP.obj");
    const std::string woodRootPath = findRelativeAsset(
        std::filesystem::path("assets") / "PBR_models" / "wood_root" / "Asset_wood_root_M_rkswd_LOD0.obj");

    if (cerberusPath.empty() && woodRootPath.empty())
    {
        OutputDebugStringA("[PBR] No PBR demo OBJ models found.\n");
        return false;
    }
    if (cerberusPath.empty())
        OutputDebugStringA("[PBR] Warning: Cerberus OBJ not found; loading wood_root only.\n");
    if (woodRootPath.empty())
        OutputDebugStringA("[PBR] Warning: wood_root OBJ not found; loading Cerberus only.\n");

    m_activeSceneKind = DemoSceneKind::PBRModel;
    m_forceMirrorMaterial = false;
    m_showIBLSkybox = true;
    m_iblDiffuseStrength = 1.0f;
    m_iblSpecularStrength = 2.0f;
    m_renderMainSceneModel = true;
    m_useTessellationForScene = false;
    m_enableFallingLights = false;
    m_enableGroundPlane = false;
    m_sceneObjects.clear();
    m_showCullingDebugGrid = false;
    m_billboardReady = false;
    m_billboardInitAttempted = false;
    m_billboardTextureSrv = -1;
    m_billboardMaterialSrvBase = -1;
    m_billboardVertexBuffer.Reset();
    m_billboardIndexBuffer.Reset();
    m_billboardVbView = {};
    m_billboardIbView = {};
    m_geometryDebugMode = 0;
    m_debugStrongDisplacement = 0;
    m_debugLineVertices.clear();
    m_activePointLightsForGpu.clear();
    m_activePointLights = 0;
    m_rainDebugStats = RainDebugStats{};
    m_particlesReinitRequested = false;

    if (!m_renderer.LoadPBRDemoModels(cerberusPath, woodRootPath))
        return false;

    BuildSingleMainSceneObject();
    m_cameraPos = XMFLOAT3(0.0f, 55.0f, -180.0f);
    m_yaw = 0.0f;
    m_pitch = -0.10f;
    m_moveSpeed = 120.0f;
    m_tessMinFactor = 1.0f;
    m_tessMaxFactor = 1.0f;
    m_tessMinDistance = 5.0f;
    m_tessMaxDistance = 80.0f;
    m_ambientColor = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    m_directionalLightDirection = XMFLOAT3(0.35f, -1.0f, 0.25f);
    m_directionalLightColor = XMFLOAT3(1.0f, 0.98f, 0.94f);
    m_directionalLightIntensity = 3.0f;
    m_enableShadows = 1;

    SetupSceneLights();
    UpdateViewMatrix();
    UpdateWindowTitle();
    m_renderer.WaitForIdle();

    if (!cerberusPath.empty() && !woodRootPath.empty())
        OutputDebugStringA("[PBR] Loaded demo scene: Cerberus + wood_root\n");
    else
        OutputDebugStringA("[PBR] Loaded partial PBR demo scene\n");
    OutputDebugStringA("[PBR] Cerberus textures: A/M/N/R\n");
    const auto& loadedMaterials = m_renderer.GetMaterials();
    if (!loadedMaterials.empty())
    {
        const auto& cerberusMaterial = loadedMaterials.front();
        char materialDebug[512];
        sprintf_s(
            materialDebug,
            "[PBR] Material debug:\n"
            "Cerberus has albedo map = %s\n"
            "Cerberus has metallic map = %s\n"
            "Cerberus has normal map = %s\n"
            "Cerberus has roughness map = %s\n",
            cerberusMaterial.hasDiffuseMap ? "true" : "false",
            cerberusMaterial.hasMetallicMap ? "true" : "false",
            cerberusMaterial.hasNormalMap ? "true" : "false",
            cerberusMaterial.hasRoughnessMap ? "true" : "false");
        OutputDebugStringA(materialDebug);
    }
    OutputDebugStringA("[PBR] wood_root textures: Albedo/Normal/Roughness, metallic fallback=0\n");
    OutputDebugStringA("[SceneSwitch][PBR] End\n");
    return true;
}

void RenderingSystem::UpdateWindowTitle() const
{
    if (!m_hwnd)
        return;

    const wchar_t* postLabel = L"Scanner+VCR";
    if (m_postProcessMode == 0) postLabel = L"Off";
    else if (m_postProcessMode == 1) postLabel = L"Scanner";
    else if (m_postProcessMode == 2) postLabel = L"VCR";
    else if (m_postProcessMode == 4) postLabel = L"Nausea";
    else if (m_postProcessMode == 5) postLabel = L"Nausea+VCR";
    const wchar_t* vcrLabel = m_vcrStrongMode ? L"Strong" : L"Normal";
    const wchar_t* nauseaLabel = m_nauseaStrongMode ? L"Strong" : L"Normal";
    const wchar_t* toneLabel = L"ACES";
    if (m_toneMapperMode == 0) toneLabel = L"None";
    else if (m_toneMapperMode == 1) toneLabel = L"Reinhard";
    else if (m_toneMapperMode == 2) toneLabel = L"Exposure";

    if (m_activeSceneKind == DemoSceneKind::Sponza)
    {
        wchar_t title[512];
        swprintf_s(
            title,
            L"[SPONZA] Deferred Renderer | Shadows:%s | Debug:%u | PostFX:%s | VCR:%s | Nausea:%s | ToneMap:%s | Exposure:%.2f | Gamma:%.2f | Particles:%u %s %s",
            m_enableShadows ? L"ON" : L"OFF",
            m_debugMode,
            postLabel,
            vcrLabel,
            nauseaLabel,
            toneLabel,
            m_exposure,
            m_gamma,
            m_particles.GetAliveCountForDraw(),
            m_particles.IsEnabled() ? L"ON" : L"OFF",
            m_particles.IsSortEnabled() ? L"SORT" : L"NOSORT");
        SetWindowTextW(m_hwnd, title);
    }
    else if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        wchar_t title[512];

        const wchar_t* modeLabel = L"[NO CULLING]";
        if (m_enableCulling)
            modeLabel = m_useOctreeMode ? L"[OCTREE + GRID]" : L"[FRUSTUM + GRID]";

        swprintf_s(
            title,
            L"%s INSTANCING: %u / %u visible | cubes:%u billboards:%u | PostFX:%s | VCR:%s | Nausea:%s | ToneMap:%s | Exposure:%.2f | Gamma:%.2f",
            modeLabel,
            m_visibleObjectCount,
            m_sceneObjectCount,
            m_cubeDrawCount,
            m_billboardDrawCount,
            postLabel,
            vcrLabel,
            nauseaLabel,
            toneLabel,
            m_exposure,
            m_gamma);

        SetWindowTextW(m_hwnd, title);
    }
    else if (m_activeSceneKind == DemoSceneKind::PerlinPlane)
    {
        wchar_t title[512];
        swprintf_s(title, L"[PERLIN PLANE] seed=%.0f | PostFX:%s | VCR:%s | Nausea:%s | ToneMap:%s | Exposure:%.2f | Gamma:%.2f",
            m_perlinNoiseSeed, postLabel, vcrLabel, nauseaLabel, toneLabel, m_exposure, m_gamma);
        SetWindowTextW(m_hwnd, title);
    }
    else if (m_activeSceneKind == DemoSceneKind::PBRModel)
    {
        wchar_t title[512];
        swprintf_s(title, L"[PBR MODEL] B scene | Shadows:%s | Debug:%u | Mirror:%s | Skybox:%s | IBLspec:%.2f | PostFX:%s | ToneMap:%s | Exposure:%.2f | Gamma:%.2f",
            m_enableShadows ? L"ON" : L"OFF",
            m_debugMode,
            m_forceMirrorMaterial ? L"ON" : L"OFF",
            m_showIBLSkybox ? L"ON" : L"OFF",
            m_iblSpecularStrength,
            postLabel,
            toneLabel,
            m_exposure,
            m_gamma);
        SetWindowTextW(m_hwnd, title);
    }
    else
    {
        wchar_t title[512];
        swprintf_s(title, L"[ALPHA SHADOW TEST] V scene | Shadows:%s | Debug:%u | PostFX:%s | VCR:%s | Nausea:%s | ToneMap:%s | Exposure:%.2f | Gamma:%.2f",
            m_enableShadows ? L"ON" : L"OFF",
            m_debugMode,
            postLabel,
            vcrLabel,
            nauseaLabel,
            toneLabel,
            m_exposure,
            m_gamma);
        SetWindowTextW(m_hwnd, title);
    }
}

bool RenderingSystem::ShouldRunParticleFountain() const
{
    return m_activeSceneKind == DemoSceneKind::Sponza;
}

XMFLOAT3 RenderingSystem::GetParticleEmitterPosition() const
{

    return XMFLOAT3(0.0f, 24.0f, -85.0f);
}

ParticleSystemGPU::FountainSettings RenderingSystem::GetParticleFountainSettings() const
{
    ParticleSystemGPU::FountainSettings settings{};

    if (!ShouldRunParticleFountain())
    {
        settings.EmitPerFrame = 0;
        return settings;
    }

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


void RenderingSystem::RequestSceneSwitch(DemoSceneKind scene)
{
    if (scene == m_activeSceneKind)
        return;
    if (m_pendingSceneSwitch.has_value() && *m_pendingSceneSwitch == scene)
        return;

    if (scene == DemoSceneKind::Sponza)
        OutputDebugStringA("[SceneSwitch] Request Sponza\n");
    else if (scene == DemoSceneKind::PerlinPlane)
        OutputDebugStringA("[SceneSwitch] Request PerlinPlane\n");
    else if (scene == DemoSceneKind::AlphaTestShadow)
        OutputDebugStringA("[SceneSwitch] Request AlphaTestShadow\n");
    else if (scene == DemoSceneKind::PBRModel)
        OutputDebugStringA("[SceneSwitch] Request PBRModel\n");
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
    if (requested == DemoSceneKind::PerlinPlane)
        return SwitchToPerlinPlaneScene();
    if (requested == DemoSceneKind::AlphaTestShadow)
        return SwitchToAlphaTestShadowScene();
    if (requested == DemoSceneKind::PBRModel)
        return SwitchToPBRModelScene();

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

bool RenderingSystem::EnsureBillboardResources()
{
    if (m_billboardReady)
        return true;

    if (m_billboardInitAttempted)
        return false;

    m_billboardInitAttempted = true;

    const Vertex quadVertices[] =
    {
        { XMFLOAT3(-0.5f, -0.5f, 0.0f), XMFLOAT3(0, 0, -1), XMFLOAT2(0, 1), XMFLOAT3(1, 0, 0), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3(-0.5f,  0.5f, 0.0f), XMFLOAT3(0, 0, -1), XMFLOAT2(0, 0), XMFLOAT3(1, 0, 0), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3( 0.5f,  0.5f, 0.0f), XMFLOAT3(0, 0, -1), XMFLOAT2(1, 0), XMFLOAT3(1, 0, 0), XMFLOAT3(0, 1, 0) },
        { XMFLOAT3( 0.5f, -0.5f, 0.0f), XMFLOAT3(0, 0, -1), XMFLOAT2(1, 1), XMFLOAT3(1, 0, 0), XMFLOAT3(0, 1, 0) },
    };

    const UINT quadIndices[] =
    {
        0, 1, 2,
        0, 2, 3
    };

    m_renderer.CreateBuffer(quadVertices, sizeof(quadVertices), &m_billboardVertexBuffer);
    m_renderer.CreateBuffer(quadIndices, sizeof(quadIndices), &m_billboardIndexBuffer);

    if (!m_billboardVertexBuffer || !m_billboardIndexBuffer)
    {
        OutputDebugStringA("[Billboard] Failed to create billboard vertex/index buffers\n");
        return false;
    }

    m_billboardVbView.BufferLocation = m_billboardVertexBuffer->GetGPUVirtualAddress();
    m_billboardVbView.StrideInBytes = sizeof(Vertex);
    m_billboardVbView.SizeInBytes = sizeof(quadVertices);

    m_billboardIbView.BufferLocation = m_billboardIndexBuffer->GetGPUVirtualAddress();
    m_billboardIbView.Format = DXGI_FORMAT_R32_UINT;
    m_billboardIbView.SizeInBytes = sizeof(quadIndices);

    if (m_billboardTextureSrv < 0 || m_billboardMaterialSrvBase < 0)
    {
        const std::filesystem::path exeDir(GetExeDir());
        const std::filesystem::path candidates[] =
        {
            exeDir / "assets" / "billboard.png",
            exeDir / ".." / "assets" / "billboard.png",
            exeDir / ".." / ".." / "assets" / "billboard.png",
            exeDir / ".." / ".." / ".." / "assets" / "billboard.png",
            std::filesystem::path("assets") / "billboard.png",
            std::filesystem::path("..") / "assets" / "billboard.png",
            std::filesystem::path("..") / ".." / "assets" / "billboard.png",
            std::filesystem::path("..") / ".." / ".." / "assets" / "billboard.png"
        };

        for (const std::filesystem::path& candidate : candidates)
        {
            const std::wstring texPath = candidate.wstring();
            std::string msg = "[Billboard] Trying texture path: " + RS_ToUtf8(texPath) + "\n";
            OutputDebugStringA(msg.c_str());

            m_billboardTextureSrv = m_renderer.LoadTextureToSrv(texPath);
            if (m_billboardTextureSrv >= 0)
            {
                m_billboardMaterialSrvBase = m_renderer.CreateMaterialSrvBlockFromAlbedo(static_cast<UINT>(m_billboardTextureSrv));
                if (m_billboardMaterialSrvBase >= 0)
                    break;

                OutputDebugStringA("[Billboard] Failed to create six-SRV material descriptor table. Billboard LOD disabled.\n");
                m_billboardTextureSrv = -1;
            }
        }
    }

    if (m_billboardTextureSrv < 0 || m_billboardMaterialSrvBase < 0)
    {
        OutputDebugStringA("[Billboard] billboard.png was not found. Billboard LOD disabled.\n");
        m_billboardReady = false;
        return false;
    }

    m_billboardReady = true;
    OutputDebugStringA("[Billboard] Billboard resources ready\n");
    return true;
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
    HRESULT mapHr = m_debugLineVertexBuffer->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[DebugLines] Failed to map debug line vertex buffer; skipping upload.\n");
        return;
    }
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
    HRESULT mapHr = m_debugLineCB->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[DebugLines] Failed to map debug line CB; skipping pass.\n");
        return;
    }
    memcpy(mapped, &cb, sizeof(cb));
    m_debugLineCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_sceneColorRtv;
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
    if (key == 'T')
    {
        m_postProcessMode = (m_postProcessMode + 1) % 6;
        UpdateWindowTitle();
        return;
    }
    if (key == 'Y')
    {
        m_vcrStrongMode = !m_vcrStrongMode;
        UpdateWindowTitle();
        return;
    }
    if (key == 'U')
    {
        m_nauseaStrongMode = !m_nauseaStrongMode;
        UpdateWindowTitle();
        return;
    }
    if (key == 'G')
    {
        m_toneMapperMode = (m_toneMapperMode + 1) % 4;
        UpdateWindowTitle();
        return;
    }
    if (key == VK_OEM_PLUS)
    {
        if (m_activeSceneKind == DemoSceneKind::PBRModel)
        {
            m_iblSpecularStrength = std::clamp(m_iblSpecularStrength + 0.25f, 0.0f, 3.0f);
            char msg[128];
            sprintf_s(msg, "[PBR] IBL specular strength = %.2f\n", m_iblSpecularStrength);
            OutputDebugStringA(msg);
        }
        else
        {
            m_exposure = std::clamp(m_exposure * 1.1f, 0.05f, 8.0f);
        }
        UpdateWindowTitle();
        return;
    }
    if (key == VK_OEM_MINUS)
    {
        if (m_activeSceneKind == DemoSceneKind::PBRModel)
        {
            m_iblSpecularStrength = std::clamp(m_iblSpecularStrength - 0.25f, 0.0f, 3.0f);
            char msg[128];
            sprintf_s(msg, "[PBR] IBL specular strength = %.2f\n", m_iblSpecularStrength);
            OutputDebugStringA(msg);
        }
        else
        {
            m_exposure = std::clamp(m_exposure / 1.1f, 0.05f, 8.0f);
        }
        UpdateWindowTitle();
        return;
    }
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
    if (key == 'C')
    {
        RequestSceneSwitch(DemoSceneKind::PerlinPlane);
        return;
    }
    if (key == 'V')
    {
        RequestSceneSwitch(DemoSceneKind::AlphaTestShadow);
        return;
    }
    if (key == 'B')
    {
        RequestSceneSwitch(DemoSceneKind::PBRModel);
        return;
    }
    if (key == 'M' && m_activeSceneKind == DemoSceneKind::PBRModel)
    {
        m_forceMirrorMaterial = !m_forceMirrorMaterial;
        OutputDebugStringA(m_forceMirrorMaterial ? "[PBR] Mirror material override ON\n" : "[PBR] Mirror material override OFF\n");
        UpdateWindowTitle();
        return;
    }

    if (key == 'W') m_moveForward = true;
    if (key == 'S') m_moveBackward = true;
    if (key == 'A') m_moveLeft = true;
    if (key == 'D') m_moveRight = true;
    if (key == 'Q') m_moveUp = true;
    if (key == 'E') m_moveDown = true;
    if (key == 'P' && ShouldRunParticleFountain())
    {
        m_particles.SetEnabled(!m_particles.IsEnabled());
        UpdateWindowTitle();
        return;
    }
    if (key == 'O' && ShouldRunParticleFountain())
    {
        m_particles.SetSortEnabled(!m_particles.IsSortEnabled());
        UpdateWindowTitle();
        return;
    }
    if (key == 'I' && ShouldRunParticleFountain())
    {
        m_particlesReinitRequested = true;
        UpdateWindowTitle();
        return;
    }
    if (key == 'L' && m_activeSceneKind == DemoSceneKind::PerlinPlane)
    {
        std::mt19937 rng(static_cast<uint32_t>(std::random_device{}()));
        std::uniform_int_distribution<int> dist(1, 1000000);
        m_perlinNoiseSeed = static_cast<float>(dist(rng));
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
    // 8=CSM shadow mask
    // 9=CSM cascade colors
    // 10=IBL diffuse only
    // 11=IBL specular/reflections only
    // 12=sampled prefiltered environment only
    // 13=material buffer (metallic, roughness, AO)
    if (key >= '0' && key <= '9')
    {
        m_debugMode = static_cast<UINT>(key - '0');
        UpdateWindowTitle();
    }
    if (key == VK_F10) { m_debugMode = 10; UpdateWindowTitle(); }
    if (key == VK_F11) { m_debugMode = 11; UpdateWindowTitle(); }
    if (key == VK_F12) { m_debugMode = 12; UpdateWindowTitle(); }
    if (key == 'N' && m_activeSceneKind == DemoSceneKind::PBRModel) { m_debugMode = 13; UpdateWindowTitle(); }

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

    // H toggles CSM sampling. Useful for checking that the base directional light
    // is visible before/after applying PCF shadows.
    if (key == 'H')
    {
        m_enableShadows = m_enableShadows ? 0u : 1u;
        UpdateWindowTitle();
    }

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
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);

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
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[3] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 16, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE),
            CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)
        };
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            3,
            samplers,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingDirectionalRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(1); // LocalLightConstants

        CD3DX12_STATIC_SAMPLER_DESC samplers[3] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 16, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE),
            CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)
        };
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            3,
            params,
            3,
            samplers,
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

    {
        CD3DX12_DESCRIPTOR_RANGE diffuseSrvRange;
        diffuseSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0); // ObjectTransformConstants
        params[1].InitAsConstantBufferView(1); // per-cascade ShadowViewProj
        params[2].InitAsConstantBufferView(2); // MaterialConstants for alpha-tested shadows
        params[3].InitAsDescriptorTable(1, &diffuseSrvRange, D3D12_SHADER_VISIBILITY_PIXEL);

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
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_shadowRS)));
    }
    {
        CD3DX12_DESCRIPTOR_RANGE sceneColorRange;
        sceneColorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_DESCRIPTOR_RANGE gbufferRange;
        gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1);
        CD3DX12_DESCRIPTOR_RANGE depthRange;
        depthRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &sceneColorRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsDescriptorTable(1, &gbufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[3].InitAsDescriptorTable(1, &depthRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)
        };

        CD3DX12_ROOT_SIGNATURE_DESC desc(4, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_postProcessRS)));
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
            std::string fileUtf8 = RS_ToUtf8(file);
            std::string lastPathUtf8 = RS_ToUtf8(lastPath);
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
    compileShader(L"ShadowMap.hlsl", "VSMain", "vs_5_0", m_shadowVS);
    compileShader(L"PostProcess.hlsl", "VSMain", "vs_5_0", m_postProcessVS);
    ComPtr<ID3DBlob> postProcessPS;
    compileShader(L"PostProcess.hlsl", "PSMain", "ps_5_0", postProcessPS);

    if (!m_geoVS || !m_geoHS || !m_geoDS || !m_geoPS || !m_geoNoTessVS || !m_geoNoTessPS || !m_lightFullscreenVS || !m_rainProxyVS || !m_debugLineVS || !m_shadowVS || !m_postProcessVS)
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

    {
        ComPtr<ID3DBlob> shadowPS;
        compileShader(L"ShadowMap.hlsl", "PSMain", "ps_5_0", shadowPS);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowDesc{};
        shadowDesc.InputLayout = { geoLayout, _countof(geoLayout) };
        shadowDesc.pRootSignature = m_shadowRS.Get();
        shadowDesc.VS = { m_shadowVS->GetBufferPointer(), m_shadowVS->GetBufferSize() };
        shadowDesc.PS = { shadowPS->GetBufferPointer(), shadowPS->GetBufferSize() };
        shadowDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        shadowDesc.RasterizerState.DepthBias = m_shadowDepthBias;
        shadowDesc.RasterizerState.SlopeScaledDepthBias = m_shadowSlopeBias;
        shadowDesc.RasterizerState.DepthBiasClamp = 0.01f;
        shadowDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        shadowDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        shadowDesc.DepthStencilState.DepthEnable = TRUE;
        shadowDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        shadowDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        shadowDesc.SampleMask = UINT_MAX;
        shadowDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        shadowDesc.NumRenderTargets = 0;
        shadowDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        shadowDesc.SampleDesc.Count = 1;
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&shadowDesc, IID_PPV_ARGS(&m_shadowPSO)));
    }

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
        desc.RTVFormats[0] = SceneColorFormat;
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
        proxyDesc.RTVFormats[0] = SceneColorFormat;
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
        lineDesc.RTVFormats[0] = SceneColorFormat;
        lineDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        lineDesc.SampleDesc.Count = 1;
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&lineDesc, IID_PPV_ARGS(&m_debugLinePSO)));
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ppDesc{};
        ppDesc.InputLayout = { nullptr, 0 };
        ppDesc.pRootSignature = m_postProcessRS.Get();
        ppDesc.VS = { m_postProcessVS->GetBufferPointer(), m_postProcessVS->GetBufferSize() };
        ppDesc.PS = { postProcessPS->GetBufferPointer(), postProcessPS->GetBufferSize() };
        ppDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        ppDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        ppDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        ppDesc.DepthStencilState.DepthEnable = FALSE;
        ppDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ppDesc.SampleMask = UINT_MAX;
        ppDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ppDesc.NumRenderTargets = 1;
        ppDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        ppDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ppDesc.SampleDesc.Count = 1;
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&ppDesc, IID_PPV_ARGS(&m_postProcessPSO)));
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

    light.Range = RS_Lerp(m_rainLightRangeMin, m_rainLightRangeMax, m_rainUnitDist(m_rainRng));
    light.Intensity = RS_Lerp(m_rainLightIntensityMin, m_rainLightIntensityMax, m_rainUnitDist(m_rainRng));

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
    frame.UseProceduralDisplacement = (m_activeSceneKind == DemoSceneKind::PerlinPlane) ? 1u : 0u;
    frame.ProceduralNoiseSeed = m_perlinNoiseSeed;

    void* frameMapped = nullptr;
    HRESULT frameMapHr = m_geometryFrameCB->Map(0, nullptr, &frameMapped);
    if (FAILED(frameMapHr) || frameMapped == nullptr)
    {
        OutputDebugStringA("[GeometryPass] Failed to map geometry frame CB; skipping geometry pass.\n");
        return;
    }
    memcpy(frameMapped, &frame, sizeof(frame));
    m_geometryFrameCB->Unmap(0, nullptr);

    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();

    if (subsets.empty())
        return;

    void* transformMapped = nullptr;
    void* materialMapped = nullptr;
    HRESULT transformMapHr = m_objectTransformCB->Map(0, nullptr, &transformMapped);
    HRESULT materialMapHr = m_materialCB->Map(0, nullptr, &materialMapped);
    if (FAILED(transformMapHr) || transformMapped == nullptr || FAILED(materialMapHr) || materialMapped == nullptr)
    {
        OutputDebugStringA("[GeometryPass] Failed to map object/material CB; skipping geometry pass.\n");
        if (transformMapped)
            m_objectTransformCB->Unmap(0, nullptr);
        if (materialMapped)
            m_materialCB->Unmap(0, nullptr);
        return;
    }

    std::uint8_t* transformBase = reinterpret_cast<std::uint8_t*>(transformMapped);
    std::uint8_t* materialBase = reinterpret_cast<std::uint8_t*>(materialMapped);

    size_t drawIndex = 0;
    const bool drawMainModel = m_renderMainSceneModel || m_sceneObjects.empty();
    const size_t objectCount = drawMainModel ? 1 : m_sceneObjects.size();

    m_cubeDrawCount = 0;
    m_billboardDrawCount = 0;

    if (drawMainModel)
        m_visibleObjectCount = static_cast<UINT>(objectCount);

    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMVECTOR viewForward = XMVector3Normalize(view.r[2]);
    const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, viewForward));
    const XMVECTOR billboardUp = XMVector3Normalize(XMVector3Cross(viewForward, right));
    const bool billboardsAllowed = (m_activeSceneKind == DemoSceneKind::DirtyInstancing) && m_billboardReady;
    m_cubeDrawCount = 0;
    m_billboardDrawCount = 0;
    for (size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        if (!drawMainModel && !m_sceneObjects[objectIndex].Visible)
            continue;

        const SceneObject* objectPtr = drawMainModel ? nullptr : &m_sceneObjects[objectIndex];
        bool drawAsBillboard = false;
        if (billboardsAllowed && objectPtr != nullptr)
        {
            const XMVECTOR objectPos = XMLoadFloat3(&objectPtr->BoundsCenter);
            const XMVECTOR cameraPos = XMLoadFloat3(&m_cameraPos);
            const float distanceToCamera = XMVectorGetX(XMVector3Length(objectPos - cameraPos));
            drawAsBillboard = (distanceToCamera >= m_billboardSwitchDistance);
        }

        const size_t subsetCount = drawAsBillboard ? 1 : subsets.size();
        for (size_t subsetIndex = 0; subsetIndex < subsetCount; ++subsetIndex)
        {
            if (drawIndex >= m_maxObjectCbCount)
                break;

            const auto& s = subsets[(std::min)(subsetIndex, subsets.size() - 1)];

            ObjectTransformConstants transform{};
            if (drawMainModel)
            {
                const XMMATRIX identity = XMMatrixIdentity();
                XMStoreFloat4x4(&transform.World, XMMatrixTranspose(identity));
                XMStoreFloat4x4(&transform.WorldInvTranspose, XMMatrixTranspose(identity));
                transform.ColorTint = XMFLOAT4(1, 1, 1, 1);
            }
            else if (!drawAsBillboard)
            {
                const SceneObject& object = m_sceneObjects[objectIndex];
                transform.World = object.World;
                transform.WorldInvTranspose = object.WorldInvTranspose;
                transform.ColorTint = object.ColorTint;
            }
            else
            {
                XMFLOAT3 center = objectPtr->BoundsCenter;
                const float scale = objectPtr->BoundsRadius * 2.0f;
                XMMATRIX world = XMMatrixIdentity();
                world.r[0] = XMVectorScale(right, scale);
                world.r[1] = XMVectorScale(billboardUp, scale);
                world.r[2] = XMVectorScale(viewForward, scale);
                world.r[3] = XMVectorSet(center.x, center.y, center.z, 1.0f);
                XMStoreFloat4x4(&transform.World, XMMatrixTranspose(world));
                XMStoreFloat4x4(&transform.WorldInvTranspose, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
                transform.ColorTint = objectPtr->ColorTint;
            }

            MaterialConstants material{};
            material.MaterialDiffuse = XMFLOAT4(1, 1, 1, 1);
            material.MaterialSpecular = XMFLOAT4(1, 1, 1, 1);
            material.SpecularPower = 32.0f;
            material.HasTexture = 0;
            material.HasNormalMap = 0;
            material.HasDisplacementMap = 0;
            material.HasMetallicMap = 0;
            material.HasRoughnessMap = 0;
            material.HasAOMap = 0;
            material.DisplacementScale = 0.0f;
            material.DisplacementBias = 0.0f;

            UINT textureSrv = 0;
            if (!drawAsBillboard && s.materialIdx >= 0 && s.materialIdx < static_cast<int>(materials.size()))
            {
                const auto& mat = materials[s.materialIdx];
                material.MaterialDiffuse = mat.diffuse;
                material.MaterialSpecular = mat.specular;
                material.SpecularPower = mat.specPower;
                if (mat.diffuseSrvHeapIndex >= 0)
                {
                    material.HasTexture = mat.hasDiffuseMap ? 1 : 0;
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
                if (mat.metallicSrvHeapIndex >= 0 && mat.hasMetallicMap)
                {
                    material.HasMetallicMap = 1;
                }
                if (mat.roughnessSrvHeapIndex >= 0 && mat.hasRoughnessMap)
                {
                    material.HasRoughnessMap = 1;
                }
                if (mat.aoSrvHeapIndex >= 0 && mat.hasAOMap)
                {
                    material.HasAOMap = 1;
                }
            }
            if (drawAsBillboard)
            {
                material.HasTexture = 1;
                material.HasNormalMap = 0;
                material.HasDisplacementMap = 0;
                material.HasMetallicMap = 0;
                material.HasRoughnessMap = 0;
                material.HasAOMap = 0;
                textureSrv = static_cast<UINT>(m_billboardMaterialSrvBase);
            }

            const UINT transformOffset = static_cast<UINT>(drawIndex * m_objectTransformCbStride);
            const UINT materialOffset = static_cast<UINT>(drawIndex * m_materialCbStride);

            memcpy(transformBase + transformOffset, &transform, sizeof(transform));
            memcpy(materialBase + materialOffset, &material, sizeof(material));

            cmdList->SetGraphicsRootConstantBufferView(0, m_objectTransformCB->GetGPUVirtualAddress() + transformOffset);
            cmdList->SetGraphicsRootConstantBufferView(1, m_geometryFrameCB->GetGPUVirtualAddress());
            cmdList->SetGraphicsRootConstantBufferView(2, m_materialCB->GetGPUVirtualAddress() + materialOffset);
            cmdList->SetGraphicsRootDescriptorTable(3, m_renderer.GetSrvGpuHandle(textureSrv));
            if (drawAsBillboard)
            {
                cmdList->IASetVertexBuffers(0, 1, &m_billboardVbView);
                cmdList->IASetIndexBuffer(&m_billboardIbView);
                cmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);
                ++m_billboardDrawCount;
                cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
                cmdList->IASetIndexBuffer(m_renderer.GetIbView());
            }
            else
            {
                cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexStart, 0, 0);
                ++m_cubeDrawCount;
            }
            ++drawIndex;
        }
    }

    m_objectTransformCB->Unmap(0, nullptr);
    m_materialCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateCascadedShadowMapsData()
{
    const float nearZ = (std::max)(m_cameraNear, 0.01f);
    const float farZ = (std::max)(m_cameraFar, nearZ + 1.0f);
    const float lambda = 0.5f;

    std::array<float, ShadowCascadeCount> splits{};
    for (UINT i = 1; i <= ShadowCascadeCount; ++i)
    {
        const float p = static_cast<float>(i) / static_cast<float>(ShadowCascadeCount);
        const float logSplit = nearZ * std::pow(farZ / nearZ, p);
        const float linearSplit = nearZ + (farZ - nearZ) * p;
        splits[i - 1] = lambda * logSplit + (1.0f - lambda) * linearSplit;
    }
    splits[ShadowCascadeCount - 1] = farZ;

    // Cascade splits are stored as view-space distances. LightingPass.hlsl compares them
    // with abs(viewPos.z) to choose the Texture2DArray slice.
    m_cascadeSplits = XMFLOAT4(splits[0], splits[1], splits[2], splits[3]);

    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);

    XMVECTOR fullNearCorners[4]{};
    XMVECTOR fullFarCorners[4]{};
    const float ndcX[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float ndcY[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (UINT i = 0; i < 4; ++i)
    {
        fullNearCorners[i] = XMVector3TransformCoord(XMVectorSet(ndcX[i], ndcY[i], 0.0f, 1.0f), invViewProj);
        fullFarCorners[i] = XMVector3TransformCoord(XMVectorSet(ndcX[i], ndcY[i], 1.0f, 1.0f), invViewProj);
    }

    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&m_directionalLightDirection));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (std::abs(XMVectorGetX(XMVector3Dot(lightDir, up))) > 0.95f)
    {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    float previousSplit = nearZ;
    for (UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
    {
        const float cascadeNear = previousSplit;
        const float cascadeFar = splits[cascade];
        previousSplit = cascadeFar;

        const float nearRatio = (cascadeNear - nearZ) / (farZ - nearZ);
        const float farRatio = (cascadeFar - nearZ) / (farZ - nearZ);

        XMVECTOR cascadeCorners[8]{};
        XMVECTOR center = XMVectorZero();
        for (UINT i = 0; i < 4; ++i)
        {
            const XMVECTOR cornerRay = fullFarCorners[i] - fullNearCorners[i];
            cascadeCorners[i] = fullNearCorners[i] + cornerRay * nearRatio;
            cascadeCorners[i + 4] = fullNearCorners[i] + cornerRay * farRatio;
            center += cascadeCorners[i] + cascadeCorners[i + 4];
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        float radius = 0.0f;
        for (const XMVECTOR& corner : cascadeCorners)
        {
            radius = (std::max)(radius, XMVectorGetX(XMVector3Length(corner - center)));
        }
        const float lightDistance = radius + 250.0f;
        const XMVECTOR eye = center - lightDir * lightDistance;
        const XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        XMFLOAT3 minBounds(
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)());
        XMFLOAT3 maxBounds(
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)(),
            -(std::numeric_limits<float>::max)());

        for (const XMVECTOR& corner : cascadeCorners)
        {
            XMFLOAT3 p{};
            XMStoreFloat3(&p, XMVector3TransformCoord(corner, lightView));
            minBounds.x = (std::min)(minBounds.x, p.x);
            minBounds.y = (std::min)(minBounds.y, p.y);
            minBounds.z = (std::min)(minBounds.z, p.z);
            maxBounds.x = (std::max)(maxBounds.x, p.x);
            maxBounds.y = (std::max)(maxBounds.y, p.y);
            maxBounds.z = (std::max)(maxBounds.z, p.z);
        }

        // Basic stabilization: snap the orthographic XY center to shadow texel units.
        const float width = maxBounds.x - minBounds.x;
        const float height = maxBounds.y - minBounds.y;
        const float texelX = width / static_cast<float>(ShadowMapResolution);
        const float texelY = height / static_cast<float>(ShadowMapResolution);
        float centerX = (minBounds.x + maxBounds.x) * 0.5f;
        float centerY = (minBounds.y + maxBounds.y) * 0.5f;
        centerX = std::floor(centerX / texelX) * texelX;
        centerY = std::floor(centerY / texelY) * texelY;
        minBounds.x = centerX - width * 0.5f;
        maxBounds.x = centerX + width * 0.5f;
        minBounds.y = centerY - height * 0.5f;
        maxBounds.y = centerY + height * 0.5f;

        minBounds.z -= 250.0f;
        maxBounds.z += 250.0f;

        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            minBounds.x,
            maxBounds.x,
            minBounds.y,
            maxBounds.y,
            minBounds.z,
            maxBounds.z);

        // Matrices in this project are uploaded transposed and consumed with row-vector mul().
        XMStoreFloat4x4(&m_shadowViewProj[cascade], XMMatrixTranspose(lightView * lightProj));
    }
}

void RenderingSystem::ShadowPass()
{
    if (!m_enableShadows || !m_shadowMap || !m_shadowPSO || !m_shadowRS || !m_shadowFrameCB || !m_shadowObjectTransformCB || !m_materialCB)
        return;

    auto cmdList = m_renderer.GetCmdList();

    auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->ResourceBarrier(1, &toDepthWrite);

    D3D12_VIEWPORT shadowViewport{};
    shadowViewport.TopLeftX = 0.0f;
    shadowViewport.TopLeftY = 0.0f;
    shadowViewport.Width = static_cast<float>(ShadowMapResolution);
    shadowViewport.Height = static_cast<float>(ShadowMapResolution);
    shadowViewport.MinDepth = 0.0f;
    shadowViewport.MaxDepth = 1.0f;
    D3D12_RECT shadowScissor{ 0, 0, static_cast<LONG>(ShadowMapResolution), static_cast<LONG>(ShadowMapResolution) };
    cmdList->RSSetViewports(1, &shadowViewport);
    cmdList->RSSetScissorRects(1, &shadowScissor);

    cmdList->SetGraphicsRootSignature(m_shadowRS.Get());
    cmdList->SetPipelineState(m_shadowPSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();
    if (!subsets.empty())
    {
        void* shadowObjectMapped = nullptr;
        void* shadowMaterialMapped = nullptr;
        HRESULT shadowObjectMapHr = m_shadowObjectTransformCB->Map(0, nullptr, &shadowObjectMapped);
        HRESULT shadowMaterialMapHr = m_materialCB->Map(0, nullptr, &shadowMaterialMapped);
        if (FAILED(shadowObjectMapHr) || shadowObjectMapped == nullptr ||
            FAILED(shadowMaterialMapHr) || shadowMaterialMapped == nullptr)
        {
            OutputDebugStringA("[ShadowPass] Failed to map shadow object/material constant buffers; skipping shadow pass.\n");
            if (shadowObjectMapped) m_shadowObjectTransformCB->Unmap(0, nullptr);
            if (shadowMaterialMapped) m_materialCB->Unmap(0, nullptr);
            auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
                m_shadowMap.Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &toShaderResource);
            return;
        }
        auto* shadowObjectBase = reinterpret_cast<std::uint8_t*>(shadowObjectMapped);
        auto* shadowMaterialBase = reinterpret_cast<std::uint8_t*>(shadowMaterialMapped);

        const bool drawMainModel = m_renderMainSceneModel || m_sceneObjects.empty();
        const size_t objectCount = drawMainModel ? 1 : m_sceneObjects.size();

        for (UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
        {
            void* shadowFrameMapped = nullptr;
            HRESULT shadowFrameMapHr = m_shadowFrameCB->Map(0, nullptr, &shadowFrameMapped);
            if (FAILED(shadowFrameMapHr) || shadowFrameMapped == nullptr)
            {
                OutputDebugStringA("[ShadowPass] Failed to map shadow frame constant buffer; skipping remaining cascades.\n");
                break;
            }
            memcpy(
                reinterpret_cast<std::uint8_t*>(shadowFrameMapped) + cascade * m_shadowFrameCbStride,
                &m_shadowViewProj[cascade],
                sizeof(XMFLOAT4X4));
            m_shadowFrameCB->Unmap(0, nullptr);

            cmdList->ClearDepthStencilView(m_shadowDsvHandles[cascade], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandles[cascade]);
            cmdList->SetGraphicsRootConstantBufferView(1, m_shadowFrameCB->GetGPUVirtualAddress() + cascade * m_shadowFrameCbStride);

            size_t drawIndex = 0;
            for (size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
            {
                const SceneObject* object = drawMainModel ? nullptr : &m_sceneObjects[objectIndex];
                for (size_t subsetIndex = 0; subsetIndex < subsets.size(); ++subsetIndex)
                {
                    if (drawIndex >= m_maxObjectCbCount)
                        break;

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
                        transform.World = object->World;
                        transform.WorldInvTranspose = object->WorldInvTranspose;
                        transform.ColorTint = object->ColorTint;
                    }

                    const auto& subset = subsets[subsetIndex];

                    MaterialConstants material{};
                    material.MaterialDiffuse = XMFLOAT4(1, 1, 1, 1);
                    material.MaterialSpecular = XMFLOAT4(1, 1, 1, 1);
                    material.SpecularPower = 32.0f;
                    material.HasTexture = 0;
                    material.HasNormalMap = 0;
                    material.HasDisplacementMap = 0;
                    material.HasMetallicMap = 0;
                    material.HasRoughnessMap = 0;
                    material.HasAOMap = 0;
                    material.DisplacementScale = 0.0f;
                    material.DisplacementBias = 0.0f;

                    UINT textureSrv = 0;
                    if (subset.materialIdx >= 0 && subset.materialIdx < static_cast<int>(materials.size()))
                    {
                        const auto& mat = materials[subset.materialIdx];
                        material.MaterialDiffuse = mat.diffuse;
                        material.MaterialSpecular = mat.specular;
                        material.SpecularPower = mat.specPower;
                        if (mat.diffuseSrvHeapIndex >= 0)
                        {
                            material.HasTexture = mat.hasDiffuseMap ? 1 : 0;
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
                        if (mat.metallicSrvHeapIndex >= 0 && mat.hasMetallicMap)
                        {
                            material.HasMetallicMap = 1;
                        }
                        if (mat.roughnessSrvHeapIndex >= 0 && mat.hasRoughnessMap)
                        {
                            material.HasRoughnessMap = 1;
                        }
                        if (mat.aoSrvHeapIndex >= 0 && mat.hasAOMap)
                        {
                            material.HasAOMap = 1;
                        }
                    }

                    const UINT transformOffset = static_cast<UINT>(drawIndex * m_objectTransformCbStride);
                    const UINT materialOffset = static_cast<UINT>(drawIndex * m_materialCbStride);
                    memcpy(shadowObjectBase + transformOffset, &transform, sizeof(transform));
                    memcpy(shadowMaterialBase + materialOffset, &material, sizeof(material));
                    cmdList->SetGraphicsRootConstantBufferView(0, m_shadowObjectTransformCB->GetGPUVirtualAddress() + transformOffset);
                    cmdList->SetGraphicsRootConstantBufferView(2, m_materialCB->GetGPUVirtualAddress() + materialOffset);
                    cmdList->SetGraphicsRootDescriptorTable(3, m_renderer.GetSrvGpuHandle(textureSrv));
                    cmdList->DrawIndexedInstanced(subset.indexCount, 1, subset.indexStart, 0, 0);
                    ++drawIndex;
                }
            }
        }

        m_shadowObjectTransformCB->Unmap(0, nullptr);
        m_materialCB->Unmap(0, nullptr);
    }

    auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toShaderResource);

    D3D12_VIEWPORT backBufferViewport{ 0.0f, 0.0f, static_cast<float>(m_renderer.GetWidth()), static_cast<float>(m_renderer.GetHeight()), 0.0f, 1.0f };
    D3D12_RECT backBufferScissor{ 0, 0, static_cast<LONG>(m_renderer.GetWidth()), static_cast<LONG>(m_renderer.GetHeight()) };
    cmdList->RSSetViewports(1, &backBufferViewport);
    cmdList->RSSetScissorRects(1, &backBufferScissor);
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
    cb.View = m_view;
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));
    for (UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
    {
        cb.ShadowViewProj[cascade] = m_shadowViewProj[cascade];
    }
    cb.CascadeSplits = m_cascadeSplits;
    cb.ShadowMapSize = static_cast<float>(ShadowMapResolution);
    cb.CascadeCount = ShadowCascadeCount;
    cb.EnableShadows = m_enableShadows;

    cb.PointLightCount = (std::min)(m_activePointLights, LightingContract::MaxPointLights);
    cb.SpotLightCount = m_activeSpotLights;
    cb.DebugMode = m_debugMode;
    cb.ForceMirrorMaterial = (m_activeSceneKind == DemoSceneKind::PBRModel && m_forceMirrorMaterial) ? 1u : 0u;
    cb.IBLDiffuseStrength = m_iblDiffuseStrength;
    cb.IBLSpecularStrength = m_iblSpecularStrength;
    cb.ShowIBLSkybox = m_showIBLSkybox ? 1u : 0u;

    void* mapped = nullptr;
    HRESULT mapHr = m_frameCB->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[Lighting] Failed to map frame CB; skipping frame constants update.\n");
        return;
    }
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
    HRESULT mapHr = m_localLightsCB->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[Lighting] Failed to map local lights CB; skipping local light constants update.\n");
        return;
    }
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
        HRESULT mapHr = m_pointLightsUploadBuffer->Map(0, nullptr, &mapped);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            OutputDebugStringA("[Lighting] Failed to map point light upload buffer; skipping point light upload.\n");
            return;
        }
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
    auto rtv = m_sceneColorRtv;

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
    auto rtv = m_sceneColorRtv;

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
    HRESULT mapHr = m_rainProxyFrameCB->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[RainProxy] Failed to map frame CB; skipping rain proxy pass.\n");
        return;
    }
    memcpy(mapped, &cb, sizeof(cb));
    m_rainProxyFrameCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_sceneColorRtv;

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
    const bool runParticleFountain = ShouldRunParticleFountain();
    if (runParticleFountain)
    {
        if (m_particlesReinitRequested)
        {
            m_particles.Reinitialize(cmdList);
            m_particlesReinitRequested = false;
        }
        m_particles.Update(cmdList, deltaTime, totalTime, m_cameraPos, GetParticleEmitterPosition(), GetParticleFountainSettings());
    }
    else
    {
        m_particlesReinitRequested = false;
    }

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing)
    {
        UpdateObjectVisibility();
    }

    UpdateCascadedShadowMapsData();
    ShadowPass();

    m_gbuffer.BeginGeometryPass(cmdList);
    m_gbuffer.Clear(cmdList);
    GeometryPass();

    m_renderer.TransitionDepthToShaderResource();
    m_gbuffer.EndGeometryPass(cmdList);

    UpdateFrameConstants();
    UpdateLocalLightConstants();
    UploadPointLightsToGpu();

    TransitionSceneColor(D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(m_sceneColorRtv, black, 0, nullptr);

    LightingPassDirectional();

    // Local lights are only needed in final and lighting debug modes.
    if (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 7)
    {
        LightingPassLocal();
    }

    if (m_enableFallingLights && (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6))
    {
        RainLightProxyPass();
    }

    if (runParticleFountain)
    {
        m_renderer.TransitionDepthToDepthRead();
        m_particles.Render(
            cmdList,
            m_sceneColorRtv,
            m_renderer.GetDsvHandle(),
            m_view,
            m_proj,
            m_cameraPos,
            m_directionalLightDirection,
            m_directionalLightIntensity,
            m_directionalLightColor,
            m_ambientColor);
        m_renderer.TransitionDepthToShaderResource();
    }

    if (m_activeSceneKind == DemoSceneKind::DirtyInstancing && m_enableCulling && m_showCullingDebugGrid)
    {
        DebugLinePass();
    }

    TransitionSceneColor(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PostProcessPass(totalTime);

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
    // Keep resize path minimal and deterministic to avoid breaking scene/pipeline switches.
    if (!m_initialized)
        return;
    if (width <= 0 || height <= 0)
        return;
    if (m_renderer.GetSrvHeap() == nullptr)
        return;

    m_renderer.WaitForIdle();
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
    CreateOrResizeSceneColorResources(static_cast<UINT>(width), static_cast<UINT>(height));

    const XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f),
        static_cast<float>(width) / static_cast<float>(height),
        m_cameraNear,
        m_cameraFar);
    XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));
}

void RenderingSystem::CreateOrResizeSceneColorResources(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return;
    auto device = m_renderer.GetDevice();
    if (!device)
        return;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = SceneColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = SceneColorFormat;
    clearValue.Color[0] = 0.0f; clearValue.Color[1] = 0.0f; clearValue.Color[2] = 0.0f; clearValue.Color[3] = 1.0f;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    m_sceneColor.Reset();
    RS_ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&m_sceneColor)));

    m_sceneColorRtv = m_renderer.GetRtvCpuHandle(SceneColorRtvIndex);
    m_sceneColorSrvCpu = m_renderer.GetSrvCpuHandle(SceneColorSrvIndex);
    m_sceneColorSrvGpu = m_renderer.GetSrvGpuHandle(SceneColorSrvIndex);

    device->CreateRenderTargetView(m_sceneColor.Get(), nullptr, m_sceneColorRtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = SceneColorFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_sceneColor.Get(), &srvDesc, m_sceneColorSrvCpu);
    m_sceneColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void RenderingSystem::TransitionSceneColor(D3D12_RESOURCE_STATES newState)
{
    if (!m_sceneColor)
        return;
    if (m_sceneColorState == newState)
        return;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_sceneColor.Get(), m_sceneColorState, newState);
    m_renderer.GetCmdList()->ResourceBarrier(1, &barrier);
    m_sceneColorState = newState;
}

void RenderingSystem::PostProcessPass(float totalTime)
{
    if (!m_postProcessPSO || !m_postProcessRS || !m_sceneColor || !m_postProcessCB)
        return;

    const float w = static_cast<float>(m_renderer.GetWidth());
    const float h = static_cast<float>(m_renderer.GetHeight());
    PostProcessConstants cb{};
    cb.ScreenSize = XMFLOAT2(w, h);
    cb.InvScreenSize = XMFLOAT2(1.0f / w, 1.0f / h);
    cb.Time = totalTime;
    cb.Mode = static_cast<UINT>(m_postProcessMode);
    cb.EdgeStrength = 1.0f;
    cb.DepthEdgeScale = 1.25f;
    cb.NormalEdgeScale = 1.0f;
    cb.LumaEdgeScale = 0.8f;
    cb.VcrIntensity = m_vcrStrongMode ? 1.65f : 1.0f;
    cb.ScanlineStrength = m_vcrStrongMode ? 0.32f : 0.20f;
    cb.NoiseStrength = m_vcrStrongMode ? 0.13f : 0.08f;
    cb.ChromaticAberration = m_vcrStrongMode ? 1.5f : 1.0f;
    cb.ScannerIntensity = 1.0f;
    cb.ScannerSpeed = 0.65f;
    cb.ScannerLineWidth = 0.045f;
    cb.ScannerTickStrength = 0.75f;
    cb.NauseaIntensity = m_nauseaStrongMode ? 1.35f : 0.85f;
    cb.NauseaSpeed = m_nauseaStrongMode ? 1.35f : 0.85f;
    cb.KaleidoscopeSegments = m_nauseaStrongMode ? 8.0f : 6.0f;
    cb.NauseaChromaticAberration = m_nauseaStrongMode ? 2.4f : 1.4f;
    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));
    cb.EyePos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    cb.ScannerMaxDistance = 1600.0f;
    cb.ScannerWorldLineWidth = 35.0f;
    cb.ScannerTrailLength = 180.0f;
    cb.ScannerGridScale = 90.0f;
    cb.Exposure = m_exposure;
    cb.Gamma = m_gamma;
    cb.ToneMapperMode = m_toneMapperMode;
    cb.ToneMapWhitePoint = 11.2f;

    void* mapped = nullptr;
    HRESULT mapHr = m_postProcessCB->Map(0, nullptr, &mapped);
    if (FAILED(mapHr) || mapped == nullptr)
    {
        OutputDebugStringA("[PostProcess] Failed to map post-process CB; skipping post-process constants update.\n");
        return;
    }
    memcpy(mapped, &cb, sizeof(cb));
    m_postProcessCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto backbufferRtv = m_renderer.GetBackBufferRtv();
    cmdList->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_postProcessRS.Get());
    cmdList->SetPipelineState(m_postProcessPSO.Get());
    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootConstantBufferView(0, m_postProcessCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_sceneColorSrvGpu);
    cmdList->SetGraphicsRootDescriptorTable(2, m_gbuffer.GetFirstSrvGpu());
    cmdList->SetGraphicsRootDescriptorTable(3, m_renderer.GetSrvGpuHandle(4));
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, 1, 0, 0);
}
