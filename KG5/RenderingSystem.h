#pragma once
#include "Renderer.h"
#include "GBuffer.h"

struct alignas(256) LightingFrameConstants
{
    XMFLOAT4 EyePos;
    XMFLOAT2 ScreenSize;
    XMFLOAT2 InvScreenSize;
    XMFLOAT4 AmbientColor;
    XMFLOAT4 DirLightDirection;
    XMFLOAT4 DirLightColorIntensity;
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

private:
    void CreateRootSignatures();
    void CreatePSOs();
    void CreateLightMeshes();

    void GeometryPass();
    void LightingPassDirectional();
    void LightingPassPoint();
    void LightingPassSpot();

    void UpdateFrameConstants();
    void UpdatePointLightCB(const XMFLOAT3& pos, float range, const XMFLOAT3& color, float intensity);
    void UpdateSpotLightCB(const XMFLOAT3& pos, float range, const XMFLOAT3& dir, float cosAngle, const XMFLOAT3& color, float intensity);

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
    ComPtr<ID3DBlob> m_lightVS;
    ComPtr<ID3DBlob> m_lightPS;

    ComPtr<ID3D12Resource> m_sphereVB;
    ComPtr<ID3D12Resource> m_sphereIB;
    D3D12_VERTEX_BUFFER_VIEW m_sphereVBV{};
    D3D12_INDEX_BUFFER_VIEW m_sphereIBV{};
    UINT m_sphereIndexCount = 0;

    ComPtr<ID3D12Resource> m_objectCB;
    ComPtr<ID3D12Resource> m_frameCB;
    ComPtr<ID3D12Resource> m_lightVolCB;
};