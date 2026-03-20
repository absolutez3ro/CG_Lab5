#pragma once
#include "Renderer.h"
#include "GBuffer.h"
#include "LightingContract.h"
#include <array>

class RenderingSystem
{
public:
    bool Init(HWND hwnd, int width, int height);
    void BeginFrame(const float clearColor[4]);
    void DrawScene(float totalTime, float deltaTime);
    void EndFrame() { m_renderer.EndFrame(); }
    void OnResize(int width, int height);
    bool LoadObj(const std::string& path) { return m_renderer.LoadObj(path); }

    void SetTexTiling(float x, float y) { m_texTiling = { x, y }; }
    void SetTexScroll(float x, float y) { m_texScroll = { x, y }; }

    void OnKeyDown(WPARAM key);
    void OnKeyUp(WPARAM key);
    void OnMouseDown(int x, int y);
    void OnMouseUp();
    void OnMouseMove(int x, int y);

private:
    bool m_initialized = false;

    void CreateRootSignatures();
    void CreatePSOs();
    void SetupSceneLights();

    void GeometryPass();
    void LightingPassDirectional();
    void LightingPassLocal();

    void UpdateFrameConstants();
    void UpdateLocalLightConstants();
    void UpdateCamera(float dt);
    void UpdateViewMatrix();

private:
    Renderer m_renderer;
    GBuffer m_gbuffer;

    XMFLOAT2 m_texTiling = { 1, 1 };
    XMFLOAT2 m_texScroll = { 0, 0 };

    ComPtr<ID3D12RootSignature> m_geometryRS;
    ComPtr<ID3D12RootSignature> m_lightingDirectionalRS;
    ComPtr<ID3D12RootSignature> m_lightingLocalRS;

    ComPtr<ID3D12PipelineState> m_geometryPSO;
    ComPtr<ID3D12PipelineState> m_psoDirectional;
    ComPtr<ID3D12PipelineState> m_psoLocal;

    ComPtr<ID3DBlob> m_geoVS;
    ComPtr<ID3DBlob> m_geoPS;
    ComPtr<ID3DBlob> m_lightFullscreenVS;

    ComPtr<ID3D12Resource> m_objectTransformCB;
    ComPtr<ID3D12Resource> m_geometryFrameCB;
    ComPtr<ID3D12Resource> m_materialCB;
    ComPtr<ID3D12Resource> m_frameCB;
    ComPtr<ID3D12Resource> m_localLightsCB;
    UINT m_objectTransformCbStride = 0;
    UINT m_materialCbStride = 0;
    UINT m_maxObjectCbCount = 0;

    XMFLOAT4X4 m_view{};
    XMFLOAT4X4 m_proj{};
    XMFLOAT3 m_cameraPos = { 0.0f, 120.0f, -300.0f };
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_moveSpeed = 350.0f;
    float m_mouseSensitivity = 0.0035f;

    bool m_moveForward = false;
    bool m_moveBackward = false;
    bool m_moveLeft = false;
    bool m_moveRight = false;

    bool m_mouseLookActive = false;
    bool m_hasLastMouse = false;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;

    UINT m_debugMode = 0;

    std::array<LightingContract::PointLightData, LightingContract::MaxPointLights> m_pointLights{};
    std::array<LightingContract::SpotLightData, LightingContract::MaxSpotLights> m_spotLights{};
    UINT m_activePointLights = 0;
    UINT m_activeSpotLights = 0;
};
