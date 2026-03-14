#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <algorithm>

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

    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&m_cameraPos),
        XMVectorSet(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z + 1.0f, 1.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(width) / static_cast<float>(height), 1.0f, 5000.0f);
    XMStoreFloat4x4(&m_view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));

    CreateRootSignatures();
    CreatePSOs();
    CreateLightMeshes();
    SetupSceneLights();

    m_objectCbStride = (sizeof(ObjectConstants) + 255u) & ~255u;
    m_maxObjectCbCount = 8192;
    m_renderer.CreateBuffer(nullptr, m_objectCbStride * m_maxObjectCbCount, &m_objectCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingFrameConstants), &m_frameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightVolumeConstants), &m_lightVolCB);

    m_initialized = true;

    return true;
}

void RenderingSystem::OnKeyDown(WPARAM key)
{
    if (key == 'W') m_moveForward = true;
    if (key == 'S') m_moveBackward = true;
    if (key == 'A') m_moveLeft = true;
    if (key == 'D') m_moveRight = true;
}

void RenderingSystem::OnKeyUp(WPARAM key)
{
    if (key == 'W') m_moveForward = false;
    if (key == 'S') m_moveBackward = false;
    if (key == 'A') m_moveLeft = false;
    if (key == 'D') m_moveRight = false;
}

void RenderingSystem::OnMouseDown(int x, int y)
{
    m_mouseLookActive = true;
    m_hasLastMouse = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void RenderingSystem::OnMouseUp()
{
    m_mouseLookActive = false;
    m_hasLastMouse = false;
}

void RenderingSystem::OnMouseMove(int x, int y)
{
    if (!m_mouseLookActive)
        return;

    if (!m_hasLastMouse)
    {
        m_hasLastMouse = true;
        m_lastMouseX = x;
        m_lastMouseY = y;
        return;
    }

    const int dx = x - m_lastMouseX;
    const int dy = y - m_lastMouseY;
    m_lastMouseX = x;
    m_lastMouseY = y;

    m_yaw += static_cast<float>(dx) * m_mouseSensitivity;
    m_pitch -= static_cast<float>(dy) * m_mouseSensitivity;

    const float pitchLimit = XM_PIDIV2 - 0.01f;
    if (m_pitch > pitchLimit) m_pitch = pitchLimit;
    if (m_pitch < -pitchLimit) m_pitch = -pitchLimit;
}

void RenderingSystem::UpdateCamera(float dt)
{
    const float sinYaw = std::sin(m_yaw);
    const float cosYaw = std::cos(m_yaw);
    const float sinPitch = std::sin(m_pitch);
    const float cosPitch = std::cos(m_pitch);

    XMVECTOR forward = XMVector3Normalize(XMVectorSet(cosPitch * sinYaw, sinPitch, cosPitch * cosYaw, 0.0f));
    XMVECTOR forwardXZ = XMVector3Normalize(XMVectorSet(sinYaw, 0.0f, cosYaw, 0.0f));
    XMVECTOR rightXZ = XMVector3Normalize(XMVectorSet(cosYaw, 0.0f, -sinYaw, 0.0f));

    XMVECTOR pos = XMLoadFloat3(&m_cameraPos);
    const float step = m_moveSpeed * dt;
    if (m_moveForward) pos = XMVectorAdd(pos, XMVectorScale(forwardXZ, step));
    if (m_moveBackward) pos = XMVectorSubtract(pos, XMVectorScale(forwardXZ, step));
    if (m_moveLeft) pos = XMVectorSubtract(pos, XMVectorScale(rightXZ, step));
    if (m_moveRight) pos = XMVectorAdd(pos, XMVectorScale(rightXZ, step));

    XMStoreFloat3(&m_cameraPos, pos);
    UpdateViewMatrix();
}

void RenderingSystem::UpdateViewMatrix()
{
    const float sinYaw = std::sin(m_yaw);
    const float cosYaw = std::cos(m_yaw);
    const float sinPitch = std::sin(m_pitch);
    const float cosPitch = std::cos(m_pitch);

    const XMVECTOR eye = XMLoadFloat3(&m_cameraPos);
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(cosPitch * sinYaw, sinPitch, cosPitch * cosYaw, 0.0f));
    const XMVECTOR at = XMVectorAdd(eye, forward);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    XMStoreFloat4x4(&m_view, XMMatrixTranspose(view));
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
        params[0].InitAsConstantBufferView(0);
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
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_geometryRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(1);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
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
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingRS)));
    }
}

void RenderingSystem::CreatePSOs()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    RS_ThrowIfFailed(D3DCompileFromFile(L"GeometryPass.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_geoVS, &errors));
    RS_ThrowIfFailed(D3DCompileFromFile(L"GeometryPass.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_geoPS, &errors));
    RS_ThrowIfFailed(D3DCompileFromFile(L"LightingPass.hlsl", nullptr, nullptr, "VSFullscreen", "vs_5_0", flags, 0, &m_lightFullscreenVS, &errors));
    RS_ThrowIfFailed(D3DCompileFromFile(L"LightingPass.hlsl", nullptr, nullptr, "VSVolume", "vs_5_0", flags, 0, &m_lightVS, &errors));

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

    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&geoDesc, IID_PPV_ARGS(&m_geometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC dirDesc{};
    ComPtr<ID3DBlob> dirPS;
    RS_ThrowIfFailed(D3DCompileFromFile(L"LightingPass.hlsl", nullptr, nullptr, "PSDirectional", "ps_5_0", flags, 0, &dirPS, &errors));
    dirDesc.InputLayout = { nullptr, 0 };
    dirDesc.pRootSignature = m_lightingRS.Get();
    dirDesc.VS = { m_lightFullscreenVS->GetBufferPointer(), m_lightFullscreenVS->GetBufferSize() };
    dirDesc.PS = { dirPS->GetBufferPointer(), dirPS->GetBufferSize() };
    dirDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    dirDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    dirDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    dirDesc.DepthStencilState.DepthEnable = FALSE;
    dirDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dirDesc.SampleMask = UINT_MAX;
    dirDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    dirDesc.NumRenderTargets = 1;
    dirDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    dirDesc.SampleDesc.Count = 1;
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&dirDesc, IID_PPV_ARGS(&m_psoDirectional)));

    D3D12_INPUT_ELEMENT_DESC lightLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    auto makeVolumePso = [&](const char* psEntry, ComPtr<ID3D12PipelineState>& outPSO)
    {
        ComPtr<ID3DBlob> psBlob;
        RS_ThrowIfFailed(D3DCompileFromFile(L"LightingPass.hlsl", nullptr, nullptr, psEntry, "ps_5_0", flags, 0, &psBlob, &errors));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.InputLayout = { lightLayout, _countof(lightLayout) };
        desc.pRootSignature = m_lightingRS.Get();
        desc.VS = { m_lightVS->GetBufferPointer(), m_lightVS->GetBufferSize() };
        desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&outPSO)));
    };

    makeVolumePso("PSPoint", m_psoPoint);
    makeVolumePso("PSSpot", m_psoSpot);
}

void RenderingSystem::CreateLightMeshes()
{
    struct LightVertex { XMFLOAT3 Position; };

    std::vector<LightVertex> vertices;
    std::vector<UINT> indices;

    const UINT sliceCount = 16;
    const UINT stackCount = 12;

    vertices.push_back({ XMFLOAT3(0.f, 1.f, 0.f) });
    for (UINT stack = 1; stack <= stackCount - 1; ++stack)
    {
        const float phi = XM_PI * stack / stackCount;
        for (UINT slice = 0; slice <= sliceCount; ++slice)
        {
            const float theta = XM_2PI * slice / sliceCount;
            vertices.push_back({ XMFLOAT3(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)) });
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

    const UINT southPoleIndex = static_cast<UINT>(vertices.size() - 1);
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

void RenderingSystem::SetupSceneLights()
{
    m_pointLights.fill(PointLight{});
    m_spotLights.fill(SpotLight{});

    auto setPointLight = [this](size_t index, const PointLight& light)
    {
        if (index < m_pointLights.size())
            m_pointLights[index] = light;
    };

    auto setSpotLight = [this](size_t index, const SpotLight& light)
    {
        if (index < m_spotLights.size())
            m_spotLights[index] = light;
    };

    // Point lights: split between left/right sides and front/back to avoid clustering.
    setPointLight(0, PointLight{ XMFLOAT3(-620.f, 130.f, -520.f), 520.f, XMFLOAT3(1.00f, 0.25f, 0.20f), 2.20f }); // red (left-front)
    setPointLight(1, PointLight{ XMFLOAT3(-620.f, 135.f, 520.f), 520.f, XMFLOAT3(0.20f, 0.55f, 1.00f), 2.20f });  // blue (left-back)
    setPointLight(2, PointLight{ XMFLOAT3(-260.f, 135.f, -40.f), 500.f, XMFLOAT3(1.00f, 0.72f, 0.20f), 2.10f });  // amber (left-center)
    setPointLight(3, PointLight{ XMFLOAT3(-120.f, 145.f, 760.f), 520.f, XMFLOAT3(0.20f, 0.95f, 1.00f), 2.20f });  // cyan (left-far)

    setPointLight(4, PointLight{ XMFLOAT3(620.f, 130.f, -520.f), 520.f, XMFLOAT3(0.30f, 1.00f, 0.35f), 2.20f });   // green (right-front)
    setPointLight(5, PointLight{ XMFLOAT3(620.f, 135.f, 520.f), 520.f, XMFLOAT3(1.00f, 0.92f, 0.30f), 2.20f });    // yellow (right-back)
    setPointLight(6, PointLight{ XMFLOAT3(260.f, 135.f, -40.f), 500.f, XMFLOAT3(0.95f, 0.35f, 1.00f), 2.10f });     // magenta (right-center)
    setPointLight(7, PointLight{ XMFLOAT3(120.f, 145.f, 760.f), 520.f, XMFLOAT3(1.00f, 0.88f, 0.80f), 2.20f });     // warm white (right-far)

    // Spot lights: distinct RGB colors, separated across opposite sides.
    setSpotLight(0, SpotLight{ XMFLOAT3(-760.f, 430.f, -180.f), 900.f, XMFLOAT3(0.35f, -1.f, 0.12f), 0.79f, XMFLOAT3(1.00f, 0.20f, 0.20f), 2.40f }); // red spot (left)
    setSpotLight(1, SpotLight{ XMFLOAT3(760.f, 430.f, -180.f), 900.f, XMFLOAT3(-0.35f, -1.f, 0.12f), 0.79f, XMFLOAT3(0.20f, 1.00f, 0.30f), 2.40f }); // green spot (right)
    setSpotLight(2, SpotLight{ XMFLOAT3(0.f, 460.f, 980.f), 980.f, XMFLOAT3(0.0f, -1.f, -0.28f), 0.78f, XMFLOAT3(0.25f, 0.50f, 1.00f), 2.20f });   // blue spot (back)
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
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_geometryRS.Get());
    cmdList->SetPipelineState(m_geometryPSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();

    if (subsets.empty())
        return;

    void* mapped = nullptr;
    m_objectCB->Map(0, nullptr, &mapped);
    std::uint8_t* cbBase = reinterpret_cast<std::uint8_t*>(mapped);

    for (size_t subsetIndex = 0; subsetIndex < subsets.size(); ++subsetIndex)
    {
        if (subsetIndex >= m_maxObjectCbCount)
            break;

        const auto& s = subsets[subsetIndex];

        ObjectConstants obj{};
        XMStoreFloat4x4(&obj.World, XMMatrixTranspose(XMMatrixIdentity()));
        obj.View = m_view;
        obj.Proj = m_proj;
        XMStoreFloat4x4(&obj.WorldInvTranspose, XMMatrixTranspose(XMMatrixIdentity()));
        obj.MaterialDiffuse = XMFLOAT4(1, 1, 1, 1);
        obj.MaterialSpecular = XMFLOAT4(1, 1, 1, 1);
        obj.SpecularPower = 32.0f;
        obj.HasTexture = 0;

        UINT textureSrv = 0;
        if (s.materialIdx >= 0 && s.materialIdx < static_cast<int>(materials.size()))
        {
            const auto& mat = materials[s.materialIdx];
            obj.MaterialDiffuse = mat.diffuse;
            obj.MaterialSpecular = mat.specular;
            obj.SpecularPower = mat.specPower;
            if (mat.srvHeapIndex >= 0)
            {
                obj.HasTexture = 1;
                textureSrv = static_cast<UINT>(mat.srvHeapIndex);
            }
        }

        const UINT cbOffset = static_cast<UINT>(subsetIndex * m_objectCbStride);
        memcpy(cbBase + cbOffset, &obj, sizeof(obj));

        cmdList->SetGraphicsRootConstantBufferView(0, m_objectCB->GetGPUVirtualAddress() + cbOffset);
        cmdList->SetGraphicsRootDescriptorTable(1, m_renderer.GetSrvGpuHandle(textureSrv));
        cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexStart, 0, 0);
    }

    m_objectCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateFrameConstants()
{
    LightingFrameConstants cb{};
    cb.EyePos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    cb.ScreenSize = XMFLOAT2(static_cast<float>(m_renderer.GetWidth()), static_cast<float>(m_renderer.GetHeight()));
    cb.InvScreenSize = XMFLOAT2(1.0f / cb.ScreenSize.x, 1.0f / cb.ScreenSize.y);
    cb.AmbientColor = XMFLOAT4(0.16f, 0.16f, 0.18f, 1.0f);

    const XMVECTOR dirLight = XMVector3Normalize(XMVectorSet(0.20f, -1.0f, 0.10f, 0.0f));
    XMFLOAT3 dirLightNormalized;
    XMStoreFloat3(&dirLightNormalized, dirLight);

    cb.DirLightDirection = XMFLOAT4(dirLightNormalized.x, dirLightNormalized.y, dirLightNormalized.z, 0.0f);
    cb.DirLightColorIntensity = XMFLOAT4(0.95f, 0.97f, 1.00f, 1.35f);

    void* mapped = nullptr;
    m_frameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_frameCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdatePointLightCB(const PointLight& light)
{
    XMMATRIX world = XMMatrixScaling(light.Range, light.Range, light.Range) * XMMatrixTranslation(light.Position.x, light.Position.y, light.Position.z);
    XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));

    LightVolumeConstants cb{};
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));
    cb.PositionRange = XMFLOAT4(light.Position.x, light.Position.y, light.Position.z, light.Range);
    cb.DirectionCos = XMFLOAT4(0, 0, 0, 0);
    cb.ColorIntensity = XMFLOAT4(light.Color.x, light.Color.y, light.Color.z, light.Intensity);

    void* mapped = nullptr;
    m_lightVolCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_lightVolCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateSpotLightCB(const SpotLight& light)
{
    XMMATRIX world = XMMatrixScaling(light.Range, light.Range, light.Range) * XMMatrixTranslation(light.Position.x, light.Position.y, light.Position.z);
    XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));

    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&light.Direction));
    XMFLOAT3 normDir;
    XMStoreFloat3(&normDir, d);

    LightVolumeConstants cb{};
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));
    cb.PositionRange = XMFLOAT4(light.Position.x, light.Position.y, light.Position.z, light.Range);
    const float clampedCos = std::clamp(light.CosAngle, 0.0f, 0.999f);
    cb.DirectionCos = XMFLOAT4(normDir.x, normDir.y, normDir.z, clampedCos);
    cb.ColorIntensity = XMFLOAT4(light.Color.x, light.Color.y, light.Color.z, light.Intensity);

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

    for (const auto& light : m_pointLights)
    {
        UpdatePointLightCB(light);
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

    for (const auto& light : m_spotLights)
    {
        UpdateSpotLightCB(light);
        cmdList->SetGraphicsRootConstantBufferView(2, m_lightVolCB->GetGPUVirtualAddress());
        cmdList->DrawIndexedInstanced(m_sphereIndexCount, 1, 0, 0, 0);
    }
}

void RenderingSystem::DrawScene(float totalTime, float deltaTime)
{
    (void)totalTime;
    UpdateCamera(deltaTime);

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
    if (!m_initialized)
        return;

    if (width <= 0 || height <= 0)
        return;

    if (m_renderer.GetSrvHeap() == nullptr)
        return;

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

    if (height > 0)
    {
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), static_cast<float>(width) / static_cast<float>(height), 1.0f, 5000.0f);
        XMStoreFloat4x4(&m_proj, XMMatrixTranspose(proj));
    }
}
