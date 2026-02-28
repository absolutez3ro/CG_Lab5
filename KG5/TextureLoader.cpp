#include "TextureLoader.h"
#include <wincodec.h>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool TextureLoader::LoadFromFile(const std::wstring& path, TextureData& out)
{
	// Конвертируем wstring в string
	std::string narrowPath(path.begin(), path.end());

	int w, h, channels;
	unsigned char* data = stbi_load(narrowPath.c_str(), &w, &h, &channels, 4);
	if (!data) return false;

	out.width = (UINT)w;
	out.height = (UINT)h;
	out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
	out.rowPitch = (UINT)w * 4;
	out.pixels.assign(data, data + (size_t)w * h * 4);

	stbi_image_free(data);
	return true;
}
// -------------------------------------------------------
// Upload to GPU default heap
// -------------------------------------------------------
bool TextureLoader::CreateTexture(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	const TextureData& data,
	ComPtr<ID3D12Resource>& texture,
	ComPtr<ID3D12Resource>& uploadBuf)
{
	// Default heap texture
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
	// Upload heap buffer
	UINT64 uploadSize = 0;
	device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
	CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
	hr = device->CreateCommittedResource(
		&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&uploadBuf));
	if (FAILED(hr)) return false;
	// Copy pixels into upload buffer
	D3D12_SUBRESOURCE_DATA subData{};
	subData.pData = data.pixels.data();
	subData.RowPitch = data.rowPitch;
	subData.SlicePitch = (LONG_PTR)data.rowPitch * data.height;
	UpdateSubresources(cmdList, texture.Get(), uploadBuf.Get(),
		0, 0, 1, &subData);
	// Transition to shader resource
	CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		texture.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmdList->ResourceBarrier(1, &barrier);
	return true;
}