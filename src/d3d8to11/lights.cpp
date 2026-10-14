// lights.cpp - night point lights from the emission masks (lamps, signs, kiosks light their surroundings).
//
// A world draw whose stage-0 texture has an emission mask (normalmap.cpp, mods\emissive) is an
// emitter. Its triangles are read once from the CPU copies of the vertex/index buffers: the mask
// is sampled at each triangle's texture centroid and the glowing ones are kept (cached per draw).
// Every frame they are moved to the world with the draw's bone palette (as CapturePusher), merged
// into clusters on a grid and the clusters nearest the camera become point lights for the next
// frame (cbuffer b5, shaders.cpp PointLights), added by the replacement lighting at night.
#include "device.h"
#include <algorithm>
#include <DirectXMath.h>
using namespace DirectX;

namespace {
// Dot-product rows as a DirectXMath row-vector matrix (as sun.cpp).
XMMATRIX Rows(const float (&rows)[4][4])
{
    XMFLOAT4X4 m;
    memcpy(&m, rows, sizeof(m));
    return XMMatrixTranspose(XMLoadFloat4x4(&m));
}



// Mask texel (rgb, already tinted) at uv, wrapped.
void SampleMask(const Texture* t, float u, float v, float out[3])
{
    const int n = t->m_emissiveMaskSize;
    u -= std::floor(u); v -= std::floor(v);
    const int x = (std::min)(n - 1, (int)(u * n)), y = (std::min)(n - 1, (int)(v * n));
    const float* p = t->m_emissiveMask + ((size_t)y * n + x) * 3;
    for (int k = 0; k < 3; k++) out[k] = p[k];
}
}

void Device::CaptureEmitters(DWORD type, UINT baseVertex, UINT startIndex, UINT primCount, bool indexed)
{
    if (!g_config.pointLights || !g_config.replaceLighting || m_worldKind != 1 || !ActiveEmissive() || m_lightsThisFrame.size() > 4096) return;
    Texture* tex = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]));
    if (!tex->m_emissiveMask) return;
    const size_t idx = m_vsHandle >> 1;
    if (idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return;
    const VSDecl& decl = m_vertexShaders[idx]->parsed;
    const VSInput *pos = nullptr, *weights = nullptr, *indices = nullptr, *uv = nullptr;
    for (const VSInput& in : decl.inputs) {
        if (in.stream != 0) continue;
        if (in.reg == 0 && in.type == 2) pos = &in;
        if (in.reg == 2 && in.type == 4) weights = &in;
        if (in.reg == 3 && in.type == 4) indices = &in;
        if (in.type == 1 && !uv) uv = &in;
    }
    VertexBuffer* vb = m_streams[0].vb;
    const UINT stride = m_streams[0].stride;
    if (!pos || !weights || !indices || !uv || !vb || !stride || vb->m_impl.m_dynamic) return;
    if (type != D8PT_TRIANGLELIST && type != D8PT_TRIANGLESTRIP) return;
    const std::vector<BYTE>& data = vb->m_impl.m_shadow;
    const IndexBuffer* ib = indexed ? m_indices : nullptr;
    if (indexed && (!ib || ib->m_impl.m_dynamic)) return;

    // Glowing triangles of this draw, found once.
    const unsigned long long key = ((unsigned long long)(uintptr_t)vb * 1099511628211ull) ^ ((unsigned long long)(uintptr_t)ib << 7) ^
        ((unsigned long long)(uintptr_t)tex << 21) ^ ((unsigned long long)baseVertex << 40) ^ ((unsigned long long)startIndex << 13) ^ primCount ^ (type << 30);
    auto found = m_emitterCache.find(key);
    if (found == m_emitterCache.end()) {
        std::vector<LightEmitter> list;
        const UINT isz = ib && ib->m_format == D8FMT_INDEX32 ? 4 : 2;
        auto index = [&](UINT n, UINT& out) {
            if (!ib) { out = baseVertex + startIndex + n; return true; }
            const size_t at = ((size_t)startIndex + n) * isz;
            if (at + isz > ib->m_impl.m_shadow.size()) return false;
            const BYTE* p = ib->m_impl.m_shadow.data() + at;
            out = baseVertex + (isz == 4 ? *(const UINT*)p : *(const WORD*)p);
            return true;
        };
        const UINT tris = (std::min)(primCount, 4096u);
        for (UINT t = 0; t < tris; t++) {
            LightEmitter e = {};
            bool ok = true;
            for (int c = 0; c < 3 && ok; c++) {
                const UINT n = type == D8PT_TRIANGLELIST ? t * 3 + c : t + c;
                ok = index(n, e.v[c]) && (size_t)(e.v[c] + 1) * stride <= data.size();
            }
            if (!ok || e.v[0] == e.v[1] || e.v[1] == e.v[2] || e.v[0] == e.v[2]) continue;
            float u = 0, v = 0;
            for (int c = 0; c < 3; c++) {
                float t2[2];
                memcpy(t2, data.data() + (size_t)e.v[c] * stride + uv->offset, 8);
                u += t2[0] / 3; v += t2[1] / 3;
            }
            SampleMask(tex, u, v, e.rgb);
            e.w = (std::max)(e.rgb[0], (std::max)(e.rgb[1], e.rgb[2]));
            if (e.w > 0.08f) list.push_back(e);
        }
        if (list.size() > 512) { // keep the brightest
            std::nth_element(list.begin(), list.begin() + 512, list.end(), [](const LightEmitter& a, const LightEmitter& b) { return a.w > b.w; });
            list.resize(512);
        }
        found = m_emitterCache.emplace(key, std::move(list)).first;
        if (!found->second.empty())
            LogOnce("emitters-" + tex->m_name + std::to_string(primCount), "Point lights: %u glowing triangles in a draw of %s (%u triangles)",
                    (UINT)found->second.size(), tex->m_name.c_str(), primCount);
    }
    if (found->second.empty()) return;

    // To the world: object space -> bone palette (c5+, indices scaled by c4) -> clip (c0-c3) -> world.
    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    const XMMATRIX model = Rows(c0) * Rows(m_sun.clipToWorld);
    auto world = [&](UINT vi, float out[3]) {
        const BYTE* v = data.data() + (size_t)vi * stride;
        float p[3];
        memcpy(p, v + pos->offset, 12);
        const BYTE* w = v + weights->offset, *ix = v + indices->offset;
        const float wv[4] = { w[2] / 255.0f, w[1] / 255.0f, w[0] / 255.0f, w[3] / 255.0f };
        const float iv[4] = { ix[2] / 255.0f, ix[1] / 255.0f, ix[0] / 255.0f, ix[3] / 255.0f };
        float o[3] = {};
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            for (int r = 0; r < 3; r++) {
                const float* row = m_vsConst[5 + a0 + r];
                o[r] += wv[j] * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3]);
            }
        }
        XMFLOAT3 f;
        XMStoreFloat3(&f, XMVector3TransformCoord(XMVectorSet(o[0], o[1], o[2], 1), model));
        out[0] = f.x; out[1] = f.y; out[2] = f.z;
        return std::isfinite(f.x + f.y + f.z);
    };
    for (const LightEmitter& e : found->second) {
        FrameLight l = {};
        bool ok = true;
        for (int c = 0; c < 3 && ok; c++) {
            float p[3];
            ok = world(e.v[c], p);
            for (int k = 0; k < 3; k++) l.pos[k] += p[k] / 3;
        }
        if (!ok) continue;
        for (int k = 0; k < 3; k++) l.rgb[k] = e.rgb[k];
        l.w = e.w;
        m_lightsThisFrame.push_back(l);
    }
}

// End of frame: emitting triangles merged into clusters; the nearest become next frame's lights.
void Device::BuildPointLights()
{
    m_pointLights = {};
    std::vector<FrameLight> tris;
    tris.swap(m_lightsThisFrame);
    if (m_emitterCache.size() > 8192) m_emitterCache.clear();
    if (tris.empty() || !g_config.pointLights || !m_tod.active) return;
    const float night = 1 - m_tod.dayW;
    if (night <= 0.01f) return;
    const float radius = g_config.pointLightRadius, cell = (std::max)(0.25f, radius * 0.35f);
    struct Cluster { double p[3]; double rgb[3]; double w; UINT n; };
    std::unordered_map<unsigned long long, Cluster> grid;
    for (const FrameLight& t : tris) {
        const long long x = (long long)std::floor(t.pos[0] / cell), y = (long long)std::floor(t.pos[1] / cell), z = (long long)std::floor(t.pos[2] / cell);
        const unsigned long long key = ((unsigned long long)(x & 0x1FFFFF) << 42) | ((unsigned long long)(y & 0x1FFFFF) << 21) | (unsigned long long)(z & 0x1FFFFF);
        Cluster& c = grid[key];
        for (int k = 0; k < 3; k++) { c.p[k] += t.pos[k] * t.w; c.rgb[k] += t.rgb[k]; }
        c.w += t.w; c.n++;
    }
    struct Light { float pos[3]; float rgb[3]; float dist; };
    std::vector<Light> lights;
    lights.reserve(grid.size());
    const float* cam = m_sun.camera;
    for (const auto& g : grid) {
        const Cluster& c = g.second;
        Light l;
        for (int k = 0; k < 3; k++) l.pos[k] = (float)(c.p[k] / c.w);
        // Average colour of the glowing triangles, a little stronger for larger glowing areas.
        const float area = (std::min)(2.0f, 0.6f + 0.2f * std::sqrt((float)c.n));
        for (int k = 0; k < 3; k++) l.rgb[k] = (float)(c.rgb[k] / c.n) * area * g_config.pointLightIntensity * night;
        const float dx = l.pos[0] - cam[0], dy = l.pos[1] - cam[1], dz = l.pos[2] - cam[2];
        l.dist = dx * dx + dy * dy + dz * dz;
        lights.push_back(l);
    }
    const size_t n = (std::min)(lights.size(), (size_t)kMaxPointLights);
    std::partial_sort(lights.begin(), lights.begin() + n, lights.end(), [](const Light& a, const Light& b) { return a.dist < b.dist; });
    m_pointLights.count[0] = (float)n;
    m_pointLights.count[1] = 1.0f / (radius * radius * 0.08f); // inverse-square knee (about a third of the radius)
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) { m_pointLights.pos[i][k] = lights[i].pos[k]; m_pointLights.color[i][k] = lights[i].rgb[k]; }
        m_pointLights.pos[i][3] = radius;
    }
    m_pointLightsShown = (UINT)n;
    m_pointLightTris = (UINT)tris.size();
}

void Device::BindPointLights()
{
    if (!m_pointLightCB) {
        D3D11_BUFFER_DESC bd = { sizeof(PointLightConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_pointLightCB))) return;
        m_pointLightsDirty = true;
    }
    if (m_pointLightsDirty) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_pointLightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &m_pointLights, sizeof(m_pointLights));
            m_ctx->Unmap(m_pointLightCB.Get(), 0);
        }
        m_pointLightsDirty = false;
    }
    m_ctx->PSSetConstantBuffers(5, 1, m_pointLightCB.GetAddressOf());
}
