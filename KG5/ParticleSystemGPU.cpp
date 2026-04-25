#include "ParticleSystemGPU.h"
#include "d3dx12.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <sstream>
#include <vector>
#include <cwchar>
#include <cstdio>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static void PS_ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr)) throw std::runtime_error(msg);
}

bool ParticleSystemGPU::Initialize(Renderer* renderer)
{
    if (m_initialized)
        return true;

    m_renderer = renderer;
    if (!m_renderer)
        return false;

    try
    {
        OutputDebugStringA("[Particles] Init\n");
        if (!CreateRootSignatures()) return false;
        if (!CreatePipelines()) return false;
        if (!CreateResources()) return false;
        if (!CreateQuadGeometry()) return false;
        m_initialized = true;
    }
    catch (const std::exception& ex)
    {
        std::string msg = std::string("[Particles] Init failed: ") + ex.what() + "\n";
        OutputDebugStringA(msg.c_str());
        return false;
    }

    return true;
}

void ParticleSystemGPU::Shutdown()
{
    m_initialized = false;
}

std::string ParticleSystemGPU::GetExeDir() const
{
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    size_t p = path.find_last_of("\\/");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

bool ParticleSystemGPU::CompileShader(const wchar_t* file, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob)
{
    const std::string exeDir = GetExeDir();
    const std::wstring exeDirW(exeDir.begin(), exeDir.end());
    const std::wstring fileW(file);
    std::vector<std::wstring> candidates = {
        fileW,
        exeDirW + fileW,
        exeDirW + L"..\\..\\" + fileW,
        exeDirW + L"..\\..\\..\\" + fileW,
        exeDirW + L"..\\..\\..\\KG5\\" + fileW,
    };

    HRESULT lastHr = E_FAIL;
    std::wstring lastPath = fileW;
    std::string lastErr;
    outBlob.Reset();

    for (const auto& p : candidates)
    {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(p.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
            D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &outBlob, &errors);
        if (SUCCEEDED(hr) && outBlob)
            return true;

        lastHr = hr;
        lastPath = p;
        if (errors && errors->GetBufferPointer())
            lastErr = static_cast<const char*>(errors->GetBufferPointer());
    }

    std::string fileUtf8(file, file + std::wcslen(file));
    std::string pathUtf8(lastPath.begin(), lastPath.end());
    std::ostringstream oss;
    oss << "[Particles] Failed shader compile: file=" << fileUtf8
        << " entry=" << entry << " target=" << target
        << " hr=0x" << std::hex << static_cast<unsigned long>(lastHr)
        << " path=" << pathUtf8;
    if (!lastErr.empty()) oss << " err=" << lastErr;
    OutputDebugStringA((oss.str() + "\n").c_str());
    throw std::runtime_error(oss.str());
}

bool ParticleSystemGPU::CreateRootSignatures()
{
    auto device = m_renderer->GetDevice();

    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);
    CD3DX12_ROOT_PARAMETER computeParams[2];
    computeParams[0].InitAsConstantBufferView(0);
    computeParams[1].InitAsDescriptorTable(1, &uavRange);
    CD3DX12_ROOT_SIGNATURE_DESC csDesc;
    csDesc.Init(_countof(computeParams), computeParams, 0, nullptr);

    ComPtr<ID3DBlob> ser, err;
    PS_ThrowIfFailed(D3D12SerializeRootSignature(&csDesc, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err), "CS RS serialize failed");
    PS_ThrowIfFailed(device->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&m_computeRS)), "CS RS create failed");

    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
    CD3DX12_ROOT_PARAMETER renderParams[2];
    renderParams[0].InitAsConstantBufferView(0);
    renderParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(_countof(renderParams), renderParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ser.Reset(); err.Reset();
    PS_ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err), "Render RS serialize failed");
    PS_ThrowIfFailed(device->CreateRootSignature(0, ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&m_renderRS)), "Render RS create failed");
    return true;
}

bool ParticleSystemGPU::CreatePipelines()
{
    auto device = m_renderer->GetDevice();
    ComPtr<ID3DBlob> initCS, emitCS, updateCS, sortCS, vs, ps;
    CompileShader(L"ParticlesCS.hlsl", "InitDeadListCS", "cs_5_0", initCS);
    CompileShader(L"ParticlesCS.hlsl", "EmitCS", "cs_5_0", emitCS);
    CompileShader(L"ParticlesCS.hlsl", "UpdateCS", "cs_5_0", updateCS);
    CompileShader(L"ParticlesCS.hlsl", "BitonicSortCS", "cs_5_0", sortCS);
    CompileShader(L"ParticleRender.hlsl", "VSMain", "vs_5_0", vs);
    CompileShader(L"ParticleRender.hlsl", "PSMain", "ps_5_0", ps);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cps{};
    cps.pRootSignature = m_computeRS.Get();

    cps.CS = { initCS->GetBufferPointer(), initCS->GetBufferSize() };
    PS_ThrowIfFailed(device->CreateComputePipelineState(&cps, IID_PPV_ARGS(&m_initDeadPso)), "Init CS PSO failed");
    cps.CS = { emitCS->GetBufferPointer(), emitCS->GetBufferSize() };
    PS_ThrowIfFailed(device->CreateComputePipelineState(&cps, IID_PPV_ARGS(&m_emitPso)), "Emit CS PSO failed");
    cps.CS = { updateCS->GetBufferPointer(), updateCS->GetBufferSize() };
    PS_ThrowIfFailed(device->CreateComputePipelineState(&cps, IID_PPV_ARGS(&m_updatePso)), "Update CS PSO failed");
    cps.CS = { sortCS->GetBufferPointer(), sortCS->GetBufferSize() };
    PS_ThrowIfFailed(device->CreateComputePipelineState(&cps, IID_PPV_ARGS(&m_sortPso)), "Sort CS PSO failed");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gps{};
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
    };
    gps.InputLayout = { layout, 1 };
    gps.pRootSignature = m_renderRS.Get();
    gps.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gps.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    gps.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gps.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gps.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    gps.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gps.DepthStencilState.DepthEnable = TRUE;
    gps.SampleMask = UINT_MAX;
    gps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gps.NumRenderTargets = 1;
    gps.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gps.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gps.SampleDesc.Count = 1;
    PS_ThrowIfFailed(device->CreateGraphicsPipelineState(&gps, IID_PPV_ARGS(&m_renderPso)), "Particle render PSO failed");
    return true;
}

bool ParticleSystemGPU::CreateResources()
{
    auto device = m_renderer->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 8;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    PS_ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_particleHeap)), "Particle heap failed");
    m_descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto makeStructured = [&](UINT count, UINT stride, ComPtr<ID3D12Resource>& out)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(count) * stride, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        PS_ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)), "Structured resource failed");
    };

    makeStructured(MaxParticles, sizeof(GpuParticle), m_particlePool);
    makeStructured(MaxParticles, sizeof(uint32_t), m_deadList);
    makeStructured(MaxParticles, sizeof(ParticleSortEntry), m_sortList);

    auto makeCounter = [&](ComPtr<ID3D12Resource>& out)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        PS_ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)), "Counter resource failed");
    };
    makeCounter(m_deadListCounter);
    makeCounter(m_sortListCounter);

    auto hpUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto hpReadback = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto oneUint = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t));
    PS_ThrowIfFailed(device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &oneUint, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_counterResetUpload)), "Counter reset upload failed");
    for (int i = 0; i < 2; ++i)
    {
        PS_ThrowIfFailed(device->CreateCommittedResource(&hpReadback, D3D12_HEAP_FLAG_NONE, &oneUint, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_counterReadback[i])), "Counter readback failed");
        m_counterReadback[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_counterReadbackMapped[i]));
    }

    auto createUploadCB = [&](UINT size, ComPtr<ID3D12Resource>& out)
    {
        auto rd = CD3DX12_RESOURCE_DESC::Buffer((size + 255u) & ~255u);
        PS_ThrowIfFailed(device->CreateCommittedResource(&hpUpload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out)), "CB upload failed");
    };
    createUploadCB(sizeof(ParticleEmitConstants), m_emitCB);
    createUploadCB(sizeof(ParticleUpdateConstants), m_updateCB);
    createUploadCB(sizeof(SortConstants), m_sortCB);
    createUploadCB(sizeof(ParticleRenderConstants), m_renderCB);

    auto baseCpu = m_particleHeap->GetCPUDescriptorHandleForHeapStart();
    auto handle = baseCpu;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_UNKNOWN;

    uav.Buffer.NumElements = MaxParticles;
    uav.Buffer.StructureByteStride = sizeof(GpuParticle);
    device->CreateUnorderedAccessView(m_particlePool.Get(), nullptr, &uav, handle); //0 u0
    handle.ptr += m_descSize;

    uav.Buffer.NumElements = MaxParticles;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    uav.Buffer.CounterOffsetInBytes = 0;
    device->CreateUnorderedAccessView(m_deadList.Get(), m_deadListCounter.Get(), &uav, handle); //1 u1
    handle.ptr += m_descSize;

    uav.Buffer.NumElements = MaxParticles;
    uav.Buffer.StructureByteStride = sizeof(ParticleSortEntry);
    uav.Buffer.CounterOffsetInBytes = 0;
    device->CreateUnorderedAccessView(m_sortList.Get(), m_sortListCounter.Get(), &uav, handle); //2 u2
    handle.ptr += m_descSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.NumElements = MaxParticles;
    srv.Buffer.StructureByteStride = sizeof(GpuParticle);
    device->CreateShaderResourceView(m_particlePool.Get(), &srv, handle); //3 t0
    handle.ptr += m_descSize;

    srv.Buffer.NumElements = MaxParticles;
    srv.Buffer.StructureByteStride = sizeof(ParticleSortEntry);
    device->CreateShaderResourceView(m_sortList.Get(), &srv, handle); //4 t1

    return true;
}

bool ParticleSystemGPU::CreateQuadGeometry()
{
    QuadVertex vertices[4] = {
        { XMFLOAT2(-0.5f, -0.5f) },
        { XMFLOAT2(-0.5f,  0.5f) },
        { XMFLOAT2( 0.5f,  0.5f) },
        { XMFLOAT2( 0.5f, -0.5f) }
    };
    uint16_t indices[6] = { 0,1,2, 0,2,3 };

    m_renderer->CreateBuffer(vertices, sizeof(vertices), &m_quadVB);
    m_renderer->CreateBuffer(indices, sizeof(indices), &m_quadIB);

    m_quadVbView.BufferLocation = m_quadVB->GetGPUVirtualAddress();
    m_quadVbView.StrideInBytes = sizeof(QuadVertex);
    m_quadVbView.SizeInBytes = sizeof(vertices);

    m_quadIbView.BufferLocation = m_quadIB->GetGPUVirtualAddress();
    m_quadIbView.Format = DXGI_FORMAT_R16_UINT;
    m_quadIbView.SizeInBytes = sizeof(indices);
    return true;
}

void ParticleSystemGPU::TransitionComputeResources(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    if (m_particlePoolState != state)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_particlePool.Get(), m_particlePoolState, state));
        m_particlePoolState = state;
    }
    if (m_deadListState != state)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_deadList.Get(), m_deadListState, state));
        m_deadListState = state;
    }
    if (m_sortListState != state)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_sortList.Get(), m_sortListState, state));
        m_sortListState = state;
    }
    if (!barriers.empty()) cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

void ParticleSystemGPU::ResetCounter(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* counterResource, uint32_t value)
{
    void* mapped = nullptr;
    m_counterResetUpload->Map(0, nullptr, &mapped);
    memcpy(mapped, &value, sizeof(uint32_t));
    m_counterResetUpload->Unmap(0, nullptr);

    auto toCopyDst = CD3DX12_RESOURCE_BARRIER::Transition(counterResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(counterResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &toCopyDst);
    cmdList->CopyBufferRegion(counterResource, 0, m_counterResetUpload.Get(), 0, sizeof(uint32_t));
    cmdList->ResourceBarrier(1, &toUav);
}

void ParticleSystemGPU::CopyCounterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* counterResource, ID3D12Resource* readbackResource)
{
    auto toCopySrc = CD3DX12_RESOURCE_BARRIER::Transition(counterResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(counterResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &toCopySrc);
    cmdList->CopyBufferRegion(readbackResource, 0, counterResource, 0, sizeof(uint32_t));
    cmdList->ResourceBarrier(1, &toUav);
}

void ParticleSystemGPU::Reinitialize(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_initialized) return;

    TransitionComputeResources(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ResetCounter(cmdList, m_deadListCounter.Get(), 0);
    ResetCounter(cmdList, m_sortListCounter.Get(), 0);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_computeRS.Get());
    ParticleUpdateConstants initCb{};
    initCb.MaxParticles = MaxParticles;
    void* mapped = nullptr;
    m_updateCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &initCb, sizeof(initCb));
    m_updateCB->Unmap(0, nullptr);
    cmdList->SetComputeRootConstantBufferView(0, m_updateCB->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(1, m_particleHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetPipelineState(m_initDeadPso.Get());
    cmdList->Dispatch((MaxParticles + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmdList->ResourceBarrier(1, &uavBarrier);
    m_aliveCountForDraw = 0;
}

void ParticleSystemGPU::DispatchBitonicSort(ID3D12GraphicsCommandList* cmdList, uint32_t elementCount)
{
    if (elementCount < 2) return;

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_computeRS.Get());
    cmdList->SetComputeRootDescriptorTable(1, m_particleHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetPipelineState(m_sortPso.Get());

    for (uint32_t subArray = 2; subArray <= elementCount; subArray <<= 1)
    {
        for (uint32_t compare = subArray >> 1; compare > 0; compare >>= 1)
        {
            SortConstants sc{};
            sc.ElementCount = elementCount;
            sc.SubArraySize = subArray;
            sc.CompareDistance = compare;
            sc.SortDescending = 1;

            void* mapped = nullptr;
            m_sortCB->Map(0, nullptr, &mapped);
            memcpy(mapped, &sc, sizeof(sc));
            m_sortCB->Unmap(0, nullptr);

            cmdList->SetComputeRootConstantBufferView(0, m_sortCB->GetGPUVirtualAddress());
            cmdList->Dispatch((elementCount + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);
            auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_sortList.Get());
            cmdList->ResourceBarrier(1, &uavBarrier);
        }
    }
}

void ParticleSystemGPU::Update(ID3D12GraphicsCommandList* cmdList, float deltaTime, float totalTime, const XMFLOAT3& cameraPos, const XMFLOAT3& emitterPos, const FountainSettings& settings)
{
    if (!m_initialized)
        return;

    if ((m_frameCounter % 240u) == 0)
    {
        OutputDebugStringA(m_sortEnabled ? "[Particles] Sort enabled\n" : "[Particles] Sort disabled\n");
    }

    TransitionComputeResources(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_frameCounter == 0)
    {
        Reinitialize(cmdList);
    }

    ResetCounter(cmdList, m_sortListCounter.Get(), 0);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(m_computeRS.Get());
    cmdList->SetComputeRootDescriptorTable(1, m_particleHeap->GetGPUDescriptorHandleForHeapStart());

    if (m_enabled)
    {
        ParticleEmitConstants emit{};
        emit.EmitterPosition = emitterPos;
        emit.EmitCount = settings.EmitPerFrame;
        emit.BaseVelocity = settings.BaseVelocity;
        emit.Time = totalTime;
        emit.VelocityRandomness = settings.VelocityRandomness;
        emit.MinLifeSpan = settings.MinLifeSpan;
        emit.MaxLifeSpan = settings.MaxLifeSpan;
        emit.MinSize = settings.MinSize;
        emit.MaxSize = settings.MaxSize;
        emit.RandomSeed = 1337u + m_frameCounter;
        emit.StartColorA = settings.StartColorA;
        emit.StartColorB = settings.StartColorB;

        void* mapped = nullptr;
        m_emitCB->Map(0, nullptr, &mapped);
        memcpy(mapped, &emit, sizeof(emit));
        m_emitCB->Unmap(0, nullptr);

        cmdList->SetPipelineState(m_emitPso.Get());
        cmdList->SetComputeRootConstantBufferView(0, m_emitCB->GetGPUVirtualAddress());
        cmdList->Dispatch((emit.EmitCount + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);

        if ((m_frameCounter % 120u) == 0)
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "[Particles] Emit count: %u\n", emit.EmitCount);
            OutputDebugStringA(msg);
        }
    }

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    cmdList->ResourceBarrier(1, &uavBarrier);

    ParticleUpdateConstants update{};
    update.DeltaTime = deltaTime;
    update.TotalTime = totalTime;
    update.MaxParticles = MaxParticles;
    update.Gravity = settings.Gravity;
    update.GroundY = settings.GroundY;
    update.CameraPosition = cameraPos;
    update.EnableGroundCollision = settings.EnableGroundCollision;

    void* mapped = nullptr;
    m_updateCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &update, sizeof(update));
    m_updateCB->Unmap(0, nullptr);

    cmdList->SetPipelineState(m_updatePso.Get());
    cmdList->SetComputeRootConstantBufferView(0, m_updateCB->GetGPUVirtualAddress());
    cmdList->Dispatch((MaxParticles + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);
    cmdList->ResourceBarrier(1, &uavBarrier);

    CopyCounterToReadback(cmdList, m_sortListCounter.Get(), m_counterReadback[m_frameCounter % 2].Get());

    const uint32_t prevIndex = (m_frameCounter + 1) % 2;
    uint32_t alivePrev = m_counterReadbackMapped[prevIndex] ? *m_counterReadbackMapped[prevIndex] : 0u;
    m_aliveCountForDraw = (std::min)(alivePrev, MaxParticles);

    if (m_sortEnabled && m_aliveCountForDraw > 1)
    {
        uint32_t sortCount = 1;
        while (sortCount < m_aliveCountForDraw) sortCount <<= 1;
        sortCount = (std::min)(sortCount, MaxParticles);
        DispatchBitonicSort(cmdList, sortCount);
    }

    if ((m_frameCounter % 120u) == 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "[Particles] Alive count: %u\n", m_aliveCountForDraw);
        OutputDebugStringA(msg);
    }

    ++m_frameCounter;
}

void ParticleSystemGPU::Render(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    const XMFLOAT4X4& view,
    const XMFLOAT4X4& proj,
    const XMFLOAT3& cameraPos,
    const XMFLOAT3& directionalLightDir,
    float directionalIntensity,
    const XMFLOAT3& directionalLightColor,
    const XMFLOAT4& ambientColor)
{
    if (!m_initialized || !m_enabled || m_aliveCountForDraw == 0)
        return;

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    if (m_particlePoolState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_particlePool.Get(), m_particlePoolState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        m_particlePoolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (m_sortListState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(m_sortList.Get(), m_sortListState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        m_sortListState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (!barriers.empty()) cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    const XMMATRIX viewM = XMMatrixTranspose(XMLoadFloat4x4(&view));
    const XMMATRIX projM = XMMatrixTranspose(XMLoadFloat4x4(&proj));
    const XMMATRIX viewProj = viewM * projM;

    XMVECTOR eye = XMLoadFloat3(&cameraPos);
    XMVECTOR at = XMVectorAdd(eye, XMVector3Normalize(XMVectorSet(viewM.r[2].m128_f32[0], viewM.r[2].m128_f32[1], viewM.r[2].m128_f32[2], 0.0f)));
    (void)at;
    XMFLOAT3 right{}, up{};
    XMStoreFloat3(&right, XMVector3Normalize(XMVectorSet(viewM.r[0].m128_f32[0], viewM.r[1].m128_f32[0], viewM.r[2].m128_f32[0], 0.0f)));
    XMStoreFloat3(&up, XMVector3Normalize(XMVectorSet(viewM.r[0].m128_f32[1], viewM.r[1].m128_f32[1], viewM.r[2].m128_f32[1], 0.0f)));

    ParticleRenderConstants rc{};
    rc.View = view;
    rc.Proj = proj;
    XMStoreFloat4x4(&rc.ViewProj, XMMatrixTranspose(viewProj));
    rc.CameraRight = right;
    rc.CameraUp = up;
    rc.DirectionalLightDir = directionalLightDir;
    rc.DirectionalLightIntensity = directionalIntensity;
    rc.DirectionalLightColor = XMFLOAT4(directionalLightColor.x, directionalLightColor.y, directionalLightColor.z, 1.0f);
    rc.AmbientColor = ambientColor;

    void* mapped = nullptr;
    m_renderCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &rc, sizeof(rc));
    m_renderCB->Unmap(0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_particleHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmdList->SetGraphicsRootSignature(m_renderRS.Get());
    cmdList->SetPipelineState(m_renderPso.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, m_renderCB->GetGPUVirtualAddress());
    auto srvTable = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_particleHeap->GetGPUDescriptorHandleForHeapStart(), 3, m_descSize);
    cmdList->SetGraphicsRootDescriptorTable(1, srvTable);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_quadVbView);
    cmdList->IASetIndexBuffer(&m_quadIbView);
    cmdList->DrawIndexedInstanced(6, m_aliveCountForDraw, 0, 0, 0);

    if ((m_frameCounter % 120u) == 0)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "[Particles] Render instances: %u\n", m_aliveCountForDraw);
        OutputDebugStringA(msg);
    }
}
