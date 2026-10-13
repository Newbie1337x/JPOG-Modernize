// normalmap.cpp - normal maps for game textures.
//
// The texture-replacement plugin (JPOGMouseFix.asi) knows which game texture is which: for a
// texture with a "<library>__<texture>_n.dds" file in mods\textures it calls
// D3D8to11_SetNormalMap(texture, path) right after the game creates the texture. The DDS is
// loaded here (BC1/BC2/BC3/BC5, DX10 BC5, 32-bit uncompressed; all mip levels) and kept on the
// Texture; world draws that sample it on stage 0 get the relief term (shaders.cpp NormalMapCode).
#include "device.h"
#include <stdio.h>

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
        case MAKEFOURCC('A', 'T', 'I', '2'): case MAKEFOURCC('B', 'C', '5', 'U'):
            d.format = DXGI_FORMAT_BC5_UNORM; d.blockBytes = 16; break;
        case MAKEFOURCC('D', 'X', '1', '0'): {
            if (f.size() < 148) return false;
            const UINT dx = u32(128);
            d.dataOffset = 148;
            if (dx == DXGI_FORMAT_BC5_UNORM || dx == DXGI_FORMAT_BC5_TYPELESS) { d.format = DXGI_FORMAT_BC5_UNORM; d.blockBytes = 16; }
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
    } else {
        return false;
    }
    return d.width && d.height;
}

} // namespace

extern "C" BOOL WINAPI D3D8to11_SetNormalMap(void* texture, const char* path)
{
    if (!texture || !path || g_config.useDgVoodoo) return FALSE;
    ID8BaseTexture* base = (ID8BaseTexture*)texture;
    if (base->GetType() != D8RTYPE_TEXTURE) return FALSE;
    Texture* tex = static_cast<Texture*>(static_cast<ID8Texture*>(base));
    std::vector<BYTE> file;
    if (FILE* f = _fsopen(path, "rb", _SH_DENYNO)) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) { file.resize((size_t)size); if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear(); }
        fclose(f);
    }
    DdsInfo d;
    if (!ParseDds(file, d)) { LogOnce(std::string("nm-") + path, "normal map: unsupported or unreadable DDS %s", path); return FALSE; }
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
    if (!mips) return FALSE;
    D3D11_TEXTURE2D_DESC td = { d.width, d.height, mips, 1, d.format, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(tex->m_dev->D3D()->CreateTexture2D(&td, init.data(), &t)) ||
        FAILED(tex->m_dev->D3D()->CreateShaderResourceView(t.Get(), nullptr, &srv))) {
        Log("normal map: texture creation failed for %s", path);
        return FALSE;
    }
    tex->m_normalSrv = srv;
    LogOnce("nm-first", "normal maps active (first: %s, %ux%u, %u mips)", path, d.width, d.height, mips);
    return TRUE;
}
