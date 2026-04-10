#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <map>
#include <tuple>
#include <algorithm>
#include <cmath>
#include <cctype>
// -------------------------------------------------------
// String helpers
// -------------------------------------------------------
static std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}
static std::string DirOf(const std::string& path)
{
	size_t p = path.find_last_of("/\\");
	return (p == std::string::npos) ? "" : path.substr(0, p + 1);
}
static int ResolveIndex(int idx, int total)
{
	if (idx < 0) return total + idx;
	return idx - 1;
}
// -------------------------------------------------------
// MTL loader
// -------------------------------------------------------
bool ObjLoader::LoadMtl(const std::string& mtlPath, std::vector<Material>& mats)
{
	std::ifstream f(mtlPath);
	if (!f.is_open()) return false;
	std::string line;
	int curIdx = -1;
	while (std::getline(f, line))
	{
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ss(line);
		std::string token;
		ss >> token;
		std::string tokenLower = token;
		std::transform(tokenLower.begin(), tokenLower.end(), tokenLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (token == "newmtl")
		{
			Material m;
			ss >> m.name;
			mats.push_back(m);
			curIdx = (int)mats.size() - 1;
		}
		else if (curIdx >= 0)
		{
			Material& cur = mats[curIdx];
				if (tokenLower == "kd")
				{
					ss >> cur.diffuse.x >> cur.diffuse.y >> cur.diffuse.z;
					cur.diffuse.w = 1.f;
				}
				else if (tokenLower == "ks")
				{
					ss >> cur.specular.x >> cur.specular.y >> cur.specular.z;
				}
				else if (tokenLower == "ns")
				{
					ss >> cur.shininess;
				}
				else if (tokenLower == "d")
				{
				// In Sponza's MTL, d=0 is incorrectly used but means opaque.
				// Only treat as transparent if d is between 0 and 1 exclusive.
				float d = 1.f;
				ss >> d;
				// Clamp: if d==0 treat as fully opaque (Sponza quirk)
				cur.diffuse.w = (d <= 0.f) ? 1.f : d;
			}
				else if (tokenLower == "tr")
				{
					float tr = 0.f;
					ss >> tr;
					cur.diffuse.w = 1.f - tr;
				}
				else if (tokenLower == "map_kd" || tokenLower == "map_ka" || tokenLower == "map_bump" || tokenLower == "bump" || tokenLower == "disp" || tokenLower == "map_disp")
				{
				// Read rest of line (path may contain spaces)
				std::string tex;
				std::getline(ss, tex);
				tex = Trim(tex);
				// Normalize backslashes to forward slashes
				for (char& c : tex) if (c == '\\') c = '/';
				// Strip any leading "./" or ".\\"
				if (tex.size() > 2 && tex[0] == '.' && tex[1] == '/')
					tex = tex.substr(2);
					if (tokenLower == "map_kd" || tokenLower == "map_ka")
						cur.diffuseTexture = tex;
					else if (tokenLower == "map_bump" || tokenLower == "bump")
						cur.normalTexture = tex;
					else
						cur.displacementTexture = tex;
				}
		}
	}
	return true;
}
// -------------------------------------------------------
// OBJ loader
// -------------------------------------------------------
bool ObjLoader::Load(const std::string& path, ObjMesh& out)
{
	std::ifstream f(path);
	if (!f.is_open()) return false;
	const std::string dir = DirOf(path);
	std::vector<XMFLOAT3> positions;
	std::vector<XMFLOAT3> normals;
	std::vector<XMFLOAT2> uvs;
	// Key: (posIdx, uvIdx, normIdx) -> output vertex index
	std::map<std::tuple<int, int, int>, UINT> vertexMap;
	int curMatIdx = -1;
	// Helper: close current subset and set its indexCount
	auto CloseSubset = [&]()
		{
			if (!out.subsets.empty())
			{
				MeshSubset& last = out.subsets.back();
				last.indexCount = (UINT)out.indices.size() - last.indexStart;
			}
		};
	// Helper: open a new subset
	auto OpenSubset = [&](int matIdx)
		{
			CloseSubset();
			MeshSubset s;
			s.indexStart = (UINT)out.indices.size();
			s.indexCount = 0;
			s.materialIdx = matIdx;
			out.subsets.push_back(s);
			curMatIdx = matIdx;
		};
	// Open default subset
	OpenSubset(-1);
	std::string line;
	while (std::getline(f, line))
	{
		line = Trim(line);
		if (line.empty() || line[0] == '#') continue;
		std::istringstream ss(line);
		std::string token;
		ss >> token;
		if (token == "v")
		{
			XMFLOAT3 p = {};
			ss >> p.x >> p.y >> p.z;
			positions.push_back(p);
		}
		else if (token == "vn")
		{
			XMFLOAT3 n = {};
			ss >> n.x >> n.y >> n.z;
			normals.push_back(n);
		}
		else if (token == "vt")
		{
			XMFLOAT2 uv = {};
			ss >> uv.x >> uv.y;
			uv.y = 1.f - uv.y; // flip Y
			uvs.push_back(uv);
		}
		else if (token == "mtllib")
		{
			std::string mtlFile;
			ss >> mtlFile;
			LoadMtl(dir + mtlFile, out.materials);
		}
		else if (token == "usemtl")
		{
			std::string matName;
			ss >> matName;
			int idx = -1;
			for (int i = 0; i < (int)out.materials.size(); ++i)
			{
				if (out.materials[i].name == matName)
				{
					idx = i;
					break;
				}
			}
			if (idx != curMatIdx)
				OpenSubset(idx);
		}
		else if (token == "f")
		{
			// Parse face vertices
			std::vector<UINT> faceVerts;
			std::string vert;
			while (ss >> vert)
			{
				// Replace '/' with space for easy parsing
				for (char& c : vert) if (c == '/') c = ' ';
				std::istringstream vs(vert);
				int pi = 0, ti = 0, ni = 0;
				vs >> pi;
				if (!vs.eof()) vs >> ti;
				if (!vs.eof()) vs >> ni;
				int pIdx = ResolveIndex(pi, (int)positions.size());
				int tIdx = (ti != 0) ? ResolveIndex(ti, (int)uvs.size()) : -1;
				int nIdx = (ni != 0) ? ResolveIndex(ni, (int)normals.size()) : -1;
				std::tuple<int, int, int> key(pIdx, tIdx, nIdx);
				std::map<std::tuple<int, int, int>, UINT>::iterator it = vertexMap.find(key);
				if (it == vertexMap.end())
				{
					ObjMesh::Vertex v;
					v.Position = (pIdx >= 0 && pIdx < (int)positions.size())
						? positions[pIdx] : XMFLOAT3(0, 0, 0);
					v.TexCoord = (tIdx >= 0 && tIdx < (int)uvs.size())
						? uvs[tIdx] : XMFLOAT2(0, 0);
					v.Normal = (nIdx >= 0 && nIdx < (int)normals.size())
						? normals[nIdx] : XMFLOAT3(0, 1, 0);
					v.Tangent = XMFLOAT3(1, 0, 0);
					v.Bitangent = XMFLOAT3(0, 0, 1);
					UINT newIdx = (UINT)out.vertices.size();
					out.vertices.push_back(v);
					vertexMap[key] = newIdx;
					faceVerts.push_back(newIdx);
				}
				else
				{
					faceVerts.push_back(it->second);
				}
			}
			// Triangulate (fan)
			for (size_t i = 1; i + 1 < faceVerts.size(); ++i)
			{
				out.indices.push_back(faceVerts[0]);
				out.indices.push_back(faceVerts[i]);
				out.indices.push_back(faceVerts[i + 1]);
			}
		}
	}
	// Close last subset
	CloseSubset();

	// Build tangents/bitangents from indexed triangles.
	if (!out.vertices.empty() && !out.indices.empty())
	{
		std::vector<XMFLOAT3> tanAccum(out.vertices.size(), XMFLOAT3(0.f, 0.f, 0.f));
		std::vector<XMFLOAT3> bitanAccum(out.vertices.size(), XMFLOAT3(0.f, 0.f, 0.f));

		for (size_t i = 0; i + 2 < out.indices.size(); i += 3)
		{
			const UINT i0 = out.indices[i + 0];
			const UINT i1 = out.indices[i + 1];
			const UINT i2 = out.indices[i + 2];

			if (i0 >= out.vertices.size() || i1 >= out.vertices.size() || i2 >= out.vertices.size())
				continue;

			const ObjMesh::Vertex& v0 = out.vertices[i0];
			const ObjMesh::Vertex& v1 = out.vertices[i1];
			const ObjMesh::Vertex& v2 = out.vertices[i2];

			const XMVECTOR p0 = XMLoadFloat3(&v0.Position);
			const XMVECTOR p1 = XMLoadFloat3(&v1.Position);
			const XMVECTOR p2 = XMLoadFloat3(&v2.Position);

			const XMFLOAT2 uv0 = v0.TexCoord;
			const XMFLOAT2 uv1 = v1.TexCoord;
			const XMFLOAT2 uv2 = v2.TexCoord;

			const float x1 = XMVectorGetX(p1) - XMVectorGetX(p0);
			const float y1 = XMVectorGetY(p1) - XMVectorGetY(p0);
			const float z1 = XMVectorGetZ(p1) - XMVectorGetZ(p0);
			const float x2 = XMVectorGetX(p2) - XMVectorGetX(p0);
			const float y2 = XMVectorGetY(p2) - XMVectorGetY(p0);
			const float z2 = XMVectorGetZ(p2) - XMVectorGetZ(p0);

			const float s1 = uv1.x - uv0.x;
			const float t1 = uv1.y - uv0.y;
			const float s2 = uv2.x - uv0.x;
			const float t2 = uv2.y - uv0.y;

			const float det = s1 * t2 - s2 * t1;
			if (std::abs(det) < 1e-8f)
				continue;

			const float r = 1.0f / det;
			const XMFLOAT3 triTangent(
				(t2 * x1 - t1 * x2) * r,
				(t2 * y1 - t1 * y2) * r,
				(t2 * z1 - t1 * z2) * r);
			const XMFLOAT3 triBitangent(
				(s1 * x2 - s2 * x1) * r,
				(s1 * y2 - s2 * y1) * r,
				(s1 * z2 - s2 * z1) * r);

			auto add = [](XMFLOAT3& a, const XMFLOAT3& b)
			{
				a.x += b.x; a.y += b.y; a.z += b.z;
			};

			add(tanAccum[i0], triTangent);
			add(tanAccum[i1], triTangent);
			add(tanAccum[i2], triTangent);
			add(bitanAccum[i0], triBitangent);
			add(bitanAccum[i1], triBitangent);
			add(bitanAccum[i2], triBitangent);
		}

		for (size_t i = 0; i < out.vertices.size(); ++i)
		{
			const XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&out.vertices[i].Normal));
			XMVECTOR t = XMLoadFloat3(&tanAccum[i]);
			XMVECTOR b = XMLoadFloat3(&bitanAccum[i]);

			if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-10f)
				t = XMVectorSet(1.f, 0.f, 0.f, 0.f);

			const XMVECTOR ntDot = XMVector3Dot(n, t);
			t = XMVector3Normalize(XMVectorSubtract(t, XMVectorMultiply(n, ntDot)));

			if (XMVectorGetX(XMVector3LengthSq(b)) < 1e-10f)
				b = XMVector3Normalize(XMVector3Cross(n, t));
			else
				b = XMVector3Normalize(b);

			XMStoreFloat3(&out.vertices[i].Tangent, t);
			XMStoreFloat3(&out.vertices[i].Bitangent, b);
		}
	}

	// Remove empty subsets
	std::vector<MeshSubset> nonEmpty;
	for (size_t i = 0; i < out.subsets.size(); ++i)
	{
		if (out.subsets[i].indexCount > 0)
			nonEmpty.push_back(out.subsets[i]);
	}
	out.subsets = nonEmpty;
	return !out.vertices.empty();
}
