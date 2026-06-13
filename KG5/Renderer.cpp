#include "Renderer.h"
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>

static void ThrowIfFailedRenderer(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw std::runtime_error("Renderer DX12 call failed");
    }
}

bool Renderer::Init(HWND hwnd, int width, int height)
{
    if (m_initialized)
        return true;

    m_width = width;
    m_height = height;

    try
    {
        CreateDevice();
        CreateCommandObjects();
        CreateSwapChain(hwnd, width, height);
        CreateDescriptorHeaps();
        CreateRenderTargetViews();
        CreateDepthStencilView();
        CreateFence();
        CreateDefaultTexture();

        m_viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
        m_scissorRect = { 0, 0, m_width, m_height };
        m_initialized = true;
    }
    catch (...)
    {
        m_initialized = false;
        return false;
    }

    return true;
}

void Renderer::BeginUploadCommands()
{
    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));
}

void Renderer::EndUploadCommands()
{
    ThrowIfFailedRenderer(m_cmdList->Close());
    ID3D12CommandList* cmdLists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmdLists);
    WaitForGPU();
}

void Renderer::CreateDevice()
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            debugController->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailedRenderer(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0;
        factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
        ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
            break;
    }

    if (!m_device)
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailedRenderer(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailedRenderer(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    }
}

void Renderer::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qDesc.NodeMask = 0;
    ThrowIfFailedRenderer(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue)));

    for (UINT i = 0; i < _countof(m_cmdAllocators); ++i)
    {
        ThrowIfFailedRenderer(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_cmdAllocators[i])));
    }

    ThrowIfFailedRenderer(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&m_cmdList)));

    ThrowIfFailedRenderer(m_cmdList->Close());
}

void Renderer::CreateSwapChain(HWND hwnd, int width, int height)
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailedRenderer(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = static_cast<UINT>(width);
    scDesc.Height = static_cast<UINT>(height);
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = _countof(m_renderTargets);
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailedRenderer(factory->CreateSwapChainForHwnd(
        m_cmdQueue.Get(),
        hwnd,
        &scDesc,
        nullptr,
        nullptr,
        &swapChain1));

    ThrowIfFailedRenderer(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailedRenderer(swapChain1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Renderer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 16;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailedRenderer(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailedRenderer(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 256;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailedRenderer(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_cbvSrvHeap)));

    m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_cbvSrvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Renderer::CreateDefaultTexture()
{
    TextureLoader::TextureData defaultWhiteTex;
    defaultWhiteTex.width = 1;
    defaultWhiteTex.height = 1;
    defaultWhiteTex.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultWhiteTex.rowPitch = 4;
    defaultWhiteTex.pixels = { 255, 255, 255, 255 };

    TextureLoader::TextureData defaultBlackTex = defaultWhiteTex;
    defaultBlackTex.pixels = { 0, 0, 0, 255 };

    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

    if (!TextureLoader::CreateTexture(
        m_device.Get(),
        m_cmdList.Get(),
        defaultWhiteTex,
        m_defaultWhiteTexture,
        m_defaultWhiteUpload))
    {
        throw std::runtime_error("Failed to create default white texture");
    }

    if (!TextureLoader::CreateTexture(
        m_device.Get(),
        m_cmdList.Get(),
        defaultBlackTex,
        m_defaultBlackTexture,
        m_defaultBlackUpload))
    {
        throw std::runtime_error("Failed to create default black texture");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    auto cpuHandle = m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateShaderResourceView(m_defaultWhiteTexture.Get(), &srvDesc, cpuHandle);

    ThrowIfFailedRenderer(m_cmdList->Close());
    ID3D12CommandList* cmdLists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmdLists);
    WaitForGPU();
}

void Renderer::CreateRenderTargetViews()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < _countof(m_renderTargets); ++i)
    {
        ThrowIfFailedRenderer(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescSize;
    }
}

void Renderer::CreateDepthStencilView()
{
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = static_cast<UINT64>(m_width);
    depthDesc.Height = static_cast<UINT>(m_height);
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailedRenderer(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&m_depthStencil)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MostDetailedMip = 0;
    depthSrvDesc.Texture2D.MipLevels = 1;
    depthSrvDesc.Texture2D.PlaneSlice = 0;
    depthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    m_device->CreateShaderResourceView(m_depthStencil.Get(), &depthSrvDesc, GetDepthSrvCpuHandle());
}

void Renderer::CreateFence()
{
    ThrowIfFailedRenderer(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[0] = 1;
    m_fenceValues[1] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        throw std::runtime_error("Failed to create fence event");
    }
}

void Renderer::BeginFrame()
{
    m_cmdAllocators[m_frameIndex]->Reset();
    m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr);

    m_viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissorRect = { 0, 0, m_width, m_height };
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_cmdList->ResourceBarrier(1, &barrier);

    if (m_depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_depthStencil.Get(),
            m_depthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_cmdList->ResourceBarrier(1, &depthBarrier);
        m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
}

void Renderer::EndFrame()
{
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_cmdList->ResourceBarrier(1, &barrier);
    ThrowIfFailedRenderer(m_cmdList->Close());

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    m_swapChain->Present(1, 0);

    MoveToNextFrame();
}


void Renderer::TransitionDepthTo(D3D12_RESOURCE_STATES newState)
{
    if (!m_depthStencil || m_depthState == newState)
        return;

    auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_depthStencil.Get(),
        m_depthState,
        newState);
    m_cmdList->ResourceBarrier(1, &depthBarrier);
    m_depthState = newState;
}

void Renderer::TransitionDepthToShaderResource()
{
    TransitionDepthTo(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Renderer::TransitionDepthToDepthRead()
{
    TransitionDepthTo(D3D12_RESOURCE_STATE_DEPTH_READ);
}

void Renderer::CreateBuffer(const void* data, UINT size, ID3D12Resource** resource)
{
    auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    ThrowIfFailedRenderer(m_device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource)));
    if (data)
    {
        void* mapped = nullptr;
        HRESULT mapHr = (*resource)->Map(0, nullptr, &mapped);
        if (FAILED(mapHr) || mapped == nullptr)
        {
            OutputDebugStringA("[Renderer] Failed to map upload buffer in CreateBuffer.\n");
            return;
        }
        memcpy(mapped, data, size);
        (*resource)->Unmap(0, nullptr);
    }
}

void Renderer::MoveToNextFrame()
{
    const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
    ThrowIfFailedRenderer(m_cmdQueue->Signal(m_fence.Get(), currentFenceValue));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
    {
        ThrowIfFailedRenderer(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

void Renderer::WaitForGPU()
{
    const UINT64 fenceValue = m_fenceValues[m_frameIndex];
    ThrowIfFailedRenderer(m_cmdQueue->Signal(m_fence.Get(), fenceValue));
    ThrowIfFailedRenderer(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
    m_fenceValues[m_frameIndex] = fenceValue + 1;
}

void Renderer::OnResize(int width, int height)
{
    if (!m_initialized || width <= 0 || height <= 0)
        return;

    m_width = width;
    m_height = height;

    WaitForGPU();

    for (UINT i = 0; i < _countof(m_renderTargets); ++i)
        m_renderTargets[i].Reset();
    m_depthStencil.Reset();
    m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    ThrowIfFailedRenderer(m_swapChain->ResizeBuffers(
        _countof(m_renderTargets),
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    CreateRenderTargetViews();
    CreateDepthStencilView();

    m_viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissorRect = { 0, 0, m_width, m_height };
}


namespace
{
    std::string ToLowerPathString(const std::filesystem::path& path)
    {
        std::string value = path.generic_string();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    void ApplyKnownPBRTextures(ObjMesh& mesh, const std::filesystem::path& baseDir)
    {
        const std::string modelDirLower = ToLowerPathString(baseDir);
        auto assignTextureIfExists = [&](std::string& dst, const char* fileName)
        {
            if (std::filesystem::exists(baseDir / fileName))
                dst = fileName;
        };

        for (Material& material : mesh.materials)
        {
            if (material.textureBaseDir.empty())
                material.textureBaseDir = baseDir.generic_string();
        }

        if (modelDirLower.find("cerberus_by_andrew_maximov") != std::string::npos)
        {
            for (Material& material : mesh.materials)
            {
                assignTextureIfExists(material.diffuseTexture, "Cerberus_A.jpg");
                assignTextureIfExists(material.metallicTexture, "Cerberus_M.jpg");
                assignTextureIfExists(material.normalTexture, "Cerberus_N.jpg");
                assignTextureIfExists(material.roughnessTexture, "Cerberus_R.jpg");
                material.aoTexture.clear();
            }
            OutputDebugStringA("[PBR] Applied explicit Cerberus texture mapping: A/M/N/R\n");
        }
        else if (modelDirLower.find("wood_root") != std::string::npos)
        {
            for (Material& material : mesh.materials)
            {
                assignTextureIfExists(material.diffuseTexture, "Asset_wood_root_M_rkswd_2K_Albedo.jpg");
                assignTextureIfExists(material.normalTexture, "Asset_wood_root_M_rkswd_2K_Normal_LOD0.jpg");
                assignTextureIfExists(material.roughnessTexture, "Asset_wood_root_M_rkswd_2K_Roughness.jpg");
                material.metallicTexture.clear();
                material.aoTexture.clear();
            }
            OutputDebugStringA("[PBR] Applied explicit wood_root texture mapping: Albedo/Normal/Roughness, metallic fallback=0\n");
        }
    }

    void NormalizeVector(XMFLOAT3& v)
    {
        XMVECTOR vec = XMLoadFloat3(&v);
        if (XMVectorGetX(XMVector3LengthSq(vec)) <= 1.0e-8f)
            return;
        vec = XMVector3Normalize(vec);
        XMStoreFloat3(&v, vec);
    }

    void NormalizeAndPlaceMesh(ObjMesh& mesh, const XMFLOAT3& targetCenterXZ, float targetHeight)
    {
        if (mesh.vertices.empty())
            return;

        XMFLOAT3 minP = mesh.vertices.front().Position;
        XMFLOAT3 maxP = mesh.vertices.front().Position;
        for (const auto& v : mesh.vertices)
        {
            minP.x = (std::min)(minP.x, v.Position.x); minP.y = (std::min)(minP.y, v.Position.y); minP.z = (std::min)(minP.z, v.Position.z);
            maxP.x = (std::max)(maxP.x, v.Position.x); maxP.y = (std::max)(maxP.y, v.Position.y); maxP.z = (std::max)(maxP.z, v.Position.z);
        }

        const float height = (std::max)(maxP.y - minP.y, 0.001f);
        const float scale = targetHeight / height;
        const float centerX = (minP.x + maxP.x) * 0.5f;
        const float centerZ = (minP.z + maxP.z) * 0.5f;

        for (auto& v : mesh.vertices)
        {
            v.Position.x = (v.Position.x - centerX) * scale + targetCenterXZ.x;
            v.Position.y = (v.Position.y - minP.y) * scale + targetCenterXZ.y;
            v.Position.z = (v.Position.z - centerZ) * scale + targetCenterXZ.z;
            NormalizeVector(v.Normal);
            NormalizeVector(v.Tangent);
            NormalizeVector(v.Bitangent);
        }
    }

    void AppendMesh(ObjMesh& dst, const ObjMesh& src)
    {
        const UINT vertexOffset = static_cast<UINT>(dst.vertices.size());
        const UINT indexOffset = static_cast<UINT>(dst.indices.size());
        const int materialOffset = static_cast<int>(dst.materials.size());

        dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
        dst.materials.insert(dst.materials.end(), src.materials.begin(), src.materials.end());

        dst.indices.reserve(dst.indices.size() + src.indices.size());
        for (UINT index : src.indices)
            dst.indices.push_back(index + vertexOffset);

        dst.subsets.reserve(dst.subsets.size() + src.subsets.size());
        for (MeshSubset subset : src.subsets)
        {
            subset.indexStart += indexOffset;
            if (subset.materialIdx >= 0)
                subset.materialIdx += materialOffset;
            dst.subsets.push_back(subset);
        }
    }
}

bool Renderer::LoadObj(const std::string& path)
{
    ObjMesh mesh;
    if (!ObjLoader::Load(path, mesh))
        return false;

    const std::filesystem::path baseDir = std::filesystem::path(path).parent_path();
    ApplyKnownPBRTextures(mesh, baseDir);
    return UploadObjMesh(mesh, baseDir);
}

bool Renderer::LoadPBRDemoModels(const std::string& cerberusObjPath, const std::string& woodRootObjPath)
{
    ObjMesh combined;
    bool loadedAny = false;

    if (!cerberusObjPath.empty())
    {
        ObjMesh cerberus;
        if (ObjLoader::Load(cerberusObjPath, cerberus))
        {
            const std::filesystem::path baseDir = std::filesystem::path(cerberusObjPath).parent_path();
            ApplyKnownPBRTextures(cerberus, baseDir);
            NormalizeAndPlaceMesh(cerberus, XMFLOAT3(-60.0f, 0.0f, 0.0f), 80.0f);
            AppendMesh(combined, cerberus);
            loadedAny = true;
            OutputDebugStringA((std::string("[PBR] Loaded PBR OBJ: ") + cerberusObjPath + "\n").c_str());
        }
        else
        {
            OutputDebugStringA((std::string("[PBR] Failed to load Cerberus OBJ: ") + cerberusObjPath + "\n").c_str());
        }
    }

    if (!woodRootObjPath.empty())
    {
        ObjMesh woodRoot;
        if (ObjLoader::Load(woodRootObjPath, woodRoot))
        {
            const std::filesystem::path baseDir = std::filesystem::path(woodRootObjPath).parent_path();
            ApplyKnownPBRTextures(woodRoot, baseDir);
            NormalizeAndPlaceMesh(woodRoot, XMFLOAT3(60.0f, 0.0f, 0.0f), 80.0f);
            AppendMesh(combined, woodRoot);
            loadedAny = true;
            OutputDebugStringA((std::string("[PBR] Loaded PBR OBJ: ") + woodRootObjPath + "\n").c_str());
        }
        else
        {
            OutputDebugStringA((std::string("[PBR] Failed to load wood_root OBJ: ") + woodRootObjPath + "\n").c_str());
        }
    }

    if (!loadedAny)
        return false;

    return UploadObjMesh(combined, std::filesystem::path("."));
}

bool Renderer::UploadObjMesh(ObjMesh& mesh, const std::filesystem::path& fallbackBaseDir)
{

    std::vector<Vertex> verts;
    verts.reserve(mesh.vertices.size());

    for (const auto& v : mesh.vertices)
    {
        Vertex vv{};
        vv.Position = v.Position;
        vv.Normal = v.Normal;
        vv.TexCoord = v.TexCoord;
        vv.Tangent = v.Tangent;
        vv.Bitangent = v.Bitangent;
        verts.push_back(vv);
    }

    const std::filesystem::path baseDir = fallbackBaseDir;
    for (Material& material : mesh.materials)
    {
        if (material.textureBaseDir.empty())
            material.textureBaseDir = baseDir.generic_string();
    }
    const std::string modelDirLower = [&]()
    {
        std::string value = baseDir.generic_string();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }();

    auto assignTextureIfExists = [&](std::string& dst, const char* fileName)
    {
        if (std::filesystem::exists(baseDir / fileName))
            dst = fileName;
    };

    // Some downloaded PBR OBJ/MTL files omit non-standard PBR map tokens.
    // For the known lab assets, assign maps by filename so GeometryPass receives real PBR inputs.
    if (modelDirLower.find("cerberus_by_andrew_maximov") != std::string::npos)
    {
        for (Material& material : mesh.materials)
        {
            assignTextureIfExists(material.diffuseTexture, "Cerberus_A.jpg");
            assignTextureIfExists(material.metallicTexture, "Cerberus_M.jpg");
            assignTextureIfExists(material.normalTexture, "Cerberus_N.jpg");
            assignTextureIfExists(material.roughnessTexture, "Cerberus_R.jpg");
        }
        OutputDebugStringA("[PBR] Applied explicit Cerberus texture mapping: A/M/N/R\n");
    }
    else if (modelDirLower.find("wood_root") != std::string::npos)
    {
        for (Material& material : mesh.materials)
        {
            assignTextureIfExists(material.diffuseTexture, "Asset_wood_root_M_rkswd_2K_Albedo.jpg");
            assignTextureIfExists(material.normalTexture, "Asset_wood_root_M_rkswd_2K_Normal_LOD0.jpg");
            assignTextureIfExists(material.roughnessTexture, "Asset_wood_root_M_rkswd_2K_Roughness.jpg");
            material.metallicTexture.clear();
        }
        OutputDebugStringA("[PBR] Applied explicit wood_root texture mapping: Albedo/Normal/Roughness, metallic fallback=0\n");
    }

    m_subsets = mesh.subsets;
    m_gpuMaterials.clear();
    m_gpuMaterials.resize(mesh.materials.size());
    m_nextSrvIndex = 16;

    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

    auto createSrvAt = [&](UINT heapIndex, ID3D12Resource* resource, DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
            m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            heapIndex,
            m_cbvSrvDescSize);

        m_device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
    };

    auto tryLoadTexture = [&](const std::filesystem::path& texPath,
                              ComPtr<ID3D12Resource>& outTexture,
                              ComPtr<ID3D12Resource>& outUpload,
                              DXGI_FORMAT& outFormat) -> bool
    {
        TextureLoader::TextureData texData;
        if (!TextureLoader::LoadFromFile(texPath.wstring(), texData))
            return false;

        if (!TextureLoader::CreateTexture(
            m_device.Get(),
            m_cmdList.Get(),
            texData,
            outTexture,
            outUpload))
        {
            return false;
        }

        outFormat = texData.format;
        return true;
    };

    auto tryLoadTextureCandidates = [&](const std::vector<std::filesystem::path>& candidates,
                                        ComPtr<ID3D12Resource>& outTexture,
                                        ComPtr<ID3D12Resource>& outUpload,
                                        DXGI_FORMAT& outFormat) -> bool
    {
        for (const auto& candidate : candidates)
        {
            if (tryLoadTexture(candidate, outTexture, outUpload, outFormat))
                return true;
        }
        return false;
    };

    bool hasGlobalOverrideNormal = false;
    bool hasGlobalOverrideDisplacement = false;
    DXGI_FORMAT globalOverrideNormalFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT globalOverrideDisplacementFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (m_forceSponzaDiagnosticMaterialOverride)
    {
        m_globalOverrideNormalTexture.Reset();
        m_globalOverrideNormalUpload.Reset();
        m_globalOverrideDisplacementTexture.Reset();
        m_globalOverrideDisplacementUpload.Reset();

        const std::filesystem::path overrideNormalAbs = LR"(E:\_Projects\VS Projects\CG_Lab5_test\KG5\assets\N_jardinera_1_displacement_2.png)";
        const std::filesystem::path overrideDispAbs = LR"(E:\_Projects\VS Projects\CG_Lab5_test\KG5\assets\jardinera_1_displacement_2.png)";

        const std::vector<std::filesystem::path> overrideNormalCandidates = {
            overrideNormalAbs,
            baseDir / "N_jardinera_1_displacement_2.png",
            baseDir / "assets" / "N_jardinera_1_displacement_2.png",
            baseDir.parent_path() / "assets" / "N_jardinera_1_displacement_2.png"
        };
        const std::vector<std::filesystem::path> overrideDisplacementCandidates = {
            overrideDispAbs,
            baseDir / "jardinera_1_displacement_2.png",
            baseDir / "assets" / "jardinera_1_displacement_2.png",
            baseDir.parent_path() / "assets" / "jardinera_1_displacement_2.png"
        };

        hasGlobalOverrideNormal = tryLoadTextureCandidates(
            overrideNormalCandidates,
            m_globalOverrideNormalTexture,
            m_globalOverrideNormalUpload,
            globalOverrideNormalFormat);

        hasGlobalOverrideDisplacement = tryLoadTextureCandidates(
            overrideDisplacementCandidates,
            m_globalOverrideDisplacementTexture,
            m_globalOverrideDisplacementUpload,
            globalOverrideDisplacementFormat);
    }

    for (size_t i = 0; i < mesh.materials.size(); ++i)
    {
        m_gpuMaterials[i].diffuse = mesh.materials[i].diffuse;
        m_gpuMaterials[i].specular = mesh.materials[i].specular;
        m_gpuMaterials[i].specPower = mesh.materials[i].shininess;
        m_gpuMaterials[i].hasDiffuseMap = false;
        m_gpuMaterials[i].hasNormalMap = false;
        m_gpuMaterials[i].hasDisplacementMap = false;
        m_gpuMaterials[i].hasMetallicMap = false;
        m_gpuMaterials[i].hasRoughnessMap = false;
        m_gpuMaterials[i].hasAOMap = false;
        m_gpuMaterials[i].displacementScale = 0.0f;
        m_gpuMaterials[i].displacementBias = 0.0f;

        if (m_nextSrvIndex + 5 >= 256)
            continue;

        // Geometry shader expects six consecutive material SRVs: albedo, normal, displacement, metallic, roughness, AO.
        const UINT diffuseSrv = m_nextSrvIndex++;
        const UINT normalSrv = m_nextSrvIndex++;
        const UINT displacementSrv = m_nextSrvIndex++;
        const UINT metallicSrv = m_nextSrvIndex++;
        const UINT roughnessSrv = m_nextSrvIndex++;
        const UINT aoSrv = m_nextSrvIndex++;
        m_gpuMaterials[i].diffuseSrvHeapIndex = static_cast<int>(diffuseSrv);
        m_gpuMaterials[i].normalSrvHeapIndex = static_cast<int>(normalSrv);
        m_gpuMaterials[i].displacementSrvHeapIndex = static_cast<int>(displacementSrv);
        m_gpuMaterials[i].metallicSrvHeapIndex = static_cast<int>(metallicSrv);
        m_gpuMaterials[i].roughnessSrvHeapIndex = static_cast<int>(roughnessSrv);
        m_gpuMaterials[i].aoSrvHeapIndex = static_cast<int>(aoSrv);

        const std::filesystem::path materialBaseDir = mesh.materials[i].textureBaseDir.empty()
            ? baseDir
            : std::filesystem::path(mesh.materials[i].textureBaseDir);
        auto resolveMaterialTexture = [&](const std::filesystem::path& texturePath) -> std::filesystem::path
        {
            if (texturePath.empty())
                return texturePath;
            if (texturePath.is_absolute())
                return texturePath;
            return materialBaseDir / texturePath;
        };

        bool hasDiffuse = false;
        DXGI_FORMAT diffuseFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!mesh.materials[i].diffuseTexture.empty())
        {
            std::filesystem::path texPath = resolveMaterialTexture(mesh.materials[i].diffuseTexture);
            if (tryLoadTexture(
                texPath,
                m_gpuMaterials[i].diffuseTexture,
                m_gpuMaterials[i].diffuseTextureUpload,
                diffuseFormat))
            {
                hasDiffuse = true;
                m_gpuMaterials[i].hasDiffuseMap = true;
            }
        }

        createSrvAt(
            diffuseSrv,
            hasDiffuse ? m_gpuMaterials[i].diffuseTexture.Get() : m_defaultWhiteTexture.Get(),
            hasDiffuse ? diffuseFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        auto toLowerCopy = [](std::string value) -> std::string
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };

        auto looksLikeNormalMapName = [&](const std::string& s) -> bool
        {
            const std::string lower = toLowerCopy(s);
            return lower.find("_ddn") != std::string::npos ||
                lower.find("_nrm") != std::string::npos ||
                lower.find("_normal") != std::string::npos ||
                lower.find("normal") != std::string::npos;
        };

        auto looksLikeDisplacementMapName = [&](const std::string& s) -> bool
        {
            const std::string lower = toLowerCopy(s);
            return lower.find("_disp") != std::string::npos ||
                lower.find("_displacement") != std::string::npos ||
                lower.find("_height") != std::string::npos ||
                lower.find("displacement") != std::string::npos ||
                lower.find("height") != std::string::npos;
        };

        auto pushUniquePath = [](std::vector<std::filesystem::path>& paths, const std::filesystem::path& pathToAdd)
        {
            if (std::find(paths.begin(), paths.end(), pathToAdd) == paths.end())
                paths.push_back(pathToAdd);
        };

        auto replaceCaseInsensitive = [&](std::string value, const std::string& needleText, const std::string& replacement, std::string& out) -> bool
        {
            const std::string lower = toLowerCopy(value);
            const std::string lowerNeedle = toLowerCopy(needleText);
            const size_t pos = lower.find(lowerNeedle);
            if (pos == std::string::npos)
                return false;
            value.replace(pos, needleText.size(), replacement);
            out = value;
            return true;
        };

        auto replaceSuffixCaseInsensitive = [&](std::string value, const std::string& suffix, const std::string& replacement, std::string& out) -> bool
        {
            const std::string lower = toLowerCopy(value);
            const std::string lowerSuffix = toLowerCopy(suffix);
            if (lower.size() < lowerSuffix.size() || lower.compare(lower.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) != 0)
                return false;
            value.replace(value.size() - suffix.size(), suffix.size(), replacement);
            out = value;
            return true;
        };

        auto pushWithCommonExtensions = [&](std::vector<std::filesystem::path>& candidates, const std::filesystem::path& parent, const std::string& stemName, const std::string& preferredExt)
        {
            if (stemName.empty())
                return;
            const std::vector<std::string> extensions = { preferredExt, ".png", ".jpg", ".jpeg", ".tga" };
            for (const std::string& ext : extensions)
            {
                if (ext.empty())
                    continue;
                pushUniquePath(candidates, materialBaseDir / (parent / (stemName + ext)));
            }
        };

        auto appendPbrSiblingCandidates = [&](std::vector<std::filesystem::path>& candidates, const std::vector<std::string>& replacements)
        {
            if (mesh.materials[i].diffuseTexture.empty())
                return;

            const std::filesystem::path diffuseRel(mesh.materials[i].diffuseTexture);
            const std::string diffuseStem = diffuseRel.stem().string();
            const std::string diffuseExt = diffuseRel.extension().string();
            const std::filesystem::path diffuseParent = diffuseRel.parent_path();

            std::string replaced;
            for (const std::string& replacement : replacements)
            {
                if (replaceSuffixCaseInsensitive(diffuseStem, "_Albedo", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceCaseInsensitive(diffuseStem, "Albedo", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceCaseInsensitive(diffuseStem, "BaseColor", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceCaseInsensitive(diffuseStem, "Diffuse", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceSuffixCaseInsensitive(diffuseStem, "_diff", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceSuffixCaseInsensitive(diffuseStem, "_dif", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
                if (replaceSuffixCaseInsensitive(diffuseStem, "_A", replacement, replaced)) pushWithCommonExtensions(candidates, diffuseParent, replaced, diffuseExt);
            }
        };

        bool hasNormal = false;
        DXGI_FORMAT normalFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        {
            if (m_forceSponzaDiagnosticMaterialOverride && hasGlobalOverrideNormal)
            {
                hasNormal = true;
                normalFormat = globalOverrideNormalFormat;
                m_gpuMaterials[i].normalTexture = m_globalOverrideNormalTexture;
                m_gpuMaterials[i].normalTextureUpload = m_globalOverrideNormalUpload;
                m_gpuMaterials[i].hasNormalMap = true;
            }

            std::vector<std::filesystem::path> normalCandidates;

            if (!hasNormal && !mesh.materials[i].normalTexture.empty())
            {
                const std::filesystem::path normalRel(mesh.materials[i].normalTexture);
                if (!looksLikeDisplacementMapName(normalRel.stem().string()))
                    normalCandidates.push_back(resolveMaterialTexture(normalRel));
            }

            if (!hasNormal && !mesh.materials[i].diffuseTexture.empty())
            {
                std::filesystem::path diffuseRel(mesh.materials[i].diffuseTexture);
                std::string diffuseStem = diffuseRel.stem().string();
                const std::string diffuseExt = diffuseRel.extension().string();
                const std::filesystem::path diffuseParent = diffuseRel.parent_path();

                auto pushNormalCandidate = [&](const std::string& stemName)
                {
                    if (stemName.empty())
                        return;
                    normalCandidates.push_back(materialBaseDir / (diffuseParent / (stemName + diffuseExt)));
                };

                pushNormalCandidate(diffuseStem + "_ddn");
                appendPbrSiblingCandidates(normalCandidates, { "_Normal_LOD0", "_Normal", "Normal", "_N" });

                const size_t diffPos = diffuseStem.find("_diff");
                if (diffPos != std::string::npos)
                {
                    std::string replaced = diffuseStem;
                    replaced.replace(diffPos, 5, "_ddn");
                    pushNormalCandidate(replaced);
                }

                const size_t difPos = diffuseStem.find("_dif");
                if (difPos != std::string::npos)
                {
                    std::string replaced = diffuseStem;
                    replaced.replace(difPos, 4, "_ddn");
                    pushNormalCandidate(replaced);
                }
            }

            if (!hasNormal && tryLoadTextureCandidates(
                normalCandidates,
                m_gpuMaterials[i].normalTexture,
                m_gpuMaterials[i].normalTextureUpload,
                normalFormat))
            {
                hasNormal = true;
                m_gpuMaterials[i].hasNormalMap = true;
            }
        }

        createSrvAt(
            normalSrv,
            hasNormal ? m_gpuMaterials[i].normalTexture.Get() : m_defaultWhiteTexture.Get(),
            hasNormal ? normalFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        bool hasDisplacement = false;
        DXGI_FORMAT displacementFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        {
            if (m_forceSponzaDiagnosticMaterialOverride && hasGlobalOverrideDisplacement)
            {
                hasDisplacement = true;
                displacementFormat = globalOverrideDisplacementFormat;
                m_gpuMaterials[i].displacementTexture = m_globalOverrideDisplacementTexture;
                m_gpuMaterials[i].displacementTextureUpload = m_globalOverrideDisplacementUpload;
                m_gpuMaterials[i].displacementScale = 1.5f;
                m_gpuMaterials[i].displacementBias = -0.06f;
                m_gpuMaterials[i].hasDisplacementMap = true;
            }

            std::vector<std::filesystem::path> displacementCandidates;

            if (!hasDisplacement && !mesh.materials[i].displacementTexture.empty())
            {
                std::filesystem::path dispRel(mesh.materials[i].displacementTexture);
                if (!looksLikeNormalMapName(dispRel.stem().string()))
                {
                    displacementCandidates.push_back(resolveMaterialTexture(dispRel));
                }
            }

            if (!hasDisplacement && !mesh.materials[i].diffuseTexture.empty())
            {
                std::filesystem::path diffuseRel(mesh.materials[i].diffuseTexture);
                std::string diffuseStem = diffuseRel.stem().string();
                const std::string diffuseExt = diffuseRel.extension().string();
                const std::filesystem::path diffuseParent = diffuseRel.parent_path();

                auto pushDisplacementCandidate = [&](const std::string& stemName)
                {
                    if (stemName.empty())
                        return;
                    displacementCandidates.push_back(baseDir / (diffuseParent / (stemName + diffuseExt)));
                };

                const size_t diffPos = diffuseStem.find("_diff");
                if (diffPos != std::string::npos)
                {
                    std::string replaced = diffuseStem;
                    replaced.replace(diffPos, 5, "_disp");
                    pushDisplacementCandidate(replaced);
                }

                const size_t difPos = diffuseStem.find("_dif");
                if (difPos != std::string::npos)
                {
                    std::string replaced = diffuseStem;
                    replaced.replace(difPos, 4, "_disp");
                    pushDisplacementCandidate(replaced);
                }

                pushDisplacementCandidate(diffuseStem + "_disp");
                pushDisplacementCandidate(diffuseStem + "_displacement");
                pushDisplacementCandidate(diffuseStem + "_height");
            }

            // Fallback only for column-like materials (does not override whole scene).
            std::string materialKey = mesh.materials[i].name + "|" + mesh.materials[i].diffuseTexture;
            std::string materialKeyLower = materialKey;
            std::transform(materialKeyLower.begin(), materialKeyLower.end(), materialKeyLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!hasDisplacement && materialKeyLower.find("column") != std::string::npos)
            {
                displacementCandidates.push_back(baseDir.parent_path() / "column_a_displacement_3_inv.png");
                displacementCandidates.push_back(baseDir / "column_a_displacement_3_inv.png");
            }

            if (!hasDisplacement && tryLoadTextureCandidates(
                displacementCandidates,
                m_gpuMaterials[i].displacementTexture,
                m_gpuMaterials[i].displacementTextureUpload,
                displacementFormat))
            {
                hasDisplacement = true;
                m_gpuMaterials[i].displacementScale = 1.5f;
                m_gpuMaterials[i].displacementBias = -0.06f;
                m_gpuMaterials[i].hasDisplacementMap = true;
            }
        }

        createSrvAt(
            displacementSrv,
            hasDisplacement ? m_gpuMaterials[i].displacementTexture.Get() : m_defaultWhiteTexture.Get(),
            hasDisplacement ? displacementFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        bool hasMetallic = false;
        DXGI_FORMAT metallicFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<std::filesystem::path> metallicCandidates;
        if (!mesh.materials[i].metallicTexture.empty())
            pushUniquePath(metallicCandidates, resolveMaterialTexture(mesh.materials[i].metallicTexture));
        appendPbrSiblingCandidates(metallicCandidates, { "_Metallic", "Metallic", "_M" });
        if (tryLoadTextureCandidates(metallicCandidates, m_gpuMaterials[i].metallicTexture, m_gpuMaterials[i].metallicTextureUpload, metallicFormat))
        {
            hasMetallic = true;
            m_gpuMaterials[i].hasMetallicMap = true;
        }
        createSrvAt(
            metallicSrv,
            hasMetallic ? m_gpuMaterials[i].metallicTexture.Get() : m_defaultBlackTexture.Get(),
            hasMetallic ? metallicFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        bool hasRoughness = false;
        DXGI_FORMAT roughnessFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<std::filesystem::path> roughnessCandidates;
        if (!mesh.materials[i].roughnessTexture.empty())
            pushUniquePath(roughnessCandidates, resolveMaterialTexture(mesh.materials[i].roughnessTexture));
        appendPbrSiblingCandidates(roughnessCandidates, { "_Roughness", "Roughness", "_R" });
        if (tryLoadTextureCandidates(roughnessCandidates, m_gpuMaterials[i].roughnessTexture, m_gpuMaterials[i].roughnessTextureUpload, roughnessFormat))
        {
            hasRoughness = true;
            m_gpuMaterials[i].hasRoughnessMap = true;
        }
        createSrvAt(
            roughnessSrv,
            hasRoughness ? m_gpuMaterials[i].roughnessTexture.Get() : m_defaultWhiteTexture.Get(),
            hasRoughness ? roughnessFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        bool hasAO = false;
        DXGI_FORMAT aoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<std::filesystem::path> aoCandidates;
        if (!mesh.materials[i].aoTexture.empty())
            pushUniquePath(aoCandidates, resolveMaterialTexture(mesh.materials[i].aoTexture));
        appendPbrSiblingCandidates(aoCandidates, { "_AO", "AO", "_Occlusion", "Occlusion" });
        if (tryLoadTextureCandidates(aoCandidates, m_gpuMaterials[i].aoTexture, m_gpuMaterials[i].aoTextureUpload, aoFormat))
        {
            hasAO = true;
            m_gpuMaterials[i].hasAOMap = true;
        }
        createSrvAt(
            aoSrv,
            hasAO ? m_gpuMaterials[i].aoTexture.Get() : m_defaultWhiteTexture.Get(),
            hasAO ? aoFormat : DXGI_FORMAT_R8G8B8A8_UNORM);

        std::ostringstream materialLog;
        materialLog << "[Material] name=" << mesh.materials[i].name
            << " diffuse=" << mesh.materials[i].diffuseTexture
            << " normal=" << mesh.materials[i].normalTexture
            << " metallic=" << mesh.materials[i].metallicTexture
            << " roughness=" << mesh.materials[i].roughnessTexture
            << " hasDiffuseMap=" << (hasDiffuse ? "true" : "false")
            << " hasNormalMap=" << (hasNormal ? "true" : "false")
            << " hasMetallicMap=" << (hasMetallic ? "true" : "false")
            << " hasRoughnessMap=" << (hasRoughness ? "true" : "false")
            << "\n";
        OutputDebugStringA(materialLog.str().c_str());
    }

    CreateBuffer(
        verts.data(),
        static_cast<UINT>(verts.size() * sizeof(Vertex)),
        &m_vertexBuffer);

    CreateBuffer(
        mesh.indices.data(),
        static_cast<UINT>(mesh.indices.size() * sizeof(UINT)),
        &m_indexBuffer);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(Vertex);
    m_vbView.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(Vertex));

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.Format = DXGI_FORMAT_R32_UINT;
    m_ibView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(UINT));

    ThrowIfFailedRenderer(m_cmdList->Close());
    ID3D12CommandList* cmdLists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, cmdLists);
    WaitForGPU();

    return true;
}

int Renderer::LoadTextureToSrv(const std::wstring& texturePath)
{
    if (m_nextSrvIndex >= 256)
    {
        OutputDebugStringA("[Billboard] SRV heap is full\n");
        return -1;
    }

    TextureLoader::TextureData texData;
    if (!TextureLoader::LoadFromFile(texturePath, texData))
    {
        OutputDebugStringW((std::wstring(L"[Billboard] Failed to load texture: ") + texturePath + L"\n").c_str());
        return -1;
    }

    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;

    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

    if (!TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(), texData, texture, upload))
    {
        OutputDebugStringA("[Billboard] Failed to create GPU texture\n");
        ThrowIfFailedRenderer(m_cmdList->Close());
        ID3D12CommandList* lists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, lists);
        WaitForGPU();
        return -1;
    }

    const UINT heapIndex = m_nextSrvIndex++;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texData.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        heapIndex,
        m_cbvSrvDescSize);

    m_device->CreateShaderResourceView(texture.Get(), &srvDesc, cpuHandle);

    ThrowIfFailedRenderer(m_cmdList->Close());
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    // SRV only stores a descriptor; the underlying resources must stay alive.
    m_extraTextures.push_back(texture);
    m_extraTextureUploads.push_back(upload);

    std::wstring msg = L"[Billboard] Loaded texture: " + texturePath + L"\n";
    OutputDebugStringW(msg.c_str());

    return static_cast<int>(heapIndex);
}

int Renderer::CreateMaterialSrvBlockFromAlbedo(UINT albedoSrvIndex)
{
    if (m_nextSrvIndex + 5 >= 256 || !m_device || !m_cbvSrvHeap || !m_defaultWhiteTexture || !m_defaultBlackTexture)
    {
        OutputDebugStringA("[Billboard] Cannot allocate six-descriptor material SRV block\n");
        return -1;
    }

    const UINT baseIndex = m_nextSrvIndex;
    m_nextSrvIndex += 6;

    const D3D12_CPU_DESCRIPTOR_HANDLE sourceAlbedo = GetSrvCpuHandle(albedoSrvIndex);
    const D3D12_CPU_DESCRIPTOR_HANDLE destAlbedo = GetSrvCpuHandle(baseIndex);
    m_device->CopyDescriptorsSimple(1, destAlbedo, sourceAlbedo, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto createDefaultSrv = [&](UINT heapIndex, ID3D12Resource* resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(resource, &srvDesc, GetSrvCpuHandle(heapIndex));
    };

    // GeometryPass always expects a six-SRV material table:
    // albedo, normal, displacement, metallic, roughness and AO.
    createDefaultSrv(baseIndex + 1, m_defaultWhiteTexture.Get()); // normal is not sampled when HasNormalMap=0
    createDefaultSrv(baseIndex + 2, m_defaultWhiteTexture.Get()); // displacement is not sampled when HasDisplacementMap=0
    createDefaultSrv(baseIndex + 3, m_defaultBlackTexture.Get()); // metallic fallback must be black/0
    createDefaultSrv(baseIndex + 4, m_defaultWhiteTexture.Get()); // roughness fallback texture is white/1
    createDefaultSrv(baseIndex + 5, m_defaultWhiteTexture.Get()); // AO fallback is white/1

    return static_cast<int>(baseIndex);
}

bool Renderer::LoadPrimitiveCubeScene()
{
    struct PrimitiveVertex
    {
        XMFLOAT3 p;
        XMFLOAT3 n;
        XMFLOAT2 uv;
        XMFLOAT3 t;
        XMFLOAT3 b;
    };

    const PrimitiveVertex vertices[] =
    {
        {{-0.5f, -0.5f, -0.5f}, {0, 0,-1}, {0,1}, {1,0,0}, {0,1,0}}, {{ 0.5f, -0.5f, -0.5f}, {0, 0,-1}, {1,1}, {1,0,0}, {0,1,0}}, {{ 0.5f,  0.5f, -0.5f}, {0, 0,-1}, {1,0}, {1,0,0}, {0,1,0}}, {{-0.5f,  0.5f, -0.5f}, {0, 0,-1}, {0,0}, {1,0,0}, {0,1,0}},
        {{-0.5f, -0.5f,  0.5f}, {0, 0, 1}, {0,1}, {-1,0,0}, {0,1,0}}, {{-0.5f,  0.5f,  0.5f}, {0, 0, 1}, {0,0}, {-1,0,0}, {0,1,0}}, {{ 0.5f,  0.5f,  0.5f}, {0, 0, 1}, {1,0}, {-1,0,0}, {0,1,0}}, {{ 0.5f, -0.5f,  0.5f}, {0, 0, 1}, {1,1}, {-1,0,0}, {0,1,0}},
        {{-0.5f, -0.5f,  0.5f}, {-1,0, 0}, {0,1}, {0,0,-1}, {0,1,0}}, {{-0.5f, -0.5f, -0.5f}, {-1,0, 0}, {1,1}, {0,0,-1}, {0,1,0}}, {{-0.5f,  0.5f, -0.5f}, {-1,0, 0}, {1,0}, {0,0,-1}, {0,1,0}}, {{-0.5f,  0.5f,  0.5f}, {-1,0, 0}, {0,0}, {0,0,-1}, {0,1,0}},
        {{ 0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0,1}, {0,0,1}, {0,1,0}}, {{ 0.5f, -0.5f,  0.5f}, {1, 0, 0}, {1,1}, {0,0,1}, {0,1,0}}, {{ 0.5f,  0.5f,  0.5f}, {1, 0, 0}, {1,0}, {0,0,1}, {0,1,0}}, {{ 0.5f,  0.5f, -0.5f}, {1, 0, 0}, {0,0}, {0,0,1}, {0,1,0}},
        {{-0.5f,  0.5f, -0.5f}, {0, 1, 0}, {0,1}, {1,0,0}, {0,0,-1}}, {{ 0.5f,  0.5f, -0.5f}, {0, 1, 0}, {1,1}, {1,0,0}, {0,0,-1}}, {{ 0.5f,  0.5f,  0.5f}, {0, 1, 0}, {1,0}, {1,0,0}, {0,0,-1}}, {{-0.5f,  0.5f,  0.5f}, {0, 1, 0}, {0,0}, {1,0,0}, {0,0,-1}},
        {{-0.5f, -0.5f,  0.5f}, {0,-1, 0}, {0,1}, {1,0,0}, {0,0,1}}, {{ 0.5f, -0.5f,  0.5f}, {0,-1, 0}, {1,1}, {1,0,0}, {0,0,1}}, {{ 0.5f, -0.5f, -0.5f}, {0,-1, 0}, {1,0}, {1,0,0}, {0,0,1}}, {{-0.5f, -0.5f, -0.5f}, {0,-1, 0}, {0,0}, {1,0,0}, {0,0,1}},
    };

    const UINT indices[] =
    {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
    };

    std::vector<Vertex> verts;
    verts.reserve(_countof(vertices));
    for (const auto& v : vertices)
    {
        Vertex out{};
        out.Position = v.p;
        out.Normal = v.n;
        out.TexCoord = v.uv;
        out.Tangent = v.t;
        out.Bitangent = v.b;
        verts.push_back(out);
    }

    m_subsets.clear();
    MeshSubset s{};
    s.indexStart = 0;
    s.indexCount = _countof(indices);
    s.materialIdx = 0;
    m_subsets.push_back(s);

    m_gpuMaterials.clear();
    m_gpuMaterials.resize(1);
    m_gpuMaterials[0].diffuse = XMFLOAT4(0.72f, 0.72f, 0.78f, 1.0f);
    m_gpuMaterials[0].specular = XMFLOAT4(1, 1, 1, 1);
    m_gpuMaterials[0].specPower = 32.0f;
    m_gpuMaterials[0].diffuseSrvHeapIndex = 0;
    m_gpuMaterials[0].normalSrvHeapIndex = 0;
    m_gpuMaterials[0].displacementSrvHeapIndex = 0;
    m_gpuMaterials[0].metallicSrvHeapIndex = 0;
    m_gpuMaterials[0].roughnessSrvHeapIndex = 0;
    m_gpuMaterials[0].aoSrvHeapIndex = 0;
    m_gpuMaterials[0].hasDiffuseMap = false;
    m_gpuMaterials[0].hasNormalMap = false;
    m_gpuMaterials[0].hasDisplacementMap = false;
    m_gpuMaterials[0].hasMetallicMap = false;
    m_gpuMaterials[0].hasRoughnessMap = false;
    m_gpuMaterials[0].hasAOMap = false;

    CreateBuffer(verts.data(), static_cast<UINT>(verts.size() * sizeof(Vertex)), &m_vertexBuffer);
    CreateBuffer(indices, sizeof(indices), &m_indexBuffer);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(Vertex);
    m_vbView.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(Vertex));

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.Format = DXGI_FORMAT_R32_UINT;
    m_ibView.SizeInBytes = sizeof(indices);
    return true;
}

bool Renderer::LoadPrimitivePlaneScene()
{
    const float half = 600.0f;
    const Vertex vertices[] =
    {
        { XMFLOAT3(-half, 0.0f, -half), XMFLOAT3(0,1,0), XMFLOAT2(0,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( half, 0.0f, -half), XMFLOAT3(0,1,0), XMFLOAT2(1,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( half, 0.0f,  half), XMFLOAT3(0,1,0), XMFLOAT2(1,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3(-half, 0.0f,  half), XMFLOAT3(0,1,0), XMFLOAT2(0,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
    };

    const UINT indices[] = { 0,2,1, 0,3,2 };

    m_subsets.clear();
    MeshSubset subset{};
    subset.indexStart = 0;
    subset.indexCount = _countof(indices);
    subset.materialIdx = 0;
    m_subsets.push_back(subset);

    m_gpuMaterials.clear();
    m_gpuMaterials.resize(1);
    m_gpuMaterials[0].diffuse = XMFLOAT4(0.55f, 0.65f, 0.72f, 1.0f);
    m_gpuMaterials[0].specular = XMFLOAT4(0.7f, 0.7f, 0.7f, 1.0f);
    m_gpuMaterials[0].specPower = 24.0f;
    m_gpuMaterials[0].diffuseSrvHeapIndex = 0;
    m_gpuMaterials[0].normalSrvHeapIndex = 0;
    m_gpuMaterials[0].displacementSrvHeapIndex = 0;
    m_gpuMaterials[0].metallicSrvHeapIndex = 0;
    m_gpuMaterials[0].roughnessSrvHeapIndex = 0;
    m_gpuMaterials[0].aoSrvHeapIndex = 0;
    m_gpuMaterials[0].hasDiffuseMap = false;
    m_gpuMaterials[0].hasNormalMap = false;
    m_gpuMaterials[0].hasDisplacementMap = false;
    m_gpuMaterials[0].hasMetallicMap = false;
    m_gpuMaterials[0].hasRoughnessMap = false;
    m_gpuMaterials[0].hasAOMap = false;

    CreateBuffer(vertices, sizeof(vertices), &m_vertexBuffer);
    CreateBuffer(indices, sizeof(indices), &m_indexBuffer);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(Vertex);
    m_vbView.SizeInBytes = sizeof(vertices);

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.Format = DXGI_FORMAT_R32_UINT;
    m_ibView.SizeInBytes = sizeof(indices);
    return true;
}


bool Renderer::LoadAlphaTestShadowScene()
{
    const float floorHalf = 180.0f;
    const float fenceHalfWidth = 42.0f;
    const float fenceHeight = 78.0f;
    const float fenceZ = -12.0f;

    const Vertex vertices[] =
    {
        // Opaque floor that receives the generated fence cutout shadow.
        { XMFLOAT3(-floorHalf, 0.0f, -floorHalf), XMFLOAT3(0,1,0), XMFLOAT2(0,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( floorHalf, 0.0f, -floorHalf), XMFLOAT3(0,1,0), XMFLOAT2(1,1), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3( floorHalf, 0.0f,  floorHalf), XMFLOAT3(0,1,0), XMFLOAT2(1,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },
        { XMFLOAT3(-floorHalf, 0.0f,  floorHalf), XMFLOAT3(0,1,0), XMFLOAT2(0,0), XMFLOAT3(1,0,0), XMFLOAT3(0,0,1) },

        // Vertical X/Y quad using the generated alpha texture; transparent gaps
        // should neither draw in the GBuffer nor write into the shadow map.
        { XMFLOAT3(-fenceHalfWidth, 0.5f, fenceZ), XMFLOAT3(0,0,-1), XMFLOAT2(0,1), XMFLOAT3(1,0,0), XMFLOAT3(0,1,0) },
        { XMFLOAT3(-fenceHalfWidth, fenceHeight, fenceZ), XMFLOAT3(0,0,-1), XMFLOAT2(0,0), XMFLOAT3(1,0,0), XMFLOAT3(0,1,0) },
        { XMFLOAT3( fenceHalfWidth, fenceHeight, fenceZ), XMFLOAT3(0,0,-1), XMFLOAT2(1,0), XMFLOAT3(1,0,0), XMFLOAT3(0,1,0) },
        { XMFLOAT3( fenceHalfWidth, 0.5f, fenceZ), XMFLOAT3(0,0,-1), XMFLOAT2(1,1), XMFLOAT3(1,0,0), XMFLOAT3(0,1,0) },
    };

    const UINT indices[] =
    {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7
    };

    m_nextSrvIndex = 16;
    m_subsets.clear();
    MeshSubset floorSubset{};
    floorSubset.indexStart = 0;
    floorSubset.indexCount = 6;
    floorSubset.materialIdx = 0;
    m_subsets.push_back(floorSubset);

    MeshSubset fenceSubset{};
    fenceSubset.indexStart = 6;
    fenceSubset.indexCount = 6;
    fenceSubset.materialIdx = 1;
    m_subsets.push_back(fenceSubset);

    m_gpuMaterials.clear();
    m_gpuMaterials.resize(2);
    m_gpuMaterials[0].diffuse = XMFLOAT4(0.72f, 0.72f, 0.68f, 1.0f);
    m_gpuMaterials[0].specular = XMFLOAT4(0.08f, 0.08f, 0.08f, 1.0f);
    m_gpuMaterials[0].specPower = 16.0f;
    m_gpuMaterials[0].diffuseSrvHeapIndex = -1;
    m_gpuMaterials[0].normalSrvHeapIndex = -1;
    m_gpuMaterials[0].displacementSrvHeapIndex = -1;
    m_gpuMaterials[0].metallicSrvHeapIndex = -1;
    m_gpuMaterials[0].roughnessSrvHeapIndex = -1;
    m_gpuMaterials[0].aoSrvHeapIndex = -1;
    m_gpuMaterials[0].hasDiffuseMap = false;
    m_gpuMaterials[0].hasNormalMap = false;
    m_gpuMaterials[0].hasDisplacementMap = false;
    m_gpuMaterials[0].hasMetallicMap = false;
    m_gpuMaterials[0].hasRoughnessMap = false;
    m_gpuMaterials[0].hasAOMap = false;

    constexpr UINT texWidth = 128;
    constexpr UINT texHeight = 64;
    TextureLoader::TextureData fenceTex{};
    fenceTex.width = texWidth;
    fenceTex.height = texHeight;
    fenceTex.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fenceTex.rowPitch = texWidth * 4;
    fenceTex.pixels.resize(static_cast<size_t>(fenceTex.rowPitch) * texHeight);

    for (UINT y = 0; y < texHeight; ++y)
    {
        for (UINT x = 0; x < texWidth; ++x)
        {
            const bool verticalBar = (x % 16u) < 7u;
            const bool topRail = y < 7u;
            const bool bottomRail = y >= texHeight - 7u;
            const bool opaque = verticalBar || topRail || bottomRail;
            const size_t idx = (static_cast<size_t>(y) * texWidth + x) * 4u;
            fenceTex.pixels[idx + 0] = opaque ? 220 : 0;
            fenceTex.pixels[idx + 1] = opaque ? 220 : 0;
            fenceTex.pixels[idx + 2] = opaque ? 220 : 0;
            fenceTex.pixels[idx + 3] = opaque ? 255 : 0;
        }
    }

    ComPtr<ID3D12Resource> fenceTexture;
    ComPtr<ID3D12Resource> fenceUpload;

    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

    if (!TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(), fenceTex, fenceTexture, fenceUpload))
    {
        ThrowIfFailedRenderer(m_cmdList->Close());
        ID3D12CommandList* lists[] = { m_cmdList.Get() };
        m_cmdQueue->ExecuteCommandLists(1, lists);
        WaitForGPU();
        return false;
    }

    const UINT fenceSrv = m_nextSrvIndex++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = fenceTex.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        fenceSrv,
        m_cbvSrvDescSize);
    m_device->CreateShaderResourceView(fenceTexture.Get(), &srvDesc, cpuHandle);

    ThrowIfFailedRenderer(m_cmdList->Close());
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();

    m_gpuMaterials[1].diffuse = XMFLOAT4(0.86f, 0.86f, 0.82f, 1.0f);
    m_gpuMaterials[1].specular = XMFLOAT4(0.05f, 0.05f, 0.05f, 1.0f);
    m_gpuMaterials[1].specPower = 8.0f;
    m_gpuMaterials[1].diffuseSrvHeapIndex = static_cast<int>(fenceSrv);
    m_gpuMaterials[1].normalSrvHeapIndex = -1;
    m_gpuMaterials[1].displacementSrvHeapIndex = -1;
    m_gpuMaterials[1].metallicSrvHeapIndex = -1;
    m_gpuMaterials[1].roughnessSrvHeapIndex = -1;
    m_gpuMaterials[1].aoSrvHeapIndex = -1;
    m_gpuMaterials[1].hasDiffuseMap = true;
    m_gpuMaterials[1].hasNormalMap = false;
    m_gpuMaterials[1].hasDisplacementMap = false;
    m_gpuMaterials[1].hasMetallicMap = false;
    m_gpuMaterials[1].hasRoughnessMap = false;
    m_gpuMaterials[1].hasAOMap = false;
    m_gpuMaterials[1].diffuseTexture = fenceTexture;
    m_gpuMaterials[1].diffuseTextureUpload = fenceUpload;
    m_extraTextures.push_back(fenceTexture);
    m_extraTextureUploads.push_back(fenceUpload);

    CreateBuffer(vertices, sizeof(vertices), &m_vertexBuffer);
    CreateBuffer(indices, sizeof(indices), &m_indexBuffer);

    m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(Vertex);
    m_vbView.SizeInBytes = sizeof(vertices);

    m_ibView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibView.Format = DXGI_FORMAT_R32_UINT;
    m_ibView.SizeInBytes = sizeof(indices);

    return true;
}

bool Renderer::LoadMassPrimitiveScene()
{
    return LoadPrimitiveCubeScene();
}
