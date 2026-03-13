#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "d3dx12.h"
#include "ObjLoader.h"
#include "TextureLoader.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Vertex {
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

// Константы объекта (для Geometry Pass)
struct alignas(256) ObjectConstants {
    XMFLOAT4X4 World;
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4X4 WorldInvTranspose;
    XMFLOAT4 MaterialDiffuse;
    XMFLOAT4 MaterialSpecular;
    float SpecularPower;
    int HasTexture;
    float Pad[2];
};

struct GpuMaterial {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> textureUpload;
    int srvHeapIndex = -1;
    XMFLOAT4 diffuse = { 1, 1, 1, 1 };
    XMFLOAT4 specular = { 1, 1, 1, 1 };
    float specPower = 32.0f;
};

class Renderer {
public:
    bool Init(HWND hwnd, int width, int height);
    void BeginFrame();
    void EndFrame();
    void OnResize(int width, int height);
    bool LoadObj(const std::string& path);

    ID3D12Device* GetDevice() { return m_device.Get(); }
    ID3D12GraphicsCommandList* GetCmdList() { return m_cmdList.Get(); }
    ID3D12DescriptorHeap* GetSrvHeap() { return m_cbvSrvHeap.Get(); }
    UINT GetRtvDescriptorSize() { return m_rtvDescSize; }
    UINT GetSrvDescriptorSize() { return m_cbvSrvDescSize; }
    UINT GetWidth() const { return static_cast<UINT>(m_width); }
    UINT GetHeight() const { return static_cast<UINT>(m_height); }

    // ВАЖНО: Хендлы для рендеринга
    D3D12_CPU_DESCRIPTOR_HANDLE GetBackBufferRtv() {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescSize);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetGbufferRtvStart() {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescSize);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetGbufferSrvCpuStart() {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_cbvSrvDescSize);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGbufferSrvGpuStart() {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), 1, m_cbvSrvDescSize);
    }

    const D3D12_VERTEX_BUFFER_VIEW* GetVbView() const { return &m_vbView; }
    const D3D12_INDEX_BUFFER_VIEW* GetIbView() const { return &m_ibView; }
    const std::vector<MeshSubset>& GetSubsets() const { return m_subsets; }
    const std::vector<GpuMaterial>& GetMaterials() const { return m_gpuMaterials; }

    void CreateBuffer(const void* data, UINT size, ID3D12Resource** resource);

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, int width, int height);
    void CreateDescriptorHeaps();
    void CreateRenderTargetViews();
    void CreateDepthStencilView();
    void CreateFence();
    void WaitForGPU();
    void MoveToNextFrame();

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_cmdQueue;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12CommandAllocator> m_cmdAllocators[2];
    ComPtr<IDXGISwapChain3> m_swapChain;
    UINT m_frameIndex = 0;

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap;
    UINT m_rtvDescSize = 0;
    UINT m_cbvSrvDescSize = 0;

    ComPtr<ID3D12Resource> m_renderTargets[2];
    ComPtr<ID3D12Resource> m_depthStencil;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[2] = { 0, 0 };
    HANDLE m_fenceEvent = nullptr;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW m_ibView{};
    std::vector<MeshSubset> m_subsets;
    std::vector<GpuMaterial> m_gpuMaterials;

    int m_width = 1280, m_height = 720;
    bool m_initialized = false;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
};
