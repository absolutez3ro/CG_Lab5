#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <vector>
#include <cmath>
#include <stdexcept>

using namespace DirectX;

static void RS_ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("RenderingSystem DX call failed");
}

bool RenderingSystem::Init(HWND hwnd, int width, int height)
{
    if (!m_renderer.Init(hwnd, width, height))
        return false;

    m_gbuffer.Initialize(
        m_renderer.GetDevice(),
        width,
        height,
        m_renderer.GetGbufferRtvStart(),
        m_renderer.GetRtvDescriptorSize(),
        m_renderer.GetGbufferSrvCpuStart(),
        m_renderer.GetGbufferSrvGpuStart(),
        m_renderer.GetSrvDescriptorSize());

    CreateRootSignatures();
    CreatePSOs();
    CreateLightMeshes();

    m_renderer.CreateBuffer(nullptr, sizeof(ObjectConstants) * 1024, &m_objectCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingFrameConstants), &m_frameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightVolumeConstants), &m_lightVolCB);

    return true;
}

void RenderingSystem::BeginFrame(const float clearColor[4])
{
    m_renderer.BeginFrame();

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();
    auto dsv = m_renderer.GetDsvHandle();

    cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(
        dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);
}

void RenderingSystem::CreateRootSignatures()
{
    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // ObjectConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized,
            &errors));

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_geometryRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0); // t0..t3 GBuffer

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(1); // LightVolumeConstants

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            3,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized,
            &errors));

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_lightingRS)));
    }
}

void RenderingSystem::CreatePSOs()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;

    RS_ThrowIfFailed(D3DCompileFromFile(
        L"GeometryPass.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0,
        &m_geoVS, &errors));

    RS_ThrowIfFailed(D3DCompileFromFile(
        L"GeometryPass.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0,
        &m_geoPS, &errors));

    RS_ThrowIfFailed(D3DCompileFromFile(
        L"LightingPass.hlsl", nullptr, nullptr,
        "VSVolume", "vs_5_0", flags, 0,
        &m_lightVS, &errors));

    RS_ThrowIfFailed(D3DCompileFromFile(
        L"LightingPass.hlsl", nullptr, nullptr,
        "PSDirectional", "ps_5_0", flags, 0,
        &m_lightPS, &errors));

    D3D12_INPUT_ELEMENT_DESC geoLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geoDesc{};
    geoDesc.InputLayout = { geoLayout, _countof(geoLayout) };
    geoDesc.pRootSignature = m_geometryRS.Get();
    geoDesc.VS = { m_geoVS->GetBufferPointer(), m_geoVS->GetBufferSize() };
    geoDesc.PS = { m_geoPS->GetBufferPointer(), m_geoPS->GetBufferSize() };
    geoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geoDesc.SampleMask = UINT_MAX;
    geoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geoDesc.NumRenderTargets = 4;
    geoDesc.RTVFormats[0] = m_gbuffer.GetFormat(GBuffer::Albedo);
    geoDesc.RTVFormats[1] = m_gbuffer.GetFormat(GBuffer::Normal);
    geoDesc.RTVFormats[2] = m_gbuffer.GetFormat(GBuffer::Position);
    geoDesc.RTVFormats[3] = m_gbuffer.GetFormat(GBuffer::Material);
    geoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geoDesc.SampleDesc.Count = 1;

    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(
        &geoDesc,
        IID_PPV_ARGS(&m_geometryPSO)));

    D3D12_INPUT_ELEMENT_DESC lightLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    auto makeLightingDesc = [&](const char* psEntry, ComPtr<ID3D12PipelineState>& outPSO)
        {
            ComPtr<ID3DBlob> psBlob;
            ComPtr<ID3DBlob> psErrors;
            RS_ThrowIfFailed(D3DCompileFromFile(
                L"LightingPass.hlsl", nullptr, nullptr,
                psEntry, "ps_5_0", flags, 0,
                &psBlob, &psErrors));

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.InputLayout = { lightLayout, _countof(lightLayout) };
            desc.pRootSignature = m_lightingRS.Get();
            desc.VS = { m_lightVS->GetBufferPointer(), m_lightVS->GetBufferSize() };
            desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
            desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;

            RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(
                &desc,
                IID_PPV_ARGS(&outPSO)));
        };

    makeLightingDesc("PSDirectional", m_psoDirectional);
    makeLightingDesc("PSPoint", m_psoPoint);
    makeLightingDesc("PSSpot", m_psoSpot);
}

void RenderingSystem::CreateLightMeshes()
{
    struct LightVertex
    {
        XMFLOAT3 Position;
    };

    std::vector<LightVertex> vertices;
    std::vector<UINT> indices;

    const UINT sliceCount = 16;
    const UINT stackCount = 12;

    vertices.push_back({ XMFLOAT3(0.f, 1.f, 0.f) });

    for (UINT stack = 1; stack <= stackCount - 1; ++stack)
    {
        float phi = XM_PI * stack / stackCount;
        for (UINT slice = 0; slice <= sliceCount; ++slice)
        {
            float theta = XM_2PI * slice / sliceCount;

            LightVertex v{};
            v.Position.x = std::sinf(phi) * std::cosf(theta);
            v.Position.y = std::cosf(phi);
            v.Position.z = std::sinf(phi) * std::sinf(theta);
            vertices.push_back(v);
        }
    }

    vertices.push_back({ XMFLOAT3(0.f, -1.f, 0.f) });

    for (UINT i = 1; i <= sliceCount; ++i)
    {
        indices.push_back(0);
        indices.push_back(i + 1);
        indices.push_back(i);
    }

    UINT baseIndex = 1;
    UINT ringVertexCount = sliceCount + 1;

    for (UINT stack = 0; stack < stackCount - 2; ++stack)
    {
        for (UINT slice = 0; slice < sliceCount; ++slice)
        {
            indices.push_back(baseIndex + stack * ringVertexCount + slice);
            indices.push_back(baseIndex + stack * ringVertexCount + slice + 1);
            indices.push_back(baseIndex + (stack + 1) * ringVertexCount + slice);

            indices.push_back(baseIndex + (stack + 1) * ringVertexCount + slice);
            indices.push_back(baseIndex + stack * ringVertexCount + slice + 1);
            indices.push_back(baseIndex + (stack + 1) * ringVertexCount + slice + 1);
        }
    }

    UINT southPoleIndex = static_cast<UINT>(vertices.size() - 1);
    baseIndex = southPoleIndex - ringVertexCount;

    for (UINT i = 0; i < sliceCount; ++i)
    {
        indices.push_back(southPoleIndex);
        indices.push_back(baseIndex + i);
        indices.push_back(baseIndex + i + 1);
    }

    m_renderer.CreateBuffer(vertices.data(), static_cast<UINT>(vertices.size() * sizeof(LightVertex)), &m_sphereVB);
    m_renderer.CreateBuffer(indices.data(), static_cast<UINT>(indices.size() * sizeof(UINT)), &m_sphereIB);

    m_sphereVBV.BufferLocation = m_sphereVB->GetGPUVirtualAddress();
    m_sphereVBV.StrideInBytes = sizeof(LightVertex);
    m_sphereVBV.SizeInBytes = static_cast<UINT>(vertices.size() * sizeof(LightVertex));

    m_sphereIBV.BufferLocation = m_sphereIB->GetGPUVirtualAddress();
    m_sphereIBV.Format = DXGI_FORMAT_R32_UINT;
    m_sphereIBV.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(UINT));

    m_sphereIndexCount = static_cast<UINT>(indices.size());
}

void RenderingSystem::GeometryPass()
{
    auto cmdList = m_renderer.GetCmdList();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] =
    {
        m_gbuffer.GetRTV(GBuffer::Albedo),
        m_gbuffer.GetRTV(GBuffer::Normal),
        m_gbuffer.GetRTV(GBuffer::Position),
        m_gbuffer.GetRTV(GBuffer::Material),
    };

    auto dsv = m_renderer.GetDsvHandle();

    cmdList->OMSetRenderTargets(4, rtvs, FALSE, &dsv);
    cmdList->SetGraphicsRootSignature(m_geometryRS.Get());
    cmdList->SetPipelineState(m_geometryPSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    const auto& subsets = m_renderer.GetSubsets();
    for (const auto& s : subsets)
    {
        cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexStart, 0, 0);
    }
}

void RenderingSystem::UpdateFrameConstants()
{
    LightingFrameConstants cb{};
    cb.EyePos = XMFLOAT4(0.f, 2.5f, -2.f, 1.f);
    cb.ScreenSize = XMFLOAT2(1280.f, 720.f);
    cb.InvScreenSize = XMFLOAT2(1.0f / 1280.0f, 1.0f / 720.0f);
    cb.AmbientColor = XMFLOAT4(0.08f, 0.08f, 0.10f, 1.0f);
    cb.DirLightDirection = XMFLOAT4(0.3f, -1.0f, 0.4f, 0.0f);
    cb.DirLightColorIntensity = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.75f);

    void* mapped = nullptr;
    m_frameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_frameCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdatePointLightCB(
    const XMFLOAT3& pos, float range,
    const XMFLOAT3& color, float intensity)
{
    XMMATRIX world = XMMatrixScaling(range, range, range) * XMMatrixTranslation(pos.x, pos.y, pos.z);
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixIdentity();

    LightVolumeConstants cb{};
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));
    cb.PositionRange = XMFLOAT4(pos.x, pos.y, pos.z, range);
    cb.DirectionCos = XMFLOAT4(0, 0, 0, 0);
    cb.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);

    void* mapped = nullptr;
    m_lightVolCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_lightVolCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateSpotLightCB(
    const XMFLOAT3& pos, float range,
    const XMFLOAT3& dir, float cosAngle,
    const XMFLOAT3& color, float intensity)
{
    XMMATRIX world = XMMatrixScaling(range, range, range) * XMMatrixTranslation(pos.x, pos.y, pos.z);
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixIdentity();

    LightVolumeConstants cb{};
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));
    cb.PositionRange = XMFLOAT4(pos.x, pos.y, pos.z, range);
    cb.DirectionCos = XMFLOAT4(dir.x, dir.y, dir.z, cosAngle);
    cb.ColorIntensity = XMFLOAT4(color.x, color.y, color.z, intensity);

    void* mapped = nullptr;
    m_lightVolCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_lightVolCB->Unmap(0, nullptr);
}

void RenderingSystem::LightingPassDirectional()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingRS.Get());
    cmdList->SetPipelineState(m_psoDirectional.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSRVGpu());
    cmdList->SetGraphicsRootConstantBufferView(2, m_lightVolCB->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::LightingPassPoint()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingRS.Get());
    cmdList->SetPipelineState(m_psoPoint.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSRVGpu());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_sphereVBV);
    cmdList->IASetIndexBuffer(&m_sphereIBV);

    const XMFLOAT3 lightPos[2] = {
        XMFLOAT3(-300.f, 120.f, 0.f),
        XMFLOAT3(300.f, 120.f, 0.f)
    };
    const XMFLOAT3 lightCol[2] = {
        XMFLOAT3(1.0f, 0.25f, 0.25f),
        XMFLOAT3(0.25f, 0.8f, 1.0f)
    };

    for (int i = 0; i < 2; ++i)
    {
        UpdatePointLightCB(lightPos[i], 250.f, lightCol[i], 2.0f);
        cmdList->SetGraphicsRootConstantBufferView(2, m_lightVolCB->GetGPUVirtualAddress());
        cmdList->DrawIndexedInstanced(m_sphereIndexCount, 1, 0, 0, 0);
    }
}

void RenderingSystem::LightingPassSpot()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingRS.Get());
    cmdList->SetPipelineState(m_psoSpot.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSRVGpu());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_sphereVBV);
    cmdList->IASetIndexBuffer(&m_sphereIBV);

    UpdateSpotLightCB(
        XMFLOAT3(0.f, 350.f, -120.f),
        450.f,
        XMFLOAT3(0.f, -1.f, 0.2f),
        0.82f,
        XMFLOAT3(1.0f, 0.95f, 0.8f),
        3.0f);

    cmdList->SetGraphicsRootConstantBufferView(2, m_lightVolCB->GetGPUVirtualAddress());
    cmdList->DrawIndexedInstanced(m_sphereIndexCount, 1, 0, 0, 0);
}

void RenderingSystem::DrawScene(float totalTime, float deltaTime)
{
    (void)totalTime;
    (void)deltaTime;

    auto cmdList = m_renderer.GetCmdList();

    m_gbuffer.TransitionToRenderTarget(cmdList);
    m_gbuffer.Clear(cmdList);

    GeometryPass();

    m_gbuffer.TransitionToShaderResource(cmdList);

    UpdateFrameConstants();
    LightingPassDirectional();
    LightingPassPoint();
    LightingPassSpot();
}

void RenderingSystem::OnResize(int width, int height)
{
    m_renderer.OnResize(width, height);
    m_gbuffer.Resize(
        m_renderer.GetDevice(),
        width,
        height,
        m_renderer.GetGbufferRtvStart(),
        m_renderer.GetRtvDescriptorSize(),
        m_renderer.GetGbufferSrvCpuStart(),
        m_renderer.GetGbufferSrvGpuStart(),
        m_renderer.GetSrvDescriptorSize());
}