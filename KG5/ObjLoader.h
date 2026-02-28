#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <vector>
#include <DirectXMath.h>
using namespace DirectX;
struct Material
{
	std::string name;
	XMFLOAT4 diffuse = { 0.8f, 0.8f, 0.8f, 1.f };
	XMFLOAT4 specular = { 0.5f, 0.5f, 0.5f, 1.f };
	float shininess = 32.f;
	std::string diffuseTexture;
};
struct MeshSubset
{
	UINT indexStart = 0;
	UINT indexCount = 0;
	int materialIdx = -1;
};
struct ObjMesh
{
	struct Vertex
	{
		XMFLOAT3 Position;
		XMFLOAT3 Normal;
		XMFLOAT2 TexCoord;
	};
	std::vector<Vertex> vertices;
	std::vector<UINT> indices;
	std::vector<MeshSubset> subsets;
	std::vector<Material> materials;
};
class ObjLoader
{
public:
	static bool Load(const std::string& path, ObjMesh& out);
private:
	static bool LoadMtl(const std::string& mtlPath,
		std::vector<Material>& materials);
};