// overlay.cpp - in-game settings panel (Dear ImGui, DX11 backend), drawn over the final image in
// Present. [Overlay] Key (Insert by default) opens and closes it.
//
// Input: the game reads mouse and keyboard through DirectInput (TInputDXInterface.dll). Our
// dinput8.dll proxy reports every mouse read here (D3D8to11_MouseInput) and, while the panel is
// open, hands the game empty mouse/keyboard state (D3D8to11_InputBlocked). The panel moves its
// own cursor with those mouse deltas; if no DirectInput mouse is seen it falls back to the
// Windows cursor.
#include "device.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_dx11.h"
#include <cmath>

namespace {

struct OverlayState {
    bool initialized = false, failed = false;
    volatile LONG open = 0;
    bool keyWasDown = false;
    // DirectInput mouse feed (game thread, same as the render thread in JPOG)
    volatile LONG dx = 0, dy = 0, wheel = 0;
    volatile LONG buttons = 0;
    volatile DWORD lastFeed = 0;
    float cursorX = 0, cursorY = 0;
    LARGE_INTEGER lastTime = {};
    float fps = 0, frameMs = 0;
    float azimuth = 225, elevation = 55; // fixed sun, degrees
    char status[96] = {};
    DWORD statusUntil = 0;
} g_ov;

void SetStatus(const char* text)
{
    strcpy_s(g_ov.status, text);
    g_ov.statusUntil = GetTickCount() + 2500;
}

// Y-up direction from azimuth/elevation (degrees) into [Lighting] SunX/Y/Z.
void SunFromAngles()
{
    const float az = g_ov.azimuth * 3.14159265f / 180, el = g_ov.elevation * 3.14159265f / 180;
    g_config.sunDirection[0] = std::cos(el) * std::sin(az);
    g_config.sunDirection[1] = std::sin(el);
    g_config.sunDirection[2] = std::cos(el) * std::cos(az);
}

void AnglesFromSun(float x, float yUp, float z)
{
    g_ov.elevation = std::asin(yUp < -1 ? -1 : (yUp > 1 ? 1 : yUp)) * 180 / 3.14159265f;
    float az = std::atan2(x, z) * 180 / 3.14159265f;
    g_ov.azimuth = az < 0 ? az + 360 : az;
}

const char* kDebugViews[] = { "Off", "Albedo (replacement)", "Normals (replacement)", "Shadow mask",
                              "Depth normals", "Mask + shadow map", "World grid", "Raw visibility",
                              "Dump one frame", "Ambient occlusion only", "Light shafts only", "Normal maps (world normals)",
                              "Skin relief self-shadow",
                              "Draw kinds (red skinned, green terrain, blue rigid, yellow grass, grey other)" };

} // namespace

extern "C" BOOL WINAPI D3D8to11_MouseInput(LONG dx, LONG dy, LONG wheel, DWORD buttons)
{
    InterlockedExchangeAdd(&g_ov.dx, dx);
    InterlockedExchangeAdd(&g_ov.dy, dy);
    InterlockedExchangeAdd(&g_ov.wheel, wheel);
    InterlockedExchange(&g_ov.buttons, (LONG)buttons);
    g_ov.lastFeed = GetTickCount();
    return g_ov.open ? TRUE : FALSE;
}

extern "C" BOOL WINAPI D3D8to11_InputBlocked()
{
    return g_ov.open ? TRUE : FALSE;
}

void Device::DrawOverlay()
{
    if (g_ov.failed) return;
    // Toggle key (only while the game window is in front).
    const bool focused = GetForegroundWindow() == m_window;
    const bool down = focused && (GetAsyncKeyState(g_config.overlayKey) & 0x8000) != 0;
    if (down && !g_ov.keyWasDown) {
        InterlockedExchange(&g_ov.open, g_ov.open ? 0 : 1);
        g_ov.cursorX = m_swapW * 0.5f;
        g_ov.cursorY = m_swapH * 0.5f;
        if (g_ov.open) {
            const float* s = g_config.sunFromGame ? m_sun.direction : nullptr;
            if (s) AnglesFromSun(s[0], -s[1], s[2]);
            else AnglesFromSun(g_config.sunDirection[0], g_config.sunDirection[1], g_config.sunDirection[2]);
        }
    }
    g_ov.keyWasDown = down;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_ov.lastTime.QuadPart ? (float)(now.QuadPart - g_ov.lastTime.QuadPart) / m_qpcFreq.QuadPart : 1 / 60.0f;
    g_ov.lastTime = now;
    if (dt > 0 && dt < 1) {
        g_ov.frameMs += (dt * 1000 - g_ov.frameMs) * 0.05f;
        g_ov.fps = g_ov.frameMs > 0 ? 1000 / g_ov.frameMs : 0;
    }
    const LONG dx = InterlockedExchange(&g_ov.dx, 0), dy = InterlockedExchange(&g_ov.dy, 0);
    const LONG wheel = InterlockedExchange(&g_ov.wheel, 0);
    if (!g_ov.open && !g_config.showFps) return;

    if (!g_ov.initialized) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        ImFontConfig font;
        font.SizePixels = std::floor(15.0f * g_config.overlayScale * (m_swapH > 1200 ? m_swapH / 1080.0f : 1.0f));
        io.Fonts->AddFontDefault(&font);
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6; style.FrameRounding = 4; style.GrabRounding = 4;
        style.Colors[ImGuiCol_WindowBg].w = 0.92f;
        if (!ImGui_ImplDX11_Init(m_device.Get(), m_ctx.Get())) {
            Log("overlay: ImGui DX11 backend init failed");
            g_ov.failed = true;
            ImGui::DestroyContext();
            return;
        }
        g_ov.initialized = true;
        Log("overlay ready (key 0x%02x)", g_config.overlayKey);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)m_swapW, (float)m_swapH);
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1 / 60.0f;
    io.MouseDrawCursor = g_ov.open != 0;
    if (g_ov.open) {
        bool left, right;
        if (GetTickCount() - g_ov.lastFeed < 500) {
            // DirectInput deltas (mickeys), roughly matched to the desktop cursor speed.
            g_ov.cursorX += dx * 1.2f;
            g_ov.cursorY += dy * 1.2f;
            left = (g_ov.buttons & 1) != 0;
            right = (g_ov.buttons & 2) != 0;
            if (wheel) io.AddMouseWheelEvent(0, wheel / 120.0f);
        } else {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(m_window, &p);
            RECT rc;
            GetClientRect(m_window, &rc);
            g_ov.cursorX = rc.right > 0 ? p.x * (float)m_swapW / rc.right : (float)p.x;
            g_ov.cursorY = rc.bottom > 0 ? p.y * (float)m_swapH / rc.bottom : (float)p.y;
            left = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
            right = focused && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        }
        g_ov.cursorX = g_ov.cursorX < 0 ? 0 : (g_ov.cursorX > m_swapW - 1 ? m_swapW - 1.0f : g_ov.cursorX);
        g_ov.cursorY = g_ov.cursorY < 0 ? 0 : (g_ov.cursorY > m_swapH - 1 ? m_swapH - 1.0f : g_ov.cursorY);
        io.AddMousePosEvent(g_ov.cursorX, g_ov.cursorY);
        io.AddMouseButtonEvent(0, left);
        io.AddMouseButtonEvent(1, right);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        io.AddMouseButtonEvent(1, false);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    if (!g_ov.open) {
        ImGui::SetNextWindowPos(ImVec2(8, (float)m_swapH - 8), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("%.0f FPS  %.2f ms", g_ov.fps, g_ov.frameMs);
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(470 * g_config.overlayScale, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("JPOG Modernize - D3D11 renderer", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if(ImGui::Button(g_config.effectsEnabled ? "Desactivar todos los efectos" : "Activar todos los efectos",ImVec2(-1,0))) {
            g_config.effectsEnabled=!g_config.effectsEnabled;
            if(!g_config.effectsEnabled)m_waterInteraction.Clear();
        }
        ImGui::SetItemTooltip("Sombras, AO, luz, color, normales, agua, pasto, per-pixel, TAA y FSR1. Conserva tus ajustes y las opciones de pantalla.");
        if(!g_config.effectsEnabled)ImGui::TextDisabled("Efectos pausados. Los ajustes se conservan.");
        ImGui::Text("%.0f FPS  (%.2f ms)   %ux%u", g_ov.fps, g_ov.frameMs, m_swapW, m_swapH);
        ImGui::TextDisabled("Changes apply immediately. Save writes d3d8to11.ini.");
        ImGui::Separator();

        if(ImGui::CollapsingHeader("Rendimiento y recursos")) {
            ImGui::Checkbox("Medir GPU (1 cuadro de cada 30)",&g_config.performanceGPU);
            ImGui::Checkbox("Guardar CSV de rendimiento (1/seg)",&g_config.performanceLog);
            if(m_perf.csvFailed)ImGui::TextColored(ImVec4(1,.4f,.3f,1),"No se pudo escribir el CSV.");
            else if(!m_perf.csvPath.empty()){ImGui::TextWrapped("CSV: %s",m_perf.csvPath.c_str());}
            ImGui::Text("Cuadro: %.2f ms  Pico: %.2f ms",m_perf.frameMs,m_perf.peakMs);
            ImGui::PlotLines("ms/cuadro",m_perf.history,180,m_perf.historyAt%180,nullptr,0,50,ImVec2(0,65));
            ImGui::Text("CPU espera Present: %.2f ms | Limite FPS: %.2f ms",m_perf.presentMs,m_perf.limiterMs);
            if(m_perf.failed)ImGui::TextDisabled("Queries GPU no disponibles.");
            else if(m_perf.enabled && m_perf.valid) {
                ImGui::Text("GPU render completo: %.3f ms",m_perf.gpu[Performance::Frame]);
                ImGui::Text("GPU AO + rayos: %.3f ms",m_perf.gpu[Performance::Effects]);
                ImGui::Text("GPU composicion sombras/color: %.3f ms",m_perf.gpu[Performance::Resolve]);
                ImGui::Text("GPU copias para agua: %.3f ms",m_perf.gpu[Performance::WaterCopy]);
                ImGui::Text("GPU temporal (TAA/FSR31/FG): %.3f ms | FSR1: %.3f ms",m_perf.gpu[Performance::Temporal],m_perf.gpu[Performance::Upscale]);
            } else ImGui::TextDisabled(m_perf.enabled ? "Esperando muestra GPU valida..." : "Medicion GPU apagada.");
            ImGui::TextDisabled("Tiempos GPU incluidos en el total; no sumarlos.");
            ImGui::TextDisabled("Agua/normales/pasto/sombras de geometria incluidos en total.");
            ImGui::Text("Muestras GPU descartadas/ocupadas: %u",m_perf.skipped);
            const double mib=1048576.0;
            if(m_perf.memoryValid) {
                ImGui::Text("RAM privada: %.1f MiB | Pico: %.1f",m_perf.privateBytes/mib,m_perf.peakPrivate/mib);
                ImGui::Text("RAM residente: %.1f MiB | Delta base: %+.1f",m_perf.workingSet/mib,
                    (double(m_perf.privateBytes)-double(m_perf.basePrivate))/mib);
            }
            if(m_perf.budgetValid)ImGui::Text("GPU local proceso: %.1f / %.1f MiB presupuesto",m_perf.gpuUsage/mib,m_perf.gpuBudget/mib);
            else ImGui::TextDisabled("Presupuesto GPU no disponible en este driver.");
            ImGui::Text("Draws: %u | Agua: %u | Sombras extra: %u",m_perfDraws,m_perfWaterDraws,m_perfShadowDraws);
            ImGui::Text("Objetos vivos: superficies %u, texturas %u, volumen %u",
                ResourceCount::live[0].load(),ResourceCount::live[1].load(),ResourceCount::live[2].load());
            ImGui::Text("Buffers vivos: vertices %u, indices %u",ResourceCount::live[3].load(),ResourceCount::live[4].load());
            ImGui::Text("Caches: blend %u, depth %u, raster %u, sampler %u",
                unsigned(m_blendStates.size()),unsigned(m_depthStates.size()),unsigned(m_rasterStates.size()),unsigned(m_samplers.size()));
            unsigned vs=0,ps=0,variants=0;
            for(auto* s:m_vertexShaders)if(s)++vs;
            for(auto* s:m_pixelShaders)if(s){++ps;variants+=unsigned(s->variants.size());}
            ImGui::Text("Shaders vivos: VS %u, PS %u | Variantes %u + FF %u",vs,ps,variants,unsigned(m_ffShaders.size()));
            ImGui::Text("Buffers dinamicos reservados: %.2f MiB",(double(m_dynVBSize)+m_dynIBSize)/mib);
            if(ImGui::Button("Reiniciar base y pico")) {
                m_perf.basePrivate=m_perf.peakPrivate=m_perf.privateBytes;m_perf.peakMs=0;
            }
            ImGui::TextDisabled("RAM incluye TODO el juego. Crecimiento no prueba un leak.");
            ImGui::TextDisabled("Comparar la misma escena; base tras terminar de cargar.");
        }

        if (ImGui::CollapsingHeader("Sun shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled", &g_config.sunLighting);
            ImGui::BeginDisabled(!g_config.sunLighting);
            ImGui::SliderFloat("Strength", &g_config.shadowStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Distance", &g_config.shadowDistance, 50.0f, 1000.0f, "%.0f units");
            ImGui::SetItemTooltip("How far from the camera shadows are drawn. Lower = sharper.");
            static const int sizes[] = { 1024, 2048, 4096, 8192 };
            int current = 2;
            for (int i = 0; i < 4; i++) if (sizes[i] == g_config.shadowResolution) current = i;
            const char* labels[] = { "1024", "2048", "4096", "8192" };
            if (ImGui::Combo("Map resolution", &current, labels, 4)) g_config.shadowResolution = sizes[current];
            ImGui::Checkbox("Terrain casts shadows", &g_config.terrainShadows);
            ImGui::SameLine();
            ImGui::Checkbox("Grass casts shadows", &g_config.grassShadows);
            ImGui::Text("Map %ux%u, %.3f units/texel, %u casters", m_sunMapSize, m_sunMapSize, m_sunTexelWorld, m_lastCasters);
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Sun direction", ImGuiTreeNodeFlags_DefaultOpen)) {
            int source = g_config.sunFromGame ? 0 : 1;
            bool changed = ImGui::RadioButton("Follow the game", &source, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Manual", &source, 1);
            if (changed) {
                if (source == 1 && g_config.sunFromGame) {
                    AnglesFromSun(m_sun.direction[0], -m_sun.direction[1], m_sun.direction[2]);
                    SunFromAngles();
                }
                g_config.sunFromGame = source == 0;
            }
            ImGui::BeginDisabled(g_config.sunFromGame);
            if (ImGui::SliderFloat("Azimuth", &g_ov.azimuth, 0, 360, "%.0f deg")) SunFromAngles();
            if (ImGui::SliderFloat("Height", &g_ov.elevation, 5, 90, "%.0f deg")) SunFromAngles();
            ImGui::EndDisabled();
            float minDeg = std::asin(g_config.sunMinElevation) * 180 / 3.14159265f;
            if (ImGui::SliderFloat("Minimum height", &minDeg, 3, 85, "%.0f deg"))
                g_config.sunMinElevation = std::sin(minDeg * 3.14159265f / 180);
            ImGui::SetItemTooltip("A lower sun gives longer shadows. The game's sun is raised to this.");
            ImGui::Text("Current sun (Y up): %.2f %.2f %.2f", m_sun.direction[0], -m_sun.direction[1], m_sun.direction[2]);
            ImGui::Checkbox("Time of day (preview)", &g_config.timeOfDay);
            ImGui::SetItemTooltip("Moves the sun through the day and the moon at night. Best with Replace the game's lighting.");
            ImGui::BeginDisabled(!g_config.timeOfDay);
            int hh = (int)g_config.timeOfDayHour, mm = (int)((g_config.timeOfDayHour - hh) * 60);
            char label[32]; sprintf_s(label, "%02d:%02d", hh, mm);
            ImGui::SliderFloat("Hour", &g_config.timeOfDayHour, 0, 23.99f, label);
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Time of day tuning")) {
            ImGui::TextDisabled("Night");
            ImGui::ColorEdit3("Moon colour", g_config.moonColor, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Moon light", &g_config.moonIntensity, 0, 3, "%.2f");
            ImGui::SliderFloat("Moon rim", &g_config.moonRim, 0, 4, "%.2f");
            ImGui::SliderFloat("Moon sheen", &g_config.moonSheen, 0, 4, "%.2f");
            ImGui::SliderFloat("Moon on grass", &g_config.grassMoon, 0, 4, "%.2f");
            ImGui::SliderFloat("Moon on flat surfaces", &g_config.nightSurface, 0, 1, "%.2f");
            ImGui::SetItemTooltip("How much the moon lights grass and water as a whole (their tips and edges catch it separately).");
            ImGui::ColorEdit3("Night ambient", g_config.nightAmbient, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Night sky", &g_config.nightSky, 0, 4, "%.2f");
            ImGui::SliderFloat("Night desaturation", &g_config.nightDesaturate, 0, 1, "%.2f");
            ImGui::SliderFloat("Stars", &g_config.stars, 0, 4, "%.2f");
            ImGui::TextDisabled("Dawn and dusk");
            ImGui::ColorEdit3("Low sun colour", g_config.lowSunColor, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Dusk ambient", g_config.duskAmbient, ImGuiColorEditFlags_Float);
            ImGui::TextDisabled("Eye adaptation");
            ImGui::SliderFloat("Max exposure", &g_config.maxExposure, 1, 6, "%.2f");
            ImGui::SetItemTooltip("How much dawn, dusk and night are brightened at most.");
            ImGui::TextDisabled("Save writes these to [TimeOfDay] in d3d8to11.ini.");
        }

        if (ImGui::CollapsingHeader("Sunlight & colour", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##post", &g_config.postEnabled);
            ImGui::BeginDisabled(!g_config.postEnabled);
            ImGui::SliderFloat("Sunlight", &g_config.sunBoost, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Warm extra light on everything the sun reaches (outside shadows).");
            ImGui::SliderFloat("Cool shadows", &g_config.shadowCool, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Exposure", &g_config.exposure, 0.5f, 2.0f, "%.2f");
            ImGui::SliderFloat("Contrast", &g_config.contrast, 0.5f, 1.5f, "%.2f");
            ImGui::SliderFloat("Saturation", &g_config.saturation, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Warmth", &g_config.warmth, -1.0f, 1.0f, "%.2f");
            if (ImGui::Button("Defaults##post")) {
                g_config.sunBoost = 0.25f; g_config.shadowCool = 0.5f; g_config.exposure = 1.0f;
                g_config.contrast = 0.97f; g_config.saturation = 1.09f; g_config.warmth = 0.23f;
            }
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Ambient occlusion & light shafts", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Ambient occlusion (HBAO)", &g_config.aoEnabled);
            ImGui::BeginDisabled(!g_config.aoEnabled);
            ImGui::SliderFloat("AO radius", &g_config.aoRadius, 0.3f, 8.0f, "%.1f units");
            ImGui::SliderFloat("AO strength", &g_config.aoStrength, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Light shafts (volumetric sun)", &g_config.shafts);
            ImGui::SetItemTooltip("Needs sun shadows. Strongest when looking toward the sun.");
            ImGui::BeginDisabled(!g_config.shafts || !g_config.sunLighting);
            ImGui::SliderFloat("Shaft strength", &g_config.shaftStrength, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Scattering", &g_config.shaftScatter, 0.0f, 0.95f, "%.2f");
            ImGui::SetItemTooltip("0 = even haze, higher = tighter glow around the sun.");
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Normal maps")) {
            ImGui::Checkbox("Enabled##normals", &g_config.normalMaps);
            ImGui::SetItemTooltip("Relief from <texture>_n.dds files in mods\\textures (tools/make_normals.py).");
            ImGui::BeginDisabled(!g_config.normalMaps);
            ImGui::SliderFloat("Relief", &g_config.normalStrength, 0.0f, 2.0f, "%.2f");
            ImGui::Checkbox("Flip green (OpenGL maps)", &g_config.normalFlipGreen);
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Depth-aware water",&g_config.waterEnabled);
            ImGui::BeginDisabled(!g_config.waterEnabled);
            const char* surfaces[]={"Automatico: oceano / lagunas","Todo laguna (manual)","Todo oceano (manual)"};
            ImGui::Combo("Tipo de agua",&g_config.waterSurfaceMode,surfaces,3);
            ImGui::SetItemTooltip("Auto usa el terreno nativo completo: agua conectada al borde = oceano; encerrada por tierra = laguna. No depende de la camara.");
            ImGui::SliderFloat("Espuma de lagunas",&g_config.waterLagoonFoam,0,1,"x%.2f");
            ImGui::SliderFloat("Ondas de lagunas",&g_config.waterLagoonRipples,0,2,"x%.2f");
            ImGui::SliderFloat("Ondas del oceano",&g_config.waterOceanRipples,0,3,"x%.2f");
            ImGui::SliderFloat("Altura del oleaje",&g_config.waterSwellAmplitude,0,2,"%.2f units");
            ImGui::SetItemTooltip("Desplaza vertices del oceano; 0 desactiva. Auto: frentes hacia la costa segun distancia a tierra. Lagunas sin desplazamiento. Limitado por la malla original.");
            ImGui::SliderFloat("Longitud de olas",&g_config.waterSwellLength,.5f,4,"x%.2f");
            ImGui::SliderFloat("Suavizado en costa",&g_config.waterSwellCoastDepth,.5f,10,"%.1f units");
            ImGui::SliderFloat("Espuma en crestas",&g_config.waterCrestFoam,0,1,"%.2f");
            if(g_config.waterSurfaceMode==0){
                if(m_waterRegionReady)ImGui::Text("Mapa nativo %ux%u: mar %u, laguna %u muestras",m_waterRegions.width,m_waterRegions.height,m_waterRegions.oceanCells,m_waterRegions.pondCells);
                else if(m_waterNativeFault)ImGui::TextColored(ImVec4(1,.4f,.3f,1),"Lectura nativa detenida: perfil oceano / override manual.");
                else if(!m_waterNativeAvailable)ImGui::TextDisabled("Sin mapa nativo aun: perfil oceano hasta obtenerlo.");
                else if(!m_waterRegionW)ImGui::TextColored(ImVec4(1,.7f,.2f,1),"Esperando altura de agua: azul = fallback, NO clasificacion.");
                else ImGui::Text("Leyendo terreno: %u / %u muestras",m_waterRegionNext,m_waterRegionW*m_waterRegionH);
                ImGui::Text("Ultimo paso CPU del mapa: %.3f ms",m_waterRegionCpuMs);
                ImGui::Text("Nivel nativo muestreado: %.3f",m_waterRegionLevel);
                if(ImGui::Button("Actualizar mapa de agua"))m_waterRegionRefresh=0;
            }
            ImGui::SliderFloat("Absorption depth",&g_config.waterDepthScale,0.5f,20,"%.1f units");
            ImGui::SliderFloat("Shallow transparency",&g_config.waterTransparency,0,1,"%.2f");
            ImGui::SliderFloat("Fresnel reflections",&g_config.waterReflection,0,1,"%.2f");
            ImGui::Checkbox("Scene reflections (SSR)",&g_config.waterSSR);
            ImGui::SliderFloat("Scene reflection strength",&g_config.waterSSRStrength,0,1,"%.2f");
            ImGui::SliderFloat("Reflection distance",&g_config.waterSSRDistance,5,250,"%.0f units");
            ImGui::SliderInt("Reflection steps",&g_config.waterSSRSteps,8,48);
            ImGui::SetItemTooltip("Higher = more detail and GPU cost. Only reflects visible objects in the opaque snapshot.");
            ImGui::SliderFloat("Shore foam",&g_config.waterFoam,0,1,"%.2f");
            ImGui::SliderFloat("Foam depth band",&g_config.waterFoamWidth,0.05f,3,"%.2f units");
            ImGui::SliderFloat("Refraction distortion",&g_config.waterDistortion,0,10,"%.1f px");
            ImGui::SliderFloat("Ripple strength",&g_config.waterRipples,0,3,"%.2f");
            ImGui::SliderFloat("Wave speed",&g_config.waterSpeed,0,3,"%.2f");
            ImGui::Checkbox("Creature ripples and wakes",&g_config.waterInteraction);
            ImGui::SliderFloat("Interaction strength",&g_config.waterInteractionStrength,0,3,"%.2f");
            ImGui::Text("%u tracked ripple pulses",m_waterInteraction.ActiveRipples(WaterTime()));
            const char* debug[]={"Shaded","Water thickness","Fresnel","Foam","Creature interaction","Scene reflection hits","Ocean blue / lagoon green","Swell height"};
            ImGui::Combo("Water debug",&g_config.waterDebug,debug,8);
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Tessellation", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Smooth model silhouettes (PN triangles)", &g_config.tessellation);
            ImGui::Checkbox("Dinosaurs only", &g_config.tessDinosOnly);
            ImGui::SliderFloat("Max subdivision", &g_config.tessMaxFactor, 1, 16, "%.0f");
            ImGui::SliderFloat("Pixels per edge", &g_config.tessPixels, 2, 64, "%.0f");
            ImGui::SetItemTooltip("Smaller = more triangles. Edges are split until a piece is about this long on screen.");
            ImGui::SliderFloat("Curvature", &g_config.tessCurvature, 0, 1.5f, "%.2f");
            ImGui::Checkbox("Weld split vertices", &g_config.tessWeld);
            ImGui::SetItemTooltip("Vertices duplicated at seams share one curve, so limbs do not crack open.");
            ImGui::Checkbox("Flip winding", &g_config.tessFlipWinding);
            ImGui::SetItemTooltip("Enable if tessellated models look inside out.");
            ImGui::SliderFloat("Relief height", &g_config.tessDisplacement, 0, 0.2f, "%.3f");
            ImGui::SetItemTooltip("Real displacement from the texture, in world units. 0 = off.");
            ImGui::SliderFloat("Relief softness", &g_config.tessDetailMip, 0, 4, "%.1f");
            ImGui::Checkbox("Invert relief", &g_config.tessInvertRelief);
            ImGui::Checkbox("Seam-safe relief", &g_config.tessSeamSafe);
            ImGui::SetItemTooltip("Fades the relief at the original triangle edges so texture seams cannot crack.");
            ImGui::Text("%u tessellated draws this frame", m_tessDrawsShown);
            if (m_tessFailed) ImGui::TextColored(ImVec4(1, .6f, .2f, 1), "Tessellation disabled after a failure: see the log.");
        }
        if (ImGui::CollapsingHeader("Skin relief (parallax)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Parallax and self-shadowing on dinosaur skins", &g_config.reliefEnabled);
            ImGui::BeginDisabled(!g_config.reliefEnabled);
            ImGui::SliderFloat("Depth (texels)", &g_config.reliefDepth, 0.5f, 12, "%.1f");
            ImGui::SetItemTooltip("Height of the brightest detail above the darkest, in texture pixels.");
            ImGui::SliderFloat("Parallax", &g_config.reliefParallax, 0, 1, "%.2f");
            ImGui::SetItemTooltip("How much the texture shifts with the view angle. 0 = only shadows.");
            ImGui::SliderFloat("Self-shadow", &g_config.reliefShadow, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Scales and folds shade their neighbours away from the sun.");
            ImGui::SliderFloat("Shadow softness", &g_config.reliefSoftness, 0.25f, 4, "%.2f");
            ImGui::Checkbox("Invert height", &g_config.reliefInvert);
            ImGui::EndDisabled();
            ImGui::Text("%u relief draws this frame", m_reliefDrawsShown);
        }
        if (ImGui::CollapsingHeader("Interactive grass", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Creatures bend the grass", &g_config.grassPhysics);
            ImGui::BeginDisabled(!g_config.grassPhysics);
            ImGui::SliderFloat("Bend", &g_config.grassPush, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Flatten", &g_config.grassFlatten, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Radius", &g_config.grassRadius, 0.2f, 4.0f, "x%.2f");
            ImGui::SliderInt("Min. bones", &g_config.grassMinBones, 1, 40);
            ImGui::SetItemTooltip("Skinned models with fewer bones (props, trees) do not push grass.");
            ImGui::Text("%u creatures pushing grass this frame", m_pusherCount);
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Display")) {
            ImGui::SliderInt("FPS limit", &g_config.fpsLimit, 0, 360, g_config.fpsLimit ? "%d" : "Off");
            bool vsync = g_config.syncInterval != 0;
            if (ImGui::Checkbox("VSync", &vsync)) g_config.syncInterval = vsync ? 1 : 0;
            static const int anisos[] = { 1, 2, 4, 8, 16 };
            int a = 0;
            for (int i = 0; i < 5; i++) if (anisos[i] <= g_config.forceAnisotropy) a = i;
            const char* alabels[] = { "Game setting", "2x", "4x", "8x", "16x" };
            if (ImGui::Combo("Anisotropic filtering", &a, alabels, 5)) g_config.forceAnisotropy = anisos[a];
            static const int msaaValues[] = { 1, 2, 4, 8 };
            int m = 0;
            for (int i = 0; i < 4; i++) if (msaaValues[i] <= g_config.msaa) m = i;
            const char* mlabels[] = { "Off", "2x", "4x", "8x" };
            if (ImGui::Combo("Antialiasing (MSAA)", &m, mlabels, 4)) g_config.msaa = msaaValues[m];
            ImGui::Checkbox("Smooth foliage edges (alpha to coverage)", &g_config.alphaToCoverage);
            ImGui::Checkbox("Apply game brightness/gamma", &g_config.applyGamma);
            ImGui::Checkbox("FPS counter when closed", &g_config.showFps);
        }

        if(ImGui::CollapsingHeader("Materiales piloto: Triceratops / hamburgueseria",ImGuiTreeNodeFlags_DefaultOpen)){
            ImGui::Checkbox("Materiales por pixel (comparar on/off)",&g_config.materialsEnabled);
            ImGui::Text("Dibujos esta imagen: Triceratops %u / hamburguesa %u",m_materialDraws[1],m_materialDraws[2]);
            ImGui::Checkbox("Diagnostico: Triceratops magenta / hamburguesa cian",&m_materialDebug);
            ImGui::SetItemTooltip("Color plano solo sobre las texturas piloto. Tricera magenta, hamburguesa cian. Si no cambia de color y el contador es cero, el material no llega al objeto.");
            {
                const int n=(int)m_skinListPrev.size();
                if(m_pickSkin>=n)m_pickSkin=n-1;
                ImGui::SliderInt("Buscar piel (rojo)",&m_pickSkin,-1,n>0?n-1:0,m_pickSkin<0?"apagado":"%d");
                ImGui::SetItemTooltip("Recorre las texturas de modelos animados dibujadas en pantalla; la elegida se pinta de rojo.");
                if(m_pickSkin>=0 && m_pickSkin<n){
                    Texture* t=m_skinListPrev[m_pickSkin];
                    ImGui::Text("%s",t->m_name.empty()?"(sin nombre)":t->m_name.c_str());
                    ImGui::Text("%ux%u fmt %lu, normal: %s",t->m_width,t->m_height,t->m_format,t->m_normalName.empty()?"ninguno":t->m_normalName.c_str());
                    static int logged=-2;
                    if(logged!=m_pickSkin){logged=m_pickSkin;Log("Skin picker %d (%s): %ux%u fmt %lu levels %u pool %lu normal %s ptr %p",m_pickSkin,t->m_name.c_str(),t->m_width,t->m_height,t->m_format,t->m_levels,t->m_pool,t->m_normalName.empty()?"none":t->m_normalName.c_str(),(void*)t);}
                }
            }
            ImGui::SliderFloat("Rugosidad de piel",&g_config.materialSkinRoughness,.1f,1,"%.2f");
            ImGui::SliderFloat("Rugosidad del adorno",&g_config.materialBuildingRoughness,.1f,1,"%.2f");
            ImGui::SliderFloat("Reflejo especular del sol",&g_config.materialSpecular,0,2,"%.2f");
            ImGui::SliderFloat("Ambiente del cielo aproximado",&g_config.materialEnvironment,0,1,"%.2f");
            ImGui::SliderFloat("Relieve del material",&g_config.materialNormalStrength,0,2,"%.2f");
            ImGui::TextDisabled("Solo dos texturas piloto; no afecta pasto, HUD ni otros edificios.");
            ImGui::TextDisabled("Rugosidad uniforme por textura; aun sin mascaras piel/cuernos ni cubemaps.");
            if(m_materialFailed)ImGui::TextColored(ImVec4(1,.6f,.2f,1),"Materiales desactivados por fallo: revisar log.");
        }
        if(ImGui::CollapsingHeader("Antialiasing temporal y FSR",ImGuiTreeNodeFlags_DefaultOpen)) {
            if(ImGui::Checkbox("FSR 3.1 temporal (experimental, Native AA)",&g_config.fsr31)){m_taaValid=false;m_viewportDirty=true;}
            ImGui::SetItemTooltip("SDK DX11 x86 real. Resolucion nativa, sin generacion de frames. Movimiento de camara; dinosaurios/agua con mascara reactiva conservadora. No reduce aun el coste de renderizado.");
            if(m_fsr31Failed)ImGui::TextColored(ImVec4(1,.6f,.2f,1),"FSR31 fallo: TAA/FSR1 de respaldo.");
            else if(m_fsr31)ImGui::Text("FSR31: %s",m_fsr31Frame+1>=m_frame?"activo":"preparado");
            if(ImGui::Button("Reintentar / reiniciar FSR31")){ReleaseFsr31();m_fsr31Failed=false;}
            ImGui::TextDisabled("Primera etapa: AA nativo; faltan vectores de animacion completos.");
            if(ImGui::Checkbox("Frame Generation x2 (prototipo sincrono)",&g_config.frameGeneration)){
                m_taaValid=false;m_frameGenReady=false;
                if(g_config.frameGeneration){g_config.fsr31=true;g_config.syncInterval=1;m_frameGenFailed=false;}
            }
            ImGui::SetItemTooltip("SDK optical flow + interpolacion. Activa VSync y FSR31. Puede aumentar latencia; requiere salida nativa. HUD semitransparente aproximado. Apagado por defecto.");
            ImGui::Text("Frames generados presentados: %llu",m_generatedPresents);
            ImGui::Checkbox("Diagnostico: ver frame intermedio (sin x2)",&g_config.frameGenerationDebug);
            if(g_config.frameGeneration && g_config.syncInterval!=1)ImGui::TextDisabled("Frame Gen requiere VSync activo.");
            if(g_config.frameGeneration && (m_backBuffer->m_width!=m_swapW || m_backBuffer->m_height!=m_swapH))ImGui::TextDisabled("Frame Gen requiere entrada y ventana del mismo tamano.");
            if(m_frameGenFailed)ImGui::TextColored(ImVec4(1,.6f,.2f,1),"Frame Gen fallo: se conservan frames reales y FSR31.");
            if(ImGui::Checkbox("TAA (experimental, mundo sin HUD)",&g_config.taa))m_taaValid=false;
            ImGui::SliderFloat("Peso del historial",&g_config.taaHistory,0,.9f,"%.2f");
            ImGui::SetItemTooltip("Mas alto reduce parpadeo, pero puede dejar rastros. No hay vectores de movimiento por dinosaurio.");
            ImGui::Checkbox("AMD FSR 1: EASU + RCAS",&g_config.fsr1);
            ImGui::SliderFloat("Nitidez RCAS",&g_config.fsrSharpness,0,1,"%.2f");
            ImGui::Text("Entrada %ux%u -> salida %ux%u",m_backBuffer->m_width,m_backBuffer->m_height,m_swapW,m_swapH);
            if(m_taaFailed)ImGui::TextDisabled("TAA fallo: se conserva MSAA, sin jitter.");
            else ImGui::Text("TAA historial: %s",m_taaValid ? "activo":"sin historial / apagado");
            if(m_fsrFailed)ImGui::TextDisabled("FSR1 no disponible: revisar shaders/fsr1 y el log.");
            else if(m_backBuffer->m_width==m_swapW && m_backBuffer->m_height==m_swapH)
                ImGui::TextDisabled("Resolucion nativa: RCAS, sin beneficio de reescalado.");
            else ImGui::TextDisabled("EASU requiere entrada menor y misma relacion de aspecto.");
            ImGui::TextDisabled("FSR1 no cambia la resolucion interna del juego.");
            ImGui::TextDisabled("DLSS no disponible: build x86, requiere GPU NVIDIA RTX.");
            ImGui::TextDisabled("Frame Gen sin scheduler asincrono; no acelera simulacion.");
            if(ImGui::Button("Reiniciar historial TAA"))m_taaValid=false;
        }

        if (ImGui::CollapsingHeader("Experimental")) {
            ImGui::Checkbox("Replace the game's lighting", &g_config.replaceLighting);
            ImGui::SetItemTooltip("Per-pixel relighting of models and terrain from the game's own light colours.");
            ImGui::BeginDisabled(!g_config.replaceLighting);
            ImGui::SliderFloat("Sun", &g_config.relightSun, 0, 3, "x%.2f");
            ImGui::SliderFloat("Ambient##relight", &g_config.relightAmbient, 0, 3, "x%.2f");
            ImGui::SliderFloat("Sky tint", &g_config.relightSkyTint, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Blue sky light on faces looking up, warm ground bounce on faces looking down.");
            ImGui::SliderFloat("Exposure##relight", &g_config.relightExposure, 0.25f, 4, "%.2f");
            ImGui::Checkbox("Soft highlights (tonemap)", &g_config.relightTonemap);
            ImGui::Checkbox("Procedural sky", &g_config.proceduralSky);
            ImGui::SetItemTooltip("Replaces the game's sky dome by day and night.");
            ImGui::Checkbox("Terrain from height field", &g_config.heightfieldLighting);
            ImGui::SetItemTooltip("Smooth terrain normals from the game's height field instead of its baked light.");
            ImGui::SliderFloat("Hill shadows", &g_config.horizonShadows, 0, 1, "%.2f");
            ImGui::SliderFloat("Rim light", &g_config.rimLight, 0, 2, "%.2f");
            ImGui::SliderFloat("Grass saturation", &g_config.grassSaturation, 0, 2, "%.2f");
            ImGui::SliderFloat("Grass brightness", &g_config.grassBrightness, 0, 2, "%.2f");
            ImGui::SetItemTooltip("The sun outlines silhouettes, mostly against the light. The moon's rim is separate.");
            ImGui::SliderFloat("Baked detail", &g_config.bakedTerrainDetail, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Keeps the shading the game painted into the terrain (under trees, hollows).");
            ImGui::Text(m_terrainReady ? "Height field: %ux%u" : "Height field: loading (%ux%u)", m_terrainW, m_terrainH);
            ImGui::EndDisabled();
            ImGui::SliderFloat("Pilot ambient", &g_config.sunAmbient, 0.1f, 2.0f, "%.2f");
            ImGui::SetItemTooltip("Material pilot light when the replacement is off.");
            ImGui::SliderFloat("Pilot sun", &g_config.sunDirect, 0.0f, 2.0f, "%.2f");
            ImGui::Combo("Debug view", &g_config.lightingDebug, kDebugViews, IM_ARRAYSIZE(kDebugViews));
        }

        ImGui::Separator();
        if (ImGui::Button("Save")) { SaveConfig(); SetStatus("Saved to d3d8to11.ini"); }
        ImGui::SameLine();
        if (ImGui::Button("Reload ini")) { LoadConfig(); SetStatus("Reloaded d3d8to11.ini"); }
        ImGui::SameLine();
        if (ImGui::Button("Close")) InterlockedExchange(&g_ov.open, 0);
        if (g_ov.status[0] && GetTickCount() < g_ov.statusUntil) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "%s", g_ov.status); }
        ImGui::TextDisabled("Game input is paused while this panel is open.");
        ImGui::End();
    }

    ImGui::Render();
    ID3D11RenderTargetView* rtv = m_swapRTV.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Device::ShutdownOverlay()
{
    if (!g_ov.initialized) return;
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    g_ov.initialized = false;
    InterlockedExchange(&g_ov.open, 0);
}
void Device::ReplayOverlay(){
    if(g_ov.initialized && !g_ov.failed)ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
