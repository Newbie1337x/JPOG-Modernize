// csmt.h - command stream to a render thread ([Performance] Threaded).
//
// The game thread records its Direct3D 8 calls into a single-producer/single-consumer ring and a
// worker thread executes them, in order, on the real Device: the translation, the D3D11 runtime
// and the driver run on another core. Commands are lambdas stored inline in the ring; the ones
// that carry data (constants, rectangles, buffer contents) copy it after the lambda.
#pragma once
#include "common.h"
#include <new>
#include <utility>

class CommandQueue {
public:
    ~CommandQueue() { Stop(); }
    bool Start();
    void Stop(); // runs everything queued, then ends the worker

    bool OnWorker() const { return GetCurrentThreadId() == m_workerId; }
    // True on the game thread while the worker runs: calls must go through the queue.
    bool OffWorker() const { return m_running && !OnWorker(); }
    DWORD WorkerThreadId() const { return m_workerId; }

    // Runs f() on the worker, later.
    template<class F> void Post(F&& f)
    {
        typedef typename std::decay<F>::type L;
        void* p = Reserve(sizeof(Header) + Align(sizeof(L)));
        Header* h = (Header*)p;
        h->size = (uint32_t)(sizeof(Header) + Align(sizeof(L)));
        h->run = [](void* body) { L* l = (L*)body; (*l)(); l->~L(); };
        new (h + 1) L(std::forward<F>(f));
        Commit(h->size);
    }
    // Runs f(data) on the worker with a copy of 'bytes' bytes from src, taken now.
    template<class F> void PostData(const void* src, size_t bytes, F&& f)
    {
        typedef typename std::decay<F>::type L;
        const size_t total = sizeof(Header) + Align(sizeof(L)) + Align(bytes);
        if (total > kBytes / 4) { // too large for the ring: run it now, while the data is valid
            L l(std::forward<F>(f));
            Sync([&]() { l(src); });
            return;
        }
        void* p = Reserve(total);
        Header* h = (Header*)p;
        h->size = (uint32_t)total;
        h->run = [](void* body) { L* l = (L*)body; l->operator()((const void*)((BYTE*)body + Align(sizeof(L)))); l->~L(); };
        new (h + 1) L(std::forward<F>(f));
        if (bytes) memcpy((BYTE*)(h + 1) + Align(sizeof(L)), src, bytes);
        Commit(h->size);
    }
    // Runs f() on the worker and waits for it (and for everything queued before it).
    template<class F> void Sync(F&& f)
    {
        if (!OffWorker()) { f(); return; }
        Post([&f, this]() { f(); SetEvent(m_syncEvent); });
        WakeWorker();
        Wait(m_syncEvent, INFINITE);
    }
    void Drain() { Sync([] {}); }

    // Frame pacing: the game records at most one frame ahead of the worker.
    void FramePosted();
    void FrameDone();

private:
    struct Header { uint32_t size; void (*run)(void*); }; // run == nullptr: skip to the ring start
    static const size_t kBytes = 32u << 20;
    static const uint32_t kMask = (uint32_t)kBytes - 1; // kBytes is a power of two
    static size_t Align(size_t n) { return (n + 7) & ~(size_t)7; }
    void* Reserve(size_t bytes);
    void Commit(size_t bytes)
    {
        // A plain volatile store publishes the command (x86 stores are ordered); the 64-bit
        // interlocked counters this replaces cost a lock cmpxchg8b per call. The sleeping check
        // can miss a worker that is just going to sleep: WakeWorker() covers it before any wait.
        m_reserved += (uint32_t)bytes;
        m_written = m_reserved;
        if (m_workerSleeping) SetEvent(m_workEvent);
    }
    void WakeWorker() { MemoryBarrier(); if (m_workerSleeping) SetEvent(m_workEvent); }
    void Wait(HANDLE event, DWORD timeout);
    static DWORD WINAPI ThreadMain(void* self);
    void Run();

    // Fields written by one thread and read by the other sit on their own cache lines: the
    // worker's per-command m_consumed store must not evict the producer's private counters.
    BYTE* m_ring = nullptr;
    uint32_t m_reserved = 0;                         // producer-only: written + reserved bytes
    uint32_t m_limit = 0;                            // producer-only: last seen m_consumed + kBytes
    HANDLE m_workEvent = nullptr, m_spaceEvent = nullptr, m_syncEvent = nullptr, m_frameEvent = nullptr;
    HANDLE m_thread = nullptr;
    DWORD m_workerId = 0;
    alignas(64) volatile uint32_t m_written = 0;     // byte counters, wrap around (only differences matter)
    alignas(64) volatile uint32_t m_consumed = 0;
    alignas(64) volatile LONG m_workerSleeping = 0;
    alignas(64) volatile LONG m_producerWaiting = 0;
    alignas(64) volatile LONG m_stop = 0, m_framesInFlight = 0;
    bool m_running = false;
};

// Waits for an event (or the timeout) while dispatching messages sent to this thread's windows:
// the worker's DXGI calls may send window messages that the game thread must answer.
inline void CommandQueue::Wait(HANDLE event, DWORD timeout)
{
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &event, FALSE, timeout, QS_SENDMESSAGE);
        if (r != WAIT_OBJECT_0 + 1) return;
        MSG m;
        PeekMessageA(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}
