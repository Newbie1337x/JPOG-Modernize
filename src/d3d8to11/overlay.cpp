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
#include "../plugin/camera_settings.h"

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

void DrawCameraSettings()
{
    ImGui::SeparatorText("Camara y limites en vivo");
    HMODULE plugin = GetModuleHandleA("JPOGMouseFix.asi");
    auto get = plugin ? (JPOGCameraSettingsGet)GetProcAddress(plugin, "_JPOGCamera_GetSettings@4") : nullptr;
    auto set = plugin ? (JPOGCameraSettingsSet)GetProcAddress(plugin, "_JPOGCamera_SetSettings@8") : nullptr;
    JPOGCameraSettings settings = {}; settings.size = sizeof(settings); settings.version = 1;
    if (!get || !set || !get(&settings)) { ImGui::TextWrapped("Plugin de camara no disponible."); return; }
    ImGui::Text("Controlador: %s | seleccion: %s", settings.controllerReady ? "activo" : "no instalado",
        settings.followReady ? "libre" : "original");
    bool changed = false;
    changed |= ImGui::SliderFloat("Distancia minima", &settings.minDistance, 0.5f, 100, "%.1f");
    changed |= ImGui::SliderFloat("Distancia maxima", &settings.maxDistance, 10, 2000, "%.0f");
    changed |= ImGui::SliderFloat("Seleccion: minima", &settings.focusMinDistance, 0.5f, 100, "%.1f");
    changed |= ImGui::SliderFloat("Seleccion: maxima", &settings.focusMaxDistance, 10, 2000, "%.0f");
    changed |= ImGui::SliderFloat("Inclinacion hacia abajo", &settings.minPitch, -89, -5, "%.0f grados");
    changed |= ImGui::SliderFloat("Limite de horizonte", &settings.maxPitch, -10, 0, "%.0f grados");
    changed |= ImGui::SliderFloat("Campo de vision (FOV)", &settings.fixedFov, 25, 90, "%.0f grados");
    changed |= ImGui::SliderFloat("Respuesta de camara", &settings.responseSeconds, 0.03f, 0.5f, "%.2f s");
    changed |= ImGui::SliderFloat("Arrastre", &settings.panSensitivity, 0.0001f, 0.02f, "%.4f");
    changed |= ImGui::SliderFloat("Rotacion", &settings.rotateSensitivity, 0.0001f, 0.02f, "%.4f");
    if (changed) set(&settings, FALSE);
    if (ImGui::Button("Guardar camara como predeterminada")) {
        get(&settings);
        SetStatus(set(&settings, TRUE) ? "Camara guardada en JPOGMouseFix.ini" : "No se pudo guardar la camara");
    }
    ImGui::TextWrapped("Cambios inmediatos. Guardar conserva estos valores para el proximo inicio. Ctrl+clic permite escribir un valor. Maxima hasta 2000; muy lejos puede aparecer el limite de dibujado.");

    auto grassGet = (JPOGGrassGet)GetProcAddress(plugin, "_JPOGGrass_Get@8");
    auto grassSet = (JPOGGrassSet)GetProcAddress(plugin, "_JPOGGrass_Set@8");
    float grass = 1, pool = 1;
    if (!grassGet || !grassSet || !grassGet(&grass, &pool)) return;
    ImGui::SeparatorText("Pasto");
    if (ImGui::SliderFloat("Distancia del pasto", &grass, 1, 6, "x%.2f")) grassSet(grass, FALSE);
    if (ImGui::Button("Guardar distancia del pasto")) SetStatus(grassSet(grass, TRUE) ? "Pasto guardado en JPOGMouseFix.ini" : "No se pudo guardar el pasto");
    if (grass > pool + 0.01f)
        ImGui::TextWrapped("Las mallas de pasto alcanzan hasta x%.2f: mas lejos faltan secciones con la camara inclinada hasta guardar y reiniciar el juego.", pool);
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

const char* kDebugViews[] = { "Apagado", "Color base del reemplazo", "Normales del reemplazo", "Mascara de sombras",
                              "Normales de profundidad", "Mascara y mapa de sombras", "Cuadricula del mundo", "Visibilidad original",
                              "Capturar un cuadro", "Solo oclusion ambiental", "Solo rayos de luz", "Normales del mundo",
                              "Autosombra del relieve",
                              "Tipo de dibujo: rojo animados, verde terreno, azul rigidos, amarillo pasto, gris otros" };

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
        style.WindowRounding = 8; style.FrameRounding = 4; style.GrabRounding = 4;
        style.WindowPadding = ImVec2(16, 14);
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(10, 8);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(.07f, .09f, .08f, .98f);
        style.Colors[ImGuiCol_Header] = ImVec4(.14f, .23f, .19f, 1);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.20f, .35f, .27f, 1);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(.24f, .42f, .32f, 1);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(.55f, .83f, .63f, 1);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(.38f, .65f, .48f, 1);
        style.Colors[ImGuiCol_Button] = style.Colors[ImGuiCol_Header];
        style.Colors[ImGuiCol_ButtonHovered] = style.Colors[ImGuiCol_HeaderHovered];
        style.Colors[ImGuiCol_ButtonActive] = style.Colors[ImGuiCol_HeaderActive];
        style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_HeaderHovered];
        style.Colors[ImGuiCol_TabSelected] = style.Colors[ImGuiCol_HeaderActive];
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
        const float availableW = (float)m_swapW > 48 ? m_swapW - 48.0f : 1;
        const float availableH = (float)m_swapH > 48 ? m_swapH - 48.0f : 1;
        ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(fminf(640 * g_config.overlayScale, availableW),
            fminf(700 * g_config.overlayScale, availableH)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(fminf(480, availableW), fminf(360, availableH)),
            ImVec2(availableW, availableH));
        ImGui::Begin("JPOG Modernize | Ajustes", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if(ImGui::Button(g_config.effectsEnabled ? "Desactivar todos los efectos" : "Activar todos los efectos",ImVec2(-1,0))) {
            g_config.effectsEnabled=!g_config.effectsEnabled;
            if(!g_config.effectsEnabled)m_waterInteraction.Clear();
        }
        ImGui::SetItemTooltip("Modo base: sin efectos, MSAA, filtrado forzado, VSync ni limite de FPS. Conserva tus ajustes para restaurarlos al activar.");
        if(!g_config.effectsEnabled)ImGui::TextDisabled("Modo base: efectos/MSAA apagados, FPS sin limite. Ajustes conservados.");
        ImGui::Text("%.0f FPS  (%.2f ms)   %ux%u", g_ov.fps, g_ov.frameMs, m_swapW, m_swapH);
        ImGui::TextDisabled("Ajustes en vivo | Insert abre y cierra este panel");
        ImGui::Separator();
        static int tab = 0;
        const char* tabs[] = { "Graficos", "Agua y pasto", "Camara", "Rendimiento", "Experimental", "Cielo y clima" };
        if (ImGui::BeginTabBar("Categorias", ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int i = 0; i < IM_ARRAYSIZE(tabs); ++i) {
                if (ImGui::BeginTabItem(tabs[i])) { tab = i; ImGui::EndTabItem(); }
            }
            ImGui::EndTabBar();
        }
        char contentId[32]; sprintf_s(contentId, "Contenido##%d", tab);
        ImGui::BeginChild(contentId, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 54), ImGuiChildFlags_Borders);
        ImGui::PushID(tab);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * .45f);
        if (tab == 2) DrawCameraSettings();
        if (tab == 4) ImGui::TextWrapped("Funciones en pruebas. Pueden producir artefactos o aumentar la latencia. Los diagnosticos solo cambian la vista, no se eliminan funciones.");
        if(tab == 3 && ImGui::CollapsingHeader("Rendimiento y recursos", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Medir CPU/GPU (1 cuadro de cada 30)",&g_config.performanceGPU);
            ImGui::Checkbox("Constantes en anillo (prueba A/B)",&g_config.constantRing);
            ImGui::Checkbox("Terreno agrupado (prueba A/B)",&g_config.terrainBatching);
            ImGui::Checkbox("Arboles instanciados (prueba A/B)",&g_config.skinInstancing);
            ImGui::Checkbox("Sombras lejanas en cache (prueba A/B)",&g_config.shadowCache);
            ImGui::Checkbox("Guardar CSV de rendimiento (1/seg)",&g_config.performanceLog);
            if(m_bench.step>=0)ImGui::Text("Midiendo costos de GPU... paso %d (no muevas la camara)",m_bench.step+1);
            else if(ImGui::Button("Medir costo de cada efecto (camara quieta, ~1 min)"))StartGpuBench();
            if(m_bench.step<0 && m_bench.results.size()>1 && ImGui::BeginTable("bench",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)) {
                const double total=m_bench.results.front().stages[Performance::Frame];
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted("GPU con todo");
                ImGui::TableNextColumn();ImGui::Text("%.2f ms",total);
                for(size_t i=1;i<m_bench.results.size();i++){
                    const auto& r=m_bench.results[i];
                    ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    if(i+1==m_bench.results.size())ImGui::Text("%.2f ms",r.stages[Performance::Frame]);
                    else ImGui::Text("%+.2f ms",total-r.stages[Performance::Frame]);
                }
                ImGui::EndTable();
            }
            if(m_perf.csvFailed)ImGui::TextColored(ImVec4(1,.4f,.3f,1),"No se pudo escribir el CSV.");
            else if(!m_perf.csvPath.empty()){ImGui::TextWrapped("CSV: %s",m_perf.csvPath.c_str());}
            ImGui::Text("Cuadro: %.2f ms  Pico: %.2f ms",m_perf.frameMs,m_perf.peakMs);
            ImGui::PlotLines("ms/cuadro",m_perf.history,180,m_perf.historyAt%180,nullptr,0,50,ImVec2(0,65));
            ImGui::Text("CPU espera Present: %.2f ms | Limite FPS: %.2f ms",m_perf.presentMs,m_perf.limiterMs);
            if(m_perf.failed)ImGui::TextDisabled("Mediciones GPU no disponibles.");
            else if(m_perf.enabled && m_perf.valid) {
                ImGui::Text("GPU render completo: %.3f ms",m_perf.gpu[Performance::Frame]);
                ImGui::Text("GPU AO + rayos: %.3f ms",m_perf.gpu[Performance::Effects]);
                ImGui::Text("GPU composicion sombras/color: %.3f ms",m_perf.gpu[Performance::Resolve]);
                ImGui::Text("GPU copias para agua: %.3f ms",m_perf.gpu[Performance::WaterCopy]);
                ImGui::Text("GPU temporal (TAA/FSR31/FG): %.3f ms | FSR1: %.3f ms",m_perf.gpu[Performance::Temporal],m_perf.gpu[Performance::Upscale]);
            } else ImGui::TextDisabled(m_perf.enabled ? "Esperando muestra GPU valida..." : "Medicion GPU apagada.");
            ImGui::TextDisabled("Tiempos GPU incluidos en el total; no sumarlos.");
            ImGui::SeparatorText("CPU: preparacion y envio (cuadro muestreado)");
            ImGui::Text("Geometria: %.3f ms | Teselacion: %.3f ms | Sombras: %.3f ms",
                m_perf.cpuShown[0],m_perf.cpuShown[1],m_perf.cpuShown[2]);
            ImGui::TextDisabled("Incluye llamadas al driver y posibles esperas; no mide toda la simulacion.");
            if(ImGui::TreeNode("Desglose de geometria CPU")) {
                const char* labels[]={"Preparacion inicial", "Clasificacion y efectos", "Diagnosticos",
                    "Enlace de vertices", "Constantes", "Texturas y materiales", "Shaders y estados", "Subidas de geometria"};
                for(int i=Performance::CpuSetup;i<Performance::CpuCount;++i)
                    ImGui::Text("%s: %.3f ms",labels[i-Performance::CpuSetup],m_perf.cpuShown[i]);
                ImGui::TextDisabled("Los primeros siete bloques desglosan preparacion; no sumarlos al total.");
                ImGui::Text("Escrituras identicas evitadas: %llu",m_perf.duplicateConstants);
                ImGui::Text("Subidas VS/PS: %llu / %llu",m_perf.vsUploads,m_perf.psUploads);
                ImGui::Text("Estados repetidos evitados: %llu / %llu",m_perf.redundantRenderStates,m_perf.redundantTextureStates);
                ImGui::Text("Samplers reutilizados/buscados: %llu / %llu",m_perf.samplerSelectionHits,m_perf.samplerSelectionLookups);
                ImGui::Text("Enlaces repetidos evitados: %llu",m_perf.redundantBinds);
                ImGui::Text("Constantes VS subidas: %.1f MiB",m_perf.constantBytes/1048576.0);
                ImGui::Text("Subidas geometria: %llu | %.2f MiB",m_perf.geometryUploads,m_perf.geometryUploadBytes/1048576.0);
                ImGui::TextDisabled("Contadores acumulados. Medicion detallada solo en cuadros muestreados.");
                ImGui::TreePop();
            }
            ImGui::Text("Cache teselacion: %llu aciertos / %llu construcciones",m_tessCacheHits,m_tessCacheMisses);
            ImGui::Text("Entradas: %u / 128 | Memoria: %.2f / 32 MiB",unsigned(m_tessCache.size()),m_tessCacheBytes/1048576.0);
            ImGui::TextDisabled("Agua/normales/pasto/sombras de geometria incluidos en total.");
            ImGui::Text("Muestras GPU descartadas/ocupadas: %u",m_perf.skipped);
            const double mib=1048576.0;
            if(m_perf.memoryValid) {
                ImGui::Text("RAM privada: %.1f MiB | Pico: %.1f",m_perf.privateBytes/mib,m_perf.peakPrivate/mib);
                ImGui::Text("RAM residente: %.1f MiB | Delta base: %+.1f",m_perf.workingSet/mib,
                    (double(m_perf.privateBytes)-double(m_perf.basePrivate))/mib);
            }
            if(m_perf.budgetValid)ImGui::Text("GPU local proceso: %.1f / %.1f MiB presupuesto",m_perf.gpuUsage/mib,m_perf.gpuBudget/mib);
            else ImGui::TextDisabled("Presupuesto GPU no disponible en este controlador.");
            ImGui::Text("Dibujos: %u | Agua: %u | Sombras extra: %u",m_perfDraws,m_perfWaterDraws,m_perfShadowDraws);
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
            ImGui::TextDisabled("RAM de todo el juego. Un aumento no demuestra una fuga.");
            ImGui::TextDisabled("Comparar la misma escena; base tras terminar de cargar.");
        }

        if (tab == 0 && ImGui::CollapsingHeader("Sombras solares", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Activar sombras", &g_config.sunLighting);
            ImGui::BeginDisabled(!g_config.sunLighting);
            ImGui::SliderFloat("Intensidad", &g_config.shadowStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Distancia", &g_config.shadowDistance, 50.0f, 1000.0f, "%.0f unidades");
            ImGui::SetItemTooltip("Distancia de las sombras. Un alcance menor permite mas detalle.");
            static const int sizes[] = { 1024, 2048, 4096, 8192 };
            int current = 2;
            for (int i = 0; i < 4; i++) if (sizes[i] == g_config.shadowResolution) current = i;
            const char* labels[] = { "1024", "2048", "4096", "8192" };
            if (ImGui::Combo("Resolucion del mapa", &current, labels, 4)) g_config.shadowResolution = sizes[current];
            ImGui::Checkbox("Sombras del terreno", &g_config.terrainShadows);
            ImGui::SameLine();
            ImGui::Checkbox("Sombras del pasto", &g_config.grassShadows);
            ImGui::Text("Mapa %ux%u | %.3f unidades/texel | %u objetos", m_sunMapSize, m_sunMapSize, m_sunTexelWorld, m_lastCasters);
            ImGui::EndDisabled();
        }

        if (tab == 0 && ImGui::CollapsingHeader("Direccion del sol")) {
            ImGui::SliderFloat("Paso de las sombras", &g_config.shadowStep, 0, 2, "%.2f grados");
            ImGui::SetItemTooltip("Con el reloj corriendo, las sombras siguen al sol a saltitos de estos grados en vez de girar cada cuadro (eso las hace temblar). 0 = cada cuadro.");
            int source = g_config.sunFromGame ? 0 : 1;
            bool changed = ImGui::RadioButton("Seguir al juego", &source, 0);
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
            if (ImGui::SliderFloat("Orientacion", &g_ov.azimuth, 0, 360, "%.0f grados")) SunFromAngles();
            if (ImGui::SliderFloat("Elevacion", &g_ov.elevation, 5, 90, "%.0f grados")) SunFromAngles();
            ImGui::EndDisabled();
            float minDeg = std::asin(g_config.sunMinElevation) * 180 / 3.14159265f;
            if (ImGui::SliderFloat("Elevacion minima", &minDeg, 3, 85, "%.0f grados"))
                g_config.sunMinElevation = std::sin(minDeg * 3.14159265f / 180);
            ImGui::SetItemTooltip("Un sol mas bajo genera sombras mas largas. Este valor limita su elevacion minima.");
            ImGui::Text("Direccion solar: %.2f %.2f %.2f", m_sun.direction[0], -m_sun.direction[1], m_sun.direction[2]);
        }
        if (tab == 5 && ImGui::CollapsingHeader("Hora y luna", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Dia y noche (en pruebas)", &g_config.timeOfDay);
            ImGui::SetItemTooltip("Mueve el sol y la luna. Funciona mejor con la iluminacion por pixel de la pestana Experimental. Las nubes y la niebla lo necesitan.");
            ImGui::BeginDisabled(!g_config.timeOfDay);
            int hh = (int)g_config.timeOfDayHour, mm = (int)((g_config.timeOfDayHour - hh) * 60);
            char label[32]; sprintf_s(label, "%02d:%02d", hh, mm);
            ImGui::SliderFloat("Hora", &g_config.timeOfDayHour, 0, 23.99f, label);
            ImGui::Checkbox("El tiempo corre", &g_config.timeRuns);
            ImGui::SetItemTooltip("La hora avanza sola. Cada medianoche la luna avanza un dia en su ciclo.");
            ImGui::SliderFloat("Duracion del dia", &g_config.dayMinutes, 2, 120, "%.0f minutos");
            ImGui::SetItemTooltip("Minutos reales que dura un dia completo del juego.");
            ImGui::SliderFloat("Ciclo lunar", &g_config.moonCycleDays, 2, 30, "%.0f dias");
            ImGui::SetItemTooltip("Dias de juego de una luna llena a la siguiente.");
            if (g_config.timeOfDayDay >= g_config.moonCycleDays) g_config.timeOfDayDay = std::fmod(g_config.timeOfDayDay, g_config.moonCycleDays);
            ImGui::SliderFloat("Dia del ciclo lunar", &g_config.timeOfDayDay, 0, g_config.moonCycleDays - 0.01f, "%.1f");
            {
                const float a = m_tod.moonAge;
                const char* phase = a < 0.03f || a > 0.97f ? "nueva" : a < 0.22f ? "creciente" : a < 0.28f ? "cuarto creciente" :
                                    a < 0.47f ? "gibosa creciente" : a < 0.53f ? "llena" : a < 0.72f ? "gibosa menguante" :
                                    a < 0.78f ? "cuarto menguante" : "menguante";
                ImGui::Text("Luna %s, %.0f%% iluminada, %s", phase, m_tod.moonLit * 100, m_tod.moonAbove > 0.5f ? "en el cielo" : "bajo el horizonte");
            }
            ImGui::EndDisabled();
        }
        if (tab == 5 && ImGui::CollapsingHeader("Islas lejanas", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Islas en el horizonte", &g_config.islands);
            ImGui::BeginDisabled(!g_config.islands);
            ImGui::SliderFloat("Altura de las islas", &g_config.islandHeight, 0.2f, 4, "x%.2f");
            ImGui::SliderFloat("Bruma sobre las islas", &g_config.islandHaze, 0, 3, "%.2f");
            ImGui::SliderFloat("Nubes en las islas", &g_config.islandClouds, 0, 2, "%.2f");
            ImGui::EndDisabled();
        }
        if (tab == 5 && ImGui::CollapsingHeader("Nubes", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(!g_config.timeOfDay);
            ImGui::Checkbox("Nubes volumetricas", &g_config.clouds);
            ImGui::SetItemTooltip("Capa de nubes calculada solo donde se ve el cielo, a media resolucion.");
            ImGui::BeginDisabled(!g_config.clouds);
            ImGui::SliderFloat("Cobertura de nubes", &g_config.cloudCoverage, 0, 1, "%.2f");
            ImGui::SliderFloat("Densidad de nubes", &g_config.cloudDensity, 0.05f, 4, "%.2f");
            ImGui::SliderFloat("Altura de nubes", &g_config.cloudHeight, 50, 5000, "%.0f");
            ImGui::SliderFloat("Grosor de nubes", &g_config.cloudThickness, 50, 5000, "%.0f");
            ImGui::SliderFloat("Viento de nubes", &g_config.cloudWind, 0, 200, "%.0f");
            ImGui::SliderFloat("Bruma del horizonte", &g_config.horizonHaze, 0.05f, 1, "%.2f");
            ImGui::SetItemTooltip("Desde que parte de la distancia visible lo lejano se funde con el cielo (1 = apagado).");
            ImGui::Checkbox("Haces de luz entre las nubes", &g_config.shafts);
            ImGui::SetItemTooltip("Haces volumetricos: el aire iluminado entre las sombras de las nubes, visibles desde cualquier angulo. Intensidad en Graficos, rayos de luz.");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        if (tab == 5 && ImGui::CollapsingHeader("Niebla atmosferica", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(!g_config.timeOfDay);
            ImGui::Checkbox("Niebla encendida", &g_config.fog);
            ImGui::SetItemTooltip("Lo lejano toma el color del cielo, mas en lo bajo (valles, mar) y mas calido hacia el sol.");
            ImGui::BeginDisabled(!g_config.fog);
            ImGui::SliderFloat("Densidad de niebla", &g_config.fogDensity, 0, 0.003f, "%.5f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Niebla baja", &g_config.fogHeightDensity, 0, 0.01f, "%.5f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Altura de niebla baja", &g_config.fogHeightFalloff, 1, 300, "%.0f");
            ImGui::SliderFloat("Piso de niebla", &g_config.fogBase, -100, 100, "%.0f");
            ImGui::SliderFloat("Brillo hacia el sol", &g_config.fogSun, 0, 4, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Niebla volumetrica", &g_config.volumeFog);
            ImGui::SetItemTooltip("Niebla que se junta en los huecos y valles entre los cerros, iluminada por el sol o la luna a traves de las sombras.");
            ImGui::BeginDisabled(!g_config.volumeFog);
            ImGui::SliderFloat("Densidad volumetrica", &g_config.volumeDensity, 0, 0.5f, "%.4f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Suavidad del borde", &g_config.volumeHeight, 1, 60, "%.1f");
            ImGui::SliderFloat("Desde que altura", &g_config.volumeHills, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Que tan altos tienen que ser los cerros alrededor de un hueco: 0 = el piso del parque, 1 = los cerros mas altos.");
            ImGui::SliderFloat("Bancos de niebla", &g_config.volumePatches, 0, 1, "%.2f");
            ImGui::SetItemTooltip("0 = capa pareja, 1 = bancos que se mueven con huecos.");
            ImGui::EndDisabled();
            ImGui::Checkbox("Rayos del sol y la luna", &g_config.godRays);
            ImGui::SetItemTooltip("Rayos desde la posicion real del sol (o la luna) donde el cielo asoma entre nubes, arboles y cerros.");
            ImGui::BeginDisabled(!g_config.godRays);
            ImGui::SliderFloat("Intensidad de los rayos", &g_config.godRayStrength, 0, 8, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        if (tab == 5 && ImGui::CollapsingHeader("Dia y noche: ajustes avanzados")) {
            ImGui::TextDisabled("Noche");
            ImGui::ColorEdit3("Color de la luna", g_config.moonColor, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Luz lunar", &g_config.moonIntensity, 0, 3, "%.2f");
            ImGui::SliderFloat("Contorno lunar", &g_config.moonRim, 0, 8, "%.2f");
            ImGui::SliderFloat("Brillo del contorno lunar", &g_config.moonRimGlow, 0, 2, "%.2f");
            ImGui::SetItemTooltip("Resalta los contornos bajo la luna, incluso sobre piel oscura.");
            ImGui::SliderFloat("Ancho del contorno lunar", &g_config.moonRimWidth, 0.05f, 1, "%.2f");
            ImGui::SliderFloat("Reflejo lunar", &g_config.moonSheen, 0, 4, "%.2f");
            ImGui::SliderFloat("Luz lunar en pasto", &g_config.grassMoon, 0, 4, "%.2f");
            ImGui::SliderFloat("Luz lunar en superficies", &g_config.nightSurface, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Iluminacion lunar general de pasto y agua. Los bordes se iluminan por separado.");
            ImGui::ColorEdit3("Ambiente nocturno", g_config.nightAmbient, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Cielo nocturno", &g_config.nightSky, 0, 4, "%.2f");
            ImGui::SliderFloat("Exposicion nocturna", &g_config.nightExposure, 0.1f, 2, "%.2f");
            ImGui::SliderFloat("Desaturacion nocturna", &g_config.nightDesaturate, 0, 1, "%.2f");
            ImGui::ColorEdit3("Tinte nocturno", g_config.nightTint, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Estrellas", &g_config.stars, 0, 4, "%.2f");
            ImGui::SliderFloat("Luz sin luna", &g_config.moonlessLight, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Luz de la noche cuando la luna esta baja o es fina, en proporcion a la luna llena.");
            ImGui::SliderFloat("Disco solar", &g_config.sunDisc, 0, 4, "%.2f");
            ImGui::SliderFloat("Brillo dorado", &g_config.sunGlow, 0, 4, "%.2f");
            ImGui::SetItemTooltip("De dia el sol marca los bordes de hojas, piel y pasto como la luna de noche, con su color. Mas fuerte al amanecer y al atardecer.");
            ImGui::TextDisabled("Color de dia (la noche no cambia)");
            ImGui::SliderFloat("Brillo de dia", &g_config.dayBrightness, 0.5f, 2, "%.2f");
            ImGui::SliderFloat("Levantar oscuros", &g_config.dayLift, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Aclara las zonas oscuras (pasto, sombras) y baja el contraste. 0 = como antes.");
            ImGui::SliderFloat("Saturacion de dia", &g_config.daySaturation, 0, 2, "%.2f");
            ImGui::SliderFloat("Sombras de dia", &g_config.dayShadowStrength, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Intensidad de las sombras de dia. La de 'Sombras solares' queda para el atardecer y la noche.");
            ImGui::SliderFloat("Luz solar de dia", &g_config.daySunBoost, 0, 1, "%.2f");
            ImGui::SliderFloat("Oclusion de dia", &g_config.dayAO, 0, 1, "%.2f");
            ImGui::TextDisabled("Amanecer y atardecer");
            ImGui::ColorEdit3("Color del sol bajo", g_config.lowSunColor, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Ambiente del atardecer", g_config.duskAmbient, ImGuiColorEditFlags_Float);
            ImGui::TextDisabled("Adaptacion visual");
            ImGui::SliderFloat("Exposicion maxima", &g_config.maxExposure, 1, 6, "%.2f");
            ImGui::SetItemTooltip("Limite del aumento de exposicion al amanecer, al atardecer y de noche.");
            ImGui::TextDisabled("Guardar conserva estos valores en [TimeOfDay] de d3d8to11.ini.");
        }

        if (tab == 0 && ImGui::CollapsingHeader("Luz y color")) {
            ImGui::Checkbox("Activar correccion##post", &g_config.postEnabled);
            ImGui::BeginDisabled(!g_config.postEnabled);
            ImGui::SliderFloat("Luz solar", &g_config.sunBoost, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Luz calida adicional en las zonas iluminadas por el sol.");
            ImGui::SliderFloat("Sombras frias", &g_config.shadowCool, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Exposicion", &g_config.exposure, 0.5f, 2.0f, "%.2f");
            ImGui::SliderFloat("Contraste", &g_config.contrast, 0.5f, 1.5f, "%.2f");
            ImGui::SliderFloat("Saturacion", &g_config.saturation, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Calidez", &g_config.warmth, -1.0f, 1.0f, "%.2f");
            if (ImGui::Button("Restablecer valores##post")) {
                g_config.sunBoost = 0.25f; g_config.shadowCool = 0.5f; g_config.exposure = 1.0f;
                g_config.contrast = 0.97f; g_config.saturation = 1.09f; g_config.warmth = 0.23f;
            }
            ImGui::EndDisabled();
        }

        if (tab == 0 && ImGui::CollapsingHeader("Oclusion ambiental y rayos de luz")) {
            ImGui::Checkbox("Oclusion ambiental (HBAO)", &g_config.aoEnabled);
            ImGui::BeginDisabled(!g_config.aoEnabled);
            ImGui::SliderFloat("Radio de oclusion", &g_config.aoRadius, 0.3f, 8.0f, "%.1f unidades");
            ImGui::SliderFloat("Intensidad de oclusion", &g_config.aoStrength, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            ImGui::Checkbox("Rayos de luz solar", &g_config.shafts);
            ImGui::SetItemTooltip("Requiere sombras solares. Se nota mas al mirar hacia el sol.");
            ImGui::BeginDisabled(!g_config.shafts || !g_config.sunLighting);
            ImGui::SliderFloat("Intensidad de rayos", &g_config.shaftStrength, 0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Dispersion", &g_config.shaftScatter, 0.0f, 0.95f, "%.2f");
            ImGui::SetItemTooltip("0: neblina uniforme. Valores altos concentran el brillo alrededor del sol.");
            ImGui::EndDisabled();
        }

        if (tab == 0 && ImGui::CollapsingHeader("Mapas de normales")) {
            ImGui::Checkbox("Activar normales##normals", &g_config.normalMaps);
            ImGui::SetItemTooltip("Relieve desde archivos <textura>_n.dds en mods\\textures.");
            ImGui::BeginDisabled(!g_config.normalMaps);
            ImGui::SliderFloat("Intensidad del relieve", &g_config.normalStrength, 0.0f, 2.0f, "%.2f");
            ImGui::Checkbox("Invertir canal verde (OpenGL)", &g_config.normalFlipGreen);
            ImGui::EndDisabled();
        }

        if (tab == 1 && ImGui::CollapsingHeader("Agua", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Agua con profundidad",&g_config.waterEnabled);
            ImGui::BeginDisabled(!g_config.waterEnabled);
            const char* surfaces[]={"Automatico: oceano / lagunas","Todo laguna (manual)","Todo oceano (manual)"};
            ImGui::Combo("Tipo de agua",&g_config.waterSurfaceMode,surfaces,3);
            ImGui::SetItemTooltip("Auto usa el terreno nativo completo: agua conectada al borde = oceano; encerrada por tierra = laguna. No depende de la camara.");
            ImGui::SliderFloat("Espuma de lagunas",&g_config.waterLagoonFoam,0,1,"x%.2f");
            ImGui::SliderFloat("Ondas de lagunas",&g_config.waterLagoonRipples,0,2,"x%.2f");
            ImGui::SliderFloat("Ondas del oceano",&g_config.waterOceanRipples,0,3,"x%.2f");
            ImGui::Checkbox("Oceano FFT",&g_config.waterOcean);
            ImGui::SetItemTooltip("Olas de un espectro de oleaje real (como los juegos actuales) en vez del ruido.");
            ImGui::SliderFloat("Tamano de las olas",&g_config.waterOceanScale,8,400,"%.0f unidades");
            ImGui::SliderFloat("Fuerza de las olas",&g_config.waterOceanStrength,0,3,"x%.2f");
            ImGui::Checkbox("Reflejar el cielo",&g_config.waterSkyReflection);
            ImGui::SetItemTooltip("El agua refleja el cielo de la hora del dia (azul, atardecer, noche) en vez de la textura original.");
            ImGui::SliderFloat("Brillo del sol en el agua",&g_config.waterSunGlitter,0,10,"%.2f");
            ImGui::ColorEdit3("Color del mar",g_config.waterOceanColor,ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Tinte del cielo reflejado",g_config.waterSkyTint,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
            ImGui::SliderFloat("Altura del oleaje",&g_config.waterSwellAmplitude,0,2,"%.2f unidades");
            ImGui::SetItemTooltip("Desplaza vertices del oceano; 0 desactiva. Auto: frentes hacia la costa segun distancia a tierra. Lagunas sin desplazamiento. Limitado por la malla original.");
            ImGui::SliderFloat("Longitud de olas",&g_config.waterSwellLength,.5f,4,"x%.2f");
            ImGui::SliderFloat("Suavizado en costa",&g_config.waterSwellCoastDepth,.5f,10,"%.1f unidades");
            ImGui::SliderFloat("Espuma en crestas",&g_config.waterCrestFoam,0,1,"%.2f");
            if(g_config.waterSurfaceMode==0 && ImGui::TreeNode("Estado del mapa de agua")){
                if(m_waterRegionReady)ImGui::Text("Mapa nativo %ux%u: mar %u, laguna %u muestras",m_waterRegions.width,m_waterRegions.height,m_waterRegions.oceanCells,m_waterRegions.pondCells);
                else if(m_waterNativeFault)ImGui::TextColored(ImVec4(1,.4f,.3f,1),"Lectura nativa detenida: perfil oceano o ajuste manual.");
                else if(!m_waterNativeAvailable)ImGui::TextDisabled("Sin mapa nativo aun: perfil oceano hasta obtenerlo.");
                else if(!m_waterRegionW)ImGui::TextColored(ImVec4(1,.7f,.2f,1),"Esperando altura de agua: azul = respaldo, no clasificacion.");
                else ImGui::Text("Leyendo terreno: %u / %u muestras",m_waterRegionNext,m_waterRegionW*m_waterRegionH);
                ImGui::Text("Ultimo paso CPU del mapa: %.3f ms",m_waterRegionCpuMs);
                ImGui::Text("Nivel nativo muestreado: %.3f",m_waterRegionLevel);
                if(ImGui::Button("Actualizar mapa de agua"))m_waterRegionRefresh=0;
                ImGui::TreePop();
            }
            ImGui::SliderFloat("Profundidad de absorcion",&g_config.waterDepthScale,0.5f,20,"%.1f unidades");
            ImGui::SliderFloat("Transparencia superficial",&g_config.waterTransparency,0,1,"%.2f");
            ImGui::SliderFloat("Reflejos Fresnel",&g_config.waterReflection,0,1,"%.2f");
            ImGui::Checkbox("Reflejos de escena (SSR)",&g_config.waterSSR);
            ImGui::SliderFloat("Intensidad de reflejos SSR",&g_config.waterSSRStrength,0,1,"%.2f");
            ImGui::SliderFloat("Alcance de reflejos",&g_config.waterSSRDistance,5,250,"%.0f unidades");
            ImGui::SliderInt("Pasos de reflejos",&g_config.waterSSRSteps,8,48);
            ImGui::SetItemTooltip("Mas pasos: mas detalle y mayor coste GPU. Solo refleja objetos visibles.");
            ImGui::SliderFloat("Espuma en la orilla",&g_config.waterFoam,0,1,"%.2f");
            ImGui::SliderFloat("Ancho de espuma",&g_config.waterFoamWidth,0.05f,3,"%.2f unidades");
            ImGui::SliderFloat("Distorsion de refraccion",&g_config.waterDistortion,0,10,"%.1f px");
            ImGui::SliderFloat("Intensidad de ondas",&g_config.waterRipples,0,3,"%.2f");
            ImGui::SliderFloat("Velocidad de ondas",&g_config.waterSpeed,0,3,"%.2f");
            ImGui::Checkbox("Ondas y estelas de animales",&g_config.waterInteraction);
            ImGui::SliderFloat("Intensidad de interaccion",&g_config.waterInteractionStrength,0,3,"%.2f");
            ImGui::Text("%u ondas de interaccion activas",m_waterInteraction.ActiveRipples(WaterTime()));
            const char* debug[]={"Vista normal","Profundidad del agua","Fresnel","Espuma","Interaccion de animales","Diagnostico de reflejos","Oceano azul / laguna verde","Altura de olas"};
            ImGui::Combo("Diagnostico del agua",&g_config.waterDebug,debug,8);
            ImGui::EndDisabled();
        }
        if (tab == 4 && ImGui::CollapsingHeader("Teselacion de modelos")) {
            ImGui::Checkbox("Suavizar siluetas (triangulos PN)", &g_config.tessellation);
            ImGui::Checkbox("Solo dinosaurios", &g_config.tessDinosOnly);
            ImGui::SliderFloat("Subdivision maxima", &g_config.tessMaxFactor, 1, 16, "%.0f");
            ImGui::SliderFloat("Pixeles por borde", &g_config.tessPixels, 2, 64, "%.0f");
            ImGui::SetItemTooltip("Valores menores generan mas triangulos y aumentan el coste GPU.");
            ImGui::SliderFloat("Curvatura", &g_config.tessCurvature, 0, 1.5f, "%.2f");
            ImGui::Checkbox("Unir vertices separados", &g_config.tessWeld);
            ImGui::SetItemTooltip("Los vertices de las costuras comparten curvatura para evitar grietas.");
            ImGui::Checkbox("Invertir orientacion de triangulos", &g_config.tessFlipWinding);
            ImGui::SetItemTooltip("Activar si las caras del modelo aparecen invertidas.");
            ImGui::SliderFloat("Altura del relieve", &g_config.tessDisplacement, 0, 0.2f, "%.3f");
            ImGui::SetItemTooltip("Desplazamiento real desde la textura. 0 lo desactiva.");
            ImGui::SliderFloat("Suavidad del relieve", &g_config.tessDetailMip, 0, 4, "%.1f");
            ImGui::Checkbox("Invertir relieve", &g_config.tessInvertRelief);
            ImGui::Checkbox("Proteger costuras", &g_config.tessSeamSafe);
            ImGui::SetItemTooltip("Reduce el relieve junto a los bordes para evitar grietas en costuras.");
            ImGui::Text("%u dibujos con teselacion este cuadro", m_tessDrawsShown);
            if (m_tessFailed) ImGui::TextColored(ImVec4(1, .6f, .2f, 1), "Teselacion desactivada tras un fallo. Revisar el registro.");
        }
        if (tab == 4 && ImGui::CollapsingHeader("Relieve de piel (parallax)")) {
            ImGui::Checkbox("Parallax y autosombras en pieles", &g_config.reliefEnabled);
            ImGui::BeginDisabled(!g_config.reliefEnabled);
            ImGui::SliderFloat("Profundidad (texeles)", &g_config.reliefDepth, 0.5f, 12, "%.1f");
            ImGui::SetItemTooltip("Altura del relieve en pixeles de textura.");
            ImGui::SliderFloat("Parallax", &g_config.reliefParallax, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Desplazamiento de textura segun el angulo. 0: solo sombras.");
            ImGui::SliderFloat("Autosombras", &g_config.reliefShadow, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Las escamas y pliegues generan sombras sobre detalles cercanos.");
            ImGui::SliderFloat("Suavidad de sombras", &g_config.reliefSoftness, 0.25f, 4, "%.2f");
            ImGui::Checkbox("Invertir altura", &g_config.reliefInvert);
            ImGui::EndDisabled();
            ImGui::Text("%u dibujos con relieve este cuadro", m_reliefDrawsShown);
        }
        if (tab == 1 && ImGui::CollapsingHeader("Pasto interactivo")) {
            ImGui::Checkbox("Animales doblan el pasto", &g_config.grassPhysics);
            ImGui::BeginDisabled(!g_config.grassPhysics);
            ImGui::SliderFloat("Curvatura del pasto", &g_config.grassPush, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Aplanado", &g_config.grassFlatten, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Radio", &g_config.grassRadius, 0.2f, 4.0f, "x%.2f");
            ImGui::SliderInt("Minimo de huesos", &g_config.grassMinBones, 1, 40);
            ImGui::SetItemTooltip("Modelos con menos huesos, como objetos y arboles, no mueven el pasto.");
            ImGui::Text("%u animales moviendo pasto este cuadro", m_pusherCount);
            ImGui::EndDisabled();
            ImGui::Checkbox("Pasto lejano mas liviano", &g_config.grassThin);
            ImGui::SetItemTooltip("A distancia se dibuja una parte de las matas y las que quedan se ven mas llenas.");
            ImGui::BeginDisabled(!g_config.grassThin);
            ImGui::SliderFloat("Empieza a", &g_config.grassThinStart, 0, 0.95f, "%.2f de la distancia");
            ImGui::SliderFloat("Matas al final", &g_config.grassThinKeep, 0.05f, 1, "%.2f");
            ImGui::SliderFloat("Relleno", &g_config.grassThinFill, 0, 1, "%.2f");
            ImGui::Text("Distancia del pasto: %.0f", m_grassFar);
            ImGui::EndDisabled();
        }

        if (tab == 0 && ImGui::CollapsingHeader("Pantalla y calidad")) {
            ImGui::SliderInt("Limite de FPS", &g_config.fpsLimit, 0, 360, g_config.fpsLimit ? "%d" : "Apagado");
            bool vsync = g_config.syncInterval != 0;
            if (ImGui::Checkbox("VSync", &vsync)) g_config.syncInterval = vsync ? 1 : 0;
            static const int anisos[] = { 1, 2, 4, 8, 16 };
            int a = 0;
            for (int i = 0; i < 5; i++) if (anisos[i] <= g_config.forceAnisotropy) a = i;
            const char* alabels[] = { "Configuracion del juego", "2x", "4x", "8x", "16x" };
            if (ImGui::Combo("Filtrado anisotropico", &a, alabels, 5)) g_config.forceAnisotropy = anisos[a];
            static const int msaaValues[] = { 1, 2, 4, 8 };
            int m = 0;
            for (int i = 0; i < 4; i++) if (msaaValues[i] <= g_config.msaa) m = i;
            const char* mlabels[] = { "Apagado", "2x", "4x", "8x" };
            if (ImGui::Combo("Suavizado de bordes (MSAA)", &m, mlabels, 4)) g_config.msaa = msaaValues[m];
            ImGui::Checkbox("Suavizar bordes de vegetacion", &g_config.alphaToCoverage);
            ImGui::Checkbox("Aplicar brillo y gamma del juego", &g_config.applyGamma);
            ImGui::Checkbox("Contador FPS con el panel cerrado", &g_config.showFps);
            ImGui::Checkbox("AMD FSR 1: EASU + RCAS", &g_config.fsr1);
            ImGui::SliderFloat("Nitidez RCAS", &g_config.fsrSharpness, 0, 1, "%.2f");
            ImGui::SetItemTooltip("A resolucion nativa aplica nitidez. El reescalado requiere una entrada menor que la salida.");
        }

        if(tab == 4 && ImGui::CollapsingHeader("Materiales piloto: Triceratops / hamburgueseria")){
            ImGui::Checkbox("Materiales por pixel (comparar activado/apagado)",&g_config.materialsEnabled);
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
        if(tab == 4 && ImGui::CollapsingHeader("Antialiasing temporal y FSR")) {
            if(ImGui::Checkbox("FSR 3.1 temporal (en pruebas, AA nativo)",&g_config.fsr31)){m_taaValid=false;m_viewportDirty=true;}
            ImGui::SetItemTooltip("SDK DX11 x86 real. Resolucion nativa, sin generacion de frames. Movimiento de camara; dinosaurios/agua con mascara reactiva conservadora. No reduce aun el coste de renderizado.");
            if(m_fsr31Failed)ImGui::TextColored(ImVec4(1,.6f,.2f,1),"FSR31 fallo: TAA/FSR1 de respaldo.");
            else if(m_fsr31)ImGui::Text("FSR31: %s",m_fsr31Frame+1>=m_frame?"activo":"preparado");
            if(ImGui::Button("Reintentar / reiniciar FSR31")){ReleaseFsr31();m_fsr31Failed=false;}
            ImGui::TextDisabled("Primera etapa: AA nativo; faltan vectores de animacion completos.");
            if(ImGui::Checkbox("Generacion de cuadros x2 (prototipo)",&g_config.frameGeneration)){
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

        if (tab == 4 && ImGui::CollapsingHeader("Iluminacion por pixel y materiales")) {
            ImGui::Checkbox("Reemplazar iluminacion del juego", &g_config.replaceLighting);
            ImGui::SetItemTooltip("Iluminacion por pixel de modelos y terreno usando los colores de luz del juego.");
            ImGui::BeginDisabled(!g_config.replaceLighting);
            ImGui::SliderFloat("Intensidad solar", &g_config.relightSun, 0, 3, "x%.2f");
            ImGui::SliderFloat("Ambiente##relight", &g_config.relightAmbient, 0, 3, "x%.2f");
            ImGui::SliderFloat("Tinte del cielo", &g_config.relightSkyTint, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Luz azul desde el cielo y rebote calido desde el suelo.");
            ImGui::SliderFloat("Exposicion##relight", &g_config.relightExposure, 0.25f, 4, "%.2f");
            ImGui::Checkbox("Suavizar luces intensas", &g_config.relightTonemap);
            ImGui::Checkbox("Cielo procedural", &g_config.proceduralSky);
            ImGui::SetItemTooltip("Reemplaza el cielo del juego de dia y de noche.");
            ImGui::Checkbox("Iluminar terreno desde su altura", &g_config.heightfieldLighting);
            ImGui::SetItemTooltip("Normales suaves calculadas desde el terreno, en lugar de su iluminacion pintada.");
            ImGui::SliderFloat("Sombras de colinas", &g_config.horizonShadows, 0, 1, "%.2f");
            ImGui::SliderFloat("Luz de contorno", &g_config.rimLight, 0, 2, "%.2f");
            ImGui::Checkbox("Reiluminar pasto", &g_config.relightGrass);
            ImGui::Checkbox("Luces nocturnas (mascaras emisivas)", &g_config.emissive);
            ImGui::SliderFloat("Intensidad de luces nocturnas", &g_config.emissiveIntensity, 0, 8, "%.2f");
            ImGui::Checkbox("Luces de faroles", &g_config.pointLights);
            ImGui::SliderFloat("Intensidad de faroles", &g_config.pointLightIntensity, 0, 16, "%.2f");
            ImGui::SliderFloat("Radio de faroles", &g_config.pointLightRadius, 0.5f, 64, "%.1f");
            ImGui::SliderFloat("Juntar faroles cercanos", &g_config.pointLightMerge, 0, 16, g_config.pointLightMerge > 0 ? "%.1f unidades" : "Apagado");
            ImGui::SetItemTooltip("Faroles dentro de la misma celda de este tamano iluminan como uno solo (mas rapido).");
            ImGui::Text("%u faroles encendidos", m_pointLightsShown);
            ImGui::SliderFloat("Calidez de faroles", &g_config.pointLightWarmth, 0, 1, "%.2f");
            ImGui::ColorEdit3("Color de faroles", g_config.pointLightTint);
            ImGui::SliderInt("Depurar faroles", &g_config.pointLightDebug, 0, 2);
            ImGui::SetItemTooltip("1: color = faroles por zona de pantalla (rojo = lleno), columnas verdes = donde quedo cada farol. 2: lo mismo sin zonas, recorriendo todos.");
            ImGui::Checkbox("Bloom", &g_config.bloom);
            ImGui::SliderFloat("Intensidad de bloom", &g_config.bloomStrength, 0, 4, "%.2f");
            ImGui::SliderFloat("Umbral de bloom", &g_config.bloomThreshold, 0, 1, "%.2f");
            ImGui::SliderFloat("Bloom diurno", &g_config.bloomDay, 0, 1, "%.2f");
            ImGui::SliderFloat("Reflejo especular diurno", &g_config.specular, 0, 4, "%.2f");
            ImGui::SliderFloat("Rugosidad especular", &g_config.specRoughness, 0.1f, 1, "%.2f");
            ImGui::Checkbox("Tonos cinematograficos (ACES)", &g_config.filmic);
            ImGui::SliderFloat("Exposicion cinematografica", &g_config.filmicExposure, 0.2f, 4, "%.2f");
            ImGui::SliderFloat("Saturacion del pasto", &g_config.grassSaturation, 0, 2, "%.2f");
            ImGui::SliderFloat("Brillo del pasto", &g_config.grassBrightness, 0, 2, "%.2f");
            ImGui::SliderFloat("Terreno como el pasto", &g_config.terrainMatch, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Lleva el verde del terreno pelado (donde ya no hay pasto) al verde amarillento del pasto.");
            ImGui::ColorEdit3("Tinte del terreno", g_config.terrainTint, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::ColorEdit3("Tinte del pasto", g_config.grassTint, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::SetItemTooltip("Color de las matas de pasto (con 'Terreno como el pasto'). Menos rojo = verde mas fresco, menos paja.");
            ImGui::SetItemTooltip("Resalta las siluetas a contraluz del sol. La luna tiene su propio control.");
            ImGui::SliderFloat("Detalle de luz original", &g_config.bakedTerrainDetail, 0, 1, "%.2f");
            ImGui::SetItemTooltip("Conserva parte de las sombras pintadas en la textura del terreno.");
            ImGui::Text(m_terrainReady ? "Terreno: %ux%u" : "Terreno: cargando (%ux%u)", m_terrainW, m_terrainH);
            ImGui::EndDisabled();
            ImGui::SliderFloat("Ambiente de materiales piloto", &g_config.sunAmbient, 0.1f, 2.0f, "%.2f");
            ImGui::SetItemTooltip("Luz de materiales piloto cuando el reemplazo de iluminacion esta apagado.");
            ImGui::SliderFloat("Sol de materiales piloto", &g_config.sunDirect, 0.0f, 2.0f, "%.2f");
            ImGui::Combo("Vista de diagnostico", &g_config.lightingDebug, kDebugViews, IM_ARRAYSIZE(kDebugViews));
        }

        ImGui::PopItemWidth();
        ImGui::PopID();
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Guardar graficos")) { SaveConfig(); SetStatus("Graficos guardados en d3d8to11.ini"); }
        ImGui::SameLine();
        if (ImGui::Button("Recargar graficos")) { LoadConfig(); SetStatus("Graficos recargados desde d3d8to11.ini"); }
        ImGui::SameLine();
        if (ImGui::Button("Cerrar")) InterlockedExchange(&g_ov.open, 0);
        if (g_ov.status[0] && GetTickCount() < g_ov.statusUntil) ImGui::TextWrapped("%s", g_ov.status);
        ImGui::TextDisabled("Los controles del juego se bloquean mientras este panel esta abierto.");
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
