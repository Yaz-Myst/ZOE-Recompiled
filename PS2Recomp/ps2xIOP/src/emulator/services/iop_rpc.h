#pragma once

#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopGuestExecutor
    {
    public:
        virtual ~IopGuestExecutor() = default;

        [[nodiscard]] virtual uint32_t executeGuestFunction(uint32_t address,
                                                            uint32_t a0,
                                                            uint32_t a1,
                                                            uint32_t a2,
                                                            uint32_t a3,
                                                            uint32_t gp) = 0;
        [[nodiscard]] virtual uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                                      uint32_t a0,
                                                                      uint32_t a1,
                                                                      uint32_t a2,
                                                                      uint32_t a3,
                                                                      uint32_t gp,
                                                                      uint32_t instructionBudget)
        {
            return executeGuestFunction(address, a0, a1, a2, a3, gp);
        }
    };

    struct IopStateAccess; // save states (iop_emulator.cpp)

    class IopRpcBridge
    {
        friend struct IopStateAccess;

    public:
        IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept;

        void reset();
        [[nodiscard]] bool dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request, IopGuestExecutor &executor);
        void onSifTransfer(const SifTransfer &transfer);
        void removeServersInRange(uint32_t base, uint32_t size);

        // IOP sifcmd command handlers (sceSifAddCmdHandler) and software registers.
        struct CmdHandler
        {
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t data = 0;
        };
        [[nodiscard]] std::optional<CmdHandler> findCmdHandler(uint32_t commandId) const;
        // System commands from the EE that sifcmd itself services (SIF_CMD_SET_SREG). True when consumed.
        [[nodiscard]] bool applySystemCommand(uint32_t commandId, const void *packet, size_t packetSize);

        [[nodiscard]] bool hasServer(uint32_t sid) const noexcept;
        [[nodiscard]] size_t serverCount() const noexcept { return m_servers.size(); }

    private:
        struct RpcServer
        {
            uint32_t sid = 0;
            uint32_t serverData = 0;
            uint32_t function = 0;
            uint32_t gp = 0;
            uint32_t buffer = 0;
            uint32_t callback = 0;
            uint32_t callbackBuffer = 0;
            uint32_t queue = 0;
        };

        IopHost &m_host;
        IopMemory &m_memory;
        IopKernel &m_kernel;
        std::unordered_map<uint32_t, RpcServer> m_servers;
        std::unordered_map<uint32_t, CmdHandler> m_cmdHandlers;
        std::array<uint32_t, 32> m_sregs{};
        uint32_t m_nextDmaId = 1u;
        bool m_sifInitialized = false;
    };
}
