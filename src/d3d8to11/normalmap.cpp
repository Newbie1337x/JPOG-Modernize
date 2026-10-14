// normalmap.cpp - normal maps for game textures.
//
// The texture-replacement plugin (JPOGMouseFix.asi) knows which game texture is which: for a
// texture with a "<library>__<texture>_n.dds" file in mods\textures it calls
// D3D8to11_SetNormalMap(texture, path) right after the game creates the texture. The DDS is
// loaded here (BC1/BC2/BC3/BC5, DX10 BC5, 32-bit uncompressed; all mip levels) and kept on the
// Texture; world draws that sample it on stage 0 get the relief term (shaders.cpp NormalMapCode).
// Roughness maps (<library>__<texture>_r.dds, D3D8to11_SetRoughnessMap; BC4 or 8-bit) drive the
// specular of relit dinosaurs per pixel.
#include "device.h"
#include <stdio.h>
#include <cctype>

namespace {

struct DdsInfo {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0, mips = 1;
    UINT blockBytes = 0, pixelBytes = 0;
    size_t dataOffset = 0;
};

bool ParseDds(const std::vector<BYTE>& f, DdsInfo& d)
{
    if (f.size() < 128 || memcmp(f.data(), "DDS ", 4) != 0) return false;
    auto u32 = [&](size_t o) { return *(const UINT*)(f.data() + o); };
    d.height = u32(12);
    d.width = u32(16);
    d.mips = u32(28) ? u32(28) : 1;
    const UINT pfFlags = u32(80), fourCC = u32(84), bits = u32(88), rMask = u32(92);
    d.dataOffset = 128;
    if (pfFlags & 4) { // DDPF_FOURCC
        switch (fourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): d.format = DXGI_FORMAT_BC1_UNORM; d.blockBytes = 8; break;
        case MAKEFOURCC('D', 'X', 'T', '3'): d.format = DXGI_FORMAT_BC2_UNORM; d.blockBytes = 16; break;
        case MAKEFOURCC('D', 'X', 'T', '5'): d.format = DXGI_FORMAT_BC3_UNORM; d.blockBytes = 16; break;
        case MAKEFOURCC('A', 'T', 'I', '1'): case MAKEFOURCC('B', 'C', '4', 'U'):
            d.format = DXGI_FORMAT_BC4_UNORM; d.blockBytes = 8; break;
        case MAKEFOURCC('A', 'T', 'I', '2'): case MAKEFOURCC('B', 'C', '5', 'U'):
            d.format = DXGI_FORMAT_BC5_UNORM; d.blockBytes = 16; break;
        case MAKEFOURCC('D', 'X', '1', '0'): {
            if (f.size() < 148) return false;
            const UINT dx = u32(128);
            d.dataOffset = 148;
            if (dx == DXGI_FORMAT_BC5_UNORM || dx == DXGI_FORMAT_BC5_TYPELESS) { d.format = DXGI_FORMAT_BC5_UNORM; d.blockBytes = 16; }
            else if (dx == DXGI_FORMAT_BC4_UNORM || dx == DXGI_FORMAT_BC4_TYPELESS) { d.format = DXGI_FORMAT_BC4_UNORM; d.blockBytes = 8; }
            else if (dx == DXGI_FORMAT_R8_UNORM) { d.format = DXGI_FORMAT_R8_UNORM; d.pixelBytes = 1; }
            else if (dx == DXGI_FORMAT_BC3_UNORM) { d.format = DXGI_FORMAT_BC3_UNORM; d.blockBytes = 16; }
            else if (dx == DXGI_FORMAT_BC1_UNORM) { d.format = DXGI_FORMAT_BC1_UNORM; d.blockBytes = 8; }
            else if (dx == DXGI_FORMAT_R8G8B8A8_UNORM) { d.format = DXGI_FORMAT_R8G8B8A8_UNORM; d.pixelBytes = 4; }
            else if (dx == DXGI_FORMAT_B8G8R8A8_UNORM) { d.format = DXGI_FORMAT_B8G8R8A8_UNORM; d.pixelBytes = 4; }
            else return false;
            break;
        }
        default: return false;
        }
    } else if (bits == 32) {
        d.format = rMask == 0x00FF0000 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.pixelBytes = 4;
    } else if (bits == 8) { // luminance / single channel
        d.format = DXGI_FORMAT_R8_UNORM;
        d.pixelBytes = 1;
    } else {
        return false;
    }
    return d.width && d.height;
}

// Whole DDS file (all mip levels) into an immutable texture.
bool LoadDdsFile(ID3D11Device* dev, const char* path, ComPtr<ID3D11ShaderResourceView>& srv, DdsInfo& d)
{
    std::vector<BYTE> file;
    if (FILE* f = _fsopen(path, "rb", _SH_DENYNO)) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) { file.resize((size_t)size); if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear(); }
        fclose(f);
    }
    if (!ParseDds(file, d)) { LogOnce(std::string("dds-") + path, "unsupported or unreadable DDS %s", path); return false; }
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    size_t offset = d.dataOffset;
    UINT mips = 0;
    for (UINT m = 0; m < d.mips; m++) {
        const UINT w = MipSize(d.width, m), h = MipSize(d.height, m);
        const UINT pitch = d.blockBytes ? ((w + 3) / 4) * d.blockBytes : w * d.pixelBytes;
        const UINT rows = d.blockBytes ? (h + 3) / 4 : h;
        if (offset + (size_t)pitch * rows > file.size()) break;
        init.push_back({ file.data() + offset, pitch, 0 });
        offset += (size_t)pitch * rows;
        mips++;
    }
    if (!mips) return false;
    d.mips = mips;
    D3D11_TEXTURE2D_DESC td = { d.width, d.height, mips, 1, d.format, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &t)) || FAILED(dev->CreateShaderResourceView(t.Get(), nullptr, &srv))) {
        Log("texture creation failed for %s", path);
        return false;
    }
    return true;
}

Texture* GameTexture(void* texture)
{
    if (!texture || g_config.useDgVoodoo) return nullptr;
    ID8BaseTexture* base = (ID8BaseTexture*)texture;
    if (base->GetType() != D8RTYPE_TEXTURE) return nullptr;
    return static_cast<Texture*>(static_cast<ID8Texture*>(base));
}

} // namespace

extern "C" BOOL WINAPI D3D8to11_SetNormalMap(void* texture, const char* path)
{
    Texture* tex = GameTexture(texture);
    if (!tex || !path) return FALSE;
    ComPtr<ID3D11ShaderResourceView> srv;
    DdsInfo d;
    if (!LoadDdsFile(tex->m_dev->D3D(), path, srv, d)) return FALSE;
    const UINT mips = d.mips;
    tex->m_normalSrv = srv;
    std::string identity=path;size_t slash=identity.find_last_of("/\\");
    if(slash!=std::string::npos)identity=identity.substr(slash+1);
    for(char& c:identity)c=char(std::tolower(static_cast<unsigned char>(c)));
    tex->m_normalName=identity;
    // Dinosaur libraries name materials, not textures: the Triceratops skins are the unnamed
    // textures 5 (adult) and 4 (juvenile); "Tricera" is texture 2 (a Pachycephalosaurus skin).
    tex->m_materialProfile=identity=="dinos_hellcreek_b__tex5_n.dds"||identity=="dinos_hellcreek_b__tex4_n.dds"?1:
        identity=="structures_hi__hamburger_n.dds"?2:0;
    if(tex->m_materialProfile)Log("Material pilot tagged: %s, profile %u (texture-scoped, not object ID)",identity.c_str(),unsigned(tex->m_materialProfile));
    LogOnce("nm-first", "normal maps active (first: %s, %ux%u, %u mips)", path, d.width, d.height, mips);
    return TRUE;
}

extern "C" BOOL WINAPI D3D8to11_SetRoughnessMap(void* texture, const char* path)
{
    Texture* tex = GameTexture(texture);
    if (!tex || !path) return FALSE;
    ComPtr<ID3D11ShaderResourceView> srv;
    DdsInfo d;
    if (!LoadDdsFile(tex->m_dev->D3D(), path, srv, d)) return FALSE;
    tex->m_roughSrv = srv;
    LogOnce(std::string("rough-") + path, "roughness map: %s (%ux%u, %u mips)", path, d.width, d.height, d.mips);
    return TRUE;
}

// Diagnostics: the plugin reports which game texture (<library>__<texture>, "+" when replaced
// by a mod file) each created texture holds, shown by the panel's skin finder and frame dump.
extern "C" BOOL WINAPI D3D8to11_SetTextureName(void* texture, const char* name)
{
    if (!texture || !name || g_config.useDgVoodoo) return FALSE;
    ID8BaseTexture* base = (ID8BaseTexture*)texture;
    if (base->GetType() != D8RTYPE_TEXTURE) return FALSE;
    Texture* tex = static_cast<Texture*>(static_cast<ID8Texture*>(base));
    tex->m_name = name;
    DecorateTexture(tex);
    return TRUE;
}

// ---- texture names from the .tml libraries, night emission masks ----------------------------
//
// Emission masks are mods\emissive\<texture>.dds (32-bit or BC1-3, the texture's own layout,
// name without the library: Set_17.dds serves Fences__Set_17 and Fences_Hi__Set_17), with an
// optional mods\emissive\emissive.ini: [Emissive] <texture>=intensity[,r,g,b].
namespace {
std::unordered_map<unsigned long long, std::string> g_tmlNames;
bool g_tmlIndexed = false;

void IndexTmlNames()
{
    g_tmlIndexed = true;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_gameDir + "Data\\matlibs\\*.tml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::vector<BYTE> tml;
        if (FILE* f = _fsopen((g_gameDir + "Data\\matlibs\\" + fd.cFileName).c_str(), "rb", _SH_DENYNO)) {
            fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
            if (size > 0) { tml.resize((size_t)size); if (fread(tml.data(), 1, tml.size(), f) != tml.size()) tml.clear(); }
            fclose(f);
        }
        if (tml.size() < 16 || memcmp(tml.data(), "TML1", 4) != 0) continue;
        std::string lib = fd.cFileName;
        lib = lib.substr(0, lib.size() - 4);
        const UINT count = *(const UINT*)(tml.data() + 8);
        size_t o = 0xc;
        std::vector<std::pair<UINT, std::pair<size_t, UINT>>> ents;
        bool ok = true;
        for (UINT i = 0; i < count; i++) {
            if (o + 28 > tml.size()) { ok = false; break; }
            const UINT idx = *(const UINT*)(tml.data() + o), size = *(const UINT*)(tml.data() + o + 4);
            if (o + 28 + size > tml.size()) { ok = false; break; }
            ents.push_back({ idx, { o + 28, size } });
            o += 28 + size;
        }
        if (!ok || o + 4 > tml.size()) continue;
        const UINT nnames = *(const UINT*)(tml.data() + o);
        o += 4;
        for (const auto& e : ents) {
            char name[33] = {};
            if (e.first < nnames && o + 32 * (e.first + 1) <= tml.size()) memcpy(name, tml.data() + o + 32 * e.first, 32);
            else sprintf_s(name, "tex%u", e.first);
            unsigned long long key = 1469598103934665603ull ^ (unsigned long long)e.second.second;
            const BYTE* p = tml.data() + e.second.first;
            for (UINT i = 0; i < e.second.second; i++) { key ^= p[i]; key *= 1099511628211ull; }
            if (!g_tmlNames.count(key)) g_tmlNames[key] = lib + "__" + name;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    Log("Texture names: %u .tml entries indexed", (UINT)g_tmlNames.size());
}

struct EmissiveEntry {
    ComPtr<ID3D11ShaderResourceView> srv; float tint[3] = { 1, 1, 1 }; float intensity = 1; bool tried = false;
    std::vector<float> mask; int maskSize = 0; // 64x64 rgb (tinted) for the point lights (lights.cpp)
};
std::unordered_map<std::string, EmissiveEntry> g_emissive; // lowercase texture name without library
bool g_emissiveScanned = false;

std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

void ScanEmissive()
{
    g_emissiveScanned = true;
    const std::string dir = g_gameDir + "mods\\emissive\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.dds").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string ini = dir + "emissive.ini";
    do {
        std::string stem = fd.cFileName;
        stem = stem.substr(0, stem.size() - 4);
        EmissiveEntry e;
        char v[96] = {};
        GetPrivateProfileStringA("Emissive", stem.c_str(), "", v, sizeof(v), ini.c_str());
        float k = 1, r = 1, g = 1, b = 1;
        const int n = sscanf_s(v, "%f , %f , %f , %f", &k, &r, &g, &b);
        if (n >= 1) e.intensity = k;
        if (n == 4) { e.tint[0] = r; e.tint[1] = g; e.tint[2] = b; }
        g_emissive[Lower(stem)] = e;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    Log("Emission masks: %u in mods\\emissive", (UINT)g_emissive.size());
}

ID3D11ShaderResourceView* LoadEmissive(ID3D11Device* dev, const std::string& stem, EmissiveEntry& e)
{
    if (e.tried) return e.srv.Get();
    e.tried = true;
    std::vector<BYTE> file;
    WIN32_FIND_DATAA fd;
    std::string path = g_gameDir + "mods\\emissive\\" + stem + ".dds";
    HANDLE h = FindFirstFileA(path.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { path = g_gameDir + "mods\\emissive\\" + fd.cFileName; FindClose(h); }
    if (FILE* f = _fsopen(path.c_str(), "rb", _SH_DENYNO)) {
        fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
        if (size > 0) { file.resize((size_t)size); if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear(); }
        fclose(f);
    }
    DdsInfo d;
    if (!ParseDds(file, d)) { Log("emission mask: unsupported or unreadable DDS %s", path.c_str()); return nullptr; }
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    size_t offset = d.dataOffset;
    UINT mips = 0;
    for (UINT m = 0; m < d.mips; m++) {
        const UINT w = MipSize(d.width, m), hh = MipSize(d.height, m);
        const UINT pitch = d.blockBytes ? ((w + 3) / 4) * d.blockBytes : w * d.pixelBytes;
        const UINT rows = d.blockBytes ? (hh + 3) / 4 : hh;
        if (offset + (size_t)pitch * rows > file.size()) break;
        init.push_back({ file.data() + offset, pitch, 0 });
        offset += (size_t)pitch * rows;
        mips++;
    }
    if (!mips) return nullptr;
    if (d.pixelBytes == 4) { // box-filtered 64x64 copy of level 0
        const int n = 64;
        e.maskSize = n;
        e.mask.assign((size_t)n * n * 3, 0.0f);
        const bool bgra = d.format == DXGI_FORMAT_B8G8R8A8_UNORM;
        for (UINT y = 0; y < d.height; y++)
            for (UINT x = 0; x < d.width; x++) {
                const BYTE* p = file.data() + d.dataOffset + ((size_t)y * d.width + x) * 4;
                float* o = &e.mask[(((size_t)y * n / d.height) * n + (size_t)x * n / d.width) * 3];
                const float r = (bgra ? p[2] : p[0]) / 255.0f, g = p[1] / 255.0f, b = (bgra ? p[0] : p[2]) / 255.0f;
                o[0] += r; o[1] += g; o[2] += b;
            }
        const float scale = (float)n * n / ((float)d.width * d.height) * e.intensity;
        for (size_t i = 0; i < e.mask.size(); i++) e.mask[i] *= scale * e.tint[i % 3];
    }
    D3D11_TEXTURE2D_DESC td = { d.width, d.height, mips, 1, d.format, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &t)) || FAILED(dev->CreateShaderResourceView(t.Get(), nullptr, &e.srv))) {
        Log("emission mask: texture creation failed for %s", path.c_str());
        return nullptr;
    }
    Log("Emission mask loaded: %s (%ux%u, intensity %.2f)", path.c_str(), d.width, d.height, e.intensity);
    return e.srv.Get();
}
} // namespace

void DecorateTexture(Texture* t)
{
    if (!t || g_config.useDgVoodoo) return;
    if (t->m_name.empty() && t->m_hashValid) {
        if (!g_tmlIndexed) IndexTmlNames();
        auto it = g_tmlNames.find(t->m_contentHash);
        if (it != g_tmlNames.end()) t->m_name = it->second;
    }
    if (t->m_name.empty() || t->m_emissiveSrv) return;
    if (!g_emissiveScanned) ScanEmissive();
    if (g_emissive.empty()) return;
    std::string base = t->m_name;
    const size_t sep = base.find("__");
    if (sep != std::string::npos) base = base.substr(sep + 2);
    const size_t space = base.find(' ');
    if (space != std::string::npos) base = base.substr(0, space); // " +mod" / " _mod" suffixes
    auto it = g_emissive.find(Lower(base));
    if (it == g_emissive.end()) return;
    if (ID3D11ShaderResourceView* srv = LoadEmissive(t->m_dev->D3D(), base, it->second)) {
        t->m_emissiveSrv = srv;
        for (int k = 0; k < 3; k++) t->m_emissive[k] = it->second.tint[k];
        t->m_emissive[3] = it->second.intensity;
        if (!it->second.mask.empty()) { t->m_emissiveMask = it->second.mask.data(); t->m_emissiveMaskSize = it->second.maskSize; }
        Log("Emission mask attached to %s", t->m_name.c_str());
    }
}
