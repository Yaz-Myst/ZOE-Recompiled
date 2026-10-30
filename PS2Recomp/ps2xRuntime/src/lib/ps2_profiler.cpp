// Minimal in-process sampling profiler for bring-up (Windows only).
// profilerToggle() starts a sampler thread that periodically suspends every other thread in the
// process and records its instruction pointer; the second call stops it and writes aggregated
// samples (module-relative RVAs per thread) to <dir>/profile.txt. Resolve names offline with the
// linker map file.
#include "ps2_profiler.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#include <tlhelp32.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Sample
    {
        DWORD tid;
        uint64_t rip;
    };

    std::atomic<bool> g_running{false};
    std::thread g_sampler;
    std::vector<Sample> g_samples;

    void samplerLoop()
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        std::vector<DWORD> tids;
        std::vector<Sample> batch;
        tids.reserve(256);
        batch.reserve(256);
        g_samples.reserve(1u << 20);
        timeBeginPeriod(1);

        uint32_t iteration = 0;
        while (g_running.load(std::memory_order_acquire))
        {
            // Re-enumerate threads only every ~250 iterations: a toolhelp snapshot per sample limited the rate to ~15 Hz.
            if ((iteration++ % 250u) != 0u)
                goto sample;
            tids.clear();
            {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                if (Thread32First(snap, &te))
                {
                    do
                    {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                            tids.push_back(te.th32ThreadID);
                    } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
            }
            }

        sample:
            batch.clear();
            for (DWORD tid : tids)
            {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
                if (!h)
                    continue;
                // No allocation between suspend and resume: a suspended thread may hold the heap lock.
                if (SuspendThread(h) != static_cast<DWORD>(-1))
                {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &ctx) && batch.size() < batch.capacity())
                        batch.push_back({tid, ctx.Rip});
                    ResumeThread(h);
                }
                CloseHandle(h);
            }
            g_samples.insert(g_samples.end(), batch.begin(), batch.end());
            Sleep(1);
        }
        timeEndPeriod(1);
    }

    std::string moduleNameFor(uint64_t address)
    {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address), &mod))
            return "?";
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        std::string name(path);
        const size_t slash = name.find_last_of("\\/");
        return slash == std::string::npos ? name : name.substr(slash + 1);
    }

    void writeReport(const std::string &dir)
    {
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::map<std::pair<DWORD, uint64_t>, uint64_t> counts;
        std::map<DWORD, uint64_t> perThread;
        for (const Sample &s : g_samples)
        {
            counts[{s.tid, s.rip}]++;
            perThread[s.tid]++;
        }
        std::ofstream out(dir + "/profile.txt");
        out << "# module_base 0x" << std::hex << base << std::dec << " samples " << g_samples.size() << "\n";
        for (const auto &[tid, n] : perThread)
            out << "# thread " << tid << " samples " << n << "\n";
        for (const auto &[key, n] : counts)
        {
            const uint64_t rip = key.second;
            const bool inModule = rip >= base && rip - base < 0x40000000ull;
            out << key.first << " " << (inModule ? "rva" : "abs") << " 0x" << std::hex << (inModule ? rip - base : rip) << std::dec << " " << n;
            if (!inModule)
                out << " " << moduleNameFor(rip);
            out << "\n";
        }
    }
}

bool ps2ProfilerToggle(const char *outputDir)
{
    if (!g_running.load())
    {
        g_samples.clear();
        g_running.store(true, std::memory_order_release);
        g_sampler = std::thread(samplerLoop);
        return true;
    }
    g_running.store(false, std::memory_order_release);
    if (g_sampler.joinable())
        g_sampler.join();
    writeReport(outputDir != nullptr ? outputDir : ".");
    return false;
}

namespace
{
    LONG WINAPI crashFilter(EXCEPTION_POINTERS *info)
    {
        const EXCEPTION_RECORD *rec = info->ExceptionRecord;
        const uint64_t rip = reinterpret_cast<uint64_t>(rec->ExceptionAddress);
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::fprintf(stderr, "[crash] exception 0x%08lx at rva 0x%llx (rip 0x%llx) thread %lu",
                     rec->ExceptionCode, static_cast<unsigned long long>(rip - base),
                     static_cast<unsigned long long>(rip), GetCurrentThreadId());
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            std::fprintf(stderr, " %s 0x%llx", rec->ExceptionInformation[0] ? "writing" : "reading",
                         static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        std::fputc('\n', stderr);

        // Walk the faulting thread's stack from the exception context; print each frame as
        // <module base>+rva so exe frames can be resolved with the linker map.
        CONTEXT ctx = *info->ContextRecord;
        for (int frame = 0; frame < 24 && ctx.Rip != 0; ++frame)
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(ctx.Rip), &module);
            char name[MAX_PATH] = "?";
            if (module)
                GetModuleFileNameA(module, name, MAX_PATH);
            const char *shortName = std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name;
            std::fprintf(stderr, "[crash]   #%d %s+0x%llx\n", frame, shortName,
                         static_cast<unsigned long long>(ctx.Rip - reinterpret_cast<uint64_t>(module)));

            DWORD64 imageBase = 0;
            RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn)
            {
                // Leaf function: the return address is at the top of the stack.
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void *handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void ps2InstallCrashReporter()
{
    SetUnhandledExceptionFilter(crashFilter);
}

namespace
{
    // Hardware write watchpoints (x64 debug registers DR0-DR3) on up to four guest words.
    uint64_t g_watchHost[4]{};
    uint32_t g_watchGuest[4]{};
    uint32_t g_watchCount = 0;
    std::atomic<uint32_t> g_watchHits[4]{};
    std::atomic<uint32_t> g_watchBad[4]{};
    constexpr uint32_t kWatchMaxLogs = 24;
    uint32_t g_watchFirst = 6; // PS2X_WATCH_FIRST: hits always logged per address before only suspicious ones are

    LONG WINAPI watchHandler(EXCEPTION_POINTERS *info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT *c = info->ContextRecord;
        const uint64_t dr6 = c->Dr6;
        if ((dr6 & 0xFu) == 0u)
            return EXCEPTION_CONTINUE_SEARCH;
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        for (uint32_t i = 0; i < g_watchCount; ++i)
        {
            if ((dr6 & (1ull << i)) == 0u)
                continue;
            uint32_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void *>(g_watchHost[i]), sizeof(value));
            float asFloat = 0.0f;
            std::memcpy(&asFloat, &value, sizeof(asFloat));
            // First few hits always; after that only values that look broken as floats (NaN/inf, huge, or tiny non-zero).
            const float mag = asFloat < 0.0f ? -asFloat : asFloat;
            const bool suspicious = !(mag == mag) || mag > 1e7f || (mag != 0.0f && mag < 1e-20f);
            const uint32_t hit = g_watchHits[i].fetch_add(1);
            if (hit >= g_watchFirst && !(suspicious && g_watchBad[i].fetch_add(1) < kWatchMaxLogs))
                continue;
            std::fprintf(stderr, "[watch] guest 0x%08x = 0x%08x (%g) thread %lu rva", g_watchGuest[i], value, asFloat,
                         GetCurrentThreadId());
            // Three host frames: the writing generated function and its two callers.
            CONTEXT walk = *c;
            for (int frame = 0; frame < 3 && walk.Rip != 0; ++frame)
            {
                std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(walk.Rip - base));
                DWORD64 imageBase = 0;
                RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    walk.Rip = *reinterpret_cast<DWORD64 *>(walk.Rsp);
                    walk.Rsp += 8;
                    continue;
                }
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
            }
            std::fputc('\n', stderr);
        }
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void armThread(DWORD tid)
    {
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!h)
            return;
        if (SuspendThread(h) != static_cast<DWORD>(-1))
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &ctx))
            {
                uint64_t dr7 = 0;
                uint64_t *regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
                for (uint32_t i = 0; i < g_watchCount; ++i)
                {
                    *regs[i] = g_watchHost[i];
                    dr7 |= 1ull << (i * 2);          // local enable
                    dr7 |= 1ull << (16 + i * 4);     // R/W = 01: data writes
                    dr7 |= 3ull << (18 + i * 4);     // LEN = 11: 4 bytes
                }
                ctx.Dr7 = dr7;
                SetThreadContext(h, &ctx);
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
}

namespace
{
    // PS2X_FPTRAP: the PS2's FPU and VU never produce infinity or NaN (they clamp to +/-max), the PC does. With the
    // overflow and invalid-operation exceptions unmasked on the EE thread, the first instruction that makes one traps
    // here: log where (host RVAs of the generated function and its callers), mask, and carry on.
    std::atomic<uint32_t> g_fpTrapLogs{0};
    constexpr uint32_t kFpTrapMaxLogs = 60;

    LONG WINAPI fpTrapHandler(EXCEPTION_POINTERS *info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != STATUS_FLOAT_MULTIPLE_TRAPS && code != STATUS_FLOAT_MULTIPLE_FAULTS && code != STATUS_FLOAT_OVERFLOW &&
            code != STATUS_FLOAT_INVALID_OPERATION)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT *c = info->ContextRecord;
        if (g_fpTrapLogs.fetch_add(1) < kFpTrapMaxLogs)
        {
            const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            std::fprintf(stderr, "[fptrap] code 0x%08lx mxcsr 0x%04lx thread %lu rva", code, static_cast<unsigned long>(c->MxCsr),
                         GetCurrentThreadId());
            CONTEXT walk = *c;
            for (int frame = 0; frame < 4 && walk.Rip != 0; ++frame)
            {
                std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(walk.Rip - base));
                DWORD64 imageBase = 0;
                RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    walk.Rip = *reinterpret_cast<DWORD64 *>(walk.Rsp);
                    walk.Rsp += 8;
                    continue;
                }
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
            }
            std::fputc('\n', stderr);
        }
        c->MxCsr = (c->MxCsr | 0x1F80u) & ~0x3Fu; // mask everything and clear the flags; re-armed later
        c->FltSave.MxCsr = c->MxCsr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

void ps2FpTrapArm()
{
    static std::atomic<bool> s_installed{false};
    if (!s_installed.exchange(true))
        AddVectoredExceptionHandler(1, fpTrapHandler);
    // unmask invalid operation (IM, bit 7) and overflow (OM, bit 10); clear sticky flags first
    _mm_setcsr((_mm_getcsr() & ~0x3Fu) & ~(0x0080u | 0x0400u));
}

void ps2WatchArm(uint8_t *rdram, const uint32_t *guestAddrs, uint32_t count)
{
    static bool s_handlerInstalled = false;
    if (!s_handlerInstalled)
    {
        AddVectoredExceptionHandler(1, watchHandler);
        s_handlerInstalled = true;
    }
    g_watchCount = count > 4u ? 4u : count;
    if (const char *first = std::getenv("PS2X_WATCH_FIRST"))
        g_watchFirst = static_cast<uint32_t>(std::strtoul(first, nullptr, 10));
    for (uint32_t i = 0; i < g_watchCount; ++i)
    {
        g_watchGuest[i] = guestAddrs[i];
        g_watchHost[i] = reinterpret_cast<uint64_t>(rdram + ((guestAddrs[i] & 0x01FFFFFCu)));
    }
    // Arm from a helper thread so the calling thread gets the registers too.
    std::thread([]
                {
                    const DWORD self = GetCurrentThreadId();
                    const DWORD pid = GetCurrentProcessId();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap == INVALID_HANDLE_VALUE)
                        return;
                    THREADENTRY32 te{};
                    te.dwSize = sizeof(te);
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                    {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                            armThread(te.th32ThreadID);
                    }
                    CloseHandle(snap);
                })
        .join();
}
#else
void ps2FpTrapArm() {}
void ps2WatchArm(uint8_t *, const uint32_t *, uint32_t)
{
}
bool ps2ProfilerToggle(const char *)
{
    return false;
}

void ps2InstallCrashReporter()
{
}
#endif
