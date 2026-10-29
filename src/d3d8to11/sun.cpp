// Sun shadows, same-frame and screen-space:
//  1. The first terrain draw of a frame gives the camera (its c0..c3 is the world-to-clip
//     matrix). A light-space orthographic box is fitted to the visible ground and the shadow
//     map is cleared.
//  2. Every opaque world draw is rendered a second time into the shadow map. The translated
//     vertex shader rebuilds the world position from its own clip position through the
//     inverse camera matrix, so skinned, rigid and terrain geometry all work the same way.
//  3. When the world is finished (first HUD draw, a depth clear, or Present) a full-screen
//     pass reads the scene depth, reconstructs world positions and normals, samples the shadow
//     map and multiplies the back buffer. The game's own lighting is never replaced.
#include "device.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
using namespace DirectX;

namespace {

struct ResolveConstants {
    float clipToWorld[4][4];
    float lightRows[4][4];
    float sunDir[4];
    float params[4]; // strength, 1/map size, depth bias (light depth units), normal offset (world)
    float screen[4]; // 1/W, 1/H, half-pixel offset applied by the vertex shaders (x, y)
    float misc[4];   // debug view, shadow distance, light depth range, shadows on
    float grade0[4]; // exposure, contrast, saturation, warmth
    float grade1[4]; // sun boost, shadow cool, post on, -
    float fx0[4];    // AO radius (world), AO intensity, AO strength, light shaft strength
    float fx1[4];    // AO on, shafts on, scattering g, -
    float tod[4];    // time of day: sky multiplier, w = sun/moon light (shafts, sunlight lift)
    float tod2[4];   // true sun direction (renderer Y-down), w = procedural sky weight
    float tod3[4];   // moon visibility, shadow tint rgb
    float tod4[4];   // night sky brightness, stars, day weight, night desaturation
    float tod5[4];   // night tint, w = filmic exposure (0 = off)
    float tod6[4];   // toward the moon (renderer Y-down), w = lit share of its disc (phase)
    float tod7[4];   // star rotation (radians), moon above the horizon, sun disc brightness, sky bodies on
    float cloud0[4]; // clouds: coverage, extinction per unit, base height, top height (above the sea)
    float cloud1[4]; // key light (sun or moon) colour, w = clouds on
    float cloud2[4]; // sky ambient, w = drift x
    float cloud3[4]; // ground ambient, w = drift z
    float cloud4[4]; // toward the key light (renderer Y-down)
    float cloud5[4]; // step jitter of this frame, seconds, horizon haze start (share of the view distance)
    float fog0[4];   // fog: density per unit, low fog density at the base, 1 / falloff height, on
    float fog1[4];   // low fog base height (above y = 0, up positive), toward the key light (renderer Y-down)
    float fog2[4];   // light scattered toward the sun (rgb)
    float rays0[4];  // god rays: the light's place on screen (uv), -, strength there (facing, on screen)
    float rays1[4];  // god ray colour, w = on
    float cloud6[4]; // cloud shadow map (volumetric shafts): world x, z of its corner, size (world units), w = ready
    float vfog0[4];  // volumetric fog: density at the floor, floor height, 1 / falloff height, on
    float vfog1[4];  // key light colour, patchiness
    float vfog2[4];  // ambient (sky) colour, seconds
    float vfog3[4];  // toward the key light (renderer Y-down)
    float vfog4[4];  // terrain height field: 1 / width, 1 / height, height it forms from, height range above that
    float day0[4];   // day colour: brightness, dark lift, saturation, weight (0 at night)
    float isle0[4];  // distant islands: on, height, haze, brightness
    float isle1[4];  // key light colour, w = highest elevation an island reaches from the camera
    float isle2[4];  // toward the key light (renderer Y-down), w = clouds on the islands
    float isle3[4];  // sky ambient, w = cloud drift (seconds)
    float isleA[8][4]; // per island: centre x, z, radius, peak height (radius 0 = none)
    float isleB[8][4]; // per island: rotation cos, sin, shape slice
};

const char* kResolveVS =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";

// Shared by the effects pass and the resolve pass. Compiled with MSAA 0 (Texture2D depth) and
// MSAA 1 (Texture2DMS depth, sample 0).
const char* kSceneCommon =
    "cbuffer Resolve : register(b0) { float4 clipToWorld[4]; float4 lightRows[4]; float4 sunDir;\n"
    "  float4 params; float4 screen; float4 misc; float4 grade0; float4 grade1; float4 fx0; float4 fx1; float4 tod;\n"
    "  float4 tod2; float4 tod3; float4 tod4; float4 tod5; float4 tod6; float4 tod7;\n"
    "  float4 cloud0; float4 cloud1; float4 cloud2; float4 cloud3; float4 cloud4; float4 cloud5; float4 fog0; float4 fog1; float4 fog2; float4 rays0; float4 rays1; float4 cloud6;\n"
    "  float4 vfog0; float4 vfog1; float4 vfog2; float4 vfog3; float4 vfog4; float4 day0; float4 isle0;\n"
    "  float4 isle1; float4 isle2; float4 isle3; float4 isleA[8]; float4 isleB[8]; };\n"
    "#if MSAA\n"
    "Texture2DMS<float> sceneDepth : register(t0);\n"
    "float LoadDepth(int2 p) { return sceneDepth.Load(p, 0); }\n"
    "#else\n"
    "Texture2D<float> sceneDepth : register(t0);\n"
    "float LoadDepth(int2 p) { return sceneDepth.Load(int3(p, 0)); }\n"
    "#endif\n"
    "Texture2D<float> shadowMap : register(t1);\n"
    "SamplerComparisonState cmp : register(s0);\n"
    "float3 Unproject(float2 px, float d) {\n"
    "  float2 ndc = float2(px.x * screen.x * 2 - 1, 1 - px.y * screen.y * 2) - screen.zw;\n"
    "  float4 c = float4(ndc, d, 1);\n"
    "  float4 w = float4(dot(c, clipToWorld[0]), dot(c, clipToWorld[1]), dot(c, clipToWorld[2]), dot(c, clipToWorld[3]));\n"
    "  return w.xyz / w.w; }\n"
    "float3 WorldAt(int2 p) { return Unproject(p + 0.5, LoadDepth(p)); }\n"
    // Light the clouds let through to a world point (Y-down): the cloud shadow map is drawn for
    // the ground, so follow the light ray down to height 0 first.
    "Texture2D<float> cloudShadowTex : register(t7);\n"
    "SamplerState cloudShadowSmp : register(s2);\n"
    "float CloudShadowAt(float3 X) {\n"
    "  if (cloud6.w < 0.5) return 1;\n"
    "  float h = clamp(-X.y, 0, cloud0.z);\n"
    "  float2 g = X.xz - cloud4.xz * (h / max(-cloud4.y, 0.05));\n"
    "  float2 uv = (g - cloud6.xy) / cloud6.z;\n"
    "  if (any(uv < 0) || any(uv > 1)) return 1;\n"
    "  return cloudShadowTex.SampleLevel(cloudShadowSmp, uv, 0); }\n"
    // Normal from depth: per axis take the neighbour on the same surface (smaller step).
    "float3 NormalAt(int2 p, float3 P, float3 V) {\n"
    "  float3 L = WorldAt(p - int2(1, 0)), R = WorldAt(p + int2(1, 0));\n"
    "  float3 U = WorldAt(p - int2(0, 1)), D = WorldAt(p + int2(0, 1));\n"
    "  float3 dx = dot(R - P, R - P) < dot(P - L, P - L) ? R - P : P - L;\n"
    "  float3 dy = dot(D - P, D - P) < dot(P - U, P - U) ? D - P : P - U;\n"
    "  float3 N = cross(dy, dx); float nl = dot(N, N);\n"
    "  N = nl > 1e-20 ? N * rsqrt(nl) : normalize(V);\n"
    "  return dot(N, V) < 0 ? -N : N; }\n"
    // Linear view depth (1 / homogeneous w of the unprojection) for depth-aware blurring.
    "float LinearDepth(int2 p) {\n"
    "  float2 ndc = float2((p.x + 0.5) * screen.x * 2 - 1, 1 - (p.y + 0.5) * screen.y * 2);\n"
    "  return 1 / dot(float4(ndc, LoadDepth(p), 1), clipToWorld[3]); }\n"
    // Interleaved gradient noise: per-pixel jitter that the 4x4 blur averages out.
    "float Ign(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }\n";

// Effects pass -> R16G16: x = ambient occlusion (1 = open), y = sun light scattered toward the
// camera along the view ray (light shafts).
const char* kFxMain =
    "float2 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 p = int2(pos.xy);\n"
    "  float d = LoadDepth(p);\n"
    "  bool sky = d >= 0.999999;\n"
    "  float noise = Ign(pos.xy);\n"
    "  float3 cam = Unproject(pos.xy, 0);\n"
    "  float3 P = Unproject(pos.xy, sky ? 0.99999 : d);\n"
    "  float ao = 1;\n"
    "  if (!sky && fx1.x > 0.5) {\n"
    // Normal-oriented horizon-based AO: 6 directions x 4 steps over a world-space radius.
    "    float3 N = NormalAt(p, P, cam - P);\n"
    "    float wpp = length(Unproject(pos.xy + float2(1, 0), d) - P);\n"
    "    float rpx = clamp(fx0.x / max(wpp, 1e-5), 3, 96);\n"
    "    float occ = 0;\n"
    "    [unroll] for (int k = 0; k < 6; k++) {\n"
    "      float ang = (k + noise) * 1.0471976;\n"
    "      float2 dir = float2(cos(ang), sin(ang));\n"
    "      float best = 0;\n"
    "      [unroll] for (int s = 0; s < 4; s++) {\n"
    "        float2 o = dir * rpx * (s + frac(noise * 3.7 + s * 0.31) + 0.25) / 4;\n"
    "        int2 q = int2(pos.xy + o);\n"
    "        float dq = LoadDepth(q);\n"
    "        if (dq >= 0.999999) continue;\n"
    "        float3 H = Unproject(q + 0.5, dq) - P;\n"
    "        float l2 = dot(H, H);\n"
    "        float h = dot(H, N) * rsqrt(max(l2, 1e-8));\n"
    "        best = max(best, (h - 0.15) * saturate(1 - l2 / (fx0.x * fx0.x)));\n"
    "      }\n"
    "      occ += saturate(best);\n"
    "    }\n"
    "    ao = 1 - saturate(occ / 6 * fx0.y);\n"
    "  }\n"
    "  float shafts = 0;\n"
    // March from the camera to the surface (or a fixed reach for the sky) through what shades the
    // air: the sun's shadow map (trees, hills; only while it is cast along the true light, fx1.w,
    // and within its distance) and the cloud shadow map. Lit air scatters light toward the camera
    // (Henyey-Greenstein phase), so the shafts exist in the world, seen from any angle.
    "  bool useMap = misc.w > 0.5 && (fx1.w > 0.5 || cloud6.w < 0.5), useClouds = cloud6.w > 0.5;\n"
    "  if (fx1.y > 0.5 && (useMap || useClouds)) {\n"
    "    float3 V = P - cam; float len = length(V); float3 dir = V / max(len, 1e-5);\n"
    "    float reach = useClouds ? 4000 : misc.y;\n"
    "    float tmax = min(sky ? reach : len, reach);\n"
    "    float lit = 0;\n"
    "    [loop] for (int i = 0; i < 24; i++) {\n"
    "      float s = (i + noise) / 24 * tmax;\n"
    "      float3 Xw = cam + dir * s;\n"
    "      float v = 1;\n"
    "      if (useMap && s < misc.y) {\n"
    "        float4 X = float4(Xw, 1);\n"
    "        float3 q = float3(dot(X, lightRows[0]), dot(X, lightRows[1]), dot(X, lightRows[2]));\n"
    "        v = shadowMap.SampleCmpLevelZero(cmp, q.xy * float2(0.5, -0.5) + 0.5, q.z - params.z);\n"
    "      }\n"
    "      if (useClouds) v *= CloudShadowAt(Xw);\n"
    "      lit += v;\n"
    "    }\n"
    "    lit /= 24;\n"
    "    float g = fx1.z, cosT = dot(dir, useClouds ? -cloud4.xyz : -sunDir.xyz);\n"
    "    float phase = (1 - g * g) / pow(max(1 + g * g + 2 * g * cosT, 1e-4), 1.5) * 0.25;\n"
    "    shafts = lit * phase * (useClouds ? 1 - exp(-tmax / 2500) : tmax / misc.y);\n"
    "  }\n"
    "  return float2(ao, shafts); }\n";

// Volumetric clouds, half resolution: a layer between cloud0.z and cloud0.w above the sea, ray
// marched from the camera on texels whose 4x4 full-resolution block shows sky (the resolve samples
// them bilinearly there). Output: scattered light (premultiplied) and transmittance.
// Where clouds are and how tall they grow comes from a large 2D weather pattern (clear gaps,
// groups of towers); the 3D noise (r, two scales that never line up) carves their shape and its
// finer channel (g) erodes the edges up close. Light: the key (sun or moon) through a short march
// toward it, a two-lobe phase (silver lining toward the light), sky/ground ambient by height.
// Far away the clouds take the colour of the haze instead of turning transparent.
const char* kCloudCommon =
    "Texture3D<float2> cloudNoise : register(t5);\n"
    "SamplerState cloudWrap : register(s1);\n"
    "float CHash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float CNoise(float2 p) { float2 i = floor(p), f = frac(p); f = f * f * (3 - 2 * f);\n"
    "  return lerp(lerp(CHash(i), CHash(i + float2(1, 0)), f.x), lerp(CHash(i + float2(0, 1)), CHash(i + 1), f.x), f.y); }\n"
    // x: where clouds are (0..1), y: how tall they grow there (share of the layer).
    "float2 CloudWeather(float2 q) {\n"
    "  float n = CNoise(q / 3200) * 0.5 + CNoise(q / 1300 + 11.3) * 0.3 + CNoise(q / 560 - 5.1) * 0.2;\n"
    "  float c = 1 - cloud0.x;\n"
    "  float there = smoothstep(c - 0.12, c + 0.12, n);\n"
    "  float tall = saturate(0.25 + 0.75 * smoothstep(c, c + 0.35, n) + (CNoise(q / 900 + 3.7) - 0.5) * 0.4);\n"
    "  return float2(there, tall); }\n"
    "float CloudDensity(float3 p, float erode) {\n" // p: x, height above the sea, z (drift applied)
    "  float h = (p.y - cloud0.z) / (cloud0.w - cloud0.z);\n"
    "  if (h < 0 || h > 1) return 0;\n"
    "  float2 w = CloudWeather(p.xz);\n"
    "  if (w.x <= 0.01) return 0;\n"
    "  float r = h / max(w.y, 0.05);\n" // height within this cloud's own top
    "  float shape = saturate(r * 7) * saturate((1 - r) * 2.5);\n"
    "  if (shape <= 0) return 0;\n"
    "  float base = cloudNoise.SampleLevel(cloudWrap, float3(p.x, p.y * 1.4, p.z) / 1800, 0).r * 0.62\n"
    "             + cloudNoise.SampleLevel(cloudWrap, float3(p.z + 517, p.y * 1.4, -p.x) / 780, 0).r * 0.38;\n"
    "  float cov = w.x * shape;\n"
    "  float d = saturate((base - (1 - cov)) / max(cov, 0.05));\n"
    "  if (erode > 0 && d > 0) {\n"
    "    float e = cloudNoise.SampleLevel(cloudWrap, p / 380 + cloud5.y * float3(0.004, 0.002, 0.003), 0).g;\n"
    "    d = saturate(d - (1 - e) * 0.35 * erode * (1 - d));\n"
    "  }\n"
    "  return d; }\n"
    "float HG(float c, float g) { float g2 = g * g; return (1 - g2) / pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5); }\n";
const char* kCloudMain =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 full = int2(pos.xy) * 2, size = int2(1 / screen.xy) - 1;\n"
    "  bool sky = false;\n"
    "  [unroll] for (int k = 0; k < 16; k++) sky = sky || LoadDepth(clamp(full + int2(k & 3, k >> 2) - 1, 0, size)) >= 0.999999;\n"
    "  float2 px = pos.xy * 2;\n"
    "  float3 o = Unproject(px, 0);\n"
    // Far world texels too: the resolve fades them into the sky and its clouds (horizon haze).
    "  if (!sky) {\n"
    "    float far = length(Unproject(px, 1) - o);\n"
    "    if (cloud5.z >= 1 || length(Unproject(px, LoadDepth(clamp(full, 0, size))) - o) < far * cloud5.z * 0.9) return float4(0, 0, 0, 1);\n"
    "  }\n"
    "  float3 dir = normalize(Unproject(px, 0.99999) - o);\n"
    "  float up = -dir.y, alt = -o.y;\n" // the world is Y-down
    // The layer follows a curved planet (radius 250000): it bends down to the horizon and meets
    // the sea far away, like real cloud decks, instead of ending above a clear band.
    "  const float bend = 2e-6;\n" // 1 / (2 x radius): height above the sea gains t^2 * bend
    "  float b0 = cloud0.z - alt, b1 = cloud0.w - alt;\n"
    "  if (b1 <= 0) return float4(0, 0, 0, 1);\n"
    "  float q0 = sqrt(up * up + 4 * bend * max(b0, 0)), q1 = sqrt(up * up + 4 * bend * b1);\n"
    "  float t0 = b0 <= 0 ? 0 : up >= 0 ? 2 * b0 / (up + q0) : (q0 - up) / (2 * bend);\n"
    "  float t1 = up >= 0 ? 2 * b1 / (up + q1) : (q1 - up) / (2 * bend);\n"
    "  if (t1 <= t0 || t0 > 120000) return float4(0, 0, 0, 1);\n"
    // The curved sea hides what lies past the horizon: a ray turned down meets the water (height 0
    // on the same planet) before the deck's far side, so no cloud shows through the sea.
    "  if (up < 0) {\n"
    "    float disc = up * up - 4 * bend * alt;\n"
    "    if (disc >= 0) {\n"
    "      float ts = 2 * alt / (-up + sqrt(disc));\n"
    "      if (ts <= t0) return float4(0, 0, 0, 1);\n"
    "      t1 = min(t1, ts);\n"
    "    }\n"
    "  }\n"
    // Steps of at most 160 units: long grazing paths stop early (the haze hides them).
    "  const int N = 56;\n"
    "  float dt = min((t1 - t0) / N, 160);\n"
    "  float jitter = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))) + cloud5.x);\n"
    "  float3 L = cloud4.xyz, Lp = float3(L.x, -L.y, L.z);\n"
    "  float c = dot(dir, L);\n"
    // Two lobes (back glow, silver lining) and a narrow one: the moon's corona through thin cloud.
    "  float phase = lerp(HG(c, -0.2), HG(c, 0.7), 0.55) + HG(c, 0.95) * 0.08;\n"
    "  float sigma = cloud0.y;\n"
    "  float2 drift = float2(cloud2.w, cloud3.w);\n"
    "  float erode = saturate(1 - t0 / 25000);\n"
    "  float3 col = 0; float T = 1;\n"
    "  [loop] for (int i = 0; i < N; i++) {\n"
    "    float t = t0 + (i + jitter) * dt;\n"
    "    float3 w = o + dir * t;\n"
    "    float3 p = float3(w.x + drift.x, alt + t * up + t * t * bend, w.z + drift.y);\n"
    "    float d = CloudDensity(p, erode);\n"
    "    if (d <= 0.002) continue;\n"
    "    float od = 0, ls = 40;\n"
    "    [unroll] for (int j = 0; j < 4; j++) { od += CloudDensity(p + Lp * ls, 0) * ls; ls *= 2.2; }\n"
    // Beer's law, with a softer second term for light scattered many times inside.
    "    float beer = max(exp(-od * sigma), exp(-od * sigma * 0.25) * 0.3);\n"
    "    float powder = 1 - exp(-d * sigma * 120);\n"
    "    float h = saturate((p.y - cloud0.z) / (cloud0.w - cloud0.z));\n"
    "    float3 S = cloud1.rgb * beer * phase * lerp(0.5, 1, powder) + lerp(cloud3.rgb, cloud2.rgb, saturate(h * 1.5));\n"
    "    float Ts = exp(-d * sigma * dt);\n"
    "    col += T * S * (1 - Ts);\n"
    "    T *= Ts;\n"
    "    if (T < 0.02) break;\n"
    "  }\n"
    // Aerial perspective: far clouds fade into the colour of the sky near the horizon.
    "  float a = 1 - T;\n"
    "  float haze = 1 - exp(-t0 / 30000);\n"
    "  col = lerp(col, cloud2.rgb * 1.15 * a, haze * 0.85);\n"
    // Toward the sea's horizon (where a ray grazes the curved sea) the clouds lie ever further
    // behind ever more air: they fade into the sky over the last 2.3 degrees instead of ending
    // in a hard line.
    "  float grazing = -2 * sqrt(max(alt, 0) * bend);\n"
    "  float fade = saturate((120000 - t0) / 30000) * smoothstep(grazing, grazing + 0.04, up);\n"
    "  return float4(col * fade, 1 - a * fade); }\n";

// Tileable n^3 cloud noise for kCloudMain: r = shape (value noise carved by Worley cells),
// g = finer Worley cells for eroding the edges. Each channel stretched to 0..1.
std::vector<BYTE> MakeCloudNoise(int n)
{
    auto hash = [](int x, int y, int z, int s) {
        uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ (uint32_t)s * 2654435761u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return (h & 0xffffff) / 16777216.0f;
    };
    auto wrap = [](int v, int m) { return ((v % m) + m) % m; };
    auto worley = [&](float x, float y, float z, int cells, int seed) {
        const float px = x * cells, py = y * cells, pz = z * cells;
        const int cx = (int)std::floor(px), cy = (int)std::floor(py), cz = (int)std::floor(pz);
        float best = 9;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int gx = cx + dx, gy = cy + dy, gz = cz + dz;
                    const int wx = wrap(gx, cells), wy = wrap(gy, cells), wz = wrap(gz, cells);
                    const float fx = gx + hash(wx, wy, wz, seed) - px, fy = gy + hash(wx, wy, wz, seed + 17) - py,
                                fz = gz + hash(wx, wy, wz, seed + 31) - pz;
                    const float d2 = fx * fx + fy * fy + fz * fz;
                    if (d2 < best) best = d2;
                }
        const float d = std::sqrt(best);
        return 1 - (d < 1 ? d : 1);
    };
    auto value = [&](float x, float y, float z, int cells, int seed) {
        const float px = x * cells, py = y * cells, pz = z * cells;
        const int ix = (int)std::floor(px), iy = (int)std::floor(py), iz = (int)std::floor(pz);
        auto s = [](float t) { return t * t * (3 - 2 * t); };
        const float fx = s(px - ix), fy = s(py - iy), fz = s(pz - iz);
        auto at = [&](int a, int b, int c) { return hash(wrap(a, cells), wrap(b, cells), wrap(c, cells), seed); };
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        return lerp(lerp(lerp(at(ix, iy, iz), at(ix + 1, iy, iz), fx), lerp(at(ix, iy + 1, iz), at(ix + 1, iy + 1, iz), fx), fy),
                    lerp(lerp(at(ix, iy, iz + 1), at(ix + 1, iy, iz + 1), fx), lerp(at(ix, iy + 1, iz + 1), at(ix + 1, iy + 1, iz + 1), fx), fy), fz);
    };
    std::vector<float> shape((size_t)n * n * n), detail(shape.size());
    for (int z = 0; z < n; z++)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                const float u = (x + 0.5f) / n, v = (y + 0.5f) / n, w = (z + 0.5f) / n;
                const float wf = worley(u, v, w, 4, 1) * 0.625f + worley(u, v, w, 8, 2) * 0.25f + worley(u, v, w, 16, 3) * 0.125f;
                const float vf = (value(u, v, w, 4, 4) * 0.5f + value(u, v, w, 8, 5) * 0.25f + value(u, v, w, 16, 6) * 0.125f +
                                  value(u, v, w, 32, 7) * 0.0625f) / 0.9375f;
                const size_t i = ((size_t)z * n + y) * n + x;
                shape[i] = vf * (0.35f + 0.65f * wf); // value noise carved by the cells
                detail[i] = worley(u, v, w, 8, 8) * 0.625f + worley(u, v, w, 16, 9) * 0.25f + worley(u, v, w, 32, 10) * 0.125f;
            }
    auto stretch = [](std::vector<float>& c) {
        float lo = 1e9f, hi = -1e9f;
        for (float f : c) { lo = f < lo ? f : lo; hi = f > hi ? f : hi; }
        for (float& f : c) f = hi > lo ? (f - lo) / (hi - lo) : 0.0f;
    };
    stretch(shape); stretch(detail);
    std::vector<BYTE> out(shape.size() * 2);
    for (size_t i = 0; i < shape.size(); i++) {
        out[i * 2] = (BYTE)(shape[i] * 255 + 0.5f);
        out[i * 2 + 1] = (BYTE)(detail[i] * 255 + 0.5f);
    }
    return out;
}

// Cloud shadow map: for each ground point (height 0) of a square around the camera, the
// transmittance of the cloud layer along the key light (10 steps through the layer, no erosion).
const char* kCloudShadowMain =
    "float main(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 g = cloud6.xy + pos.xy * (cloud6.z / 512);\n"
    "  float up = -cloud4.y;\n"
    "  if (up < 0.03) return 1;\n"
    "  float2 slope = cloud4.xz / up;\n" // ground offset per unit of height toward the light
    "  float2 drift = float2(cloud2.w, cloud3.w);\n"
    "  const int N = 10;\n"
    "  float dh = (cloud0.w - cloud0.z) / N, od = 0;\n"
    "  [loop] for (int i = 0; i < N; i++) {\n"
    "    float hh = cloud0.z + (i + 0.5) * dh;\n"
    "    float2 xz = g + slope * hh + drift;\n"
    "    od += CloudDensity(float3(xz.x, hh, xz.y), 0) * dh / up;\n"
    "  }\n"
    "  return lerp(1, exp(-od * cloud0.y), saturate((up - 0.03) / 0.05)); }\n";

// Volumetric fog, half resolution: 32 steps along the view ray (to the nearest surface of the
// 2x2 block, at most 3000 units), through a layer thick low down and thinning with height,
// carved into drifting banks by the cloud noise. Each step is lit by the key light through the
// sun's shadow map (when it is cast along the true light) and the cloud shadow map, with a
// forward-scattering phase, plus the sky's ambient. Output: in-scattered light and transmittance.
const char* kFogMain =
    "Texture2D<float2> fogPool : register(t10);\n"
    "SamplerState fogPoolSmp : register(s3);\n"
    // The pools (kFogPoolMain): fog up to their surface, softened by VolumeHeight.
    "float FogDensity(float3 X) {\n" // X: world, Y-down
    "  float2 uv = (X.xz + 0.5) * vfog4.xy;\n"
    "  if (any(uv < 0) || any(uv > 1) || -X.y > vfog4.z + vfog4.w + 10) return 0;\n"
    "  float2 pool = fogPool.SampleLevel(fogPoolSmp, uv, 0);\n"
    "  if (pool.y <= 0.001) return 0;\n"
    "  float d = vfog0.x * pool.y * saturate((pool.x + X.y) / max(1.1 / vfog0.z * 0.25, 0.5));\n"
    "  if (vfog1.w > 0) {\n"
    "    float3 q = float3(X.x + vfog2.w * 3, -X.y * 2.5, X.z + vfog2.w * 2) / 320;\n"
    "    float n = cloudNoise.SampleLevel(cloudWrap, q, 0).r * 0.7 + cloudNoise.SampleLevel(cloudWrap, q * 2.7 + 0.37, 0).g * 0.3;\n"
    "    d *= lerp(1, saturate(n * 2.2 - 0.6), vfog1.w);\n"
    "  }\n"
    "  return d; }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 full = int2(pos.xy) * 2, size = int2(1 / screen.xy) - 1;\n"
    "  float d = 1;\n"
    "  [unroll] for (int k = 0; k < 4; k++) d = min(d, LoadDepth(clamp(full + int2(k & 1, k >> 1), 0, size)));\n"
    "  float2 px = pos.xy * 2;\n"
    "  float3 o = Unproject(px, 0), P = Unproject(px, min(d, 0.99999));\n"
    "  float3 V = P - o; float len = length(V); float3 dir = V / max(len, 1e-4);\n"
    "  float tmax = min(d >= 0.999999 ? 3000 : len, 3000);\n"
    "  const int N = 32;\n"
    "  float ds = tmax / N;\n"
    "  float jitter = frac(Ign(pos.xy) + cloud5.x);\n"
    "  float phase = lerp(1, HG(dot(dir, vfog3.xyz), 0.6), 0.35);\n"
    "  bool useMap = misc.w > 0.5 && fx1.w > 0.5;\n"
    "  float3 L = 0; float T = 1;\n"
    "  [loop] for (int i = 0; i < N; i++) {\n"
    "    float s = (i + jitter) * ds;\n"
    "    float3 X = o + dir * s;\n"
    "    float rho = FogDensity(X);\n"
    "    if (rho < 1e-6) continue;\n"
    "    float vis = CloudShadowAt(X);\n"
    "    if (useMap && s < misc.y) {\n"
    "      float4 X4 = float4(X, 1);\n"
    "      float3 q = float3(dot(X4, lightRows[0]), dot(X4, lightRows[1]), dot(X4, lightRows[2]));\n"
    "      vis *= shadowMap.SampleCmpLevelZero(cmp, q.xy * float2(0.5, -0.5) + 0.5, q.z - params.z);\n"
    "    }\n"
    "    float3 S = vfog1.rgb * vis * phase * 0.8 + vfog2.rgb;\n"
    "    float Ts = exp(-rho * ds);\n"
    "    L += T * S * (1 - Ts);\n"
    "    T *= Ts;\n"
    "    if (T < 0.01) break;\n"
    "  }\n"
    "  return float4(L, T); }\n";

// Fog pools, one texel per terrain sample: how far the ground lies below its surroundings (the
// average height 25 and 60 units around). Hollows and valleys between hills fill with fog up to a
// little above that; ridges and even slopes get none, nor ground whose surroundings stay below the
// hill height (vfog4.z). Output: the height the fog fills up to, its strength.
const char* kFogPoolMain =
    "Texture2D<float> fogTerrain : register(t9);\n"
    "SamplerState fogTerrainSmp : register(s3);\n"
    "float FogTerrainH(float2 p) { return fogTerrain.SampleLevel(fogTerrainSmp, (p + 0.5) * vfog4.xy, 0); }\n"
    "float2 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = pos.xy - 0.5;\n"
    "  float hc = FogTerrainH(p), around = 0, top = hc;\n"
    "  [unroll] for (int k = 0; k < 8; k++) {\n"
    "    float2 o; sincos(k * 0.785398 + 0.2, o.y, o.x);\n"
    "    float h1 = FogTerrainH(p + o * 25), h2 = FogTerrainH(p + o * 60);\n"
    "    around += h1 + h2; top = max(top, max(h1, h2));\n"
    "  }\n"
    "  around /= 16;\n"
    "  float depth = around - hc;\n"
    "  float strength = smoothstep(vfog4.z, vfog4.z + vfog4.w * 0.35, top) * smoothstep(0.2, 2.5, depth);\n"
    "  return float2(hc + max(depth, 0) * 1.3 + 1, strength); }\n";

// God rays, half resolution: from each texel, 48 steps toward the light's place on screen
// (rays0.xy), summing the sky that shows there (sky depth, times the clouds' transmittance),
// brightest near the light and decaying along the way.
const char* kRaysMain =
    "Texture2D<float4> cloudTex : register(t4);\n"
    "SamplerState linearClamp : register(s1);\n"
    "float RaySource(float2 uv, int2 size) {\n"
    "  if (any(uv < 0) || any(uv > 1)) return 0;\n"
    "  if (LoadDepth(clamp(int2(uv / screen.xy), 0, size)) < 0.999999) return 0;\n"
    "  float t = cloud1.w > 0.5 ? cloudTex.SampleLevel(linearClamp, uv, 0).a : 1;\n"
    "  float2 r = (uv - rays0.xy) * float2(screen.y / screen.x, 1);\n"
    "  return t * exp(-dot(r, r) * 18);\n"
    "}\n"
    "float main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 size = int2(1 / screen.xy) - 1;\n"
    "  float2 uv = pos.xy * 2 * screen.xy;\n"
    "  const int N = 48;\n"
    "  float2 step = (rays0.xy - uv) / N;\n"
    "  float2 s = uv + step * Ign(pos.xy + cloud5.x * 64);\n"
    "  float sum = 0, w = 1;\n"
    "  [loop] for (int i = 0; i < N; i++) { sum += RaySource(s, size) * w; w *= 0.97; s += step; }\n"
    "  return sum / N * rays0.w; }\n";

// The time-of-day sky, shared by the resolve and the sky the water reflects (kSkyEnvMain).
const char* kSkyCommon =
    // Time-of-day sky (night and dusk; the game's sky texture is kept by day). Y-down world.
    "float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "float VNoise(float2 p) { float2 i = floor(p), f = frac(p); f = f * f * (3 - 2 * f);\n"
    "  return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + 1), f.x), f.y); }\n"
    // The sky turns with the hour around the pole of the sun's arc (sun.cpp EvaluateTimeOfDay:
    // Y-up axis (0, cos 70, -sin 70)): a world direction to the fixed star frame.
    "float3 StarDir(float3 dir) {\n"
    "  float3 u = float3(dir.x, -dir.y, dir.z), a = float3(0, 0.34202, -0.93969);\n"
    "  float s, c; sincos(tod7.x, s, c);\n"
    "  return u * c + cross(a, u) * s + a * dot(a, u) * (1 - c); }\n"
    "float3 ProceduralSkyGlow(float3 dir, float glow) {\n"
    "  float el = -dir.y, sunEl = -tod2.y;\n"
    "  float moonSky = tod7.y * (0.15 + 0.85 * tod6.w);\n" // the moon's share of the night light
    "  float3 night = lerp(float3(0.055, 0.075, 0.12), float3(0.012, 0.02, 0.045), saturate(el * 1.6)) * tod4.x * (0.55 + 0.45 * moonSky);\n"
    "  float3 sd3 = StarDir(dir); float lat = asin(clamp(sd3.y, -1, 1));\n"
    "  float2 sc = float2(atan2(sd3.z, sd3.x), lat) * 600, cell = floor(sc);\n"
    "  float h = Hash(cell);\n"
    "  float2 at = float2(Hash(cell + 3.1), Hash(cell + 5.7)) * 0.6 + 0.2;\n"
    // Cells narrow toward the pole: fewer stars there keeps the density even.
    "  float star = step(1 - 0.006 * max(cos(lat), 0.05), h) * saturate(1.3 - length(frac(sc) - at) * 2.6);\n"
    // A bright moon washes out the faint stars.
    "  night += saturate(star) * saturate(el * 5) * (0.4 + 1.2 * Hash(cell + 7.3)) * float3(0.9, 0.95, 1) * tod4.y * (1 - 0.6 * tod6.w * tod7.y);\n"
    "  float m = saturate(dot(dir, tod6.xyz));\n"
    "  night += tod3.x * tod6.w * saturate(el * 20 + 1) * (pow(m, 600) * float3(0.55, 0.62, 0.8) * 0.6 + pow(m, 24) * float3(0.10, 0.13, 0.20));\n"
    "  float2 flat = dir.xz / max(length(dir.xz), 1e-4), sunFlat = tod2.xz / max(length(tod2.xz), 1e-4);\n"
    "  float toward = saturate(dot(flat, sunFlat) * 0.5 + 0.5);\n"
    "  float3 horizon = lerp(float3(0.55, 0.35, 0.38), float3(1.0, 0.52, 0.22), toward);\n"
    "  float3 dusk = lerp(horizon, float3(0.22, 0.32, 0.58), saturate(el * 2.2));\n"
    "  dusk += float3(1.0, 0.6, 0.3) * pow(saturate(dot(dir, tod2.xyz)), 200) * 2 * glow;\n"
    "  float duskW = saturate(1 - abs(sunEl + 0.02) * 5);\n" // around sunrise and sunset
    "  float3 haze = float3(0.74, 0.83, 0.93);\n"
    "  float3 day = el >= 0 ? lerp(haze, float3(0.30, 0.52, 0.86), pow(saturate(el), 0.45))\n"
    "                       : lerp(haze, float3(0.55, 0.60, 0.62), saturate(-el * 4));\n"
    "  float sd = saturate(dot(dir, tod2.xyz));\n"
    "  day += float3(1.0, 0.95, 0.85) * pow(sd, 12) * 0.18 * glow;\n"
    "  return lerp(lerp(night, dusk, duskW), day, tod4.z); }\n"
    "float3 ProceduralSky(float3 dir) { return ProceduralSkyGlow(dir, 1); }\n";

// The sky the water reflects: the upper hemisphere into 128x64 (u: azimuth, v: from the zenith
// down to the horizon), read by next frame's water (shaders.cpp ApplyWater).
const char* kSkyEnvMain =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  float az = (pos.x / 128 - 0.5) * 6.2831853, th = pos.y / 64 * 1.5707963;\n"
    "  float3 d = float3(sin(th) * cos(az), -cos(th), sin(th) * sin(az));\n"
    "  return float4(ProceduralSky(d), 1); }\n";

const char* kResolveMain =
    "Texture2D<float4> sceneColor : register(t2);\n"
    "Texture2D<float2> fxTex : register(t3);\n"
    "Texture2D<float4> cloudTex : register(t4);\n"
    "SamplerState linearClamp : register(s1);\n"
    "Texture2D<float> raysTex : register(t6);\n"
    "Texture2D<float4> fogTex : register(t8);\n"
    "static const float2 kTaps[12] = { float2(-0.326,-0.406), float2(-0.840,-0.074), float2(-0.696,0.457),\n"
    "  float2(-0.203,0.621), float2(0.962,-0.195), float2(0.473,-0.480), float2(0.519,0.767),\n"
    "  float2(0.185,-0.893), float2(0.507,0.064), float2(0.896,0.412), float2(-0.322,-0.933),\n"
    "  float2(-0.792,-0.598) };\n"
    // Soft shoulder above 0.8 so boosted highlights roll off instead of clipping.
    "float3 Shoulder(float3 x) { return x <= 0.8 ? x : 0.8 + 0.2 * (1 - exp(-(x - 0.8) / 0.2)); }\n"
    // 4x4 depth-aware average of the effects texture (removes the interleaved jitter).
    "float2 BlurFx(int2 p) {\n"
    "  float zc = LinearDepth(p); float2 sum = 0; float wsum = 0;\n"
    "  [unroll] for (int y = -2; y < 2; y++) [unroll] for (int x = -2; x < 2; x++) {\n"
    "    int2 q = p + int2(x, y);\n"
    "    float w = 1 / (0.02 + abs(LinearDepth(q) - zc) / max(zc, 1e-4) * 40);\n"
    "    sum += fxTex.Load(int3(q, 0)) * w; wsum += w;\n"
    "  }\n"
    "  return sum / wsum; }\n"
    // Dark seas of the moon's face (q: position on the disc, radius 1).
    "float MoonAlbedo(float2 q) {\n"
    "  float m = smoothstep(0.42, 0.05, length(q - float2(-0.28, -0.22))) + smoothstep(0.30, 0.04, length(q - float2(0.18, -0.30)))\n"
    "    + smoothstep(0.34, 0.05, length(q - float2(-0.05, 0.25))) + smoothstep(0.22, 0.03, length(q - float2(0.40, 0.10)));\n"
    "  m += (VNoise(q * 6) - 0.5) * 0.4 + (VNoise(q * 14 + 3) - 0.5) * 0.2;\n"
    "  return 1 - 0.32 * saturate(m); }\n"
    // The sun's and the moon's discs, by day over the game's sky texture too.
    "float3 SkyBodies(float3 c, float3 dir) {\n"
    "  float horizon = smoothstep(-0.01, 0.005, -dir.y);\n"
    // Sun: 0.9 degrees, darker toward the limb, orange near the horizon.
    "  float sd = dot(dir, tod2.xyz);\n"
    "  if (sd > 0.9997) {\n"
    "    float r = sqrt(saturate(1 - sd * sd)) / 0.0157;\n"
    "    float disc = saturate((1 - r) * 25) * horizon * tod7.z;\n"
    "    float3 sunCol = lerp(float3(1.0, 0.45, 0.18), float3(1.0, 0.96, 0.88), saturate(-tod2.y * 5));\n"
    "    c = lerp(c, sunCol * (0.55 + 0.45 * sqrt(saturate(1 - r * r))) * 1.6, saturate(disc));\n"
    "  }\n"
    // Moon: 1.25 degrees, a sphere lit by the sun, so its phase follows from where the two are.
    "  float3 md = tod6.xyz;\n"
    "  float cm = dot(dir, md);\n"
    "  if (cm > 0.999) {\n"
    "    float3 e1 = abs(md.y) > 0.999 ? float3(1, 0, 0) : normalize(cross(float3(0, -1, 0), md));\n"
    "    float3 e2 = cross(md, e1);\n"
    "    float2 q = float2(dot(dir, e1), dot(dir, e2)) / 0.022;\n"
    "    float r2 = dot(q, q);\n"
    "    if (r2 < 1.2) {\n"
    "      float cover = saturate((1 - sqrt(r2)) * 25) * horizon;\n"
    "      float3 n = e1 * q.x + e2 * q.y - md * sqrt(saturate(1 - r2));\n"
    "      float lit = smoothstep(-0.04, 0.12, dot(n, tod2.xyz));\n"
    "      float3 albedo = float3(0.97, 0.96, 0.92) * MoonAlbedo(q);\n"
    // Night: the disc hides the stars, its dark side a shade darker than the sky and faintly lit by
    // the Earth (a black half reads as a hole in the sky). Day: only its lit
    // side, pale over the blue.
    "      float3 nightC = lerp(c, c * 0.8 + albedo * (lit * 1.5 + 0.02), cover);\n"
    "      float3 dayC = c + albedo * lit * 0.45 * cover;\n"
    "      c = lerp(dayC, nightC, tod3.x);\n"
    "    }\n"
    "  }\n"
    "  return c; }\n"
    // Distant islands: eroded mountain height fields (islands.cpp) placed far out at sea around
    // the park (isleA: centre x, z, radius, peak height; isleB: rotation cos, sin, shape), raymarched
    // from the camera on the sky past the sea's edge. Lit by the key light (sun or moon) with their
    // own shadows, and hazed by the air toward the horizon sky.
    "Texture2DArray<float4> isleTex : register(t11);\n"
    "Texture3D<float2> isleNoise : register(t5);\n"
    "SamplerState isleWrap : register(s4);\n"
    "float4 IsleAt(int i, float2 xz, float lod) {\n"
    "  float2 l = (xz - isleA[i].xy) / isleA[i].z;\n"
    "  float2 r = float2(l.x * isleB[i].x - l.y * isleB[i].y, l.x * isleB[i].y + l.y * isleB[i].x);\n"
    "  return isleTex.SampleLevel(linearClamp, float3(r * 0.5 + 0.5, isleB[i].z), lod); }\n"
    // Now and then a big cloud drifting past an island (sparse cumulus from a fifth of its height to
    // a third above its peak, within 1.8 radii of it): in front of the mountain where the ray crosses
    // it first, so ridges disappear behind it and show again. el: the ray's rise, from sea level.
    "float3 IsleClouds(float3 c, float3 dir, float3 o, float el, int i, float ta, float tb, float3 air) {\n"
    "  if (i < 0 || isle2.w <= 0 || tb <= ta || el <= 0) return c;\n"
    "  float4 M = isleA[i];\n"
    "  const int K = 24;\n"
    "  float dt = (tb - ta) / K, T = 1; float3 Lm = 0;\n"
    "  [loop] for (int k = 0; k < K; k++) {\n"
    "    float t = ta + dt * (k + 0.5), y = el * t / M.w;\n"
    "    float2 X = o.xz + dir.xz * t;\n"
    "    float band = smoothstep(0.2, 0.45, y) * smoothstep(1.35, 0.95, y) * smoothstep(1.8, 1.1, length(X - M.xy) / M.z);\n"
    "    if (band <= 0) continue;\n"
    "    float3 q = float3(X.x + isle3.w * 14, el * t * 1.4, X.y + isle3.w * 6) / (M.z * 1.4);\n"
    "    float n = isleNoise.SampleLevel(isleWrap, q, 0).r * 0.75 + isleNoise.SampleLevel(isleWrap, q * 3.1 + 0.31, 0).g * 0.25;\n"
    "    float rho = saturate((n - 0.8 + band * isle2.w * 0.3) * 4) * 8 / M.z;\n"
    "    if (rho <= 0) continue;\n"
    "    float Ts = exp(-rho * dt);\n"
    // Lit tops, greyer bases, like the clouds of the sky.
    "    float3 S = isle1.rgb * lerp(0.45, 1.1, saturate(y - 0.3)) + isle3.rgb * 0.7;\n"
    "    Lm += T * S * (1 - Ts);\n"
    "    T *= Ts;\n"
    "    if (T < 0.02) break;\n"
    "  }\n"
    "  float haze = 1 - exp(-ta * isle0.z * 3e-4);\n"
    "  return c * T + lerp(Lm, air * (1 - T), haze); }"
    "float3 DistantIslands(float3 c, float3 dir, float3 o, float pixAng, float far) {\n"
    "  float3 air = ProceduralSkyGlow(normalize(float3(dir.x, -0.008, dir.z)), 0.3);\n"
    // Below the horizon the sky shows past the end of the game's sea: the haze its edge fades into.
    "  if (dir.y > 0) c = air;\n"
    // The game's sea ends at the far plane, below the true horizon: the islands stand on that edge
    // (rays tilted up by its depression, from sea level), not on a band of haze beyond it.
    "  float el = -dir.y + max(-o.y, 0) / max(far, 1), hc = 0;\n"
    "  if (el > isle1.w) return c;\n"
    "  float tSea = el < 0 ? 0 : 1e9;\n"
    "  float best = 1e9; int hit = -1;\n"
    // The island whose air the ray enters first, for the clouds passing it.
    "  float mt0 = 1e9, mt1 = 0; int mi = -1;\n"
    "  [loop] for (int i = 0; i < 8; i++) {\n"
    "    float4 A = isleA[i];\n"
    "    if (A.z <= 0) continue;\n"
    "    float2 oc = o.xz - A.xy, d2 = dir.xz;\n"
    "    float a = dot(d2, d2), b = dot(oc, d2), cc = dot(oc, oc) - A.z * A.z;\n"
    "    float cr = A.z * 1.8, cdisc = b * b - a * (dot(oc, oc) - cr * cr);\n"
    "    if (cdisc > 0 && a > 1e-8) {\n"
    "      float ct0 = max((-b - sqrt(cdisc)) / a, 0), ct1 = min((-b + sqrt(cdisc)) / a, tSea);\n"
    "      if (ct0 < mt0 && ct1 > ct0) { mt0 = ct0; mt1 = ct1; mi = i; }\n"
    "    }\n"
    "    float disc = b * b - a * cc;\n"
    "    if (disc <= 0 || a < 1e-8) continue;\n"
    "    float sq = sqrt(disc), t0 = max((-b - sq) / a, 0), t1 = min((-b + sq) / a, min(tSea, best));\n"
    // Only where the ray is below the peak.
    "    if (el > 0) t1 = min(t1, (A.w - hc) / el); else if (hc > A.w) t0 = max(t0, (hc - A.w) / -el);\n"
    "    if (t1 <= t0) continue;\n"
    "    const int S = 72;\n"
    "    float dt = (t1 - t0) / S, texel = 2 * A.z / 512;\n"
    "    float tp = t0, fp = hc + el * t0 - A.w * IsleAt(i, o.xz + d2 * t0, 0).x;\n"
    "    [loop] for (int s = 1; s <= S; s++) {\n"
    "      float t = t0 + dt * s, lod = log2(max(t * pixAng / texel, 1));\n"
    "      float f = hc + el * t - A.w * IsleAt(i, o.xz + d2 * t, lod).x;\n"
    "      if (f < 0) {\n"
    "        float ta = tp, tb = t;\n"
    "        [unroll] for (int k = 0; k < 6; k++) {\n"
    "          float tm = (ta + tb) * 0.5;\n"
    "          if (hc + el * tm - A.w * IsleAt(i, o.xz + d2 * tm, lod).x < 0) tb = tm; else ta = tm;\n"
    "        }\n"
    "        best = tb; hit = i; break;\n"
    "      }\n"
    "      tp = t; fp = f;\n"
    "    }\n"
    "  }\n"
    "  if (hit < 0) return IsleClouds(c, dir, o, el, mi, mt0, min(mt1, best), air);\n"
    "  float4 A = isleA[hit], B = isleB[hit];\n"
    "  float3 P = o + dir * best;\n"
    "  float lod = log2(max(best * pixAng / (2 * A.z / 512), 1));\n"
    "  float4 s = IsleAt(hit, P.xz, lod);\n"
    // The slope in the world (the texture is rotated and spans two radii), the normal Y-down.
    "  float2 g = float2(B.x * s.y + B.y * s.z, -B.y * s.y + B.x * s.z) * (0.5 * A.w / A.z);\n"
    "  float3 n = normalize(float3(-g.x, -1, -g.y));\n"
    "  float up = -n.y, hw = s.x * A.w;\n"
    "  float3 L = isle2.xyz;\n"
    "  float ndl = saturate(dot(n, L));\n"
    // Shadows: the ground between the point and the key light, across the same island.
    "  float sh = 1;\n"
    "  if (ndl > 0) {\n"
    "    [loop] for (int j = 1; j <= 20; j++) {\n"
    "      float tj = A.z * 0.9 * (j * j) / 400.0;\n"
    "      float above = hw - L.y * tj - A.w * IsleAt(hit, P.xz + L.xz * tj, lod + 1).x;\n"
    "      sh = min(sh, saturate(above / (tj * 0.06) + 0.5));\n"
    "      if (sh <= 0) break;\n"
    "    }\n"
    "  }\n"
    // Rain forest on the slopes (darker and greener in the gullies), bare rock on the cliffs and
    // the high crests, sand at the water.
    "  float3 forest = lerp(float3(0.075, 0.115, 0.050), float3(0.045, 0.085, 0.040), s.w);\n"
    "  float rock = saturate(smoothstep(0.78, 0.55, up) * 0.85 + smoothstep(0.7, 1, s.x) * 0.4);\n"
    "  float3 alb = lerp(forest, float3(0.20, 0.18, 0.15), rock);\n"
    "  alb = lerp(float3(0.36, 0.33, 0.26), alb, saturate(hw / 5));\n"
    "  float3 light = isle0.w * (isle1.rgb * ndl * sh + isle3.rgb * (0.45 + 0.55 * up) * (1 - 0.45 * s.w));\n"
    "  float3 land = alb * light;\n"
    // The air between: thicker the further and the lower (the feet fade more than the crests),
    // toward the horizon sky in that direction.
    "  float haze = 1 - exp(-best * isle0.z * 4.5e-4 * (1.6 - 0.8 * saturate(s.x)));\n"
    "  return IsleClouds(lerp(land, air, haze), dir, o, el, mi, mt0, min(mt1, best), air); }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 p = int2(pos.xy);\n"
    "  float3 color = sceneColor.Load(int3(p, 0)).rgb;\n"
    "  if (misc.x == 5 && misc.w > 0.5 && pos.x < 540 && pos.y < 540) {\n"
    "    uint mw, mh; shadowMap.GetDimensions(mw, mh);\n"
    "    float v = shadowMap.Load(int3(pos.xy / 540 * mw, 0));\n"
    "    return v >= 1 ? float4(0.2, 0.2, 0.6, 1) : float4(frac(v * misc.z / 2).xxx, 1); }\n"
    "  float d = LoadDepth(p);\n"
    "  bool sky = d >= 0.999999;\n"
    "  float2 fx = (fx1.x > 0.5 || fx1.y > 0.5) ? BlurFx(p) : float2(1, 0);\n"
    "  if (misc.x == 9) return float4(fx.xxx, 1);\n"
    "  if (misc.x == 10) return float4(saturate(fx.y * fx0.w * 4).xxx, 1);\n"
    "  float shadow = 0, vis = 1;\n"
    "  if (sky && misc.x >= 3) return float4(1, 0.6, 1, 1);\n"
    "  if (!sky && misc.w > 0.5) {\n"
    "    float3 P = Unproject(pos.xy, d);\n"
    "    float3 V = Unproject(pos.xy, 0) - P;\n"
    "    float dist = length(V);\n"
    "    float3 N = NormalAt(p, P, V);\n"
    "    if (misc.x == 4) return float4(N * 0.5 + 0.5, 1);\n"
    "    if (misc.x == 6) { float2 g = frac(P.xz / 8); return float4(g.x < 0.08 || g.y < 0.08 ? float3(1, 0, 0) : frac(P.yyy / 4), 1); }\n"
    "    float ndl = dot(N, sunDir.xyz);\n"
    // Offset toward the sun: robust where depth normals are noisy (grass, foliage).
    "    float3 Q = P + sunDir.xyz * params.w * 3;\n"
    "    float4 Q4 = float4(Q, 1);\n"
    "    float3 q = float3(dot(Q4, lightRows[0]), dot(Q4, lightRows[1]), dot(Q4, lightRows[2]));\n"
    "    float2 uv = q.xy * float2(0.5, -0.5) + 0.5;\n"
    "    float edge = saturate(min(min(uv.x, 1 - uv.x), min(uv.y, 1 - uv.y)) * 12);\n"
    "    if (edge > 0 && q.z > 0 && q.z < 1) {\n"
    "      float z = q.z - params.z, sum = 0;\n"
    "      [unroll] for (int k = 0; k < 12; k++) sum += shadowMap.SampleCmpLevelZero(cmp, uv + kTaps[k] * params.y * 1.6, z);\n"
    "      vis = sum / 12;\n"
    "    }\n"
    "    if (misc.x == 7) return float4(vis.xxx, 1);\n"
    // Faces turned away from the sun are already dark in the game's own lighting.
    "    float facing = lerp(0.6, 1, saturate(ndl * 4));\n"
    "    float fade = saturate((misc.y - dist) / (misc.y * 0.2));\n"
    "    shadow = (1 - vis) * edge * facing * fade;\n"
    "  }\n"
    "  float3 shade = (1 - params.x * shadow) * lerp(float3(1, 1, 1), tod3.yzw, shadow);\n"
    "  if (misc.x == 3) return float4(shade, 1);\n"
    "  float3 c = color * shade;\n"
    // Ambient occlusion: full in shadow, softer where the sun hits directly.
    "  if (!sky) c *= lerp(1, fx.x, fx0.z * lerp(0.6, 1, saturate(shadow * 2)));\n"
    // Light shafts: warm sun light scattered by the air between camera and surface.
    "  c += (cloud6.w > 0.5 ? cloud1.rgb * 1.5 : float3(1.0, 0.9, 0.72) * tod.w) * fx.y * fx0.w;\n"
    // Night colour fade: per world draw (shaders.cpp NightGrade), so emission keeps its colour.
    "  if (sky) {\n"
    "    c *= tod.rgb;\n"
    "    if (tod2.w > 0.001 || tod7.w > 0.5) {\n"
    "      float3 vd = normalize(Unproject(pos.xy, 0.99999) - Unproject(pos.xy, 0));\n"
    "      if (tod2.w > 0.001) c = lerp(c, ProceduralSky(vd), tod2.w);\n"
    "      if (tod7.w > 0.5) c = SkyBodies(c, vd);\n"
    "      if (cloud1.w > 0.5) { float4 cl = cloudTex.SampleLevel(linearClamp, pos.xy * screen.xy, 0); c = c * cl.a + cl.rgb; }\n"
    "      if (isle0.x > 0.5) {\n"
    "        float3 o = Unproject(pos.xy, 0);\n"
    "        float pixAng = length(normalize(Unproject(pos.xy + float2(0, 1), 0.99999) - Unproject(pos.xy + float2(0, 1), 0)) - vd);\n"
    "        c = DistantIslands(c, vd, o, pixAng, length(Unproject(pos.xy, 1) - o));\n"
    "      }\n"
    "    }\n"
    "  }\n"
    // Atmospheric perspective: the air between the camera and the surface, by distance plus a low
    // fog that thins with height (exponential, integrated along the view ray). Its colour is the sky
    // at the horizon toward the pixel, warmed toward the key light (sun or moon) by forward scattering.
    "  if (!sky && fog0.w > 0.5) {\n"
    "    float3 o = Unproject(pos.xy, 0), P = Unproject(pos.xy, d);\n"
    "    float D = length(P - o); float3 vd = (P - o) / max(D, 1e-4);\n"
    "    float hc = max(-o.y - fog1.x, 0), hp = max(-P.y - fog1.x, 0), b = fog0.z;\n" // Y-down world
    "    float low = abs(hp - hc) > 0.01 ? (exp(-b * hc) - exp(-b * hp)) / (b * (hp - hc)) : exp(-b * hc);\n"
    "    float f = 1 - exp(-D * (fog0.x + fog0.y * low));\n"
    "    float3 hd = normalize(float3(vd.x, min(vd.y, -0.02), vd.z));\n"
    "    float3 air = ProceduralSky(hd);\n"
    "    float k = saturate(dot(vd, fog1.yzw));\n"
    "    air += fog2.rgb * (pow(k, 8) * 0.6 + pow(k, 40) * 0.6);\n"
    "    c = lerp(c, air, f);\n"
    "  }\n"
    // Volumetric fog: what the layer hides and the light it scatters toward the camera.
    "  if (vfog0.w > 0.5) { float4 vf = fogTex.SampleLevel(linearClamp, pos.xy * screen.xy, 0); c = c * vf.a + vf.rgb; }\n"
    // Horizon haze (dusk and night, where the sky is ours): the far world fades into the sky and its
    // clouds in its own direction, so the sea's edge meets the sky instead of the game's flat fog.
    "  if (!sky && tod2.w > 0.001 && cloud5.z < 1) {\n"
    "    float3 o = Unproject(pos.xy, 0), P = Unproject(pos.xy, d);\n"
    "    float far = length(Unproject(pos.xy, 1) - o);\n"
    "    float f = saturate((length(P - o) / far - cloud5.z) / max(1 - cloud5.z, 0.01));\n"
    "    f = f * f * tod2.w;\n"
    "    if (f > 0) {\n"
    "      float3 vd = normalize(P - o); vd.y = min(vd.y, -0.002); vd = normalize(vd);\n"
    // The sun's glow mostly left out: below the sun it would show under a distant island in front of it.
    "      float3 hz = ProceduralSkyGlow(vd, 0.3);\n"
    "      if (cloud1.w > 0.5) { float4 cl = cloudTex.SampleLevel(linearClamp, pos.xy * screen.xy, 0); hz = hz * cl.a + cl.rgb; }\n"
    "      c = lerp(c, hz, f);\n"
    "    }\n"
    "  }\n"
    // God rays: the light scattered toward the camera along the rays the sky shows through.
    "  if (rays1.w > 0.5) c += rays1.rgb * raysTex.SampleLevel(linearClamp, pos.xy * screen.xy, 0);\n"
    "  if (tod5.w > 0) {\n" // filmic curve (ACES fit) over the whole picture
    "    float3 x = pow(max(c, 0), 2.2) * tod5.w * 0.6;\n"
    "    c = pow(saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)), 1 / 2.2);\n"
    "  }\n"
    // Day colour on the world (the sky keeps its own): raise the darks through the luminance
    // (hue kept), brightness, saturation.
    "  if (day0.w > 0 && !sky) {\n"
    "    const float3 kL = float3(0.2126, 0.7152, 0.0722);\n"
    "    float3 dc = c * lerp(1, day0.x, day0.w);\n"
    "    float l = max(dot(dc, kL), 1e-4);\n"
    "    float lt = pow(saturate(l), 1 / (1 + day0.y * day0.w));\n"
    "    dc *= min(lt / l, 3);\n"
    "    c = max(lerp(dot(dc, kL).xxx, dc, lerp(1, day0.z, day0.w)), 0);\n"
    "  }\n"
    "  if (grade1.z > 0.5) {\n"
    // Sunlight: warm lift where the sun reaches (everything outside the shadow), cool shadows.
    "    float lit = sky ? 0 : 1 - shadow;\n"
    "    c *= 1 + grade1.x * lit * tod.w * float3(1.0, 0.93, 0.78);\n"
    "    c = lerp(c, c * float3(0.88, 0.94, 1.1), grade1.y * saturate(shadow * 2));\n"
    "    c *= grade0.x;\n"
    "    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    c = lerp(luma.xxx, c, grade0.z);\n"
    "    c *= float3(1 + 0.08 * grade0.w, 1, 1 - 0.08 * grade0.w);\n"
    "    c = (c - 0.5) * grade0.y + 0.5;\n"
    "    c = Shoulder(max(c, 0));\n"
    "  }\n"
    "  return float4(c, 1); }\n";
void LoadRows(const float (&rows)[4][4], XMMATRIX& out)
{
    // rows[i] are dot-product rows (clip_i = rows[i] . p); as a DirectXMath row-vector matrix
    // that is the transpose.
    XMFLOAT4X4 m;
    memcpy(&m, rows, sizeof(m));
    out = XMMatrixTranspose(XMLoadFloat4x4(&m));
}

void StoreRows(FXMMATRIX m, float (&rows)[4][4])
{
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, XMMatrixTranspose(m));
    memcpy(rows, &f, sizeof(f));
}

} // namespace

bool Device::CreateSunResources()
{
    D3D11_BUFFER_DESC cb = { sizeof(SunConstants), D3D11_USAGE_DYNAMIC,
        D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_sunCB))) return false;
    // Everything but the map itself is small and created up front, so shadows can be switched
    // on from the overlay; the map follows [Lighting] ShadowResolution (EnsureShadowMap).
    cb.ByteWidth = (sizeof(ResolveConstants) + 15) & ~15u;
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_resolveCB))) return false;
    cb.ByteWidth = sizeof(float) * 4 * 33; // Grass: pushers[32], grassParams
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_grassCB))) return false;

    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sm.BorderColor[0] = sm.BorderColor[1] = sm.BorderColor[2] = sm.BorderColor[3] = 1;
    sm.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; sm.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE; // casters above the light box still cast
    raster.SlopeScaledDepthBias = 1.5f; raster.DepthBiasClamp = 0.002f;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(m_device->CreateSamplerState(&sm, &m_sunSampler)) ||
        FAILED(m_device->CreateDepthStencilState(&depth, &m_sunDepth)) ||
        FAILED(m_device->CreateRasterizerState(&raster, &m_sunRaster)) ||
        FAILED(m_device->CreateBlendState(&blend, &m_multiplyBlend))) return false;

    auto vs = CompileHLSL(kResolveVS, "vs_4_0", "sun_resolve_vs");
    if (!vs || FAILED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_resolveVS)))
        return false;
    auto makePS = [&](const char* body, int msaa, const char* name, ComPtr<ID3D11PixelShader>& out) {
        auto blob = CompileHLSL(std::string(msaa ? "#define MSAA 1\n" : "#define MSAA 0\n") + kSceneCommon + body, "ps_4_1", name);
        return blob && SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };
    const std::string resolveBody = std::string(kSkyCommon) + kResolveMain;
    if (!makePS(resolveBody.c_str(), 0, "scene_resolve_ps", m_resolvePS) || !makePS(resolveBody.c_str(), 1, "scene_resolve_ps_ms", m_resolvePSMS) ||
        !makePS(kFxMain, 0, "scene_fx_ps", m_fxPS) || !makePS(kFxMain, 1, "scene_fx_ps_ms", m_fxPSMS))
        return false;
    Log("Sun shadows: enabled %d, %d map, distance %.0f, strength %.2f, sun %s, grass %d, replacement lighting %d, debug %d",
        g_config.sunLighting, g_config.shadowResolution, g_config.shadowDistance, g_config.shadowStrength, g_config.sunFromGame ? "from game" : "fixed",
        g_config.grassShadows, g_config.replaceLighting, g_config.lightingDebug);
    return true;
}

// Called on the first full-screen terrain draw of a frame: c0..c3 is the camera's world-to-clip.
bool Device::EnsureShadowMap()
{
    const UINT size = (UINT)g_config.shadowResolution;
    if (m_sunMap && m_sunMapSize == size) return true;
    m_sunSRV.Reset(); m_sunDSV.Reset(); m_sunMap.Reset();
    m_sunMapSize = 0;
    D3D11_TEXTURE2D_DESC td = { size, size, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sunMap)) ||
        FAILED(m_device->CreateDepthStencilView(m_sunMap.Get(), &dd, &m_sunDSV)) ||
        FAILED(m_device->CreateShaderResourceView(m_sunMap.Get(), &sd, &m_sunSRV))) {
        LogOnce("sun-map-fail", "Shadow map %u creation failed", size);
        m_sunSRV.Reset(); m_sunDSV.Reset(); m_sunMap.Reset();
        return false;
    }
    m_sunMapSize = size;
    Log("Shadow map %ux%u created", size, size);
    return true;
}

// The two shadow cache maps, like the live map (CopyResource copies one into it).
bool Device::EnsureShadowCache()
{
    if (!m_sunMap) return false;
    if (m_sunCacheMap[0] && m_sunCacheMap[1] && m_sunCacheSize == m_sunMapSize) return true;
    m_sunCacheReady = m_sunCacheBuild = -1;
    m_sunCacheSize = 0;
    D3D11_TEXTURE2D_DESC td;
    m_sunMap->GetDesc(&td);
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    for (int k = 0; k < 2; k++) {
        m_sunCacheDSV[k].Reset(); m_sunCacheMap[k].Reset();
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sunCacheMap[k])) ||
            FAILED(m_device->CreateDepthStencilView(m_sunCacheMap[k].Get(), &dd, &m_sunCacheDSV[k]))) {
            Log("Shadow cache: maps failed, disabled");
            m_sunCacheDSV[0].Reset(); m_sunCacheMap[0].Reset(); m_sunCacheDSV[1].Reset(); m_sunCacheMap[1].Reset();
            g_config.shadowCache = false;
            return false;
        }
    }
    m_sunCacheSize = m_sunMapSize;
    Log("Shadow cache: 2 maps of %ux%u, %d frames per build, live within %.0f units", m_sunMapSize, m_sunMapSize,
        g_config.shadowCacheFrames, g_config.shadowCacheNear);
    return true;
}

// Where a batched caster goes while the shadow cache is on. Animals and casters other than
// skinned meshes are live (drawn every frame). The others go into the cache map being built in
// their mesh's frame of the cycle (all draws of one mesh the same frame, so every object of it is
// in), and into the live map too when the cache did not serve this frame or one of the objects is
// near the camera (just come into view, or swaying close up).
BYTE Device::ShadowTargets(const float* instanceConsts, UINT instances, const UINT* instanceInfo, const ShadowDraw& d, int bones)
{
    if (m_worldKind != 1 || bones < 0 || bones >= g_config.grassMinBones) { ++m_cacheLive; return kShadowLive; }
    UINT64 key = (UINT64)(uintptr_t)d.vb[0] * 0x9E3779B97F4A7C15ull ^ (UINT64)(uintptr_t)d.ib * 0xC2B2AE3D27D4EB4Full ^
                 ((UINT64)d.start << 20) ^ d.count ^ ((UINT64)(UINT)d.base << 40);
    key ^= key >> 31;
    BYTE targets = 0;
    if (m_sunCacheBuild >= 0 && (UINT)(key % (UINT64)g_config.shadowCacheFrames) == m_sunCacheSlot) targets |= kShadowBuild;
    bool live = !m_sunCacheUse;
    // An object's root: bone 0's origin through c0..c3 to clip, then to the world.
    auto nearRoot = [&](const float (*c)[4]) {
        const float origin[4] = { c[5][3], c[6][3], c[7][3], 1 };
        float clip[4], world[4];
        for (int r = 0; r < 4; r++) clip[r] = origin[0] * c[r][0] + origin[1] * c[r][1] + origin[2] * c[r][2] + origin[3] * c[r][3];
        for (int r = 0; r < 4; r++)
            world[r] = clip[0] * m_sun.clipToWorld[r][0] + clip[1] * m_sun.clipToWorld[r][1] +
                       clip[2] * m_sun.clipToWorld[r][2] + clip[3] * m_sun.clipToWorld[r][3];
        if (!(std::fabs(world[3]) > 1e-8f)) return true;
        float d2 = 0;
        for (int k = 0; k < 3; k++) { const float e = world[k] / world[3] - m_sun.camera[k]; d2 += e * e; }
        return !(d2 >= g_config.shadowCacheNear * g_config.shadowCacheNear);
    };
    if (!live) {
        if (instances > 1 && instanceConsts && instanceInfo) {
            const UINT lo = instanceInfo[1], window = instanceInfo[2];
            float regs[8][4];
            for (UINT i = 0; i < instances && !live; i++) {
                for (UINT r = 0; r < 8; r++)
                    memcpy(regs[r], r >= lo && r < lo + window ? instanceConsts + ((size_t)i * window + r - lo) * 4 : m_vsConst[r], 16);
                live = nearRoot(regs);
            }
        } else {
            live = nearRoot(m_vsConst);
        }
    }
    if (live) { targets |= kShadowLive; ++m_cacheLive; }
    if (targets & kShadowBuild) ++m_cacheBuilt;
    if (!targets) ++m_cacheSkipped;
    return targets;
}

// The game clock: with [TimeOfDay] Running the hour advances DayMinutes real minutes per game
// day, and each midnight moves the moon one day on in its cycle. A frame after a long stall
// (loading, the window in the background) does not jump the sky.
void Device::TickClock()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double dt = m_clockLast.QuadPart ? (now.QuadPart - m_clockLast.QuadPart) / (double)m_qpcFreq.QuadPart : 0.0;
    m_clockLast = now;
    if (!(dt > 0) || dt > 0.25) return;
    m_cloudTime += dt;
    if (!g_config.timeOfDay || !g_config.timeRuns) return;
    float hour = g_config.timeOfDayHour + (float)(dt * 24.0 / (g_config.dayMinutes * 60.0));
    if (hour >= 24) {
        hour -= 24;
        g_config.timeOfDayDay += 1;
        if (g_config.timeOfDayDay >= g_config.moonCycleDays) g_config.timeOfDayDay -= g_config.moonCycleDays;
    }
    g_config.timeOfDayHour = hour;
}

// Time of day (Insert panel): the sun crosses a tilted arc from east (6 h) through a 70 degree
// noon to west (18 h). The moon runs the same arc a share of a turn behind it, the share growing
// with the days of its cycle: together with the sun (new), opposite (full, up all night), or up
// by day in between. At night the moon lights the scene by its phase while it is up; sky glow
// (MoonlessLight) is the floor. Colours are multipliers on the game's own light, 1 at noon. Y-up here.
void Device::EvaluateTimeOfDay()
{
    m_tod = TimeOfDay();
    if (!g_config.timeOfDay) return;
    auto smooth = [](float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); };
    auto mix = [](const float* a, const float* b, float t, float* o) { for (int k = 0; k < 3; k++) o[k] = a[k] + (b[k] - a[k]) * t; };
    const float pi = 3.14159265f;
    const float tilt = 70.0f * pi / 180, phi = (g_config.timeOfDayHour - 6.0f) / 12.0f * pi;
    const float sun[3] = { std::cos(phi), std::sin(phi) * std::sin(tilt), std::sin(phi) * std::cos(tilt) };
    const float sunY = sun[1];
    const float up = smooth(-0.03f, 0.12f, sunY), warm = smooth(0.0f, 0.45f, sunY);
    const float white[3] = { 1, 1, 1 };
    float sunColor[3]; mix(g_config.lowSunColor, white, warm, sunColor); // golden low sun
    const float moonUp = smooth(0.0f, 0.15f, -sunY) * (1 - up); // night
    // The moon's age in its cycle: 0 new, 0.5 full.
    float age = (g_config.timeOfDayDay + g_config.timeOfDayHour / 24) / (std::max)(g_config.moonCycleDays, 1.0f);
    age -= std::floor(age);
    const float moonPhi = phi - 2 * pi * age;
    const float moon[3] = { std::cos(moonPhi), std::sin(moonPhi) * std::sin(tilt), std::sin(moonPhi) * std::cos(tilt) };
    const float moonLit = 0.5f * (1 - std::cos(2 * pi * age)); // lit share of the disc
    const float moonAbove = smooth(-0.02f, 0.12f, moon[1]);
    const float moonShare = moonAbove * (0.15f + 0.85f * moonLit); // 1 = the full moon high up
    const float nightLight = moonShare > g_config.moonlessLight ? moonShare : g_config.moonlessLight;
    // The night light comes from the moon, turning to a high sky light as it sets (no jump).
    float nightDir[3] = { moon[0] * (0.4f + 0.6f * moonAbove), moon[1] * moonAbove + (1 - moonAbove), moon[2] * (0.4f + 0.6f * moonAbove) };
    {
        const float len = std::sqrt(nightDir[0] * nightDir[0] + nightDir[1] * nightDir[1] + nightDir[2] * nightDir[2]);
        for (int k = 0; k < 3; k++) nightDir[k] /= len > 1e-4f ? len : 1.0f;
    }
    for (int k = 0; k < 3; k++) {
        // moonlight: cold, silvery, clearly directional
        m_tod.light[k] = sunColor[k] * up * (0.8f + 0.2f * warm) + g_config.moonColor[k] * g_config.moonIntensity * moonUp * nightLight;
        m_tod.dir[k] = sunY > -0.03f ? sun[k] : nightDir[k];
    }
    // Dusk ambient is the blue sky: warm sunlit faces against cool shade.
    const float day[3] = { 1, 1, 1 };
    const float* dusk = g_config.duskAmbient;
    const float* night = g_config.nightAmbient;
    const float skyDusk[3] = { 1.0f, 0.62f, 0.48f }, skyNight[3] = { 0.05f, 0.07f, 0.16f };
    const float dayW = smooth(-0.05f, 0.30f, sunY), nightW = 1 - smooth(-0.25f, -0.02f, sunY);
    float a[3], s[3];
    mix(dusk, day, dayW, a); mix(a, night, nightW, m_tod.ambient);
    mix(skyDusk, day, dayW, s); mix(s, skyNight, nightW, m_tod.sky);
    // Distant haze: the game's daytime haze colour toward the night horizon.
    const float fogNight[3] = { 0.15f, 0.19f, 0.26f };
    mix(s, fogNight, nightW, m_tod.fog);
    auto lum = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    // Eye adaptation: flat ground under a low sun or the moon receives little light; raise the
    // exposure toward the noon level (at most 2.2x) so dawn and night are not just dark.
    const float lightY = std::fabs(m_tod.dir[1]) > 0.2f ? std::fabs(m_tod.dir[1]) : 0.2f;
    const float key = lum(m_tod.ambient) * 0.6f + lum(m_tod.light) * 0.4f * lightY;
    m_tod.exposure = key > 1e-3f ? 0.75f / key : g_config.maxExposure;
    m_tod.exposure = m_tod.exposure < 1 ? 1 : m_tod.exposure > g_config.maxExposure ? g_config.maxExposure : m_tod.exposure;
    m_tod.exposure *= 1 + (g_config.nightExposure - 1) * moonUp; // night is meant to look dark
    for (int k = 0; k < 3; k++) {
        // Surfaces without normals (grass, water): the moon mostly lights their tips and edges.
        const float lw = 0.55f * (1 - moonUp) + g_config.nightSurface * moonUp;
        float t = (m_tod.ambient[k] * 0.45f + m_tod.light[k] * lw) * m_tod.exposure;
        m_tod.surface[k] = t > 1.1f ? 1.1f : t;
    }
    m_tod.sunLight = lum(m_tod.light);
    // Shadows: as deep as the direct light's share of the total away from noon, and cooler.
    const float share = lum(m_tod.light) / (lum(m_tod.light) + lum(m_tod.ambient) + 1e-3f);
    const float baseShadow = g_config.shadowStrength + (g_config.dayShadowStrength - g_config.shadowStrength) * dayW;
    m_tod.shadowStrength = baseShadow + (share * 0.9f - baseShadow) * (1 - dayW);
    if (m_tod.shadowStrength < baseShadow) m_tod.shadowStrength = baseShadow;
    // Shadows dissolve while the light hands over from the sun to the night light (their directions
    // differ), instead of jumping.
    m_tod.shadowStrength *= sunY > -0.03f ? smooth(-0.03f, 0.03f, sunY) : smooth(0.03f, 0.09f, -sunY);
    const float coolDay[3] = { 0.93f, 0.96f, 1.0f }, coolDusk[3] = { 0.70f, 0.80f, 1.08f };
    mix(coolDusk, coolDay, dayW, m_tod.shadowTint);
    for (int k = 0; k < 3; k++) m_tod.sunDir[k] = sun[k];
    m_tod.procSky = 1 - dayW; // the game's sky texture only by day
    m_tod.dayW = dayW;
    m_tod.sheen = g_config.moonSheen * moonUp * nightLight + g_config.specular * up;
    m_tod.moon = moonUp;
    // Golden glow: by day the sun outlines things like the moon at night, softly at noon and most
    // at sunrise and sunset.
    m_tod.golden = g_config.sunGlow * up * (0.35f + 0.65f * (1 - warm));
    m_tod.rim = g_config.moonRim * moonUp * nightLight + g_config.rimLight * up * (1.0f + 0.8f * (1 - warm)) // moon, then the sun (stronger when low)
                + g_config.moonRim * m_tod.golden;
    {
        // The outline's colour by day: the sun's light tinted by the sky around it (blue at noon,
        // pink and orange at dusk), like the moonlight's silver at night.
        const float skyDayT[3] = { 0.78f, 0.88f, 1.0f }, skyDuskT[3] = { 1.0f, 0.62f, 0.55f };
        float tint[3]; mix(skyDuskT, skyDayT, warm, tint);
        const float l = 0.2126f * m_tod.light[0] + 0.7152f * m_tod.light[1] + 0.0722f * m_tod.light[2];
        for (int k = 0; k < 3; k++) m_tod.glowColor[k] = m_tod.light[k] * 0.55f + tint[k] * l * 0.45f;
    }
    for (int k = 0; k < 3; k++) m_tod.moonDir[k] = moon[k];
    m_tod.moonAge = age;
    m_tod.moonLit = moonLit;
    m_tod.moonAbove = moonAbove;
    m_tod.moonGlitter = moonUp * moonShare;
    m_tod.starAngle = phi;
    {
        // Clouds: lit by the sun until a little after sunset (from below, red), then by the moon.
        const float lowCloud[3] = { 1.0f, 0.42f, 0.22f };
        float sunC[3]; mix(lowCloud, white, smooth(0.0f, 0.35f, sunY), sunC);
        if (sunY > -0.08f) {
            const float k = smooth(-0.08f, 0.05f, sunY) * 1.1f;
            for (int i = 0; i < 3; i++) { m_tod.cloudKey[i] = sunC[i] * k; m_tod.cloudKeyDir[i] = sun[i]; }
        } else {
            const float k = moonShare * smooth(0.08f, 0.16f, -sunY) * 0.6f;
            for (int i = 0; i < 3; i++) { m_tod.cloudKey[i] = g_config.moonColor[i] * k; m_tod.cloudKeyDir[i] = moon[i]; }
        }
        const float skyDay[3] = { 0.62f, 0.70f, 0.82f }, skyDuskC[3] = { 0.50f, 0.36f, 0.40f };
        const float nightK = g_config.nightSky * (0.5f + 0.5f * moonShare);
        const float skyNightC[3] = { 0.030f * nightK, 0.040f * nightK, 0.065f * nightK };
        float t[3];
        mix(skyDuskC, skyDay, dayW, t); mix(t, skyNightC, nightW, m_tod.cloudSky);
        const float groundTint[3] = { 0.50f, 0.46f, 0.42f };
        for (int i = 0; i < 3; i++) m_tod.cloudGround[i] = m_tod.cloudSky[i] * groundTint[i];
    }
    m_tod.active = true;
}

void Device::StartSunFrame()
{
    if (g_config.sunLighting && !EnsureShadowMap()) return;
    XMMATRIX vp;
    float rows[4][4];
    memcpy(rows, m_vsConst, sizeof(rows));
    LoadRows(rows, vp);
    XMVECTOR det;
    XMMATRIX inv = XMMatrixInverse(&det, vp);
    if (std::fabs(XMVectorGetX(det)) < 1e-20f) return;
    StoreRows(inv, m_sun.clipToWorld);
    memcpy(m_waterViewProjection,rows,sizeof(rows));
    // Camera position: the point that maps to clip (0, 0, z, 0).
    XMVECTOR camH = XMVector4Transform(XMVectorSet(0, 0, 1, 0), inv);
    XMVECTOR cam;
    if (std::fabs(XMVectorGetW(camH)) > 1e-12f) {
        cam = camH / XMVectorSplatW(camH);
    } else {
        // Orthographic: no finite eye; put it far back along the view direction.
        XMVECTOR centre = XMVector3TransformCoord(XMVectorSet(0, 0, 0, 1), inv);
        cam = centre - XMVector3Normalize(XMVectorSetW(camH, 0)) * 1e5f;
    }
    m_sun.camera[0] = XMVectorGetX(cam); m_sun.camera[1] = XMVectorGetY(cam); m_sun.camera[2] = XMVectorGetZ(cam);
    BuildLightTiles(); // this frame's camera (view-projection and position)
    if (g_config.pointLightDebug && m_frame % 300 == 0)
        Log("Sun segment: camera %.1f %.1f %.1f, viewport %ux%u at %u,%u, rt %s", m_sun.camera[0], m_sun.camera[1], m_sun.camera[2],
            m_viewport.Width, m_viewport.Height, m_viewport.X, m_viewport.Y, m_rt == m_backBuffer ? "back" : "other");

    // JPOG's world is Y-down: "up" is -Y (camera above the park at y = -55, palm crowns at
    // y = -6 over ground at -1). SunX/Y/Z in the ini use Y-up, so Y is negated here.
    EvaluateTimeOfDay();
    XMVECTOR sun = m_tod.active ? XMVectorSet(m_tod.dir[0], -m_tod.dir[1], m_tod.dir[2], 0)
        : g_config.sunFromGame && m_gameSunValid
        ? XMVectorSet(m_gameSun[0], m_gameSun[1], m_gameSun[2], 0)
        : XMVectorSet(g_config.sunDirection[0], -g_config.sunDirection[1], g_config.sunDirection[2], 0);
    sun = XMVector3Normalize(sun);
    float elevation = -XMVectorGetY(sun);
    if (elevation < g_config.sunMinElevation) {
        // Keep the azimuth, raise the sun: grazing light gives endless, unstable shadows.
        XMVECTOR flat = XMVectorSet(XMVectorGetX(sun), 0, XMVectorGetZ(sun), 0);
        float fl = XMVectorGetX(XMVector3Length(flat));
        flat = fl > 1e-4f ? flat / fl : XMVectorSet(1, 0, 0, 0);
        float h = g_config.sunMinElevation;
        sun = XMVectorSetY(flat * std::sqrt(1 - h * h), -h);
    }
    // Steps instead of a slow continuous turn: between steps the light box and its texel grid stay
    // put, so shadow edges hold still; a step of a fraction of a degree is hardly visible.
    if (g_config.shadowStep > 0) {
        const float stepCos = std::cos(g_config.shadowStep * 0.0174533f);
        const XMVECTOR held = XMVectorSet(m_shadowSunDir[0], m_shadowSunDir[1], m_shadowSunDir[2], 0);
        if (!m_shadowSunValid || XMVectorGetX(XMVector3Dot(held, sun)) < stepCos) {
            XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(m_shadowSunDir), sun);
            m_shadowSunValid = true;
        } else {
            sun = held;
        }
    }
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(m_sun.direction), sun);

    // The light box covers the whole view frustum cut at the shadow distance. Guessing the ground
    // height from the terrain chunk offset put the box beside the view when zoomed in (terrain
    // heights are added on top of that offset).
    const float groundY = m_vsConst[4][1];
    const float maxDist = g_config.shadowDistance;
    XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), -sun,
        std::fabs(XMVectorGetY(sun)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0));
    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
    for (int iy = -1; iy <= 1; iy += 2) {
        for (int ix = -1; ix <= 1; ix += 2) {
            XMVECTOR n = XMVector3TransformCoord(XMVectorSet((float)ix, (float)iy, 0, 1), inv);
            XMVECTOR f = XMVector3TransformCoord(XMVectorSet((float)ix, (float)iy, 1, 1), inv);
            XMVECTOR ray = f - n;
            float len = XMVectorGetX(XMVector3Length(ray));
            if (!(len > 1e-6f)) continue;
            XMVECTOR farPt = n + ray * ((len < maxDist ? len : maxDist) / len);
            XMVECTOR a = XMVector3TransformCoord(n, lightView), b = XMVector3TransformCoord(farPt, lightView);
            lo = XMVectorMin(lo, XMVectorMin(a, b));
            hi = XMVectorMax(hi, XMVectorMax(a, b));
        }
    }
    XMFLOAT3 l, h;
    XMStoreFloat3(&l, lo); XMStoreFloat3(&h, hi);
    if (!(l.x < h.x) || !std::isfinite(l.x + l.y + l.z + h.x + h.y + h.z)) return;
    const float res = (float)(m_sunMapSize ? m_sunMapSize : 4096);
    const float extent = h.x - l.x > h.y - l.y ? h.x - l.x : h.y - l.y;
    // Casters toward the sun (trees, buildings, hills off screen) sit well in front of the ground.
    const float zNear = l.z - maxDist * 0.5f - 200.0f, zFar = h.z + 100.0f;
    // The light box around this view. A cache map's box is wider (margin, zExtra): it keeps
    // serving while the view stays inside it.
    auto makeBox = [&](float margin, float zExtra, ShadowCache& c) {
        float size = extent * 1.15f * margin + 16.0f;
        size = std::ceil(size / 32.0f) * 32.0f; // stable texel size while the camera pans
        const float texel = size / res;
        const float cx = std::floor((l.x + h.x) * 0.5f / texel) * texel;
        const float cy = std::floor((l.y + h.y) * 0.5f / texel) * texel;
        const float box[6] = { cx - size / 2, cx + size / 2, cy - size / 2, cy + size / 2, zNear - zExtra, zFar + zExtra };
        memcpy(c.box, box, sizeof(box));
        XMMATRIX proj = XMMatrixOrthographicOffCenterLH(box[0], box[1], box[2], box[3], box[4], box[5]);
        StoreRows(lightView * proj, c.rows);
        c.texel = texel;
        c.depthRange = box[5] - box[4];
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(c.dir), sun);
    };
    FlushShadowBatch(); // a segment that never resolved
    const bool cache = g_config.sunLighting && m_sunDSV && g_config.shadowCache && EnsureShadowCache();
    if (!cache) m_sunCacheReady = m_sunCacheBuild = -1;
    if (cache && m_sunCacheBuild < 0) {
        // A new build cycle, centred on this view.
        const int b = m_sunCacheReady == 0 ? 1 : 0;
        makeBox(1.3f, 150.0f, m_sunCache[b]);
        m_ctx->ClearDepthStencilView(m_sunCacheDSV[b].Get(), D3D11_CLEAR_DEPTH, 1, 0);
        m_sunCacheBuild = b;
        m_sunCacheSlot = 0;
    }
    m_sunCacheUse = false;
    if (cache && m_sunCacheReady >= 0) {
        // The complete cache serves while this view fits in its box, its texels are not much
        // coarser than this view's own box would give, and the sun has turned under half a degree.
        const ShadowCache& c = m_sunCache[m_sunCacheReady];
        const float facing = c.dir[0] * XMVectorGetX(sun) + c.dir[1] * XMVectorGetY(sun) + c.dir[2] * XMVectorGetZ(sun);
        m_sunCacheUse = l.x >= c.box[0] && h.x <= c.box[1] && l.y >= c.box[2] && h.y <= c.box[3] &&
                        zNear >= c.box[4] && zFar <= c.box[5] && (extent * 1.15f + 16.0f) * 1.8f >= c.box[1] - c.box[0] &&
                        facing > 0.99996f;
    }
    ShadowCache own;
    if (!m_sunCacheUse) makeBox(1.0f, 0.0f, own);
    const ShadowCache& light = m_sunCacheUse ? m_sunCache[m_sunCacheReady] : own;
    // The cache's sun can lag the current one by up to half a degree: lighting follows the
    // direction the shadows were cast with, so shadow edges and N.L agree.
    memcpy(m_sun.direction, light.dir, sizeof(light.dir));
    const float size = light.box[1] - light.box[0], texel = light.texel;
    memcpy(m_sun.writeRows, light.rows, sizeof(m_sun.writeRows));
    memcpy(m_sun.readRows, light.rows, sizeof(m_sun.readRows));
    m_sunDepthRange = light.depthRange;
    m_sunTexelWorld = texel;
    m_sun.settings[0] = g_config.sunAmbient; m_sun.settings[1] = g_config.sunDirect;
    m_sun.settings[2] = g_config.shadowStrength; m_sun.settings[3] = 1.0f / res;
    if (g_config.sunLighting && m_sunDSV) {
        // The live map starts as the cached casters, or empty.
        if (m_sunCacheUse) m_ctx->CopyResource(m_sunMap.Get(), m_sunCacheMap[m_sunCacheReady].Get());
        else m_ctx->ClearDepthStencilView(m_sunDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    }
    if (cache) ++(m_sunCacheUse ? m_cacheUsedFrames : m_cacheFullFrames);
    m_sunStarted = true;
    m_pusherCount = 0;
    m_grassUploaded = false;
    // Grass thinning follows the grass distance the game used last frame.
    m_grassFar = m_grassFar > 0 ? m_grassFar + (m_grassFarNext - m_grassFar) * 0.2f : m_grassFarNext;
    m_grassFarNext = 0;
    LogOnce("sun-viewport", "Sun camera viewport %lu,%lu %lux%lu depth %.3f..%.3f; camera rows z %g %g %g %g w %g %g %g %g",
            m_viewport.X, m_viewport.Y, m_viewport.Width, m_viewport.Height, m_viewport.MinZ, m_viewport.MaxZ,
            rows[2][0], rows[2][1], rows[2][2], rows[2][3], rows[3][0], rows[3][1], rows[3][2], rows[3][3]);
    m_segmentCasters = 0;
    TraceSun("Sun segment start: ground %.1f, box %.0f, camera z row %g %g %g %g, w row %g %g %g %g", groundY, size,
             rows[2][0], rows[2][1], rows[2][2], rows[2][3], rows[3][0], rows[3][1], rows[3][2], rows[3][3]);
    LogOnce("sun-start", "Sun frame: camera ground y %.1f, light box %.0f units (%.3f per texel), sun %.2f %.2f %.2f (%s)",
            groundY, size, texel, XMVectorGetX(sun), XMVectorGetY(sun), XMVectorGetZ(sun),
            g_config.sunFromGame && m_gameSunValid ? "game" : "fixed");
}

// Skinned shaders light with N . -c89, where N is in the space that c0..c3 maps to clip.
// Mapping -c89 through c0 and the inverse camera gives the game's sun in world space.
void Device::CaptureGameSun()
{
    XMVECTOR l = XMVectorSet(-m_vsConst[89][0], -m_vsConst[89][1], -m_vsConst[89][2], 0);
    if (XMVectorGetX(XMVector3LengthSq(l)) < 1e-8f) return;
    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    XMMATRIX modelToClip, clipToWorld;
    LoadRows(c0, modelToClip);
    LoadRows(m_sun.clipToWorld, clipToWorld);
    XMMATRIX model = modelToClip * clipToWorld;
    XMVECTOR w = XMVector3TransformNormal(l, model);
    float len = XMVectorGetX(XMVector3Length(w));
    if (!(len > 1e-6f) || !std::isfinite(len)) return;
    w = w / len;
    // In-game this comes out as e.g. (0.47 -0.74 0.47): negative Y is up in JPOG, so the sun
    // is above the island. Should it ever point below the ground, mirror it up.
    if (XMVectorGetY(w) > 0) w = -w;
    static bool logged = false; // LogOnce locks and searches a set: not on every skinned draw
    if (!logged) {
    logged = true;
    XMFLOAT4X4 mf;
    XMStoreFloat4x4(&mf, model);
    LogOnce("sun-game", "Game sun (skin c89) %.3f %.3f %.3f; model matrix w column %.4f %.4f %.4f %.4f; terrain c22 %.3f %.3f %.3f",
            XMVectorGetX(w), XMVectorGetY(w), XMVectorGetZ(w), mf._14, mf._24, mf._34, mf._44,
            m_vsConst[22][0], m_vsConst[22][1], m_vsConst[22][2]);
    }
    if (!m_gameSunValid) {
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun), w);
        m_gameSunValid = true;
        return;
    }
    XMVECTOR prev = XMLoadFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun));
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun), XMVector3Normalize(XMVectorLerp(prev, w, 0.05f)));
}

// Grass pushers: a sample of the draw's own vertices is skinned on the CPU exactly like the
// skinned vertex shader does (bone rows at c[5 + a0], a0 = c4 * index, weighted by v2), then
// mapped to world through c0..c3 and the inverse camera. The game uploads the whole 30-bone
// palette for every skinned draw, so the creature test is the number of *distinct* bones the
// sampled vertices use: props and buildings use one or a few, animals many.
void Device::CapturePusher(UINT firstVertex, UINT numVertices)
{
    if (!numVertices) return;
    const size_t idx = m_vsHandle >> 1;
    if (idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return;
    const VSDecl& decl = m_vertexShaders[idx]->parsed;
    const VSInput *pos = nullptr, *weights = nullptr, *indices = nullptr;
    for (const VSInput& in : decl.inputs) {
        if (in.stream != 0) continue;
        if (in.reg == 0 && in.type == 2) pos = &in;
        if (in.reg == 2 && in.type == 4) weights = &in;
        if (in.reg == 3 && in.type == 4) indices = &in;
    }
    VertexBuffer* vb = m_streams[0].vb;
    const UINT stride = m_streams[0].stride;
    if (!pos || !weights || !indices || !vb || !stride || numVertices >= (1u << 24)) return;
    const std::vector<BYTE>& data = vb->m_impl.m_shadow;
    if ((size_t)(firstVertex + numVertices) * stride > data.size()) return;
    const BufferImpl& impl = vb->m_impl;
    if (m_boneCounts.empty()) m_boneCounts.resize(kBoneCounts);
    BoneCount& known = m_boneCounts[(UINT)((impl.m_identity * 0x9E3779B1u) ^ (firstVertex * 0x85EBCA6Bu) ^ numVertices) % kBoneCounts];
    const bool hit = known.identity == impl.m_identity && known.revision == impl.m_revision && known.first == firstVertex &&
                     known.count == numVertices && known.vs == m_vsHandle && !memcmp(known.c4, m_vsConst[4], sizeof(known.c4));
    if (hit && known.distinct < (UINT)g_config.grassMinBones && !m_trace) return;

    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    XMMATRIX modelToClip, clipToWorld;
    LoadRows(c0, modelToClip);
    LoadRows(m_sun.clipToWorld, clipToWorld);
    const XMMATRIX model = modelToClip * clipToWorld;
    auto color = [](const BYTE* b) { // D3DCOLOR as the shader sees it: (R, G, B, A)
        return XMFLOAT4(b[2] / 255.0f, b[1] / 255.0f, b[0] / 255.0f, b[3] / 255.0f);
    };
    BYTE used[256] = {};
    UINT distinct = 0;
    const UINT samples = numVertices < 48 ? numVertices : 48;
    // Bones first: trees, props and buildings use one or a few and stop here, without the skinning.
    for (UINT k = 0; k < samples; k++) {
        const BYTE* v = data.data() + (size_t)(firstVertex + k * numVertices / samples) * stride;
        const XMFLOAT4 w = color(v + weights->offset), i = color(v + indices->offset);
        const float wv[4] = { w.x, w.y, w.z, w.w }, iv[4] = { i.x, i.y, i.z, i.w };
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            if (!used[a0 & 255]) { used[a0 & 255] = 1; distinct++; }
        }
    }
    known = { impl.m_identity, impl.m_revision, firstVertex, numVertices, {}, m_vsHandle, distinct };
    memcpy(known.c4, m_vsConst[4], sizeof(known.c4));
    if (distinct < (UINT)g_config.grassMinBones && !m_trace) return;
    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
    for (UINT k = 0; k < samples; k++) {
        const BYTE* v = data.data() + (size_t)(firstVertex + k * numVertices / samples) * stride;
        float p[4];
        memcpy(p, v + pos->offset, 12);
        p[3] = 1;
        XMFLOAT4 w = color(v + weights->offset), i = color(v + indices->offset);
        const float wv[4] = { w.x, w.y, w.z, w.w }, iv[4] = { i.x, i.y, i.z, i.w };
        float out[3] = {};
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            for (int r = 0; r < 3; r++) {
                const float* row = m_vsConst[5 + a0 + r];
                out[r] += wv[j] * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3] * p[3]);
            }
        }
        XMVECTOR wp = XMVector3TransformCoord(XMVectorSet(out[0], out[1], out[2], 1), model);
        lo = XMVectorMin(lo, wp);
        hi = XMVectorMax(hi, wp);
    }
    XMFLOAT3 l, h;
    XMStoreFloat3(&l, lo); XMStoreFloat3(&h, hi);
    if (!std::isfinite(l.x + l.z + h.x + h.z + h.y) || h.x - l.x > 200 || h.z - l.z > 200) return;
    TraceSun("Skinned draw: %u distinct bones, box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f",
             distinct, l.x, h.x, l.y, h.y, l.z, h.z);
    if (distinct < (UINT)g_config.grassMinBones) return;
    // Body parts of one animal come as separate draws: merge overlapping footprints.
    for (UINT k = 0; k < m_pusherCount; k++) {
        float* b = m_pushBoxes[k];
        if (l.x <= b[2] && h.x >= b[0] && l.z <= b[3] && h.z >= b[1]) {
            b[0] = b[0] < l.x ? b[0] : l.x; b[1] = b[1] < l.z ? b[1] : l.z;
            b[2] = b[2] > h.x ? b[2] : h.x; b[3] = b[3] > h.z ? b[3] : h.z;
            m_pushers[k][1]=m_pushers[k][1]>h.y?m_pushers[k][1]:h.y;
            m_grassUploaded = false;
            return;
        }
    }
    if(m_pusherCount>=32)return;
    float* b = m_pushBoxes[m_pusherCount];
    b[0] = l.x; b[1] = l.z; b[2] = h.x; b[3] = h.z;
    m_pushers[m_pusherCount][1] = h.y; // Y-down: the larger y is the lowest point (feet)
    m_pusherCount++;
    m_grassUploaded = false;
    LogOnce("grass-first", "Grass pusher found: %u distinct bones, box %.1f x %.1f", distinct, h.x - l.x, h.z - l.z);
}
void Device::UploadGrass()
{
    if (!m_grassCB) return;
    const bool on=g_config.effectsEnabled && g_config.grassPhysics && m_worldDraw;
    if (!m_grassUploaded || m_grassApplied!=on) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_grassCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float* f = (float*)map.pData;
            // Footprint circle: box centre, 0.4 of the longer side (animals are long, not round).
            for (UINT k = 0; k < m_pusherCount; k++) {
                const float* b = m_pushBoxes[k];
                float r = 0.4f * ((b[2] - b[0]) > (b[3] - b[1]) ? (b[2] - b[0]) : (b[3] - b[1]));
                r = (r < 0.8f ? 0.8f : (r > 20.0f ? 20.0f : r)) * g_config.grassRadius;
                m_pushers[k][0] = (b[0] + b[2]) * 0.5f; m_pushers[k][2] = (b[1] + b[3]) * 0.5f; m_pushers[k][3] = r;
            }
            memcpy(f, m_pushers, sizeof(m_pushers));
            // Only the main view's grass reacts (the 256x256 offscreen pass has its own camera).
            f[128] = on ? (float)m_pusherCount : 0.0f;
            f[129] = g_config.grassPush;
            f[130] = g_config.grassFlatten;
            f[131] = 0;
            m_ctx->Unmap(m_grassCB.Get(), 0);
            m_grassUploaded = true;
            m_grassApplied = on;
        }
    }
    BindVSCB(3, 1, m_grassCB.GetAddressOf());
}
void Device::UpdateSun(bool shadowPass)
{
    if (!g_config.effectsEnabled) {
        // Translated shaders still reference b1/b2: upload a neutral state, not stale
        // relighting/night/grass parameters from the last enabled frame.
        m_worldDraw = m_worldCaster = false;
        if (!m_sunNeutral) {
            m_sun = {};
            if (!m_sunUploadedValid || memcmp(&m_sunUploaded, &m_sun, sizeof(m_sun))) {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(m_ctx->Map(m_sunCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    memcpy(map.pData, &m_sun, sizeof(m_sun)); m_ctx->Unmap(m_sunCB.Get(), 0);
                    m_sunUploaded = m_sun; m_sunUploadedValid = true;
                }
            }
            m_sunNeutral = m_sunUploadedValid && !memcmp(&m_sunUploaded, &m_sun, sizeof(m_sun));
        }
        BindVSCB(2, 1, m_sunCB.GetAddressOf());
        BindPSCB(1, 1, m_sunCB.GetAddressOf());
        if (m_worldKind == 4) UploadGrass(); // explicitly disable old pusher data
        return;
    }
    m_sunNeutral = false;
    if (!SceneFx()) {
        m_worldDraw = false;
    } else {
        if (m_worldDraw && m_worldKind == 2 && !m_sunStarted && !m_sunResolved) StartSunFrame();
        if (!m_sunStarted) {
            if (m_worldCaster) m_earlyCasters++;
            m_worldDraw = false;
        } else if (m_sunResolved && m_worldCaster) {
            m_lateWorldDraws++;
        }
        if (m_worldDraw && m_worldKind == 1 && !m_sunResolved && !shadowPass) {
            CaptureGameSun();
        }
    }
    m_sun.flags[0] = m_worldDraw ? 1.0f : 0.0f;
    m_sun.flags[1] = shadowPass ? 1.0f : 0.0f;
    // Rigid pre-lit meshes (paths: vs_002, oD0 = vertex colour * c4) carry the terrain's baked
    // light; they are relit with the light colours of the last terrain draw.
    // Grass (vs_010, oD0 = c5) is relit the same way, over the height field's normal.
    m_sun.flags[2] = m_terrainLightValid && (m_worldKind == 3 || (m_worldKind == 4 && g_config.relightGrass)) ? 1.0f : 0.0f;
    m_sun.flags[3] = (float)m_worldKind;
    const bool thin = m_worldKind == 4 && g_config.grassThin && m_worldDraw && m_grassFar > 1;
    if (m_worldKind == 4) {
        const float dx = m_vsConst[4][0] - m_sun.camera[0], dz = m_vsConst[4][2] - m_sun.camera[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d > m_grassFarNext) m_grassFarNext = d;
    }
    m_sun.grassThin[0] = thin ? m_grassFar * g_config.grassThinStart : 0;
    m_sun.grassThin[1] = thin ? m_grassFar : 0;
    m_sun.grassThin[2] = g_config.grassThinKeep;
    m_sun.grassThin[3] = thin ? 1 + g_config.grassThinFill : 0;
    m_sun.control[0] = g_config.replaceLighting || ActiveMaterial() ? 1.0f : 0.0f;
    m_sun.control[1] = (float)g_config.lightingDebug;
    m_sun.control[2] = m_tessCandidate && !shadowPass ? 1.0f : 0.0f; // VS emits normals for the hull shader
    m_sun.camera[3] = g_config.normalStrength * (g_config.normalFlipGreen ? -1.0f : 1.0f);
    // The game's own light colours for this draw (vs_000/vs_001: oD0 = (N.-c89 * c91 + c90) * c88;
    // terrain vs_005: v0.w * c7 + c6). Other kinds fall back to the configured pilot light.
    for (int k = 0; k < 3; k++) {
        float amb = g_config.sunAmbient * 0.75f, sun = g_config.sunDirect * (k == 0 ? 1.0f : k == 1 ? 0.95f : 0.84f);
        if (m_worldKind == 1) { amb = m_vsConst[90][k] * m_vsConst[88][k]; sun = m_vsConst[91][k] * m_vsConst[88][k]; }
        else if (m_worldKind == 2) {
            amb = m_vsConst[6][k]; sun = m_vsConst[7][k];
            if (m_worldDraw && !shadowPass) { m_terrainLight[0][k] = amb; m_terrainLight[1][k] = sun; m_terrainLightValid = true; }
        } else if ((m_worldKind == 3 || m_worldKind == 4) && m_terrainLightValid) { amb = m_terrainLight[0][k]; sun = m_terrainLight[1][k]; }
        m_sun.gameAmbient[k] = amb; m_sun.gameSun[k] = sun;
    }
    m_sun.gameAmbient[3] = g_config.relightTonemap ? 1.0f : 0.0f;
    m_sun.gameSun[3] = g_config.replaceLighting ? 1.0f : 0.0f;
    m_sun.relightParams[0] = g_config.relightSun;
    m_sun.relightParams[1] = g_config.relightAmbient;
    m_sun.relightParams[2] = g_config.relightSkyTint;
    m_sun.relightParams[3] = g_config.relightExposure * (m_tod.active ? m_tod.exposure : 1.0f);
    if (m_worldDraw && m_worldKind == 2 && !shadowPass) UpdateTerrainHeights();
    m_sun.terrainMap[0] = TerrainHeightsActive() ? 1.0f : 0.0f;
    m_sun.terrainMap[1] = m_terrainW ? 1.0f / m_terrainW : 0.0f;
    m_sun.terrainMap[2] = m_terrainH ? 1.0f / m_terrainH : 0.0f;
    m_sun.terrainMap[3] = g_config.horizonShadows;
    m_sun.control[3] = g_config.bakedTerrainDetail;
    for (int k = 0; k < 3; k++) {
        m_sun.todSun[k] = m_tod.active ? m_tod.light[k] : 1.0f;
        m_sun.todAmbient[k] = m_tod.active ? m_tod.ambient[k] : 1.0f;
        m_sun.todNight[k] = m_tod.active ? m_tod.surface[k] : 1.0f;
    }
    // Rim light: from the time-of-day light, otherwise from the game's sun with a warm white.
    m_sun.todAmbient[3] = m_tod.active ? m_tod.rim : g_config.rimLight;
    static const float warmWhite[3] = { 1.0f, 0.93f, 0.80f };
    for (int k = 0; k < 3; k++) m_sun.rimColor[k] = m_tod.active ? (m_tod.moon > 0.5f ? m_tod.light[k] : m_tod.glowColor[k]) : warmWhite[k];
    m_sun.todSun[3] = m_tod.active ? m_tod.sheen : g_config.specular;
    m_sun.specParams[0] = g_config.specRoughness; m_sun.specParams[1] = 0.04f;
    m_sun.specParams[2] = m_tod.active ? m_tod.moon : 0.0f; m_sun.specParams[3] = g_config.moonRimWidth;
    for (int k = 0; k < 3; k++) m_sun.moonDir[k] = m_tod.active ? (k == 1 ? -m_tod.moonDir[k] : m_tod.moonDir[k]) : 0.0f;
    m_sun.moonDir[3] = m_tod.active ? m_tod.moonGlitter : 0.0f;
    // The sun's glitter on water comes from where the sun is in the sky, low over the sea at dusk,
    // not from the raised direction the shadows use.
    for (int k = 0; k < 3; k++) m_sun.skySun[k] = m_tod.active ? (k == 1 ? -m_tod.sunDir[k] : m_tod.sunDir[k]) : 0.0f;
    m_sun.skySun[3] = m_tod.active ? std::clamp((m_tod.sunDir[1] + 0.02f) / 0.08f, 0.0f, 1.0f) * (1 - m_tod.moon) : 0.0f;
    m_sun.rimStyle[0] = m_tod.active ? (std::min)(1.0f, m_tod.moon + m_tod.golden) : 0.0f;
    for (int k = 0; k < 3; k++) m_sun.gameSunDir[k] = m_gameSunValid ? m_gameSun[k] : m_sun.direction[k];
    {
        // Night emission: fades in as the day fades out.
        const Texture* et = ActiveEmissive() ? static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0])) : nullptr;
        const float night = m_tod.active ? 1 - m_tod.dayW : 0.0f;
        for (int k = 0; k < 3; k++) m_sun.emissiveGlow[k] = et ? et->m_emissive[k] * et->m_emissive[3] * g_config.emissiveIntensity * night : 0.0f;
        for (int k = 0; k < 3; k++) m_sun.nightGrade[k] = g_config.nightTint[k];
        m_sun.nightGrade[3] = m_tod.active ? g_config.nightDesaturate * m_tod.moon : 0.0f;
    }
    m_sun.rimColor[3] = m_tod.active ? g_config.grassMoon * (m_tod.moon + m_tod.golden) : 0.0f;
    m_sun.grassGrade[0] = g_config.grassSaturation; m_sun.grassGrade[1] = g_config.grassBrightness;
    m_sun.grassGrade[2] = 0.08f + (m_tod.active ? g_config.moonRimGlow * (m_tod.moon + m_tod.golden) : 0.0f);
    m_sun.grassGrade[3] = g_config.replaceLighting ? 1.0f : 0.0f;
    for (int k = 0; k < 3; k++) m_sun.terrainTint[k] = g_config.terrainTint[k];
    for (int k = 0; k < 3; k++) m_sun.grassTint[k] = g_config.grassTint[k];
    m_sun.grassTint[3] = m_sun.terrainTint[3] = g_config.terrainMatch * (m_tod.active ? 1 - m_tod.moon : 1.0f); // a multiplier on the lit colour: follows the light through the day and dusk; off at night (the night look as it was)
    for (int k = 0; k < 3; k++) m_sun.fogTint[k] = m_tod.active ? m_tod.fog[k] : 1.0f;
    m_sun.todNight[3] = m_tod.active && !shadowPass && (m_worldDraw || (m_waterShaderDraw && m_rt == m_backBuffer)) ? 1.0f : 0.0f;
    if (!m_sunUploadedValid || memcmp(&m_sunUploaded, &m_sun, sizeof(m_sun))) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_sunCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &m_sun, sizeof(m_sun)); m_ctx->Unmap(m_sunCB.Get(), 0);
            m_sunUploaded = m_sun; m_sunUploadedValid = true;
        }
    }
    BindVSCB(2, 1, m_sunCB.GetAddressOf());
    if (m_worldKind == 4) UploadGrass();
    BindPSCB(1, 1, m_sunCB.GetAddressOf());
}

void Device::DrawSunShadow(bool indexed, UINT count, UINT start, INT base, UINT instances, const UINT* instanceInfo,
                           const float* instanceConsts)
{
    CpuPerformanceScope cpu(m_perf,2);
    const int bones = m_drawBones; // set by the caller for this draw only
    m_drawBones = -1;
    if (!g_config.effectsEnabled || !g_config.sunLighting || !m_worldCaster || !m_sunStarted || m_sunResolved || !m_sunDSV) return;
    // The depth-only variant of the draw's pixel shader. The sun constants are not uploaded here:
    // a batched caster records them for FlushShadowBatch, and uploading the shadow pass state
    // (then the camera pass state again at the next draw) cost two buffer maps per caster.
    const float pass = m_sun.flags[1];
    m_sun.flags[1] = 1.0f;
    ID3D11PixelShader* depthPixel = GetPixelShader();
    m_sun.flags[1] = pass;
    if (!depthPixel) return;
    ++m_shadowDraws;
    ++m_perfShadowDraws;
    ++m_segmentCasters;
    // Batched: the caster is recorded and drawn with the others into the shadow map before the
    // resolve reads it (FlushShadowBatch), instead of switching the render target to the shadow
    // map and back for every caster (about 3 us of GPU each, 4 ms a frame with 1300 casters).
    // Recorded state comes from the context itself; the vertex constants stay in the ring window
    // this draw used. Grass casters (pusher constants) and draws from our own dynamic buffers
    // keep the immediate path.
    ShadowDraw d = {};
    ComPtr<ID3D11Buffer> ib, vbs[kMaxStreams];
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11VertexShader> vs;
    if (const ShadowInput* in = m_shadowInput) {
        d.ib = in->ib; d.ibFormat = in->ibFormat;
        memcpy(d.vb, in->vb, sizeof(d.vb));
        memcpy(d.stride, in->stride, sizeof(d.stride)); // offsets are always 0
        d.layout = in->layout; d.vs = in->vs; d.topo = in->topo;
    } else {
        m_ctx->IAGetIndexBuffer(&ib, &d.ibFormat, &d.ibOffset);
        m_ctx->IAGetVertexBuffers(0, kMaxStreams, vbs[0].GetAddressOf(), d.stride, d.offset);
        m_ctx->IAGetInputLayout(&layout);
        m_ctx->VSGetShader(&vs, nullptr, nullptr);
        m_ctx->IAGetPrimitiveTopology(&d.topo);
        d.ib = ib.Get();
        for (int s = 0; s < kMaxStreams; s++) d.vb[s] = vbs[s].Get();
        d.layout = layout.Get(); d.vs = vs.Get();
    }
    bool batch = m_vsRing && g_config.constantRing && m_vsRingBound && m_worldKind != 4 && !(indexed && d.ib == m_dynIB.Get());
    for (int s = 0; s < kMaxStreams; s++)
        if (d.vb[s] && d.vb[s] == m_dynVB.Get()) batch = false;
    if (!batch) {
        FlushShadowBatch();
        UpdateSun(true);
        m_ctx->PSSetShader(depthPixel, nullptr, 0);
        m_lastPS = nullptr;
        m_ctx->OMSetRenderTargets(0, nullptr, m_sunDSV.Get());
        D3D11_VIEWPORT vp = { 0, 0, (float)m_sunMapSize, (float)m_sunMapSize, 0, 1 };
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->RSSetState(m_sunRaster.Get());
        m_ctx->OMSetDepthStencilState(m_sunDepth.Get(), 0);
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        if (instances > 1) m_ctx->DrawIndexedInstanced(count, instances, start, base, 0); // instance data still bound
        else if (indexed) m_ctx->DrawIndexed(count, start, base); else m_ctx->Draw(count, start);
        // Vertex/pixel shaders, streams and constants remain valid. PrepareDraw restores targets,
        // viewport and the sun-pass flag before the next game draw.
        m_rtDirty = m_viewportDirty = true;
        m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
        return;
    }
    d.targets = kShadowLive;
    if (m_sunCacheBuild >= 0 || m_sunCacheUse) {
        d.targets = ShadowTargets(instanceConsts, instances, instanceInfo, d, bones);
        if (!d.targets) return; // in the cache already
    }
    int slot = 0;
    KeepForShadow(slot++, d.ib);
    for (int s = 0; s < kMaxStreams; s++) KeepForShadow(slot++, d.vb[s]);
    KeepForShadow(slot++, d.layout);
    KeepForShadow(slot++, d.vs);
    d.ps = depthPixel;
    // Stage textures and samplers as PrepareDraw bound them (the binding cache), unless the cache
    // was invalidated since.
    const ID3D11ShaderResourceView* const invalidSrv = (ID3D11ShaderResourceView*)~(uintptr_t)0;
    const ID3D11SamplerState* const invalidSamp = (ID3D11SamplerState*)~(uintptr_t)0;
    bool cached = true;
    for (int s = 0; s < kMaxStages; s++) cached = cached && m_bind.psSrv[s] != invalidSrv && m_bind.psSamp[s] != invalidSamp;
    if (cached) {
        memcpy(d.srv, m_bind.psSrv, sizeof(d.srv));
        memcpy(d.sampler, m_bind.psSamp, sizeof(d.sampler));
    } else {
        ComPtr<ID3D11ShaderResourceView> srvs[kMaxStages];
        ComPtr<ID3D11SamplerState> samplers[kMaxStages];
        m_ctx->PSGetShaderResources(0, kMaxStages, srvs[0].GetAddressOf());
        m_ctx->PSGetSamplers(0, kMaxStages, samplers[0].GetAddressOf());
        for (int s = 0; s < kMaxStages; s++) { d.srv[s] = srvs[s].Get(); d.sampler[s] = samplers[s].Get(); KeepForShadow(slot + s, d.srv[s]); KeepForShadow(slot + kMaxStages + s, d.sampler[s]); }
    }
    if (cached)
        for (int s = 0; s < kMaxStages; s++) { KeepForShadow(slot + s, d.srv[s]); KeepForShadow(slot + kMaxStages + s, d.sampler[s]); }
    d.vsFirst = m_vsRingFirst;
    // The shadow pass reads the light rows, flags and controls; the draw's own light colours only
    // shade the camera pass, and dropping them lets consecutive casters share one upload.
    SunConstants shadowSun = m_sun;
    shadowSun.flags[1] = 1.0f;
    shadowSun.control[2] = 0.0f;
    shadowSun.todNight[3] = 0.0f;
    memset(shadowSun.gameAmbient, 0, sizeof(shadowSun.gameAmbient));
    memset(shadowSun.gameSun, 0, sizeof(shadowSun.gameSun));
    memset(shadowSun.emissiveGlow, 0, sizeof(shadowSun.emissiveGlow));
    if (m_shadowSuns.empty() || memcmp(&m_shadowSuns.back(), &shadowSun, sizeof(shadowSun))) m_shadowSuns.push_back(shadowSun);
    d.sun = (UINT)m_shadowSuns.size() - 1;
    if (m_shadowPS.empty() || memcmp(&m_shadowPS.back(), &m_psConst, sizeof(m_psConst))) m_shadowPS.push_back(m_psConst);
    d.psc = (UINT)m_shadowPS.size() - 1;
    d.indexed = indexed; d.count = count; d.start = start; d.base = base;
    d.instances = instances;
    if (instances > 1) {
        // Own copy of the window constants: x becomes the first element in m_shadowInstConsts.
        memcpy(d.instanceInfo, instanceInfo, sizeof(d.instanceInfo));
        d.instanceInfo[0] = (UINT)(m_shadowInstConsts.size() / 4);
        m_shadowInstConsts.insert(m_shadowInstConsts.end(), instanceConsts, instanceConsts + (size_t)instances * instanceInfo[2] * 4);
    }
    m_shadowBatch.push_back(d);
}

// Draws the recorded casters into the shadow map in one render-target switch. Called before the
// resolve reads the map, and before anything the recorded draws reference changes: the vertex
// constant ring wrapping, a game buffer or texture being written.
void Device::FlushShadowBatch()
{
    if (m_shadowBatch.empty()) return;
    ++m_shadowBatchesFrame;
    if (!m_shadowPSCB) {
        D3D11_BUFFER_DESC bd = { (UINT)((sizeof(PSConstants) + 15) & ~15u), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_shadowPSCB))) { m_shadowBatch.clear(); return; }
    }
    PerformanceScope perf(m_perf, m_device.Get(), m_ctx.Get(), Performance::Shadow);
    bool instancesReady = true;
    if (!m_shadowInstConsts.empty()) {
        const UINT elements = (UINT)(m_shadowInstConsts.size() / 4);
        if (elements > m_shadowInstElements) {
            UINT size = m_shadowInstElements ? m_shadowInstElements : 1u << 16;
            while (size < elements) size *= 2;
            D3D11_BUFFER_DESC bd = { size * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = size;
            m_shadowInstBuf.Reset(); m_shadowInstSRV.Reset(); m_shadowInstElements = 0;
            if (SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, &m_shadowInstBuf)) &&
                SUCCEEDED(m_device->CreateShaderResourceView(m_shadowInstBuf.Get(), &sd, &m_shadowInstSRV)))
                m_shadowInstElements = size;
        }
        if (m_shadowInstElements) {
            const D3D11_BOX box = { 0, 0, 0, elements * 16, 1, 1 };
            m_ctx->UpdateSubresource(m_shadowInstBuf.Get(), 0, &box, m_shadowInstConsts.data(), 0, 0);
        } else instancesReady = false;
    }
    D3D11_VIEWPORT vp = { 0, 0, (float)m_sunMapSize, (float)m_sunMapSize, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_sunRaster.Get());
    m_ctx->OMSetDepthStencilState(m_sunDepth.Get(), 0);
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    ID3D11Buffer* none = nullptr;
    m_ctx->VSSetConstantBuffers(4, 1, &none); // water block: casters are never water
    m_ctx->PSSetConstantBuffers(0, 1, m_shadowPSCB.GetAddressOf());
    m_ctx->VSSetConstantBuffers(2, 1, m_sunCB.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1, 1, m_sunCB.GetAddressOf());
    // The shadow cache map being built (with its own light rows), then the live map.
    BYTE present = 0;
    for (const ShadowDraw& d : m_shadowBatch) present |= d.targets;
    for (const BYTE target : { kShadowBuild, kShadowLive }) {
    if (!(present & target) || (target == kShadowBuild && m_sunCacheBuild < 0)) continue;
    const float (*rows)[4] = target == kShadowBuild ? m_sunCache[m_sunCacheBuild].rows : nullptr;
    m_ctx->OMSetRenderTargets(0, nullptr, target == kShadowBuild ? m_sunCacheDSV[m_sunCacheBuild].Get() : m_sunDSV.Get());
    UINT sun = ~0u, psc = ~0u, first = ~0u;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* ib = nullptr;
    UINT ibOffset = 0;
    const ShadowDraw* prev = nullptr;
    const UINT* lastInfo = nullptr;
    for (const ShadowDraw& d : m_shadowBatch) {
        if (!(d.targets & target)) continue;
        if (d.sun != sun) {
            sun = d.sun;
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(m_ctx->Map(m_sunCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                SunConstants* out = static_cast<SunConstants*>(map.pData);
                memcpy(out, &m_shadowSuns[sun], sizeof(m_sun));
                if (rows) memcpy(out->writeRows, rows, sizeof(out->writeRows));
                m_ctx->Unmap(m_sunCB.Get(), 0);
            }
        }
        if (d.psc != psc) {
            psc = d.psc;
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(m_ctx->Map(m_shadowPSCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &m_shadowPS[psc], sizeof(PSConstants)); m_ctx->Unmap(m_shadowPSCB.Get(), 0);
            }
        }
        if (d.vsFirst != first) {
            first = d.vsFirst;
            const UINT num = kMaxVSConst;
            m_ctx1->VSSetConstantBuffers1(0, 1, m_vsRing.GetAddressOf(), &first, &num);
        }
        if (d.vs != vs) { vs = d.vs; m_ctx->VSSetShader(vs, nullptr, 0); }
        if (d.ps != ps) { ps = d.ps; m_ctx->PSSetShader(ps, nullptr, 0); }
        if (d.layout != layout) { layout = d.layout; m_ctx->IASetInputLayout(layout); }
        if (d.topo != topo) { topo = d.topo; m_ctx->IASetPrimitiveTopology(topo); }
        // Consecutive casters of one model share streams, textures and samplers.
        if (!prev || memcmp(prev->vb, d.vb, sizeof(d.vb)) || memcmp(prev->stride, d.stride, sizeof(d.stride)) ||
            memcmp(prev->offset, d.offset, sizeof(d.offset)))
            m_ctx->IASetVertexBuffers(0, kMaxStreams, d.vb, d.stride, d.offset);
        if (d.indexed && (d.ib != ib || d.ibOffset != ibOffset)) { ib = d.ib; ibOffset = d.ibOffset; m_ctx->IASetIndexBuffer(ib, d.ibFormat, d.ibOffset); }
        if (!prev || memcmp(prev->srv, d.srv, sizeof(d.srv))) m_ctx->PSSetShaderResources(0, kMaxStages, d.srv);
        if (!prev || memcmp(prev->sampler, d.sampler, sizeof(d.sampler))) m_ctx->PSSetSamplers(0, kMaxStages, d.sampler);
        prev = &d;
        if (d.instances > 1) {
            if (!instancesReady) continue;
            if (!lastInfo || memcmp(lastInfo, d.instanceInfo, sizeof(d.instanceInfo))) { SetSkinInstanceInfo(d.instanceInfo, m_shadowInstSRV.Get()); lastInfo = d.instanceInfo; }
            m_ctx->DrawIndexedInstanced(d.count, d.instances, d.start, d.base, 0);
        } else if (d.indexed) m_ctx->DrawIndexed(d.count, d.start, d.base); else m_ctx->Draw(d.count, d.start);
    }
    }
    m_shadowBatch.clear();
    m_shadowKeep.clear();
    memset(m_shadowKept, 0, sizeof(m_shadowKept));
    m_shadowSuns.clear();
    m_shadowPS.clear();
    m_shadowInstConsts.clear();
    // Everything the game's draws had bound is rebound by the next PrepareDraw.
    InvalidateBindings();
    m_rtDirty = m_viewportDirty = true;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr; m_lastIB = nullptr;
    for (int s = 0; s < kMaxStreams; s++) m_lastVB[s] = nullptr;
    m_sunUploadedValid = false;
}

// End of the world pass: copies the scene and redraws it with sun shadows and the [Post]
// sunlight/colour grade applied, before the HUD is drawn on top.
void Device::ResolveSun(const char* reason)
{
    if (!m_sunStarted || m_sunResolved) return;
    FlushShadowBatch(); // the shadow map is complete before anything reads it
    // This segment's share of the shadow cache build is drawn (the game ends the world segment
    // with a depth clear, before Present).
    if (m_sunCacheBuild >= 0 && ++m_sunCacheSlot >= (UINT)g_config.shadowCacheFrames) {
        m_sunCacheReady = m_sunCacheBuild; // complete: serves from the next segment
        m_sunCacheBuild = -1;
    }
    // JPOG clears depth at the end of the world, before Present. Capture the
    // current scene's footprints while its camera and pusher list are valid.
    UpdateWaterInteractions();
    m_sunResolved = true;
    TraceSun("Resolve at %s: %u casters in this segment", reason, m_segmentCasters);
    const bool shadows = g_config.sunLighting && m_segmentCasters && m_sunSRV;
    // Volumetric shafts through the clouds' shadow need no sun shadow map.
    const bool cloudShafts = m_tod.active && g_config.clouds && g_config.shafts;
    const bool shafts = (shadows && g_config.shafts) || cloudShafts;
    const bool fx = g_config.aoEnabled || shafts;
    if (!shadows && !g_config.postEnabled && !fx && !m_tod.active && !ProceduralSkyActive()) {ResolveTemporal();return;}
    if (!m_resolvePS || !m_backBuffer || !m_autoDepth || !m_autoDepth->m_srv || !EnsureSceneCopy()) {ResolveTemporal();return;}
    if (shadows && g_config.lightingDebug == 8 && !m_dumped) {
        if (!m_dumpAt) m_dumpAt = m_frame + 600;
        else if (m_frame >= m_dumpAt) { DumpSunFrame(); m_dumped = true; }
    }

    ResolveConstants rc = {};
    memcpy(rc.clipToWorld, m_sun.clipToWorld, sizeof(rc.clipToWorld));
    memcpy(rc.lightRows, m_sun.writeRows, sizeof(rc.lightRows));
    memcpy(rc.sunDir, m_sun.direction, sizeof(rc.sunDir));
    rc.params[0] = m_tod.active ? m_tod.shadowStrength : g_config.shadowStrength;
    rc.params[1] = m_sunMapSize ? 1.0f / m_sunMapSize : 0.0f;
    rc.params[2] = 1.5f * m_sunTexelWorld / m_sunDepthRange;
    rc.params[3] = 1.5f * m_sunTexelWorld;
    const float w = (float)m_backBuffer->m_width, h = (float)m_backBuffer->m_height;
    rc.screen[0] = 1.0f / w; rc.screen[1] = 1.0f / h;
    rc.screen[2] = -1.0f / w; rc.screen[3] = 1.0f / h; // VSExtraConstants::pixelOffset
    float aaX=0,aaY=0;TemporalJitter(aaX,aaY);rc.screen[2]+=aaX;rc.screen[3]+=aaY;
    rc.misc[0] = (float)g_config.lightingDebug;
    rc.misc[1] = g_config.shadowDistance;
    rc.misc[2] = m_sunDepthRange;
    rc.misc[3] = shadows ? 1.0f : 0.0f;
    rc.grade0[0] = g_config.exposure; rc.grade0[1] = g_config.contrast;
    rc.grade0[2] = g_config.saturation; rc.grade0[3] = g_config.warmth;
    const float dayW = m_tod.active ? m_tod.dayW : 0.0f; // day values over the dusk and night ones
    rc.grade1[0] = g_config.sunBoost + (g_config.daySunBoost - g_config.sunBoost) * dayW; rc.grade1[1] = g_config.shadowCool;
    rc.grade1[2] = g_config.postEnabled ? 1.0f : 0.0f;
    rc.fx0[0] = g_config.aoRadius; rc.fx0[1] = 1.6f; rc.fx0[2] = g_config.aoStrength + (g_config.dayAO - g_config.aoStrength) * dayW; rc.fx0[3] = g_config.shaftStrength;
    rc.fx1[0] = g_config.aoEnabled ? 1.0f : 0.0f; rc.fx1[1] = shafts ? 1.0f : 0.0f; rc.fx1[2] = g_config.shaftScatter;
    {
        // The sun's shadow map shades the shafts only while it is cast along the true light (not
        // raised to SunMinHeight): otherwise they would come from the wrong place.
        const float* k = m_tod.cloudKeyDir;
        rc.fx1[3] = !m_tod.active || k[0] * m_sun.direction[0] - k[1] * m_sun.direction[1] + k[2] * m_sun.direction[2] > 0.9995f ? 1.0f : 0.0f;
    }
    for (int k = 0; k < 3; k++) rc.tod[k] = m_tod.active ? m_tod.sky[k] : 1.0f;
    rc.tod[3] = m_tod.active ? (m_tod.sunLight < 1 ? m_tod.sunLight : 1.0f) : 1.0f;
    for (int k = 0; k < 3; k++) rc.tod2[k] = m_tod.active ? (k == 1 ? -m_tod.sunDir[k] : m_tod.sunDir[k]) : m_sun.direction[k];
    rc.tod2[3] = ProceduralSkyActive() ? 1.0f : m_tod.active ? m_tod.procSky : 0.0f;
    rc.tod4[2] = m_tod.active ? m_tod.dayW : 1.0f;
    rc.tod3[0] = m_tod.active ? m_tod.moon : 0.0f;
    static const float defaultTint[3] = { 0.93f, 0.96f, 1.0f };
    for (int k = 0; k < 3; k++) rc.tod3[k + 1] = m_tod.active ? m_tod.shadowTint[k] : defaultTint[k];
    rc.tod4[0] = g_config.nightSky; rc.tod4[1] = g_config.stars;
    rc.tod4[3] = 0.0f;
    for (int k = 0; k < 3; k++) rc.tod5[k] = g_config.nightTint[k];
    for (int k = 0; k < 3; k++) rc.tod6[k] = m_tod.active ? (k == 1 ? -m_tod.moonDir[k] : m_tod.moonDir[k]) : 0.0f;
    rc.tod6[3] = m_tod.active ? m_tod.moonLit : 0.0f;
    rc.tod7[0] = m_tod.starAngle; rc.tod7[1] = m_tod.moonAbove; rc.tod7[2] = g_config.sunDisc;
    rc.tod7[3] = m_tod.active ? 1.0f : 0.0f;
    const bool clouds = m_tod.active && g_config.clouds && EnsureClouds();
    rc.cloud5[2] = g_config.horizonHaze;
    rc.cloud5[0] = (float)((m_frame * 0.61803398875) - std::floor(m_frame * 0.61803398875));
    bool rays = false;
    const bool vfog = m_tod.active && g_config.volumeFog && TerrainHeightsActive() && m_terrainW && m_terrainH && EnsureFog();
    if (vfog) {
        // Where the hills start: percentiles of the terrain heights (park floor and highest hills),
        // taken again after each rescan of the height field.
        if (m_fogHillScan != m_terrainRefresh) {
            m_fogHillScan = m_terrainRefresh;
            m_fogPoolDirty = true;
            std::vector<float> sample;
            for (size_t i = 0; i < m_terrainHeights.size(); i += 13) sample.push_back(m_terrainHeights[i]);
            if (!sample.empty()) {
                std::nth_element(sample.begin(), sample.begin() + sample.size() / 2, sample.end());
                m_fogHillFloor = sample[sample.size() / 2];
                std::nth_element(sample.begin(), sample.begin() + sample.size() * 99 / 100, sample.end());
                m_fogHillTop = sample[sample.size() * 99 / 100];
                static float logged[2] = { 1e9f, 1e9f };
                if (logged[0] != m_fogHillFloor || logged[1] != m_fogHillTop)
                    Log("Volumetric fog: terrain floor %.1f, hills up to %.1f", m_fogHillFloor, m_fogHillTop);
                logged[0] = m_fogHillFloor; logged[1] = m_fogHillTop;
            }
        }
        rc.vfog0[0] = g_config.volumeDensity;
        rc.vfog0[2] = 1.0986f / g_config.volumeHeight; rc.vfog0[3] = 1.0f;
        for (int k = 0; k < 3; k++) {
            rc.vfog1[k] = m_tod.cloudKey[k];
            rc.vfog2[k] = m_tod.cloudSky[k] * 0.8f;
            rc.vfog3[k] = k == 1 ? -m_tod.cloudKeyDir[k] : m_tod.cloudKeyDir[k];
        }
        rc.vfog1[3] = g_config.volumePatches;
        rc.vfog2[3] = (float)std::fmod(m_cloudTime, 10000.0);
        rc.vfog4[0] = 1.0f / m_terrainW; rc.vfog4[1] = 1.0f / m_terrainH;
        const float range = (std::max)(m_fogHillTop - m_fogHillFloor, 1.0f);
        rc.vfog4[2] = m_fogHillFloor + g_config.volumeHills * range;
        rc.vfog4[3] = (std::max)(m_fogHillTop - rc.vfog4[2], 1.0f);
    }
    if (m_tod.active && g_config.godRays) {
        // The sun while it is up (strongest low down), then the moon by its light.
        const float sunY = m_tod.sunDir[1];
        const bool sunLight = sunY > -0.05f;
        const float* L = sunLight ? m_tod.sunDir : m_tod.moonDir;
        float k = sunLight ? (1 - 0.6f * (std::min)(1.0f, (std::max)(0.0f, sunY * 2))) * (std::min)(1.0f, (std::max)(0.0f, (sunY + 0.05f) / 0.1f))
                           : m_tod.moonGlitter * 0.6f;
        const float reach = 5000.0f;
        const float X[4] = { m_sun.camera[0] + L[0] * reach, m_sun.camera[1] - L[1] * reach, m_sun.camera[2] + L[2] * reach, 1 };
        float clip[4];
        for (int r = 0; r < 4; r++)
            clip[r] = X[0] * m_waterViewProjection[r][0] + X[1] * m_waterViewProjection[r][1] + X[2] * m_waterViewProjection[r][2] + X[3] * m_waterViewProjection[r][3];
        if (clip[3] > 1e-3f && k > 0.001f && EnsureRays()) {
            const float u = clip[0] / clip[3] * 0.5f + 0.5f, v = 0.5f - clip[1] / clip[3] * 0.5f;
            // Fades out as the light leaves the screen.
            const float out = (std::max)(std::fabs(u - 0.5f), std::fabs(v - 0.5f)) - 0.5f;
            k *= (std::min)(1.0f, (std::max)(0.0f, 1 - out / 0.6f));
            if (k > 0.001f) {
                rays = true;
                rc.rays0[0] = u; rc.rays0[1] = v; rc.rays0[3] = k;
                const float* col = sunLight ? nullptr : g_config.moonColor;
                static const float sunCol[3] = { 1.0f, 0.78f, 0.52f };
                for (int c = 0; c < 3; c++) rc.rays1[c] = (sunLight ? sunCol[c] : col[c] * 0.6f) * g_config.godRayStrength * 2.0f;
                rc.rays1[3] = 1.0f;
            }
        }
    }
    if (m_tod.active && g_config.fog) {
        rc.fog0[0] = g_config.fogDensity; rc.fog0[1] = g_config.fogHeightDensity;
        rc.fog0[2] = 1.0986f / g_config.fogHeightFalloff; // a third per falloff height
        rc.fog0[3] = 1.0f;
        rc.fog1[0] = g_config.fogBase;
        // Toward the sun the air glows with its light (strongest when it is low), toward the moon faintly.
        for (int k = 0; k < 3; k++) {
            rc.fog2[k] = m_tod.cloudKey[k] * g_config.fogSun;
            rc.fog1[k + 1] = k == 1 ? -m_tod.cloudKeyDir[k] : m_tod.cloudKeyDir[k];
        }
    }
    if (clouds) {
        rc.cloud0[0] = g_config.cloudCoverage; rc.cloud0[1] = 0.012f * g_config.cloudDensity;
        rc.cloud0[2] = g_config.cloudHeight; rc.cloud0[3] = g_config.cloudHeight + g_config.cloudThickness;
        const float drift = (float)std::fmod(m_cloudTime * g_config.cloudWind, 102000.0);
        for (int k = 0; k < 3; k++) {
            rc.cloud1[k] = m_tod.cloudKey[k];
            rc.cloud2[k] = m_tod.cloudSky[k];
            rc.cloud3[k] = m_tod.cloudGround[k];
            rc.cloud4[k] = k == 1 ? -m_tod.cloudKeyDir[k] : m_tod.cloudKeyDir[k];
        }
        rc.cloud1[3] = 1.0f;
        rc.cloud2[3] = drift * 0.8f; rc.cloud3[3] = drift * 0.6f;
        rc.cloud5[1] = (float)std::fmod(m_cloudTime, 10000.0);
        // The cloud shadow map follows the camera in whole texels (no swimming).
        const float size = 8000.0f, texel = size / kCloudShadowRes;
        rc.cloud6[0] = std::floor(m_sun.camera[0] / texel) * texel - size / 2;
        rc.cloud6[1] = std::floor(m_sun.camera[2] / texel) * texel - size / 2;
        rc.cloud6[2] = size;
        rc.cloud6[3] = 1.0f;
    }
    rc.tod5[3] = g_config.filmic && g_config.replaceLighting ? g_config.filmicExposure : 0.0f;
    if (m_tod.active) {
        rc.day0[0] = g_config.dayBrightness; rc.day0[1] = g_config.dayLift; rc.day0[2] = g_config.daySaturation;
        rc.day0[3] = m_tod.dayW;
        if (g_config.islands && EnsureIslands()) {
            rc.isle0[0] = 1.0f; rc.isle0[1] = g_config.islandHeight; rc.isle0[2] = g_config.islandHaze; rc.isle0[3] = 1.0f;
            for (int k = 0; k < 3; k++) {
                rc.isle1[k] = m_tod.cloudKey[k];
                rc.isle2[k] = k == 1 ? -m_tod.cloudKeyDir[k] : m_tod.cloudKeyDir[k];
                rc.isle3[k] = m_tod.cloudSky[k];
            }
            rc.isle2[3] = m_cloudNoiseSRV ? g_config.islandClouds : 0.0f;
            rc.isle3[3] = (float)std::fmod(m_cloudTime, 10000.0);
            // Around the park, well past its sea's edge, in units of the park's half size: bearing
            // (degrees), distance, radius, peak height, shape.
            static const float kIsles[7][5] = {
                { 18, 6.0f, 1.5f, 0.55f, 0 }, { 64, 8.8f, 2.3f, 0.80f, 1 }, { 116, 5.4f, 0.9f, 0.34f, 2 },
                { 168, 7.6f, 1.8f, 0.70f, 3 }, { 226, 9.6f, 2.6f, 0.85f, 1 }, { 284, 6.3f, 1.2f, 0.45f, 2 },
                { 332, 10.2f, 2.0f, 0.75f, 0 } };
            const float half = 0.5f * (float)(std::max)(m_terrainW ? m_terrainW : 1024u, m_terrainH ? m_terrainH : 1024u);
            const float cx = m_terrainW ? 0.5f * m_terrainW : half, cz = m_terrainH ? 0.5f * m_terrainH : half;
            const float camH = 0; // the islands stand on the sea's edge, seen from sea level
            float top = 0;
            for (int i = 0; i < 7; i++) {
                const float a = kIsles[i][0] * 0.0174533f;
                float* A = rc.isleA[i];
                A[0] = cx + std::cos(a) * kIsles[i][1] * half; A[1] = cz + std::sin(a) * kIsles[i][1] * half;
                A[2] = kIsles[i][2] * half; A[3] = kIsles[i][3] * half * g_config.islandHeight;
                rc.isleB[i][0] = std::cos(a * 3.7f); rc.isleB[i][1] = std::sin(a * 3.7f); rc.isleB[i][2] = kIsles[i][4];
                const float dx = A[0] - m_sun.camera[0], dz = A[1] - m_sun.camera[2];
                const float gap = (std::max)(std::sqrt(dx * dx + dz * dz) - A[2] * 1.8f, 1.0f); // to the clouds around it
                top = (std::max)(top, (A[3] * 1.35f - camH) / gap);
            }
            rc.isle1[3] = top * 1.05f + 0.002f;
        }
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_resolveCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &rc, sizeof(rc));
    m_ctx->Unmap(m_resolveCB.Get(), 0);

    CopySceneColor();
    D3D11_VIEWPORT vp = { 0, 0, w, h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    const bool ms = m_autoDepth->m_samples > 1;
    if (clouds) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::Effects);
        ID3D11ShaderResourceView* none[8] = {};
        m_ctx->PSSetShaderResources(0, 8, none);
        ID3D11RenderTargetView* rtv = m_cloudShadowRTV.Get();
        m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT svp = { 0, 0, (float)kCloudShadowRes, (float)kCloudShadowRes, 0, 1 };
        m_ctx->RSSetViewports(1, &svp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_cloudShadowPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* noise = m_cloudNoiseSRV.Get();
        m_ctx->PSSetShaderResources(5, 1, &noise);
        m_ctx->PSSetSamplers(1, 1, m_cloudWrap.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 8, none);
        m_ctx->RSSetViewports(1, &vp);
    }
    if (fx && EnsureFxTarget()) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::Effects);
        // Effects pass: AO + light shafts into m_fxTex, read (blurred) by the resolve below.
        ID3D11ShaderResourceView* none[4] = {};
        m_ctx->PSSetShaderResources(0, 4, none);
        ID3D11RenderTargetView* fxRtv = m_fxRTV.Get();
        m_ctx->OMSetRenderTargets(1, &fxRtv, nullptr);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(ms ? m_fxPSMS.Get() : m_fxPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* in[2] = { m_autoDepth->m_srv.Get(), shadows ? m_sunSRV.Get() : nullptr };
        m_ctx->PSSetShaderResources(0, 2, in);
        ID3D11ShaderResourceView* cloudShadow = clouds ? m_cloudShadowSRV.Get() : nullptr;
        m_ctx->PSSetShaderResources(7, 1, &cloudShadow);
        m_ctx->PSSetSamplers(0, 1, m_sunSampler.GetAddressOf());
        m_ctx->PSSetSamplers(2, 1, m_linearClamp.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 2, none);
        ID3D11ShaderResourceView* noShadow = nullptr;
        m_ctx->PSSetShaderResources(7, 1, &noShadow);
    }
    if (clouds) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::Effects);
        ID3D11ShaderResourceView* none[6] = {};
        m_ctx->PSSetShaderResources(0, 6, none);
        ID3D11RenderTargetView* cloudRtv = m_cloudRTV.Get();
        m_ctx->OMSetRenderTargets(1, &cloudRtv, nullptr);
        D3D11_VIEWPORT cvp = { 0, 0, (float)m_cloudW, (float)m_cloudH, 0, 1 };
        m_ctx->RSSetViewports(1, &cvp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(ms ? m_cloudPSMS.Get() : m_cloudPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* in[6] = { m_autoDepth->m_srv.Get(), nullptr, nullptr, nullptr, nullptr, m_cloudNoiseSRV.Get() };
        m_ctx->PSSetShaderResources(0, 6, in);
        m_ctx->PSSetSamplers(1, 1, m_cloudWrap.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 6, none);
        m_ctx->RSSetViewports(1, &vp);
    }
    if (vfog && (m_fogPoolDirty || m_fogPoolHills != g_config.volumeHills)) {
        m_fogPoolDirty = false;
        m_fogPoolHills = g_config.volumeHills;
        ID3D11ShaderResourceView* none[11] = {};
        m_ctx->PSSetShaderResources(0, 11, none);
        ID3D11RenderTargetView* poolRtv = m_fogPoolRTV.Get();
        m_ctx->OMSetRenderTargets(1, &poolRtv, nullptr);
        D3D11_VIEWPORT pvp = { 0, 0, (float)m_terrainW, (float)m_terrainH, 0, 1 };
        m_ctx->RSSetViewports(1, &pvp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_fogPoolPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* terrain = m_terrainHeightSRV.Get();
        m_ctx->PSSetShaderResources(9, 1, &terrain);
        m_ctx->PSSetSamplers(3, 1, m_terrainSampler.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 11, none);
        m_ctx->RSSetViewports(1, &vp);
    }
    if (vfog) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::Effects);
        ID3D11ShaderResourceView* none[9] = {};
        m_ctx->PSSetShaderResources(0, 9, none);
        ID3D11RenderTargetView* fogRtv = m_fogRTV.Get();
        m_ctx->OMSetRenderTargets(1, &fogRtv, nullptr);
        D3D11_VIEWPORT fvp = { 0, 0, (float)m_fogW, (float)m_fogH, 0, 1 };
        m_ctx->RSSetViewports(1, &fvp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(ms ? m_fogPSMS.Get() : m_fogPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* in[11] = { m_autoDepth->m_srv.Get(), shadows ? m_sunSRV.Get() : nullptr, nullptr, nullptr, nullptr,
                                             m_cloudNoiseSRV.Get(), nullptr, clouds ? m_cloudShadowSRV.Get() : nullptr, nullptr,
                                             nullptr, m_fogPoolSRV.Get() };
        m_ctx->PSSetShaderResources(0, 11, in);
        m_ctx->PSSetSamplers(3, 1, m_terrainSampler.GetAddressOf());
        m_ctx->PSSetSamplers(0, 1, m_sunSampler.GetAddressOf());
        m_ctx->PSSetSamplers(1, 1, m_cloudWrap.GetAddressOf());
        m_ctx->PSSetSamplers(2, 1, m_linearClamp.GetAddressOf());
        m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none11[11] = {};
        m_ctx->PSSetShaderResources(0, 11, none11);
        m_ctx->RSSetViewports(1, &vp);
    }
    // The sky the water reflects, for next frame's water draws (they read it at t26).
    if (m_tod.active && g_config.waterEnabled && g_config.waterSkyReflection && EnsureSkyEnv()) {
        ID3D11ShaderResourceView* noEnv = nullptr;
        m_ctx->PSSetShaderResources(26, 1, &noEnv);
        ID3D11RenderTargetView* envRtv = m_skyEnvRTV.Get();
        m_ctx->OMSetRenderTargets(1, &envRtv, nullptr);
        D3D11_VIEWPORT evp = { 0, 0, 128, 64, 0, 1 };
        m_ctx->RSSetViewports(1, &evp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_skyEnvPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_ctx->GenerateMips(m_skyEnvSRV.Get());
        m_ctx->RSSetViewports(1, &vp);
        m_skyEnvReady = true;
    }
    if (rays) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::Effects);
        ID3D11ShaderResourceView* none[6] = {};
        m_ctx->PSSetShaderResources(0, 6, none);
        ID3D11RenderTargetView* raysRtv = m_raysRTV.Get();
        m_ctx->OMSetRenderTargets(1, &raysRtv, nullptr);
        D3D11_VIEWPORT rvp = { 0, 0, (float)m_raysW, (float)m_raysH, 0, 1 };
        m_ctx->RSSetViewports(1, &rvp);
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(ms ? m_raysPSMS.Get() : m_raysPS.Get(), nullptr, 0);
        m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
        ID3D11ShaderResourceView* in[5] = { m_autoDepth->m_srv.Get(), nullptr, nullptr, nullptr, clouds ? m_cloudSRV.Get() : nullptr };
        m_ctx->PSSetShaderResources(0, 5, in);
        m_ctx->PSSetSamplers(1, 1, m_linearClamp.GetAddressOf());
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 6, none);
        m_ctx->RSSetViewports(1, &vp);
    }
    PerformanceScope perfResolve(m_perf,m_device.Get(),m_ctx.Get(),Performance::Resolve);
    ID3D11RenderTargetView* rtv = m_backBuffer->Rtv();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    m_ctx->RSSetState(m_noCull.Get());
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(ms ? m_resolvePSMS.Get() : m_resolvePS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
    ID3D11ShaderResourceView* srvs[9] = { m_autoDepth->m_srv.Get(), shadows ? m_sunSRV.Get() : nullptr, m_sceneCopySRV.Get(),
                                          fx ? m_fxSRV.Get() : nullptr, clouds ? m_cloudSRV.Get() : nullptr, nullptr,
                                          rays ? m_raysSRV.Get() : nullptr, clouds ? m_cloudShadowSRV.Get() : nullptr,
                                          vfog ? m_fogSRV.Get() : nullptr };
    m_ctx->PSSetShaderResources(0, 9, srvs);
    ID3D11ShaderResourceView* isles = rc.isle0[0] > 0.5f ? m_islandSRV.Get() : nullptr;
    m_ctx->PSSetShaderResources(11, 1, &isles);
    if (rc.isle2[3] > 0) {
        ID3D11ShaderResourceView* noise = m_cloudNoiseSRV.Get();
        m_ctx->PSSetShaderResources(5, 1, &noise);
        m_ctx->PSSetSamplers(4, 1, m_cloudWrap.GetAddressOf());
    }
    m_ctx->PSSetSamplers(2, 1, m_linearClamp.GetAddressOf());
    m_ctx->PSSetSamplers(0, 1, m_sunSampler.GetAddressOf());
    m_ctx->PSSetSamplers(1, 1, m_linearClamp.GetAddressOf());
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[12] = {};
    m_ctx->PSSetShaderResources(0, 12, nulls);
    ApplyBloom();

    // Everything above has to be rebound by the next game draw.
    m_rtDirty = m_viewportDirty = true;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    LogOnce(std::string("sun-resolve-") + reason, "Scene pass at %s: shadows %d (%u casters), post %d",
            reason, shadows, m_segmentCasters, g_config.postEnabled);
    ResolveTemporal();
}

// Cloud pass: shaders, the noise volume and the sampler on first use, the half-resolution
// target whenever the back buffer size changes. Any failure turns the clouds off for the session.
bool Device::EnsureClouds()
{
    if (m_cloudFault || !m_backBuffer || !m_resolveVS) return false;
    if (!m_cloudPS) {
        auto makePS = [&](int msaa, const char* name, ComPtr<ID3D11PixelShader>& out) {
            auto blob = CompileHLSL(std::string(msaa ? "#define MSAA 1\n" : "#define MSAA 0\n") + kSceneCommon + kCloudCommon + kCloudMain, "ps_4_1", name);
            return blob && SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
        };
        const int n = 64;
        const std::vector<BYTE> noise = MakeCloudNoise(n);
        D3D11_TEXTURE3D_DESC td = { (UINT)n, (UINT)n, (UINT)n, 1, DXGI_FORMAT_R8G8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA init = { noise.data(), (UINT)n * 2, (UINT)(n * n * 2) };
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        auto shadowBlob = CompileHLSL(std::string("#define MSAA 0\n") + kSceneCommon + kCloudCommon + kCloudShadowMain, "ps_4_1", "cloud_shadow_ps");
        D3D11_TEXTURE2D_DESC sd2 = { kCloudShadowRes, kCloudShadowRes, 1, 1, DXGI_FORMAT_R16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (!makePS(0, "cloud_ps", m_cloudPS) || !makePS(1, "cloud_ps_ms", m_cloudPSMS) || !shadowBlob ||
            FAILED(m_device->CreatePixelShader(shadowBlob->GetBufferPointer(), shadowBlob->GetBufferSize(), nullptr, &m_cloudShadowPS)) ||
            FAILED(m_device->CreateTexture2D(&sd2, nullptr, &m_cloudShadowTex)) ||
            FAILED(m_device->CreateRenderTargetView(m_cloudShadowTex.Get(), nullptr, &m_cloudShadowRTV)) ||
            FAILED(m_device->CreateShaderResourceView(m_cloudShadowTex.Get(), nullptr, &m_cloudShadowSRV)) ||
            FAILED(m_device->CreateTexture3D(&td, &init, &m_cloudNoise)) ||
            FAILED(m_device->CreateShaderResourceView(m_cloudNoise.Get(), nullptr, &m_cloudNoiseSRV)) ||
            FAILED(m_device->CreateSamplerState(&sd, &m_cloudWrap))) {
            Log("Clouds: setup failed, disabled");
            m_cloudPS.Reset(); m_cloudPSMS.Reset();
            m_cloudFault = true;
            return false;
        }
        Log("Clouds: %d^3 noise volume ready", n);
    }
    const UINT w = (m_backBuffer->m_width + 1) / 2, h = (m_backBuffer->m_height + 1) / 2;
    if (m_cloudTex && m_cloudW == w && m_cloudH == h) return true;
    m_cloudSRV.Reset(); m_cloudRTV.Reset(); m_cloudTex.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_cloudTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_cloudTex.Get(), nullptr, &m_cloudRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_cloudTex.Get(), nullptr, &m_cloudSRV))) {
        m_cloudTex.Reset();
        m_cloudFault = true;
        return false;
    }
    m_cloudW = w; m_cloudH = h;
    return true;
}

// God ray pass: shaders on first use, the half-resolution target whenever the back buffer size
// changes. Any failure turns the rays off for the session.
bool Device::EnsureRays()
{
    if (m_raysFault || !m_backBuffer || !m_resolveVS) return false;
    if (!m_raysPS) {
        auto makePS = [&](int msaa, const char* name, ComPtr<ID3D11PixelShader>& out) {
            auto blob = CompileHLSL(std::string(msaa ? "#define MSAA 1\n" : "#define MSAA 0\n") + kSceneCommon + kRaysMain, "ps_4_1", name);
            return blob && SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
        };
        if (!makePS(0, "rays_ps", m_raysPS) || !makePS(1, "rays_ps_ms", m_raysPSMS)) {
            Log("God rays: shaders failed, disabled");
            m_raysPS.Reset(); m_raysPSMS.Reset();
            m_raysFault = true;
            return false;
        }
    }
    const UINT w = (m_backBuffer->m_width + 1) / 2, h = (m_backBuffer->m_height + 1) / 2;
    if (m_raysTex && m_raysW == w && m_raysH == h) return true;
    m_raysSRV.Reset(); m_raysRTV.Reset(); m_raysTex.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_raysTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_raysTex.Get(), nullptr, &m_raysRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_raysTex.Get(), nullptr, &m_raysSRV))) {
        m_raysTex.Reset();
        m_raysFault = true;
        return false;
    }
    m_raysW = w; m_raysH = h;
    return true;
}

// The water's sky reflection target (128x64, mips for rough water) and its shader.
bool Device::EnsureSkyEnv()
{
    if (m_skyEnvFault || !m_resolveVS) return false;
    if (m_skyEnvTex) return true;
    auto blob = CompileHLSL(std::string("#define MSAA 0\n") + kSceneCommon + kSkyCommon + kSkyEnvMain, "ps_4_1", "sky_env_ps");
    D3D11_TEXTURE2D_DESC td = { 128, 64, 7, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
    if (!blob || FAILED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_skyEnvPS)) ||
        FAILED(m_device->CreateTexture2D(&td, nullptr, &m_skyEnvTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_skyEnvTex.Get(), nullptr, &m_skyEnvRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_skyEnvTex.Get(), nullptr, &m_skyEnvSRV))) {
        Log("Water sky reflection: creation failed, disabled");
        m_skyEnvSRV.Reset(); m_skyEnvRTV.Reset(); m_skyEnvTex.Reset(); m_skyEnvPS.Reset();
        m_skyEnvFault = true;
        return false;
    }
    return true;
}

// Volumetric fog pass: needs the cloud noise volume (EnsureClouds), its shaders on first use and
// the half-resolution target whenever the back buffer size changes.
bool Device::EnsureFog()
{
    if (m_fogFault || !m_backBuffer || !m_resolveVS || !EnsureClouds()) return false;
    if (!m_fogPS) {
        auto makePS = [&](int msaa, const char* name, ComPtr<ID3D11PixelShader>& out) {
            auto blob = CompileHLSL(std::string(msaa ? "#define MSAA 1\n" : "#define MSAA 0\n") + kSceneCommon + kCloudCommon + kFogMain, "ps_4_1", name);
            return blob && SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
        };
        auto poolBlob = CompileHLSL(std::string("#define MSAA 0\n") + kSceneCommon + kFogPoolMain, "ps_4_1", "fog_pool_ps");
        if (!makePS(0, "fog_ps", m_fogPS) || !makePS(1, "fog_ps_ms", m_fogPSMS) || !poolBlob ||
            FAILED(m_device->CreatePixelShader(poolBlob->GetBufferPointer(), poolBlob->GetBufferSize(), nullptr, &m_fogPoolPS))) {
            Log("Volumetric fog: shaders failed, disabled");
            m_fogPS.Reset(); m_fogPSMS.Reset();
            m_fogFault = true;
            return false;
        }
    }
    D3D11_TEXTURE2D_DESC pd = {};
    if (m_fogPoolTex) m_fogPoolTex->GetDesc(&pd);
    if (!m_fogPoolTex || pd.Width != m_terrainW || pd.Height != m_terrainH) {
        m_fogPoolSRV.Reset(); m_fogPoolRTV.Reset(); m_fogPoolTex.Reset();
        D3D11_TEXTURE2D_DESC td = { m_terrainW, m_terrainH, 1, 1, DXGI_FORMAT_R16G16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_fogPoolTex)) ||
            FAILED(m_device->CreateRenderTargetView(m_fogPoolTex.Get(), nullptr, &m_fogPoolRTV)) ||
            FAILED(m_device->CreateShaderResourceView(m_fogPoolTex.Get(), nullptr, &m_fogPoolSRV))) {
            m_fogPoolTex.Reset();
            m_fogFault = true;
            return false;
        }
        m_fogPoolDirty = true;
    }
    const UINT w = (m_backBuffer->m_width + 1) / 2, h = (m_backBuffer->m_height + 1) / 2;
    if (m_fogTex && m_fogW == w && m_fogH == h) return true;
    m_fogSRV.Reset(); m_fogRTV.Reset(); m_fogTex.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_fogTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_fogTex.Get(), nullptr, &m_fogRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_fogTex.Get(), nullptr, &m_fogSRV))) {
        m_fogTex.Reset();
        m_fogFault = true;
        return false;
    }
    m_fogW = w; m_fogH = h;
    return true;
}

bool Device::EnsureFxTarget()
{
    const UINT w = m_backBuffer->m_width, h = m_backBuffer->m_height;
    if (m_fxTex && m_fxW == w && m_fxH == h) return true;
    m_fxSRV.Reset(); m_fxRTV.Reset(); m_fxTex.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R16G16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_fxTex)) ||
        FAILED(m_device->CreateRenderTargetView(m_fxTex.Get(), nullptr, &m_fxRTV)) ||
        FAILED(m_device->CreateShaderResourceView(m_fxTex.Get(), nullptr, &m_fxSRV))) {
        m_fxTex.Reset();
        return false;
    }
    m_fxW = w; m_fxH = h;
    return true;
}

bool Device::EnsureSceneCopy()
{
    const UINT w = m_backBuffer->m_width, h = m_backBuffer->m_height;
    if (m_sceneCopy && m_sceneCopyW == w && m_sceneCopyH == h) return true;
    m_sceneCopySRV.Reset(); m_sceneCopy.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sceneCopy)) ||
        FAILED(m_device->CreateShaderResourceView(m_sceneCopy.Get(), nullptr, &m_sceneCopySRV))) {
        m_sceneCopy.Reset();
        return false;
    }
    m_sceneCopyW = w; m_sceneCopyH = h;
    return true;
}

// Back buffer -> m_sceneCopy (resolving MSAA samples when the back buffer is multisampled).
void Device::CopySceneColor()
{
    if (m_backBuffer->m_samples > 1)
        m_ctx->ResolveSubresource(m_sceneCopy.Get(), 0, m_backBuffer->m_tex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    else
        m_ctx->CopyResource(m_sceneCopy.Get(), m_backBuffer->m_tex.Get());
}
// DebugView=8: writes the shadow map, scene depth, back buffer and matrices once to
// d3d8to11_dump\ for offline analysis (tools/sundump.py).
void Device::DumpSunFrame()
{
    if (m_autoDepth->m_samples > 1) { Log("Sun frame dump needs MSAA off"); return; }
    const std::string dir = g_gameDir + "d3d8to11_dump\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    auto save = [&](ID3D11Texture2D* tex, const char* name, UINT bytesPerPixel) {
        if (!tex) return;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) { Log("dump: staging %s failed", name); return; }
        m_ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
        if (FILE* f = _fsopen((dir + name).c_str(), "wb", _SH_DENYNO)) {
            for (UINT y = 0; y < td.Height; y++) fwrite((BYTE*)map.pData + (size_t)y * map.RowPitch, bytesPerPixel, td.Width, f);
            fclose(f);
        }
        m_ctx->Unmap(staging.Get(), 0);
    };
    save(m_sunMap.Get(), "sunmap.f32", 4);
    save(m_autoDepth->m_tex.Get(), "depth.d24s8", 4);
    save(m_backBuffer->m_tex.Get(), "color.bgra", 4);
    if (FILE* f = _fsopen((dir + "meta.txt").c_str(), "w", _SH_DENYNO)) {
        auto rows = [&](const char* n, const float (&m)[4][4]) {
            fprintf(f, "%s", n);
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) fprintf(f, " %.9g", m[r][c]);
            fprintf(f, "\n");
        };
        fprintf(f, "screen %u %u\nmap %u\n", m_backBuffer->m_width, m_backBuffer->m_height, m_sunMapSize);
        rows("clipToWorld", m_sun.clipToWorld);
        rows("lightRows", m_sun.writeRows);
        fprintf(f, "sun %.6f %.6f %.6f\ntexel %.6f\ndepthRange %.6f\ncasters %u\n", m_sun.direction[0], m_sun.direction[1],
                m_sun.direction[2], m_sunTexelWorld, m_sunDepthRange, m_segmentCasters);
        fclose(f);
    }
    Log("Sun frame dumped to d3d8to11_dump (frame %llu)", m_frame);
}

void Device::TraceSun(const char* fmt, ...)
{
    if (!m_trace || !g_config.effectsEnabled) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (m_traceLast == line) { m_traceRepeat++; return; }
    TraceFlush();
    m_traceLast = line;
    m_traceRepeat = 1;
}

void Device::TraceFlush()
{
    if (m_trace && m_traceRepeat) {
        char head[16];
        sprintf_s(head, "%4ux ", m_traceRepeat);
        m_traceBuf += head + m_traceLast + "\n";
    }
    m_traceRepeat = 0;
    m_traceLast.clear();
}

// Present: finish a frame that never reached a HUD draw, then reset per-frame state.
void Device::EndSunFrame()
{
    ResolveSun("present");
    InvalidateBindings();
    if (!g_config.effectsEnabled) {
        // A trace scheduled while shadows were enabled must not start (or wait
        // forever for a shadow draw) after the master switch is turned off.
        m_trace=false; m_traceAt=0; m_traceRepeat=0;
        m_traceBuf.clear(); m_traceLast.clear();
    }
    if (m_trace) {
        TraceFlush();
        // Keep only a frame that actually rendered the park with shadows.
        if (m_shadowDraws) {
            if (FILE* f = _fsopen((g_gameDir + "d3d8to11_frame.log").c_str(), "w", _SH_DENYNO)) {
                fputs(m_traceBuf.c_str(), f);
                fclose(f);
            }
            m_trace = false;
            m_traceDone = true;
            Log("Frame trace written to d3d8to11_frame.log (frame %llu)", m_frame);
        }
        m_traceBuf.clear();
    }
    if (g_config.effectsEnabled && g_config.sunLighting && !m_traceDone && !m_trace) {
        if (!m_traceAt && m_shadowDraws) m_traceAt = m_frame + 600;
        if (m_traceAt && m_frame + 1 >= m_traceAt) m_trace = true;
    }
    if (m_sunStarted && (m_earlyCasters || m_lateWorldDraws))
        LogOnce("sun-order", "Sun frame order: %u caster draws before the terrain, %u world draws after the resolve",
                m_earlyCasters, m_lateWorldDraws);
    m_lastCasters = m_shadowDraws;
    if (m_cacheStatsFrame + 600 <= m_frame) {
        if ((m_cacheUsedFrames || m_cacheFullFrames) && m_cacheLogged < 30) {
            ++m_cacheLogged;
            Log("Shadow cache: %u frames from the cache, %u without; casters %u live, %u into the cache, %u skipped (last 600 frames)",
                m_cacheUsedFrames, m_cacheFullFrames, m_cacheLive, m_cacheBuilt, m_cacheSkipped);
        }
        m_cacheStatsFrame = (UINT)m_frame;
        m_cacheUsedFrames = m_cacheFullFrames = m_cacheLive = m_cacheBuilt = m_cacheSkipped = 0;
    }
    m_sunStarted = m_sunResolved = false;
    m_waterSnapshotReady = false;
    m_shadowDraws = m_earlyCasters = m_lateWorldDraws = 0;
}
