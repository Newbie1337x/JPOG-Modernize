// csmt_queue.cpp - the ring between the game thread and the render thread (csmt.h).
#include "csmt.h"

bool CommandQueue::Start()
{
    if (m_running) return true;
    m_ring = (BYTE*)VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    m_workEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    m_spaceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    m_syncEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    m_frameEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_ring || !m_workEvent || !m_spaceEvent || !m_syncEvent || !m_frameEvent) { Stop(); return false; }
    m_written = m_consumed = m_reserved = 0;
    m_limit = (uint32_t)kBytes;
    m_stop = 0;
    m_thread = CreateThread(nullptr, 0, ThreadMain, this, 0, &m_workerId);
    if (!m_thread) { Stop(); return false; }
    SetThreadPriority(m_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    m_running = true;
    return true;
}

void CommandQueue::Stop()
{
    if (m_running && !OnWorker()) {
        Drain();
        InterlockedExchange(&m_stop, 1);
        SetEvent(m_workEvent);
        WaitForSingleObject(m_thread, INFINITE);
    }
    m_running = false;
    if (m_thread) { CloseHandle(m_thread); m_thread = nullptr; }
    for (HANDLE* h : { &m_workEvent, &m_spaceEvent, &m_syncEvent, &m_frameEvent })
        if (*h) { CloseHandle(*h); *h = nullptr; }
    if (m_ring) { VirtualFree(m_ring, 0, MEM_RELEASE); m_ring = nullptr; }
    m_workerId = 0;
}

void* CommandQueue::Reserve(size_t bytes)
{
    auto waitFree = [this](uint32_t need) {
        if (m_limit - m_reserved >= need) return; // room known from the last look at the worker
        for (int spin = 0;; ++spin) {
            m_limit = m_consumed + (uint32_t)kBytes;
            if (m_limit - m_reserved >= need) return;
            if (spin < 256) { YieldProcessor(); continue; }
            InterlockedExchange(&m_producerWaiting, 1);
            m_limit = m_consumed + (uint32_t)kBytes;
            if (m_limit - m_reserved < need) {
                WakeWorker();
                Wait(m_spaceEvent, 1); // bounded: the worker's wake-up check is not fenced
            }
            InterlockedExchange(&m_producerWaiting, 0);
        }
    };
    uint32_t pos = m_reserved & kMask;
    if (pos + bytes > kBytes) {
        // Not enough room before the end: a skip marker fills it and the command starts at 0.
        const uint32_t pad = (uint32_t)kBytes - pos;
        waitFree(pad);
        Header* skip = (Header*)(m_ring + pos);
        skip->size = pad;
        skip->run = nullptr;
        m_reserved += pad;
        m_written = m_reserved;
        pos = 0;
    }
    waitFree((uint32_t)bytes);
    return m_ring + pos;
}

void CommandQueue::FramePosted()
{
    InterlockedIncrement(&m_framesInFlight);
    while (InterlockedCompareExchange(&m_framesInFlight, 0, 0) > 1) {
        WakeWorker();
        Wait(m_frameEvent, INFINITE);
    }
}

void CommandQueue::FrameDone()
{
    InterlockedDecrement(&m_framesInFlight);
    SetEvent(m_frameEvent);
}

DWORD WINAPI CommandQueue::ThreadMain(void* self)
{
    ((CommandQueue*)self)->Run();
    return 0;
}

void CommandQueue::Run()
{
    uint32_t consumed = m_consumed;
    for (;;) {
        uint32_t written = m_written;
        if (written == consumed) {
            if (m_stop) return;
            // Brief spin: the game usually records the next call within a few microseconds.
            for (int spin = 0; spin < 4000 && written == consumed; ++spin) {
                YieldProcessor();
                written = m_written;
            }
            if (written == consumed) {
                InterlockedExchange(&m_workerSleeping, 1);
                if (m_written == consumed && !m_stop)
                    WaitForSingleObject(m_workEvent, INFINITE);
                InterlockedExchange(&m_workerSleeping, 0);
                continue;
            }
        }
        while (consumed != written) {
            Header* h = (Header*)(m_ring + (consumed & kMask));
            const uint32_t size = h->size;
            if (h->run) h->run(h + 1);
            consumed += size;
            m_consumed = consumed;
            if (m_producerWaiting) SetEvent(m_spaceEvent);
        }
    }
}
