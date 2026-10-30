#pragma once

#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopKernel;

    struct IopStateAccess; // save states (iop_emulator.cpp)

    class IopVblank
    {
        friend struct IopStateAccess;

    public:
        explicit IopVblank(IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);

        // Calls the handlers registered with RegisterVblankHandler at each vblank start/end edge.
        void serviceDue(uint64_t currentCycle, IopGuestExecutor &executor);
        [[nodiscard]] uint64_t nextEventCycle(uint64_t fallback) const noexcept;

    private:
        struct Handler
        {
            uint32_t edge = 0u; // 0 = vblank start, 1 = vblank end
            int32_t priority = 0;
            uint32_t function = 0u;
            uint32_t argument = 0u;
            uint32_t gp = 0u;
        };

        void callHandlers(uint32_t edge, IopGuestExecutor &executor);

        IopKernel &m_kernel;
        std::vector<Handler> m_handlers;
        uint64_t m_nextStart = 0u;
        uint64_t m_nextEnd = 0u;
        bool m_servicing = false;
    };
}
