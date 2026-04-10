#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <cstdio>

using namespace DirectX;

static void RS_ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("RenderingSystem DX call failed");
}

static float RS_Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

struct alignas(256) RainProxyFrameConstants
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4 CameraRightAndRadius;
    XMFLOAT4 CameraUpAndSoftness;
    UINT PointLightCount = 0;
    XMFLOAT3 Padding = { 0.0f, 0.0f, 0.0f };
};

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
    m_renderer.CreateBuffer(nullptr, sizeof(RainProxyFrameConstants), &m_rainProxyFrameCB);

    const UINT pointLightsBufferSize = static_cast<UINT>(sizeof(LightingContract::PointLightData) * LightingContract::MaxPointLights);
    m_renderer.CreateBuffer(nullptr, pointLightsBufferSize, &m_pointLightsUploadBuffer);

    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(pointLightsBufferSize);
    RS_ThrowIfFailed(m_renderer.GetDevice()->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_pointLightsDefaultBuffer)));

    D3D12_SHADER_RESOURCE_VIEW_DESC pointLightsSrvDesc{};
    pointLightsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pointLightsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    pointLightsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    pointLightsSrvDesc.Buffer.FirstElement = 0;
    pointLightsSrvDesc.Buffer.NumElements = LightingContract::MaxPointLights;
    pointLightsSrvDesc.Buffer.StructureByteStride = sizeof(LightingContract::PointLightData);
    pointLightsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE pointLightsSrvCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_renderer.GetSrvHeap()->GetCPUDescriptorHandleForHeapStart(),
        PointLightsSrvIndex,
        m_renderer.GetSrvDescriptorSize());
    m_renderer.GetDevice()->CreateShaderResourceView(m_pointLightsDefaultBuffer.Get(), &pointLightsSrvDesc, pointLightsSrvCpuHandle);

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
    // 0=Final
    // 1=Albedo
    // 2=Normal
    // 3=Material
    // 4=Depth
    // 5=Lighting only
    // 6=Point lights only (all active point lights)
    // 7=Spot lights only
    // 8=Rain point lights debug (alias of point-only for explicit QA)
    if (key >= '0' && key <= '8')
        m_debugMode = static_cast<UINT>(key - '0');

    // Geometry debug visualization (F1..F4):
    // F1 = regular render
    // F2 = transformed normal visualization
    // F3 = displacement value visualization
    // F4 = tessellation factor visualization
    if (key == VK_F1) m_geometryDebugMode = 0;
    if (key == VK_F2) m_geometryDebugMode = 1;
    if (key == VK_F3) m_geometryDebugMode = 2;
    if (key == VK_F4) m_geometryDebugMode = 3;
    if (key == VK_F5) m_debugStrongDisplacement = (m_debugStrongDisplacement == 0) ? 1u : 0u;
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
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstantBufferView(0); // ObjectTransformConstants
        params[1].InitAsConstantBufferView(1); // GeometryFrameConstants
        params[2].InitAsConstantBufferView(2); // MaterialConstants
        params[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);

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
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);

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

    {
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0); // RainProxyFrameConstants
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_ROOT_SIGNATURE_DESC desc(
            2,
            params,
            0,
            nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        RS_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rainProxyRS)));
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
    compileShader(L"GeometryPass.hlsl", "HSMain", "hs_5_0", m_geoHS);
    compileShader(L"GeometryPass.hlsl", "DSMain", "ds_5_0", m_geoDS);
    compileShader(L"GeometryPass.hlsl", "PSMain", "ps_5_0", m_geoPS);
    compileShader(L"LightingPass.hlsl", "VSFullscreen", "vs_5_0", m_lightFullscreenVS);
    compileShader(L"RainLightProxy.hlsl", "VSProxy", "vs_5_0", m_rainProxyVS);

    D3D12_INPUT_ELEMENT_DESC geoLayout[] =
    {
        { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geoDesc{};
    geoDesc.InputLayout = { geoLayout, _countof(geoLayout) };
    geoDesc.pRootSignature = m_geometryRS.Get();
    geoDesc.VS = { m_geoVS->GetBufferPointer(), m_geoVS->GetBufferSize() };
    geoDesc.HS = { m_geoHS->GetBufferPointer(), m_geoHS->GetBufferSize() };
    geoDesc.DS = { m_geoDS->GetBufferPointer(), m_geoDS->GetBufferSize() };
    geoDesc.PS = { m_geoPS->GetBufferPointer(), m_geoPS->GetBufferSize() };
    geoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geoDesc.SampleMask = UINT_MAX;
    geoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
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

    {
        ComPtr<ID3DBlob> proxyPsBlob;
        compileShader(L"RainLightProxy.hlsl", "PSProxy", "ps_5_0", proxyPsBlob);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC proxyDesc{};
        proxyDesc.InputLayout = { nullptr, 0 };
        proxyDesc.pRootSignature = m_rainProxyRS.Get();
        proxyDesc.VS = { m_rainProxyVS->GetBufferPointer(), m_rainProxyVS->GetBufferSize() };
        proxyDesc.PS = { proxyPsBlob->GetBufferPointer(), proxyPsBlob->GetBufferSize() };
        proxyDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        proxyDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        proxyDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        proxyDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        proxyDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        proxyDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        proxyDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        proxyDesc.DepthStencilState.DepthEnable = FALSE;
        proxyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        proxyDesc.SampleMask = UINT_MAX;
        proxyDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        proxyDesc.NumRenderTargets = 1;
        proxyDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        proxyDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        proxyDesc.SampleDesc.Count = 1;

        RS_ThrowIfFailed(m_renderer.GetDevice()->CreateGraphicsPipelineState(&proxyDesc, IID_PPV_ARGS(&m_psoRainProxy)));
    }
}

void RenderingSystem::SetupSceneLights()
{
    InitializeRainLightSystem();

    m_spotLights.fill(LightingContract::SpotLightData{});
    m_activeSpotLights = 0;

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

void RenderingSystem::InitializeRainLightSystem()
{
    m_fallingRainLights.clear();
    m_groundedRainLights.clear();

    m_activePointLightsForGpu.clear();
    m_activePointLightsForGpu.reserve(LightingContract::MaxPointLights);
    m_activePointLights = 0;

    m_rainSpawnAccumulator = 0.0f;
    m_rainNextSpawnIndex = 1;
    m_rainDebugStats = RainDebugStats{};
    m_rainDebugFrameCounter = 0;
}

RenderingSystem::RainPointLight RenderingSystem::GenerateRainLightParameters()
{
    RainPointLight light{};

    // Recommended cold palette for rain lights:
    // cyan/blue/violet only, with no warm yellow tones.
    static constexpr XMFLOAT3 palette[] =
    {
        XMFLOAT3(0.25f, 0.60f, 1.00f), // cold blue
        XMFLOAT3(0.20f, 0.85f, 1.00f), // cyan
        XMFLOAT3(0.35f, 0.50f, 1.00f), // azure
        XMFLOAT3(0.55f, 0.40f, 0.95f), // violet
        XMFLOAT3(0.45f, 0.70f, 1.00f), // icy blue
    };

    const size_t paletteCount = std::size(palette);
    const float selector = m_rainUnitDist(m_rainRng) * static_cast<float>(paletteCount - 1);
    const size_t idxA = static_cast<size_t>(selector);
    const size_t idxB = (std::min)(idxA + 1, paletteCount - 1);
    const float t = selector - static_cast<float>(idxA);

    light.Color = XMFLOAT3(
        RS_Lerp(palette[idxA].x, palette[idxB].x, t),
        RS_Lerp(palette[idxA].y, palette[idxB].y, t),
        RS_Lerp(palette[idxA].z, palette[idxB].z, t));

    // Keep actual lighting contribution moderate to avoid full-scene overexposure.
    // Visual readability is handled mostly by the proxy pass.
    light.Range = RS_Lerp(m_rainLightRangeMin, m_rainLightRangeMax, m_rainUnitDist(m_rainRng));
    light.Intensity = RS_Lerp(m_rainLightIntensityMin, m_rainLightIntensityMax, m_rainUnitDist(m_rainRng));

    // Jitter keeps motion organic while preserving overall rain density.
    const float speedJitter = RS_Lerp(-16.0f, 16.0f, m_rainUnitDist(m_rainRng));
    light.Velocity = XMFLOAT3(0.0f, -(m_rainFallSpeed + speedJitter), 0.0f);

    return light;
}

void RenderingSystem::SpawnRainLight()
{
    if (m_fallingRainLights.size() >= static_cast<size_t>(m_rainMaxFallingLights))
        return;

    RainPointLight light = GenerateRainLightParameters();
    light.Position.x = RS_Lerp(m_rainSpawnMinXZ.x, m_rainSpawnMaxXZ.x, m_rainUnitDist(m_rainRng));
    light.Position.y = m_rainSpawnY;
    light.Position.z = RS_Lerp(m_rainSpawnMinXZ.y, m_rainSpawnMaxXZ.y, m_rainUnitDist(m_rainRng));

    light.Landed = false;
    light.SpawnIndex = m_rainNextSpawnIndex++;

    m_fallingRainLights.push_back(light);
}

void RenderingSystem::TrimGroundedLightsIfNeeded()
{
    const size_t minKeep = static_cast<size_t>(m_rainMinGroundedLights);
    const size_t maxKeep = static_cast<size_t>((std::max)(m_rainMaxGroundedLights, m_rainMinGroundedLights));

    m_rainDebugStats.GroundedTrimmedThisFrame = 0;

    // Keep at least the guaranteed floor pool, but trim oldest once we exceed upper bound.
    while (m_groundedRainLights.size() > maxKeep && m_groundedRainLights.size() > minKeep)
    {
        m_groundedRainLights.pop_front();
        ++m_rainDebugStats.GroundedTrimmedThisFrame;
    }
}

void RenderingSystem::UpdateRainLights(float dt)
{
    if (dt <= 0.0f)
        return;

    m_rainSpawnAccumulator += dt;
    while (m_rainSpawnAccumulator >= m_rainSpawnInterval)
    {
        m_rainSpawnAccumulator -= m_rainSpawnInterval;
        SpawnRainLight();
    }

    std::deque<RainPointLight> stillFalling;
    stillFalling.clear();

    for (RainPointLight& light : m_fallingRainLights)
    {
        light.Position.y += light.Velocity.y * dt;

        if (light.Position.y <= m_rainFloorY)
        {
            light.Position.y = m_rainFloorY;
            light.Velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
            light.Landed = true;
            m_groundedRainLights.push_back(light);
        }
        else
        {
            stillFalling.push_back(light);
        }
    }

    m_fallingRainLights.swap(stillFalling);
    TrimGroundedLightsIfNeeded();
}

void RenderingSystem::BuildActivePointLightsForGpu()
{
    m_activePointLightsForGpu.clear();

    const size_t maxForGpu = static_cast<size_t>((std::min)(m_rainMaxRenderablePointLights, LightingContract::MaxPointLights));
    const size_t reservedFalling = static_cast<size_t>((std::min)(m_rainReservedRenderableFallingLights, m_rainMaxFallingLights));

    auto appendLight = [this, maxForGpu](const RainPointLight& rain)
    {
        if (m_activePointLightsForGpu.size() >= maxForGpu)
            return;

        LightingContract::PointLightData gpuLight{};
        gpuLight.Position = rain.Position;
        gpuLight.Range = rain.Range;
        gpuLight.Color = rain.Color;
        gpuLight.Intensity = rain.Intensity;
        m_activePointLightsForGpu.push_back(gpuLight);
    };

    // Keep descending lights visibly active even with a large grounded pool.
    size_t fallingRendered = 0;
    for (const RainPointLight& falling : m_fallingRainLights)
    {
        if (fallingRendered >= reservedFalling)
            break;

        appendLight(falling);
        ++fallingRendered;
    }

    for (const RainPointLight& grounded : m_groundedRainLights)
    {
        appendLight(grounded);
    }

    // Use any remaining GPU budget for additional falling lights.
    for (size_t i = fallingRendered; i < m_fallingRainLights.size(); ++i)
    {
        appendLight(m_fallingRainLights[i]);
    }

    m_activePointLights = static_cast<UINT>(m_activePointLightsForGpu.size());

    m_rainDebugStats.FallingCount = static_cast<UINT>(m_fallingRainLights.size());
    m_rainDebugStats.GroundedCount = static_cast<UINT>(m_groundedRainLights.size());
    m_rainDebugStats.TotalSimulatedCount = m_rainDebugStats.FallingCount + m_rainDebugStats.GroundedCount;
    m_rainDebugStats.TotalSelectedForGpu = m_activePointLights;

    const size_t selected = m_activePointLightsForGpu.size();
    const size_t simulated = m_fallingRainLights.size() + m_groundedRainLights.size();
    m_rainDebugStats.ClippedDuringGpuSelection = (simulated > selected)
        ? static_cast<UINT>(simulated - selected)
        : 0;
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
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    cmdList->IASetVertexBuffers(0, 1, m_renderer.GetVbView());
    cmdList->IASetIndexBuffer(m_renderer.GetIbView());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    GeometryFrameConstants frame{};
    frame.View = m_view;
    frame.Proj = m_proj;
    frame.CameraPos = XMFLOAT4(m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 1.0f);
    frame.TessFactorRange = XMFLOAT2(m_tessMinFactor, m_tessMaxFactor);
    frame.TessDistanceRange = XMFLOAT2(m_tessMinDistance, m_tessMaxDistance);
    frame.GeometryDebugMode = m_geometryDebugMode;
    frame.DebugStrongDisplacement = m_debugStrongDisplacement;

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
        material.HasNormalMap = 0;
        material.HasDisplacementMap = 0;
        material.DisplacementScale = 0.0f;
        material.DisplacementBias = 0.0f;

        UINT textureSrv = 0;
        if (s.materialIdx >= 0 && s.materialIdx < static_cast<int>(materials.size()))
        {
            const auto& mat = materials[s.materialIdx];
            material.MaterialDiffuse = mat.diffuse;
            material.MaterialSpecular = mat.specular;
            material.SpecularPower = mat.specPower;
            if (mat.diffuseSrvHeapIndex >= 0)
            {
                material.HasTexture = 1;
                textureSrv = static_cast<UINT>(mat.diffuseSrvHeapIndex);
            }
            if (mat.normalSrvHeapIndex >= 0 && mat.hasNormalMap)
            {
                material.HasNormalMap = 1;
            }
            if (mat.displacementSrvHeapIndex >= 0 && mat.hasDisplacementMap)
            {
                material.HasDisplacementMap = 1;
                material.DisplacementScale = mat.displacementScale;
                material.DisplacementBias = mat.displacementBias;
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

    cb.PointLightCount = (std::min)(m_activePointLights, LightingContract::MaxPointLights);
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


void RenderingSystem::UploadPointLightsToGpu()
{
    const UINT clampedPointLightCount = static_cast<UINT>((std::min)(
        m_activePointLightsForGpu.size(),
        static_cast<size_t>(LightingContract::MaxPointLights)));
    const UINT pointLightDataSize = static_cast<UINT>(sizeof(LightingContract::PointLightData) * clampedPointLightCount);

    if (pointLightDataSize > 0)
    {
        void* mapped = nullptr;
        m_pointLightsUploadBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, m_activePointLightsForGpu.data(), pointLightDataSize);
        m_pointLightsUploadBuffer->Unmap(0, nullptr);
    }

    m_activePointLights = clampedPointLightCount;
    m_rainDebugStats.TotalUploadedToGpu = clampedPointLightCount;
    m_rainDebugStats.ClippedDuringGpuUpload = (m_activePointLightsForGpu.size() > clampedPointLightCount)
        ? static_cast<UINT>(m_activePointLightsForGpu.size() - clampedPointLightCount)
        : 0;

    auto cmdList = m_renderer.GetCmdList();

    auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pointLightsDefaultBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        m_pointLightsDefaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    static bool initializedForShaderRead = false;
    if (initializedForShaderRead)
    {
        cmdList->ResourceBarrier(1, &toCopyDest);
    }

    if (pointLightDataSize > 0)
    {
        cmdList->CopyBufferRegion(
            m_pointLightsDefaultBuffer.Get(),
            0,
            m_pointLightsUploadBuffer.Get(),
            0,
            pointLightDataSize);
    }

    cmdList->ResourceBarrier(1, &toShaderResource);
    initializedForShaderRead = true;
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

void RenderingSystem::RainLightProxyPass()
{
    if (m_activePointLights == 0)
    {
        m_rainDebugStats.TotalVisibleProxiesRendered = 0;
        return;
    }

    RainProxyFrameConstants cb{};
    cb.View = m_view;
    cb.Proj = m_proj;

    const XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_view));
    const XMVECTOR cameraRight = XMVector3Normalize(XMVectorSet(view.r[0].m128_f32[0], view.r[1].m128_f32[0], view.r[2].m128_f32[0], 0.0f));
    const XMVECTOR cameraUp = XMVector3Normalize(XMVectorSet(view.r[0].m128_f32[1], view.r[1].m128_f32[1], view.r[2].m128_f32[1], 0.0f));

    XMFLOAT3 right3{};
    XMFLOAT3 up3{};
    XMStoreFloat3(&right3, cameraRight);
    XMStoreFloat3(&up3, cameraUp);

    cb.CameraRightAndRadius = XMFLOAT4(right3.x, right3.y, right3.z, m_rainProxyRadius);
    cb.CameraUpAndSoftness = XMFLOAT4(up3.x, up3.y, up3.z, m_rainProxySoftness);
    cb.PointLightCount = m_activePointLights;

    void* mapped = nullptr;
    m_rainProxyFrameCB->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    m_rainProxyFrameCB->Unmap(0, nullptr);

    auto cmdList = m_renderer.GetCmdList();
    auto rtv = m_renderer.GetBackBufferRtv();

    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetGraphicsRootSignature(m_rainProxyRS.Get());
    cmdList->SetPipelineState(m_psoRainProxy.Get());

    ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootConstantBufferView(0, m_rainProxyFrameCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootDescriptorTable(1, m_renderer.GetSrvGpuHandle(PointLightsSrvIndex));

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(6, m_activePointLights, 0, 0);
    m_rainDebugStats.TotalVisibleProxiesRendered = m_activePointLights;
}

void RenderingSystem::DrawScene(float totalTime, float deltaTime)
{
    (void)totalTime;
    UpdateCamera(deltaTime);
    UpdateRainLights(deltaTime);
    BuildActivePointLightsForGpu();

    auto cmdList = m_renderer.GetCmdList();

    m_gbuffer.BeginGeometryPass(cmdList);
    m_gbuffer.Clear(cmdList);
    GeometryPass();

    m_renderer.TransitionDepthToShaderResource();
    m_gbuffer.EndGeometryPass(cmdList);

    UpdateFrameConstants();
    UpdateLocalLightConstants();
    UploadPointLightsToGpu();

    LightingPassDirectional();

    // Local lights are only needed in final and lighting debug modes.
    if (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 7 || m_debugMode == 8)
    {
        LightingPassLocal();
    }

    if (m_debugMode == 0 || m_debugMode == 5 || m_debugMode == 6 || m_debugMode == 8)
    {
        RainLightProxyPass();
    }

    if (m_rainDebugOutputEnabled)
    {
        ++m_rainDebugFrameCounter;
        if (m_rainDebugFrameCounter % (std::max)(1u, m_rainDebugOutputIntervalFrames) == 0)
        {
            char msg[512];
            std::snprintf(
                msg,
                sizeof(msg),
                "[RainDebug] falling=%u grounded=%u simulated=%u selectedGPU=%u uploadedGPU=%u proxies=%u clipSelect=%u clipUpload=%u groundedTrim=%u\n",
                m_rainDebugStats.FallingCount,
                m_rainDebugStats.GroundedCount,
                m_rainDebugStats.TotalSimulatedCount,
                m_rainDebugStats.TotalSelectedForGpu,
                m_rainDebugStats.TotalUploadedToGpu,
                m_rainDebugStats.TotalVisibleProxiesRendered,
                m_rainDebugStats.ClippedDuringGpuSelection,
                m_rainDebugStats.ClippedDuringGpuUpload,
                m_rainDebugStats.GroundedTrimmedThisFrame);
            OutputDebugStringA(msg);
        }
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
