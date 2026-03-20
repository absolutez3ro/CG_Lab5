#include "RenderingSystem.h"
#include <d3dcompiler.h>
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
    SetupSceneLights();

    m_objectTransformCbStride = (sizeof(ObjectTransformConstants) + 255u) & ~255u;
    m_materialCbStride = (sizeof(MaterialConstants) + 255u) & ~255u;
    m_maxObjectCbCount = 8192;

    m_renderer.CreateBuffer(nullptr, m_objectTransformCbStride * m_maxObjectCbCount, &m_objectTransformCB);
    m_renderer.CreateBuffer(nullptr, m_materialCbStride * m_maxObjectCbCount, &m_materialCB);
    m_renderer.CreateBuffer(nullptr, sizeof(GeometryFrameConstants), &m_geometryFrameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LightingFrameConstants), &m_frameCB);
    m_renderer.CreateBuffer(nullptr, sizeof(LightingContract::LocalLightConstants), &m_localLightsCB);

    m_initialized = true;
    return true;
}

void RenderingSystem::OnKeyDown(WPARAM key)
{
    if (key == 'W') m_moveForward = true;
    if (key == 'S') m_moveBackward = true;
    if (key == 'A') m_moveLeft = true;
    if (key == 'D') m_moveRight = true;

    // Debug modes:
    // 0=Final, 1=Albedo, 2=Normal, 3=Material, 4=Depth,
    // 5=Lighting only, 6=Point only, 7=Spot only.
    if (key >= '0' && key <= '7')
        m_debugMode = static_cast<UINT>(key - '0');
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

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0); // ObjectTransformConstants
        params[1].InitAsConstantBufferView(1); // GeometryFrameConstants
        params[2].InitAsConstantBufferView(2); // MaterialConstants
        params[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            4,
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

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            1,
            &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingDirectionalRS)));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0); // LightingFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(1); // LocalLightConstants

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
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_lightingLocalRS)));
    }
}

void RenderingSystem::CreatePSOs()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileShader = [&](const wchar_t* file, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob)
    {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(
            file,
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry,
            target,
            flags,
            0,
            &outBlob,
            &errors);

        if (FAILED(hr))
        {
            std::string message = "Shader compilation failed";
            if (errors && errors->GetBufferPointer())
            {
                message += ": ";
                message += static_cast<const char*>(errors->GetBufferPointer());
            }
            throw std::runtime_error(message);
        }
    };

    compileShader(L"GeometryPass.hlsl", "VSMain", "vs_5_0", m_geoVS);
    compileShader(L"GeometryPass.hlsl", "PSMain", "ps_5_0", m_geoPS);
    compileShader(L"LightingPass.hlsl", "VSFullscreen", "vs_5_0", m_lightFullscreenVS);

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
    geoDesc.NumRenderTargets = 3;
    geoDesc.RTVFormats[0] = m_gbuffer.GetFormat(GBuffer::Albedo);
    geoDesc.RTVFormats[1] = m_gbuffer.GetFormat(GBuffer::Normal);
    geoDesc.RTVFormats[2] = m_gbuffer.GetFormat(GBuffer::Material);
    geoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    geoDesc.SampleDesc.Count = 1;

    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&geoDesc, IID_PPV_ARGS(&m_geometryPSO)));

    auto makeFullscreenLightingPso = [&](const char* psEntry, bool additive, ID3D12RootSignature* rootSignature, ComPtr<ID3D12PipelineState>& outPSO)
    {
        ComPtr<ID3DBlob> psBlob;
        compileShader(L"LightingPass.hlsl", psEntry, "ps_5_0", psBlob);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.InputLayout = { nullptr, 0 };
        desc.pRootSignature = rootSignature;
        desc.VS = { m_lightFullscreenVS->GetBufferPointer(), m_lightFullscreenVS->GetBufferSize() };
        desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        if (additive)
        {
            desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
            desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        }

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

    makeFullscreenLightingPso("PSDirectional", false, m_lightingDirectionalRS.Get(), m_psoDirectional);
    makeFullscreenLightingPso("PSLocalLights", true, m_lightingLocalRS.Get(), m_psoLocal);
}

void RenderingSystem::SetupSceneLights()
{
    m_pointLights.fill(LightingContract::PointLightData{});
    m_spotLights.fill(LightingContract::SpotLightData{});
    m_activePointLights = 0;
    m_activeSpotLights = 0;

    auto setPointLight = [this](size_t index, const LightingContract::PointLightData& light)
    {
        if (index < m_pointLights.size())
        {
            m_pointLights[index] = light;
            const UINT usedCount = static_cast<UINT>(index + 1);
            if (usedCount > m_activePointLights)
                m_activePointLights = usedCount;
        }
    };

    auto setSpotLight = [this](size_t index, const LightingContract::SpotLightData& light)
    {
        if (index < m_spotLights.size())
        {
            m_spotLights[index] = light;
            const UINT usedCount = static_cast<UINT>(index + 1);
            if (usedCount > m_activeSpotLights)
                m_activeSpotLights = usedCount;
        }
    };

    setPointLight(0, LightingContract::PointLightData{ XMFLOAT3(-620.f, 130.f, -520.f), 520.f, XMFLOAT3(1.00f, 0.25f, 0.20f), 2.20f });
    setPointLight(1, LightingContract::PointLightData{ XMFLOAT3(-620.f, 135.f, 520.f), 520.f, XMFLOAT3(0.20f, 0.55f, 1.00f), 2.20f });
    setPointLight(2, LightingContract::PointLightData{ XMFLOAT3(-260.f, 135.f, -40.f), 500.f, XMFLOAT3(1.00f, 0.72f, 0.20f), 2.10f });
    setPointLight(3, LightingContract::PointLightData{ XMFLOAT3(-120.f, 145.f, 760.f), 520.f, XMFLOAT3(0.20f, 0.95f, 1.00f), 2.20f });
    setPointLight(4, LightingContract::PointLightData{ XMFLOAT3(620.f, 130.f, -520.f), 520.f, XMFLOAT3(0.30f, 1.00f, 0.35f), 2.20f });
    setPointLight(5, LightingContract::PointLightData{ XMFLOAT3(620.f, 135.f, 520.f), 520.f, XMFLOAT3(1.00f, 0.92f, 0.30f), 2.20f });
    setPointLight(6, LightingContract::PointLightData{ XMFLOAT3(260.f, 135.f, -40.f), 500.f, XMFLOAT3(0.95f, 0.35f, 1.00f), 2.10f });
    setPointLight(7, LightingContract::PointLightData{ XMFLOAT3(120.f, 145.f, 760.f), 520.f, XMFLOAT3(1.00f, 0.88f, 0.80f), 2.20f });

    LightingContract::SpotLightData leftSpot{};
    leftSpot.Position = XMFLOAT3(-760.f, 430.f, -180.f);
    leftSpot.Range = 900.f;
    leftSpot.Direction = XMFLOAT3(0.35f, -1.f, 0.12f);
    leftSpot.InnerCos = 0.88f;
    leftSpot.Color = XMFLOAT3(1.00f, 0.20f, 0.20f);
    leftSpot.OuterCos = 0.79f;
    leftSpot.Intensity = 2.40f;
    setSpotLight(0, leftSpot);

    LightingContract::SpotLightData rightSpot{};
    rightSpot.Position = XMFLOAT3(760.f, 430.f, -180.f);
    rightSpot.Range = 900.f;
    rightSpot.Direction = XMFLOAT3(-0.35f, -1.f, 0.12f);
    rightSpot.InnerCos = 0.88f;
    rightSpot.Color = XMFLOAT3(0.20f, 1.00f, 0.30f);
    rightSpot.OuterCos = 0.79f;
    rightSpot.Intensity = 2.40f;
    setSpotLight(1, rightSpot);

    LightingContract::SpotLightData backSpot{};
    backSpot.Position = XMFLOAT3(0.f, 460.f, 980.f);
    backSpot.Range = 980.f;
    backSpot.Direction = XMFLOAT3(0.0f, -1.f, -0.28f);
    backSpot.InnerCos = 0.87f;
    backSpot.Color = XMFLOAT3(0.25f, 0.50f, 1.00f);
    backSpot.OuterCos = 0.78f;
    backSpot.Intensity = 2.20f;
    setSpotLight(2, backSpot);
}

void RenderingSystem::GeometryPass()
{
    auto cmdList = m_renderer.GetCmdList();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] =
    {
        m_gbuffer.GetRtvHandle(GBuffer::Albedo),
        m_gbuffer.GetRtvHandle(GBuffer::Normal),
        m_gbuffer.GetRtvHandle(GBuffer::Material),
    };

    auto dsv = m_renderer.GetDsvHandle();
    cmdList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    cmdList->SetGraphicsRootSignature(m_geometryRS.Get());
    cmdList->SetPipelineState(m_geometryPSO.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    GeometryFrameConstants frame{};
    frame.View = m_view;
    frame.Proj = m_proj;

    void* frameMapped = nullptr;
    m_geometryFrameCB->Map(0, nullptr, &frameMapped);
    memcpy(frameMapped, &frame, sizeof(frame));
    m_geometryFrameCB->Unmap(0, nullptr);

    const auto& subsets = m_renderer.GetSubsets();
    const auto& materials = m_renderer.GetMaterials();

    if (subsets.empty())
        return;

    void* transformMapped = nullptr;
    void* materialMapped = nullptr;
    m_objectTransformCB->Map(0, nullptr, &transformMapped);
    m_materialCB->Map(0, nullptr, &materialMapped);

    std::uint8_t* transformBase = reinterpret_cast<std::uint8_t*>(transformMapped);
    std::uint8_t* materialBase = reinterpret_cast<std::uint8_t*>(materialMapped);

    for (size_t subsetIndex = 0; subsetIndex < subsets.size(); ++subsetIndex)
    {
        if (subsetIndex >= m_maxObjectCbCount)
            break;

        const auto& s = subsets[subsetIndex];

        ObjectTransformConstants transform{};
        XMStoreFloat4x4(&transform.World, XMMatrixTranspose(XMMatrixIdentity()));
        XMStoreFloat4x4(&transform.WorldInvTranspose, XMMatrixTranspose(XMMatrixIdentity()));

        MaterialConstants material{};
        material.MaterialDiffuse = XMFLOAT4(1, 1, 1, 1);
        material.MaterialSpecular = XMFLOAT4(1, 1, 1, 1);
        material.SpecularPower = 32.0f;
        material.HasTexture = 0;

        UINT textureSrv = 0;
        if (s.materialIdx >= 0 && s.materialIdx < static_cast<int>(materials.size()))
        {
            const auto& mat = materials[s.materialIdx];
            material.MaterialDiffuse = mat.diffuse;
            material.MaterialSpecular = mat.specular;
            material.SpecularPower = mat.specPower;
            if (mat.srvHeapIndex >= 0)
            {
                material.HasTexture = 1;
                textureSrv = static_cast<UINT>(mat.srvHeapIndex);
            }
        }

        const UINT transformOffset = static_cast<UINT>(subsetIndex * m_objectTransformCbStride);
        const UINT materialOffset = static_cast<UINT>(subsetIndex * m_materialCbStride);

        memcpy(transformBase + transformOffset, &transform, sizeof(transform));
        memcpy(materialBase + materialOffset, &material, sizeof(material));

        cmdList->SetGraphicsRootConstantBufferView(0, m_objectTransformCB->GetGPUVirtualAddress() + transformOffset);
        cmdList->SetGraphicsRootConstantBufferView(1, m_geometryFrameCB->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootConstantBufferView(2, m_materialCB->GetGPUVirtualAddress() + materialOffset);
        cmdList->SetGraphicsRootDescriptorTable(3, m_renderer.GetSrvGpuHandle(textureSrv));
        cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexStart, 0, 0);
    }

    m_objectTransformCB->Unmap(0, nullptr);
    m_materialCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateFrameConstants()
{
    LightingContract::LightingFrameConstants cb{};
    cb.EyePos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    cb.ScreenSize = XMFLOAT2(static_cast<float>(m_renderer.GetWidth()), static_cast<float>(m_renderer.GetHeight()));
    cb.InvScreenSize = XMFLOAT2(1.0f / cb.ScreenSize.x, 1.0f / cb.ScreenSize.y);
    cb.AmbientColor = XMFLOAT4(0.16f, 0.16f, 0.18f, 1.0f);

    const XMVECTOR dirLight = XMVector3Normalize(XMVectorSet(0.20f, -1.0f, 0.10f, 0.0f));
    XMStoreFloat3(&cb.DirectionalLight.Direction, dirLight);
    cb.DirectionalLight.Color = XMFLOAT3(0.95f, 0.97f, 1.00f);
    cb.DirectionalLight.Intensity = 1.35f;

    XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_proj));
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));

    cb.PointLightCount = m_activePointLights;
    cb.SpotLightCount = m_activeSpotLights;
    cb.DebugMode = m_debugMode;

    void* mapped = nullptr;
    m_frameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_frameCB->Unmap(0, nullptr);
}

void RenderingSystem::UpdateLocalLightConstants()
{
    LightingContract::LocalLightConstants lights{};

    for (UINT i = 0; i < m_activePointLights; ++i)
    {
        lights.PointLights[i] = m_pointLights[i];
    }

    for (UINT i = 0; i < m_activeSpotLights; ++i)
    {
        lights.SpotLights[i] = m_spotLights[i];

        XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&lights.SpotLights[i].Direction));
        XMStoreFloat3(&lights.SpotLights[i].Direction, direction);

        lights.SpotLights[i].OuterCos = std::clamp(lights.SpotLights[i].OuterCos, 0.0f, 0.9999f);
        lights.SpotLights[i].InnerCos = std::clamp(lights.SpotLights[i].InnerCos, lights.SpotLights[i].OuterCos, 0.9999f);
    }

    void* mapped = nullptr;
    m_localLightsCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &lights, sizeof(lights));
    m_localLightsCB->Unmap(0, nullptr);
}

void RenderingSystem::LightingPassDirectional()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingDirectionalRS.Get());
    cmdList->SetPipelineState(m_psoDirectional.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSrvGpu());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::LightingPassLocal()
{
    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_lightingLocalRS.Get());
    cmdList->SetPipelineState(m_psoLocal.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_frameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_gbuffer.GetFirstSrvGpu());
    cmdList->SetGraphicsRootConstantBufferView(2, m_localLightsCB->GetGPUVirtualAddress());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::DrawScene(float totalTime, float deltaTime)
{
    (void)totalTime;
    UpdateCamera(deltaTime);

    auto cmdList = m_renderer.GetCmdList();

    m_gbuffer.BeginGeometryPass(cmdList);
    m_gbuffer.Clear(cmdList);
    GeometryPass();

    m_renderer.TransitionDepthToShaderResource();
    m_gbuffer.EndGeometryPass(cmdList);

    UpdateFrameConstants();
    UpdateLocalLightConstants();

    LightingPassDirectional();

    // Local lights are only needed in final and lighting debug modes.
    if (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 7)
    {
        LightingPassLocal();
    }
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
