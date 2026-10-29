// Distant islands: a few mountain height fields generated once on a worker thread, raymarched by
// the resolve (sun.cpp DistantIslands) on the sky beyond the sea's edge. Each shape is ridged
// noise over a warped island outline with one to three massifs, then carved by droplet hydraulic
// erosion (Hans Theobald Beyer, "Implementation of a method for hydraulic erosion"), which gives
// the valleys and spurs that make a ridge read as a mountain from far away.
// Texel: height (1 = the island's peak, below 0 = sea), its slope along u and v (per unit of the
// texture), and how far the ground lies in a gully (darker, wetter, greener in the shader).
#include "device.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <future>
#include <random>
#include <vector>

namespace {
const int N = 512, kShapes = 4; // Device::kIslandSize, kIslandShapes

uint32_t Mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

// Gradient noise, roughly -0.7..0.7.
float Perlin(float x, float y, uint32_t seed)
{
    const int ix = (int)std::floor(x), iy = (int)std::floor(y);
    const float fx = x - ix, fy = y - iy;
    auto grad = [&](int cx, int cy, float dx, float dy) {
        const float a = Mix((uint32_t)cx * 0x8da6b343u ^ (uint32_t)cy * 0xd8163841u ^ seed) * (6.2831853f / 4294967296.0f);
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const float u = fade(fx), v = fade(fy);
    const float a = grad(ix, iy, fx, fy), b = grad(ix + 1, iy, fx - 1, fy);
    const float c = grad(ix, iy + 1, fx, fy - 1), d = grad(ix + 1, iy + 1, fx - 1, fy - 1);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

float Fbm(float x, float y, int octaves, uint32_t seed)
{
    float sum = 0, amp = 0.5f;
    for (int o = 0; o < octaves; o++) { sum += Perlin(x, y, seed + o * 1013) * amp; x *= 2.03f; y *= 2.03f; amp *= 0.5f; }
    return sum;
}

// Ridged multifractal: sharp crests where the noise crosses zero, finer crests riding on the big ones.
float Ridged(float x, float y, int octaves, uint32_t seed)
{
    float sum = 0, amp = 0.5f, weight = 1, norm = 0;
    for (int o = 0; o < octaves; o++) {
        float n = 1 - std::fabs(Perlin(x, y, seed + o * 7919) * 1.45f);
        n = n * n * weight;
        weight = (std::min)(1.0f, n * 1.6f);
        sum += n * amp; norm += amp;
        x *= 2.1f; y *= 2.1f; amp *= 0.52f;
    }
    return sum / norm;
}

struct Droplets {
    std::vector<float>& h;
    // Brush: the cells within radius 3 of a node and their weights.
    std::vector<int> ox, oy; std::vector<float> ow;
    explicit Droplets(std::vector<float>& map) : h(map)
    {
        const int r = 3; float sum = 0;
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                const float d = std::sqrt(float(x * x + y * y));
                if (d < r) { ox.push_back(x); oy.push_back(y); ow.push_back(1 - d / r); sum += 1 - d / r; }
            }
        for (float& w : ow) w /= sum;
    }
    void Gradient(float x, float y, float& height, float& gx, float& gy) const
    {
        const int cx = (int)x, cy = (int)y;
        const float u = x - cx, v = y - cy;
        const float a = h[cy * N + cx], b = h[cy * N + cx + 1], c = h[(cy + 1) * N + cx], d = h[(cy + 1) * N + cx + 1];
        gx = (b - a) * (1 - v) + (d - c) * v;
        gy = (c - a) * (1 - u) + (d - b) * u;
        height = a * (1 - u) * (1 - v) + b * u * (1 - v) + c * (1 - u) * v + d * u * v;
    }
    void Run(int count, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> at(4.0f, N - 5.0f);
        const float inertia = 0.05f, capacityK = 4, minCapacity = 0.01f, erode = 0.3f, deposit = 0.3f;
        const float evaporate = 0.01f, gravity = 4;
        for (int n = 0; n < count; n++) {
            float x = at(rng), y = at(rng), dx = 0, dy = 0, speed = 1, water = 1, sediment = 0;
            for (int life = 0; life < 45; life++) {
                const int cx = (int)x, cy = (int)y;
                const float u = x - cx, v = y - cy;
                float height, gx, gy;
                Gradient(x, y, height, gx, gy);
                dx = dx * inertia - gx * (1 - inertia);
                dy = dy * inertia - gy * (1 - inertia);
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-8f) break;
                dx /= len; dy /= len;
                x += dx; y += dy;
                if (x < 4 || y < 4 || x >= N - 5 || y >= N - 5) break;
                // Into the sea: what it carries settles on the shore.
                if (height < 0) break;
                float nh, ngx, ngy;
                Gradient(x, y, nh, ngx, ngy);
                const float dh = nh - height;
                const float capacity = (std::max)(-dh * speed * water * capacityK, minCapacity);
                if (sediment > capacity || dh > 0) {
                    const float amount = dh > 0 ? (std::min)(dh, sediment) : (sediment - capacity) * deposit;
                    sediment -= amount;
                    h[cy * N + cx] += amount * (1 - u) * (1 - v);
                    h[cy * N + cx + 1] += amount * u * (1 - v);
                    h[(cy + 1) * N + cx] += amount * (1 - u) * v;
                    h[(cy + 1) * N + cx + 1] += amount * u * v;
                } else {
                    const float amount = (std::min)((capacity - sediment) * erode, -dh);
                    for (size_t k = 0; k < ow.size(); k++) {
                        const int bx = cx + ox[k], by = cy + oy[k];
                        if (bx < 0 || by < 0 || bx >= N || by >= N) continue;
                        float& cell = h[by * N + bx];
                        const float take = (std::min)(cell + 1.0f, amount * ow[k]);
                        cell -= take; sediment += take;
                    }
                }
                speed = std::sqrt((std::max)(speed * speed + dh * gravity * -1, 0.0f));
                water *= 1 - evaporate;
            }
        }
    }
};

std::vector<float> MakeShape(int s)
{
    const uint32_t seed = Mix(0x51a7d + s * 0x9e3779b9u);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> r01(0, 1);
    // Elongated: one axis the full radius, the other two fifths to four fifths of it.
    const float ax = 1.0f, ay = 0.42f + 0.4f * r01(rng);
    const float rot = r01(rng) * 6.2831853f, cr = std::cos(rot), sr = std::sin(rot);
    // The massifs: one to three peaks, the first the highest.
    const int peaks = 1 + (int)(r01(rng) * 3);
    float pcx[3], pcy[3], ph[3], pw[3];
    for (int k = 0; k < peaks; k++) {
        const float a = r01(rng) * 6.2831853f, d = k ? 0.25f + 0.25f * r01(rng) : 0.12f * r01(rng);
        pcx[k] = std::cos(a) * d * ax; pcy[k] = std::sin(a) * d * ay;
        ph[k] = k ? 0.45f + 0.4f * r01(rng) : 1.0f;
        pw[k] = 0.22f + 0.18f * r01(rng);
    }
    std::vector<float> h((size_t)N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const float px = (x + 0.5f) / N * 2 - 1, py = (y + 0.5f) / N * 2 - 1;
            // A ragged coastline: the outline warped by low noise.
            const float qx = px + 0.24f * Fbm(px * 1.5f + 3.1f, py * 1.5f, 5, seed + 11);
            const float qy = py + 0.24f * Fbm(px * 1.5f, py * 1.5f + 5.3f, 5, seed + 23);
            const float lx = (qx * cr - qy * sr) / ax, ly = (qx * sr + qy * cr) / ay;
            const float r = std::sqrt(lx * lx + ly * ly);
            const float t = (std::min)(1.0f, (std::max)(0.0f, (1.1f - r) / 0.65f));
            const float land = t * t * (3 - 2 * t);
            float massif = 0;
            for (int k = 0; k < peaks; k++) {
                const float ex = qx - pcx[k], ey = qy - pcy[k];
                massif = (std::max)(massif, ph[k] * std::exp(-(ex * ex + ey * ey) / (pw[k] * pw[k])));
            }
            const float ridge = Ridged(qx * 2.6f + 7.7f, qy * 2.6f, 7, seed + 41);
            float v = std::pow(land, 1.4f) * (0.12f + 0.55f * massif) + land * land * ridge * (0.15f + 0.6f * massif);
            v -= 0.02f + 0.12f * (1 - land);
            h[(size_t)y * N + x] = v;
        }
    float top = 0;
    for (float v : h) top = (std::max)(top, v);
    for (float& v : h) v /= (std::max)(top, 1e-3f);
    Droplets(h).Run(140000, seed + 77);
    // Soften the droplets' pits and keep the sea floor below the water.
    std::vector<float> soft(h);
    for (int y = 1; y < N - 1; y++)
        for (int x = 1; x < N - 1; x++) {
            float sum = 0;
            for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) sum += h[(size_t)(y + j) * N + x + i] * (i || j ? 1.0f : 4.0f);
            soft[(size_t)y * N + x] = sum / 12;
        }
    top = 0;
    for (float v : soft) top = (std::max)(top, v);
    for (float& v : soft) v = (std::max)(v / (std::max)(top, 1e-3f), -0.3f);
    return soft;
}

struct Built { std::vector<uint16_t> texels[kShapes][10]; int mips = 0; };

Built Build()
{
    Built out;
    for (int s = 0; s < kShapes; s++) {
        const std::vector<float> h = MakeShape(s);
        auto at = [&](int x, int y) { return h[(size_t)std::clamp(y, 0, N - 1) * N + std::clamp(x, 0, N - 1)]; };
        // Gullies: how far below the ground around (radius 6 texels) a texel lies.
        std::vector<float> blur(h.size()), tmp(h.size());
        const int R = 6;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) { float sum = 0; for (int i = -R; i <= R; i++) sum += at(x + i, y); tmp[(size_t)y * N + x] = sum / (2 * R + 1); }
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float sum = 0; for (int j = -R; j <= R; j++) sum += tmp[(size_t)std::clamp(y + j, 0, N - 1) * N + x];
                blur[(size_t)y * N + x] = sum / (2 * R + 1);
            }
        std::vector<float> level((size_t)N * N * 4);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float* t = &level[((size_t)y * N + x) * 4];
                t[0] = at(x, y);
                t[1] = (at(x + 1, y) - at(x - 1, y)) * N / 2;
                t[2] = (at(x, y + 1) - at(x, y - 1)) * N / 2;
                t[3] = (std::min)(1.0f, (std::max)(0.0f, (blur[(size_t)y * N + x] - t[0]) * 30));
            }
        int size = N, m = 0;
        for (;; m++) {
            std::vector<uint16_t>& half = out.texels[s][m];
            half.resize((size_t)size * size * 4);
            for (size_t i = 0; i < half.size(); i++) half[i] = DirectX::PackedVector::XMConvertFloatToHalf(level[i]);
            if (size == 4 || m == 9) break;
            const int next = size / 2;
            std::vector<float> down((size_t)next * next * 4);
            for (int y = 0; y < next; y++)
                for (int x = 0; x < next; x++)
                    for (int ch = 0; ch < 4; ch++)
                        down[((size_t)y * next + x) * 4 + ch] = 0.25f * (level[(((size_t)2 * y) * size + 2 * x) * 4 + ch] + level[(((size_t)2 * y) * size + 2 * x + 1) * 4 + ch] +
                                                                         level[(((size_t)2 * y + 1) * size + 2 * x) * 4 + ch] + level[(((size_t)2 * y + 1) * size + 2 * x + 1) * 4 + ch]);
            level.swap(down);
            size = next;
        }
        out.mips = m + 1;
    }
    return out;
}

std::future<Built> g_islandJob;
}

bool Device::EnsureIslands()
{
    if (m_islandSRV) return true;
    if (m_islandFault) return false;
    if (!g_islandJob.valid()) { g_islandJob = std::async(std::launch::async, Build); return false; }
    if (g_islandJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    const Built built = g_islandJob.get();
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    for (int s = 0; s < kIslandShapes; s++)
        for (int m = 0; m < built.mips; m++)
            init.push_back({ built.texels[s][m].data(), (UINT)((N >> m) * 4 * sizeof(uint16_t)), 0 });
    D3D11_TEXTURE2D_DESC td = { (UINT)N, (UINT)N, (UINT)built.mips, (UINT)kIslandShapes, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 },
                                D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, init.data(), &m_islandTex)) ||
        FAILED(m_device->CreateShaderResourceView(m_islandTex.Get(), nullptr, &m_islandSRV))) {
        Log("Distant islands: texture creation failed");
        m_islandSRV.Reset(); m_islandTex.Reset();
        m_islandFault = true;
        return false;
    }
    Log("Distant islands: %d eroded %dx%d height fields", kIslandShapes, N, N);
    return true;
}
