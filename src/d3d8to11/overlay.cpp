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
                              "Dump one frame" };

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
        ImGui::Text("%.0f FPS  (%.2f ms)   %ux%u", g_ov.fps, g_ov.frameMs, m_swapW, m_swapH);
        ImGui::TextDisabled("Changes apply immediately. Save writes d3d8to11.ini.");
        ImGui::Separator();

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
            ImGui::Checkbox("Apply game brightness/gamma", &g_config.applyGamma);
            ImGui::Checkbox("FPS counter when closed", &g_config.showFps);
        }

        if (ImGui::CollapsingHeader("Experimental")) {
            ImGui::Checkbox("Replace the game's lighting", &g_config.replaceLighting);
            ImGui::SetItemTooltip("Per-pixel relighting instead of the game's baked light. Research path.");
            ImGui::BeginDisabled(!g_config.replaceLighting);
            ImGui::SliderFloat("Ambient", &g_config.sunAmbient, 0.1f, 2.0f, "%.2f");
            ImGui::SliderFloat("Sun intensity", &g_config.sunDirect, 0.0f, 2.0f, "%.2f");
            ImGui::EndDisabled();
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
