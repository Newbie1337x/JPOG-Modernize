// profiler.cpp - in-process sampling profiler (no administrator rights, no external tools).
//
// d3d8to11_profile.req in the game folder starts it (automation.cpp). A background thread
// samples the render thread's instruction pointer about every millisecond for the requested
// time (the file may hold the seconds, default 15): the thread is suspended for a moment, its
// context read and resumed. Samples taken while the thread waits (Present, Sleep, driver
// waits) land in ntdll/win32u/kernel stubs, so the split is of wall time, like the frame time.
// The report (d3d8to11_profile.txt) gives the time per module, the top functions (names from
// PDBs next to the modules or from export tables, through dbghelp) and the CPU time of every
// thread of the process in that window, with the module its start address belongs to.
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>
#include <algorithm>
#include <map>
#include <thread>
#include <vector>

namespace {
struct Module { uintptr_t base, end; std::string name; };

std::vector<Module> ListModules()
{
    std::vector<Module> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi = {};
        char path[MAX_PATH] = {};
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        std::string name = path;
        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        out.push_back({ (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, name });
    }
    return out;
}

const Module* Find(const std::vector<Module>& mods, uintptr_t a)
{
    for (const Module& m : mods) if (a >= m.base && a < m.end) return &m;
    return nullptr;
}

// dbghelp, loaded on demand (SYMBOL_INFO with room for the name).
typedef BOOL(WINAPI* SymInitialize_t)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI* SymCleanup_t)(HANDLE);
typedef DWORD(WINAPI* SymSetOptions_t)(DWORD);
typedef BOOL(WINAPI* SymFromAddr_t)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

typedef NTSTATUS(NTAPI* NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ThreadTimes { DWORD id; ULONGLONG cpu; uintptr_t start; };

ULONGLONG ThreadCpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

std::vector<ThreadTimes> SnapshotThreads()
{
    std::vector<ThreadTimes> out;
    static auto query = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ThreadTimes t = { te.th32ThreadID, ThreadCpu(h), 0 };
        if (query) query(h, 9 /* ThreadQuerySetWin32StartAddress */, &t.start, sizeof(t.start), nullptr);
        out.push_back(t);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
}

void Run(DWORD target, int seconds, std::string out)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, target);
    if (!thread) { Log("Profiler: cannot open the render thread (%lu)", GetLastError()); return; }
    typedef UINT(WINAPI* Period_t)(UINT);
    HMODULE winmm = LoadLibraryA("winmm.dll");
    Period_t begin = winmm ? (Period_t)GetProcAddress(winmm, "timeBeginPeriod") : nullptr;
    Period_t end = winmm ? (Period_t)GetProcAddress(winmm, "timeEndPeriod") : nullptr;
    if (begin) begin(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const std::vector<ThreadTimes> before = SnapshotThreads();
    std::vector<uintptr_t> samples;
    samples.reserve((size_t)seconds * 1100);
    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    do {
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &c)) samples.push_back((uintptr_t)c.Eip);
            ResumeThread(thread);
        }
        Sleep(1);
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - t0.QuadPart < f.QuadPart * seconds);
    const double wall = (double)(now.QuadPart - t0.QuadPart) / f.QuadPart;
    const std::vector<ThreadTimes> after = SnapshotThreads();
    if (end) end(1);
    CloseHandle(thread);

    // Attribution.
    const std::vector<Module> mods = ListModules();
    std::map<std::string, size_t> perModule;
    std::map<uintptr_t, size_t> perAddress;
    for (uintptr_t a : samples) {
        const Module* m = Find(mods, a);
        perModule[m ? m->name : "(unknown)"]++;
        perAddress[a]++;
    }
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    auto symInit = dbghelp ? (SymInitialize_t)GetProcAddress(dbghelp, "SymInitialize") : nullptr;
    auto symOpts = dbghelp ? (SymSetOptions_t)GetProcAddress(dbghelp, "SymSetOptions") : nullptr;
    auto symFrom = dbghelp ? (SymFromAddr_t)GetProcAddress(dbghelp, "SymFromAddr") : nullptr;
    auto symDone = dbghelp ? (SymCleanup_t)GetProcAddress(dbghelp, "SymCleanup") : nullptr;
    bool syms = false;
    if (symInit && symFrom) {
        if (symOpts) symOpts(0x00000002 /* UNDNAME */ | 0x00000004 /* DEFERRED_LOADS */);
        syms = symInit(GetCurrentProcess(), g_gameDir.c_str(), TRUE) != FALSE;
    }
    std::map<std::string, size_t> perFunction;
    for (const auto& e : perAddress) {
        const Module* m = Find(mods, e.first);
        std::string name = m ? m->name : "(unknown)";
        alignas(8) BYTE buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* si = (SYMBOL_INFO*)buffer;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (syms && symFrom(GetCurrentProcess(), e.first, &disp, si)) name += std::string("!") + si->Name;
        else if (m) { char b[32]; sprintf_s(b, "+0x%05x", (unsigned)((e.first - m->base) & ~0xFFFu)); name += b; }
        perFunction[name] += e.second;
    }
    if (syms && symDone) symDone(GetCurrentProcess());

    FILE* fp = nullptr;
    if (fopen_s(&fp, out.c_str(), "w") || !fp) { Log("Profiler: cannot write %s", out.c_str()); return; }
    const double n = samples.empty() ? 1.0 : (double)samples.size();
    fprintf(fp, "Render thread %lu: %zu samples in %.1f s (%.0f per second)\n\n", target, samples.size(), wall, samples.size() / wall);
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& e : perModule) sorted.push_back({ e.second, e.first });
    std::sort(sorted.rbegin(), sorted.rend());
    fprintf(fp, "Time per module (render thread, wall time):\n");
    for (const auto& e : sorted) fprintf(fp, "  %6.2f%%  %s\n", 100.0 * e.first / n, e.second.c_str());
    sorted.clear();
    for (const auto& e : perFunction) sorted.push_back({ e.second, e.first });
    std::sort(sorted.rbegin(), sorted.rend());
    fprintf(fp, "\nTop functions (render thread):\n");
    for (size_t i = 0; i < sorted.size() && i < 80; i++) fprintf(fp, "  %6.2f%%  %s\n", 100.0 * sorted[i].first / n, sorted[i].second.c_str());
    fprintf(fp, "\nCPU time per thread in the window (ms of CPU per second of wall time):\n");
    std::vector<std::pair<double, std::string>> threads;
    for (const ThreadTimes& a : after) {
        ULONGLONG was = 0;
        for (const ThreadTimes& b : before) if (b.id == a.id) was = b.cpu;
        const double ms = (a.cpu - was) / 10000.0 / wall;
        const Module* m = Find(mods, a.start);
        char line[160];
        sprintf_s(line, "thread %5lu%s  start in %s", a.id, a.id == target ? " (render)" : "", m ? m->name.c_str() : "?");
        threads.push_back({ ms, line });
    }
    std::sort(threads.rbegin(), threads.rend());
    for (const auto& t : threads) if (t.first >= 0.5) fprintf(fp, "  %7.1f  %s\n", t.first, t.second.c_str());
    fclose(fp);
    Log("Profiler: %zu samples written to %s", samples.size(), out.c_str());
}
}

void StartSamplingProfiler(int seconds)
{
    static volatile LONG running = 0;
    if (InterlockedCompareExchange(&running, 1, 0) != 0) return;
    const DWORD target = GetCurrentThreadId();
    const std::string out = g_gameDir + "d3d8to11_profile.txt";
    Log("Profiler: sampling the render thread %lu for %d s", target, seconds);
    std::thread([target, seconds, out]() { Run(target, seconds, out); InterlockedExchange(&running, 0); }).detach();
}
