// lights.cpp - night point lights from the emission masks (lamps, signs, kiosks light their surroundings).
//
// A world draw whose stage-0 texture has an emission mask (normalmap.cpp, mods\emissive) is an
// emitter. Its triangles are read once from the CPU copies of the vertex/index buffers: the mask
// is sampled at each triangle's texture centroid and the glowing ones are merged in object space
// into a few emitters (one per lamp, cached per draw). Every frame those are moved to the world
// with the draw's bone palette (as CapturePusher) and kept in a world grid that remembers lights
// whose objects went off screen for a while, so the light they cast on visible ground stays.
// At the end of the frame the grid is merged more coarsely with distance and the nearest
// lights (faded out toward the cut) light the next frame (cbuffer b5, shaders.cpp PointLights).
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

const UINT kLightMemoryFrames = 900; // a light whose object is not drawn stays this long
}

void Device::CaptureEmitters(DWORD type, UINT baseVertex, UINT startIndex, UINT primCount, bool indexed)
{
    if (!g_config.effectsEnabled || !g_config.pointLights || !g_config.replaceLighting || m_worldKind != 1 || !ActiveEmissive()) return;
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

    // Glowing parts of this draw, found once and merged per lamp in object space.
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
        const float cell = (std::max)(0.25f, g_config.pointLightRadius * 0.12f);
        UINT glowing = 0;
        const UINT tris = (std::min)(primCount, 16384u);
        for (UINT t = 0; t < tris; t++) {
            UINT v[3];
            bool ok = true;
            for (int c = 0; c < 3 && ok; c++) {
                const UINT n = type == D8PT_TRIANGLELIST ? t * 3 + c : t + c;
                ok = index(n, v[c]) && (size_t)(v[c] + 1) * stride <= data.size();
            }
            if (!ok || v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
            float u = 0, w = 0, p[3] = {};
            for (int c = 0; c < 3; c++) {
                const BYTE* vx = data.data() + (size_t)v[c] * stride;
                float t2[2], q[3];
                memcpy(t2, vx + uv->offset, 8);
                memcpy(q, vx + pos->offset, 12);
                u += t2[0] / 3; w += t2[1] / 3;
                for (int k = 0; k < 3; k++) p[k] += q[k] / 3;
            }
            float rgb[3];
            SampleMask(tex, u, w, rgb);
            const float bright = (std::max)(rgb[0], (std::max)(rgb[1], rgb[2]));
            if (bright <= 0.08f || !std::isfinite(p[0] + p[1] + p[2])) continue;
            glowing++;
            // Same lamp: within one cell of an emitter already found.
            LightEmitter* into = nullptr;
            for (LightEmitter& e : list) {
                const float dx = e.local[0] / e.weight - p[0], dy = e.local[1] / e.weight - p[1], dz = e.local[2] / e.weight - p[2];
                if (dx * dx + dy * dy + dz * dz < cell * cell) { into = &e; break; }
            }
            if (!into) {
                if (list.size() >= 256) continue;
                list.push_back({});
                into = &list.back();
                const BYTE* vx = data.data() + (size_t)v[0] * stride;
                memcpy(into->weights, vx + weights->offset, 4);
                memcpy(into->indices, vx + indices->offset, 4);
            }
            for (int k = 0; k < 3; k++) { into->local[k] += p[k] * bright; into->rgb[k] += rgb[k]; }
            into->weight += bright;
            into->count++;
        }
        for (LightEmitter& e : list) {
            for (int k = 0; k < 3; k++) { e.local[k] /= e.weight; e.rgb[k] /= e.count; }
        }
        found = m_emitterCache.emplace(key, std::move(list)).first;
        if (!found->second.empty())
            LogOnce("emitters-" + tex->m_name + std::to_string(primCount), "Point lights: %u glowing triangles, %u lamps in a draw of %s (%u triangles)",
                    glowing, (UINT)found->second.size(), tex->m_name.c_str(), primCount);
    }
    if (found->second.empty()) return;

    // To the world: object space -> bone palette (c5+, indices scaled by c4) -> clip (c0-c3) -> world.
    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    const XMMATRIX model = Rows(c0) * Rows(m_sun.clipToWorld);
    const float cell = (std::max)(0.25f, g_config.pointLightRadius * 0.12f);
    for (const LightEmitter& e : found->second) {
        const float wv[4] = { e.weights[2] / 255.0f, e.weights[1] / 255.0f, e.weights[0] / 255.0f, e.weights[3] / 255.0f };
        const float iv[4] = { e.indices[2] / 255.0f, e.indices[1] / 255.0f, e.indices[0] / 255.0f, e.indices[3] / 255.0f };
        float o[3] = {};
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            for (int r = 0; r < 3; r++) {
                const float* row = m_vsConst[5 + a0 + r];
                o[r] += wv[j] * (row[0] * e.local[0] + row[1] * e.local[1] + row[2] * e.local[2] + row[3]);
            }
        }
        XMFLOAT3 f;
        XMStoreFloat3(&f, XMVector3TransformCoord(XMVectorSet(o[0], o[1], o[2], 1), model));
        if (!std::isfinite(f.x + f.y + f.z)) continue;
        const long long x = (long long)std::floor(f.x / cell), y = (long long)std::floor(f.y / cell), z = (long long)std::floor(f.z / cell);
        const unsigned long long k = ((unsigned long long)(x & 0x1FFFFF) << 42) | ((unsigned long long)(y & 0x1FFFFF) << 21) | (unsigned long long)(z & 0x1FFFFF);
        WorldLight& l = m_worldLights[k];
        if (l.lastSeen != (UINT)(m_frame + 1)) { // first sighting this frame: replace
            l = {};
            l.lastSeen = (UINT)(m_frame + 1);
        }
        const float n = (float)l.count;
        l.pos[0] = (l.pos[0] * n + f.x) / (n + 1); l.pos[1] = (l.pos[1] * n + f.y) / (n + 1); l.pos[2] = (l.pos[2] * n + f.z) / (n + 1);
        const float area = (std::min)(2.0f, 0.6f + 0.2f * std::sqrt((float)e.count));
        for (int c = 0; c < 3; c++) l.rgb[c] += e.rgb[c] * area;
        l.count++;
    }
}

// End of frame: remembered lights merged more coarsely with distance; the nearest light the next frame.
void Device::BuildPointLights()
{
    m_pointLights = {};
    m_pointLightsShown = 0;
    if (!g_config.effectsEnabled || !g_config.pointLights || !m_tod.active) { m_worldLights.clear(); return; }
    if (m_emitterCache.size() > 8192) m_emitterCache.clear();
    const UINT now = (UINT)(m_frame + 1);
    for (auto it = m_worldLights.begin(); it != m_worldLights.end();)
        it = now - it->second.lastSeen > kLightMemoryFrames ? m_worldLights.erase(it) : std::next(it);
    m_pointLightTris = (UINT)m_worldLights.size();
    const float night = 1 - m_tod.dayW;
    if (night <= 0.01f || m_worldLights.empty()) return;

    struct Candidate { float pos[3]; float rgb[3]; float dist; };
    std::vector<Candidate> all;
    all.reserve(m_worldLights.size());
    const float* cam = m_sun.camera;
    for (const auto& w : m_worldLights) {
        Candidate c;
        memcpy(c.pos, w.second.pos, sizeof(c.pos));
        memcpy(c.rgb, w.second.rgb, sizeof(c.rgb));
        const float dx = c.pos[0] - cam[0], dy = c.pos[1] - cam[1], dz = c.pos[2] - cam[2];
        c.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        all.push_back(c);
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });

    // Greedy: each light joins a chosen one closer than a distance-scaled merge range.
    const float radius = g_config.pointLightRadius;
    struct Chosen { float pos[3]; float rgb[3]; float w; float dist; };
    std::vector<Chosen> chosen;
    float cutoff = 1e30f;
    for (const Candidate& c : all) {
        const float merge = (std::max)(radius * 0.3f, c.dist * 0.08f);
        Chosen* into = nullptr;
        for (Chosen& s : chosen) {
            const float dx = s.pos[0] / s.w - c.pos[0], dy = s.pos[1] / s.w - c.pos[1], dz = s.pos[2] / s.w - c.pos[2];
            if (dx * dx + dy * dy + dz * dz < merge * merge) { into = &s; break; }
        }
        if (!into) {
            if (chosen.size() >= (size_t)kMaxPointLights) { cutoff = c.dist; break; }
            chosen.push_back({});
            into = &chosen.back();
            into->dist = c.dist;
        }
        const float lum = c.rgb[0] * 0.2126f + c.rgb[1] * 0.7152f + c.rgb[2] * 0.0722f;
        for (int k = 0; k < 3; k++) { into->pos[k] += c.pos[k] * lum; into->rgb[k] += c.rgb[k]; }
        into->w += lum;
    }

    // Colour: the mask's own colour pulled toward the warm lamp tint; faded toward the cut.
    const float warm = g_config.pointLightWarmth;
    for (size_t i = 0; i < chosen.size(); i++) {
        Chosen& s = chosen[i];
        const float fade = cutoff < 1e29f ? (std::min)(1.0f, (std::max)(0.0f, (cutoff - s.dist) / (cutoff * 0.25f))) : 1.0f;
        const float lum = s.rgb[0] * 0.2126f + s.rgb[1] * 0.7152f + s.rgb[2] * 0.0722f;
        for (int k = 0; k < 3; k++) {
            m_pointLights.pos[i][k] = s.pos[k] / (std::max)(s.w, 1e-6f);
            const float c = s.rgb[k] + (lum * g_config.pointLightTint[k] - s.rgb[k]) * warm;
            m_pointLights.color[i][k] = c * g_config.pointLightIntensity * night * fade;
        }
        m_pointLights.pos[i][3] = radius;
    }
    m_pointLights.count[0] = (float)chosen.size();
    m_pointLights.count[1] = 1.0f / (radius * radius * 0.08f); // inverse-square knee (about a third of the radius)
    m_pointLightsShown = (UINT)chosen.size();
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
    BindPSCB(5, 1, m_pointLightCB.GetAddressOf());
}
