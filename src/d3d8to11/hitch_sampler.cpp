// hitch_sampler.cpp - see hitch_sampler.h.
#include "hitch_sampler.h"
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mmsystem.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

namespace hitch_sampler {
namespace {
const int kStackWords = 192; // the top 768 bytes of the stack
const UINT kSamples = 8192;  // about 8 s at 1 kHz
struct Sample { LONGLONG t; DWORD eip; DWORD stack[kStackWords]; };

Sample* g_ring = nullptr;
std::atomic<UINT> g_written{0};
HANDLE g_target = nullptr, g_thread = nullptr;
std::atomic<bool> g_stop{false};
LONGLONG g_frameStart = 0, g_freq = 0;
UINT g_logged = 0;

DWORD WINAPI Run(void*)
{
    timeBeginPeriod(1);
    CONTEXT ctx;
    while (!g_stop.load()) {
        Sleep(1);
        if (SuspendThread(g_target) == (DWORD)-1) continue;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &ctx)) {
            Sample& s = g_ring[g_written.load() % kSamples];
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            s.t = now.QuadPart;
            s.eip = ctx.Eip;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)(uintptr_t)ctx.Esp, s.stack, sizeof(s.stack), &got)) got = 0;
            for (size_t i = got / 4; i < kStackWords; i++) s.stack[i] = 0;
            ResumeThread(g_target);
            g_written.fetch_add(1);
        } else {
            ResumeThread(g_target);
        }
    }
    timeEndPeriod(1);
    return 0;
}

struct Module { uintptr_t base, end; std::string name; };
std::vector<Module> g_modules;

void LoadModules()
{
    g_modules.clear();
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++) {
        MODULEINFO mi;
        char path[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) || !GetModuleFileNameA(mods[i], path, MAX_PATH)) continue;
        const char* base = strrchr(path, '\\');
        g_modules.push_back({ (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, base ? base + 1 : path });
    }
}

const Module* ModuleOf(DWORD a)
{
    for (const Module& m : g_modules) if (a >= m.base && a < m.end) return &m;
    return nullptr;
}

// A return address: inside a module and right after a call instruction.
bool IsReturnAddress(DWORD a)
{
    if (!ModuleOf(a) || a < 8) return false;
    const BYTE* p = (const BYTE*)(uintptr_t)a;
    __try {
        if (p[-5] == 0xE8) return true;                                   // call rel32
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;         // call [disp32] / [reg+disp32]
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;         // call [reg+disp8]
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;         // call reg / [reg]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;         // call [reg*s+disp32]
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

std::string Name(DWORD a)
{
    const Module* m = ModuleOf(a);
    char s[160];
    if (m) sprintf_s(s, "%s+0x%x", m->name.c_str(), (unsigned)(a - m->base));
    else sprintf_s(s, "0x%08lx", a);
    return s;
}

std::string Top(const std::map<DWORD, UINT>& counts, UINT samples, size_t n)
{
    std::vector<std::pair<UINT, DWORD>> v;
    for (auto& c : counts) v.push_back({ c.second, c.first });
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (size_t i = 0; i < v.size() && i < n; i++) {
        char item[200];
        sprintf_s(item, "\n    %3.0f%% %s", 100.0 * v[i].first / samples, Name(v[i].second).c_str());
        s += item;
    }
    return s;
}

// The samples taken between start and end, added to where (instruction pointers) and callers.
UINT Collect(LONGLONG start, LONGLONG end, std::map<DWORD, UINT>& where, std::map<DWORD, UINT>& callers)
{
    const UINT written = g_written.load();
    const UINT first = written > kSamples ? written - kSamples : 0;
    UINT samples = 0;
    for (UINT i = first; i < written; i++) {
        const Sample& s = g_ring[i % kSamples];
        if (s.t < start || s.t > end) continue;
        samples++;
        where[s.eip]++;
        // Each caller once per sample; the nearest ones carry the most information.
        DWORD seen[48]; int n = 0;
        for (int k = 0; k < kStackWords && n < 48; k++) {
            const DWORD a = s.stack[k];
            if (!IsReturnAddress(a)) continue;
            bool dup = false;
            for (int j = 0; j < n; j++) if (seen[j] == a) { dup = true; break; }
            if (dup) continue;
            seen[n++] = a;
            callers[a]++;
        }
    }
    return samples;
}

void Report(LONGLONG start, LONGLONG end, double ms)
{
    std::map<DWORD, UINT> where, callers;
    LoadModules();
    const UINT samples = Collect(start, end, where, callers);
    if (!samples) { Log("Hitch %.0f ms: no samples", ms); return; }
    // Group the instruction pointers by 256-byte block: one function is usually one or two blocks.
    std::map<DWORD, UINT> blocks;
    for (auto& w : where) blocks[w.first & ~0xFFu] += w.second;
    Log("Hitch %.0f ms on the game thread, %u samples. Where (256-byte blocks):%s\n  Callers on the stack:%s",
        ms, samples, Top(blocks, samples, 12).c_str(), Top(callers, samples, 24).c_str());
}

// HitchProfileSeconds: the slow frames of a period, summed.
std::map<DWORD, UINT> g_profWhere, g_profCallers;
UINT g_profSamples = 0, g_profFrames = 0, g_profAllFrames = 0;
double g_profMs = 0, g_profAllMs = 0;
LONGLONG g_profStart = 0;

void ProfileFrame(LONGLONG start, LONGLONG end, double ms)
{
    if (g_modules.empty()) LoadModules();
    g_profAllFrames++;
    g_profAllMs += ms;
    if (ms >= g_config.hitchThresholdMs) {
        g_profSamples += Collect(start, end, g_profWhere, g_profCallers);
        g_profFrames++;
        g_profMs += ms;
    }
    if ((end - g_profStart) < (LONGLONG)g_config.hitchProfileSeconds * g_freq) return;
    if (g_profSamples) {
        LoadModules();
        std::map<std::string, UINT> modules;
        for (auto& w : g_profWhere) { const Module* m = ModuleOf(w.first); modules[m ? m->name : "?"] += w.second; }
        std::vector<std::pair<UINT, std::string>> mv;
        for (auto& m : modules) mv.push_back({ m.second, m.first });
        std::sort(mv.rbegin(), mv.rend());
        std::string mods;
        for (size_t i = 0; i < mv.size() && i < 10; i++) {
            char item[200];
            sprintf_s(item, "\n    %3.0f%% %s", 100.0 * mv[i].first / g_profSamples, mv[i].second.c_str());
            mods += item;
        }
        std::map<DWORD, UINT> blocks;
        for (auto& w : g_profWhere) blocks[w.first & ~0xFFu] += w.second;
        Log("CPU profile: %u of %u frames over %d ms (avg %.1f ms, all frames %.1f ms), %u samples.\n  Modules:%s\n"
            "  Where (256-byte blocks):%s\n  Callers on the stack:%s",
            g_profFrames, g_profAllFrames, g_config.hitchThresholdMs, g_profMs / g_profFrames, g_profAllMs / g_profAllFrames,
            g_profSamples, mods.c_str(), Top(blocks, g_profSamples, 40).c_str(), Top(g_profCallers, g_profSamples, 60).c_str());
    }
    g_profWhere.clear(); g_profCallers.clear();
    g_profSamples = g_profFrames = g_profAllFrames = 0;
    g_profMs = g_profAllMs = 0;
    g_profStart = end;
}
}

void FramePresented()
{
    if (!g_config.hitchSampler) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_thread) {
        g_ring = (Sample*)VirtualAlloc(nullptr, sizeof(Sample) * kSamples, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0) || !g_ring) {
            g_config.hitchSampler = false;
            return;
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
        g_thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
        SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
        Log("Hitch sampler on: frames over %d ms of thread %lu are reported", g_config.hitchThresholdMs, GetCurrentThreadId());
        g_frameStart = g_profStart = now.QuadPart;
        return;
    }
    const double ms = (now.QuadPart - g_frameStart) * 1000.0 / g_freq;
    if (g_config.hitchProfileSeconds > 0) {
        ProfileFrame(g_frameStart, now.QuadPart, ms);
        QueryPerformanceCounter(&now);
    } else if (ms >= g_config.hitchThresholdMs && g_logged < 300) {
        g_logged++;
        Report(g_frameStart, now.QuadPart, ms);
        QueryPerformanceCounter(&now); // the report is not part of the next frame
    }
    g_frameStart = now.QuadPart;
}

void Stop()
{
    if (!g_thread) return;
    g_stop = true;
    WaitForSingleObject(g_thread, 1000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}
}
