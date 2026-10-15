// lights.cpp - night point lights from the emission masks (lamps, signs, kiosks light their surroundings).
//
// A world draw whose stage-0 texture has an emission mask (normalmap.cpp, mods\emissive) is an
// emitter. Its triangles are read once from the CPU copies of the vertex/index buffers: the mask
// is sampled at each triangle's texture centroid and the glowing ones are merged in object space
// into a few emitters (one per lamp, cached per draw). Every frame those are moved to the world
// with the draw's bone palette (as CapturePusher) and kept in a world grid that remembers lights
// whose objects went off screen for a while, so the light they cast on visible ground stays.
// At the end of the frame the nearest lamps, each its own light, light the next frame through
// screen tiles (t17/t18, shaders.cpp PointLights).
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
const float kLampLevel = 1.3f; // brightest channel of every lamp, before PointLightIntensity
}

void Device::CaptureEmitters(DWORD type, UINT baseVertex, UINT startIndex, UINT primCount, bool indexed)
{
    if (!g_config.effectsEnabled || !g_config.pointLights || !g_config.replaceLighting) return;
    // PointLightDebug: why a draw of a lamp texture lights nothing, once per texture and reason.
    auto skip = [&](const char* why) {
        if (g_config.pointLightDebug && m_textures[0] && m_textures[0]->GetType() == D8RTYPE_TEXTURE) {
            const Texture* t = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]));
            if (t->m_emissiveMask)
                LogOnce("lampskip-" + t->m_name + why, "Point lights: %s skipped (%s; kind %d, world %d, sun started %d resolved %d, %u triangles)",
                        t->m_name.c_str(), why, m_worldKind, (int)m_worldDraw, (int)m_sunStarted, (int)m_sunResolved, primCount);
        }
    };
    if (m_worldKind != 1) { skip("not a skinned world draw"); return; }
    if (!ActiveEmissive()) { skip(m_worldDraw ? "no emissive" : "outside the world pass"); return; }
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
    if (!pos || !weights || !indices || !uv) { skip("vertex layout"); return; }
    if (!vb || !stride || vb->m_impl.m_dynamic) { skip("dynamic vertices"); return; }
    if (type != D8PT_TRIANGLELIST && type != D8PT_TRIANGLESTRIP) { skip("primitive type"); return; }
    if (primCount > 16384) skip("over 16384 triangles, the rest unread");
    const std::vector<BYTE>& data = vb->m_impl.m_shadow;
    const IndexBuffer* ib = indexed ? m_indices : nullptr;
    if (indexed && (!ib || ib->m_impl.m_dynamic)) { skip("dynamic indices"); return; }

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
        auto key = [](long long x, long long y, long long z) {
            return ((unsigned long long)(x & 0x1FFFFF) << 42) | ((unsigned long long)(y & 0x1FFFFF) << 21) | (unsigned long long)(z & 0x1FFFFF);
        };
        unsigned long long k = key(x, y, z);
        // A lamp nudged across a cell border (LOD, animation) keeps its entry instead of leaving a
        // stale copy behind for kLightMemoryFrames, which doubled its brightness.
        if (!m_worldLights.count(k))
            for (int n = 0; n < 27; n++) {
                if (n == 13) continue;
                const unsigned long long nk = key(x + n % 3 - 1, y + n / 3 % 3 - 1, z + n / 9 - 1);
                if (m_worldLights.count(nk)) { k = nk; break; }
            }
        WorldLight& l = m_worldLights[k];
        if (l.lastSeen != (UINT)(m_frame + 1)) { // first sighting this frame: replace
            l = {};
            l.lastSeen = (UINT)(m_frame + 1);
        }
        const float n = (float)l.count;
        l.pos[0] = (l.pos[0] * n + f.x) / (n + 1); l.pos[1] = (l.pos[1] * n + f.y) / (n + 1); l.pos[2] = (l.pos[2] * n + f.z) / (n + 1);
        // Every lamp the same strength, in its mask's hue: the glowing triangle count and the
        // texel under each centroid differ between a model's LODs (a fence segment has 30 glowing
        // triangles per lamp, the merged far set 4), which made a lamp change as the camera moved.
        // Two draws of one lamp in a frame (shared posts, LOD overlap) do not add up.
        const float peak = (std::max)(e.rgb[0], (std::max)(e.rgb[1], e.rgb[2]));
        for (int c = 0; c < 3; c++) l.rgb[c] = (std::max)(l.rgb[c], kLampLevel * e.rgb[c] / (std::max)(peak, 1e-3f));
        l.count++;
    }
}

// End of frame: every remembered lamp is its own light (no grouping that changes with the camera);
// the nearest kMaxPointLights light the next frame, eased in and out per lamp.
void Device::BuildPointLights()
{
    m_lightData.clear();
    m_pointLightsShown = 0;
    if (!g_config.effectsEnabled || !g_config.pointLights || !m_tod.active) { m_worldLights.clear(); m_shownLights.clear(); return; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const float dt = m_shownLightsTime.QuadPart ? (float)((t.QuadPart - m_shownLightsTime.QuadPart) / (double)m_qpcFreq.QuadPart) : 0.0f;
    m_shownLightsTime = t;
    const float ease = 1.0f - std::exp(-(std::min)(dt, 0.25f) / 0.15f); // 0.15 s to settle
    if (m_emitterCache.size() > 8192) m_emitterCache.clear();
    const UINT now = (UINT)(m_frame + 1);
    for (auto it = m_worldLights.begin(); it != m_worldLights.end();)
        it = now - it->second.lastSeen > kLightMemoryFrames ? m_worldLights.erase(it) : std::next(it);
    m_pointLightTris = (UINT)m_worldLights.size();
    const float night = 1 - m_tod.dayW;
    if (night <= 0.01f || m_worldLights.empty()) { m_shownLights.clear(); return; }

    struct Candidate { const WorldLight* l; float dist; unsigned long long key; };
    std::vector<Candidate> all;
    all.reserve(m_worldLights.size());
    const float* cam = m_sun.camera;
    for (const auto& w : m_worldLights) {
        const float dx = w.second.pos[0] - cam[0], dy = w.second.pos[1] - cam[1], dz = w.second.pos[2] - cam[2];
        all.push_back({ &w.second, std::sqrt(dx * dx + dy * dy + dz * dz), w.first });
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
    const size_t keep = (std::min)(all.size(), (size_t)kMaxPointLights);
    // Past the cap the farthest kept lamps fade out by distance, not by rank.
    const float cutoff = all.size() > keep ? all[keep].dist : 1e30f;

    // Colour: the mask's own colour pulled toward the warm lamp tint.
    const float warm = g_config.pointLightWarmth;
    for (auto& e : m_shownLights) e.second.live = false;
    for (size_t i = 0; i < keep; i++) {
        const Candidate& c = all[i];
        const float fade = cutoff < 1e29f ? (std::min)(1.0f, (std::max)(0.0f, (cutoff - c.dist) / (cutoff * 0.25f))) : 1.0f;
        const float* rgb = c.l->rgb;
        const float lum = rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
        auto found = m_shownLights.find(c.key);
        const bool known = found != m_shownLights.end();
        ShownLight& o = known ? found->second : (m_shownLights[c.key] = ShownLight{});
        for (int k = 0; k < 3; k++) {
            o.pos[k] = c.l->pos[k];
            const float col = rgb[k] + (lum * g_config.pointLightTint[k] - rgb[k]) * warm;
            const float target = col * g_config.pointLightIntensity * night * fade;
            o.rgb[k] = known ? o.rgb[k] + (target - o.rgb[k]) * ease : target;
        }
        o.level += (1.0f - o.level) * ease;
        o.dist = c.dist;
        o.live = true;
    }
    // Lamps no longer kept fade out where they were.
    for (auto it = m_shownLights.begin(); it != m_shownLights.end();) {
        if (!it->second.live) it->second.level -= it->second.level * ease;
        it = !it->second.live && it->second.level < 0.01f ? m_shownLights.erase(it) : std::next(it);
    }
    std::vector<const ShownLight*> order;
    order.reserve(m_shownLights.size());
    for (const auto& e : m_shownLights) order.push_back(&e.second);
    // Nearest first: a tile that overflows keeps its nearest lamps (BuildLightTiles).
    std::sort(order.begin(), order.end(), [](const ShownLight* a, const ShownLight* b) {
        return a->live != b->live ? a->live : a->dist < b->dist; });
    const size_t n = (std::min)(order.size(), (size_t)kMaxPointLights);
    const float radius = g_config.pointLightRadius;
    m_lightData.resize(n * 8);
    for (size_t i = 0; i < n; i++) {
        float* d = &m_lightData[i * 8];
        for (int k = 0; k < 3; k++) {
            d[k] = order[i]->pos[k];
            d[4 + k] = order[i]->rgb[k] * order[i]->level;
        }
        d[3] = radius;
        d[7] = 0;
    }
    m_pointLightsShown = (UINT)n;
}

// Lamps (t18, two float4 each: position + radius, colour) and per screen tile the lamps whose
// sphere can reach it (t17: one uint per tile, offset << 8 | count, then the index lists).
// Built when the frame's camera is known.
void Device::BuildLightTiles()
{
    m_pointLights = {};
    m_pointLightsDirty = true;
    const UINT n = (UINT)(m_lightData.size() / 8);
    if (!n || !m_backBuffer) return;
    const UINT w = m_backBuffer->m_width, h = m_backBuffer->m_height;
    const UINT tx = (w + kLightTile - 1) / kLightTile, ty = (h + kLightTile - 1) / kLightTile;
    if (!tx || !ty) return;
    const UINT tiles = tx * ty;

    struct Rect { int ax, bx, ay, by; };
    std::vector<Rect> rects(n);
    std::vector<UINT> counts(tiles, 0);
    const float (*vp)[4] = m_waterViewProjection; // clip[r] = dot(float4(P, 1), vp[r])
    UINT nearPlane = 0, behind = 0;
    for (UINT i = 0; i < n; i++) {
        const float* l = &m_lightData[i * 8];
        const float r = l[3];
        Rect& b = rects[i];
        b.bx = -1;
        // Screen bound of the lamp's box, clipped to the near plane (clip z = 0): a lamp beside or
        // behind the camera reaches no pixel, one crossing the plane covers only what its front
        // part projects to. (Taking every tile for those filled the tiles with lamps nobody sees,
        // and the visible ones were dropped: all lamps went out with the camera low.)
        float clip[8][4];
        bool front = false, back = false;
        for (int c = 0; c < 8; c++) {
            const float p[3] = { l[0] + (c & 1 ? r : -r), l[1] + (c & 2 ? r : -r), l[2] + (c & 4 ? r : -r) };
            for (int k = 0; k < 4; k++) clip[c][k] = p[0] * vp[k][0] + p[1] * vp[k][1] + p[2] * vp[k][2] + vp[k][3];
            (clip[c][2] >= 0 ? front : back) = true;
        }
        if (!front) { behind++; continue; }
        nearPlane += back;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool unbounded = false;
        auto add = [&](const float* q) {
            if (q[3] < 1e-4f) { unbounded = true; return; }
            const float sx = (q[0] / q[3] * 0.5f + 0.5f) * w, sy = (0.5f - q[1] / q[3] * 0.5f) * h;
            x0 = (std::min)(x0, sx); x1 = (std::max)(x1, sx); y0 = (std::min)(y0, sy); y1 = (std::max)(y1, sy);
        };
        for (int c = 0; c < 8; c++) {
            if (clip[c][2] >= 0) add(clip[c]);
            for (int bit = 1; bit < 8; bit <<= 1) { // the box edges that cross the plane, once each
                const int d = c | bit;
                if (d == c || (clip[c][2] >= 0) == (clip[d][2] >= 0)) continue;
                const float t = clip[c][2] / (clip[c][2] - clip[d][2]);
                float q[4];
                for (int k = 0; k < 4; k++) q[k] = clip[c][k] + (clip[d][k] - clip[c][k]) * t;
                add(q);
            }
        }
        b = { 0, (int)tx - 1, 0, (int)ty - 1 };
        if (!unbounded) {
            if (x1 < 0 || y1 < 0 || x0 >= w || y0 >= h) { b.bx = -1; continue; } // off screen
            // One tile of margin: TAA/FSR jitter and the rasteriser's half-pixel rules.
            b.ax = (std::max)(0, (int)(x0 / kLightTile) - 1); b.bx = (std::min)((int)tx - 1, (int)(x1 / kLightTile) + 1);
            b.ay = (std::max)(0, (int)(y0 / kLightTile) - 1); b.by = (std::min)((int)ty - 1, (int)(y1 / kLightTile) + 1);
        }
        for (int y = b.ay; y <= b.by; y++)
            for (int x = b.ax; x <= b.bx; x++) {
                UINT& c = counts[size_t(y) * tx + x];
                if (c < kLightsPerTile) c++;
            }
    }
    UINT total = tiles, full = 0, most = 0;
    for (UINT i = 0; i < tiles; i++) { total += counts[i]; full += counts[i] >= kLightsPerTile; most = (std::max)(most, counts[i]); }
    if (g_config.pointLightDebug && m_frame % 300 == 0)
        Log("Point lights: first at %.1f %.1f %.1f (tiles %d-%d, %d-%d); %u remembered, %u shown, %u behind the camera, %u across its near plane; tiles: %u of %u full, %u most, %.1f average",
            m_lightData[0], m_lightData[1], m_lightData[2], rects[0].ax, rects[0].bx, rects[0].ay, rects[0].by, m_pointLightTris, n, behind, nearPlane, full, tiles, most, (total - tiles) / (float)tiles);
    m_lightTiles.assign(total, 0);
    UINT at = tiles;
    for (UINT i = 0; i < tiles; i++) { m_lightTiles[i] = at << 8; at += counts[i]; }
    for (UINT i = 0; i < n; i++) { // nearest lamps first, so a full tile keeps those
        const Rect& b = rects[i];
        for (int y = b.ay; y <= b.by; y++)
            for (int x = b.ax; x <= b.bx; x++) {
                UINT& e = m_lightTiles[size_t(y) * tx + x];
                const UINT c = e & 0xff;
                if (c >= counts[size_t(y) * tx + x]) continue;
                m_lightTiles[(e >> 8) + c] = i;
                e++;
            }
    }

    // Grow either buffer geometrically; both are rewritten every frame.
    auto ensure = [&](ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11ShaderResourceView>& srv, UINT& capacity, UINT need, UINT bytesPer, DXGI_FORMAT fmt) {
        if (capacity >= need) return true;
        const UINT size = (std::max)(need, capacity * 2);
        srv.Reset(); buf.Reset(); capacity = 0;
        D3D11_BUFFER_DESC bd = { size * bytesPer, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = size;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &buf)) || FAILED(m_device->CreateShaderResourceView(buf.Get(), &sd, &srv))) {
            buf.Reset(); srv.Reset(); return false;
        }
        capacity = size;
        return true;
    };
    if (!ensure(m_lightTileBuf, m_lightTileSRV, m_lightTileCapacity, total, 4, DXGI_FORMAT_R32_UINT) ||
        !ensure(m_lightDataBuf, m_lightDataSRV, m_lightDataCapacity, n * 2, 16, DXGI_FORMAT_R32G32B32A32_FLOAT)) return;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_lightTileBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, m_lightTiles.data(), m_lightTiles.size() * sizeof(UINT));
    m_ctx->Unmap(m_lightTileBuf.Get(), 0);
    if (FAILED(m_ctx->Map(m_lightDataBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, m_lightData.data(), m_lightData.size() * sizeof(float));
    m_ctx->Unmap(m_lightDataBuf.Get(), 0);
    const float radius = g_config.pointLightRadius;
    m_pointLights.count[0] = (float)n;
    m_pointLights.count[1] = 1.0f / (radius * radius * 0.08f); // inverse-square knee (about a third of the radius)
    m_pointLights.count[2] = (float)tx;
    m_pointLights.count[3] = g_config.pointLightDebug == 2 ? 3.0f : g_config.pointLightDebug ? 2.0f : 1.0f;
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
    ID3D11ShaderResourceView* views[2] = { m_lightTileSRV.Get(), m_lightDataSRV.Get() };
    BindPSSrv(17, 2, views);
}
