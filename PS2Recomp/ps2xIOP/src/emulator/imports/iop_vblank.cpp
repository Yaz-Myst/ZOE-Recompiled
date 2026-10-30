#include "iop_vblank.h"
#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <cstdio>

namespace ps2x::iop::detail
{
    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    void IopVblank::reset()
    {
        m_handlers.clear();
        m_nextStart = 0u;
        m_nextEnd = 0u;
        m_servicing = false;
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler(edge, priority, handler, arg)
        {
            Handler handler{cpu.gpr[4] & 1u, static_cast<int32_t>(cpu.gpr[5]), cpu.gpr[6], cpu.gpr[7], cpu.gpr[28]};
            if (handler.function == 0u)
            {
                cpu.gpr[2] = static_cast<uint32_t>(-1);
                return true;
            }
            // Keep priority order (lower value runs first), stable for equal priorities.
            const auto at = std::upper_bound(m_handlers.begin(), m_handlers.end(), handler,
                                             [](const Handler &a, const Handler &b)
                                             { return a.priority < b.priority; });
            m_handlers.insert(at, handler);
            if (m_nextStart == 0u)
            {
                // Same field grid as the WaitVblank* imports above.
                const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
                m_nextStart = fieldStart + kVblankPeriodCycles;
                m_nextEnd = fieldStart + kVblankEndPhaseCycles;
                if (m_nextEnd <= currentCycle)
                    m_nextEnd += kVblankPeriodCycles;
            }
            cpu.gpr[2] = 0u;
            return true;
        }
        case 9: // ReleaseVblankHandler(edge, handler)
        {
            const uint32_t edge = cpu.gpr[4] & 1u;
            const uint32_t function = cpu.gpr[5];
            const auto it = std::find_if(m_handlers.begin(), m_handlers.end(), [&](const Handler &h)
                                         { return h.edge == edge && h.function == function; });
            if (it != m_handlers.end())
                m_handlers.erase(it);
            cpu.gpr[2] = 0u;
            return true;
        }
        default:
            return false;
        }
    }

    void IopVblank::callHandlers(uint32_t edge, IopGuestExecutor &executor)
    {
        // Copy: a handler may register or release handlers while it runs.
        const std::vector<Handler> handlers = m_handlers;
        for (const Handler &handler : handlers)
        {
            if (handler.edge != edge)
                continue;
            const uint32_t result = executor.executeGuestFunctionWithBudget(handler.function, handler.argument, 0u, 0u, 0u,
                                                                            handler.gp, 100000u);
            if (result == 0u)
            {
                // IOP SDK convention: a handler returning 0 is unregistered.
                const auto it = std::find_if(m_handlers.begin(), m_handlers.end(), [&](const Handler &h)
                                             { return h.edge == edge && h.function == handler.function; });
                if (it != m_handlers.end())
                {
                    m_handlers.erase(it);
                    std::printf("[iop:vblank] handler 0x%x returned 0; unregistered\n", handler.function);
                    std::fflush(stdout);
                }
            }
        }
    }

    void IopVblank::serviceDue(uint64_t currentCycle, IopGuestExecutor &executor)
    {
        if (m_servicing || m_handlers.empty())
            return;
        m_servicing = true;
        struct ServiceGuard
        {
            bool &flag;
            ~ServiceGuard() { flag = false; }
        } guard{m_servicing};

        // If the IOP fell behind, deliver one edge and move to the next future field rather than
        // replaying every missed vblank.
        if (m_nextStart <= currentCycle)
        {
            callHandlers(0u, executor);
            m_nextStart += kVblankPeriodCycles;
            if (m_nextStart <= currentCycle)
                m_nextStart = currentCycle - (currentCycle % kVblankPeriodCycles) + kVblankPeriodCycles;
        }
        if (m_nextEnd <= currentCycle)
        {
            callHandlers(1u, executor);
            m_nextEnd += kVblankPeriodCycles;
            if (m_nextEnd <= currentCycle)
                m_nextEnd = currentCycle - (currentCycle % kVblankPeriodCycles) + kVblankPeriodCycles + kVblankEndPhaseCycles;
        }
    }

    uint64_t IopVblank::nextEventCycle(uint64_t fallback) const noexcept
    {
        if (m_handlers.empty())
            return fallback;
        return std::min({fallback, m_nextStart, m_nextEnd});
    }
}
