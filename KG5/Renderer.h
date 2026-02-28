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
#include <array>
#include "d3dx12.h"
#include "ObjLoader.h"
#include "TextureLoader.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
using Microsoft::WRL::ComPtr;
using namespace DirectX;
struct Vertex
{
	XMFLOAT3 Position;
	XMFLOAT3 Normal;
	XMFLOAT2 TexCoord;
};
// 256-byte aligned CB
struct alignas(256) ConstantBufferData
{
	XMFLOAT4X4 World;
	XMFLOAT4X4 View;
	XMFLOAT4X4 Proj;
	XMFLOAT4X4 WorldInvTranspose;
	XMFLOAT4 LightDir;
	XMFLOAT4 LightColor;
	XMFLOAT4 AmbientColor;
	XMFLOAT4 EyePos;
	XMFLOAT4 MaterialDiffuse;
	XMFLOAT4 MaterialSpecular;
	float SpecularPower;
	float TotalTime;
	float TexTilingX;
	float TexTilingY;
	float TexScrollX;
	float TexScrollY;
	float FadeStart;
	float FadeEnd;
	int HasTexture;
	int Pad2[3];
};
struct GpuMaterial
{
	ComPtr<ID3D12Resource> texture;
	ComPtr<ID3D12Resource> textureUpload;
	int srvHeapIndex = -1;
	XMFLOAT4 diffuse = { 0.8f, 0.8f, 0.8f, 1.f };
	XMFLOAT4 specular = { 0.5f, 0.5f, 0.5f, 1.f };
	float shininess = 32.f;
	bool hasTexture = false;
};
class Renderer
{
public:
	static constexpr UINT FRAME_COUNT = 2;
	static constexpr UINT MAX_TEXTURES = 512;
	static constexpr UINT MAX_SUBSETS = 512;
	Renderer() = default;
	~Renderer();
	bool Init(HWND hwnd, int width, int height);
	void BeginFrame(const float clearColor[4]);
	void DrawScene(float totalTime, float deltaTime);
	void EndFrame();
	void OnResize(int width, int height);
	bool LoadObj(const std::string& path);
	void SetTexTiling(float x, float y) { m_texTiling = { x, y }; }
	void SetTexScroll(float x, float y) { m_texScroll = { x, y }; }
private:
	void CreateDevice();
	void CreateCommandObjects();
	void CreateSwapChain(HWND hwnd, int width, int height);
	void CreateDescriptorHeaps();
	void CreateRenderTargetViews();
	void CreateDepthStencilView();
	void CreateFence();
	void CreateRootSignature();
	void CreatePipelineStateObject();
	void CreateCubeGeometry();
	void UploadMeshToGpu(const std::vector<Vertex>& verts,
		const std::vector<UINT>& indices);
	void CreateConstantBuffer();
	void CompileShaders();
	void LoadMaterials(const ObjMesh& mesh, const std::string& baseDir);
	void CreateFallbackTexture();
	void WaitForGPU();
	void FlushCommandQueue();
	void MoveToNextFrame();
	ComPtr<ID3D12Device> m_device;
	ComPtr<IDXGIFactory6> m_factory;
	ComPtr<ID3D12CommandQueue> m_cmdQueue;
	ComPtr<ID3D12GraphicsCommandList> m_cmdList;
	ComPtr<ID3D12CommandAllocator> m_cmdAllocators[FRAME_COUNT];
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Resource> m_renderTargets[FRAME_COUNT];
	UINT m_frameIndex = 0;
	ComPtr<ID3D12Resource> m_depthStencil;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap;
	UINT m_rtvDescSize = 0;
	UINT m_cbvSrvDescSize = 0;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValues[FRAME_COUNT]{};
	HANDLE m_fenceEvent = nullptr;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pso;
	ComPtr<ID3DBlob> m_vsBlob;
	ComPtr<ID3DBlob> m_psBlob;
	ComPtr<ID3D12Resource> m_vertexBuffer;
	ComPtr<ID3D12Resource> m_indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_vbView{};
	D3D12_INDEX_BUFFER_VIEW m_ibView{};
	std::vector<MeshSubset> m_subsets;
	std::vector<GpuMaterial> m_gpuMaterials;
	ComPtr<ID3D12Resource> m_constantBuffer;
	ConstantBufferData* m_cbMapped = nullptr;
	UINT m_cbSlotSize = 0;
	XMFLOAT2 m_texTiling = { 1.f, 1.f };
	XMFLOAT2 m_texScroll = { 0.05f, 0.f };
	int m_width = 0;
	int m_height = 0;
	XMFLOAT3 m_eye = { -1000.f, 150.f, 0.f };
	XMFLOAT3 m_target = { 0.f, 150.f, 0.f };
	XMFLOAT3 m_up = { 0.f, 1.f, 0.f };
	XMFLOAT3 m_sceneCenter = { 0.f,0.f,0.f };
	float m_sceneRadius = 1.f;
	bool m_initialized = false;

	// Белая текстура-заглушка для слота 0 (fallback когда нет текстуры)
	ComPtr<ID3D12Resource> m_fallbackTexture;
	ComPtr<ID3D12Resource> m_fallbackTextureUpload;

	// Переменные для второй (процедурной) текстуры
	ComPtr<ID3D12Resource> m_farTexture;
	ComPtr<ID3D12Resource> m_farTextureUpload;
	int m_farTexSrvIndex = -1;
};