// automation.cpp - hooks for tuning from outside the game: d3d8to11.ini is reloaded when it
// changes on disk, and creating d3d8to11_capture.req in the game folder writes the next frame
// (without the Insert panel) to d3d8to11_capture.bmp and deletes the request.
#include "device.h"

namespace {
bool FileTime(const std::string& path, FILETIME& t)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return false;
    t = a.ftLastWriteTime;
    return true;
}
}

void Device::PollAutomation()
{
    if (m_texDumpActive && m_frame > m_texDumpFrame + 30) WriteTextureDump(); // 30 frames: draws can be spread out
    if (m_frame % 15) return;
    const std::string ini = g_gameDir + "d3d8to11.ini";
    FILETIME t;
    if (FileTime(ini, t)) {
        if (m_iniTimeValid && CompareFileTime(&t, &m_iniTime) != 0) {
            LoadConfig();
            Log("d3d8to11.ini changed on disk: reloaded");
        }
        m_iniTime = t; m_iniTimeValid = true;
    }
    const std::string dumpReq = g_gameDir + "d3d8to11_texdump.req";
    if (!m_texDumpActive && GetFileAttributesA(dumpReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(dumpReq.c_str());
        m_texDumpActive = true;
        m_texDumpFrame = m_frame + 1;
    }
    const std::string req = g_gameDir + "d3d8to11_capture.req";
    if (GetFileAttributesA(req.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(req.c_str());
    if (!CaptureSwapChain(g_gameDir + "d3d8to11_capture.bmp")) Log("Capture failed");
}

void Device::CollectDumpTextures()
{
    if (!m_texDumpActive || m_frame < m_texDumpFrame || m_sun.flags[1] > .5f) return;
    for (int stage = 0; stage < 2; stage++) {
        if (!m_textures[stage] || m_textures[stage]->GetType() != D8RTYPE_TEXTURE) continue;
        Texture* t = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[stage]));
        if (t->m_name.empty() && !t->m_hashValid) continue;
        bool seen = false;
        for (auto& e : m_texDumpList) if (e.first == t) { seen = true; break; }
        if (!seen) { t->AddRef(); m_texDumpList.push_back({ t, m_worldDraw ? m_worldKind : -1 - (int)(m_vsHandle == 0) }); }
    }
}

namespace {
// Mip 0 as a DDS file: BC1-3 as DXT1/3/5, B8G8R8A8 uncompressed; other formats are skipped.
bool WriteDds(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    DWORD fourCC = 0;
    bool bgra = false;
    switch (d.Format) {
    case DXGI_FORMAT_BC1_UNORM: fourCC = MAKEFOURCC('D', 'X', 'T', '1'); break;
    case DXGI_FORMAT_BC2_UNORM: fourCC = MAKEFOURCC('D', 'X', 'T', '3'); break;
    case DXGI_FORMAT_BC3_UNORM: fourCC = MAKEFOURCC('D', 'X', 'T', '5'); break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: bgra = true; break;
    default: return false;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const UINT rowBytes = DxgiRowPitch(d.Format, d.Width), rows = DxgiRowCount(d.Format, d.Height);
    FILE* f = nullptr;
    bool ok = fopen_s(&f, path.c_str(), "wb") == 0 && f;
    if (ok) {
        DWORD h[32] = {};
        h[0] = MAKEFOURCC('D', 'D', 'S', ' ');
        h[1] = 124; h[2] = 0x1007 | (fourCC ? 0x80000 : 0x8); h[3] = d.Height; h[4] = d.Width;
        h[5] = fourCC ? rowBytes * rows : rowBytes;
        h[19] = 32; // pixel format
        if (fourCC) { h[20] = 0x4; h[21] = fourCC; }
        else { h[20] = 0x41; h[22] = 32; h[23] = 0x00FF0000; h[24] = 0x0000FF00; h[25] = 0x000000FF; h[26] = 0xFF000000; }
        h[27] = 0x1000;
        fwrite(h, 4, 32, f);
        for (UINT y = 0; y < rows; y++) fwrite((const BYTE*)map.pData + (size_t)y * map.RowPitch, 1, rowBytes, f);
        fclose(f);
    }
    ctx->Unmap(staging.Get(), 0);
    return ok;
}
}

void Device::WriteTextureDump()
{
    m_texDumpActive = false;
    const std::string dir = g_gameDir + "d3d8to11_textures\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    FILE* list = nullptr;
    fopen_s(&list, (dir + "textures.txt").c_str(), "w");
    UINT written = 0;
    for (auto& e : m_texDumpList) {
        Texture* t = e.first;
        char hash[24];
        sprintf_s(hash, "h%016llx", t->m_contentHash);
        std::string file = t->m_name.empty() ? std::string(hash) : t->m_name;
        for (char& c : file) if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '+') c = '_';
        const bool ok = t->m_tex && WriteDds(m_device.Get(), m_ctx.Get(), t->m_tex.Get(), dir + file + ".dds");
        if (ok) written++;
        if (list) fprintf(list, "%s\t%s\tkind %d\t%ux%u\t%s\n", file.c_str(), hash, e.second, t->m_width, t->m_height, ok ? "ok" : "skipped (format)");
        t->Release();
    }
    if (list) fclose(list);
    Log("Texture dump: %u of %u world textures written to %s", written, (UINT)m_texDumpList.size(), dir.c_str());
    m_texDumpList.clear();
}

bool Device::CaptureSwapChain(const std::string& path)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.SampleDesc = { 1, 0 }; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &staging))) return false;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = sd; rd.Usage = D3D11_USAGE_DEFAULT; rd.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> resolved;
        if (FAILED(m_device->CreateTexture2D(&rd, nullptr, &resolved))) return false;
        m_ctx->ResolveSubresource(resolved.Get(), 0, back.Get(), 0, d.Format);
        m_ctx->CopyResource(staging.Get(), resolved.Get());
    } else {
        m_ctx->CopyResource(staging.Get(), back.Get());
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    FILE* f = nullptr;
    bool ok = fopen_s(&f, path.c_str(), "wb") == 0 && f;
    if (ok) {
        const UINT w = d.Width, h = d.Height, row = w * 4;
        BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + row * h;
        ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        std::vector<BYTE> line(row);
        for (UINT y = 0; y < h; y++) {
            const BYTE* src = (const BYTE*)map.pData + (size_t)y * map.RowPitch;
            memcpy(line.data(), src, row);
            if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT x = 0; x < w; x++) std::swap(line[x * 4], line[x * 4 + 2]);
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }
    m_ctx->Unmap(staging.Get(), 0);
    if (ok) Log("Captured %ux%u frame to %s", d.Width, d.Height, path.c_str());
    return ok;
}
