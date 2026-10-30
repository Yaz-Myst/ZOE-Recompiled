#include "runtime/ps2_gs_worker.h"

#include "ThreadNaming.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "ps2_frame_log.h"
#include "ps2_host_settings.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif

namespace ps2_gs_worker
{
    std::atomic<uint32_t> g_pending{0u};

    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct PrivWrite
        {
            uint32_t address = 0u;
            uint64_t value = 0u;
            uint32_t size = 0u;
            bool frame = false; // counted in g_framesQueued
            bool flip = false;  // counted in g_flips when applied (presentation clock)
            bool movie = false; // ps2MovieActive() when the game issued this write (meaningful with flip)
        };

        struct Job
        {
            PS2Memory *mem = nullptr;
            std::vector<PS2Memory::PendingTransfer> gif;
            std::vector<PS2Memory::PendingTransfer> vif1;
            std::vector<PrivWrite> priv; // applied after the transfers
        };

        // Display flips queued and not yet applied; the game waits at kMaxFramesAhead.
        constexpr uint32_t kMaxFramesAhead = 2u;
        std::atomic<uint32_t> g_framesQueued{0u};
        uint64_t g_lastFrameTick = ~0ull; // game thread: one frame per vsync tick at most

        // 0 = game (or any other) thread, 1 = worker (stage 1: VIF1/VU1/GIF), 2 = draw thread (stage 2: GS).
        thread_local int t_stage = 0;

        // Stage 1 queue (jobs from the game thread).
        std::mutex g_mutex;
        std::condition_variable g_workCv;
        std::condition_variable g_idleCv;
        std::deque<Job> g_queue;
        bool g_stop = false;
        std::thread g_thread;

        // Stage 2 (PS2X_GS_THREAD=2): the GIF packet stream stage 1 produced, as segments of records.
        bool g_split = false;
        std::mutex g_mutex2;
        std::condition_variable g_workCv2;
        std::deque<std::vector<uint8_t>> g_queue2;
        bool g_stop2 = false;
        std::thread g_thread2;
        std::vector<uint8_t> g_segment; // stage 1 only: records of the job being run
        PS2Memory *g_segmentMem = nullptr;
        void (*g_drawFn)(void *, const uint8_t *, uint32_t) = nullptr;
        void *g_drawCtx = nullptr;

        enum RecordType : uint32_t
        {
            kRecGif = 0u,
            kRecPriv = 1u,
        };
        struct RecordHeader // 16 bytes, so every payload stays 16-byte aligned
        {
            uint32_t type;
            uint32_t size;
            uint32_t reserved[2];
        };
        static_assert(sizeof(RecordHeader) == 16, "record header");

        void appendRecord(uint32_t type, const void *payload, uint32_t size)
        {
            const size_t at = g_segment.size();
            const size_t padded = (static_cast<size_t>(size) + 15u) & ~static_cast<size_t>(15u);
            g_segment.resize(at + sizeof(RecordHeader) + padded);
            RecordHeader h{type, size, {0u, 0u}};
            std::memcpy(g_segment.data() + at, &h, sizeof(h));
            std::memcpy(g_segment.data() + at + sizeof(h), payload, size);
        }

        std::atomic<bool> g_workerExited{false}; // set by workerLoop / drawLoop on their way out
        std::atomic<bool> g_drawExited{false};

        // Stats, printed every 5 s by the game thread.
        std::atomic<uint64_t> g_busyNs{0u};
        std::atomic<uint64_t> g_busyNs2{0u};
        std::atomic<uint64_t> g_jobs{0u};
        uint64_t g_waitNs[static_cast<size_t>(Sync::Count)] = {};
        uint32_t g_waits[static_cast<size_t>(Sync::Count)] = {};
        uint32_t g_maxQueue = 0u;
        std::atomic<uint32_t> g_maxQueue2{0u};
        // GS privileged register accesses that waited: key = offset | write << 16; value bits OR-ed for writes.
        std::map<uint32_t, std::pair<uint32_t, uint64_t>> g_privWaits;
        Clock::time_point g_statStart = Clock::now();

        void jobFinished()
        {
            // Release: everything written is visible to a game thread that sees the count drop.
            if (g_pending.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_idleCv.notify_all();
            }
        }

        // Presentation clock (see flipCount in the header).
        std::mutex g_flipMutex;
        std::condition_variable g_flipCv;
        std::atomic<uint64_t> g_flips{0};
        std::atomic<bool> g_flipMovie{false};
        uint64_t g_lastFlipNoticeTick = ~0ull; // game thread only

        bool claimFlip(PS2Memory &mem, uint32_t address)
        {
            const uint32_t off = (address - PS2_GS_PRIV_REG_BASE) & 0xFFF8u;
            if (off != 0x70u && off != 0x90u) // DISPFB1 / DISPFB2
                return false;
            const uint64_t tick = mem.gs().vsyncTick.load(std::memory_order_relaxed);
            if (tick == g_lastFlipNoticeTick)
                return false;
            g_lastFlipNoticeTick = tick;
            return true;
        }

        void noteFlip()
        {
            {
                std::lock_guard<std::mutex> lock(g_flipMutex);
                g_flips.fetch_add(1u, std::memory_order_acq_rel);
            }
            g_flipCv.notify_all();
        }

        // Budget and clock bookkeeping of a queued display-register write; thread-safe (atomics and the mutex only).
        void notePrivWrite(const PrivWrite &w)
        {
            if (w.flip)
            {
                g_flipMovie.store(w.movie, std::memory_order_release); // before the count: a reader that sees the flip sees it
                noteFlip();
            }
            if (w.frame)
            {
                g_framesQueued.fetch_sub(1u, std::memory_order_acq_rel);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_idleCv.notify_all();
            }
        }

        // Worker thread (PS2X_GS_THREAD=1) or draw thread (=2): the register write, then the bookkeeping.
        void applyPrivWrite(PS2Memory &mem, const PrivWrite &w)
        {
            ps2ApplyGsPrivWrite(mem, w.address, w.value, w.size);
            notePrivWrite(w);
        }

        void runJob(Job &job)
        {
            PS2Memory &mem = *job.mem;
            try
            {
                // Same order as PS2Memory::processPendingTransfers: PATH3 first, then VIF1, then the arbiter.
                ps2ProcessGifTransfers(mem, job.gif);
                ps2ProcessVif1Transfers(mem, job.vif1);
                if (mem.m_gifArbiter)
                    mem.m_gifArbiter->drain();
            }
            catch (const std::exception &e)
            {
                std::cerr << "[gsw] job failed: " << e.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[gsw] job failed: unknown exception" << std::endl;
            }
            // The queued display-register writes are applied even when a transfer threw: each one with w.frame holds a
            // slot of the frame-ahead budget (g_framesQueued) and each w.flip advances the presentation clock, so
            // skipping them would leave the game thread waiting in deferGsPrivWrite forever after two failed jobs.
            for (const PrivWrite &w : job.priv)
            {
                try
                {
                    if (g_split)
                        appendRecord(kRecPriv, &w, sizeof(w)); // after this job's packets
                    else
                        applyPrivWrite(mem, w);
                }
                catch (const std::exception &e) // appendRecord could not grow the segment: the register value is lost,
                {                               // the budget slot and the flip count are not (the draw thread owns gs_regs)
                    std::cerr << "[gsw] display write dropped: " << e.what() << std::endl;
                    if (g_split)
                        notePrivWrite(w);
                }
            }
            if (g_split && !g_segment.empty())
            {
                g_segmentMem = &mem;
                g_pending.fetch_add(1u, std::memory_order_acq_rel); // before this job counts as done
                size_t depth = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_mutex2);
                    g_queue2.push_back(std::move(g_segment));
                    depth = g_queue2.size();
                }
                g_segment = {};
                g_segment.reserve(64u * 1024u);
                g_workCv2.notify_one();
                uint32_t prev = g_maxQueue2.load(std::memory_order_relaxed);
                while (depth > prev && !g_maxQueue2.compare_exchange_weak(prev, static_cast<uint32_t>(depth)))
                {
                }
            }
        }

        void workerLoop()
        {
            t_stage = 1;
            ThreadNaming::SetCurrentThreadName("GsWorker");
            g_segment.reserve(64u * 1024u);
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_workCv.wait(lock, []
                                  { return g_stop || !g_queue.empty(); });
                    if (g_queue.empty())
                    {
                        g_workerExited.store(true, std::memory_order_release);
                        return; // stopping
                    }
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                const auto t0 = Clock::now();
                runJob(job);
                {
                    const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
                    g_busyNs.fetch_add(ns, std::memory_order_relaxed);
                    ps2_frame_log::counters().gsWorkerNs.fetch_add(ns, std::memory_order_relaxed);
                }
                g_jobs.fetch_add(1u, std::memory_order_relaxed);
                jobFinished();
            }
        }

        void drawLoop()
        {
            t_stage = 2;
            ThreadNaming::SetCurrentThreadName("GsDraw");
            for (;;)
            {
                std::vector<uint8_t> segment;
                {
                    std::unique_lock<std::mutex> lock(g_mutex2);
                    g_workCv2.wait(lock, []
                                   { return g_stop2 || !g_queue2.empty(); });
                    if (g_queue2.empty())
                    {
                        g_drawExited.store(true, std::memory_order_release);
                        return; // stopping
                    }
                    segment = std::move(g_queue2.front());
                    g_queue2.pop_front();
                }
                const auto t0 = Clock::now();
                // One try per GIF record, not per segment: a throw while drawing must not skip the kRecPriv records
                // that follow it, since each w.frame holds a slot of the frame-ahead budget (see runJob).
                bool failed = false;
                size_t at = 0u;
                while (at + sizeof(RecordHeader) <= segment.size())
                {
                    RecordHeader h;
                    std::memcpy(&h, segment.data() + at, sizeof(h));
                    const uint8_t *payload = segment.data() + at + sizeof(h);
                    if (h.type == kRecGif)
                    {
                        try
                        {
                            g_drawFn(g_drawCtx, payload, h.size);
                        }
                        catch (const std::exception &e)
                        {
                            if (!failed)
                                std::cerr << "[gsw] draw segment failed: " << e.what() << std::endl;
                            failed = true;
                        }
                        catch (...)
                        {
                            if (!failed)
                                std::cerr << "[gsw] draw segment failed: unknown exception" << std::endl;
                            failed = true;
                        }
                    }
                    else if (h.type == kRecPriv)
                    {
                        PrivWrite w;
                        std::memcpy(&w, payload, sizeof(w));
                        applyPrivWrite(*g_segmentMem, w);
                    }
                    at += sizeof(RecordHeader) + ((static_cast<size_t>(h.size) + 15u) & ~static_cast<size_t>(15u));
                }
                {
                    const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
                    g_busyNs2.fetch_add(ns, std::memory_order_relaxed);
                    ps2_frame_log::counters().gsDrawNs.fetch_add(ns, std::memory_order_relaxed);
                }
                jobFinished();
            }
        }

        void printStats()
        {
            const auto now = Clock::now();
            const double secs = std::chrono::duration<double>(now - g_statStart).count();
            if (secs < 5.0)
                return;
            static const char *const kNames[] = {"gsPriv", "vu1Mem", "vifGifReg", "fifo", "gsDirect", "frameLimit"};
            uint64_t totalWaitNs = 0u;
            uint32_t totalWaits = 0u;
            for (size_t i = 0; i < static_cast<size_t>(Sync::Count); ++i)
            {
                totalWaitNs += g_waitNs[i];
                totalWaits += g_waits[i];
            }
            std::cout << "[gsw] 5s: jobs/s=" << static_cast<double>(g_jobs.exchange(0u)) / secs
                      << " workerBusy%=" << static_cast<double>(g_busyNs.exchange(0u)) / (secs * 1e7);
            if (g_split)
                std::cout << " drawBusy%=" << static_cast<double>(g_busyNs2.exchange(0u)) / (secs * 1e7)
                          << " maxDrawQueue=" << g_maxQueue2.exchange(0u);
            std::cout << " gameWait%=" << static_cast<double>(totalWaitNs) / (secs * 1e7)
                      << " waits/s=" << static_cast<double>(totalWaits) / secs << " maxQueue=" << g_maxQueue << " by:";
            for (size_t i = 0; i < static_cast<size_t>(Sync::Count); ++i)
            {
                if (g_waits[i] != 0u)
                    std::cout << " " << kNames[i] << "=" << g_waits[i] << "/" << static_cast<double>(g_waitNs[i]) / 1e6 << "ms";
                g_waits[i] = 0u;
                g_waitNs[i] = 0u;
            }
            if (!g_privWaits.empty())
            {
                std::cout << " regs:" << std::hex;
                for (const auto &kv : g_privWaits)
                {
                    const bool write = (kv.first >> 16) != 0u;
                    std::cout << " " << (write ? "w" : "r") << "0x" << (kv.first & 0xFFFFu) << "=" << std::dec << kv.second.first;
                    if (write)
                        std::cout << "(bits 0x" << std::hex << kv.second.second << ")";
                    std::cout << std::hex;
                }
                std::cout << std::dec;
                g_privWaits.clear();
            }
            std::cout << std::endl;
            g_maxQueue = 0u;
            g_statStart = now;
        }

        // Asks both threads to stop and waits up to timeoutMs for them, then joins; a thread still busy after that
        // is detached (an exit must not hang on a stuck draw). Used by shutdown() and at static destruction.
        void stopThreads(uint32_t timeoutMs)
        {
            auto settle = [&](std::thread &t, std::atomic<bool> &exited, const char *what)
            {
                if (!t.joinable())
                    return;
                const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs); // per stage
                while (!exited.load(std::memory_order_acquire) && Clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (exited.load(std::memory_order_acquire))
                    t.join();
                else
                {
                    std::cerr << "[gsw] " << what << " still busy at exit: detached" << std::endl;
                    t.detach();
                }
            };
            // Stage 1 first: the worker drains its queue and hands its last segments to the draw thread, which must
            // still be running to take them (each segment carries a g_pending count only drawLoop releases).
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stop = true;
            }
            g_workCv.notify_all();
            settle(g_thread, g_workerExited, "render worker");
            {
                std::lock_guard<std::mutex> lock(g_mutex2);
                g_stop2 = true;
            }
            g_workCv2.notify_all();
            settle(g_thread2, g_drawExited, "draw thread");
        }

        // Defined after every object the threads touch, so it is destroyed (and the threads stopped) before them.
        struct ExitGuard
        {
            ~ExitGuard() { stopThreads(2000u); }
        } g_exitGuard;
    }

    bool enabled()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_GS_THREAD");
            if (env == nullptr || *env == 0 || *env == '0')
                return false;
            // VU capture and verification compare against single-threaded order; keep them on one thread.
            if (std::getenv("PS2X_VU_CAPTURE") != nullptr || std::getenv("PS2X_VU_VERIFY") != nullptr)
            {
                std::cout << "[gsw] disabled: VU capture/verify active" << std::endl;
                return false;
            }
            g_split = *env == '2';
            std::cout << "[gsw] render worker thread enabled" << (g_split ? " (+ GS draw thread)" : "") << std::endl;
            return true;
        }();
        return on;
    }

    bool onWorker()
    {
        return t_stage != 0;
    }

    void setDrawTarget(void (*fn)(void *ctx, const uint8_t *data, uint32_t size), void *ctx)
    {
        g_drawFn = fn;
        g_drawCtx = ctx;
    }

    bool captureGifPacket(const uint8_t *data, uint32_t size)
    {
        if (t_stage != 1 || !g_split || g_drawFn == nullptr)
            return false;
        appendRecord(kRecGif, data, size);
        return true;
    }

    void submit(PS2Memory &mem, std::vector<PS2Memory::PendingTransfer> &&gif, std::vector<PS2Memory::PendingTransfer> &&vif1)
    {
        Job job;
        job.mem = &mem;
        job.gif = std::move(gif);
        job.vif1 = std::move(vif1);
        g_pending.fetch_add(1u, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_thread.joinable())
            {
                g_stop = false;
                g_workerExited.store(false, std::memory_order_relaxed); // a restart after shutdown()
                g_thread = std::thread(workerLoop);
                if (g_split)
                {
                    g_stop2 = false;
                    g_drawExited.store(false, std::memory_order_relaxed);
                    g_thread2 = std::thread(drawLoop);
                }
            }
            g_queue.push_back(std::move(job));
            g_maxQueue = std::max<uint32_t>(g_maxQueue, static_cast<uint32_t>(g_queue.size()));
        }
        g_workCv.notify_one();
        printStats();
    }

    void drainSlow(Sync why)
    {
        if (t_stage != 0)
            return;
        const auto t0 = Clock::now();
        // Most waits are short: spin briefly before sleeping.
        for (uint32_t i = 0; i < 4096u && g_pending.load(std::memory_order_acquire) != 0u; ++i)
            _mm_pause();
        if (g_pending.load(std::memory_order_acquire) != 0u)
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_idleCv.wait(lock, []
                          { return g_pending.load(std::memory_order_acquire) == 0u; });
        }
        const size_t idx = static_cast<size_t>(why);
        {
                    const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
                    g_waitNs[idx] += ns;
                    ps2_frame_log::counters().gameWaitNs.fetch_add(ns, std::memory_order_relaxed);
                }
        ++g_waits[idx];
    }

    bool deferGsPrivWrite(PS2Memory &mem, uint32_t address, uint64_t value, uint32_t size)
    {
        if (t_stage != 0 || g_pending.load(std::memory_order_acquire) == 0u)
            return false; // nothing in flight: apply now
        PrivWrite w;
        w.address = address;
        w.value = value;
        w.size = size;
        w.flip = claimFlip(mem, address);
        w.movie = ps2MovieActive();
        const uint32_t off = (address - PS2_GS_PRIV_REG_BASE) & 0xFFF8u;
        const uint64_t tick = mem.gs().vsyncTick.load(std::memory_order_relaxed);
        if ((off == 0x70u || off == 0x90u) && tick != g_lastFrameTick) // DISPFB1 / DISPFB2: a flip
        {
            g_lastFrameTick = tick;
            w.frame = true;
            if (g_framesQueued.load(std::memory_order_acquire) >= kMaxFramesAhead)
            {
                const auto t0 = Clock::now();
                std::unique_lock<std::mutex> lock(g_mutex);
                g_idleCv.wait(lock, []
                              { return g_framesQueued.load(std::memory_order_acquire) < kMaxFramesAhead; });
                const size_t idx = static_cast<size_t>(Sync::FrameLimit);
                {
                    const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
                    g_waitNs[idx] += ns;
                    ps2_frame_log::counters().gameWaitNs.fetch_add(ns, std::memory_order_relaxed);
                }
                ++g_waits[idx];
            }
            g_framesQueued.fetch_add(1u, std::memory_order_acq_rel);
        }
        Job job;
        job.mem = &mem;
        job.priv.push_back(w);
        g_pending.fetch_add(1u, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push_back(std::move(job));
        }
        g_workCv.notify_one();
        return true;
    }

    void noteDirectGsPrivWrite(PS2Memory &mem, uint32_t address)
    {
        if (t_stage == 0 && claimFlip(mem, address))
        {
            g_flipMovie.store(ps2MovieActive(), std::memory_order_release);
            noteFlip();
        }
    }

    bool flipMovie()
    {
        return g_flipMovie.load(std::memory_order_acquire);
    }

    uint64_t flipCount()
    {
        return g_flips.load(std::memory_order_acquire);
    }

    uint64_t waitForFlip(uint64_t seen, uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(g_flipMutex);
        g_flipCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [seen] { return g_flips.load(std::memory_order_acquire) != seen; });
        return g_flips.load(std::memory_order_acquire);
    }

    void noteGsPrivWait(uint32_t address, bool write, uint64_t value)
    {
        if (t_stage != 0)
            return;
        auto &e = g_privWaits[((address - PS2_GS_PRIV_REG_BASE) & 0xFFF8u) | (write ? 0x10000u : 0u)];
        ++e.first;
        e.second |= value;
    }

    void shutdown()
    {
        stopThreads(10000u); // the queued jobs are drained first; a normal shutdown finishes in milliseconds
    }
}
