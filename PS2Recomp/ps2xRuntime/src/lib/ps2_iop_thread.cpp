#include "ps2_iop_thread.h"

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "ThreadNaming.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif

namespace ps2_iop_thread
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Idle time is run in chunks so an early wake stops the IOP thread within ~30 us. Multiple of the IOP's own
        // batch (IopEmulator::runEeCycles, 8192 EE cycles).
        constexpr uint64_t kChunkEeCycles = 8192u * 8u;

        thread_local bool t_onIopThread = false;

        PS2Runtime *g_runtime = nullptr;
        RunFn g_run = nullptr;
        std::thread g_thread;

        std::mutex g_mutex;
        std::condition_variable g_workCv;
        std::condition_variable g_parkedCv;
        bool g_stop = false;
        bool g_active = false; // an idle budget is being run (guarded by g_mutex)
        uint64_t g_budget = 0u;
        std::atomic<uint64_t> g_ran{0u};
        std::atomic<bool> g_stopRequest{false};
        std::atomic<uint32_t> g_gameWaiting{0u};

        // Game thread only.
        bool g_begun = false;
        uint64_t g_prepaid = 0u;

        struct DeferredCommand
        {
            uint32_t commandId = 0u;
            std::vector<uint8_t> packet;
        };
        std::mutex g_deferredMutex;
        std::vector<DeferredCommand> g_deferred;
        std::atomic<bool> g_hasDeferred{false};

        // Stats, printed every 5 s by the game thread.
        uint64_t g_offloaded = 0u, g_inline = 0u, g_endWaitNs = 0u;
        uint32_t g_idles = 0u, g_early = 0u;
        std::atomic<uint64_t> g_busyNs{0u};
        Clock::time_point g_statStart = Clock::now();

        std::atomic<bool> g_exited{false}; // set by threadLoop on its way out

        // Asks the thread to stop (also mid-budget: g_stopRequest is checked per chunk) and waits up to timeoutMs
        // for it, then joins; still busy after that, it is detached so an exit cannot hang on it.
        void stopThread(uint32_t timeoutMs)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stop = true;
                g_stopRequest.store(true, std::memory_order_release);
            }
            g_workCv.notify_all();
            if (!g_thread.joinable())
                return;
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
            while (!g_exited.load(std::memory_order_acquire) && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (g_exited.load(std::memory_order_acquire))
                g_thread.join();
            else
            {
                std::cerr << "[iopt] IOP thread still busy at exit: detached" << std::endl;
                g_thread.detach();
            }
        }

        // Defined after every object the thread touches, so it is destroyed (and the thread stopped) before them.
        struct ExitGuard
        {
            ~ExitGuard() { stopThread(2000u); }
        } g_exitGuard;

        void threadLoop()
        {
            t_onIopThread = true;
            ThreadNaming::SetCurrentThreadName("IopThread");
            std::unique_lock<std::mutex> lock(g_mutex);
            for (;;)
            {
                g_workCv.wait(lock, []
                              { return g_stop || g_active; });
                if (g_stop)
                {
                    g_exited.store(true, std::memory_order_release);
                    return;
                }
                const uint64_t budget = g_budget;
                lock.unlock();
                const auto t0 = Clock::now();
                uint64_t ran = 0u;
                while (ran < budget && !g_stopRequest.load(std::memory_order_acquire))
                {
                    // A game-thread call into the IOP (debug panel, shutdown) goes first.
                    while (g_gameWaiting.load(std::memory_order_acquire) != 0u)
                        _mm_pause();
                    const uint64_t chunk = std::min(kChunkEeCycles, budget - ran);
                    g_run(*g_runtime, chunk); // on this thread: runs the IOP under the call lock
                    ran += chunk;
                    g_ran.store(ran, std::memory_order_release);
                }
                g_busyNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()),
                                   std::memory_order_relaxed);
                lock.lock();
                g_active = false;
                g_parkedCv.notify_all();
            }
        }

        void printStats()
        {
            const auto now = Clock::now();
            const double secs = std::chrono::duration<double>(now - g_statStart).count();
            if (secs < 5.0)
                return;
            const double total = static_cast<double>(std::max<uint64_t>(1u, g_offloaded + g_inline));
            std::cout << "[iopt] 5s: offloaded%=" << 100.0 * static_cast<double>(g_offloaded) / total
                      << " iopThreadBusy%=" << static_cast<double>(g_busyNs.exchange(0u)) / (secs * 1e7)
                      << " idles/s=" << static_cast<double>(g_idles) / secs << " earlyWakes=" << g_early
                      << " endWaitMs=" << static_cast<double>(g_endWaitNs) / 1e6 << std::endl;
            g_offloaded = g_inline = g_endWaitNs = 0u;
            g_idles = g_early = 0u;
            g_statStart = now;
        }
    }

    bool enabled()
    {
        static const bool on = []
        {
            const char *env = std::getenv("PS2X_IOP_THREAD");
            const bool value = env != nullptr && *env != 0 && *env != '0';
            if (value)
                std::cout << "[iopt] IOP thread enabled (runs the IOP while the game thread sleeps)" << std::endl;
            return value;
        }();
        return on;
    }

    bool onIopThread()
    {
        return t_onIopThread;
    }

    void beginIdle(PS2Runtime &runtime, uint64_t budgetEeCycles, RunFn run)
    {
        // Only the time left after the prepaid part is new; tiny waits are not worth a hand-off.
        if (budgetEeCycles <= g_prepaid || budgetEeCycles - g_prepaid < kChunkEeCycles)
            return;
        const uint64_t budget = budgetEeCycles - g_prepaid;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_thread.joinable())
        {
            g_runtime = &runtime;
            g_run = run;
            g_stop = false;
            g_exited.store(false, std::memory_order_relaxed); // a restart after shutdown()
            g_thread = std::thread(threadLoop);
        }
        g_budget = budget;
        g_ran.store(0u, std::memory_order_relaxed);
        g_stopRequest.store(false, std::memory_order_relaxed);
        g_active = true;
        g_begun = true;
        g_workCv.notify_one();
    }

    void endIdle(PS2Runtime &runtime)
    {
        if (g_begun)
        {
            g_begun = false;
            const auto t0 = Clock::now();
            g_stopRequest.store(true, std::memory_order_release);
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_parkedCv.wait(lock, []
                                { return !g_active; });
            }
            const uint64_t ran = g_ran.load(std::memory_order_acquire);
            if (ran < g_budget)
                ++g_early;
            g_prepaid += ran;
            g_offloaded += ran;
            ++g_idles;
            g_endWaitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        }
        if (g_hasDeferred.load(std::memory_order_acquire))
            runDeferred(runtime);
        printStats();
    }

    uint64_t consumePrepaid(uint64_t eeCycles)
    {
        const uint64_t used = std::min(g_prepaid, eeCycles);
        g_prepaid -= used;
        g_inline += eeCycles - used;
        return eeCycles - used;
    }

    void runDeferred(PS2Runtime &runtime)
    {
        std::vector<DeferredCommand> commands;
        {
            std::lock_guard<std::mutex> lock(g_deferredMutex);
            commands.swap(g_deferred);
            g_hasDeferred.store(false, std::memory_order_relaxed);
        }
        uint8_t *const rdram = runtime.memory().getRDRAM();
        for (const DeferredCommand &c : commands)
            (void)ps2_stubs::dispatchSifCommand(rdram, &runtime, c.commandId, c.packet.data(), c.packet.size());
    }

    void deferSifCommand(uint32_t commandId, const void *packet, size_t packetSize)
    {
        DeferredCommand c;
        c.commandId = commandId;
        c.packet.assign(static_cast<const uint8_t *>(packet), static_cast<const uint8_t *>(packet) + packetSize);
        std::lock_guard<std::mutex> lock(g_deferredMutex);
        g_deferred.push_back(std::move(c));
        g_hasDeferred.store(true, std::memory_order_release);
    }

    void lockFromGame(std::unique_lock<std::recursive_mutex> &lock)
    {
        if (!enabled() || t_onIopThread)
        {
            lock.lock();
            return;
        }
        g_gameWaiting.fetch_add(1u, std::memory_order_acq_rel);
        lock.lock();
        g_gameWaiting.fetch_sub(1u, std::memory_order_acq_rel);
    }

    void shutdown()
    {
        stopThread(10000u);
    }
}
