#pragma once
#include "Renderer.h"
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>

class ParticleSystemGPU
{
public:
    struct alignas(16) GpuParticle
    {
        DirectX::XMFLOAT3 Position;
        float Life;
        DirectX::XMFLOAT3 Velocity;
        float LifeSpan;
        DirectX::XMFLOAT4 Color;
        float Size;
        float Weight;
        float Age;
        float Padding;
    };

    struct alignas(16) ParticleSortEntry
    {
        uint32_t ParticleIndex;
        float DistanceSq;
        float Padding0;
        float Padding1;
    };

    bool Initialize(Renderer* renderer);
    void Shutdown();
    void Reinitialize(ID3D12GraphicsCommandList* cmdList);

    struct FountainSettings
    {
        DirectX::XMFLOAT3 BaseVelocity = { 0.0f, 30.0f, 0.0f };
        DirectX::XMFLOAT3 VelocityRandomness = { 12.0f, 8.0f, 12.0f };
        DirectX::XMFLOAT3 Gravity = { 0.0f, -9.8f, 0.0f };
        DirectX::XMFLOAT4 StartColorA = { 1.0f, 0.65f, 0.15f, 1.0f };
        DirectX::XMFLOAT4 StartColorB = { 0.35f, 0.55f, 1.0f, 1.0f };
        float MinLifeSpan = 2.0f;
        float MaxLifeSpan = 4.5f;
        float MinSize = 1.6f;
        float MaxSize = 4.0f;
        float GroundY = -10.0f;
        uint32_t EmitPerFrame = 48;
        uint32_t EnableGroundCollision = 1;
    };

    void Update(
        ID3D12GraphicsCommandList* cmdList,
        float deltaTime,
        float totalTime,
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMFLOAT3& emitterPos,
        const FountainSettings& settings);

    void Render(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& proj,
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMFLOAT3& directionalLightDir,
        float directionalIntensity,
        const DirectX::XMFLOAT3& directionalLightColor,
        const DirectX::XMFLOAT4& ambientColor);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    void SetSortEnabled(bool enabled) { m_sortEnabled = enabled; }
    bool IsSortEnabled() const { return m_sortEnabled; }
    uint32_t GetAliveCountForDraw() const { return m_aliveCountForDraw; }

private:
    static constexpr uint32_t MaxParticles = 4096;
    static constexpr uint32_t ThreadsPerGroup = 256;

    struct alignas(16) ParticleEmitConstants
    {
        DirectX::XMFLOAT3 EmitterPosition;
        uint32_t EmitCount;
        DirectX::XMFLOAT3 BaseVelocity;
        float Time;
        DirectX::XMFLOAT3 VelocityRandomness;
        float MinLifeSpan;
        float MaxLifeSpan;
        float MinSize;
        float MaxSize;
        uint32_t RandomSeed;
        DirectX::XMFLOAT4 StartColorA;
        DirectX::XMFLOAT4 StartColorB;
    };

    struct alignas(16) ParticleUpdateConstants
    {
        float DeltaTime;
        float TotalTime;
        uint32_t MaxParticles;
        uint32_t Padding0;
        DirectX::XMFLOAT3 Gravity;
        float GroundY;
        DirectX::XMFLOAT3 CameraPosition;
        uint32_t EnableGroundCollision;
    };

    struct alignas(16) SortConstants
    {
        uint32_t ElementCount;
        uint32_t SubArraySize;
        uint32_t CompareDistance;
        uint32_t SortDescending;
    };

    struct alignas(16) ParticleRenderConstants
    {
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Proj;
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT3 CameraRight;
        float Padding0;
        DirectX::XMFLOAT3 CameraUp;
        float Padding1;
        DirectX::XMFLOAT3 DirectionalLightDir;
        float DirectionalLightIntensity;
        DirectX::XMFLOAT4 DirectionalLightColor;
        DirectX::XMFLOAT4 AmbientColor;
    };

    struct QuadVertex
    {
        DirectX::XMFLOAT2 Corner;
    };

    bool CreateRootSignatures();
    bool CreatePipelines();
    bool CreateResources();
    bool CreateQuadGeometry();
    bool CompileShader(const wchar_t* file, const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
    std::string GetExeDir() const;

    void ResetCounter(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* counterResource, uint32_t value);
    void CopyCounterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* counterResource, ID3D12Resource* readbackResource);
    void TransitionComputeResources(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);
    void DispatchBitonicSort(ID3D12GraphicsCommandList* cmdList, uint32_t elementCount);

private:
    Renderer* m_renderer = nullptr;
    bool m_initialized = false;
    bool m_enabled = true;
    bool m_sortEnabled = true;
    uint32_t m_aliveCountForDraw = 0;
    uint32_t m_frameCounter = 0;
    uint32_t m_emitPerFrame = 48;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_renderRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_initDeadPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_emitPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_updatePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_sortPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_renderPso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_particlePool;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_deadList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sortList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_deadListCounter;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sortListCounter;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_particleHeap;
    UINT m_descSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_emitCB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_updateCB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sortCB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderCB;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterResetUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counterReadback[2];
    uint32_t* m_counterReadbackMapped[2] = { nullptr, nullptr };

    Microsoft::WRL::ComPtr<ID3D12Resource> m_quadVB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_quadIB;
    D3D12_VERTEX_BUFFER_VIEW m_quadVbView{};
    D3D12_INDEX_BUFFER_VIEW m_quadIbView{};

    D3D12_RESOURCE_STATES m_particlePoolState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_deadListState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_sortListState = D3D12_RESOURCE_STATE_COMMON;
};

static_assert(sizeof(ParticleSystemGPU::GpuParticle) == 64, "GpuParticle layout must match HLSL.");
