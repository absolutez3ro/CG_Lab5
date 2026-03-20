#include "GBuffer.h"
#include <stdexcept>

static void ThrowIfFailedGBuffer(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("DirectX call failed");
}

bool GBuffer::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart,
    UINT rtvDescriptorSize,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
    UINT srvDescriptorSize)
{
    Release();

    if (!CreateResources(device, width, height))
        return false;

    CreateRtvDescriptors(device, rtvStart, rtvDescriptorSize);
    CreateSrvDescriptors(device, srvCpuStart, srvGpuStart, srvDescriptorSize);
    return true;
}

bool GBuffer::Resize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart,
    UINT rtvDescriptorSize,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
    UINT srvDescriptorSize)
{
    return Initialize(device, width, height, rtvStart, rtvDescriptorSize, srvCpuStart, srvGpuStart, srvDescriptorSize);
}

bool GBuffer::CreateResources(ID3D12Device* device, UINT width, UINT height)
{
    m_width = width;
    m_height = height;

    for (UINT i = 0; i < BufferCount; ++i)
    {
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = m_formats[i];
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = (i == Material) ? 32.0f / 255.0f : 1.0f;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = m_formats[i];
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailedGBuffer(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue,
            IID_PPV_ARGS(&m_targets[i])));

        m_currentStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    return true;
}

void GBuffer::CreateRtvDescriptors(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE rtvStart, UINT rtvDescriptorSize)
{
    for (UINT i = 0; i < BufferCount; ++i)
    {
        m_rtvHandles[i] = rtvStart;
        device->CreateRenderTargetView(m_targets[i].Get(), nullptr, m_rtvHandles[i]);
        rtvStart.ptr += rtvDescriptorSize;
    }
}

void GBuffer::CreateSrvDescriptors(
    ID3D12Device* device,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
    UINT srvDescriptorSize)
{
    for (UINT i = 0; i < BufferCount; ++i)
    {
        m_srvCpuHandles[i] = srvCpuStart;
        m_srvGpuHandles[i] = srvGpuStart;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = m_formats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, m_srvCpuHandles[i]);

        srvCpuStart.ptr += srvDescriptorSize;
        srvGpuStart.ptr += srvDescriptorSize;
    }
}

void GBuffer::Release()
{
    for (UINT i = 0; i < BufferCount; ++i)
    {
        m_targets[i].Reset();
        m_currentStates[i] = D3D12_RESOURCE_STATE_COMMON;
        m_rtvHandles[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_srvCpuHandles[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
        m_srvGpuHandles[i] = D3D12_GPU_DESCRIPTOR_HANDLE{};
    }

    m_width = 0;
    m_height = 0;
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList) const
{
    static const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
    static const float normalClear[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
    static const float materialClear[4] = { 0.f, 0.f, 0.f, 32.0f / 255.0f };

    cmdList->ClearRenderTargetView(m_rtvHandles[Albedo], zero, 0, nullptr);
    cmdList->ClearRenderTargetView(m_rtvHandles[Normal], normalClear, 0, nullptr);
    cmdList->ClearRenderTargetView(m_rtvHandles[Material], materialClear, 0, nullptr);
}

void GBuffer::TransitionAll(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState)
{
    D3D12_RESOURCE_BARRIER barriers[BufferCount]{};
    UINT barrierCount = 0;

    for (UINT i = 0; i < BufferCount; ++i)
    {
        if (m_currentStates[i] == newState)
            continue;

        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            m_targets[i].Get(),
            m_currentStates[i],
            newState);

        m_currentStates[i] = newState;
    }

    if (barrierCount > 0)
        cmdList->ResourceBarrier(barrierCount, barriers);
}

void GBuffer::BeginGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    TransitionAll(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    TransitionAll(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
