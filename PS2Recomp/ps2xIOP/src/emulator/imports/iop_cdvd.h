#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2_state
{
    class Writer;
    class Reader;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

        // Save states. The disc image handle and the virtual ISO index stay as they are (same disc).
        [[nodiscard]] bool stateIdle() const noexcept;
        void saveState(ps2_state::Writer &w) const;
        void loadState(ps2_state::Reader &r);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
