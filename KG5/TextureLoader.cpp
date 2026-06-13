#include "TextureLoader.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace
{
    std::string g_lastTextureLoaderError;

    void SetTextureLoaderError(const char* message)
    {
        g_lastTextureLoaderError = message;
    }

    constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    constexpr uint32_t DDS_ALPHA_PIXELS = 0x00000001;
    constexpr uint32_t DDS_FOURCC = 0x00000004;
    constexpr uint32_t DDS_RGB = 0x00000040;
    constexpr uint32_t DDS_LUMINANCE = 0x00020000;
    constexpr uint32_t DDS_HEADER_FLAGS_VOLUME = 0x00800000;
    constexpr uint32_t DDSCAPS2_CUBEMAP = 0x00000200;
    constexpr uint32_t DDSCAPS2_CUBEMAP_ALLFACES = 0x0000FC00;
    constexpr uint32_t DDS_RESOURCE_MISC_TEXTURECUBE = 0x4;

    uint32_t MakeFourCC(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(a) |
            (static_cast<uint32_t>(b) << 8) |
            (static_cast<uint32_t>(c) << 16) |
            (static_cast<uint32_t>(d) << 24);
    }

#pragma pack(push, 1)
    struct DDS_PIXELFORMAT
    {
        uint32_t size;
        uint32_t flags;
        uint32_t fourCC;
        uint32_t rgbBitCount;
        uint32_t rBitMask;
        uint32_t gBitMask;
        uint32_t bBitMask;
        uint32_t aBitMask;
    };

    struct DDS_HEADER
    {
        uint32_t size;
        uint32_t flags;
        uint32_t height;
        uint32_t width;
        uint32_t pitchOrLinearSize;
        uint32_t depth;
        uint32_t mipMapCount;
        uint32_t reserved1[11];
        DDS_PIXELFORMAT ddspf;
        uint32_t caps;
        uint32_t caps2;
        uint32_t caps3;
        uint32_t caps4;
        uint32_t reserved2;
    };

    struct DDS_HEADER_DXT10
    {
        DXGI_FORMAT dxgiFormat;
        uint32_t resourceDimension;
        uint32_t miscFlag;
        uint32_t arraySize;
        uint32_t miscFlags2;
    };
#pragma pack(pop)

    size_t BitsPerPixel(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 128;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 64;
        case DXGI_FORMAT_R32G32_FLOAT: return 64;
        case DXGI_FORMAT_R32_FLOAT: return 32;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM: return 32;
        case DXGI_FORMAT_R16G16_FLOAT: return 32;
        case DXGI_FORMAT_R16G16_UNORM: return 32;
        case DXGI_FORMAT_R16_FLOAT: return 16;
        case DXGI_FORMAT_R8G8_UNORM: return 16;
        case DXGI_FORMAT_R8_UNORM: return 8;
        default: return 0;
        }
    }

    bool IsBlockCompressedFormat(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    size_t BytesPerBlock(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            return 0;
        }
    }

    bool IsSupportedDDSFormat(DXGI_FORMAT fmt)
    {
        return BitsPerPixel(fmt) != 0 || IsBlockCompressedFormat(fmt);
    }

    bool GetSurfaceInfo(UINT width, UINT height, DXGI_FORMAT fmt, size_t& rowPitch, size_t& slicePitch)
    {
        if (IsBlockCompressedFormat(fmt))
        {
            const size_t blockBytes = BytesPerBlock(fmt);
            const size_t blocksWide = (std::max)(size_t(1), (static_cast<size_t>(width) + 3u) / 4u);
            const size_t blocksHigh = (std::max)(size_t(1), (static_cast<size_t>(height) + 3u) / 4u);
            rowPitch = blocksWide * blockBytes;
            slicePitch = rowPitch * blocksHigh;
            return blockBytes != 0;
        }

        const size_t bpp = BitsPerPixel(fmt);
        if (bpp == 0)
            return false;

        rowPitch = (static_cast<size_t>(width) * bpp + 7u) / 8u;
        slicePitch = rowPitch * height;
        return true;
    }

    bool LegacyFormatFromPixelFormat(const DDS_PIXELFORMAT& pf, DXGI_FORMAT& fmt)
    {
        if ((pf.flags & DDS_FOURCC) != 0)
        {
            if (pf.fourCC == MakeFourCC('D', 'X', 'T', '1')) { fmt = DXGI_FORMAT_BC1_UNORM; return true; }
            if (pf.fourCC == MakeFourCC('D', 'X', 'T', '3')) { fmt = DXGI_FORMAT_BC2_UNORM; return true; }
            if (pf.fourCC == MakeFourCC('D', 'X', 'T', '5')) { fmt = DXGI_FORMAT_BC3_UNORM; return true; }
            if (pf.fourCC == MakeFourCC('A', 'T', 'I', '1') ||
                pf.fourCC == MakeFourCC('B', 'C', '4', 'U')) { fmt = DXGI_FORMAT_BC4_UNORM; return true; }
            if (pf.fourCC == MakeFourCC('A', 'T', 'I', '2') ||
                pf.fourCC == MakeFourCC('B', 'C', '5', 'U')) { fmt = DXGI_FORMAT_BC5_UNORM; return true; }
            if (pf.fourCC == MakeFourCC('B', 'C', '4', 'S')) { fmt = DXGI_FORMAT_BC4_SNORM; return true; }
            if (pf.fourCC == MakeFourCC('B', 'C', '5', 'S')) { fmt = DXGI_FORMAT_BC5_SNORM; return true; }
            if (pf.fourCC == MakeFourCC('B', 'C', '6', 'H')) { fmt = DXGI_FORMAT_BC6H_UF16; return true; }

            // Common legacy D3DFORMAT float/integer codes used by environment-map tools.
            if (pf.fourCC == 34) { fmt = DXGI_FORMAT_R16G16_UNORM; return true; }
            if (pf.fourCC == 111) { fmt = DXGI_FORMAT_R16_FLOAT; return true; }
            if (pf.fourCC == 112) { fmt = DXGI_FORMAT_R16G16_FLOAT; return true; }
            if (pf.fourCC == 113) { fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; return true; }
            if (pf.fourCC == 114) { fmt = DXGI_FORMAT_R32_FLOAT; return true; }
            if (pf.fourCC == 115) { fmt = DXGI_FORMAT_R32G32_FLOAT; return true; }
            if (pf.fourCC == 116) { fmt = DXGI_FORMAT_R32G32B32A32_FLOAT; return true; }
            return false;
        }

        if ((pf.flags & DDS_RGB) != 0 && pf.rgbBitCount == 32 &&
            pf.rBitMask == 0x000000ff && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x00ff0000 && pf.aBitMask == 0xff000000)
        {
            fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            return true;
        }

        if ((pf.flags & DDS_RGB) != 0 && pf.rgbBitCount == 32 &&
            pf.rBitMask == 0x00ff0000 && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x000000ff && pf.aBitMask == 0xff000000)
        {
            fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
            return true;
        }

        if ((pf.flags & DDS_RGB) != 0 && pf.rgbBitCount == 32 &&
            pf.rBitMask == 0x00ff0000 && pf.gBitMask == 0x0000ff00 &&
            pf.bBitMask == 0x000000ff && pf.aBitMask == 0x00000000)
        {
            fmt = DXGI_FORMAT_B8G8R8X8_UNORM;
            return true;
        }

        if (((pf.flags & DDS_RGB) != 0 || (pf.flags & DDS_LUMINANCE) != 0) && pf.rgbBitCount == 16 &&
            pf.rBitMask == 0x000000ff &&
            (pf.gBitMask == 0x0000ff00 || ((pf.flags & DDS_ALPHA_PIXELS) != 0 && pf.aBitMask == 0x0000ff00)) &&
            pf.bBitMask == 0x00000000)
        {
            fmt = DXGI_FORMAT_R8G8_UNORM;
            return true;
        }

        if ((pf.flags & DDS_RGB) != 0 && pf.rgbBitCount == 32 &&
            pf.rBitMask == 0x0000ffff && pf.gBitMask == 0xffff0000 &&
            pf.bBitMask == 0x00000000 && pf.aBitMask == 0x00000000)
        {
            fmt = DXGI_FORMAT_R16G16_UNORM;
            return true;
        }

        return false;
    }
}

const std::string& TextureLoader::GetLastError()
{
    return g_lastTextureLoaderError;
}

bool TextureLoader::LoadFromFile(const std::wstring& path, TextureData& out)
{
    int w, h, channels;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || fp == nullptr)
        return false;

    unsigned char* data = stbi_load_from_file(fp, &w, &h, &channels, 4);
    fclose(fp);
    if (!data) return false;

    out.width = (UINT)w;
    out.height = (UINT)h;
    out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    out.rowPitch = (UINT)w * 4;
    out.pixels.assign(data, data + (size_t)w * h * 4);

    stbi_image_free(data);
    return true;
}

bool TextureLoader::LoadDDSFromFile(const std::wstring& path, DDSData& out)
{
    g_lastTextureLoaderError.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        SetTextureLoaderError("file not found");
        return false;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize < static_cast<std::streamsize>(sizeof(uint32_t) + sizeof(DDS_HEADER)))
    {
        SetTextureLoaderError("not enough DDS data");
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize))
    {
        SetTextureLoaderError("not enough DDS data");
        return false;
    }

    const uint8_t* ptr = bytes.data();
    uint32_t magic = 0;
    std::memcpy(&magic, ptr, sizeof(magic));
    if (magic != DDS_MAGIC)
    {
        SetTextureLoaderError("invalid DDS magic");
        return false;
    }
    ptr += sizeof(uint32_t);

    DDS_HEADER header{};
    std::memcpy(&header, ptr, sizeof(header));
    if (header.size != 124 || header.ddspf.size != 32)
    {
        SetTextureLoaderError("invalid DDS header");
        return false;
    }
    ptr += sizeof(header);

    out = DDSData{};
    out.width = header.width;
    out.height = header.height;
    out.mipLevels = (std::max)(header.mipMapCount, 1u);
    out.headerFlags = header.flags;
    out.headerMipMapCount = header.mipMapCount;
    out.headerCaps = header.caps;
    out.headerCaps2 = header.caps2;
    out.pixelFormatFlags = header.ddspf.flags;
    out.pixelFormatFourCC = header.ddspf.fourCC;
    out.pixelFormatRGBBitCount = header.ddspf.rgbBitCount;
    out.pixelFormatRBitMask = header.ddspf.rBitMask;
    out.pixelFormatGBitMask = header.ddspf.gBitMask;
    out.pixelFormatBBitMask = header.ddspf.bBitMask;
    out.pixelFormatABitMask = header.ddspf.aBitMask;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT arraySize = 1;
    bool isCube = (header.caps2 & DDSCAPS2_CUBEMAP) != 0;
    bool dxt10DeclaresCube = false;
    UINT mipLevels = (std::max)(header.mipMapCount, 1u);

    if ((header.ddspf.flags & DDS_FOURCC) != 0 && header.ddspf.fourCC == MakeFourCC('D', 'X', '1', '0'))
    {
        if (ptr + sizeof(DDS_HEADER_DXT10) > bytes.data() + bytes.size())
        {
            SetTextureLoaderError("not enough DDS data");
            return false;
        }

        DDS_HEADER_DXT10 dxt10{};
        std::memcpy(&dxt10, ptr, sizeof(dxt10));
        ptr += sizeof(dxt10);

        if (dxt10.resourceDimension != 3u)
        {
            SetTextureLoaderError("unsupported DDS dimension");
            return false;
        }

        format = dxt10.dxgiFormat;
        arraySize = (std::max)(dxt10.arraySize, 1u);
        dxt10DeclaresCube = ((dxt10.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) != 0);
        isCube = isCube || dxt10DeclaresCube;
        if (isCube)
            arraySize *= 6;
    }
    else if (!LegacyFormatFromPixelFormat(header.ddspf, format))
    {
        SetTextureLoaderError("unsupported DDS format");
        return false;
    }

    out.format = format;
    if (!IsSupportedDDSFormat(format))
    {
        SetTextureLoaderError("unsupported bits per pixel");
        return false;
    }
    if ((header.flags & DDS_HEADER_FLAGS_VOLUME) != 0 || header.depth > 1)
    {
        SetTextureLoaderError("unsupported DDS dimension");
        return false;
    }
    if (isCube)
    {
        if (!dxt10DeclaresCube && (header.caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES)
        {
            SetTextureLoaderError("unsupported cubemap: missing faces");
            return false;
        }
        if (arraySize < 6)
            arraySize = 6;
    }

    const uint8_t* dataBegin = ptr;
    const uint8_t* dataEnd = bytes.data() + bytes.size();

    out.arraySize = arraySize;
    out.mipLevels = mipLevels;
    out.format = format;
    out.isCubeMap = isCube;
    out.pixels.assign(dataBegin, dataEnd);
    out.subresources.reserve(static_cast<size_t>(arraySize) * mipLevels);

    size_t offset = 0;
    for (UINT arraySlice = 0; arraySlice < arraySize; ++arraySlice)
    {
        UINT mipWidth = header.width;
        UINT mipHeight = header.height;
        for (UINT mip = 0; mip < mipLevels; ++mip)
        {
            size_t rowPitch = 0;
            size_t slicePitch = 0;
            if (!GetSurfaceInfo(mipWidth, mipHeight, format, rowPitch, slicePitch))
            {
                SetTextureLoaderError("unsupported DDS format");
                return false;
            }
            if (offset + slicePitch > out.pixels.size())
            {
                SetTextureLoaderError("not enough DDS data");
                return false;
            }

            D3D12_SUBRESOURCE_DATA sub{};
            sub.pData = out.pixels.data() + offset;
            sub.RowPitch = static_cast<LONG_PTR>(rowPitch);
            sub.SlicePitch = static_cast<LONG_PTR>(slicePitch);
            out.subresources.push_back(sub);

            offset += slicePitch;
            mipWidth = (std::max)(1u, mipWidth >> 1);
            mipHeight = (std::max)(1u, mipHeight >> 1);
        }
    }

    return true;
}

bool TextureLoader::CreateTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const TextureData& data,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& uploadBuf)
{
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = data.width;
    texDesc.Height = data.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = data.format;
    texDesc.SampleDesc = { 1, 0 };
    CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return false;

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    hr = device->CreateCommittedResource(
        &upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr)) return false;

    D3D12_SUBRESOURCE_DATA subData{};
    subData.pData = data.pixels.data();
    subData.RowPitch = data.rowPitch;
    subData.SlicePitch = (LONG_PTR)data.rowPitch * data.height;
    UpdateSubresources(cmdList, texture.Get(), uploadBuf.Get(),
        0, 0, 1, &subData);

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    return true;
}

bool TextureLoader::CreateTextureFromDDS(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const DDSData& data,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& uploadBuf)
{
    g_lastTextureLoaderError.clear();
    if (data.subresources.empty() || data.format == DXGI_FORMAT_UNKNOWN)
    {
        SetTextureLoaderError("unsupported DDS format");
        return false;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = data.width;
    texDesc.Height = data.height;
    texDesc.DepthOrArraySize = static_cast<UINT16>(data.arraySize);
    texDesc.MipLevels = static_cast<UINT16>(data.mipLevels);
    texDesc.Format = data.format;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture));
    if (FAILED(hr))
    {
        SetTextureLoaderError("CreateCommittedResource failed");
        return false;
    }

    const UINT subresourceCount = static_cast<UINT>(data.subresources.size());
    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, subresourceCount);
    CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    hr = device->CreateCommittedResource(
        &upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr))
    {
        SetTextureLoaderError("CreateCommittedResource failed");
        return false;
    }

    const UINT64 uploadedBytes = UpdateSubresources(cmdList, texture.Get(), uploadBuf.Get(), 0, 0, subresourceCount, data.subresources.data());
    if (uploadedBytes == 0)
    {
        SetTextureLoaderError("UpdateSubresources failed");
        return false;
    }

    CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    return true;
}

bool TextureLoader::CreateShaderResourceView(
    ID3D12Device* device,
    ID3D12Resource* texture,
    DXGI_FORMAT format,
    bool isCubeMap,
    UINT mipLevels,
    D3D12_CPU_DESCRIPTOR_HANDLE destHandle)
{
    if (!device || !texture)
    {
        SetTextureLoaderError("CreateShaderResourceView failed");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = format;
    if (isCubeMap)
    {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = mipLevels;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    else
    {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = mipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    }

    device->CreateShaderResourceView(texture, &srvDesc, destHandle);
    return true;
}
