#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <map>
#include <tuple>
#include <algorithm>
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
			if (token == "Kd")
			{
				ss >> cur.diffuse.x >> cur.diffuse.y >> cur.diffuse.z;
				cur.diffuse.w = 1.f;
			}
			else if (token == "Ks")
			{
				ss >> cur.specular.x >> cur.specular.y >> cur.specular.z;
			}
			else if (token == "Ns")
			{
				ss >> cur.shininess;
			}
			else if (token == "d")
			{
				// In Sponza's MTL, d=0 is incorrectly used but means opaque.
				// Only treat as transparent if d is between 0 and 1 exclusive.
				float d = 1.f;
				ss >> d;
				// Clamp: if d==0 treat as fully opaque (Sponza quirk)
				cur.diffuse.w = (d <= 0.f) ? 1.f : d;
			}
			else if (token == "Tr")
			{
				float tr = 0.f;
				ss >> tr;
				cur.diffuse.w = 1.f - tr;
			}
			else if (token == "map_Kd" || token == "map_Ka")
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
				cur.diffuseTexture = tex;
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