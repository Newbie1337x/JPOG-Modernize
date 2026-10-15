// shader_jobs.cpp - see shader_jobs.h.
#include "shader_jobs.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace shader_jobs {
namespace {
// Never freed: workers may still be waiting on it while the process exits.
struct Queue {
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> urgent, prewarm;
    bool started = false;
    UINT prewarmDone = 0;
    LONGLONG prewarmStart = 0, prewarmLogged = 0;
};
Queue& Q() { static Queue* q = new Queue(); return *q; }

double Ms(LONGLONG from, LONGLONG to)
{
    static LONGLONG freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return (to - from) * 1000.0 / freq;
}
LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

void Worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Queue& q = Q();
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> l(q.lock);
            q.wake.wait(l, [&] { return !q.urgent.empty() || !q.prewarm.empty(); });
            std::deque<std::shared_ptr<Job>>& from = q.urgent.empty() ? q.prewarm : q.urgent;
            job = std::move(from.front());
            from.pop_front();
        }
        const LONGLONG start = Now();
        if (ComPtr<ID3DBlob> blob = CompileHLSL(job->hlsl, "ps_4_0", job->name)) {
            const HRESULT hr = job->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &job->shader);
            if (FAILED(hr)) Log("pixel shader %s: CreatePixelShader failed 0x%08lx", job->name.c_str(), hr);
        }
        job->device.Reset();
        job->hlsl.clear();
        job->hlsl.shrink_to_fit();
        job->done.store(true, std::memory_order_release);
        const LONGLONG end = Now();
        if (!job->prewarm) {
            Log("pixel shader %s ready in the background in %.0f ms", job->name.c_str(), Ms(start, end));
            continue;
        }
        std::lock_guard<std::mutex> l(q.lock);
        ++q.prewarmDone;
        // The game creates its shaders in small groups, so the queue empties many times while loading.
        if (q.prewarm.empty() && Ms(q.prewarmLogged, end) > 2000) {
            Log("shader prewarm: %u variants ready %.0f ms after the first request", q.prewarmDone, Ms(q.prewarmStart, end));
            q.prewarmLogged = end;
        }
    }
}

// ---- variants.bin --------------------------------------------------------------------------------
// Header {magic, version, sizeof(PSVariant), sizeof(FFKey)}, then records: a type byte (1 = game
// shader variant: function hash + PSVariant, 2 = FFKey) and its bytes. A file written with other
// structure sizes is ignored and replaced.
//
// d3d8to11_shadervariants.bin next to the game, installed with the renderer, has the same format:
// the variants of a full play session, so even the first session prewarms them while loading. The
// variants found in play are appended to d3d8to11_shadercache\variants.bin only.
const DWORD kMagic = 'MV8D', kVersion = 1;
struct Manifest {
    bool loaded = false;
    std::multimap<UINT64, PSVariant> variants;
    std::vector<FFKey> ff;
    std::set<std::string> seen; // record bytes
    std::mutex lock;            // the facade thread creates shaders while the render thread draws (Threaded)
};
Manifest& M() { static Manifest* m = new Manifest(); return *m; }
std::string ManifestPath() { return g_gameDir + "d3d8to11_shadercache\\variants.bin"; }

// False when the file exists but is not a variant list of this build.
bool Read(const std::string& path)
{
    Manifest& m = M();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return true;
    DWORD header[4] = {};
    bool valid = fread(header, sizeof(header), 1, f) == 1 && header[0] == kMagic && header[1] == kVersion &&
                 header[2] == sizeof(PSVariant) && header[3] == sizeof(FFKey);
    while (valid) {
        const int type = fgetc(f);
        if (type == 1) {
            char rec[8 + sizeof(PSVariant)];
            if (fread(rec, sizeof(rec), 1, f) != 1) break;
            if (!m.seen.insert(std::string(1, '\1') + std::string(rec, sizeof(rec))).second) continue;
            UINT64 h; PSVariant v;
            memcpy(&h, rec, 8);
            memcpy(&v, rec + 8, sizeof(v));
            m.variants.insert({ h, v });
        } else if (type == 2) {
            FFKey k;
            if (fread(&k, sizeof(k), 1, f) != 1) break;
            if (!m.seen.insert(std::string(1, '\2') + std::string((const char*)&k, sizeof(k))).second) continue;
            m.ff.push_back(k);
        } else {
            break;
        }
    }
    fclose(f);
    return valid;
}

void Load()
{
    Manifest& m = M();
    if (m.loaded) return;
    m.loaded = true;
    if (!Read(g_gameDir + "d3d8to11_shadervariants.bin")) Log("d3d8to11_shadervariants.bin is from another build, ignored");
    if (!Read(ManifestPath())) DeleteFileA(ManifestPath().c_str());
    Log("shader prewarm list: %zu game shader variants, %zu fixed-function", m.variants.size(), m.ff.size());
}

void Append(const std::string& record)
{
    Manifest& m = M();
    Load();
    if (!m.seen.insert(record).second) return;
    CreateDirectoryA((g_gameDir + "d3d8to11_shadercache").c_str(), nullptr);
    const std::string path = ManifestPath();
    const bool fresh = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "ab") || !f) return;
    if (fresh) {
        const DWORD header[4] = { kMagic, kVersion, sizeof(PSVariant), sizeof(FFKey) };
        fwrite(header, sizeof(header), 1, f);
    }
    fwrite(record.data(), record.size(), 1, f);
    fclose(f);
}
}

std::shared_ptr<Job> Submit(ID3D11Device* device, std::string hlsl, std::string name, bool prewarm)
{
    auto job = std::make_shared<Job>();
    job->hlsl = std::move(hlsl);
    job->name = std::move(name);
    job->device = device;
    job->prewarm = prewarm;
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> l(q.lock);
        if (!q.started) {
            q.started = true;
            const unsigned cores = std::thread::hardware_concurrency();
            const unsigned workers = cores > 4 ? 3 : cores > 2 ? 2 : 1;
            for (unsigned i = 0; i < workers; i++) std::thread(Worker).detach();
        }
        if (prewarm && q.prewarm.empty()) { q.prewarmStart = Now(); q.prewarmDone = 0; }
        (prewarm ? q.prewarm : q.urgent).push_back(job);
    }
    q.wake.notify_one();
    return job;
}

void Promote(const std::shared_ptr<Job>& job)
{
    Queue& q = Q();
    std::lock_guard<std::mutex> l(q.lock);
    for (auto it = q.prewarm.begin(); it != q.prewarm.end(); ++it) {
        if (*it != job) continue;
        q.prewarm.erase(it);
        q.urgent.push_front(job);
        return;
    }
}

UINT64 FunctionHash(const std::vector<DWORD>& func)
{
    UINT64 h = 1469598103934665603ull;
    const BYTE* p = (const BYTE*)func.data();
    for (size_t i = 0; i < func.size() * 4; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

void RecordVariant(UINT64 function, const PSVariant& v)
{
    std::lock_guard<std::mutex> l(M().lock);
    std::string r(1, '\1');
    r.append((const char*)&function, 8);
    r.append((const char*)&v, sizeof(v));
    Append(r);
}

void RecordFF(const FFKey& k)
{
    std::lock_guard<std::mutex> l(M().lock);
    Append(std::string(1, '\2') + std::string((const char*)&k, sizeof(k)));
}

std::vector<PSVariant> KnownVariants(UINT64 function)
{
    std::lock_guard<std::mutex> l(M().lock);
    Load();
    std::vector<PSVariant> out;
    auto range = M().variants.equal_range(function);
    for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
    return out;
}

std::vector<FFKey> KnownFF()
{
    std::lock_guard<std::mutex> l(M().lock);
    Load();
    return M().ff;
}
}
