// FFT ocean surface for the water shader (shaders.cpp WaterWave): a looping animation of wave
// slopes generated once from a Phillips wind-wave spectrum (Tessendorf, "Simulating Ocean Water").
// Each texel holds the slopes along x and z and their squares, so the mip chain averages the
// squares too: a filtered texel knows how much slope variance it lost, which the shader turns into
// GGX roughness (far water reads smooth with a broad sun path instead of aliasing into moire).
#include "device.h"
#include <DirectXPackedVector.h>
#include <complex>
#include <random>
#include <vector>

namespace {
using Complex = std::complex<float>;
const float kPi = 3.14159265f;

// In-place inverse FFT of n (a power of two) values spaced `stride` apart.
void InverseFFT(Complex* a, int n, int stride)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i * stride], a[j * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = 2 * kPi / len;
        const Complex step(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            Complex w(1, 0);
            for (int k = 0; k < len / 2; k++) {
                Complex& lo = a[(i + k) * stride];
                Complex& hi = a[(i + k + len / 2) * stride];
                const Complex u = lo, v = hi * w;
                lo = u + v; hi = u - v;
                w *= step;
            }
        }
    }
}
}

bool Device::EnsureOcean()
{
    if (m_oceanSRV) return true;
    if (m_oceanFault) return false;
    const int N = kOceanSize, F = kOceanFrames;
    const float L = 64.0f;           // patch size in spectrum units (the shader scales it to the world)
    const float wind = 7.0f, g = 9.81f, Lw = wind * wind / g;
    const float windDir[2] = { std::cos(0.35f), std::sin(0.35f) };
    // Frequencies rounded to multiples of the loop's own so the animation repeats seamlessly.
    const float w0 = 2 * kPi / kOceanPeriod;
    std::mt19937 rng(1234567);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<Complex> h0(N * N);
    std::vector<float> omega(N * N), kx(N * N), kz(N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const int i = y * N + x;
            const float mx = (float)(x < N / 2 ? x : x - N), my = (float)(y < N / 2 ? y : y - N);
            kx[i] = 2 * kPi * mx / L; kz[i] = 2 * kPi * my / L;
            const float k = std::sqrt(kx[i] * kx[i] + kz[i] * kz[i]);
            float phillips = 0;
            if (k > 1e-4f) {
                const float c = (kx[i] * windDir[0] + kz[i] * windDir[1]) / k;
                phillips = std::exp(-1 / (k * Lw * k * Lw)) / (k * k * k * k) * c * c;
                if (c < 0) phillips *= 0.07f;                 // little against the wind
                phillips *= std::exp(-k * k * 0.3f * 0.3f);  // no waves under the texel scale
            }
            const float a = std::sqrt(phillips * 0.5f);
            h0[i] = Complex(gauss(rng) * a, gauss(rng) * a);
            omega[i] = std::floor(std::sqrt(g * k) / w0) * w0;
        }
    const int mips = kOceanMips;
    std::vector<std::vector<float>> frames(F); // per frame: N*N*4 (sx, sz, sx^2, sz^2)
    std::vector<Complex> c(N * N);
    double sum2 = 0;
    for (int f = 0; f < F; f++) {
        const float t = kOceanPeriod * f / F;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                const int i = y * N + x, j = ((N - y) % N) * N + (N - x) % N; // -k
                const Complex e(std::cos(omega[i] * t), std::sin(omega[i] * t));
                const Complex h = h0[i] * e + std::conj(h0[j]) * std::conj(e);
                // Both slope fields in one transform: i kx h + i (i kz h); real part sx, imaginary sz.
                c[i] = Complex(0, kx[i]) * h - kz[i] * h;
            }
        for (int y = 0; y < N; y++) InverseFFT(&c[y * N], N, 1);
        for (int x = 0; x < N; x++) InverseFFT(&c[x], N, N);
        std::vector<float>& out = frames[f];
        out.resize(N * N * 4);
        for (int i = 0; i < N * N; i++) {
            out[i * 4 + 0] = c[i].real(); out[i * 4 + 1] = c[i].imag();
            sum2 += c[i].real() * c[i].real() + c[i].imag() * c[i].imag();
        }
    }
    // Normalized to a slope RMS of 1 per axis; the shader sets the real strength.
    const float scale = sum2 > 0 ? (float)(1 / std::sqrt(sum2 / (2.0 * N * N * F))) : 1.0f;
    std::vector<std::vector<uint16_t>> levels((size_t)F * mips);
    std::vector<D3D11_SUBRESOURCE_DATA> init((size_t)F * mips);
    for (int f = 0; f < F; f++) {
        std::vector<float> level = frames[f];
        for (int i = 0; i < N * N; i++) {
            float& sx = level[i * 4], & sz = level[i * 4 + 1];
            sx *= scale; sz *= scale;
            level[i * 4 + 2] = sx * sx; level[i * 4 + 3] = sz * sz;
        }
        int size = N;
        for (int m = 0; m < mips; m++) {
            std::vector<uint16_t>& half = levels[(size_t)f * mips + m];
            half.resize((size_t)size * size * 4);
            for (size_t i = 0; i < half.size(); i++) half[i] = DirectX::PackedVector::XMConvertFloatToHalf(level[i]);
            init[(size_t)f * mips + m] = { half.data(), (UINT)(size * 4 * sizeof(uint16_t)), 0 };
            if (size == 1) break;
            // Box-filtered next level: the squares average with the slopes, so the variance survives.
            const int next = size / 2;
            std::vector<float> down((size_t)next * next * 4);
            for (int y = 0; y < next; y++)
                for (int x = 0; x < next; x++)
                    for (int ch = 0; ch < 4; ch++)
                        down[(y * next + x) * 4 + ch] = 0.25f * (level[((2 * y) * size + 2 * x) * 4 + ch] + level[((2 * y) * size + 2 * x + 1) * 4 + ch] +
                                                                 level[((2 * y + 1) * size + 2 * x) * 4 + ch] + level[((2 * y + 1) * size + 2 * x + 1) * 4 + ch]);
            level.swap(down);
            size = next;
        }
    }
    D3D11_TEXTURE2D_DESC td = { (UINT)N, (UINT)N, (UINT)mips, (UINT)F, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 },
                                D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateTexture2D(&td, init.data(), &m_oceanTex)) ||
        FAILED(m_device->CreateShaderResourceView(m_oceanTex.Get(), nullptr, &m_oceanSRV)) ||
        FAILED(m_device->CreateSamplerState(&sd, &m_oceanSampler))) {
        Log("FFT ocean: texture creation failed, using the noise ripples");
        m_oceanSRV.Reset(); m_oceanTex.Reset(); m_oceanSampler.Reset();
        m_oceanFault = true;
        return false;
    }
    Log("FFT ocean: %d frames of %dx%d slopes over %.0f s", F, N, N, kOceanPeriod);
    return true;
}
