#include "iop_rpc.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2x::iop::detail
{
    IopRpcBridge::IopRpcBridge(IopHost &host, IopMemory &memory, IopKernel &kernel) noexcept
        : m_host(host), m_memory(memory), m_kernel(kernel)
    {
    }

    void IopRpcBridge::reset()
    {
        m_servers.clear();
        m_cmdHandlers.clear();
        m_sregs.fill(0u);
        m_nextDmaId = 1u;
        m_sifInitialized = false;
    }

    std::optional<IopRpcBridge::CmdHandler> IopRpcBridge::findCmdHandler(uint32_t commandId) const
    {
        const auto it = m_cmdHandlers.find(commandId);
        if (it == m_cmdHandlers.end() || it->second.function == 0u)
            return std::nullopt;
        return it->second;
    }

    bool IopRpcBridge::applySystemCommand(uint32_t commandId, const void *packet, size_t packetSize)
    {
        constexpr uint32_t kSifCmdSetSreg = 0x80000001u;
        constexpr size_t kHeaderSize = 16u;
        if (commandId != kSifCmdSetSreg || !packet || packetSize < kHeaderSize + 8u)
            return false;
        uint32_t index = 0u;
        uint32_t value = 0u;
        std::memcpy(&index, static_cast<const uint8_t *>(packet) + kHeaderSize, sizeof(index));
        std::memcpy(&value, static_cast<const uint8_t *>(packet) + kHeaderSize + 4u, sizeof(value));
        if (index < m_sregs.size())
            m_sregs[index] = value;
        return true;
    }

    bool IopRpcBridge::dispatchSifManImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 4: // sceSifDma2Init
        case 5: // sceSifInit
            m_sifInitialized = true;
            setV0(0u);
            return true;
        case 7: // sceSifSetDma
        {
            constexpr uint32_t kDescriptorSize = 16u;
            constexpr uint32_t kMaxDescriptors = 32u;
            const uint32_t descriptorAddress = cpu.gpr[4];
            const uint32_t descriptorCount = cpu.gpr[5];
            // DIAGNOSTIC (PS2X_SIF_LOG=<hex lo>,<hex hi>): every rejected call,
            // and every transfer whose EE destination touches [lo, hi) (missing 32 KB in a ZOE motion file, 24 Oct).
            static const std::array<uint32_t, 3> s_sifLog = []
            {
                std::array<uint32_t, 3> r{0u, 0u, 0u};
                if (const char *env = std::getenv("PS2X_SIF_LOG"))
                {
                    r[2] = 1u; // "<lo>,<hi>": transfers touching that EE range; anything else: every transfer >= 0x800
                    if (std::sscanf(env, "%x,%x", &r[0], &r[1]) != 2)
                        r[0] = r[1] = 0u;
                }
                return r;
            }();
            const auto reject = [&](const char *why, uint32_t a, uint32_t b)
            {
                if (s_sifLog[2] != 0u)
                {
                    char line[160];
                    std::snprintf(line, sizeof(line), "[IOP sif:dma] REJECTED %s desc=0x%x count=%u (0x%x, 0x%x)", why,
                                  descriptorAddress, descriptorCount, a, b);
                    m_host.log(LogLevel::Info, line);
                }
                setV0(0u);
                return true;
            };
            if (descriptorAddress == 0u || descriptorCount == 0u || descriptorCount > kMaxDescriptors)
                return reject("descriptor list", descriptorAddress, descriptorCount);

            struct PendingTransfer
            {
                uint32_t source = 0u;
                uint32_t destination = 0u;
                uint32_t size = 0u;
            };

            std::array<uint32_t, kMaxDescriptors * 4u> descriptorWords{};
            const size_t descriptorBytes = static_cast<size_t>(descriptorCount) * kDescriptorSize;
            if (!m_memory.readRam(descriptorAddress, descriptorWords.data(), descriptorBytes))
                return reject("descriptor read", descriptorAddress, static_cast<uint32_t>(descriptorBytes));

            std::array<PendingTransfer, kMaxDescriptors> pending{};
            uint32_t pendingCount = 0u;
            uint32_t largestTransfer = 0u;
            for (uint32_t i = 0u; i < descriptorCount; ++i)
            {
                const uint32_t source = descriptorWords[i * 4u + 0u];
                const uint32_t destination = descriptorWords[i * 4u + 1u];
                const int32_t signedSize = static_cast<int32_t>(descriptorWords[i * 4u + 2u]);
                if (signedSize <= 0)
                {
                    if (s_sifLog[2] != 0u)
                    {
                        char line[128];
                        std::snprintf(line, sizeof(line), "[IOP sif:dma] skipped size %d src=0x%x dst=0x%x", signedSize,
                                      source, destination);
                        m_host.log(LogLevel::Info, line);
                    }
                    continue;
                }

                const uint32_t size = static_cast<uint32_t>(signedSize);
                if (!m_memory.ownsRamRange(source, size))
                    return reject("source", source, size);
                pending[pendingCount++] = {source, destination, size};
                largestTransfer = std::max(largestTransfer, size);
            }

            // IOP-side sceSifSetDma sends IOP RAM to the EE. Validate all EE
            // destinations before committing any write so a bad chain cannot
            // partially update guest memory, but maybe we could skip this check if we trust the EE-side SIF driver to validate the chain ?!
            // TODO check later
            std::vector<uint8_t> scratch(largestTransfer);
            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_host.readGuest(transfer.destination, scratch.data(), transfer.size))
                    return reject("destination", transfer.destination, transfer.size);
            }

            for (uint32_t i = 0u; i < pendingCount; ++i)
            {
                const PendingTransfer &transfer = pending[i];
                if (!m_memory.readRam(transfer.source, scratch.data(), transfer.size) || !m_host.writeGuest(transfer.destination, scratch.data(), transfer.size))
                    return reject("copy", transfer.destination, transfer.size);
                const uint32_t dst = transfer.destination & 0x1FFFFFFFu;
                const bool inRange = s_sifLog[1] != 0u ? dst < s_sifLog[1] && dst + transfer.size > s_sifLog[0]
                                                       : transfer.size >= 0x800u;
                if (s_sifLog[2] != 0u && inRange)
                {
                    char line[128];
                    std::snprintf(line, sizeof(line), "[IOP sif:dma] id=%u #%u/%u src=0x%x dst=0x%x size=0x%x", m_nextDmaId, i,
                                  pendingCount, transfer.source, transfer.destination, transfer.size);
                    m_host.log(LogLevel::Info, line);
                }
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            {
                m_nextDmaId = 1u;
            }
            setV0(dmaId);
            return true;
        }
        case 8: // sceSifDmaStat
            setV0(0xFFFFFFFFu);
            return true;
        case 29: // sceSifCheckInit
            setV0(m_sifInitialized ? 1u : 0u);
            return true;
        default:
            setV0(0u);
            return true;
        }
    }

    bool IopRpcBridge::dispatchSifCmdImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        switch (ordinal)
        {
        case 6: // sceSifGetSreg
        {
            const uint32_t index = cpu.gpr[4];
            setV0(index < m_sregs.size() ? m_sregs[index] : 0u);
            return true;
        }
        case 7: // sceSifSetSreg (IOP-local software register; the EE writes it with SIF_CMD_SET_SREG)
        {
            const uint32_t index = cpu.gpr[4];
            const uint32_t value = cpu.gpr[5];
            if (index < m_sregs.size())
                m_sregs[index] = value;
            setV0(value);
            return true;
        }
        case 10: // sceSifAddCmdHandler(cid, handler, harg)
        {
            m_cmdHandlers[cpu.gpr[4]] = CmdHandler{cpu.gpr[5], cpu.gpr[28], cpu.gpr[6]};
            static uint32_t logged = 0u;
            if (logged < 32u)
            {
                ++logged;
                char line[96];
                std::snprintf(line, sizeof(line), "[IOP] sifcmd: handler for cid 0x%x = 0x%x (harg 0x%x)", cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
                m_host.log(LogLevel::Info, line);
            }
            setV0(0);
            return true;
        }
        case 11: // sceSifRemoveCmdHandler(cid)
            m_cmdHandlers.erase(cpu.gpr[4]);
            setV0(0);
            return true;
        case 4: // InitCmd
        case 5: // ExitCmd
        case 8: // SetCmdBuffer
        case 9: // SetSysCmdBuffer
        case 14: // InitRpc
        case 15:
        case 16:
            setV0(0);
            return true;
        case 12: // sceSifSendCmd
        case 13: // isceSifSendCmd
        {
            constexpr uint32_t kHeaderSize = 16u;
            constexpr uint32_t kMaxPacketSize = 112u;
            const uint32_t commandId = cpu.gpr[4];
            const uint32_t packetAddress = cpu.gpr[5];
            const uint32_t packetSize = cpu.gpr[6];
            const uint32_t extraSource = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            const uint32_t extraDestination = m_memory.read32(stackPointer + 16u);
            const int32_t signedExtraSize = static_cast<int32_t>(m_memory.read32(stackPointer + 20u));

            if (packetAddress == 0u || packetSize < kHeaderSize || packetSize > kMaxPacketSize ||
                !m_memory.ownsRamRange(packetAddress, packetSize))
            {
                setV0(0u);
                return true;
            }

            std::array<uint8_t, kMaxPacketSize> packet{};
            if (!m_memory.readRam(packetAddress, packet.data(), packetSize))
            {
                setV0(0u);
                return true;
            }

            uint32_t extraSize = 0u;
            if (signedExtraSize > 0)
            {
                extraSize = static_cast<uint32_t>(signedExtraSize);
                if (extraSource == 0u || extraDestination == 0u ||
                    !m_memory.ownsRamRange(extraSource, extraSize) ||
                    !m_host.writeGuest(extraDestination, m_memory.ram().data() + IopMemory::physicalAddress(extraSource), extraSize))
                {
                    setV0(0u);
                    return true;
                }
            }

            const uint32_t sizeWord = packetSize | (extraSize << 8u);
            std::memcpy(packet.data() + 0u, &sizeWord, sizeof(sizeWord));
            std::memcpy(packet.data() + 4u, &extraDestination, sizeof(extraDestination));
            std::memcpy(packet.data() + 8u, &commandId, sizeof(commandId));

            const bool delivered = m_host.sendSifCommand(commandId, packet.data(), packetSize);
            // A command without an EE handler is still a completed DMA on real hardware. Only malformed packets fail above.
            static uint32_t logged = 0u;
            if (logged < 32u)
            {
                ++logged;
                char line[112];
                std::snprintf(line, sizeof(line), "[IOP] sifcmd to EE: cid=0x%x size=0x%x extra=0x%x -> 0x%x (%s)",
                              commandId, packetSize, extraSize, extraDestination, delivered ? "EE handler queued" : "no EE handler");
                m_host.log(LogLevel::Info, line);
            }

            const uint32_t dmaId = m_nextDmaId++;
            if (m_nextDmaId == 0u || m_nextDmaId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                m_nextDmaId = 1u;
            setV0(dmaId);
            return true;
        }
        case 17: // sceSifRegisterRpc
        {
            RpcServer server;
            server.serverData = cpu.gpr[4];
            server.sid = cpu.gpr[5];
            server.function = cpu.gpr[6];
            server.gp = cpu.gpr[28];
            server.buffer = cpu.gpr[7];
            const uint32_t stackPointer = cpu.gpr[29];
            server.callback = m_memory.read32(stackPointer + 16u);
            server.callbackBuffer = m_memory.read32(stackPointer + 20u);
            server.queue = m_memory.read32(stackPointer + 24u);
            m_servers[server.sid] = server;
            if (server.serverData != 0u)
            {
                m_memory.write32(server.serverData + 0x20u, server.sid);
                m_memory.write32(server.serverData + 0x28u, server.function);
                m_memory.write32(server.serverData + 0x2Cu, server.buffer);
            }
            setV0(server.serverData);
            return true;
        }
        case 18:
            setV0(0);
            return true;
        case 19: // SetRpcQueue
            setV0(cpu.gpr[4]);
            return true;
        case 20:
        case 21:
            setV0(0);
            return true;
        case 22: // RpcLoop
            m_kernel.sleepCurrent(cpu);
            setV0(0);
            return true;
        case 23:
            setV0(0);
            return true;
        case 24: // RemoveRpc
        {
            const uint32_t serverData = cpu.gpr[4];
            for (auto server = m_servers.begin(); server != m_servers.end(); ++server)
            {
                if (server->second.serverData == serverData)
                {
                    m_servers.erase(server);
                    break;
                }
            }
            setV0(0);
            return true;
        }
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    RpcResult IopRpcBridge::handleRpc(const RpcRequest &request, IopGuestExecutor &executor)
    {
        RpcResult result{};
        const auto serverIt = m_servers.find(request.sid);
        if (serverIt == m_servers.end() || serverIt->second.function == 0u)
            return result;

        RpcServer &server = serverIt->second;
        if (request.send.size != 0u && server.buffer != 0u)
        {
            const uint32_t copySize = std::min<uint32_t>(request.send.size, IopMemory::RamSize - std::min(server.buffer, IopMemory::RamSize));
            if (copySize != 0u)
            {
                std::vector<uint8_t> payload(copySize);
                if (m_host.readGuest(request.send.address, payload.data(), payload.size()))
                    (void)m_memory.writeRam(server.buffer, payload.data(), payload.size());
            }
        }

        uint32_t returnPointer = executor.executeGuestFunction(server.function,
                                                               request.function,
                                                               server.buffer,
                                                               request.send.size,
                                                               0u,
                                                               server.gp);
        if (returnPointer == 0u)
            returnPointer = server.buffer;
        if (request.receive.address != 0u && request.receive.size != 0u && returnPointer != 0u)
        {
            const uint32_t physical = IopMemory::physicalAddress(returnPointer);
            if (physical < IopMemory::RamSize)
            {
                const uint32_t copySize = std::min<uint32_t>(request.receive.size, IopMemory::RamSize - physical);
                (void)m_host.writeGuest(request.receive.address, m_memory.ram().data() + physical, copySize);
                if (copySize < request.receive.size)
                    (void)m_host.zeroGuest(request.receive.address + copySize, request.receive.size - copySize);
            }
        }

        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        result.signalNowaitCompletion = true;
        result.signalCompletion = true;
        return result;
    }

    void IopRpcBridge::onSifTransfer(const SifTransfer &transfer)
    {
        // The EE SIF transport owns the actual directional memory movement.
        // Services still receive both phases through IopSubsystem, but mirroring
        // IOP bytes through an equal-numbered EE address would alias two distinct
        // PS2 address spaces and can overwrite live game data.
        (void)transfer;
    }

    void IopRpcBridge::removeServersInRange(uint32_t base, uint32_t size)
    {
        for (auto handler = m_cmdHandlers.begin(); handler != m_cmdHandlers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(handler->second.function);
            if (function >= base && function < base + size)
                handler = m_cmdHandlers.erase(handler);
            else
                ++handler;
        }
        for (auto server = m_servers.begin(); server != m_servers.end();)
        {
            const uint32_t function = IopMemory::physicalAddress(server->second.function);
            if (function >= base && function < base + size)
                server = m_servers.erase(server);
            else
                ++server;
        }
    }

    bool IopRpcBridge::hasServer(uint32_t sid) const noexcept
    {
        const auto server = m_servers.find(sid);
        return server != m_servers.end() && server->second.function != 0u;
    }
}
