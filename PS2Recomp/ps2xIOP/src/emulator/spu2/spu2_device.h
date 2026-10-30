#pragma once

// SPU2 (PS2 sound processor) emulation for the IOP emulator.
//
// The chip itself is PCSX2's SPU2 core (pcsx2/SPU2, GPL-3.0+), ported with its sources kept as
// close to upstream as possible; see spu2/README.md. This header is the only interface the rest
// of ps2xIOP sees, so no PCSX2 types or globals leak out.

#include <cstdint>

namespace ps2x::iop::detail
{
    class IopMemory;
}

namespace ps2_state
{
    class Writer;
    class Reader;
}

namespace ps2x::iop::spu2
{
    // Interrupt lines the SPU2 raises, as intrman IRQ numbers.
    constexpr int kIrqSpu2 = 9;
    constexpr int kIrqDma4 = 0x24;
    constexpr int kIrqDma7 = 0x28;

    constexpr uint32_t kRegBase = 0x1F900000u;
    constexpr uint32_t kRegEnd = 0x1F910000u;

    [[nodiscard]] inline bool isRegister(uint32_t phys) noexcept
    {
        return phys >= kRegBase && phys < kRegEnd;
    }

    // Connects the SPU2 to IOP RAM, the IOP DMA registers and the IOP cycle counter.
    void attach(detail::IopMemory &memory, const uint64_t *iopCycle) noexcept;

    // Power-on state (both cores, sound RAM cleared).
    void reset() noexcept;

    [[nodiscard]] uint16_t read16(uint32_t phys) noexcept;
    void write16(uint32_t phys, uint16_t value) noexcept;

    // An IOP DMA channel 4 (core 0) or 7 (core 1) transfer was started through its CHCR register.
    void startDma(int channel, uint32_t madr, uint32_t bcr, uint32_t chcr) noexcept;

    // Catches the SPU2 up to the current IOP cycle (mixes the samples that are due).
    void update() noexcept;

    // Absolute IOP cycle at which update() next has work to do.
    [[nodiscard]] uint64_t nextEventCycle() noexcept;

    // IRQs raised since the last call, as a bitmask: bit 0 = kIrqSpu2, bit 1 = kIrqDma4, bit 2 = kIrqDma7.
    [[nodiscard]] uint32_t takePendingIrqs() noexcept;
    // The IOP loop polls these every instruction; reading them through a pointer avoids two calls per instruction.
    [[nodiscard]] const uint64_t *nextEventCyclePtr() noexcept;
    [[nodiscard]] const uint32_t *pendingIrqsPtr() noexcept;

    // Save states: both cores, sound RAM, registers, the PCSX2 glue counters. Host pointers inside the cores (DMA
    // pointers into IOP RAM, voice pointers into the ADPCM cache) are saved as offsets; the ADPCM blocks the voices
    // are in the middle of are saved with them, the rest of the cache is dropped.
    void saveState(ps2_state::Writer &w) noexcept;
    void loadState(ps2_state::Reader &r) noexcept;
}
