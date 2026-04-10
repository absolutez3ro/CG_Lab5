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
#include <stdexcept>
#include "d3dx12.h"
#include "ObjLoader.h"
#include "TextureLoader.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct Vertex {
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
    XMFLOAT3 Tangent;
    XMFLOAT3 Bitangent;
};

// Geometry pass constants are intentionally split:
// - transform constants (per draw)
// - frame/view constants (per frame)
// - material constants (per draw/material)
struct ObjectTransformConstants
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;
};

struct GeometryFrameConstants
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4 CameraPos;
    XMFLOAT2 TessFactorRange;
    XMFLOAT2 TessDistanceRange;
    UINT GeometryDebugMode = 0;
    UINT DebugStrongDisplacement = 1;
    UINT GeometryFramePad[2] = { 0, 0 };
};

struct MaterialConstants
{
    XMFLOAT4 MaterialDiffuse;
    XMFLOAT4 MaterialSpecular;
    float SpecularPower;
    int HasTexture;
    int HasNormalMap;
    int HasDisplacementMap;
    float DisplacementScale;
    float DisplacementBias;
    float Pad[2];
};

static_assert(sizeof(ObjectTransformConstants) % 16 == 0, "ObjectTransformConstants must be 16-byte aligned for HLSL packing.");
static_assert(sizeof(GeometryFrameConstants) % 16 == 0, "GeometryFrameConstants must be 16-byte aligned for HLSL packing.");
static_assert(sizeof(MaterialConstants) % 16 == 0, "MaterialConstants must be 16-byte aligned for HLSL packing.");
// Note: these structs are packed to 16-byte boundaries; per-draw CBV offsets are padded
// to 256-byte boundaries where uploaded (see RenderingSystem stride setup).

struct GpuMaterial {
    ComPtr<ID3D12Resource> diffuseTexture;
    ComPtr<ID3D12Resource> diffuseTextureUpload;
    ComPtr<ID3D12Resource> normalTexture;
    ComPtr<ID3D12Resource> normalTextureUpload;
    ComPtr<ID3D12Resource> displacementTexture;
    ComPtr<ID3D12Resource> displacementTextureUpload;
    int diffuseSrvHeapIndex = -1;
    int normalSrvHeapIndex = -1;
    int displacementSrvHeapIndex = -1;
    XMFLOAT4 diffuse = { 1, 1, 1, 1 };
    XMFLOAT4 specular = { 1, 1, 1, 1 };
    float specPower = 32.0f;
    float displacementScale = 0.0f;
    float displacementBias = 0.0f;
    bool hasNormalMap = false;
    bool hasDisplacementMap = false;
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
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(UINT index) const {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), index, m_cbvSrvDescSize);
    }

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
        if (!m_cbvSrvHeap)
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_cbvSrvDescSize);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGbufferSrvGpuStart() {
        if (!m_cbvSrvHeap)
            return D3D12_GPU_DESCRIPTOR_HANDLE{};
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), 1, m_cbvSrvDescSize);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthSrvCpuHandle() const {
        if (!m_cbvSrvHeap)
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(), 4, m_cbvSrvDescSize);
    }
    const D3D12_VERTEX_BUFFER_VIEW* GetVbView() const { return &m_vbView; }
    const D3D12_INDEX_BUFFER_VIEW* GetIbView() const { return &m_ibView; }
    const std::vector<MeshSubset>& GetSubsets() const { return m_subsets; }
    const std::vector<GpuMaterial>& GetMaterials() const { return m_gpuMaterials; }

    void CreateBuffer(const void* data, UINT size, ID3D12Resource** resource);
    void TransitionDepthToShaderResource();

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, int width, int height);
    void CreateDescriptorHeaps();
    void CreateRenderTargetViews();
    void CreateDepthStencilView();
    void CreateFence();
    void CreateDefaultTexture();
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
    D3D12_RESOURCE_STATES m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[2] = { 0, 0 };
    HANDLE m_fenceEvent = nullptr;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_defaultWhiteTexture;
    ComPtr<ID3D12Resource> m_defaultWhiteUpload;
    bool m_forceSponzaDiagnosticMaterialOverride = true;
    ComPtr<ID3D12Resource> m_globalOverrideNormalTexture;
    ComPtr<ID3D12Resource> m_globalOverrideNormalUpload;
    ComPtr<ID3D12Resource> m_globalOverrideDisplacementTexture;
    ComPtr<ID3D12Resource> m_globalOverrideDisplacementUpload;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW m_ibView{};
    std::vector<MeshSubset> m_subsets;
    std::vector<GpuMaterial> m_gpuMaterials;

    int m_width = 1280, m_height = 720;
    bool m_initialized = false;
    UINT m_nextSrvIndex = 8;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
};
