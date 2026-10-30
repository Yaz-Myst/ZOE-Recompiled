// Save states - see ps2_savestate.h.
#include "ps2_savestate.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_gs_worker.h"
#include "runtime/ps2_vu_fast.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

namespace ps2_stubs
{
    std::function<void(R5900Context &)> makeCdStReadCompletionForState(uint8_t *rdram, PS2Runtime *runtime, const uint32_t *args);
}

namespace ps2_state
{
    namespace
    {
        // Rebuilds a wait/resume closure from its save-state tag (see EeCompletionTag).
        std::function<void(R5900Context &)> makeCompletion(const EeCompletionTag &tag, PS2Runtime &rt)
        {
            switch (tag.kind)
            {
            case EeCompletionTag::SetPc:
            {
                const uint32_t pc = tag.args[0];
                return [pc](R5900Context &context) { context.pc = pc; };
            }
            case EeCompletionTag::CdStRead:
                return ps2_stubs::makeCdStReadCompletionForState(rt.memory().getRDRAM(), &rt, tag.args);
            default:
                return {};
            }
        }

        // Requests from the window thread (hotkeys), serviced on the EE executor at a safe point.
        enum class Op : int
        {
            None = 0,
            Save = 1,
            Load = 2,
        };
        std::atomic<int> g_op{0};
        std::atomic<int> g_slot{0};
        std::mutex g_pathMutex;
        std::string g_explicitPath; // overrides the slot path when set (PS2X_STATE_SAVE_AT / PS2X_STATE_LOAD)
        std::chrono::steady_clock::time_point g_requestTime{};
        std::string g_lastMessage;
        PS2Runtime *g_runtimeForWake = nullptr;
        std::string g_gameName;
        constexpr uint32_t kWakeType = 0x45564153u; // 'SAVE': no guest thread waits on this external type

        // Sizes of the structures copied as raw bytes; a state written by a build with other layouts is refused.
        struct LayoutCheck
        {
            uint32_t context = sizeof(R5900Context);
            uint32_t vu1State = sizeof(VU1State);
            uint32_t gsContext = sizeof(GSContext);
            uint32_t vif = sizeof(VIFRegisters);
            uint32_t dma = sizeof(DMARegisters);
        };

        struct Header
        {
            uint32_t magic = kMagic;
            uint32_t version = kFormatVersion;
            LayoutCheck layout{};
            uint64_t vsyncTick = 0;
            uint64_t savedAtUnix = 0;
        };

        std::vector<Part> &partRegistry()
        {
            static std::vector<Part> parts;
            return parts;
        }

        std::vector<Part> sortedParts()
        {
            std::vector<Part> list = partRegistry();
            std::sort(list.begin(), list.end(), [](const Part &a, const Part &b) { return std::strcmp(a.name, b.name) < 0; });
            return list;
        }
    }

    bool registerPart(const Part &part)
    {
        partRegistry().push_back(part);
        return true;
    }

    PS2Runtime *runtime() { return g_runtimeForWake; }

    struct Access
    {
        // ---- EE memory: RAM, scratchpad, VU memories, GS VRAM, I/O registers, DMA/VIF/GIF/timers -----------------------
        static void saveMemory(Writer &w, PS2Memory &m)
        {
            w.beginSection(tag("EEMM"), 1u);
            w.blob(m.m_rdram, PS2_RAM_SIZE);
            w.blob(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
            w.blob(m.m_vu0Code, PS2_VU0_CODE_SIZE);
            w.blob(m.m_vu0Data, PS2_VU0_DATA_SIZE);
            w.blob(m.m_vu1Code, PS2_VU1_CODE_SIZE);
            w.blob(m.m_vu1Data, PS2_VU1_DATA_SIZE);
            w.blob(m.m_gsVRAM, PS2_GS_VRAM_SIZE);
            w.u64(m.m_ioRegisters.size());
            for (const auto &[addr, value] : m.m_ioRegisters)
            {
                w.u32(addr);
                w.u32(value);
            }
            const GSRegisters &g = m.gs_regs;
            for (uint64_t v : {g.pmode, g.smode1, g.smode2, g.srfsh, g.synch1, g.synch2, g.syncv, g.dispfb1, g.display1, g.dispfb2,
                               g.display2, g.extbuf, g.extdata, g.extwrite, g.bgcolor})
                w.u64(v);
            w.u64(g.csr.load(std::memory_order_relaxed));
            w.u64(g.vsyncTick.load(std::memory_order_relaxed));
            w.u64(g.imr);
            w.u64(g.busdir);
            w.u64(g.siglblid);
            w.pod(m.vif0_regs);
            w.pod(m.vif1_regs);
            w.pod(m.dma_regs);
            w.u64(m.m_tlbEntries.size());
            for (const auto &e : m.m_tlbEntries)
            {
                w.u32(e.vpn);
                w.u32(e.pfn);
                w.u32(e.mask);
                w.boolean(e.valid);
            }
            w.boolean(m.m_seenGifCopy);
            w.boolean(m.m_path3Masked);
            w.u32(m.m_vif1PendingPath2ImageQwc);
            w.boolean(m.m_vif1PendingPath2DirectHl);
            w.u64(m.m_path3MaskedFifo.size());
            for (const auto &packet : m.m_path3MaskedFifo)
                w.podVector(packet);
            auto saveTransfers = [&](const std::vector<PS2Memory::PendingTransfer> &list)
            {
                w.u64(list.size());
                for (const auto &t : list)
                {
                    w.boolean(t.fromScratchpad);
                    w.u32(t.srcAddr);
                    w.u32(t.qwc);
                    w.podVector(t.chainData);
                }
            };
            saveTransfers(m.m_pendingGifTransfers);
            saveTransfers(m.m_pendingVif0Transfers);
            saveTransfers(m.m_pendingVif1Transfers);
            {
                std::lock_guard<std::mutex> lock(m.m_completedDmacMutex);
                w.podVector(m.m_completedDmacCauses);
            }
            for (const auto &t : m.m_eeTimers)
            {
                w.u32(t.count);
                w.u32(t.mode);
                w.u32(t.compare);
                w.u32(t.hold);
                w.u64(t.clockRemainder);
            }
            w.endSection();
        }

        static void loadMemory(Reader &r, PS2Memory &m)
        {
            if (r.beginSection(tag("EEMM")) != 1u)
                return;
            r.blob(m.m_rdram, PS2_RAM_SIZE);
            r.blob(m.m_scratchpad, PS2_SCRATCHPAD_SIZE);
            r.blob(m.m_vu0Code, PS2_VU0_CODE_SIZE);
            r.blob(m.m_vu0Data, PS2_VU0_DATA_SIZE);
            r.blob(m.m_vu1Code, PS2_VU1_CODE_SIZE);
            r.blob(m.m_vu1Data, PS2_VU1_DATA_SIZE);
            r.blob(m.m_gsVRAM, PS2_GS_VRAM_SIZE);
            m.m_ioRegisters.clear();
            const uint64_t ioCount = r.u64();
            for (uint64_t i = 0; i < ioCount && r.ok(); ++i)
            {
                const uint32_t addr = r.u32();
                m.m_ioRegisters[addr] = r.u32();
            }
            GSRegisters &g = m.gs_regs;
            for (uint64_t *v : {&g.pmode, &g.smode1, &g.smode2, &g.srfsh, &g.synch1, &g.synch2, &g.syncv, &g.dispfb1, &g.display1,
                                &g.dispfb2, &g.display2, &g.extbuf, &g.extdata, &g.extwrite, &g.bgcolor})
                *v = r.u64();
            g.csr.store(r.u64(), std::memory_order_relaxed);
            g.vsyncTick.store(r.u64(), std::memory_order_relaxed);
            g.imr = r.u64();
            g.busdir = r.u64();
            g.siglblid = r.u64();
            r.pod(m.vif0_regs);
            r.pod(m.vif1_regs);
            r.pod(m.dma_regs);
            // Element counts come from the file: bound them before resizing, so a damaged count fails the load
            // with a message instead of an allocation of gigabytes (podVector/str already check against the bytes left).
            auto count = [&](uint64_t n, uint64_t max, const char *what) -> size_t
            {
                if (n > max)
                {
                    r.fail(std::string(what) + " count " + std::to_string(n) + " out of range");
                    return 0u;
                }
                return static_cast<size_t>(n);
            };
            m.m_tlbEntries.resize(count(r.u64(), 64u, "TLB entry")); // the R5900 has 48 TLB entries
            for (auto &e : m.m_tlbEntries)
            {
                e.vpn = r.u32();
                e.pfn = r.u32();
                e.mask = r.u32();
                e.valid = r.boolean();
            }
            m.m_seenGifCopy = r.boolean();
            m.m_path3Masked = r.boolean();
            m.m_vif1PendingPath2ImageQwc = r.u32();
            m.m_vif1PendingPath2DirectHl = r.boolean();
            m.m_path3MaskedFifo.resize(count(r.u64(), 1u << 20, "masked PATH3 packet")); // unbounded while PATH3 is masked
            for (auto &packet : m.m_path3MaskedFifo)
                r.podVector(packet);
            auto loadTransfers = [&](std::vector<PS2Memory::PendingTransfer> &list)
            {
                list.resize(count(r.u64(), 1u << 20, "pending transfer"));
                for (auto &t : list)
                {
                    t.fromScratchpad = r.boolean();
                    t.srcAddr = r.u32();
                    t.qwc = r.u32();
                    r.podVector(t.chainData);
                }
            };
            loadTransfers(m.m_pendingGifTransfers);
            loadTransfers(m.m_pendingVif0Transfers);
            loadTransfers(m.m_pendingVif1Transfers);
            {
                std::lock_guard<std::mutex> lock(m.m_completedDmacMutex);
                r.podVector(m.m_completedDmacCauses);
            }
            for (auto &t : m.m_eeTimers)
            {
                t.count = r.u32();
                t.mode = r.u32();
                t.compare = r.u32();
                t.hold = r.u32();
                t.clockRemainder = r.u64();
            }
            // Derived caches keyed on code memory: the VU engines re-decode when the generation changes.
            m.markVU0CodeModified();
            m.markVU1CodeModified();
            r.endSection("EEMM");
        }

        // ---- EE kernel: threads, semaphores, event flags, alarms, interrupt handlers, ready queues, timed events --------
        static void saveWait(Writer &w, const EeWaitState &wait)
        {
            w.u8(static_cast<uint8_t>(wait.reason));
            w.u8(static_cast<uint8_t>(wait.payload.index()));
            if (const auto *p = std::get_if<EeSemaphoreWait>(&wait.payload))
                w.i32(p->id);
            else if (const auto *p = std::get_if<EeEventFlagWait>(&wait.payload))
            {
                w.i32(p->id);
                w.u32(p->bits);
                w.u32(p->mode);
                w.u32(p->resultAddress);
            }
            else if (const auto *p = std::get_if<EeVSyncWait>(&wait.payload))
            {
                w.u64(p->afterTick);
                w.i32(p->fixedResult);
            }
            else if (const auto *p = std::get_if<EeExternalWait>(&wait.payload))
            {
                w.u32(p->type);
                w.u64(p->token);
            }
            w.pod(wait.tag);
        }

        static void loadWait(Reader &r, EeWaitState &wait, PS2Runtime &rt)
        {
            wait = {};
            wait.reason = static_cast<EeWaitReason>(r.u8());
            switch (r.u8())
            {
            case 1:
            {
                EeSemaphoreWait p;
                p.id = r.i32();
                wait.payload = p;
                break;
            }
            case 2:
            {
                EeEventFlagWait p;
                p.id = r.i32();
                p.bits = r.u32();
                p.mode = r.u32();
                p.resultAddress = r.u32();
                wait.payload = p;
                break;
            }
            case 3:
            {
                EeVSyncWait p;
                p.afterTick = r.u64();
                p.fixedResult = r.i32();
                wait.payload = p;
                break;
            }
            case 4:
            {
                EeExternalWait p;
                p.type = r.u32();
                p.token = r.u64();
                wait.payload = p;
                break;
            }
            default:
                break;
            }
            r.pod(wait.tag);
            wait.completion = makeCompletion(wait.tag, rt);
        }

        // Why the scheduler cannot be saved right now (empty = it can).
        static std::string kernelBlocker(const EeScheduler &s)
        {
            if (!s.m_pendingInvocations.empty())
                return "a guest callback is queued";
            for (const auto &[id, t] : s.m_threads)
            {
                if (!t.invocations.empty())
                    return "thread " + std::to_string(id) + " is inside a callback/interrupt handler";
                if (t.wait.completion && t.wait.tag.kind == 0u)
                    return "thread " + std::to_string(id) + " waits with a host continuation (movie playback?)";
                if (t.resumeCompletion && t.resumeTag.kind == 0u)
                    return "thread " + std::to_string(id) + " resumes with a host continuation";
            }
            return {};
        }

        // Logs what the guest threads and queued callbacks are doing (when a save is refused, e.g. during a hang).
        static void dumpKernel(PS2Runtime &rt)
        {
            const EeScheduler &s = *rt.m_eeScheduler;
            static const char *kinds[] = {"interrupt", "alarm", "gs-callback", "rpc-callback", "syscall-override", "exit-handler", "hle-call", "sif-command"};
            static const char *states[] = {"running", "ready", "waiting", "waitsusp", "suspended", "dormant"};
            static const char *reasons[] = {"none", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
            std::cout << "[state] kernel: current thread " << s.m_currentThreadId << ", " << s.m_pendingInvocations.size()
                      << " queued callbacks, tick " << s.m_vsyncTick << std::endl;
            for (const GuestInvocation &inv : s.m_pendingInvocations)
                std::cout << "[state]   queued " << kinds[static_cast<int>(inv.kind) % 8] << " seq=" << inv.sequence << " tag=0x"
                          << std::hex << inv.tag << " pc=0x" << inv.context.pc << std::dec << std::endl;
            for (const auto &[id, t] : s.m_threads)
            {
                const R5900Context &c = t.activeContext();
                std::cout << "[state]   thread " << id << " prio=" << t.currentPriority << " " << states[static_cast<int>(t.status) % 6]
                          << " wait=" << reasons[static_cast<int>(t.wait.reason) % 7] << " pc=0x" << std::hex << c.pc
                          << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(c.r[31], 0)) << " entry=0x" << t.entry << std::dec
                          << " invocations=" << t.invocations.size();
                for (const GuestInvocation &inv : t.invocations)
                    std::cout << " [" << kinds[static_cast<int>(inv.kind) % 8] << " pc=0x" << std::hex << inv.context.pc << std::dec << "]";
                std::cout << std::endl;
            }
            for (int prio = 0; prio < EeScheduler::kPriorityCount; ++prio)
                if (!s.m_readyQueues[prio].empty())
                {
                    std::cout << "[state]   ready prio " << prio << ":";
                    for (int id : s.m_readyQueues[prio])
                        std::cout << " " << id;
                    std::cout << std::endl;
                }
        }

        static void saveKernel(Writer &w, EeScheduler &s)
        {
            w.beginSection(tag("EEKN"), 1u);
            for (const auto &queue : s.m_readyQueues)
            {
                w.u32(static_cast<uint32_t>(queue.size()));
                for (int id : queue)
                    w.i32(id);
            }
            w.u32(static_cast<uint32_t>(s.m_threads.size()));
            for (const auto &[id, t] : s.m_threads)
            {
                w.i32(t.id);
                w.pod(t.context);
                w.u32(t.entry);
                w.u32(t.stack);
                w.u32(t.stackSize);
                w.u32(t.gp);
                w.u32(t.attr);
                w.u32(t.option);
                w.u32(t.arg);
                w.i32(t.initialPriority);
                w.i32(t.currentPriority);
                w.u8(static_cast<uint8_t>(t.status));
                w.i32(t.suspendCount);
                w.u32(t.wakeupCount);
                w.boolean(t.ownsStack);
                w.u32(t.tlsBase);
                saveWait(w, t.wait);
                w.pod(t.resumeTag);
            }
            w.u32(static_cast<uint32_t>(s.m_semaphores.size()));
            for (const auto &[id, sem] : s.m_semaphores)
            {
                w.i32(sem.id);
                w.i32(sem.count);
                w.i32(sem.maxCount);
                w.i32(sem.initCount);
                w.u32(sem.attr);
                w.u32(sem.option);
                w.u32(static_cast<uint32_t>(sem.waiters.size()));
                for (int waiter : sem.waiters)
                    w.i32(waiter);
            }
            w.u32(static_cast<uint32_t>(s.m_eventFlags.size()));
            for (const auto &[id, ef] : s.m_eventFlags)
            {
                w.i32(ef.id);
                w.u32(ef.attr);
                w.u32(ef.option);
                w.u32(ef.initBits);
                w.u32(ef.bits);
                w.u32(static_cast<uint32_t>(ef.waiters.size()));
                for (int waiter : ef.waiters)
                    w.i32(waiter);
            }
            w.u32(static_cast<uint32_t>(s.m_alarms.size()));
            for (const auto &[id, a] : s.m_alarms)
                w.pod(a);
            for (const auto *handlers : {&s.m_intcHandlers, &s.m_dmacHandlers})
            {
                w.u32(static_cast<uint32_t>(handlers->size()));
                for (const auto &[id, h] : *handlers)
                    w.pod(h);
            }
            for (int v : {s.m_nextThreadId, s.m_nextInvocationThreadId, s.m_nextSemaphoreId, s.m_nextEventFlagId, s.m_nextAlarmId,
                          s.m_nextIntcHandlerId, s.m_nextDmacHandlerId, s.m_intcHeadOrder, s.m_intcTailOrder, s.m_dmacHeadOrder,
                          s.m_dmacTailOrder, s.m_currentThreadId})
                w.i32(v);
            w.u32(s.m_enabledIntcMask);
            w.u32(s.m_enabledDmacMask);
            w.boolean(s.m_rescheduleRequested);
            w.boolean(s.m_timeSliceExpired);
            w.boolean(s.m_insideInterrupt);
            w.u32(s.m_pendingEeTimerInterrupts);
            w.u64(s.m_eeCycle);
            w.u64(s.m_sliceEndCycle);
            {
                std::lock_guard lock(s.m_eventMutex);
                w.u32(static_cast<uint32_t>(s.m_events.size()));
                for (const EeEvent &e : s.m_events)
                    w.pod(e);
                // Timed events: host time relative to now, so a load re-anchors them.
                const auto now = std::chrono::steady_clock::now();
                w.u32(static_cast<uint32_t>(s.m_deadlines.size()));
                for (const auto &d : s.m_deadlines)
                {
                    w.u64(d.deadlineCycle);
                    w.pod(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d.hostDeadline - now).count()));
                    w.pod(d.event);
                    w.u64(d.sequence);
                }
            }
            w.u64(s.m_eventSequence);
            w.u64(s.m_invocationSequence);
            w.u64(s.m_vsyncTick);
            w.u32(s.m_vsyncFlagAddress);
            w.u32(s.m_vsyncTickAddress);
            w.u32(s.m_gsVSyncCallback);
            w.u32(s.m_gsVSyncCallbackGp);
            w.u32(s.m_gsVSyncCallbackSp);
            w.u32(static_cast<uint32_t>(s.m_invocationStackTops.size()));
            for (const auto &[key, top] : s.m_invocationStackTops)
            {
                w.u64(key);
                w.u32(top);
            }
            w.pod(s.m_vu0Shadow);
            w.boolean(s.m_vu0ShadowValid);
            w.endSection();
        }

        static void loadKernel(Reader &r, EeScheduler &s, PS2Runtime &rt)
        {
            if (r.beginSection(tag("EEKN")) != 1u)
                return;
            for (auto &queue : s.m_readyQueues)
            {
                queue.clear();
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok(); ++i)
                    queue.push_back(r.i32());
            }
            s.m_threads.clear();
            const uint32_t threadCount = r.u32();
            for (uint32_t i = 0; i < threadCount && r.ok(); ++i)
            {
                GuestThread t{};
                t.id = r.i32();
                r.pod(t.context);
                t.entry = r.u32();
                t.stack = r.u32();
                t.stackSize = r.u32();
                t.gp = r.u32();
                t.attr = r.u32();
                t.option = r.u32();
                t.arg = r.u32();
                t.initialPriority = r.i32();
                t.currentPriority = r.i32();
                t.status = static_cast<EeThreadStatus>(r.u8());
                t.suspendCount = r.i32();
                t.wakeupCount = r.u32();
                t.ownsStack = r.boolean();
                t.tlsBase = r.u32();
                loadWait(r, t.wait, rt);
                r.pod(t.resumeTag);
                t.resumeCompletion = makeCompletion(t.resumeTag, rt);
                const int id = t.id;
                s.m_threads.emplace(id, std::move(t));
            }
            s.m_semaphores.clear();
            const uint32_t semCount = r.u32();
            for (uint32_t i = 0; i < semCount && r.ok(); ++i)
            {
                EeSemaphore sem{};
                sem.id = r.i32();
                sem.count = r.i32();
                sem.maxCount = r.i32();
                sem.initCount = r.i32();
                sem.attr = r.u32();
                sem.option = r.u32();
                const uint32_t n = r.u32();
                for (uint32_t k = 0; k < n && r.ok(); ++k)
                    sem.waiters.push_back(r.i32());
                const int id = sem.id;
                s.m_semaphores.emplace(id, std::move(sem));
            }
            s.m_eventFlags.clear();
            const uint32_t efCount = r.u32();
            for (uint32_t i = 0; i < efCount && r.ok(); ++i)
            {
                EeEventFlag ef{};
                ef.id = r.i32();
                ef.attr = r.u32();
                ef.option = r.u32();
                ef.initBits = r.u32();
                ef.bits = r.u32();
                const uint32_t n = r.u32();
                for (uint32_t k = 0; k < n && r.ok(); ++k)
                    ef.waiters.push_back(r.i32());
                const int id = ef.id;
                s.m_eventFlags.emplace(id, std::move(ef));
            }
            s.m_alarms.clear();
            const uint32_t alarmCount = r.u32();
            for (uint32_t i = 0; i < alarmCount && r.ok(); ++i)
            {
                EeAlarm a{};
                r.pod(a);
                s.m_alarms.emplace(a.id, a);
            }
            for (auto *handlers : {&s.m_intcHandlers, &s.m_dmacHandlers})
            {
                handlers->clear();
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && r.ok(); ++i)
                {
                    EeIrqHandler h{};
                    r.pod(h);
                    handlers->emplace(h.id, h);
                }
            }
            for (int *v : {&s.m_nextThreadId, &s.m_nextInvocationThreadId, &s.m_nextSemaphoreId, &s.m_nextEventFlagId, &s.m_nextAlarmId,
                           &s.m_nextIntcHandlerId, &s.m_nextDmacHandlerId, &s.m_intcHeadOrder, &s.m_intcTailOrder, &s.m_dmacHeadOrder,
                           &s.m_dmacTailOrder, &s.m_currentThreadId})
                *v = r.i32();
            s.m_enabledIntcMask = r.u32();
            s.m_enabledDmacMask = r.u32();
            s.m_rescheduleRequested = r.boolean();
            s.m_timeSliceExpired = r.boolean();
            s.m_insideInterrupt = r.boolean();
            s.m_pendingEeTimerInterrupts = r.u32();
            s.m_eeCycle = r.u64();
            s.m_sliceEndCycle = r.u64();
            {
                std::lock_guard lock(s.m_eventMutex);
                s.m_events.clear();
                const uint32_t eventCount = r.u32();
                for (uint32_t i = 0; i < eventCount && r.ok(); ++i)
                {
                    EeEvent e{};
                    r.pod(e);
                    s.m_events.push_back(e);
                }
                s.m_deadlines.clear();
                const auto now = std::chrono::steady_clock::now();
                const uint32_t deadlineCount = r.u32();
                for (uint32_t i = 0; i < deadlineCount && r.ok(); ++i)
                {
                    EeScheduler::ScheduledEvent d{};
                    d.deadlineCycle = r.u64();
                    const int64_t relNs = r.get<int64_t>();
                    d.hostDeadline = now + std::chrono::nanoseconds(relNs);
                    r.pod(d.event);
                    d.sequence = r.u64();
                    s.m_deadlines.push_back(d);
                }
                s.m_pendingInvocations.clear();
            }
            s.m_eventSequence = r.u64();
            s.m_invocationSequence = r.u64();
            s.m_vsyncTick = r.u64();
            s.m_vsyncFlagAddress = r.u32();
            s.m_vsyncTickAddress = r.u32();
            s.m_gsVSyncCallback = r.u32();
            s.m_gsVSyncCallbackGp = r.u32();
            s.m_gsVSyncCallbackSp = r.u32();
            s.m_invocationStackTops.clear();
            const uint32_t topCount = r.u32();
            for (uint32_t i = 0; i < topCount && r.ok(); ++i)
            {
                const uint64_t key = r.u64();
                s.m_invocationStackTops[key] = r.u32();
            }
            r.pod(s.m_vu0Shadow);
            s.m_vu0ShadowValid = r.boolean();
            {
                std::lock_guard lock(s.m_eventMutex);
                s.updateNextDeadline();
            }
            s.m_checkpointPending.store(true, std::memory_order_release);
            s.publishSnapshot();
            r.endSection("EEKN");
        }

        // ---- GS: frontend registers and vertex queue, backend CLUT/transfer state; GPU caches dropped on load ------------
        static void saveGs(Writer &w, GS &gs)
        {
            w.beginSection(tag("GSST"), 1u);
            {
                std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
                w.pod(gs.m_ctx);
                w.pod(gs.m_prim);
                w.pod(gs.m_primRegister);
                w.pod(gs.m_prmodeRegister);
                for (uint8_t v : {gs.m_curR, gs.m_curG, gs.m_curB, gs.m_curA, gs.m_curFog, gs.m_fogR, gs.m_fogG, gs.m_fogB})
                    w.u8(v);
                w.pod(gs.m_curQ);
                w.pod(gs.m_curS);
                w.pod(gs.m_curT);
                w.pod(gs.m_curU);
                w.pod(gs.m_curV);
                w.boolean(gs.m_prmodecont);
                w.boolean(gs.m_pabe);
                w.u64(gs.m_scanmsk);
                w.u64(gs.m_dimx);
                w.u64(gs.m_dthe);
                w.u64(gs.m_colclamp);
                w.pod(gs.m_texa);
                w.pod(gs.m_texclut);
                w.pod(gs.m_bitbltbuf);
                w.pod(gs.m_trxpos);
                w.pod(gs.m_trxreg);
                w.u32(gs.m_trxdir);
                w.pod(gs.m_vtxQueue);
                w.i32(gs.m_vtxCount);
                w.i32(gs.m_vtxIndex);
                w.pod(gs.m_preferredDisplaySourceFrame);
                w.u32(gs.m_preferredDisplayDestFbp);
                w.boolean(gs.m_hasPreferredDisplaySource);
            }
            {
                std::lock_guard<std::mutex> lock(gs.m_backendLifetimeMutex);
                if (auto *gpu = dynamic_cast<GSGpuBackend *>(gs.m_backend.get()))
                {
                    w.u8(2u);
                    gpu->SaveState(w);
                }
                else if (auto *cpu = dynamic_cast<GSCpuBackend *>(gs.m_backend.get()))
                {
                    w.u8(1u);
                    cpu->SaveState(w);
                }
                else
                    w.u8(0u);
            }
            w.endSection();
        }

        static void loadGs(Reader &r, GS &gs)
        {
            if (r.beginSection(tag("GSST")) != 1u)
                return;
            {
                std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
                r.pod(gs.m_ctx);
                r.pod(gs.m_prim);
                r.pod(gs.m_primRegister);
                r.pod(gs.m_prmodeRegister);
                for (uint8_t *v : {&gs.m_curR, &gs.m_curG, &gs.m_curB, &gs.m_curA, &gs.m_curFog, &gs.m_fogR, &gs.m_fogG, &gs.m_fogB})
                    *v = r.u8();
                r.pod(gs.m_curQ);
                r.pod(gs.m_curS);
                r.pod(gs.m_curT);
                r.pod(gs.m_curU);
                r.pod(gs.m_curV);
                gs.m_prmodecont = r.boolean();
                gs.m_pabe = r.boolean();
                gs.m_scanmsk = r.u64();
                gs.m_dimx = r.u64();
                gs.m_dthe = r.u64();
                gs.m_colclamp = r.u64();
                r.pod(gs.m_texa);
                r.pod(gs.m_texclut);
                r.pod(gs.m_bitbltbuf);
                r.pod(gs.m_trxpos);
                r.pod(gs.m_trxreg);
                gs.m_trxdir = r.u32();
                r.pod(gs.m_vtxQueue);
                gs.m_vtxCount = r.i32();
                gs.m_vtxIndex = r.i32();
                r.pod(gs.m_preferredDisplaySourceFrame);
                gs.m_preferredDisplayDestFbp = r.u32();
                gs.m_hasPreferredDisplaySource = r.boolean();
            }
            {
                std::lock_guard<std::mutex> lock(gs.m_backendLifetimeMutex);
                const uint8_t kind = r.u8();
                auto *gpu = dynamic_cast<GSGpuBackend *>(gs.m_backend.get());
                auto *cpu = dynamic_cast<GSCpuBackend *>(gs.m_backend.get());
                if (kind == 2u && gpu)
                    gpu->LoadState(r);
                else if (kind == 1u && cpu)
                    cpu->LoadState(r);
                else if (kind != 0u)
                    r.fail("the state was saved with the other GS renderer (PS2X_GPU)");
            }
            r.endSection("GSST");
        }

        // ---- VU1 registers (VU0 lives in the EE contexts / the scheduler's shared VU0 copy) ----------------------------
        static void saveVu(Writer &w, PS2Runtime &rt)
        {
            w.beginSection(tag("VU01"), 1u);
            w.pod(rt.m_vu1.m_state);
            w.endSection();
        }
        static void loadVu(Reader &r, PS2Runtime &rt)
        {
            if (r.beginSection(tag("VU01")) != 1u)
                return;
            r.pod(rt.m_vu1.m_state);
            rt.m_vu1.m_decodedCodeCacheValid = false;
            r.endSection("VU01");
        }

        // ---- Runtime: main context template, exit handlers, syscall overrides, guest heap, module list -----------------
        static void saveRuntime(Writer &w, PS2Runtime &rt)
        {
            w.beginSection(tag("RTIM"), 1u);
            w.pod(rt.m_cpuContext);
            {
                std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
                w.u64(rt.m_eeExitHandlers.size());
                for (const auto &[thread, handlers] : rt.m_eeExitHandlers)
                {
                    w.i32(thread);
                    w.podVector(handlers);
                }
                w.podMap(rt.m_eeSyscallOverrides);
                std::vector<uint32_t> mirrors(rt.m_eeSyscallMirrorAddresses.begin(), rt.m_eeSyscallMirrorAddresses.end());
                w.podVector(mirrors);
            }
            {
                std::lock_guard<std::mutex> lock(rt.m_guestHeapMutex);
                w.podVector(rt.m_guestHeapBlocks);
                for (uint32_t v : {rt.m_guestHeapBase, rt.m_guestHeapEnd, rt.m_guestHeapLimit, rt.m_guestHeapSuggestedBase})
                    w.u32(v);
                w.boolean(rt.m_guestHeapConfigured);
            }
            {
                std::lock_guard<std::mutex> lock(rt.m_asyncCallbackStackMutex);
                w.u32(rt.m_asyncCallbackStackFloor);
                w.u32(rt.m_asyncCallbackStackTop);
            }
            w.u64(rt.m_loadedModules.size());
            for (const auto &m : rt.m_loadedModules)
            {
                w.str(m.name);
                w.u32(m.baseAddress);
                w.u64(m.size);
                w.boolean(m.active);
            }
            w.i32(rt.m_vfs.m_nextDescriptor);
            w.endSection();
        }
        static void loadRuntime(Reader &r, PS2Runtime &rt)
        {
            if (r.beginSection(tag("RTIM")) != 1u)
                return;
            r.pod(rt.m_cpuContext);
            {
                std::lock_guard<std::mutex> lock(rt.m_eeKernelStateMutex);
                rt.m_eeExitHandlers.clear();
                const uint64_t n = r.u64();
                for (uint64_t i = 0; i < n && r.ok(); ++i)
                {
                    const int thread = r.i32();
                    r.podVector(rt.m_eeExitHandlers[thread]);
                }
                r.podMap(rt.m_eeSyscallOverrides);
                std::vector<uint32_t> mirrors;
                r.podVector(mirrors);
                rt.m_eeSyscallMirrorAddresses = std::unordered_set<uint32_t>(mirrors.begin(), mirrors.end());
            }
            {
                std::lock_guard<std::mutex> lock(rt.m_guestHeapMutex);
                r.podVector(rt.m_guestHeapBlocks);
                for (uint32_t *v : {&rt.m_guestHeapBase, &rt.m_guestHeapEnd, &rt.m_guestHeapLimit, &rt.m_guestHeapSuggestedBase})
                    *v = r.u32();
                rt.m_guestHeapConfigured = r.boolean();
            }
            {
                std::lock_guard<std::mutex> lock(rt.m_asyncCallbackStackMutex);
                rt.m_asyncCallbackStackFloor = r.u32();
                rt.m_asyncCallbackStackTop = r.u32();
            }
            rt.m_loadedModules.clear();
            const uint64_t modules = r.u64();
            for (uint64_t i = 0; i < modules && r.ok(); ++i)
            {
                PS2Runtime::LoadedModule m;
                m.name = r.str();
                m.baseAddress = r.u32();
                m.size = static_cast<size_t>(r.u64());
                m.active = r.boolean();
                rt.m_loadedModules.push_back(std::move(m));
            }
            rt.m_vfs.m_nextDescriptor = r.i32();
            rt.m_activeOverlay.store(nullptr, std::memory_order_relaxed); // re-validated against RAM on the next lookup
            r.endSection("RTIM");
        }

        // ---- Library-replacement parts ----------------------------------------------------------------------------------
        static void saveParts(Writer &w)
        {
            w.beginSection(tag("PRTS"), 1u);
            const auto list = sortedParts();
            w.u32(static_cast<uint32_t>(list.size()));
            for (const Part &p : list)
            {
                w.str(p.name);
                const size_t before = w.data().size();
                w.u64(0u);
                p.save(w);
                const uint64_t size = w.data().size() - before - sizeof(uint64_t);
                std::memcpy(w.data().data() + before, &size, sizeof(size));
            }
            w.endSection();
        }
        static void loadParts(Reader &r)
        {
            if (r.beginSection(tag("PRTS")) != 1u)
                return;
            const auto list = sortedParts();
            const uint32_t n = r.u32();
            if (n != list.size())
                r.fail("the state has " + std::to_string(n) + " library parts, this build " + std::to_string(list.size()));
            for (const Part &p : list)
            {
                if (!r.ok())
                    break;
                const std::string name = r.str();
                if (name != p.name)
                {
                    r.fail("library part '" + name + "' where '" + p.name + "' was expected");
                    break;
                }
                (void)r.u64();
                p.load(r);
            }
            r.endSection("PRTS");
        }

        // ---- Whole machine ------------------------------------------------------------------------------------------
        static std::string blocker(PS2Runtime &rt)
        {
            if (std::string why = kernelBlocker(*rt.m_eeScheduler); !why.empty())
                return why;
            if (rt.m_iopSubsystem)
                if (std::string why = rt.m_iopSubsystem->stateBlocker(); !why.empty())
                    return why;
            for (const Part &p : sortedParts())
                if (p.blocker)
                    if (const char *why = p.blocker())
                        return why;
            if (!rt.m_vfs.m_descriptors.empty())
                return "an EE file is open";
            return {};
        }

        // Render threads idle, queued GIF packets processed: everything they own is back with this thread.
        static void quiesce(PS2Runtime &rt)
        {
            ps2_gs_worker::drain(ps2_gs_worker::Sync::GsDirect);
            rt.m_gifArbiter.drain();
        }

        static void saveAll(Writer &w, PS2Runtime &rt)
        {
            quiesce(rt);
            Header h;
            h.vsyncTick = rt.m_eeScheduler->currentVSyncTick();
            h.savedAtUnix = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                      std::chrono::system_clock::now().time_since_epoch())
                                                      .count());
            w.pod(h);
            w.str(g_gameName);
            saveMemory(w, rt.m_memory);
            saveKernel(w, *rt.m_eeScheduler);
            saveRuntime(w, rt);
            saveVu(w, rt);
            saveGs(w, rt.m_gs);
            if (rt.m_iopSubsystem)
            {
                auto lock = rt.m_iopHost->lockIop();
                rt.m_iopSubsystem->saveState(w);
            }
            saveParts(w);
            w.u32(kMagic); // end marker
        }

        static void loadAll(Reader &r, PS2Runtime &rt)
        {
            quiesce(rt);
            Header h;
            r.pod(h);
            if (!r.ok())
                return;
            if (h.magic != kMagic)
            {
                r.fail("not a save state file");
                return;
            }
            if (h.version != kFormatVersion)
            {
                r.fail("state format " + std::to_string(h.version) + ", this build reads " + std::to_string(kFormatVersion));
                return;
            }
            const LayoutCheck expected{};
            if (std::memcmp(&h.layout, &expected, sizeof(LayoutCheck)) != 0)
            {
                r.fail("the state was written by a build with different CPU/VU/GS structures");
                return;
            }
            const std::string game = r.str();
            if (game != g_gameName)
            {
                r.fail("the state is from '" + game + "', this is '" + g_gameName + "'");
                return;
            }
            loadMemory(r, rt.m_memory);
            loadKernel(r, *rt.m_eeScheduler, rt);
            loadRuntime(r, rt);
            loadVu(r, rt);
            loadGs(r, rt.m_gs);
            if (rt.m_iopSubsystem)
            {
                auto lock = rt.m_iopHost->lockIop();
                rt.m_iopSubsystem->loadState(r);
            }
            loadParts(r);
            if (r.ok() && r.u32() != kMagic)
                r.fail("state file end marker missing");
            // Host-side caches of guest data.
            rt.m_audioBackend.stopAll();
        }
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Orchestration
// ---------------------------------------------------------------------------------------------------------------------
namespace ps2_state
{
    namespace
    {
        std::mutex g_messageMutex;
        std::chrono::steady_clock::time_point g_messageTime{};

        void setMessage(const std::string &text)
        {
            {
                std::lock_guard<std::mutex> lock(g_messageMutex);
                g_lastMessage = text;
                g_messageTime = std::chrono::steady_clock::now();
            }
            std::cout << "[state] " << text << std::endl;
        }

        std::string stateDir()
        {
            if (const char *env = std::getenv("PS2X_STATE_DIR"); env && *env)
                return env;
            return "states";
        }

        void postRequest(Op op, const std::string &path)
        {
            {
                std::lock_guard<std::mutex> lock(g_pathMutex);
                g_explicitPath = path;
                g_requestTime = std::chrono::steady_clock::now();
            }
            g_op.store(static_cast<int>(op), std::memory_order_release);
            if (g_runtimeForWake != nullptr)
                g_runtimeForWake->postEeEvent(EeEvent{EeEventType::ExternalWake, kWakeType, 0u});
        }

        bool writeFile(const std::string &path, const std::vector<uint8_t> &data, std::string &error)
        {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            const std::string tmp = path + ".tmp";
            FILE *f = std::fopen(tmp.c_str(), "wb");
            if (!f)
            {
                error = "cannot write " + tmp;
                return false;
            }
            const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            std::fclose(f);
            if (!ok)
            {
                error = "write failed: " + tmp;
                return false;
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                std::filesystem::remove(path, ec);
                std::filesystem::rename(tmp, path, ec);
            }
            if (ec)
            {
                error = "cannot rename to " + path + ": " + ec.message();
                return false;
            }
            return true;
        }

        bool readFile(const std::string &path, std::vector<uint8_t> &data, std::string &error)
        {
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f)
            {
                error = "no state file " + path;
                return false;
            }
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            data.resize(size > 0 ? static_cast<size_t>(size) : 0u);
            const bool ok = size > 0 && std::fread(data.data(), 1, data.size(), f) == data.size();
            std::fclose(f);
            if (!ok)
                error = "cannot read " + path;
            return ok;
        }
    }

    void requestSave(int s)
    {
        setSlot(s);
        postRequest(Op::Save, {});
    }
    void requestLoad(int s)
    {
        setSlot(s);
        postRequest(Op::Load, {});
    }
    void requestSaveTo(const std::string &path) { postRequest(Op::Save, path); }
    void requestLoadFrom(const std::string &path) { postRequest(Op::Load, path); }
    int slot() { return g_slot.load(std::memory_order_relaxed); }
    void stepSlot(int delta)
    {
        setSlot(slot() + delta);
        std::error_code ec;
        const std::string path = slotPath(slot());
        if (!std::filesystem::exists(path, ec))
        {
            setMessage("State slot " + std::to_string(slot()) + " (empty)");
            return;
        }
        const auto written = std::filesystem::last_write_time(path, ec);
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        char when[64] = {};
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        std::strftime(when, sizeof(when), "%b %d %H:%M", &local);
        setMessage("State slot " + std::to_string(slot()) + " (saved " + when + ")");
    }
    void setSlot(int s) { g_slot.store(((s % 10) + 10) % 10, std::memory_order_relaxed); }
    std::string slotPath(int s)
    {
        std::string game = g_gameName.empty() ? std::string("game") : g_gameName;
        return stateDir() + "/" + game + ".slot" + std::to_string(s) + ".p2s";
    }
    bool pending() { return g_op.load(std::memory_order_acquire) != 0; }

    std::string lastMessage(double &ageSeconds)
    {
        std::lock_guard<std::mutex> lock(g_messageMutex);
        ageSeconds = g_messageTime == std::chrono::steady_clock::time_point{}
                         ? 1e9
                         : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_messageTime).count();
        return g_lastMessage;
    }

    bool service(PS2Runtime &rt)
    {
        const Op op = static_cast<Op>(g_op.exchange(0, std::memory_order_acq_rel));
        if (op == Op::None)
            return false;
        std::string path, what;
        std::chrono::steady_clock::time_point requested;
        {
            std::lock_guard<std::mutex> lock(g_pathMutex);
            path = g_explicitPath.empty() ? slotPath(slot()) : g_explicitPath;
            what = g_explicitPath.empty() ? ("state " + std::to_string(slot())) : g_explicitPath;
            requested = g_requestTime;
        }

        // Not every safe point can be captured (a guest callback in flight, a movie playing...): retry for a while.
        // A load replaces all of it, so only saves wait.
        const std::string blocker = op == Op::Save ? Access::blocker(rt) : std::string();
        if (!blocker.empty())
        {
            if (std::chrono::steady_clock::now() - requested < std::chrono::seconds(3))
            {
                g_op.store(static_cast<int>(op), std::memory_order_release);
                return false;
            }
            setMessage("Cannot save " + what + " now: " + blocker);
            Access::dumpKernel(rt);
            for (const Part &p : sortedParts())
                if (p.describe)
                    p.describe();
            return false;
        }

        const auto t0 = std::chrono::steady_clock::now();
        if (op == Op::Save)
        {
            Writer w;
            Access::saveAll(w, rt);
            std::string error;
            if (!writeFile(path, w.data(), error))
            {
                setMessage("Save failed: " + error);
                return true;
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            char line[256];
            std::snprintf(line, sizeof(line), "Saved %s (tick %llu, %.1f MB, %.0f ms)", what.c_str(),
                          static_cast<unsigned long long>(rt.eeScheduler().currentVSyncTick()), w.data().size() / 1048576.0, ms);
            setMessage(line);
            return true;
        }

        std::vector<uint8_t> data;
        std::string error;
        if (!readFile(path, data, error))
        {
            setMessage("Load failed: " + error);
            return true;
        }
        // Keep the current machine so a bad file cannot leave it half loaded.
        Writer backup;
        Access::saveAll(backup, rt);
        Reader r(data.data(), data.size());
        Access::loadAll(r, rt);
        if (!r.ok())
        {
            const std::string why = r.error();
            Reader undo(backup.data().data(), backup.data().size());
            Access::loadAll(undo, rt);
            setMessage("Load failed (" + why + ")" + (undo.ok() ? "; kept the running game" : "; restore of the running game ALSO failed: " + undo.error()));
            return true;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char line[256];
        std::snprintf(line, sizeof(line), "Loaded %s (tick %llu, %.0f ms)", what.c_str(),
                      static_cast<unsigned long long>(rt.eeScheduler().currentVSyncTick()), ms);
        setMessage(line);
        return true;
    }

    void checkAutomation(uint64_t vsyncTick)
    {
        struct Auto
        {
            uint64_t tick = 0;
            std::string path;
            bool armed = false;
        };
        auto parse = [](const char *name)
        {
            Auto a;
            if (const char *env = std::getenv(name))
            {
                const std::string v(env);
                const size_t comma = v.find(',');
                if (comma != std::string::npos)
                {
                    a.tick = std::strtoull(v.c_str(), nullptr, 10);
                    a.path = v.substr(comma + 1);
                    a.armed = !a.path.empty();
                }
            }
            return a;
        };
        static Auto s_save = parse("PS2X_STATE_SAVE_AT");
        static Auto s_load = parse("PS2X_STATE_LOAD_AT");
        if (s_save.armed && vsyncTick >= s_save.tick)
        {
            s_save.armed = false;
            requestSaveTo(s_save.path);
        }
        if (s_load.armed && vsyncTick >= s_load.tick)
        {
            s_load.armed = false;
            requestLoadFrom(s_load.path);
        }
    }
}

namespace ps2_state
{
    void attach(PS2Runtime &runtime, const std::string &gameName)
    {
        g_runtimeForWake = &runtime;
        g_gameName = gameName;
    }
}
