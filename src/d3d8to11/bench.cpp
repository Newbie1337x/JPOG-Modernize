// GPU cost of each effect and draw kind (Insert panel, Rendimiento). With the camera still, every
// step turns one effect off (or skips one kind of world draw) and times the GPU frame against
// the baseline with everything on. The difference is what that step costs in this view.
#include "device.h"

namespace {
struct BenchStep { const char* name; bool* flag; int skipKind; };
constexpr unsigned kSettleFrames = 40, kSamples = 120;

const std::vector<BenchStep>& Steps()
{
    static const std::vector<BenchStep> steps = {
        { "Todo encendido", nullptr, -1 },
        { "Teselado", &g_config.tessellation, -1 },
        { "Relieve", &g_config.reliefEnabled, -1 },
        { "Materiales", &g_config.materialsEnabled, -1 },
        { "Mapas de normales", &g_config.normalMaps, -1 },
        { "Luces puntuales", &g_config.pointLights, -1 },
        { "AO", &g_config.aoEnabled, -1 },
        { "Rayos de sol", &g_config.shafts, -1 },
        { "Bloom", &g_config.bloom, -1 },
        { "Emision nocturna", &g_config.emissive, -1 },
        { "Reflejos del agua (SSR)", &g_config.waterSSR, -1 },
        { "Sombras del sol", &g_config.sunLighting, -1 },
        // Batching saves CPU; off, the GPU may be faster (negative: the batched path costs GPU).
        { "Terreno agrupado", &g_config.terrainBatching, -1 },
        { "Constantes en anillo", &g_config.constantRing, -1 },
        { "Arboles instanciados", &g_config.skinInstancing, -1 },
        { "Sombras lejanas en cache", &g_config.shadowCache, -1 },
        { "Modelos con huesos (dinos, arboles, objetos)", nullptr, 1 },
        { "Terreno", nullptr, 2 },
        { "Modelos rigidos (caminos, edificios)", nullptr, 3 },
        { "Pasto", nullptr, 4 },
        { "Resto (cielo, agua, efectos del juego)", nullptr, 0 },
        { "Todo encendido (control)", nullptr, -1 },
    };
    return steps;
}
}

void Device::StartGpuBench()
{
    if (m_bench.step >= 0) return;
    m_bench = GpuBench();
    m_bench.step = 0;
    m_bench.timingWas = g_config.performanceGPU;
    g_config.performanceGPU = true;
    m_perf.everyFrame = true;
    Log("GPU bench: started");
}

// Present: one step per settle + sample window.
void Device::TickGpuBench()
{
    if (m_bench.step < 0) return;
    const auto& steps = Steps();
    const BenchStep& s = steps[m_bench.step];
    if (m_bench.frames == 0) {
        if (s.flag) { m_bench.flagWas = *s.flag; *s.flag = false; }
        m_benchSkipKind = s.skipKind;
    }
    ++m_bench.frames;
    if (m_bench.frames == kSettleFrames) { m_perf.benchCount = 0; for (double& v : m_perf.benchSum) v = 0; }
    // Timed frames can be fewer than presented ones (all query slots busy): the step ends anyway.
    const bool sampled = m_bench.frames >= kSettleFrames && m_perf.benchCount >= kSamples;
    if (!sampled && m_bench.frames < kSettleFrames + kSamples * 4) return;
    GpuBench::Result r;
    r.name = s.name;
    const double n = m_perf.benchCount ? m_perf.benchCount : 1;
    for (int i = 0; i < Performance::Count; i++) r.stages[i] = m_perf.benchSum[i] / n;
    r.samples = m_perf.benchCount;
    m_bench.results.push_back(r);
    if (s.flag) *s.flag = m_bench.flagWas;
    m_benchSkipKind = -1;
    m_bench.frames = 0;
    if (++m_bench.step < (int)steps.size()) return;

    m_bench.step = -1;
    m_perf.everyFrame = false;
    g_config.performanceGPU = m_bench.timingWas;
    const GpuBench::Result& base = m_bench.results.front();
    const double total = base.stages[Performance::Frame];
    Log("GPU bench: everything on %.3f ms (control at the end %.3f ms)", total, m_bench.results.back().stages[Performance::Frame]);
    Log("  stages: shadow casters %.3f, AO+shafts %.3f, composition %.3f, water copy %.3f, temporal %.3f, upscale %.3f",
        base.stages[Performance::Shadow], base.stages[Performance::Effects], base.stages[Performance::Resolve],
        base.stages[Performance::WaterCopy], base.stages[Performance::Temporal], base.stages[Performance::Upscale]);
    for (size_t i = 1; i + 1 < m_bench.results.size(); i++) {
        const GpuBench::Result& r = m_bench.results[i];
        Log("  without %-45s %.3f ms (costs %+.3f ms, %u samples)", r.name.c_str(), r.stages[Performance::Frame],
            total - r.stages[Performance::Frame], r.samples);
    }
}

