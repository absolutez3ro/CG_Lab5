#pragma once
#define WIN32_LEAN_AND_MEAN
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
	// Load image file into CPU memory
	static bool LoadFromFile(const std::wstring& path, TextureData& out);
	// Upload CPU data to a GPU default heap texture.
	// uploadBuf must stay alive until command list is executed.
	static bool CreateTexture(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* cmdList,
		const TextureData& data,
		ComPtr<ID3D12Resource>& texture,
		ComPtr<ID3D12Resource>& uploadBuf);
};