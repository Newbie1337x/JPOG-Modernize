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
// Two more views: the hottest source lines of modules with line information (d3d8.dll with
// its PDB), and, for samples in system modules (d3d11, the driver, ntdll, the CRT), the first
// caller on the stack that belongs to the game or to us, found by scanning the stack for
// return addresses that follow a call instruction.
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>
#include <algorithm>
#include <map>
#include <thread>
#include <vector>

namespace {
struct Module { uintptr_t base, end; std::string name; bool system; };

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
        std::string lower = path;
        for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
        const bool system = lower.find("\\windows\\") != std::string::npos || lower.find("vcruntime") != std::string::npos ||
            lower.find("msvcp") != std::string::npos || lower.find("ucrtbase") != std::string::npos;
        out.push_back({ (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, name, system });
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
typedef BOOL(WINAPI* SymGetLine_t)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

const int kStackWords = 96;

// Copies the top of a suspended thread's stack; false if a page is not readable.
bool CopyStack(DWORD esp, DWORD* out)
{
    __try { memcpy(out, (const void*)(uintptr_t)esp, kStackWords * sizeof(DWORD)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// True when the bytes before a return address are a call: E8 rel32, FF 15 [abs], FF /2 reg or memory.
bool FollowsCall(uintptr_t r)
{
    __try {
        const BYTE* p = (const BYTE*)r;
        if (p[-5] == 0xE8) return true;
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;  // FF /2 with disp32 (incl. FF 15)
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;  // FF /2 disp8
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;  // FF /2 register or [reg]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;  // FF /2 SIB + disp32
        if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;  // FF /2 SIB + disp8
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

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
    std::vector<DWORD> stacks;
    stacks.reserve((size_t)seconds * 1100 * kStackWords);
    DWORD stack[kStackWords];
    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    do {
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &c)) {
                samples.push_back((uintptr_t)c.Eip);
                if (!CopyStack(c.Esp, stack)) memset(stack, 0, sizeof(stack));
                stacks.insert(stacks.end(), stack, stack + kStackWords);
            }
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
        if (symOpts) symOpts(0x00000002 /* UNDNAME */ | 0x00000004 /* DEFERRED_LOADS */ | 0x00000010 /* LOAD_LINES */);
        syms = symInit(GetCurrentProcess(), g_gameDir.c_str(), TRUE) != FALSE;
    }
    std::map<std::string, size_t> perFunction;
    std::map<std::string, DWORD64> maxDisp;
    for (const auto& e : perAddress) {
        const Module* m = Find(mods, e.first);
        std::string name = m ? m->name : "(unknown)";
        alignas(8) BYTE buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* si = (SYMBOL_INFO*)buffer;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (syms && symFrom(GetCurrentProcess(), e.first, &disp, si)) {
            char at[48];
            sprintf_s(at, " [+0x%x]", m ? (unsigned)(si->Address - m->base) : 0u);
            name += std::string("!") + si->Name + at;
            if (disp > maxDisp[name]) maxDisp[name] = disp;
        }
        else if (m) { char b[32]; sprintf_s(b, "+0x%05x", (unsigned)((e.first - m->base) & ~0xFFFu)); name += b; }
        perFunction[name] += e.second;
    }
    // Source lines (only modules whose symbols carry line numbers).
    auto symLine = dbghelp ? (SymGetLine_t)GetProcAddress(dbghelp, "SymGetLineFromAddr64") : nullptr;
    std::map<std::string, size_t> perLine;
    if (syms && symLine) {
        for (const auto& e : perAddress) {
            IMAGEHLP_LINE64 line = { sizeof(line) };
            DWORD disp = 0;
            if (!symLine(GetCurrentProcess(), e.first, &disp, &line) || !line.FileName) continue;
            std::string file = line.FileName;
            const size_t slash = file.find_last_of("\\/");
            if (slash != std::string::npos) file = file.substr(slash + 1);
            perLine[file + ":" + std::to_string(line.LineNumber)] += e.second;
        }
    }
    // Samples in system modules, by the first game/proxy caller on the stack.
    std::map<std::string, size_t> perCaller;
    auto symbolName = [&](uintptr_t a) {
        const Module* m = Find(mods, a);
        std::string s = m ? m->name : "(unknown)";
        alignas(8) BYTE buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* si = (SYMBOL_INFO*)buffer;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (syms && symFrom(GetCurrentProcess(), a, &disp, si)) s += std::string("!") + si->Name;
        return s;
    };
    std::map<uintptr_t, std::string> names;
    auto cachedName = [&](uintptr_t a) -> const std::string& {
        auto it = names.find(a);
        if (it == names.end()) it = names.emplace(a, symbolName(a)).first;
        return it->second;
    };
    size_t systemSamples = 0;
    for (size_t i = 0; i < samples.size(); i++) {
        const Module* m = Find(mods, samples[i]);
        if (!m || !m->system) continue;
        systemSamples++;
        const DWORD* st = &stacks[i * kStackWords];
        std::string caller = "(no caller found)";
        for (int w = 0; w < kStackWords; w++) {
            const Module* cm = Find(mods, st[w]);
            if (!cm || cm->system || !FollowsCall(st[w])) continue;
            caller = cachedName(st[w]);
            IMAGEHLP_LINE64 line = { sizeof(line) };
            DWORD lineDisp = 0;
            if (symLine && symLine(GetCurrentProcess(), st[w] - 1, &lineDisp, &line) && line.FileName) {
                std::string file = line.FileName;
                const size_t slash = file.find_last_of("\\/");
                if (slash != std::string::npos) file = file.substr(slash + 1);
                caller += " (" + file + ":" + std::to_string(line.LineNumber) + ")";
            }
            break;
        }
        perCaller[caller + "  ->  " + m->name]++;
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
    for (size_t i = 0; i < sorted.size() && i < 80; i++) {
        auto d = maxDisp.find(sorted[i].second);
        if (d != maxDisp.end()) fprintf(fp, "  %6.2f%%  %s  (samples up to +0x%llx)\n", 100.0 * sorted[i].first / n, sorted[i].second.c_str(), d->second);
        else fprintf(fp, "  %6.2f%%  %s\n", 100.0 * sorted[i].first / n, sorted[i].second.c_str());
    }
    sorted.clear();
    for (const auto& e : perLine) sorted.push_back({ e.second, e.first });
    std::sort(sorted.rbegin(), sorted.rend());
    fprintf(fp, "\nHot source lines (modules with line information):\n");
    for (size_t i = 0; i < sorted.size() && i < 60; i++) fprintf(fp, "  %6.2f%%  %s\n", 100.0 * sorted[i].first / n, sorted[i].second.c_str());
    sorted.clear();
    for (const auto& e : perCaller) sorted.push_back({ e.second, e.first });
    std::sort(sorted.rbegin(), sorted.rend());
    fprintf(fp, "\nSystem-module time (%.2f%% of samples) by first game/proxy caller:\n", 100.0 * systemSamples / n);
    for (size_t i = 0; i < sorted.size() && i < 60; i++) fprintf(fp, "  %6.2f%%  %s\n", 100.0 * sorted[i].first / n, sorted[i].second.c_str());
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
