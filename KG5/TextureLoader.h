#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "d3dx12.h"
using Microsoft::WRL::ComPtr;

class TextureLoader
{
public:
    struct TextureData
    {
        std::vector<uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT rowPitch = 0;
    };

    struct DDSData
    {
        std::vector<uint8_t> pixels;
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        UINT width = 0;
        UINT height = 0;
        UINT arraySize = 1;
        UINT mipLevels = 1;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool isCubeMap = false;

        // Raw DDS header/pixel-format values are retained for detailed IBL diagnostics,
        // especially when a legacy DDS is rejected before GPU upload.
        UINT headerFlags = 0;
        UINT headerMipMapCount = 0;
        UINT headerCaps = 0;
        UINT headerCaps2 = 0;
        UINT pixelFormatFlags = 0;
        UINT pixelFormatFourCC = 0;
        UINT pixelFormatRGBBitCount = 0;
        UINT pixelFormatRBitMask = 0;
        UINT pixelFormatGBitMask = 0;
        UINT pixelFormatBBitMask = 0;
        UINT pixelFormatABitMask = 0;
    };

    // Load image file into CPU memory
    static bool LoadFromFile(const std::wstring& path, TextureData& out);
    static bool LoadDDSFromFile(const std::wstring& path, DDSData& out);
    static const std::string& GetLastError();

    // Upload CPU data to a GPU default heap texture.
    // uploadBuf must stay alive until command list is executed.
    static bool CreateTexture(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const TextureData& data,
        ComPtr<ID3D12Resource>& texture,
        ComPtr<ID3D12Resource>& uploadBuf);

    static bool CreateTextureFromDDS(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const DDSData& data,
        ComPtr<ID3D12Resource>& texture,
        ComPtr<ID3D12Resource>& uploadBuf);

    static bool CreateShaderResourceView(
        ID3D12Device* device,
        ID3D12Resource* texture,
        DXGI_FORMAT format,
        bool isCubeMap,
        UINT mipLevels,
        D3D12_CPU_DESCRIPTOR_HANDLE destHandle);
};
