#include "iop_emulator.h"
#include "emulator/spu2/spu2_device.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"
#include "../iop_module_manager.h"
#include "ps2x/state_io.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

// DIAGNOSTIC (PS2X_IOP_DIAG): vsync tick for the IOP thread / timer dump.
uint64_t ps2CurrentVsyncTick();

namespace ps2x::iop::detail
{
    // DIAGNOSTIC (PS2X_IOP_WATCH, defined in core/iop_memory.cpp).
    extern uint32_t g_iopWatchLo[4];
    extern uint32_t g_iopWatchHi[4];
    extern uint32_t g_iopWatchCount;
    extern void (*g_iopWatchHook)(uint32_t address, size_t size);
    void diagWatchHit(uint32_t address, size_t size);

    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        // DIAGNOSTIC: PS2X_IOP_DIAG=<interval ms of IOP time> (0 / unset = off), see diagTick().
        const uint32_t kIopDiagIntervalMs = std::getenv("PS2X_IOP_DIAG") ? static_cast<uint32_t>(std::strtoul(std::getenv("PS2X_IOP_DIAG"), nullptr, 0)) : 0u;
        const bool kIopDiag = kIopDiagIntervalMs != 0u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
            uint32_t argument1 = 0u;
            uint32_t releaseAddress = 0u; // IOP RAM block freed after the call (SIF command packet copy)
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(host, memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            spu2::attach(memory, &totalCycles);
            reset();
            diagAttach();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            vblank.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            iopTimeTarget = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
            spu2::reset(); // after totalCycles is zeroed: the chip is clocked from it
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (memory.hasDmaStart())
                if (const auto dma = memory.takeDmaStart())
                    pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
            static const uint32_t *const s_spu2Irqs = spu2::pendingIrqsPtr();
            if (*s_spu2Irqs != 0u)
                queueSpu2Interrupts();
        }

        // Interrupts the SPU2 raised (voice/DMA IRQ address hit, DMA 4/7 finished) go through the
        // same queue as other device interrupts and are dispatched between guest instructions.
        void queueSpu2Interrupts()
        {
            const uint32_t irqs = spu2::takePendingIrqs();
            if (irqs == 0u)
                return;
            if (irqs & 1u)
                pendingDmaInterrupts[spu2::kIrqSpu2] = totalCycles;
            if (irqs & 2u)
                pendingDmaInterrupts[spu2::kIrqDma4] = totalCycles;
            if (irqs & 4u)
                pendingDmaInterrupts[spu2::kIrqDma7] = totalCycles;
        }

        void serviceSpu2()
        {
            static const uint64_t *const s_spu2Next = spu2::nextEventCyclePtr();
            if (totalCycles < *s_spu2Next)
                return;
            spu2::update();
            queueSpu2Interrupts();
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        // PS2X_IOP_TRACE=<max lines>[,<library>]: log every IOP import call (library:ordinal, arguments, caller).
        // The optional library name limits the trace to that library.
        static void traceImportCall(const IopImportCall &call, const CpuState &cpu, uint64_t cycles, IopHost &host)
        {
            struct Config
            {
                uint32_t limit = 0u;
                std::string library;
            };
            static const Config config = []
            {
                Config c;
                if (const char *env = std::getenv("PS2X_IOP_TRACE"))
                {
                    c.limit = static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
                    if (const char *comma = std::strchr(env, ','))
                        c.library = comma + 1;
                }
                return c;
            }();
            static uint32_t logged = 0u;
            if (config.limit == 0u || logged >= config.limit)
                return;
            if (!config.library.empty() && !iequals(call.library, config.library))
                return;
            ++logged;
            char line[200];
            std::snprintf(line, sizeof(line), "[IOP trace] cyc=%llu %s:%u a0=%x a1=%x a2=%x a3=%x ra=%x",
                          static_cast<unsigned long long>(cycles), call.library.c_str(), static_cast<unsigned>(call.ordinal),
                          cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], cpu.gpr[31]);
            host.log(LogLevel::Info, line);
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            traceImportCall(call, cpu, totalCycles, host);
            if (kIopDiag && (iequals(call.library, "thbase") || iequals(call.library, "threadman")))
                diagImportCall(call.ordinal, cpu);
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // Import stubs are "jr ra" + an addiu marker: only decode (a table search) when the word is jr ra.
            if (memory.fetch32(cpu.pc) == 0x03E00008u)
            if (const auto import = imports.decode(cpu.pc))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                serviceSpu2();
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            // DIAGNOSTIC (PS2X_IOP_DIAG): a nested guest call (interrupt / timer / SIF handler, module start)
            // that did not return: instruction budget used up, or the handler tried to block (yielded).
            if (kIopDiag)
            {
                ++diagCbTotal;
                if (!cpu.stopped)
                {
                    ++diagCbUnfinished;
                    if (diagCbUnfinished <= 40u)
                    {
                        char line[256];
                        std::snprintf(line, sizeof(line),
                                      "[iopdiag] tick=%llu cyc=%llu UNFINISHED guest call fn=0x%x a0=0x%x -> pc=0x%x ra=0x%x yielded=%d depth=%u",
                                      static_cast<unsigned long long>(ps2CurrentVsyncTick()),
                                      static_cast<unsigned long long>(totalCycles), address, a0, cpu.pc, cpu.gpr[31],
                                      cpu.yielded ? 1 : 0, depth);
                        log(LogLevel::Info, line);
                    }
                }
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    const bool delivered = intrman.dispatchInterrupt(irq, *this);
                    if (kIopDiag && irq >= 0 && irq < 64)
                    {
                        ++diagIrqCount[irq];
                        if (delivered)
                            ++diagIrqDelivered[irq];
                    }
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           callback.argument1,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                    if (callback.releaseAddress != 0u)
                        (void)freeAllocation(callback.releaseAddress);
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // EE -> IOP SIF command (sceSifSendCmd on the EE). System commands are serviced by sifcmd
        // itself; user commands run the handler the IOP registered with sceSifAddCmdHandler. Like the
        // SIF interrupt on hardware, the handler runs in IOP time (a short DMA latency after the send),
        // not inside the EE thread that sent the packet. The handler gets a copy of the packet in IOP RAM.
        bool deliverSifCommand(uint32_t commandId, const void *packet, size_t packetSize)
        {
            constexpr uint64_t kSifCommandLatencyCycles = 256u;
            if (!packet || packetSize < 16u || packetSize > 112u)
                return false;
            static uint32_t logged = 0u;
            const bool consumed = rpc.applySystemCommand(commandId, packet, packetSize);
            const auto handler = consumed ? std::nullopt : rpc.findCmdHandler(commandId);
            if (logged < 32u)
            {
                ++logged;
                std::ostringstream out;
                out << "[IOP] sifcmd from EE: cid=0x" << std::hex << commandId << " size=0x" << packetSize
                    << (consumed ? " (system)" : handler ? " (handler)" : " (no handler)");
                log(LogLevel::Info, out.str());
            }
            // DIAGNOSTIC (PS2X_SIF_LOG): every user command with its packet
            // words 3..12 and the handler argument block's words 4..8, 16 and 22 (for ZOE's CDBIOS, cid 4: request
            // lsn / EE dest / bytes done / bytes to pause / total, abort flag, last command) when it is queued.
            static const bool s_sifCmdLog = std::getenv("PS2X_SIF_LOG") != nullptr;
            if (s_sifCmdLog && !consumed && handler && packetSize >= 0x30u)
            {
                uint32_t w[12]{};
                std::memcpy(w, packet, std::min<size_t>(packetSize, sizeof(w)));
                uint32_t h[23]{};
                (void)memory.readRam(handler->data, h, sizeof(h));
                char line[320];
                std::snprintf(line, sizeof(line),
                              "[IOP sif:cmd] cyc=%llu cid=0x%x type=%u p10=0x%x req=%x,%x,%x,%x,%x | iop req=%x,%x,%x,%x,%x abort=%u last=%u",
                              static_cast<unsigned long long>(totalCycles), commandId, w[3], w[4], w[8], w[9], w[10],
                              w[11], packetSize >= 0x34u ? static_cast<const uint32_t *>(packet)[12] : 0u, h[4], h[5], h[6],
                              h[7], h[8], h[16], h[22]);
                log(LogLevel::Info, line);
            }
            // DIAGNOSTIC (PS2X_IOP_DIAG): every EE -> IOP_MAIN sound command (cid 1) with the tick it was sent.
            if (kIopDiag && commandId == 1u)
            {
                uint32_t w[12]{};
                std::memcpy(w, packet, std::min<size_t>(packetSize, sizeof(w)));
                char line[256];
                std::snprintf(line, sizeof(line),
                              "[iopdiag] SOUNDCMD tick=%llu cyc=%llu psize=%zu words: %x %x %x %x | %x %x %x %x",
                              static_cast<unsigned long long>(ps2CurrentVsyncTick()), static_cast<unsigned long long>(totalCycles),
                              packetSize, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                log(LogLevel::Info, line);
            }
            if (consumed)
                return true;
            if (!handler)
                return false;
            const uint32_t copy = allocate(static_cast<uint32_t>(packetSize), 16u);
            if (copy == 0u)
                return false;
            if (!writeRam(copy, packet, packetSize))
            {
                (void)freeAllocation(copy);
                return false;
            }
            pendingGuestCallbacks.emplace(
                totalCycles + kSifCommandLatencyCycles,
                ScheduledGuestCallback{handler->function, handler->gp, copy, handler->data, copy});
            return true;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    if (kIopDiag)
                        diagTick();
                    serviceSpu2();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    for (const uint8_t irq : timrman.takePendingIrqs())
                        pendingDmaInterrupts[irq] = totalCycles;
                    vblank.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        nextWake = vblank.nextEventCycle(nextWake);
                        nextWake = std::min(nextWake, std::max(spu2::nextEventCycle(), totalCycles + 1u));
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    if (kIopDiag && next->id >= 0 && next->id < 64)
                        ++diagRuns[next->id];
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                // Mix whatever SPU2 samples are due for this batch.
                spu2::update();
                queueSpu2Interrupts();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            (void)memory.freeAllocation(it->second.base);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        uint64_t iopTimeTarget = 0; // IOP cycle the EE's elapsed time corresponds to
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        // DIAGNOSTIC (PS2X_IOP_DIAG=<interval ms of IOP time>): see diagTick() at the end of this file.
        uint64_t diagNextCycle = 0u;
        uint64_t diagLastChangeCycle = 0u;
        uint64_t diagLastChangeTick = 0u;
        uint32_t diagLastStatusTime = 0xFFFFFFFFu;
        uint32_t diagStallDumps = 0u;
        uint32_t diagIrqCount[64]{};
        uint32_t diagIrqDelivered[64]{};
        uint32_t diagCbUnfinished = 0u;
        uint32_t diagCbTotal = 0u;
        uint64_t diagRuns[64]{};      // timeslices run per IOP thread id
        uint64_t diagSleepCalls[64]{};  // SleepThread calls per thread id
        uint64_t diagWakeCalls[64]{};   // [i]WakeupThread calls targeting thread id
        uint64_t diagWakeWhileSleep[64]{}; // ... that found the thread in Sleep (woke it)
        uint64_t diagWakeBadId = 0u;    // [i]WakeupThread with an id that is not a thread
        void diagTick();
        void diagDump(const char *why);
        void diagAttach();
        void diagImportCall(uint16_t ordinal, const CpuState &cpu);
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        // The EE reports cycles at every block checkpoint (often a few dozen). Running the IOP
        // scheduler that often costs far more than the IOP work itself, so batch ~28 us of EE time.
        constexpr uint64_t kBatchEeCycles = 8192u;
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        if (total < kBatchEeCycles)
        {
            m_impl->eeCycleCarry = total;
            return;
        }
        m_impl->eeCycleCarry = total % 8u;

        // Run up to an absolute IOP time derived from EE time. Guest handlers (interrupts,
        // vblank, timers) advance totalCycles on their own; with a relative budget that time was
        // added on top of every batch, so the IOP (and the SPU2 clocked from it) ran ~0.5% fast.
        // Small overshoots are paid back by the next batches; large jumps (e.g. module start-up
        // code run outside this loop) are forgiven so the IOP never stalls waiting for the EE.
        constexpr uint64_t kMaxIopLead = 36864u * 4u; // ~4 ms of IOP time
        m_impl->iopTimeTarget += total / 8u;
        if (m_impl->totalCycles > m_impl->iopTimeTarget + kMaxIopLead)
            m_impl->iopTimeTarget = m_impl->totalCycles;
        if (m_impl->iopTimeTarget > m_impl->totalCycles)
            m_impl->runCycles(m_impl->iopTimeTarget - m_impl->totalCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    bool IopEmulator::deliverSifCommand(uint32_t commandId, const void *packet, size_t packetSize)
    {
        return m_impl->deliverSifCommand(commandId, packet, packetSize);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}

// ---------------------------------------------------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------------------------------------------------
namespace ps2x::iop::detail
{
    struct IopStateAccess
    {
        static bool threadRunning(const IopKernel &k) { return k.m_currentThread != nullptr; }

        // DIAGNOSTIC (PS2X_IOP_DIAG): read-only views for IopEmulator::Impl::diagDump().
        static const std::map<int, IopThread> &diagThreads(const IopKernel &k) { return k.m_threads; }
        static const IopThread *diagCurrent(const IopKernel &k) { return k.m_currentThread; }
        static const auto &diagSemaphores(const IopKernel &k) { return k.m_semaphores; }
        static const auto &diagEventFlags(const IopKernel &k) { return k.m_eventFlags; }
        static const auto &diagTimers(const IopTimrman &t) { return t.m_timers; }
        static size_t diagPendingTimerIrqs(const IopTimrman &t) { return t.m_pendingIrqs.size(); }
        static const auto &diagHandlers(const IopIntrman &i) { return i.m_handlers; }
        static const auto &diagEnabled(const IopIntrman &i) { return i.m_enabled; }

        template <typename Map>
        static void savePodMap(ps2_state::Writer &w, const Map &map)
        {
            w.u32(static_cast<uint32_t>(map.size()));
            for (const auto &[key, value] : map)
            {
                w.pod(key);
                w.pod(value);
            }
        }
        template <typename Map>
        static void loadPodMap(ps2_state::Reader &r, Map &map)
        {
            map.clear();
            const uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok(); ++i)
            {
                typename Map::key_type key{};
                typename Map::mapped_type value{};
                r.pod(key);
                r.pod(value);
                map.emplace(key, value);
            }
        }

        static void save(ps2_state::Writer &w, const IopMemory &m)
        {
            w.u64(m.m_ram.size());
            w.bytes(m.m_ram.data(), m.m_ram.size());
            w.u64(m.m_owned.size());
            w.bytes(m.m_owned.data(), m.m_owned.size());
            w.u64(m.m_scratch.size());
            w.bytes(m.m_scratch.data(), m.m_scratch.size());
            savePodMap(w, m.m_hardware);
            w.podVector(m.m_allocations);
            w.u32(m.m_heapCursor);
            w.u32(m.m_interruptStatus);
            w.u32(m.m_interruptMask);
            w.u32(m.m_interruptControl);
        }
        static void load(ps2_state::Reader &r, IopMemory &m)
        {
            r.blob(m.m_ram.data(), m.m_ram.size());
            r.blob(m.m_owned.data(), m.m_owned.size());
            r.blob(m.m_scratch.data(), m.m_scratch.size());
            loadPodMap(r, m.m_hardware);
            r.podVector(m.m_allocations);
            m.m_heapCursor = r.u32();
            m.m_interruptStatus = r.u32();
            m.m_interruptMask = r.u32();
            m.m_interruptControl = r.u32();
            m.m_dmaStart.reset();
        }

        static void save(ps2_state::Writer &w, const IopKernel &k)
        {
            savePodMap(w, k.m_threads);
            savePodMap(w, k.m_semaphores);
            savePodMap(w, k.m_eventFlags);
            w.u32(k.m_nextThreadId);
            w.u32(k.m_nextSemaphoreId);
            w.u32(k.m_nextEventFlagId);
        }
        static void load(ps2_state::Reader &r, IopKernel &k)
        {
            loadPodMap(r, k.m_threads);
            loadPodMap(r, k.m_semaphores);
            loadPodMap(r, k.m_eventFlags);
            k.m_nextThreadId = r.u32();
            k.m_nextSemaphoreId = r.u32();
            k.m_nextEventFlagId = r.u32();
            k.m_currentThread = nullptr;
        }

        static void save(ps2_state::Writer &w, const IopImportRegistry &imports)
        {
            w.u32(static_cast<uint32_t>(imports.m_libraries.size()));
            for (const auto &[address, lib] : imports.m_libraries)
            {
                w.u32(address);
                w.u32(lib.tableAddress);
                w.pod(lib.version);
                w.str(lib.name);
                w.podVector(lib.functions);
            }
        }
        static void load(ps2_state::Reader &r, IopImportRegistry &imports)
        {
            imports.m_libraries.clear();
            const uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && r.ok(); ++i)
            {
                const uint32_t address = r.u32();
                IopImportRegistry::ExportLibrary lib;
                lib.tableAddress = r.u32();
                r.pod(lib.version);
                lib.name = r.str();
                r.podVector(lib.functions);
                imports.m_libraries.emplace(address, std::move(lib));
            }
        }

        static void save(ps2_state::Writer &w, const IopIntrman &intr)
        {
            savePodMap(w, intr.m_handlers);
            savePodMap(w, intr.m_enabled);
        }
        static void load(ps2_state::Reader &r, IopIntrman &intr)
        {
            loadPodMap(r, intr.m_handlers);
            loadPodMap(r, intr.m_enabled);
        }

        static void save(ps2_state::Writer &w, const IopVblank &vb)
        {
            w.podVector(vb.m_handlers);
            w.u64(vb.m_nextStart);
            w.u64(vb.m_nextEnd);
        }
        static void load(ps2_state::Reader &r, IopVblank &vb)
        {
            r.podVector(vb.m_handlers);
            vb.m_nextStart = r.u64();
            vb.m_nextEnd = r.u64();
            vb.m_servicing = false;
        }

        static void save(ps2_state::Writer &w, const IopTimrman &t)
        {
            w.pod(t.m_timers);
            w.podVector(t.m_pendingIrqs);
            w.u32(t.m_holdMode);
        }
        static void load(ps2_state::Reader &r, IopTimrman &t)
        {
            r.pod(t.m_timers);
            r.podVector(t.m_pendingIrqs);
            t.m_holdMode = r.u32();
            t.m_servicing = false;
        }

        static void save(ps2_state::Writer &w, const IopIoman &io)
        {
            w.u32(static_cast<uint32_t>(io.m_devices.size()));
            for (const auto &d : io.m_devices)
            {
                w.u32(d.address);
                w.u32(d.gp);
                w.str(d.name);
            }
            w.u32(static_cast<uint32_t>(io.m_files.size()));
            for (const auto &[fd, f] : io.m_files)
            {
                w.i32(fd);
                w.u64(f.size);
                w.u64(f.position);
                w.str(f.path);
            }
            w.i32(io.m_nextFd);
        }
        static void load(ps2_state::Reader &r, IopIoman &io)
        {
            io.m_devices.clear();
            const uint32_t devices = r.u32();
            for (uint32_t i = 0; i < devices && r.ok(); ++i)
            {
                IopIoman::Device d;
                d.address = r.u32();
                d.gp = r.u32();
                d.name = r.str();
                io.m_devices.push_back(std::move(d));
            }
            // Host handles belong to this process: close ours, reopen the saved files by path (reads are positional).
            for (auto &[fd, f] : io.m_files)
                if (f.handle != 0u)
                    io.m_host.closeHostFile(f.handle);
            io.m_files.clear();
            const uint32_t files = r.u32();
            for (uint32_t i = 0; i < files && r.ok(); ++i)
            {
                const int32_t fd = r.i32();
                IopIoman::OpenFile f;
                f.size = r.u64();
                f.position = r.u64();
                f.path = r.str();
                const std::string translated = io.m_host.translateGuestPath(f.path);
                f.handle = io.m_host.openHostFile(translated.empty() ? f.path : translated);
                if (f.handle == 0u)
                    io.m_host.log(LogLevel::Warning, "[IOP] save state: cannot reopen " + f.path);
                io.m_files.emplace(fd, std::move(f));
            }
            io.m_nextFd = r.i32();
        }

        static void save(ps2_state::Writer &w, const IopRpcBridge &rpc)
        {
            savePodMap(w, rpc.m_servers);
            savePodMap(w, rpc.m_cmdHandlers);
            w.pod(rpc.m_sregs);
            w.u32(rpc.m_nextDmaId);
            w.boolean(rpc.m_sifInitialized);
        }
        static void load(ps2_state::Reader &r, IopRpcBridge &rpc)
        {
            loadPodMap(r, rpc.m_servers);
            loadPodMap(r, rpc.m_cmdHandlers);
            r.pod(rpc.m_sregs);
            rpc.m_nextDmaId = r.u32();
            rpc.m_sifInitialized = r.boolean();
        }

        static void save(ps2_state::Writer &w, const IopModuleManager &mm)
        {
            w.u32(static_cast<uint32_t>(mm.m_records.size()));
            for (const auto &[id, rec] : mm.m_records)
            {
                w.i32(id);
                w.str(rec.key);
                w.u32(rec.references);
                w.boolean(rec.physical);
            }
            w.u32(static_cast<uint32_t>(mm.m_hleIdsByKey.size()));
            for (const auto &[key, id] : mm.m_hleIdsByKey)
            {
                w.str(key);
                w.i32(id);
            }
            w.u32(static_cast<uint32_t>(mm.m_loadedKeyReferences.size()));
            for (const auto &[key, refs] : mm.m_loadedKeyReferences)
            {
                w.str(key);
                w.u32(refs);
            }
            w.i32(mm.m_nextHleId);
        }
        static void load(ps2_state::Reader &r, IopModuleManager &mm)
        {
            mm.m_records.clear();
            const uint32_t records = r.u32();
            for (uint32_t i = 0; i < records && r.ok(); ++i)
            {
                const int32_t id = r.i32();
                IopModuleManager::Record rec;
                rec.key = r.str();
                rec.references = r.u32();
                rec.physical = r.boolean();
                mm.m_records.emplace(id, std::move(rec));
            }
            mm.m_hleIdsByKey.clear();
            const uint32_t ids = r.u32();
            for (uint32_t i = 0; i < ids && r.ok(); ++i)
            {
                std::string key = r.str();
                mm.m_hleIdsByKey[key] = r.i32();
            }
            mm.m_loadedKeyReferences.clear();
            const uint32_t refs = r.u32();
            for (uint32_t i = 0; i < refs && r.ok(); ++i)
            {
                std::string key = r.str();
                mm.m_loadedKeyReferences[key] = r.u32();
            }
            mm.m_nextHleId = r.i32();
        }
    };

    void saveModuleManagerState(const IopModuleManager &manager, ps2_state::Writer &w) { IopStateAccess::save(w, manager); }
    void loadModuleManagerState(IopModuleManager &manager, ps2_state::Reader &r) { IopStateAccess::load(r, manager); }

    std::string IopEmulator::stateBlocker() const
    {
        const Impl &d = *m_impl;
        if (d.activeCpu != nullptr || d.callDepth != 0u)
            return "the IOP is running a guest call";
        if (d.servicingDmaInterrupts || d.servicingGuestCallbacks)
            return "the IOP is servicing interrupts";
        if (IopStateAccess::threadRunning(d.kernel))
            return "an IOP thread is mid-timeslice";
        if (!d.cdvd.stateIdle())
            return "an IOP CD read callback is pending";
        return {};
    }

    void IopEmulator::saveState(ps2_state::Writer &w) const
    {
        const Impl &d = *m_impl;
        IopStateAccess::save(w, d.memory);
        IopStateAccess::save(w, d.kernel);
        IopStateAccess::save(w, d.imports);
        IopStateAccess::save(w, d.intrman);
        IopStateAccess::save(w, d.vblank);
        IopStateAccess::save(w, d.timrman);
        IopStateAccess::save(w, d.ioman);
        IopStateAccess::save(w, d.rpc);
        d.cdvd.saveState(w);
        w.u32(static_cast<uint32_t>(d.modules.size()));
        for (const auto &[id, m] : d.modules)
        {
            w.i32(m.id);
            w.str(m.path);
            w.str(m.name);
            w.u32(m.base);
            w.u32(m.size);
            w.u32(m.entry);
            w.u32(m.gp);
            w.boolean(m.resident);
        }
        IopStateAccess::savePodMap(w, d.pendingDmaInterrupts);
        w.u32(static_cast<uint32_t>(d.pendingGuestCallbacks.size()));
        for (const auto &[due, cb] : d.pendingGuestCallbacks)
        {
            w.u64(due);
            w.pod(cb);
        }
        w.u32(d.nextModuleId);
        w.u32(d.moduleCursor);
        w.u64(d.totalCycles);
        w.u64(d.totalInstructions);
        w.u64(d.eeCycleCarry);
        w.u64(d.iopTimeTarget);
        w.pod(d.secrMcCommandHandler);
        w.pod(d.secrMcDevIdHandler);
        w.pod(d.checkKelfPathCallback);
        spu2::saveState(w);
    }

    void IopEmulator::loadState(ps2_state::Reader &r)
    {
        Impl &d = *m_impl;
        IopStateAccess::load(r, d.memory);
        IopStateAccess::load(r, d.kernel);
        IopStateAccess::load(r, d.imports);
        IopStateAccess::load(r, d.intrman);
        IopStateAccess::load(r, d.vblank);
        IopStateAccess::load(r, d.timrman);
        IopStateAccess::load(r, d.ioman);
        IopStateAccess::load(r, d.rpc);
        d.cdvd.loadState(r);
        d.modules.clear();
        const uint32_t modules = r.u32();
        for (uint32_t i = 0; i < modules && r.ok(); ++i)
        {
            Impl::Module m;
            m.id = r.i32();
            m.path = r.str();
            m.name = r.str();
            m.base = r.u32();
            m.size = r.u32();
            m.entry = r.u32();
            m.gp = r.u32();
            m.resident = r.boolean();
            d.modules.emplace(m.id, std::move(m));
        }
        IopStateAccess::loadPodMap(r, d.pendingDmaInterrupts);
        d.pendingGuestCallbacks.clear();
        const uint32_t callbacks = r.u32();
        for (uint32_t i = 0; i < callbacks && r.ok(); ++i)
        {
            const uint64_t due = r.u64();
            Impl::ScheduledGuestCallback cb{};
            r.pod(cb);
            d.pendingGuestCallbacks.emplace(due, cb);
        }
        d.nextModuleId = r.u32();
        d.moduleCursor = r.u32();
        d.totalCycles = r.u64();
        d.totalInstructions = r.u64();
        d.eeCycleCarry = r.u64();
        d.iopTimeTarget = r.u64();
        r.pod(d.secrMcCommandHandler);
        r.pod(d.secrMcDevIdHandler);
        r.pod(d.checkKelfPathCallback);
        d.activeCpu = nullptr;
        d.servicingDmaInterrupts = false;
        d.servicingGuestCallbacks = false;
        d.callDepth = 0u;
        spu2::loadState(r);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // DIAGNOSTIC (PS2X_IOP_DIAG=<interval ms of IOP time>, default off).
    // ZOE's IOP_MAIN (Konami sound driver) refreshes the EE status block from IOP 0x6adcc from a 5 ms thread. This
    // watches the block's time word (+0x14); every <interval> ms and at the moment the word stops advancing it logs
    // every IOP thread (state, pc, ra, wait), the hardware timers, interrupt handlers / counts, semaphores, event
    // flags and the dispatcher flags, prefixed [iopdiag]. No behaviour change.
    // -----------------------------------------------------------------------------------------------------------------
    void IopEmulator::Impl::diagDump(const char *why)
    {
        constexpr uint32_t kStatusBlock = 0x6adcc;
        constexpr uint32_t kSoundArgBlock = 0x6abc0; // IOP_MAIN's cid-1 handler argument (EE status dst at +0x22c)
        const uint64_t tick = ps2CurrentVsyncTick();
        char line[512];
        const auto where = [&](uint32_t address) -> std::string
        {
            const uint32_t phys = address & 0x1FFFFFFFu;
            for (const auto &[id, m] : modules)
            {
                if (phys >= m.base && phys < m.base + m.size)
                {
                    char text[96];
                    std::snprintf(text, sizeof(text), "%s+0x%x", m.name.c_str(), phys - m.base);
                    return text;
                }
            }
            char text[32];
            std::snprintf(text, sizeof(text), "0x%x", address);
            return text;
        };
        const auto emit = [&]()
        { log(LogLevel::Info, line); };

        // IOP_MAIN data region 0x6aa90..0x6ac90 (stream state machine arrays: core 0/1 state at 0x6ac48/0x6ac4c, pending
        // command 0x6aaf0/4, pause flags 0x6ab38/c, time words 0x6ab28/c ...), 8 words per row, rows that are all zero skipped.
        for (uint32_t base = 0x6aa90; base < 0x6ac90; base += 32u)
        {
            uint32_t row[8]{};
            (void)memory.readRam(base, row, sizeof(row));
            bool any = false;
            for (const uint32_t v : row)
                any = any || v != 0u;
            if (!any)
                continue;
            std::snprintf(line, sizeof(line), "[iopdiag] mem %x: %x %x %x %x %x %x %x %x", base, row[0], row[1], row[2], row[3], row[4],
                          row[5], row[6], row[7]);
            emit();
        }

        uint32_t block[8]{};
        (void)memory.readRam(kStatusBlock, block, sizeof(block));
        std::snprintf(line, sizeof(line),
                      "[iopdiag] ==== %s tick=%llu cyc=%llu instr=%llu | status block (IOP) %x %x %x %x %x %x %x %x eeDst=0x%x | irqMask=0x%x irqStat=0x%x irqCtl=%u pendingDma=%zu pendingCb=%zu servDma=%d servCb=%d callDepth=%u cbUnfinished=%u/%u",
                      why, static_cast<unsigned long long>(tick), static_cast<unsigned long long>(totalCycles),
                      static_cast<unsigned long long>(totalInstructions), block[0], block[1], block[2], block[3], block[4], block[5],
                      block[6], block[7], memory.read32(kSoundArgBlock + 0x22cu), memory.interruptMask(), memory.interruptStatus(),
                      memory.interruptControl(), pendingDmaInterrupts.size(), pendingGuestCallbacks.size(),
                      servicingDmaInterrupts ? 1 : 0, servicingGuestCallbacks ? 1 : 0, callDepth, diagCbUnfinished, diagCbTotal);
        emit();

        static const char *const kStates[] = {"Dormant", "Ready", "Running", "Sleep", "Delay", "Semaphore", "EventFlag", "Suspended", "Dead"};
        const IopThread *current = IopStateAccess::diagCurrent(kernel);
        for (const auto &[id, t] : IopStateAccess::diagThreads(kernel))
        {
            std::snprintf(line, sizeof(line),
                          "[iopdiag] thr %d%s entry=%s prio=%u state=%s pc=%s ra=%s sp=0x%x wakeCyc=%llu waitId=%d wakeups=%d",
                          t.id, &t == current ? "*" : "", where(t.entry).c_str(), t.priority,
                          kStates[std::min<size_t>(static_cast<size_t>(t.state), 8u)], where(t.cpu.pc).c_str(),
                          where(t.cpu.gpr[31]).c_str(), t.cpu.gpr[29], static_cast<unsigned long long>(t.wakeCycle), t.waitId,
                          t.wakeupCount);
            emit();
        }
        for (const auto &[id, s] : IopStateAccess::diagSemaphores(kernel))
        {
            std::snprintf(line, sizeof(line), "[iopdiag] sema %d count=%d/%d", s.id, s.current, s.maximum);
            emit();
        }
        for (const auto &[id, e] : IopStateAccess::diagEventFlags(kernel))
        {
            std::snprintf(line, sizeof(line), "[iopdiag] evflg %d bits=0x%x", e.id, e.bits);
            emit();
        }
        for (const auto &[id, t] : IopStateAccess::diagThreads(kernel))
        {
            if (id < 0 || id >= 64)
                continue;
            std::snprintf(line, sizeof(line),
                          "[iopdiag] thrstat %d runs=%llu sleepCalls=%llu wakeCalls=%llu wakeFoundSleeping=%llu (wakeBadId=%llu)", id,
                          static_cast<unsigned long long>(diagRuns[id]), static_cast<unsigned long long>(diagSleepCalls[id]),
                          static_cast<unsigned long long>(diagWakeCalls[id]), static_cast<unsigned long long>(diagWakeWhileSleep[id]),
                          static_cast<unsigned long long>(diagWakeBadId));
            emit();
        }
        int ti = 0;
        for (const auto &t : IopStateAccess::diagTimers(timrman))
        {
            if (t.users != 0u)
            {
                std::snprintf(line, sizeof(line),
                              "[iopdiag] timer %d irq=%u running=%d mode=0x%x compare=%u nextCompareCyc=%llu cbFn=%s",
                              ti, t.irq, t.running ? 1 : 0, t.liveMode, t.compare,
                              t.compareCycle == UINT64_MAX ? 0ull : static_cast<unsigned long long>(t.compareCycle),
                              where(t.compareCallback.function).c_str());
                emit();
            }
            ++ti;
        }
        for (const auto &[irq, h] : IopStateAccess::diagHandlers(intrman))
        {
            const auto en = IopStateAccess::diagEnabled(intrman).find(irq);
            const uint32_t raised = irq >= 0 && irq < 64 ? diagIrqCount[irq] : 0u;
            const uint32_t delivered = irq >= 0 && irq < 64 ? diagIrqDelivered[irq] : 0u;
            std::snprintf(line, sizeof(line), "[iopdiag] irq %d handler=%s enabled=%d raised=%u delivered=%u", irq,
                          where(h.function).c_str(), en != IopStateAccess::diagEnabled(intrman).end() && en->second ? 1 : 0, raised,
                          delivered);
            emit();
        }
    }

    static std::function<void(uint32_t, size_t)> s_diagWatch;

    void diagWatchHit(uint32_t address, size_t size)
    {
        if (s_diagWatch)
            s_diagWatch(address, size);
    }

    void IopEmulator::Impl::diagAttach()
    {
        if (g_iopWatchCount == 0u)
            return;
        // Logs only when a write actually changed a word of the watched range (snapshot diff, up to 6 words per line);
        // the first write to a range just takes the snapshot. Writer = the IOP cpu (pc/ra/thread) or host/EE.
        s_diagWatch = [this](uint32_t address, size_t size)
        {
            static uint32_t s_logged = 0u;
            static std::vector<uint32_t> s_snapshot[4];
            static bool s_have[4]{};
            if (s_logged >= 600u)
                return;
            for (uint32_t i = 0; i < g_iopWatchCount; ++i)
            {
                if (!(address < g_iopWatchHi[i] && address + size > g_iopWatchLo[i]))
                    continue;
                const uint32_t lo = g_iopWatchLo[i];
                const uint32_t count = (g_iopWatchHi[i] - lo + 3u) / 4u;
                std::vector<uint32_t> now(count, 0u);
                (void)memory.readRam(lo, now.data(), count * 4u);
                if (!s_have[i])
                {
                    s_have[i] = true;
                    s_snapshot[i] = now;
                    continue;
                }
                char line[600];
                int len = 0;
                uint32_t changed = 0u;
                for (uint32_t w = 0; w < count; ++w)
                {
                    if (now[w] == s_snapshot[i][w])
                        continue;
                    if (changed < 6u)
                        len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " [0x%x]=%x->%x", lo + w * 4u,
                                             s_snapshot[i][w], now[w]);
                    ++changed;
                }
                s_snapshot[i] = std::move(now);
                if (changed == 0u)
                    continue;
                ++s_logged;
                const IopThread *current = IopStateAccess::diagCurrent(kernel);
                char head[256];
                std::snprintf(head, sizeof(head),
                              "[iopdiag] CHG tick=%llu cyc=%llu range%u by %s pc=0x%x ra=0x%x thread=%d depth=%u servDma=%d servCb=%d (%u words):",
                              static_cast<unsigned long long>(ps2CurrentVsyncTick()), static_cast<unsigned long long>(totalCycles), i,
                              activeCpu ? "IOP cpu" : "host/EE", activeCpu ? activeCpu->pc : 0u, activeCpu ? activeCpu->gpr[31] : 0u,
                              current ? current->id : -1, callDepth, servicingDmaInterrupts ? 1 : 0, servicingGuestCallbacks ? 1 : 0,
                              changed);
                std::string text = head;
                text.append(line, static_cast<size_t>(len));
                log(LogLevel::Info, text);
            }
        };
        g_iopWatchHook = &diagWatchHit;
    }

    // SleepThread / [i]WakeupThread bookkeeping (called before the kernel handles the import).
    void IopEmulator::Impl::diagImportCall(uint16_t ordinal, const CpuState &cpu)
    {
        const IopThread *current = IopStateAccess::diagCurrent(kernel);
        if (ordinal == 24u)
        {
            if (current && current->id >= 0 && current->id < 64)
                ++diagSleepCalls[current->id];
        }
        else if (ordinal == 25u || ordinal == 26u)
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto &threads = IopStateAccess::diagThreads(kernel);
            const auto it = threads.find(id);
            if (it == threads.end())
            {
                ++diagWakeBadId;
                if (diagWakeBadId <= 20u)
                {
                    char line[200];
                    std::snprintf(line, sizeof(line),
                                  "[iopdiag] tick=%llu cyc=%llu WakeupThread(%d) bad thread id, ra=0x%x current=%d",
                                  static_cast<unsigned long long>(ps2CurrentVsyncTick()), static_cast<unsigned long long>(totalCycles), id,
                                  cpu.gpr[31], current ? current->id : -1);
                    log(LogLevel::Info, line);
                }
                return;
            }
            if (id >= 0 && id < 64)
            {
                ++diagWakeCalls[id];
                if (it->second.state == IopThreadState::Sleep)
                    ++diagWakeWhileSleep[id];
            }
        }
    }

    void IopEmulator::Impl::diagTick()
    {
        constexpr uint32_t kStatusTimeWord = 0x6adcc + 0x14;
        if (totalCycles < diagNextCycle)
            return;
        diagNextCycle = totalCycles + kIopClockHz / 1000u; // look every ~1 ms of IOP time
        const uint32_t statusTime = memory.read32(kStatusTimeWord);
        if (statusTime != diagLastStatusTime)
        {
            diagLastStatusTime = statusTime;
            diagLastChangeCycle = totalCycles;
            diagLastChangeTick = ps2CurrentVsyncTick();
            diagStallDumps = 0u;
        }
        else if (totalCycles - diagLastChangeCycle > kIopClockHz / 5u) // the word has not moved for 200 ms
        {
            static const uint64_t kRepeatCycles[] = {0u, kIopClockHz * 1u, kIopClockHz * 3u};
            if (diagStallDumps < 3u && totalCycles - diagLastChangeCycle >= kRepeatCycles[diagStallDumps] + kIopClockHz / 5u)
            {
                ++diagStallDumps;
                char why[160];
                std::snprintf(why, sizeof(why), "STALL#%u status time word 0x%x last changed at cyc=%llu tick=%llu", diagStallDumps,
                              statusTime, static_cast<unsigned long long>(diagLastChangeCycle),
                              static_cast<unsigned long long>(diagLastChangeTick));
                diagDump(why);
            }
        }
        static uint64_t s_nextPeriodic = 0u;
        if (totalCycles >= s_nextPeriodic)
        {
            s_nextPeriodic = totalCycles + static_cast<uint64_t>(kIopDiagIntervalMs) * kIopClockHz / 1000u;
            diagDump("periodic");
        }
    }
}
