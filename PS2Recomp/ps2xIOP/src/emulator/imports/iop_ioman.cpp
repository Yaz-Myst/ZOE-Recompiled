#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <sstream>

namespace ps2x::iop::detail
{
    IopIoman::IopIoman(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        for (auto &[fd, file] : m_files)
            m_host.closeHostFile(file.handle);
        m_files.clear();
        m_nextFd = 3;
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        constexpr uint32_t kErrNoEnt = static_cast<uint32_t>(-2);
        constexpr uint32_t kErrBadF = static_cast<uint32_t>(-9);
        constexpr uint32_t kErrInval = static_cast<uint32_t>(-22);
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];

        switch (ordinal)
        {
        case 4: // open(name, flags, mode): read-only access to host/disc files
        {
            const std::string name = m_memory.readString(a0, 256u);
            const std::string translated = m_host.translateGuestPath(name);
            const uint64_t handle = m_host.openHostFile(translated.empty() ? name : translated);
            uint64_t size = 0u;
            if (handle == 0u || !m_host.hostFileSize(handle, size))
            {
                if (handle != 0u)
                    m_host.closeHostFile(handle);
                m_host.log(LogLevel::Warning, "[IOP] ioman open failed: " + name);
                setV0(kErrNoEnt);
                return true;
            }
            const int32_t fd = m_nextFd++;
            m_files[fd] = OpenFile{handle, size, 0u, name};
            setV0(static_cast<uint32_t>(fd));
            return true;
        }
        case 5: // close(fd)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(kErrBadF);
                return true;
            }
            m_host.closeHostFile(file->second.handle);
            m_files.erase(file);
            setV0(0u);
            return true;
        }
        case 6: // read(fd, buffer, size)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(kErrBadF);
                return true;
            }
            OpenFile &f = file->second;
            const uint64_t remaining = f.position < f.size ? f.size - f.position : 0u;
            std::vector<uint8_t> data(static_cast<size_t>(std::min<uint64_t>(a2, remaining)));
            size_t got = 0u;
            if (!data.empty() && (!m_host.readHostFile(f.handle, f.position, data.data(), data.size(), got) ||
                                  !m_memory.writeRam(a1, data.data(), got)))
            {
                setV0(kErrInval);
                return true;
            }
            f.position += got;
            setV0(static_cast<uint32_t>(got));
            return true;
        }
        case 8: // lseek(fd, offset, whence)
        {
            const auto file = m_files.find(static_cast<int32_t>(a0));
            if (file == m_files.end())
            {
                setV0(kErrBadF);
                return true;
            }
            OpenFile &f = file->second;
            const int64_t offset = static_cast<int32_t>(a1);
            int64_t target = offset;
            if (a2 == 1u)
                target = static_cast<int64_t>(f.position) + offset;
            else if (a2 == 2u)
                target = static_cast<int64_t>(f.size) + offset;
            if (target < 0)
            {
                setV0(kErrInval);
                return true;
            }
            f.position = static_cast<uint64_t>(target);
            setV0(static_cast<uint32_t>(f.position));
            return true;
        }
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }
}
