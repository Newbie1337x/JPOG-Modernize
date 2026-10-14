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
    const std::string req = g_gameDir + "d3d8to11_capture.req";
    if (GetFileAttributesA(req.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(req.c_str());
    if (!CaptureSwapChain(g_gameDir + "d3d8to11_capture.bmp")) Log("Capture failed");
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
