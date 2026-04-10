#include "Renderer.h"
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <cctype>

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
    TextureLoader::TextureData defaultTex;
    defaultTex.width = 1;
    defaultTex.height = 1;
    defaultTex.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    defaultTex.rowPitch = 4;
    defaultTex.pixels = { 255, 255, 255, 255 };

    ThrowIfFailedRenderer(m_cmdAllocators[0]->Reset());
    ThrowIfFailedRenderer(m_cmdList->Reset(m_cmdAllocators[0].Get(), nullptr));

    if (!TextureLoader::CreateTexture(
        m_device.Get(),
        m_cmdList.Get(),
        defaultTex,
        m_defaultWhiteTexture,
        m_defaultWhiteUpload))
    {
        throw std::runtime_error("Failed to create default texture");
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


void Renderer::TransitionDepthToShaderResource()
{
    if (m_depthState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        return;

    auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_depthStencil.Get(),
        m_depthState,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_cmdList->ResourceBarrier(1, &depthBarrier);
    m_depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void Renderer::CreateBuffer(const void* data, UINT size, ID3D12Resource** resource)
{
    auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    ThrowIfFailedRenderer(m_device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource)));
    if (data)
    {
        void* mapped = nullptr;
        (*resource)->Map(0, nullptr, &mapped);
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

bool Renderer::LoadObj(const std::string& path)
{
    ObjMesh mesh;
    if (!ObjLoader::Load(path, mesh))
        return false;

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

    m_subsets = mesh.subsets;
    m_gpuMaterials.clear();
    m_gpuMaterials.resize(mesh.materials.size());
    m_nextSrvIndex = 8;

    const std::filesystem::path baseDir = std::filesystem::path(path).parent_path();

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
        m_gpuMaterials[i].hasNormalMap = false;
        m_gpuMaterials[i].hasDisplacementMap = false;
        m_gpuMaterials[i].displacementScale = 0.0f;
        m_gpuMaterials[i].displacementBias = 0.0f;

        if (m_nextSrvIndex + 2 >= 256)
            continue;

        const UINT diffuseSrv = m_nextSrvIndex++;
        const UINT normalSrv = m_nextSrvIndex++;
        const UINT displacementSrv = m_nextSrvIndex++;
        m_gpuMaterials[i].diffuseSrvHeapIndex = static_cast<int>(diffuseSrv);
        m_gpuMaterials[i].normalSrvHeapIndex = static_cast<int>(normalSrv);
        m_gpuMaterials[i].displacementSrvHeapIndex = static_cast<int>(displacementSrv);

        bool hasDiffuse = false;
        DXGI_FORMAT diffuseFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!mesh.materials[i].diffuseTexture.empty())
        {
            std::filesystem::path texPath = baseDir / mesh.materials[i].diffuseTexture;
            if (tryLoadTexture(
                texPath,
                m_gpuMaterials[i].diffuseTexture,
                m_gpuMaterials[i].diffuseTextureUpload,
                diffuseFormat))
            {
                hasDiffuse = true;
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
                    normalCandidates.push_back(baseDir / normalRel);
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
                    normalCandidates.push_back(baseDir / (diffuseParent / (stemName + diffuseExt)));
                };

                pushNormalCandidate(diffuseStem + "_ddn");

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
                    displacementCandidates.push_back(baseDir / dispRel);
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
