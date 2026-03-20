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
        Albedo = 0,   // Base color in RGBA8
        Normal = 1,   // Encoded world normal in RGBA16F
        Material = 2, // Specular.rgb + shininess in alpha
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

    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList);
    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);
    void Clear(ID3D12GraphicsCommandList* cmdList) const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHandle(UINT index) const { return m_rtvHandles[index]; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(UINT index) const { return m_srvGpuHandles[index]; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetFirstSrvGpu() const { return m_srvGpuHandles[0]; }
    DXGI_FORMAT GetFormat(UINT index) const { return m_formats[index]; }

private:
    bool CreateResources(ID3D12Device* device, UINT width, UINT height);
    void CreateRtvDescriptors(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE rtvStart, UINT rtvDescriptorSize);
    void CreateSrvDescriptors(
        ID3D12Device* device,
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart,
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStart,
        UINT srvDescriptorSize);
    void TransitionAll(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

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
    D3D12_RESOURCE_STATES m_currentStates[BufferCount]{};
    UINT m_width = 0;
    UINT m_height = 0;
};
