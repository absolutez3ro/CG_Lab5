#pragma once
#include "Renderer.h"
#include "GBuffer.h"
#include <array>

struct alignas(256) LightingFrameConstants
{
    XMFLOAT4 EyePos;
    XMFLOAT2 ScreenSize;
    XMFLOAT2 InvScreenSize;
    XMFLOAT4 AmbientColor;
    XMFLOAT4 DirLightDirection;
    XMFLOAT4 DirLightColorIntensity;
    XMFLOAT4X4 InvViewProj;
};

struct alignas(256) LightVolumeConstants
{
    XMFLOAT4X4 WorldViewProj;
    XMFLOAT4 PositionRange;
    XMFLOAT4 DirectionCos;
    XMFLOAT4 ColorIntensity;
};

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

    struct PointLight
    {
        XMFLOAT3 Position;
        float Range;
        XMFLOAT3 Color;
        float Intensity;
    };

    struct SpotLight
    {
        XMFLOAT3 Position;
        float Range;
        XMFLOAT3 Direction;
        float CosAngle;
        XMFLOAT3 Color;
        float Intensity;
    };

    void CreateRootSignatures();
    void CreatePSOs();
    void CreateLightMeshes();
    void SetupSceneLights();

    void GeometryPass();
    void LightingPassDirectional();
    void LightingPassPoint();
    void LightingPassSpot();

    void UpdateFrameConstants();
    void UpdateCamera(float dt);
    void UpdateViewMatrix();
    void UpdatePointLightCB(const PointLight& light);
    void UpdateSpotLightCB(const SpotLight& light);

private:
    Renderer m_renderer;
    GBuffer m_gbuffer;

    XMFLOAT2 m_texTiling = { 1, 1 };
    XMFLOAT2 m_texScroll = { 0, 0 };

    ComPtr<ID3D12RootSignature> m_geometryRS;
    ComPtr<ID3D12RootSignature> m_lightingRS;

    ComPtr<ID3D12PipelineState> m_geometryPSO;
    ComPtr<ID3D12PipelineState> m_psoDirectional;
    ComPtr<ID3D12PipelineState> m_psoPoint;
    ComPtr<ID3D12PipelineState> m_psoSpot;

    ComPtr<ID3DBlob> m_geoVS;
    ComPtr<ID3DBlob> m_geoPS;
    ComPtr<ID3DBlob> m_lightFullscreenVS;
    ComPtr<ID3DBlob> m_lightVS;

    ComPtr<ID3D12Resource> m_sphereVB;
    ComPtr<ID3D12Resource> m_sphereIB;
    D3D12_VERTEX_BUFFER_VIEW m_sphereVBV{};
    D3D12_INDEX_BUFFER_VIEW m_sphereIBV{};
    UINT m_sphereIndexCount = 0;

    ComPtr<ID3D12Resource> m_objectCB;
    ComPtr<ID3D12Resource> m_frameCB;
    ComPtr<ID3D12Resource> m_lightVolCB;
    UINT m_objectCbStride = 0;
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

    std::array<PointLight, 8> m_pointLights{};
    std::array<SpotLight, 3> m_spotLights{};
};
