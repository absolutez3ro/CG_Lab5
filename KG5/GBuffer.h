#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

class GBuffer
{
public:
    static constexpr UINT BufferCount = 3;
    enum Target : UINT
    {
        Albedo = 0,
        Normal = 1,
        Material = 2,
    };

    bool Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart,
        UINT rtvDescriptorSize,
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
        UINT srvDescriptorSize);

    void Release();
    bool Resize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart,
        UINT rtvDescriptorSize,
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
        UINT srvDescriptorSize);

    void Clear(ID3D12GraphicsCommandList* cmdList) const;
    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmdList);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(UINT index) const { return m_rtvHandles[index]; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetFirstSRVGpu() const { return m_srvGpuHandles[0]; }
    DXGI_FORMAT GetFormat(UINT index) const { return m_formats[index]; }

private:
    bool CreateResources(
        ID3D12Device* device,
        UINT width,
        UINT height,
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart,
        UINT rtvDescriptorSize,
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
        UINT srvDescriptorSize);

private:
    ComPtr<ID3D12Resource> m_targets[BufferCount];
    DXGI_FORMAT m_formats[BufferCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandles[BufferCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpuHandles[BufferCount]{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpuHandles[BufferCount]{};
    UINT m_width = 0;
    UINT m_height = 0;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
