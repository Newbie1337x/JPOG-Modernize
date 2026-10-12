// JPOGMouseFix.asi - Jurassic Park: Operation Genesis (PC)
//
// 1) Cursor: quita la aceleracion (Toshi::TGUIScreen::ApplyPointerAccel en TGuiInterface.dll
//    duplica/cuadruplica el delta cuando supera 6 conteos) y la reemplaza por una escala lineal
//    configurable con acumulacion sub-pixel. Ademas corrige que el cursor vaya mas rapido en X
//    que en Y cuando la pantalla no es 4:3 (la GUI usa un tamano virtual distinto al real).
// 2) Slider "Sensibilidad del mouse" (controla MouseSensitivityFP, camara en primera persona):
//    baja el minimo de 0.25 a un valor configurable.
// 3) Zoom con la ruedita: AScrollingState::ProcessInput aplica un impulso de zoom = dt * 2 por
//    cada clic, asi que a mas FPS menos zoom. Para eventos de la rueda se usa un dt fijo.
// 4) Widescreen Hor+: el motor fija el FOV horizontal (pierde imagen arriba/abajo en 16:9).
//    Se calcula la focal a partir del alto * 4/3 para mantener el FOV vertical de 4:3.
//
// Config: JPOGMouseFix.ini junto al .asi.

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <initializer_list>
#include <string>
#include <vector>
#include <unordered_map>

static float g_cursorSpeed = 0.5f;
static bool  g_disableAccel = true;
static bool  g_cursorAspectFix = true;
static float g_sliderMin = 0.05f;
static float g_wheelDt = 1.0f / 30.0f;
static bool  g_wheelFix = true;
static bool  g_horPlus = true;
static bool  g_freeKeyboard = true;
static float g_remX = 0.0f, g_remY = 0.0f;

static const float g_four3 = 4.0f / 3.0f;
static const float g_half = 0.5f;

static bool PatchBytes(void* addr, const void* data, size_t len)
{
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

static bool BytesMatch(const void* addr, const unsigned char* expected, size_t len)
{
    __try {
        return memcmp(addr, expected, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void WriteRel(void* from, void* to, unsigned char opcode, size_t totalLen)
{
    unsigned char buf[16];
    memset(buf, 0x90, sizeof(buf));
    buf[0] = opcode; // 0xE9 jmp / 0xE8 call
    *(DWORD*)(buf + 1) = (DWORD)((BYTE*)to - (BYTE*)from - 5);
    PatchBytes(from, buf, totalLen);
}

// ---------------------------------------------------------------------------------------------
// Cursor

static int Scale(int delta, float& remainder, float speed)
{
    float v = delta * speed + remainder;
    int whole = (int)v; // trunca hacia cero
    remainder = v - whole;
    return whole;
}

typedef void(__stdcall* GetScreenSize_t)(float* outVec2);
static GetScreenSize_t GetGuiScreenSize = (GetScreenSize_t)0x50bce0;    // ACamera::GetGuiScreenSize
static GetScreenSize_t GetRenderScreenSize = (GetScreenSize_t)0x50bd30; // ACamera::GetRenderScreenSize

// Factor para X que hace que un conteo del mouse mueva lo mismo en pixeles en X que en Y.
static float CursorAspectFactor()
{
    float gui[2] = {}, ren[2] = {};
    __try {
        GetGuiScreenSize(gui);
        GetRenderScreenSize(ren);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
    if (gui[0] <= 0 || gui[1] <= 0 || ren[0] <= 0 || ren[1] <= 0)
        return 1.0f;
    float f = (gui[0] / gui[1]) / (ren[0] / ren[1]);
    return (f > 0.25f && f < 4.0f) ? f : 1.0f;
}

// ---------------------------------------------------------------------------------------------
// HUD 4:3 centrado (PGUIRenderer.dll, PGUITRDisplayContext::BeginScene)
//
// La GUI se dibuja en coordenadas virtuales 4:3 con una ortho que las estira a todo el ancho.
// Despues de BeginScene, la base de la pila de matrices (this+0x230 + idx*0x40) es identidad y
// cada Push* compone sobre ella. Se cambia por escala X = f y traslacion X = (1-f)/2 * anchoGUI,
// y se reenvia al render context (this+0x21c, vtable+0x7c) como hace PushScale.
static bool g_fixHud = true;
static void* g_beginSceneTramp = nullptr;

// Escala X del HUD (1 = sin cambio). Solo actua en pantallas mas anchas que la GUI (4:3).
static float HudFactor(float* gui, float* ren)
{
    gui[0] = gui[1] = ren[0] = ren[1] = 0.0f;
    if (!g_fixHud) return 1.0f;
    __try {
        GetGuiScreenSize(gui);
        GetRenderScreenSize(ren);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
    if (gui[0] <= 0 || gui[1] <= 0 || ren[0] <= 0 || ren[1] <= 0) return 1.0f;
    const float f = (gui[0] / gui[1]) / (ren[0] / ren[1]);
    return (f < 0.999f && f > 0.25f) ? f : 1.0f;
}

// Coordenada X de la GUI (HUD centrado) -> X de la GUI "estirada" a toda la pantalla (0..W).
static float GuiToFullX(float x, float f, float w) { return f * x + (1.0f - f) * 0.5f * w; }

// Reemplazo de TGUIScreen::SetPointerPosition(int x, int y) (TGuiInterface, thiscall, ret 8).
// El original limita X a [0, W-1]; con el HUD centrado se amplia para cubrir toda la pantalla.
static bool __fastcall SetPointerPosition_Hook(BYTE* screen, void* /*edx*/, int x, int y)
{
    const int w = *(short*)(screen + 0x70);
    const int h = *(short*)(screen + 0x72);
    int lo = 0, hi = w - 1;
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f < 1.0f) {
        const int ext = (int)((1.0f - f) * 0.5f * w / f);
        lo -= ext;
        hi += ext;
    }
    if (x > hi) x = hi;
    if (x < lo) x = lo;
    if (y > h - 1) y = h - 1;
    if (y < 0) y = 0;
    int* px = (int*)(screen + 0x114);
    int* py = (int*)(screen + 0x118);
    if (*px == x && *py == y) return false;
    *px = x;
    *py = y;
    return true;
}

// Reemplazo de ACamera::ConvertGuiScreenToRenderScreen(const TVector2& gui, TVector2& out)
// (SimJP 0x50bd60, stdcall, ret 8): mismo mapeo que el HUD centrado, para que el clic en el
// mundo 3D caiga donde se ve el cursor.
static void __stdcall ConvertGuiToRender_Hook(const float* in, float* out)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (gui[0] <= 0 || gui[1] <= 0) { out[0] = in[0]; out[1] = in[1]; return; }
    float x = GuiToFullX(in[0], f, gui[0]) * ren[0] / gui[0];
    float y = in[1] * ren[1] / gui[1];
    out[0] = x < 0 ? 0 : (x > ren[0] ? ren[0] : x);
    out[1] = y < 0 ? 0 : (y > ren[1] ? ren[1] : y);
}

// Inversa: ACamera::ConvertRenderScreenToGuiScreen(const TVector2& ren, TVector2& gui) (0x50bdd0).
static void __stdcall ConvertRenderToGui_Hook(const float* in, float* out)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (ren[0] <= 0 || ren[1] <= 0) { out[0] = in[0]; out[1] = in[1]; return; }
    const float full = in[0] * gui[0] / ren[0];
    out[0] = f < 1.0f ? (full - (1.0f - f) * 0.5f * gui[0]) / f : full;
    out[1] = in[1] * gui[1] / ren[1];
}

// Mundo -> GUI: ARenderer::ConvertToScreenCoords(TVector3&) (0x477ec0) devuelve coordenadas GUI
// estiradas (y escala Y con el ancho). Se pasan al espacio del HUD centrado; asi los iconos sobre
// dinos/edificios y la deteccion de que hay bajo el cursor coinciden con lo que se ve.
static void* g_toScreenTramp = nullptr;
static void __fastcall ToScreenCoords_Hook(void* rnd, void*, float* v)
{
    ((void(__thiscall*)(void*, float*))g_toScreenTramp)(rnd, v);
    if (v[0] == -100.0f && v[1] == -100.0f) return; // detras de la camara
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f >= 1.0f || gui[0] <= 0) return;
    v[0] = (v[0] - (1.0f - f) * 0.5f * gui[0]) / f;
    v[1] = v[1] / f;
}

// Desplazamiento por bordes (AScrollingState::OnUpdate 0x55cbe5: mov ebx,[edi+0x114]).
// Se reemplaza por "call EdgeXStub": EBX = X del cursor llevada a 0..W de pantalla completa.
static int __cdecl EdgeX(int x)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    return f < 1.0f ? (int)GuiToFullX((float)x, f, gui[0]) : x;
}

__declspec(naked) static void EdgeXStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [edi + 0x114]
        call EdgeX
        add esp, 4
        mov ebx, eax
        pop edx
        pop ecx
        pop eax
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// HUD anclado a los bordes (TGUIScreen::Paint, TGuiInterface)
//
// Con el HUD centrado en 4:3, los paneles pegados a un borde de la pantalla 4:3 (plata, contadores,
// menu de construccion...) se corren a los bordes reales de la pantalla 16:9 moviendo su posicion
// (TGUIWidget +0x6c x, +0x70 ancho). Como el cursor y los clics usan las mismas coordenadas, la
// deteccion de clics sigue coincidiendo. Si hay un panel de pantalla completa visible (menus,
// Dinopedia...) no se ancla nada y se restauran las posiciones.
// Hijos: primer nodo en +0x50, centinela en +0x44, siguiente en +0xc. Visible: flags +0x88 & 4.
static int g_hudMode = 2; // 0 = estirado, 1 = 4:3 centrado, 2 = 16:9 anclado
static void* g_screenPaintTramp = nullptr;
struct AnchorState { short origX, setX; };
static std::unordered_map<BYTE*, AnchorState>* g_anchors = nullptr;
static const int kEdgeSnap = 48; // un panel a <= 48 unidades de un borde 4:3 se considera anclado

// Menus emergentes (PGUIMenuDialog / PGUIDiscMenuDialog::UpdatePosition, PGUICore): el juego los
// ubica junto al boton que los abrio y los limita a [0, W]. Con el HUD anclado ese boton puede
// estar fuera de ese rango, asi que se simula una pantalla mas ancha durante la llamada.
// Estos paneles se excluyen del anclado (se recuerdan por unos frames).
static void* g_menuUpdPosTramp = nullptr;
static void* g_discUpdPosTramp = nullptr;
static BYTE* g_lastScreen = nullptr;
static unsigned g_paintFrame = 0;
static std::unordered_map<BYTE*, unsigned>* g_selfPositioned = nullptr;

static int HudExtent(int W)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    return (f < 1.0f && g_hudMode == 2) ? (int)((1.0f - f) * 0.5f * W / f) : 0;
}

static void UpdatePositionWide(void* tramp, BYTE* dlg, int* px, int* py)
{
    typedef void(__thiscall* Fn)(BYTE*, int*, int*);
    BYTE* screen = g_lastScreen;
    const int ext = screen ? HudExtent(*(short*)(screen + 0x70)) : 0;
    if (!g_selfPositioned) g_selfPositioned = new std::unordered_map<BYTE*, unsigned>();
    (*g_selfPositioned)[dlg] = g_paintFrame;
    if (!ext) { ((Fn)tramp)(dlg, px, py); return; }
    short& sw = *(short*)(screen + 0x70);
    const short oldW = sw;
    *px += ext;
    sw = (short)(oldW + 2 * ext);
    ((Fn)tramp)(dlg, px, py);
    sw = oldW;
    *(short*)(dlg + 0x6c) -= (short)ext;
    *px -= ext;
}

static void __fastcall MenuUpdatePosition_Hook(BYTE* dlg, void*, int* px, int* py)
{
    UpdatePositionWide(g_menuUpdPosTramp, dlg, px, py);
}

static void __fastcall DiscUpdatePosition_Hook(BYTE* dlg, void*, int* px, int* py)
{
    UpdatePositionWide(g_discUpdPosTramp, dlg, px, py);
}

static bool IsSelfPositioned(BYTE* w)
{
    if (!g_selfPositioned) return false;
    auto it = g_selfPositioned->find(w);
    if (it == g_selfPositioned->end()) return false;
    // Mientras siga visible lo maneja el juego; al ocultarse se olvida (la memoria puede reusarse).
    if (!(*(DWORD*)(w + 0x88) & 4) && g_paintFrame - it->second > 2) { g_selfPositioned->erase(it); return false; }
    return true;
}

static void AnchorHud(BYTE* screen)
{
    g_lastScreen = screen;
    g_paintFrame++;
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    const int W = *(short*)(screen + 0x70), H = *(short*)(screen + 0x72);
    if (!g_anchors) g_anchors = new std::unordered_map<BYTE*, AnchorState>();
    const int ext = HudExtent(W);

    BYTE* sentinel = screen + 0x44;
    bool fullscreen = false;
    int guard = 0;
    for (BYTE* w = *(BYTE**)(screen + 0x50); w && w != sentinel && guard < 512; w = *(BYTE**)(w + 0xc), guard++) {
        if (!(*(DWORD*)(w + 0x88) & 4)) continue;
        if (*(short*)(w + 0x70) >= W - 40 && *(short*)(w + 0x72) >= H - 80) { fullscreen = true; break; }
    }
    guard = 0;
    for (BYTE* w = *(BYTE**)(screen + 0x50); w && w != sentinel && guard < 512; w = *(BYTE**)(w + 0xc), guard++) {
        if (IsSelfPositioned(w)) { g_anchors->erase(w); continue; }
        short& x = *(short*)(w + 0x6c);
        const short width = *(short*)(w + 0x70);
        auto it = g_anchors->find(w);
        if (it == g_anchors->end() || x != it->second.setX) {
            AnchorState s = { x, x };
            it = g_anchors->insert_or_assign(w, s).first;
        }
        AnchorState& s = it->second;
        short target = s.origX;
        // Solo paneles chicos: los anchos (ej. barra de mensajes) acompanan a lo que esta centrado.
        if (ext && !fullscreen && width <= W / 2 && s.origX >= 0 && s.origX + width <= W) {
            const bool nearLeft = s.origX <= kEdgeSnap;
            const bool nearRight = s.origX + width >= W - kEdgeSnap;
            if (nearLeft && !nearRight) target = (short)(s.origX - ext);
            else if (nearRight && !nearLeft) target = (short)(s.origX + ext);
        }
        x = target;
        s.setX = target;
    }
}

static void __fastcall ScreenPaint_Hook(BYTE* screen, void* /*edx*/, void* widget)
{
    __try {
        AnchorHud(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((void(__thiscall*)(BYTE*, void*))g_screenPaintTramp)(screen, widget);
}

// Vistas 3D dentro de la GUI (vista previa de edificios, Dinopedia...):
// AGUIRenderObject::SetViewRectangle (SimJP 0x678a90) pasa el rectangulo GUI a pixeles con la
// escala estirada. Se le pasa el rectangulo ya convertido al espacio del HUD centrado/anclado.
static void* g_setViewRectTramp = nullptr;

static void __fastcall SetViewRectangle_Hook(BYTE* obj, void*, const int* rect)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    int r[4] = { rect[0], rect[1], rect[2], rect[3] };
    if (f < 1.0f && gui[0] > 0) {
        r[0] = (int)floorf(GuiToFullX((float)rect[0], f, gui[0]) + 0.5f);
        r[2] = (int)floorf(rect[2] * f + 0.5f);
    }
    ((void(__thiscall*)(BYTE*, const int*))g_setViewRectTramp)(obj, r);
}

static void __fastcall BeginScene_Hook(BYTE* ctx, void* /*edx*/)
{
    ((void(__thiscall*)(BYTE*))g_beginSceneTramp)(ctx);
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f >= 1.0f) return;
    const int idx = *(int*)(ctx + 0x22c);
    float* m = (float*)(ctx + 0x230 + idx * 0x40);
    m[0] *= f; m[1] *= f; m[2] *= f;          // fila 0: eje X
    m[12] += (1.0f - f) * 0.5f * gui[0];      // fila 3: traslacion X
    void* rc = *(void**)(ctx + 0x21c);
    void** vt = *(void***)rc;
    ((void(__thiscall*)(void*, float*))vt[0x7c / 4])(rc, m);
}

// Reemplazo de: void __thiscall TGUIScreen::ApplyPointerAccel(int& dx, int& dy)  (ret 8)
// __fastcall: ECX = this, EDX = basura, 2 args en el stack que limpia el callee -> ret 8.
static void __fastcall ApplyPointerAccel_Hook(void* screen, void* /*edx*/, int* dx, int* dy)
{
    if (!g_disableAccel) {
        // Comportamiento original: umbral en +0x138, modo en +0x140 (0 = off, 1 = x2, 2 = x4)
        const int threshold = *(int*)((BYTE*)screen + 0x138);
        const int mode = *(int*)((BYTE*)screen + 0x140);
        for (int* d : { dx, dy }) {
            if (abs(*d) > threshold && mode != 0) *d *= 2;
            if (abs(*d) > threshold && mode == 2) *d *= 2;
        }
    }
    // Con el HUD centrado en 4:3 las unidades de la GUI ya son cuadradas en pantalla.
    const float fx = (g_cursorAspectFix && !g_fixHud) ? CursorAspectFactor() : 1.0f;
    *dx = Scale(*dx, g_remX, g_cursorSpeed * fx);
    *dy = Scale(*dy, g_remY, g_cursorSpeed);
}

// ---------------------------------------------------------------------------------------------
// Zoom con la rueda (SimJP.exe, AScrollingState::ProcessInput)
//
// Sitio original (dos veces, zoom in y zoom out):
//   fld dword ptr [esp]     ; dt del frame
//   mov edx, [ebx]          ; vtable de la camara
// Se reemplaza por "call ZoomDtStub". EBP = InputEvent*, cuyo primer campo es el doodad.

static const int DOODAD_WHEEL_FORWARD = 0x30009;
static const int DOODAD_WHEEL_BACKWARD = 0x3000a;

static float __cdecl AdjustZoomDt(const int* inputEvent, float dt)
{
    __try {
        const int doodad = inputEvent[0];
        if (doodad == DOODAD_WHEEL_FORWARD || doodad == DOODAD_WHEEL_BACKWARD)
            return g_wheelDt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return dt;
}

__declspec(naked) static void ZoomDtStub()
{
    __asm {
        mov edx, dword ptr [esp + 4] // el [esp] del llamador (dt), corrido por la direccion de retorno
        push edx
        push ebp
        call AdjustZoomDt            // devuelve el float en ST0, igual que el fld original
        add esp, 8
        mov edx, dword ptr [ebx]
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Suavizado de camara independiente de FPS (SimJP.exe, AWorldCamera::OnUpdate)
//
// Por frame: v = v * scm_fDampening + impulso * scm_fOMDampening. El impulso ya es dt * k, asi que
// la velocidad final no depende de los FPS, pero la aceleracion/frenado dura N frames: a mas FPS,
// arranca y frena mas de golpe (pan con bordes/teclas, rotacion y zoom).
// Se reemplazan las dos cargas por stubs que devuelven, con dt en EDI:
//   D'  = D ^ (dt * RefFPS)
//   OM' = OM * (1 - D') / (1 - D)     (mantiene la velocidad final)

static float* const scm_fDampening = (float*)0xd12c00;
static float* const scm_fOMDampening = (float*)0x8b61f0;
static float g_refFps = 30.0f;
static bool  g_smoothFix = true;

// Velocidad del desplazamiento por bordes (AScrollingState::OnUpdate). Constantes usadas solo ahi:
// 0xd1a954 = adelante/atras (original 2.0), 0xd1a958 = costados (original 1.0).
static float* const kEdgeVertical = (float*)0xd1a954;
static float* const kEdgeHorizontal = (float*)0xd1a958;
static float g_edgeVertical = 1.5f;
static float g_edgeHorizontal = 1.5f;

static float ClampDt(float dt)
{
    if (!(dt > 0.0f)) return 0.0f;
    return dt > 0.25f ? 0.25f : dt;
}

static float __cdecl DampeningFor(float dt)
{
    const float d = *scm_fDampening;
    if (d <= 0.0f || d >= 1.0f) return d;
    return powf(d, ClampDt(dt) * g_refFps);
}

static float __cdecl OMDampeningFor(float dt)
{
    const float d = *scm_fDampening;
    const float om = *scm_fOMDampening;
    if (d <= 0.0f || d >= 1.0f) return om;
    const float dEff = powf(d, ClampDt(dt) * g_refFps);
    return om * (1.0f - dEff) / (1.0f - d);
}

// Sitios: 0x527d3f fld [scm_fDampening] / 0x527d51 fld [scm_fOMDampening] (6 bytes c/u).
// EDI = dt (float en registro entero, cargado al inicio de OnUpdate).
__declspec(naked) static void DampeningStub()
{
    __asm {
        push edi
        call DampeningFor
        add esp, 4
        ret
    }
}

__declspec(naked) static void OMDampeningStub()
{
    __asm {
        push edi
        call OMDampeningFor
        add esp, 4
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Hor+(TRenderInterface.dll, TCameraViewportHandler::OnBeginRenderEvent, rama FOV horizontal)
//
// Original:  fld dword ptr [0.5] ; fmul dword ptr [esi+0x20] (ancho) ; fdivrp  -> focal = 0.5*W/tan
// Con el stub: carga 0.5*k, donde k = min(1, (H*4/3)/W), y el fmul original lo multiplica por W,
// quedando focal = 0.5*(H*4/3)/tan: mismo FOV vertical que en 4:3, mas imagen a los costados.

__declspec(naked) static void HorPlusStub()
{
    __asm {
        fld dword ptr [esi + 0x24]   // H
        fmul dword ptr [g_four3]     // H*4/3
        fdiv dword ptr [esi + 0x20]  // k = H*4/3 / W
        fld1
        fcomip st, st(1)             // 1 vs k
        jae keep                     // k <= 1 (pantalla ancha): usar k
        fstp st(0)
        fld1                         // pantalla mas angosta que 4:3: comportamiento original
    keep:
        fmul dword ptr [g_half]
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Ventana sin bordes (requiere ScreenWindowed = true en options.ini)
//
// Un hilo busca la ventana principal del proceso y, si tiene barra de titulo/bordes, se los saca y
// la estira al monitor donde esta. Se re-chequea cada 500 ms por si el juego la vuelve a cambiar.

static bool g_borderless = true;

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rc;
    GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 320 || rc.bottom - rc.top < 240)
        return TRUE;
    *(HWND*)lp = hwnd;
    return FALSE;
}

static void MakeBorderless(HWND hwnd)
{
    const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!(style & (WS_CAPTION | WS_THICKFRAME)) || IsIconic(hwnd))
        return; // ya sin bordes, pantalla completa exclusiva, o minimizada

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi))
        return;

    SetWindowLongA(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
    const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                                             WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

static DWORD WINAPI BorderlessThread(LPVOID)
{
    for (;;) {
        HWND hwnd = nullptr;
        EnumWindows(FindGameWindowProc, (LPARAM)&hwnd);
        if (hwnd)
            MakeBorderless(hwnd);
        Sleep(500);
    }
}

// ---------------------------------------------------------------------------------------------
// Modo ventana forzado via Direct3D 8
//
// El motor ignora ScreenWindowed en la practica, asi que se intercepta Direct3DCreate8 (IAT de
// TRenderD3DInterface.dll) y luego IDirect3D8::CreateDevice / IDirect3DDevice8::Reset para pedir
// siempre modo ventana. La ventana se deja sin bordes y del tamano del monitor.

struct D3DPP8 {
    UINT BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType, SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
struct D3DDISPLAYMODE8 { UINT Width, Height, RefreshRate; DWORD Format; };

typedef void* (WINAPI* Direct3DCreate8_t)(UINT);
typedef HRESULT(WINAPI* CreateDevice_t)(void*, UINT, DWORD, HWND, DWORD, D3DPP8*, void**);
typedef HRESULT(WINAPI* GetAdapterDisplayMode_t)(void*, UINT, D3DDISPLAYMODE8*);
typedef HRESULT(WINAPI* Reset_t)(void*, D3DPP8*);

static bool g_forceWindowed = true;
static bool g_altF4 = true;
static Direct3DCreate8_t g_origCreate8 = nullptr;
static CreateDevice_t g_origCreateDevice = nullptr;
static Reset_t g_origReset = nullptr;
static UINT g_bbWidth = 0, g_bbHeight = 0;
static DWORD g_desktopFormat = 0; // D3DFMT_UNKNOWN
static HWND g_gameWnd = nullptr;
static WNDPROC g_origWndProc = nullptr;

static void FitWindowToMonitor(HWND hwnd)
{
    if (!hwnd) return;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi)) return;
    const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    SetWindowLongA(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE);
    const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                                             WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_altF4 && msg == WM_SYSKEYDOWN && wp == VK_F4)
        TerminateProcess(GetCurrentProcess(), 0);
    return IsWindowUnicode(hwnd) ? CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp)
                                 : CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

static void SubclassGameWindow(HWND hwnd)
{
    if (!hwnd || g_gameWnd == hwnd) return;
    g_gameWnd = hwnd;
    if (IsWindowUnicode(hwnd))
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
    else
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
}

static void MakeWindowed(D3DPP8* pp, HWND focus)
{
    if (!pp) return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = 0; // D3DPRESENT_INTERVAL_DEFAULT (obligatorio en ventana)
    pp->BackBufferFormat = g_desktopFormat;  // en ventana debe coincidir con el escritorio
    if (pp->SwapEffect == 2)                 // D3DSWAPEFFECT_FLIP
        pp->SwapEffect = 1;                  // D3DSWAPEFFECT_DISCARD
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    FitWindowToMonitor(w);
    SubclassGameWindow(w);
}

static HRESULT WINAPI Reset_Hook(void* dev, D3DPP8* pp)
{
    if (g_forceWindowed) MakeWindowed(pp, g_gameWnd);
    HRESULT hr = g_origReset(dev, pp);
    if (hr >= 0 && pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
    return hr;
}

static void PatchVtable(void** vtbl, int index, void* hook, void** orig)
{
    if (vtbl[index] == hook) return;
    *orig = vtbl[index];
    PatchBytes(&vtbl[index], &hook, sizeof(void*));
}

// Present: despues de mostrar el frame se limpia el backbuffer completo a negro. En los menus nada
// se dibuja en las franjas laterales del HUD 4:3 y, con SWAPEFFECT_DISCARD, quedaban restos del
// cursor de frames anteriores ("estela").
struct D3DVIEWPORT8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
typedef HRESULT(WINAPI* Present_t)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT(WINAPI* Clear_t)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
typedef HRESULT(WINAPI* Viewport_t)(void*, D3DVIEWPORT8*);
static Present_t g_origPresent = nullptr;

static HRESULT WINAPI Present_Hook(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* rgn)
{
    HRESULT hr = g_origPresent(dev, src, dst, wnd, rgn);
    if (g_fixHud && g_bbWidth && g_bbHeight) {
        void** vt = *(void***)dev;
        D3DVIEWPORT8 old = {};
        if (((Viewport_t)vt[41])(dev, &old) >= 0) {           // GetViewport
            D3DVIEWPORT8 full = { 0, 0, g_bbWidth, g_bbHeight, 0.0f, 1.0f };
            ((Viewport_t)vt[40])(dev, &full);                  // SetViewport
            ((Clear_t)vt[36])(dev, 0, nullptr, 1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);
            ((Viewport_t)vt[40])(dev, &old);
        }
    }
    return hr;
}

static HRESULT WINAPI CreateDevice_Hook(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                        D3DPP8* pp, void** outDev)
{
    if (g_forceWindowed) {
        D3DDISPLAYMODE8 mode = {};
        void** vtbl = *(void***)d3d;
        if (((GetAdapterDisplayMode_t)vtbl[8])(d3d, adapter, &mode) >= 0)
            g_desktopFormat = mode.Format;
        MakeWindowed(pp, focus);
    } else {
        SubclassGameWindow(pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    }
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, outDev);
    if (hr >= 0 && outDev && *outDev && g_forceWindowed)
        PatchVtable(*(void***)*outDev, 14, (void*)&Reset_Hook, (void**)&g_origReset); // IDirect3DDevice8::Reset
    if (hr >= 0 && outDev && *outDev) {
        PatchVtable(*(void***)*outDev, 15, (void*)&Present_Hook, (void**)&g_origPresent); // Present
        if (pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
    }
    return hr;
}

static void* WINAPI Direct3DCreate8_Hook(UINT sdk)
{
    void* d3d = g_origCreate8(sdk);
    if (d3d)
        PatchVtable(*(void***)d3d, 15, (void*)&CreateDevice_Hook, (void**)&g_origCreateDevice); // IDirect3D8::CreateDevice
    return d3d;
}

// Reemplaza una entrada de la IAT de 'module' (importada de 'dll'!'func').
static bool PatchIAT(HMODULE module, const char* dll, const char* func, void* hook, void** orig)
{
    BYTE* base = (BYTE*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto orig_thunk = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig_thunk->u1.AddressOfData; ++thunk, ++orig_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + orig_thunk->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            *orig = (void*)thunk->u1.Function;
            void* p = hook;
            return PatchBytes(&thunk->u1.Function, &p, sizeof(p));
        }
    }
    return false;
}

static void InstallD3DHooks()
{
    HMODULE rd3d = GetModuleHandleA("TRenderD3DInterface.dll");
    if (rd3d)
        PatchIAT(rd3d, "d3d8.dll", "Direct3DCreate8", (void*)&Direct3DCreate8_Hook, (void**)&g_origCreate8);
}

// ---------------------------------------------------------------------------------------------
// Reemplazo de texturas (mods\textures)
//
// Archivos: mods\textures\<libreria>__<textura>.png|.dds|.jpg|.tga|.bmp
//   ej. mods\textures\gui_InGame__Menu_Buttons1.png  (libreria = Data\matlibs\gui_InGame.tml)
// Al arrancar se lee cada .tml mencionado, se ubica la textura por nombre y se guarda el hash de
// sus bytes. El motor crea las texturas DDS con D3DXCreateTextureFromFileInMemoryEx (enlazada
// estaticamente en TRenderD3DInterface.dll, RVA 0xf51a); si los bytes coinciden con un hash se le
// pasa el archivo de reemplazo. Luego se restaura en D3DXIMAGE_INFO el ancho/alto ORIGINAL, porque
// la GUI calcula sus coordenadas en pixeles de la textura original (con el tamano real se rompe).

struct TexReplacement {
    std::string path;
    UINT origW, origH;
};
static std::unordered_map<unsigned long long, TexReplacement> g_texMods;
static bool g_texModsEnabled = true;
static int g_texModsUsed = 0;

static unsigned long long Fnv64(const BYTE* p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL ^ n;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static bool ReadWholeFile(const std::string& path, std::vector<BYTE>& out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    BOOL ok = size == 0 || ReadFile(f, out.data(), size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

static std::string GameDir()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    return dir;
}

static void LogTexMods(const char* fmt, const char* a, const char* b)
{
    static std::string logPath = GameDir() + "mods\\textures\\_log.txt";
    static bool first = true;
    FILE* f = nullptr;
    if (fopen_s(&f, logPath.c_str(), first ? "w" : "a") != 0 || !f) return;
    first = false;
    fprintf(f, fmt, a, b);
    fputc('\n', f);
    fclose(f);
}

// Busca en un .tml la textura <name>; devuelve puntero/tamano dentro de 'tml'.
static bool FindTmlTexture(const std::vector<BYTE>& tml, const char* name, const BYTE** blob, UINT* size,
                           UINT* hdrW, UINT* hdrH)
{
    if (tml.size() < 16 || memcmp(tml.data(), "TML1", 4) != 0) return false;
    const UINT count = *(const UINT*)(tml.data() + 8);
    size_t o = 0xc;
    struct Ent { UINT idx; size_t off; UINT size; UINT w, h; };
    std::vector<Ent> ents;
    for (UINT i = 0; i < count; i++) {
        if (o + 28 > tml.size()) return false;
        Ent e = { *(const UINT*)(tml.data() + o), o + 28, *(const UINT*)(tml.data() + o + 4),
                   *(const unsigned short*)(tml.data() + o + 18), *(const unsigned short*)(tml.data() + o + 20) };
        if (e.off + e.size > tml.size()) return false;
        ents.push_back(e);
        o = e.off + e.size;
    }
    if (o + 4 > tml.size()) return false;
    const UINT nnames = *(const UINT*)(tml.data() + o);
    o += 4;
    for (UINT n = 0; n < nnames && o + 32 * (n + 1) <= tml.size(); n++) {
        char buf[33] = {};
        memcpy(buf, tml.data() + o + 32 * n, 32);
        if (_stricmp(buf, name) != 0) continue;
        for (const Ent& e : ents)
            if (e.idx == n) { *blob = tml.data() + e.off; *size = e.size; *hdrW = e.w; *hdrH = e.h; return true; }
    }
    return false;
}

// Recorre mods\textures y todas sus subcarpetas (ui\menu, dinos, ...). Las carpetas que empiezan
// con "_" (ej. _backup) se ignoran. Si el mismo archivo esta en dos carpetas gana el primero.
static void ScanTextureModsDir(const std::string& gameDir, const std::string& folder, const std::string& rel,
                               std::unordered_map<std::string, std::vector<BYTE>>& tmls, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((folder + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string file = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (file[0] != '.' && file[0] != '_' && depth < 8)
                ScanTextureModsDir(gameDir, folder + file + "\\", rel + file + "\\", tmls, depth + 1);
            continue;
        }
        size_t sep = file.find("__"), dot = file.rfind('.');
        if (sep == std::string::npos || dot == std::string::npos || dot < sep) continue;
        std::string lib = file.substr(0, sep), tex = file.substr(sep + 2, dot - sep - 2);
        auto it = tmls.find(lib);
        if (it == tmls.end()) {
            std::vector<BYTE> data;
            ReadWholeFile(gameDir + "Data\\matlibs\\" + lib + ".tml", data);
            it = tmls.emplace(lib, std::move(data)).first;
        }
        const std::string shown = rel + file;
        const BYTE* blob; UINT size, hw = 0, hh = 0;
        if (!FindTmlTexture(it->second, tex.c_str(), &blob, &size, &hw, &hh) || size < 16) {
            LogTexMods("NO ENCONTRADA: %s (%s)", shown.c_str(), "revisar nombre de libreria/textura");
            continue;
        }
        const unsigned long long key = Fnv64(blob, size);
        if (g_texMods.count(key)) {
            LogTexMods("REPETIDA (se ignora): %s%s", shown.c_str(), "");
            continue;
        }
        TexReplacement r;
        r.path = folder + file;
        if (memcmp(blob, "DDS ", 4) == 0) {
            r.origH = *(const UINT*)(blob + 12);
            r.origW = *(const UINT*)(blob + 16);
        } else { // textura cruda (1555/4444/8888): tamano del encabezado del .tml
            r.origW = hw;
            r.origH = hh;
        }
        g_texMods[key] = std::move(r);
        LogTexMods("ok: %s%s", shown.c_str(), "");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void ScanTextureMods()
{
    const std::string dir = GameDir();
    std::unordered_map<std::string, std::vector<BYTE>> tmls;
    ScanTextureModsDir(dir, dir + "mods\\textures\\", "", tmls, 0);
}

typedef HRESULT(WINAPI* D3DXCreateTexMemEx_t)(void* dev, const void* data, UINT size, UINT w, UINT h, UINT mips,
                                              DWORD usage, DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter,
                                              DWORD colorKey, UINT* srcInfo, void* palette, void** tex);
static D3DXCreateTexMemEx_t g_origCreateTexMem = nullptr;

static HRESULT WINAPI CreateTexMem_Hook(void* dev, const void* data, UINT size, UINT w, UINT h, UINT mips,
                                        DWORD usage, DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter,
                                        DWORD colorKey, UINT* srcInfo, void* palette, void** tex)
{
    if (!g_texMods.empty() && data && size >= 128) {
        auto it = g_texMods.find(Fnv64((const BYTE*)data, size));
        if (it != g_texMods.end()) {
            TexReplacement& r = it->second;
            std::vector<BYTE> fileData;
            ReadWholeFile(r.path, fileData);
            if (!fileData.empty()) {
                // El motor pasa formato D3DFMT_UNKNOWN y tamano D3DX_DEFAULT: se toma todo del archivo.
                HRESULT hr = g_origCreateTexMem(dev, fileData.data(), (UINT)fileData.size(), w, h, mips, usage,
                                                fmt, pool, filter, mipFilter, colorKey, srcInfo, palette, tex);
                if (hr >= 0) {
                    if (srcInfo) { srcInfo[0] = r.origW; srcInfo[1] = r.origH; } // D3DXIMAGE_INFO.Width/Height
                    g_texModsUsed++;
                    return hr;
                }
                LogTexMods("ERROR al cargar: %s%s", r.path.c_str(), "");
            }
        }
    }
    return g_origCreateTexMem(dev, data, size, w, h, mips, usage, fmt, pool, filter, mipFilter, colorKey,
                              srcInfo, palette, tex);
}

// Texturas crudas: TTextureResourceHAL::CreateFromMemory5551/4444/8888(w, h, mips, data)
// (thiscall, ret 0x10). Si hay reemplazo se crea con D3DX desde el archivo en this+0x60 y se deja
// en this+0x64/+0x68 el tamano original, igual que hace el motor.
typedef bool(__thiscall* CreateRaw_t)(BYTE*, UINT, UINT, UINT, const void*);
typedef void*(__thiscall* GetRenderer_t)(void*);
static CreateRaw_t g_origRaw[3] = {};
static GetRenderer_t g_getRenderer = nullptr;

static bool CreateRawReplacement(BYTE* res, UINT w, UINT h, const void* data, UINT bpp)
{
    if (g_texMods.empty() || !data || !g_getRenderer || !g_origCreateTexMem) return false;
    auto it = g_texMods.find(Fnv64((const BYTE*)data, (size_t)w * h * bpp));
    if (it == g_texMods.end()) return false;
    std::vector<BYTE> fileData;
    if (!ReadWholeFile(it->second.path, fileData) || fileData.empty()) return false;
    BYTE* renderer = (BYTE*)g_getRenderer(res);
    if (!renderer) return false;
    void* dev = *(void**)(renderer + 0x178);
    UINT info[5] = {};
    HRESULT hr = g_origCreateTexMem(dev, fileData.data(), (UINT)fileData.size(), (UINT)-1, (UINT)-1, (UINT)-1, 0, 0,
                                    1 /*MANAGED*/, (DWORD)-1, (DWORD)-1, 0, info, nullptr, (void**)(res + 0x60));
    if (hr < 0) { LogTexMods("ERROR al cargar: %s%s", it->second.path.c_str(), ""); return false; }
    *(UINT*)(res + 0x64) = w;
    *(UINT*)(res + 0x68) = h;
    g_texModsUsed++;
    return true;
}

static bool __fastcall Raw5551_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d)
{ return CreateRawReplacement(r, w, h, d, 2) || g_origRaw[0](r, w, h, m, d); }
static bool __fastcall Raw4444_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d)
{ return CreateRawReplacement(r, w, h, d, 2) || g_origRaw[1](r, w, h, m, d); }
static bool __fastcall Raw8888_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d)
{ return CreateRawReplacement(r, w, h, d, 4) || g_origRaw[2](r, w, h, m, d); }

static void* MakeTrampoline(BYTE* fn, size_t len, void* hook)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, fn, len);
    t[len] = 0xE9;
    *(int*)(t + len + 1) = (int)((fn + len) - (t + len + 5));
    WriteRel(fn, hook, 0xE9, len);
    return t;
}

static void InstallTextureMods()
{
    if (!g_texModsEnabled) return;
    ScanTextureMods();
    if (g_texMods.empty()) return;
    HMODULE rd3d = GetModuleHandleA("TRenderD3DInterface.dll");
    if (!rd3d) return;
    BYTE* fn = (BYTE*)rd3d + 0xf51a;
    static const unsigned char expect[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C, 0x04, 0x00, 0x00 };
    if (!BytesMatch(fn, expect, sizeof(expect))) {
        LogTexMods("ERROR: TRenderD3DInterface.dll distinto al esperado%s%s", "", "");
        return;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, fn, 9);
    t[9] = 0xE9;
    *(int*)(t + 10) = (int)((fn + 9) - (t + 14));
    g_origCreateTexMem = (D3DXCreateTexMemEx_t)t;
    WriteRel(fn, (void*)&CreateTexMem_Hook, 0xE9, 9);

    HMODULE ri = GetModuleHandleA("TRenderInterface.dll");
    g_getRenderer = ri ? (GetRenderer_t)GetProcAddress(ri, "?GetRenderer@TResource@Toshi@@QBEPAVTRenderInterface@2@XZ") : nullptr;
    struct { DWORD rva; unsigned char b6; void* hook; } raws[] = {
        { 0xcfd0, 0x1C, (void*)&Raw5551_Hook }, { 0xd1d0, 0x1C, (void*)&Raw4444_Hook }, { 0xd3d0, 0x2C, (void*)&Raw8888_Hook },
    };
    for (int i = 0; i < 3 && g_getRenderer; i++) {
        BYTE* f = (BYTE*)rd3d + raws[i].rva;
        const unsigned char ex[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, raws[i].b6 };
        if (BytesMatch(f, ex, sizeof(ex)))
            g_origRaw[i] = (CreateRaw_t)MakeTrampoline(f, 7, raws[i].hook);
    }
}

// ---------------------------------------------------------------------------------------------

static void LoadConfig(HMODULE self)
{
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy_s(dot, path + MAX_PATH - dot, ".ini");

    char buf[64];
    GetPrivateProfileStringA("Cursor", "Speed", "0.5", buf, sizeof(buf), path);
    g_cursorSpeed = (float)atof(buf);
    if (g_cursorSpeed <= 0.0f) g_cursorSpeed = 0.5f;
    g_disableAccel = GetPrivateProfileIntA("Cursor", "DisableAcceleration", 1, path) != 0;
    g_cursorAspectFix = GetPrivateProfileIntA("Cursor", "FixAspect", 1, path) != 0;

    GetPrivateProfileStringA("FirstPerson", "SliderMin", "0.05", buf, sizeof(buf), path);
    g_sliderMin = (float)atof(buf);
    if (g_sliderMin <= 0.0f || g_sliderMin > 0.25f) g_sliderMin = 0.05f;

    g_wheelFix = GetPrivateProfileIntA("Zoom", "FixWheel", 1, path) != 0;
    GetPrivateProfileStringA("Zoom", "WheelStrength", "1.0", buf, sizeof(buf), path);
    float strength = (float)atof(buf);
    if (strength <= 0.0f) strength = 1.0f;
    g_wheelDt = strength / 30.0f;

    g_horPlus = GetPrivateProfileIntA("Widescreen", "HorPlus", 1, path) != 0;
    g_fixHud = GetPrivateProfileIntA("Widescreen", "FixHUD", 1, path) != 0;
    g_hudMode = GetPrivateProfileIntA("Widescreen", "HUDMode", 2, path);
    if (!g_fixHud) g_hudMode = 0;
    if (g_hudMode < 0 || g_hudMode > 2) g_hudMode = 2;
    g_fixHud = g_hudMode != 0;
    g_texModsEnabled = GetPrivateProfileIntA("Mods", "TextureReplacement", 1, path) != 0;
    g_borderless = GetPrivateProfileIntA("Display", "Borderless", 1, path) != 0;
    g_forceWindowed = g_borderless; // borderless = ventana forzada + sin bordes
    g_altF4 = GetPrivateProfileIntA("Display", "AltF4Quit", 1, path) != 0;
    g_freeKeyboard = GetPrivateProfileIntA("Display", "FreeKeyboard", 1, path) != 0;

    GetPrivateProfileStringA("Camera", "EdgeScrollVertical", "1.5", buf, sizeof(buf), path);
    g_edgeVertical = (float)atof(buf);
    if (g_edgeVertical <= 0.0f) g_edgeVertical = 1.5f;
    GetPrivateProfileStringA("Camera", "EdgeScrollHorizontal", "1.5", buf, sizeof(buf), path);
    g_edgeHorizontal = (float)atof(buf);
    if (g_edgeHorizontal <= 0.0f) g_edgeHorizontal = 1.5f;

    g_smoothFix = GetPrivateProfileIntA("Camera", "FixSmoothing", 1, path) != 0;
    GetPrivateProfileStringA("Camera", "ReferenceFPS", "30", buf, sizeof(buf), path);
    g_refFps = (float)atof(buf);
    if (g_refFps < 10.0f || g_refFps > 300.0f) g_refFps = 30.0f;
}

static void ApplyPatches()
{
    // --- Cursor: TGuiInterface.dll ---
    HMODULE gui = GetModuleHandleA("TGuiInterface.dll");
    if (gui) {
        void* fn = (void*)GetProcAddress(gui, "?ApplyPointerAccel@TGUIScreen@Toshi@@IAEXAAH0@Z");
        static const unsigned char expect[] = { 0x57, 0x56, 0x55, 0x53, 0x56, 0x8B, 0xD9 };
        if (fn && BytesMatch(fn, expect, sizeof(expect)))
            WriteRel(fn, (void*)&ApplyPointerAccel_Hook, 0xE9, 5);
    }

    // --- Slider de sensibilidad FP: SimJP.exe (base fija 0x400000, sin ASLR) ---
    // AWindowsControlsOptionsDialog::AWindowsControlsOptionsDialog
    //   0x57d672: mov dword ptr [esp+4], 0x3e800000        (min = 0.25)
    //   0x57d682: mov dword ptr [ecx+0xa0], 0x3e800000     (min = 0.25)
    static const unsigned char expectA[] = { 0xC7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x80, 0x3E };
    static const unsigned char expectB[] = { 0xC7, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3E };
    BYTE* a = (BYTE*)0x57d672;
    BYTE* b = (BYTE*)0x57d682;
    if (BytesMatch(a, expectA, sizeof(expectA)) && BytesMatch(b, expectB, sizeof(expectB))) {
        PatchBytes(a + 4, &g_sliderMin, 4);
        PatchBytes(b + 6, &g_sliderMin, 4);
    }

    // --- Zoom con la rueda: SimJP.exe ---
    if (g_wheelFix) {
        static const unsigned char expectZoom[] = { 0xD9, 0x04, 0x24, 0x8B, 0x13, 0x57 };
        BYTE* zin = (BYTE*)0x55c5bf;
        BYTE* zout = (BYTE*)0x55c609;
        if (BytesMatch(zin, expectZoom, sizeof(expectZoom)) && BytesMatch(zout, expectZoom, sizeof(expectZoom))) {
            WriteRel(zin, (void*)&ZoomDtStub, 0xE8, 5);
            WriteRel(zout, (void*)&ZoomDtStub, 0xE8, 5);
        }
    }

    // --- Suavizado de camara: SimJP.exe ---
    if (g_smoothFix) {
        static const unsigned char expectD[] = { 0xD9, 0x05, 0x00, 0x2C, 0xD1, 0x00 };
        static const unsigned char expectOM[] = { 0xD9, 0x05, 0xF0, 0x61, 0x8B, 0x00 };
        BYTE* sd = (BYTE*)0x527d3f;
        BYTE* som = (BYTE*)0x527d51;
        if (BytesMatch(sd, expectD, sizeof(expectD)) && BytesMatch(som, expectOM, sizeof(expectOM))) {
            WriteRel(sd, (void*)&DampeningStub, 0xE8, 6);
            WriteRel(som, (void*)&OMDampeningStub, 0xE8, 6);
        }
    }

    // --- Velocidad del desplazamiento por bordes: SimJP.exe ---
    if (*kEdgeVertical == 2.0f && *kEdgeHorizontal == 1.0f) {
        PatchBytes(kEdgeVertical, &g_edgeVertical, 4);
        PatchBytes(kEdgeHorizontal, &g_edgeHorizontal, 4);
    }

    // --- Teclado no exclusivo: TInputDXInterface.dll (base relocable) ---
    // EnumerateDeviceCallback hace SetCooperativeLevel(hwnd, 0x15) = EXCLUSIVE|FOREGROUND|NOWINKEY
    // para el teclado, lo que impide que Windows reciba Impr Pant, Alt+F4 y la tecla Windows.
    // Se cambia por 0x06 = NONEXCLUSIVE|FOREGROUND.
    if (g_freeKeyboard) {
        HMODULE idx = GetModuleHandleA("TInputDXInterface.dll");
        if (idx) {
            BYTE* site = (BYTE*)idx + 0x39dc;
            static const unsigned char expectKb[] = { 0x6A, 0x15, 0x52, 0x56 };
            if (BytesMatch(site, expectKb, sizeof(expectKb))) {
                const unsigned char flags = 0x06;
                PatchBytes(site + 1, &flags, 1);
            }
        }
    }

    // --- Hor+: TRenderInterface.dll (base relocable) ---
    if (g_horPlus) {
        HMODULE ri = GetModuleHandleA("TRenderInterface.dll");
        if (ri) {
            BYTE* site = (BYTE*)ri + 0xebf5;
            unsigned char expect[12] = { 0xD9, 0x05, 0, 0, 0, 0, 0xD8, 0x4E, 0x20, 0xDE, 0xF1 };
            *(DWORD*)(expect + 2) = (DWORD)((BYTE*)ri + 0x30fcc); // fld dword ptr [0.5] (reubicado)
            if (BytesMatch(site, expect, 11))
                WriteRel(site, (void*)&HorPlusStub, 0xE8, 6);
        }
    }

    // --- HUD 4:3 centrado: PGUIRenderer.dll ---
    // Prologo: push ebp; push ebx; sub esp,1c; mov ebx,ecx  (7 bytes, sin relativos)
    if (g_fixHud) {
        HMODULE pr = GetModuleHandleA("PGUIRenderer.dll");
        BYTE* fn = pr ? (BYTE*)GetProcAddress(pr, "?BeginScene@PGUITRDisplayContext@@UAEXXZ") : nullptr;
        static const unsigned char expectBs[] = { 0x55, 0x53, 0x83, 0xEC, 0x1C, 0x8B, 0xD9 };
        if (fn && BytesMatch(fn, expectBs, sizeof(expectBs))) {
            BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (t) {
                memcpy(t, fn, 7);
                t[7] = 0xE9;
                *(int*)(t + 8) = (int)((fn + 7) - (t + 12));
                g_beginSceneTramp = t;
                WriteRel(fn, (void*)&BeginScene_Hook, 0xE9, 7);
            }
        }

        // HUD anclado: TGUIScreen::Paint (prologo push edi/esi/ebp/ebx; sub esp,14; mov ebx,ecx)
        if (g_hudMode == 2) {
            HMODULE tgi = GetModuleHandleA("TGuiInterface.dll");
            BYTE* pf = tgi ? (BYTE*)GetProcAddress(tgi, "?Paint@TGUIScreen@Toshi@@IAEXPAVTGUIWidget@2@@Z") : nullptr;
            static const unsigned char expectPf[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, 0x14, 0x8B, 0xD9 };
            if (pf && BytesMatch(pf, expectPf, sizeof(expectPf))) {
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (t) {
                    memcpy(t, pf, 9);
                    t[9] = 0xE9;
                    *(int*)(t + 10) = (int)((pf + 9) - (t + 14));
                    g_screenPaintTramp = t;
                    WriteRel(pf, (void*)&ScreenPaint_Hook, 0xE9, 9);
                }
            }
        }

        // Menus emergentes: PGUICore UpdatePosition (prologos sin saltos relativos)
        HMODULE pc = GetModuleHandleA("PGUICore.dll");
        if (pc && g_hudMode == 2) {
            struct { const char* name; const unsigned char* expect; size_t len; void* hook; void** tramp; } sites[] = {
                { "?UpdatePosition@PGUIMenuDialog@@UAEXAAH0@Z",
                  (const unsigned char*)"\x57\x56\x55\x53\x83\xEC\x24\x89\x4C\x24\x20", 11,
                  (void*)&MenuUpdatePosition_Hook, &g_menuUpdPosTramp },
                { "?UpdatePosition@PGUIDiscMenuDialog@@UAEXAAH0@Z",
                  (const unsigned char*)"\x57\x56\x55\x53\x83\xEC\x14\x8B\xD9", 9,
                  (void*)&DiscUpdatePosition_Hook, &g_discUpdPosTramp },
            };
            for (auto& site : sites) {
                BYTE* fn = (BYTE*)GetProcAddress(pc, site.name);
                if (!fn || !BytesMatch(fn, site.expect, site.len)) continue;
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (!t) continue;
                memcpy(t, fn, site.len);
                t[site.len] = 0xE9;
                *(int*)(t + site.len + 1) = (int)((fn + site.len) - (t + site.len + 5));
                *site.tramp = t;
                WriteRel(fn, site.hook, 0xE9, site.len);
            }
        }

        // Vistas 3D en la GUI: AGUIRenderObject::SetViewRectangle
        {
            BYTE* fn = (BYTE*)0x678a90;
            static const unsigned char expectVr[] = { 0x56, 0x55, 0x53, 0x8B, 0xF1, 0x8B, 0x6C, 0x24, 0x10 };
            if (BytesMatch(fn, expectVr, sizeof(expectVr))) {
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (t) {
                    memcpy(t, fn, 9);
                    t[9] = 0xE9;
                    *(int*)(t + 10) = (int)((fn + 9) - (t + 14));
                    g_setViewRectTramp = t;
                    WriteRel(fn, (void*)&SetViewRectangle_Hook, 0xE9, 9);
                }
            }
        }

        // Rango del cursor: TGUIScreen::SetPointerPosition
        HMODULE tg = GetModuleHandleA("TGuiInterface.dll");
        BYTE* sp = tg ? (BYTE*)GetProcAddress(tg, "?SetPointerPosition@TGUIScreen@Toshi@@QAE_NHH@Z") : nullptr;
        static const unsigned char expectSp[] = { 0x55, 0x83, 0xEC, 0x10, 0x0F, 0xBF, 0x41, 0x70 };
        if (sp && BytesMatch(sp, expectSp, sizeof(expectSp)))
            WriteRel(sp, (void*)&SetPointerPosition_Hook, 0xE9, 5);

        // Clic en el mundo 3D: ACamera::ConvertGuiScreenToRenderScreen
        static const unsigned char expectCv[] = { 0x55, 0x53, 0x83, 0xEC, 0x14, 0x8B, 0x6C, 0x24, 0x20 };
        BYTE* cv = (BYTE*)0x50bd60;
        if (BytesMatch(cv, expectCv, sizeof(expectCv)))
            WriteRel(cv, (void*)&ConvertGuiToRender_Hook, 0xE9, 5);

        BYTE* cr = (BYTE*)0x50bdd0;
        if (BytesMatch(cr, expectCv, sizeof(expectCv)))
            WriteRel(cr, (void*)&ConvertRenderToGui_Hook, 0xE9, 5);

        // Mundo -> GUI (iconos sobre unidades, seleccion)
        static const unsigned char expectTs[] = { 0x57, 0x56, 0x55, 0x83, 0xEC, 0x38, 0x8B, 0xF9, 0x8B, 0x74, 0x24, 0x48 };
        BYTE* ts = (BYTE*)0x477ec0;
        if (BytesMatch(ts, expectTs, sizeof(expectTs)))
            g_toScreenTramp = MakeTrampoline(ts, 12, (void*)&ToScreenCoords_Hook);

        // Desplazamiento por bordes
        static const unsigned char expectEx[] = { 0x8B, 0x9F, 0x14, 0x01, 0x00, 0x00 };
        BYTE* ex = (BYTE*)0x55cbe5;
        if (BytesMatch(ex, expectEx, sizeof(expectEx)))
            WriteRel(ex, (void*)&EdgeXStub, 0xE8, 6);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadConfig(inst);
        ApplyPatches();
        InstallD3DHooks();
        InstallTextureMods();
        if (g_borderless) {
            HANDLE t = CreateThread(nullptr, 0, BorderlessThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
