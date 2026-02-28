#include "Renderer.h"
#include <stdexcept>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <cfloat>
static void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr)) throw std::runtime_error("DirectX call failed");
}
// ============================================================================
// Init
// ============================================================================
bool Renderer::Init(HWND hwnd, int width, int height)
{
	m_width = width;
	m_height = height;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	try
	{
		CreateDevice();
		CreateCommandObjects();
		CreateSwapChain(hwnd, width, height);
		CreateDescriptorHeaps();
		CreateRenderTargetViews();
		CreateDepthStencilView();
		CreateFence();
		CompileShaders();
		CreateRootSignature();
		CreatePipelineStateObject();
		CreateCubeGeometry();
		CreateConstantBuffer();
		ThrowIfFailed(m_cmdList->Close());
		ID3D12CommandList* cmds[] = { m_cmdList.Get() };
		m_cmdQueue->ExecuteCommandLists(1, cmds);
		WaitForGPU();
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(e.what());
		return false;
	}
	m_initialized = true;
	return true;
}
// ============================================================================
// Device
// ============================================================================
void Renderer::CreateDevice()
{
#ifdef _DEBUG
	ComPtr<ID3D12Debug> dbg;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
		dbg->EnableDebugLayer();
#endif
	ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0;
		m_factory->EnumAdapterByGpuPreference(
			i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
			IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(),
			D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
			break;
	}
	if (!m_device)
		ThrowIfFailed(D3D12CreateDevice(nullptr,
			D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
}
void Renderer::CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC q{};
	q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ThrowIfFailed(m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_cmdQueue)));
	for (UINT i = 0; i < FRAME_COUNT; ++i)
		ThrowIfFailed(m_device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocators[i])));
	ThrowIfFailed(m_device->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList)));
}
void Renderer::CreateSwapChain(HWND hwnd, int width, int height)
{
	DXGI_SWAP_CHAIN_DESC1 sc{};
	sc.Width = width; sc.Height = height;
	sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc.SampleDesc = { 1, 0 };
	sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc.BufferCount = FRAME_COUNT;
	sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> sc1;
	ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
		m_cmdQueue.Get(), hwnd, &sc, nullptr, nullptr, &sc1));
	ThrowIfFailed(sc1.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
void Renderer::CreateDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvD{};
	rtvD.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvD.NumDescriptors = FRAME_COUNT;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvD, IID_PPV_ARGS(&m_rtvHeap)));
	m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_DESCRIPTOR_HEAP_DESC dsvD{};
	dsvD.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvD.NumDescriptors = 1;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvD, IID_PPV_ARGS(&m_dsvHeap)));
	// slot0 = reserved, slots 1..MAX_TEXTURES = SRVs
	D3D12_DESCRIPTOR_HEAP_DESC cbvD{};
	cbvD.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvD.NumDescriptors = 1 + MAX_TEXTURES;
	cbvD.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&cbvD, IID_PPV_ARGS(&m_cbvSrvHeap)));
	m_cbvSrvDescSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}
void Renderer::CreateRenderTargetViews()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE h(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < FRAME_COUNT; ++i)
	{
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
		m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, h);
		h.Offset(1, m_rtvDescSize);
	}
}
void Renderer::CreateDepthStencilView()
{
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = m_width; d.Height = m_height;
	d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.Format = DXGI_FORMAT_D32_FLOAT;
	d.SampleDesc = { 1, 0 };
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE cv{};
	cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
	CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &d,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_depthStencil)));
	m_device->CreateDepthStencilView(m_depthStencil.Get(), nullptr,
		m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}
void Renderer::CreateFence()
{
	ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	m_fenceValues[m_frameIndex] = 1;
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
}
void Renderer::CompileShaders()
{
	UINT flags = 0;
#ifdef _DEBUG
	flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
	ComPtr<ID3DBlob> errors;
	HRESULT hr = D3DCompileFromFile(L"PhongShader.hlsl",
		nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_vsBlob, &errors);
	if (FAILED(hr)) {
		if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
		ThrowIfFailed(hr);
	}
	hr = D3DCompileFromFile(L"PhongShader.hlsl",
		nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_psBlob, &errors);
	if (FAILED(hr)) {
		if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
		ThrowIfFailed(hr);
	}
}
// ============================================================================
// Root Signature:
// param[0] = CBV b0 (VS+PS)
// param[1] = descriptor table: SRV t0 (PS)
// static sampler s0: WRAP + LINEAR
// ============================================================================
void Renderer::CreateRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	CD3DX12_ROOT_PARAMETER params[2];
	params[0].InitAsConstantBufferView(0);
	params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
	CD3DX12_STATIC_SAMPLER_DESC sampler(0,
		D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP);
	CD3DX12_ROOT_SIGNATURE_DESC rsDesc(2, params, 1, &sampler,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	ComPtr<ID3DBlob> serialized, errors;
	HRESULT hr = D3D12SerializeRootSignature(
		&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if (FAILED(hr)) {
		if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
		ThrowIfFailed(hr);
	}
	ThrowIfFailed(m_device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&m_rootSignature)));
}
void Renderer::CreatePipelineStateObject()
{
	D3D12_INPUT_ELEMENT_DESC layout[] =
	{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
	pso.InputLayout = { layout, _countof(layout) };
	pso.pRootSignature = m_rootSignature.Get();
	pso.VS = { m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize() };
	pso.PS = { m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize() };
	D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso.BlendState = blendDesc;
	pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pso.SampleDesc = { 1, 0 };
	ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}
// ============================================================================
// Default cube
// ============================================================================
void Renderer::CreateCubeGeometry()
{
	std::array<Vertex, 24> verts =
	{ {
	{ { -1,-1, 1 }, { 0, 0, 1 }, { 0,1 } }, { { 1,-1, 1 }, { 0, 0, 1 }, { 1,1 } },
	{ { 1, 1, 1 }, { 0, 0, 1 }, { 1,0 } }, { { -1, 1, 1 }, { 0, 0, 1 }, { 0,0 } },
	{ { 1,-1,-1 }, { 0, 0,-1 }, { 0,1 } }, { { -1,-1,-1 }, { 0, 0,-1 }, { 1,1 } },
	{ { -1, 1,-1 }, { 0, 0,-1 }, { 1,0 } }, { { 1, 1,-1 }, { 0, 0,-1 }, { 0,0 } },
	{ { -1,-1,-1 }, {-1, 0, 0 }, { 0,1 } }, { { -1,-1, 1 }, {-1, 0, 0 }, { 1,1 } },
	{ { -1, 1, 1 }, {-1, 0, 0 }, { 1,0 } }, { { -1, 1,-1 }, {-1, 0, 0 }, { 0,0 } },
	{ { 1,-1, 1 }, { 1, 0, 0 }, { 0,1 } }, { { 1,-1,-1 }, { 1, 0, 0 }, { 1,1 } },
	{ { 1, 1,-1 }, { 1, 0, 0 }, { 1,0 } }, { { 1, 1, 1 }, { 1, 0, 0 }, { 0,0 } },
	{ { -1, 1, 1 }, { 0, 1, 0 }, { 0,1 } }, { { 1, 1, 1 }, { 0, 1, 0 }, { 1,1 } },
	{ { 1, 1,-1 }, { 0, 1, 0 }, { 1,0 } }, { { -1, 1,-1 }, { 0, 1, 0 }, { 0,0 } },
	{ { -1,-1,-1 }, { 0,-1, 0 }, { 0,1 } }, { { 1,-1,-1 }, { 0,-1, 0 }, { 1,1 } },
	{ { 1,-1, 1 }, { 0,-1, 0 }, { 1,0 } }, { { -1,-1, 1 }, { 0,-1, 0 }, { 0,0 } },
	} };
	std::array<UINT, 36> idx;
	for (int f = 0; f < 6; ++f)
	{
		UINT b = f * 4;
		idx[f * 6 + 0] = b + 0; idx[f * 6 + 1] = b + 1; idx[f * 6 + 2] = b + 2;
		idx[f * 6 + 3] = b + 0; idx[f * 6 + 4] = b + 2; idx[f * 6 + 5] = b + 3;
	}
	std::vector<Vertex> v(verts.begin(), verts.end());
	std::vector<UINT> i(idx.begin(), idx.end());
	MeshSubset sub; sub.indexStart = 0; sub.indexCount = 36; sub.materialIdx = 0;
	m_subsets = { sub };
	GpuMaterial mat;
	mat.diffuse = { 0.2f, 0.5f, 0.9f, 1.f };
	mat.specular = { 0.8f, 0.8f, 0.8f, 1.f };
	mat.shininess = 32.f; mat.hasTexture = false;
	m_gpuMaterials = { mat };
	UploadMeshToGpu(v, i);
}
void Renderer::UploadMeshToGpu(const std::vector<Vertex>& verts,
	const std::vector<UINT>& indices)
{
	m_vertexBuffer.Reset();
	m_indexBuffer.Reset();
	auto upload = [&](const void* data, UINT sz, ComPtr<ID3D12Resource>& buf)
		{
			CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
			CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sz);
			ThrowIfFailed(m_device->CreateCommittedResource(
				&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)));
			void* p = nullptr; buf->Map(0, nullptr, &p);
			memcpy(p, data, sz); buf->Unmap(0, nullptr);
		};
	UINT vbSz = (UINT)(verts.size() * sizeof(Vertex));
	UINT ibSz = (UINT)(indices.size() * sizeof(UINT));
	upload(verts.data(), vbSz, m_vertexBuffer);
	upload(indices.data(), ibSz, m_indexBuffer);
	m_vbView = { m_vertexBuffer->GetGPUVirtualAddress(), vbSz, sizeof(Vertex) };
	m_ibView = { m_indexBuffer->GetGPUVirtualAddress(), ibSz, DXGI_FORMAT_R32_UINT };
}
void Renderer::CreateConstantBuffer()
{
	// Each slot must be 256-byte aligned
	m_cbSlotSize = (sizeof(ConstantBufferData) + 255) & ~255;
	// Total: FRAME_COUNT frames * MAX_SUBSETS slots each
	UINT totalSize = m_cbSlotSize * Renderer::MAX_SUBSETS * Renderer::FRAME_COUNT;
	CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)));
	m_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped));
}
// ============================================================================
// LoadObj
// ============================================================================
bool Renderer::LoadObj(const std::string& path)
{
	if (m_initialized) FlushCommandQueue();
	ObjMesh mesh;
	if (!ObjLoader::Load(path, mesh)) return false;
	std::vector<Vertex> verts(mesh.vertices.size());
	for (size_t i = 0; i < verts.size(); ++i)
	{
		verts[i].Position = mesh.vertices[i].Position;
		verts[i].Normal = mesh.vertices[i].Normal;
		verts[i].TexCoord = mesh.vertices[i].TexCoord;
	}

	
	XMFLOAT3 vmin = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 vmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& v : verts)
	{
		vmin.x = (std::min)(vmin.x, v.Position.x); vmin.y = (std::min)(vmin.y, v.Position.y); vmin.z = (std::min)(vmin.z, v.Position.z);
		vmax.x = (std::max)(vmax.x, v.Position.x); vmax.y = (std::max)(vmax.y, v.Position.y); vmax.z = (std::max)(vmax.z, v.Position.z);
	}
	m_sceneCenter = { (vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f, (vmin.z + vmax.z) * 0.5f };
	XMFLOAT3 ext = { (vmax.x - vmin.x), (vmax.y - vmin.y), (vmax.z - vmin.z) };
	m_sceneRadius = 0.5f * sqrtf(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
	if (m_sceneRadius < 0.001f) m_sceneRadius = 1.0f;

	
	m_target = m_sceneCenter;
	m_eye = {
	m_sceneCenter.x + m_sceneRadius * 0.6f,
	m_sceneCenter.y + m_sceneRadius * 0.55f,
	m_sceneCenter.z + m_sceneRadius * 1.6f
	};

	m_subsets = mesh.subsets;
	std::string dir;
	size_t p = path.find_last_of("/\\");
	if (p != std::string::npos) dir = path.substr(0, p + 1);
	ThrowIfFailed(m_cmdAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr));
	LoadMaterials(mesh, dir);
	UploadMeshToGpu(verts, mesh.indices);
	ThrowIfFailed(m_cmdList->Close());
	ID3D12CommandList* cmds[] = { m_cmdList.Get() };
	m_cmdQueue->ExecuteCommandLists(1, cmds);
	WaitForGPU();
	for (auto& mat : m_gpuMaterials) mat.textureUpload.Reset();
	return true;
}
void Renderer::LoadMaterials(const ObjMesh& mesh, const std::string& baseDir)
{
	m_gpuMaterials.clear();
	if (mesh.materials.empty())
	{
		GpuMaterial def;
		def.diffuse = { 0.8f,0.8f,0.8f,1.f };
		def.specular = { 0.5f,0.5f,0.5f,1.f };
		def.shininess = 32.f; def.hasTexture = false;
		m_gpuMaterials.push_back(def);
		return;
	}
	m_gpuMaterials.resize(mesh.materials.size());
	int srvSlot = 0;
	for (size_t i = 0; i < mesh.materials.size(); ++i)
	{
		const Material& src = mesh.materials[i];
		GpuMaterial& dst = m_gpuMaterials[i];
		dst.diffuse = src.diffuse;
		dst.specular = src.specular;
		dst.shininess = src.shininess;
		if (!src.diffuseTexture.empty())
		{
			std::wstring wpath(baseDir.begin(), baseDir.end());
			std::wstring wtex(src.diffuseTexture.begin(), src.diffuseTexture.end());
			wpath += wtex;
			// Debug: show which texture we're trying to load
			std::wstring dbgMsg = L"Loading texture: " + wpath + L"\n";
			OutputDebugStringW(dbgMsg.c_str());
			TextureLoader::TextureData td;
			bool loadOk = TextureLoader::LoadFromFile(wpath, td);
			if (!loadOk)
			{
				std::wstring errMsg = L" FAILED to load: " + wpath + L"\n";
				OutputDebugStringW(errMsg.c_str());
			}
			if (loadOk &&
				TextureLoader::CreateTexture(m_device.Get(), m_cmdList.Get(),
					td, dst.texture, dst.textureUpload))
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Format = td.format;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
					m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
					1 + srvSlot, m_cbvSrvDescSize);
				m_device->CreateShaderResourceView(dst.texture.Get(), &srvDesc, srvHandle);
				dst.srvHeapIndex = 1 + srvSlot;
				dst.hasTexture = true;
				++srvSlot;
				OutputDebugStringW((L" OK: " + wpath + L"\n").c_str());
			}
		}
	}
}

// ============================================================================
// BeginFrame
// ============================================================================
void Renderer::BeginFrame(const float clearColor[4])
{
	ThrowIfFailed(m_cmdAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_cmdList->Reset(m_cmdAllocators[m_frameIndex].Get(), nullptr));
	CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
		m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_cmdList->ResourceBarrier(1, &b);
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
		m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescSize);
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	D3D12_VIEWPORT vp{ 0, 0, (float)m_width, (float)m_height, 0, 1 };
	D3D12_RECT sc{ 0, 0, m_width, m_height };
	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &sc);
}
// ============================================================================
// DrawScene
// ============================================================================
void Renderer::DrawScene(float totalTime, float /*dt*/)
{
	if (!m_pso || m_subsets.empty()) return;
	m_cmdList->SetPipelineState(m_pso.Get());
	m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
	ID3D12DescriptorHeap* heaps[] = { m_cbvSrvHeap.Get() };
	m_cmdList->SetDescriptorHeaps(1, heaps);
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_cmdList->IASetVertexBuffers(0, 1, &m_vbView);
	m_cmdList->IASetIndexBuffer(&m_ibView);
	// No rotation for large static scenes like Sponza
	XMMATRIX world = XMMatrixIdentity();
	XMMATRIX view = XMMatrixLookAtLH(
		XMLoadFloat3(&m_eye), XMLoadFloat3(&m_target), XMLoadFloat3(&m_up));
	float aspect = (float)m_width / (float)m_height;
	// Far plane 10000 for large scenes like Sponza
	float nearPlane = (std::max)(0.01f, m_sceneRadius / 1000.0f);
	float farPlane = (std::max)(100.0f, m_sceneRadius * 50.0f);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.f), aspect, nearPlane, farPlane);
	XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
	UINT numSubsets = (UINT)m_subsets.size();
	for (UINT subIdx = 0; subIdx < numSubsets; ++subIdx)
	{
		const MeshSubset& sub = m_subsets[subIdx];
		if (sub.indexCount == 0) continue;
		int matIdx = (sub.materialIdx >= 0 && sub.materialIdx < (int)m_gpuMaterials.size())
			? sub.materialIdx : 0;
		const GpuMaterial& mat = m_gpuMaterials.empty() ? GpuMaterial{} : m_gpuMaterials[matIdx];
		// Each subset gets its own CB slot: frameIndex * MAX_SUBSETS + subIdx
		// If more subsets than MAX_SUBSETS, wrap around (safe for static CBs)
		UINT slotIdx = m_frameIndex * Renderer::MAX_SUBSETS + (subIdx % Renderer::MAX_SUBSETS);
		UINT8* slotPtr = reinterpret_cast<UINT8*>(m_cbMapped) + slotIdx * m_cbSlotSize;
		D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
			m_constantBuffer->GetGPUVirtualAddress() + slotIdx * m_cbSlotSize;
		ConstantBufferData cb{};
		XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
		XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
		XMStoreFloat4x4(&cb.WorldInvTranspose, XMMatrixTranspose(wit));
		cb.LightDir = { 0.3f, -1.f, 0.5f, 0.f };
		cb.LightColor = { 1.f, 1.f, 1.f, 1.f };
		cb.AmbientColor = { 0.25f, 0.25f, 0.3f, 1.f };
		cb.EyePos = { m_eye.x, m_eye.y, m_eye.z, 1.f };
		cb.MaterialDiffuse = mat.diffuse;
		cb.MaterialSpecular = mat.specular;
		cb.SpecularPower = mat.shininess;
		cb.HasTexture = mat.hasTexture ? 1 : 0;
		cb.TotalTime = totalTime;
		cb.TexTilingX = m_texTiling.x;
		cb.TexTilingY = m_texTiling.y;
		cb.TexScrollX = m_texScroll.x;
		cb.TexScrollY = m_texScroll.y;
		memcpy(slotPtr, &cb, sizeof(cb));
		m_cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);
		// Bind SRV
		int srvIdx = (mat.hasTexture && mat.srvHeapIndex >= 0) ? mat.srvHeapIndex : 0;
		CD3DX12_GPU_DESCRIPTOR_HANDLE srvH(
			m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
			srvIdx, m_cbvSrvDescSize);
		m_cmdList->SetGraphicsRootDescriptorTable(1, srvH);
		m_cmdList->DrawIndexedInstanced(sub.indexCount, 1, sub.indexStart, 0, 0);
	}
}
// ============================================================================
// EndFrame
// ============================================================================
void Renderer::EndFrame()
{
	CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
		m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	m_cmdList->ResourceBarrier(1, &b);
	ThrowIfFailed(m_cmdList->Close());
	ID3D12CommandList* cmds[] = { m_cmdList.Get() };
	m_cmdQueue->ExecuteCommandLists(1, cmds);
	ThrowIfFailed(m_swapChain->Present(1, 0));
	MoveToNextFrame();
}
void Renderer::WaitForGPU()
{
	const UINT64 val = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), val));
	m_fenceValues[m_frameIndex]++;
	if (m_fence->GetCompletedValue() < val)
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(val, m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}
}
void Renderer::MoveToNextFrame()
{
	const UINT64 cur = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), cur));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(
			m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}
	m_fenceValues[m_frameIndex] = cur + 1;
}
void Renderer::FlushCommandQueue() { WaitForGPU(); }
void Renderer::OnResize(int width, int height)
{
	if (!m_initialized || (m_width == width && m_height == height)) return;
	m_width = width; m_height = height;
	FlushCommandQueue();
	for (auto& rt : m_renderTargets) rt.Reset();
	m_depthStencil.Reset();
	ThrowIfFailed(m_swapChain->ResizeBuffers(
		FRAME_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	CreateRenderTargetViews();
	CreateDepthStencilView();
}
Renderer::~Renderer()
{
	if (m_initialized) FlushCommandQueue();
	if (m_constantBuffer && m_cbMapped) m_constantBuffer->Unmap(0, nullptr);
	if (m_fenceEvent) CloseHandle(m_fenceEvent);
	CoUninitialize();
}