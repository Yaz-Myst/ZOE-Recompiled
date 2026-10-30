// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// ps2xIOP glue for the PCSX2 SPU2 core. It replaces PCSX2's SPU2/spu2.cpp (host audio stream,
// config, save states) and the IOP-side hooks in PCSX2's IopDma.cpp / IopCounters.cpp (SPU DMA
// start and finish, IRQ lines, counter 6) with ps2xIOP equivalents. Functions marked "(PCSX2)"
// are carried over from those files with only the plumbing changed.

#include "emulator/spu2/spu2_device.h"

#include "emulator/core/iop_memory.h"
#include "ps2x/iop/spu2_debug.h"
#include "ps2x/state_io.h"

#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/spu2.h"

#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R3000A.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

// ---- Globals PCSX2 defines outside SPU2/ ----------------------------------------------------

const StereoOut32 StereoOut32::Empty(0, 0);
u64 lClocks = 0;
psxCounter psxCounters[8] = {};
s32 psxNextDeltaCounter = 0;
u32 psxNextStartCounter = 0;
u16 psxSoundDataTransferControl = 0;
u16 psxSPUSTAT = 0;
u16 psxSPUCNT = 0;

namespace
{
    constexpr u32 kTickInterval = 768; // IOP cycles per 48 kHz sample
    constexpr u64 kStatsIntervalCycles = 36864000ull * 5ull;

    ps2x::iop::detail::IopMemory *s_memory = nullptr;
    const uint64_t *s_cycle = nullptr;
    uint32_t s_pendingIrqs = 0;
    uint64_t s_nextEvent = 0;
    float s_dcFilterIn[2] = {};
    float s_dcFilterOut[2] = {};

    struct Stats
    {
        uint64_t samples = 0;
        uint64_t dmaWrites[2] = {};
        uint64_t dmaWriteBytes[2] = {};
        uint64_t dmaReads[2] = {};
        uint64_t keyOns = 0;
        uint64_t irqSpu = 0;
        uint64_t irqDma[2] = {};
        float peak = 0.0f;
    };
    Stats s_stats;
    uint64_t s_nextStatsCycle = kStatsIntervalCycles;

    // Sound start address (in 16-bit words) -> pitch it was last keyed on with. Written on the IOP
    // (game) thread, read by the F9 dump on the window thread.
    std::map<u32, u16> s_keyedSounds;
    std::mutex s_keyedSoundsMutex;

    // Optional capture of everything the SPU2 outputs: PS2X_SPU2_WAV=<path>.
    struct WavCapture
    {
        std::FILE *file = nullptr;
        uint32_t dataBytes = 0;
        uint32_t sinceHeader = 0;

        void open()
        {
            const char *path = std::getenv("PS2X_SPU2_WAV");
            if (!path || !*path || file)
                return;
            file = std::fopen(path, "wb");
            if (file)
                writeHeader();
        }

        void writeHeader()
        {
            const uint32_t rate = 48000, channels = 2, bits = 16;
            const uint32_t byteRate = rate * channels * bits / 8, blockAlign = channels * bits / 8;
            const uint32_t riffSize = 36 + dataBytes, fmtSize = 16;
            const uint16_t format = 1, ch = channels, align = blockAlign, bps = bits;
            std::fseek(file, 0, SEEK_SET);
            std::fwrite("RIFF", 1, 4, file);
            std::fwrite(&riffSize, 4, 1, file);
            std::fwrite("WAVEfmt ", 1, 8, file);
            std::fwrite(&fmtSize, 4, 1, file);
            std::fwrite(&format, 2, 1, file);
            std::fwrite(&ch, 2, 1, file);
            std::fwrite(&rate, 4, 1, file);
            std::fwrite(&byteRate, 4, 1, file);
            std::fwrite(&align, 2, 1, file);
            std::fwrite(&bps, 2, 1, file);
            std::fwrite("data", 1, 4, file);
            std::fwrite(&dataBytes, 4, 1, file);
            std::fseek(file, 0, SEEK_END);
        }

        void write(s16 left, s16 right)
        {
            if (!file)
                return;
            const s16 frame[2] = {left, right};
            std::fwrite(frame, sizeof(frame), 1, file);
            dataBytes += sizeof(frame);
            // Keep the header current (about once a second) so the file stays playable if the game is killed.
            if (++sinceHeader >= 48000)
            {
                sinceHeader = 0;
                writeHeader();
                std::fflush(file);
            }
        }
    };
    WavCapture s_wav;

    // Live output: single-producer (IOP thread) / single-consumer (host audio thread) ring.
    constexpr uint64_t kRingFrames = 32768;      // ~0.68 s
    constexpr double kTargetFill = 2400.0;       // ~50 ms queued (the EE, and so the SPU2, runs in ~16 ms bursts)
    constexpr double kMaxRateAdjust = 0.005;     // +/-0.5% playback rate
    std::array<s16, kRingFrames * 2> s_ring{};
    std::atomic<uint64_t> s_ringWrite{0};
    std::atomic<uint64_t> s_ringRead{0};
    std::atomic<bool> s_outputActive{false};
    std::atomic<uint64_t> s_underruns{0};
    std::atomic<uint64_t> s_underrunsTotal{0};
    std::atomic<uint64_t> s_overruns{0};
    std::atomic<uint32_t> s_lastFill{0};
    std::atomic<int32_t> s_lastRatePpm{0};

    void pushOutput(s16 left, s16 right)
    {
        if (!s_outputActive.load(std::memory_order_relaxed))
            return;
        const uint64_t w = s_ringWrite.load(std::memory_order_relaxed);
        const uint64_t r = s_ringRead.load(std::memory_order_acquire);
        if (w - r >= kRingFrames - 1u)
        {
            s_overruns.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        const size_t i = static_cast<size_t>(w % kRingFrames) * 2u;
        s_ring[i] = left;
        s_ring[i + 1u] = right;
        s_ringWrite.store(w + 1u, std::memory_order_release);
    }

    void raiseIrq(int bit)
    {
        s_pendingIrqs |= 1u << bit;
    }

    void recordKeyOn(uint core, u32 mask, uint firstVoice)
    {
        for (uint i = 0; i < 16; ++i)
        {
            if (!(mask & (1u << i)))
                continue;
            const uint voice = firstVoice + i;
            if (voice >= 24)
                continue;
            const V_Voice &vc = Cores[core].Voices[voice];
            {
                std::lock_guard lock(s_keyedSoundsMutex);
                s_keyedSounds[vc.StartA] = vc.Pitch;
            }
            ++s_stats.keyOns;
        }
    }

    // (PCSX2) DC blocking filter from SPU2/spu2.cpp.
    void dcFilter(float *input)
    {
        float output[2];
        output[0] = (input[0] - s_dcFilterIn[0] + ((0.995f * s_dcFilterOut[0])));
        output[1] = (input[1] - s_dcFilterIn[1] + ((0.995f * s_dcFilterOut[1])));
        s_dcFilterIn[0] = input[0];
        s_dcFilterIn[1] = input[1];
        s_dcFilterOut[0] = output[0];
        s_dcFilterOut[1] = output[1];
        input[0] = output[0];
        input[1] = output[1];
    }

    // (PCSX2) SPU2::InternalReset from SPU2/spu2.cpp, PS2 mode only.
    void internalReset()
    {
        spu2Mix = isa_native::spu2Mix;
        ReverbDownsample = isa_native::ReverbDownsample;
        ReverbUpsample = isa_native::ReverbUpsample;

        std::memset(spu2regs, 0, 0x010000);
        std::memset(_spu2mem, 0, 0x200000);
        std::memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
        std::memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.

        std::memset(s_dcFilterIn, 0, sizeof(s_dcFilterIn));
        std::memset(s_dcFilterOut, 0, sizeof(s_dcFilterOut));

        Spdif.Info = 0; // Reset IRQ Status if it got set in a previously run game

        Cores[0].Init(0);
        Cores[1].Init(1);
    }

    // (PCSX2) psxDmaGeneric from IopDma.cpp.
    void psxDmaGeneric(u32 madr, u32 bcr, u32 chcr, u32 spuCore)
    {
        const char dmaNum = spuCore ? 7 : 4;
        int size = (bcr >> 16) * (bcr & 0xFFFF);
        // ps2xIOP deviation: PCSX2's IOP RAM sits inside a larger block, so an overlong transfer
        // reads neighbouring data there; ours is exactly 2 MiB, so clip instead of faulting.
        const uint32_t ramOffset = madr & (ps2x::iop::detail::IopMemory::RamSize - 1u);
        const uint32_t maxWords = (ps2x::iop::detail::IopMemory::RamSize - ramOffset) / 4u;
        if (static_cast<uint32_t>(size) > maxWords)
        {
            std::printf("[spu2:dma] ch%d madr=%08x bcr=%08x runs past IOP RAM; clipped 0x%x -> 0x%x bytes\n", dmaNum, madr, bcr,
                        size * 4, maxWords * 4u);
            std::fflush(stdout);
            size = static_cast<int>(maxWords);
        }

        switch (chcr)
        {
        case 0x01000201: // cpu to spu2 transfer
            if (dmaNum == 7)
                SPU2writeDMA7Mem((u16 *)iopPhysMem(madr), size * 2);
            else if (dmaNum == 4)
                SPU2writeDMA4Mem((u16 *)iopPhysMem(madr), size * 2);
            ++s_stats.dmaWrites[spuCore];
            s_stats.dmaWriteBytes[spuCore] += static_cast<uint64_t>(size) * 4u;
            break;

        case 0x01000200: // spu2 to cpu transfer
            if (dmaNum == 7)
                SPU2readDMA7Mem((u16 *)iopPhysMem(madr), size * 2);
            else if (dmaNum == 4)
                SPU2readDMA4Mem((u16 *)iopPhysMem(madr), size * 2);
            ++s_stats.dmaReads[spuCore];
            break;

        default:
            std::fprintf(stderr, "[spu2] DMA %d unknown chcr=%x madr=%x bcr=%x\n", dmaNum, chcr, madr, bcr);
            break;
        }
    }
}

// ---- IOP-side hooks the SPU2 core calls (PCSX2's R3000A.h / IopHw.h / IopMem.h / IopDma.h) ----

Spu2PsxRegsView spu2PsxRegs()
{
    return {s_cycle ? static_cast<u32>(*s_cycle) : 0u};
}

u32 &spu2HwReg32(u32 address)
{
    return s_memory->hardwareRef(address);
}

u8 *iopPhysMem(u32 address)
{
    return s_memory->ramData() + (address & (ps2x::iop::detail::IopMemory::RamSize - 1u));
}

// (PCSX2) IopDma.cpp
void spu2DMA4Irq()
{
    SPU2interruptDMA4();
    if (HW_DMA4_CHCR & 0x01000000)
    {
        HW_DMA4_CHCR &= ~0x01000000;
        ++s_stats.irqDma[0];
        raiseIrq(1);
    }
}

// (PCSX2) IopDma.cpp
void spu2DMA7Irq()
{
    SPU2interruptDMA7();
    if (HW_DMA7_CHCR & 0x01000000)
    {
        HW_DMA7_CHCR &= ~0x01000000;
        ++s_stats.irqDma[1];
        raiseIrq(2);
    }
}

// (PCSX2) IopIrq.cpp: iopIntcIrq(9)
void spu2Irq()
{
    ++s_stats.irqSpu;
    raiseIrq(0);
}

// ---- Entry points PCSX2's SPU2/spu2.cpp provides to the rest of the SPU2 core ----------------

// (PCSX2)
void SPU2readDMA4Mem(u16 *pMem, u32 size) // size now in 16bit units
{
    TimeUpdate(psxRegs.cycle);
    Cores[0].DoDMAread(pMem, size);
}

// (PCSX2)
void SPU2writeDMA4Mem(u16 *pMem, u32 size) // size now in 16bit units
{
    TimeUpdate(psxRegs.cycle);
    Cores[0].DoDMAwrite(pMem, size);
}

// (PCSX2)
void SPU2interruptDMA4()
{
    if (Cores[0].DmaMode)
        Cores[0].Regs.STATX |= 0x80;
    Cores[0].Regs.STATX &= ~0x400;
    Cores[0].TSA = Cores[0].ActiveTSA;
}

// (PCSX2)
void SPU2interruptDMA7()
{
    if (Cores[1].DmaMode)
        Cores[1].Regs.STATX |= 0x80;
    Cores[1].Regs.STATX &= ~0x400;
    Cores[1].TSA = Cores[1].ActiveTSA;
}

// (PCSX2)
void SPU2readDMA7Mem(u16 *pMem, u32 size)
{
    TimeUpdate(psxRegs.cycle);
    Cores[1].DoDMAread(pMem, size);
}

// (PCSX2)
void SPU2writeDMA7Mem(u16 *pMem, u32 size)
{
    TimeUpdate(psxRegs.cycle);
    Cores[1].DoDMAwrite(pMem, size);
}

// PS1 (PSX) compatibility mode is never used by PS2 games.
bool SPU2::IsRunningPSXMode()
{
    return false;
}

// (PCSX2)
void SPU2async()
{
    TimeUpdate(psxRegs.cycle);
}

// (PCSX2)
u16 SPU2read(u32 rmem)
{
    u16 ret = 0xDEAD;
    u32 core = 0;
    const u32 mem = rmem & 0xFFFF;
    u32 omem = mem;

    if (mem & 0x400)
    {
        omem ^= 0x400;
        core = 1;
    }

    if (omem == 0x1f9001AC)
    {
        Cores[core].ActiveTSA = Cores[core].TSA;
        for (int i = 0; i < 2; i++)
        {
            if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
            {
                SetIrqCall(i);
            }
        }
        ret = Cores[core].DmaRead();
    }
    else
    {
        TimeUpdate(psxRegs.cycle);

        if (rmem >> 16 == 0x1f80)
        {
            ret = Cores[0].ReadRegPS1(rmem);
        }
        else if (mem >= 0x800)
        {
            ret = spu2Ru16(mem);
        }
        else
        {
            ret = *(regtable[(mem >> 1)]);
        }
    }

    return ret;
}

// (PCSX2)
void SPU2write(u32 rmem, u16 value)
{
    // Note: Reverb/Effects are very sensitive to having precise update timings.
    // If the SPU2 isn't in in sync with the IOP, samples can end up playing at rather
    // incorrect pitches and loop lengths.

    TimeUpdate(psxRegs.cycle);

    if (rmem >> 16 == 0x1f80)
        Cores[0].WriteRegPS1(rmem, value);
    else
        SPU2_FastWrite(rmem, value);
}

// (PCSX2) spu2Output, with PCSX2's host stream replaced by ps2xIOP's sinks.
void spu2Output(StereoOut32 out)
{
    float conv[2];

    conv[0] = static_cast<float>(clamp_mix(out.Left)) / INT16_MAX;
    conv[1] = static_cast<float>(clamp_mix(out.Right)) / INT16_MAX;

    /* Some games pause voices with the volume left on leaving us with
     * significant DC offset, so we filter it out (e.x. SSX 3)*/
    dcFilter(conv);

    ++s_stats.samples;
    s_stats.peak = std::max({s_stats.peak, std::abs(conv[0]), std::abs(conv[1])});

    const auto toS16 = [](float v)
    {
        return static_cast<s16>(std::clamp(v * 32767.0f, -32768.0f, 32767.0f));
    };
    s_wav.write(toS16(conv[0]), toS16(conv[1]));
    pushOutput(toS16(conv[0]), toS16(conv[1]));
}

// ---- ps2xIOP interface -------------------------------------------------------------------------

namespace ps2x::iop::spu2
{
    void attach(detail::IopMemory &memory, const uint64_t *iopCycle) noexcept
    {
        s_memory = &memory;
        s_cycle = iopCycle;
    }

    void reset() noexcept
    {
        lClocks = psxRegs.cycle;
        internalReset();
        s_pendingIrqs = 0;
        s_nextEvent = 0;
        {
            std::lock_guard lock(s_keyedSoundsMutex);
            s_keyedSounds.clear();
        }
        s_stats = {};
        s_nextStatsCycle = (s_cycle ? *s_cycle : 0u) + kStatsIntervalCycles;
        s_wav.open();
    }

    uint16_t read16(uint32_t phys) noexcept
    {
        return SPU2read(phys);
    }

    void write16(uint32_t phys, uint16_t value) noexcept
    {
        SPU2write(phys, value);

        // Remember which sounds get keyed on, and at what pitch, for tools/spu2_extract.py.
        const u32 reg = phys & 0x7FF;
        const uint core = (reg & 0x400) ? 1u : 0u;
        const u32 local = reg & 0x3FF;
        if (local == REG_S_KON)
            recordKeyOn(core, value, 0);
        else if (local == REG_S_KON + 2)
            recordKeyOn(core, value, 16);

        s_nextEvent = 0; // register writes can start DMA or change timing; re-evaluate
    }

    void startDma(int channel, uint32_t madr, uint32_t bcr, uint32_t chcr) noexcept
    {
        psxDmaGeneric(madr, bcr, chcr, channel == 7 ? 1u : 0u);
        s_nextEvent = 0;
    }

    void update() noexcept
    {
        if (!s_cycle)
            return;

        // (PCSX2) the SPU2 part of psxRcntUpdate in IopCounters.cpp: counter 6 fires every
        // sample, and CounterUpdate() pulls it in earlier when a DMA is due to finish.
        const u32 now = static_cast<u32>(*s_cycle);
        const u32 spu2_delta = (now - static_cast<u32>(lClocks)) % kTickInterval;
        psxCounters[6].startCycle = now - spu2_delta;
        psxCounters[6].deltaCycles = kTickInterval;
        psxNextStartCounter = now;
        psxNextDeltaCounter = psxCounters[6].deltaCycles;
        SPU2async();

        // Mixing itself doesn't need the IOP woken for every sample: update() also runs at the
        // end of every IOP batch and on every register access. Only a DMA in flight needs its
        // completion interrupt on time.
        if (Cores[0].DMAICounter > 0 || Cores[1].DMAICounter > 0)
        {
            const u32 untilNext = psxCounters[6].startCycle + static_cast<u32>(psxCounters[6].deltaCycles) - now;
            s_nextEvent = *s_cycle + std::clamp<u32>(untilNext, 1u, kTickInterval);
        }
        else
        {
            s_nextEvent = UINT64_MAX;
        }

        if (*s_cycle >= s_nextStatsCycle)
        {
            s_nextStatsCycle = *s_cycle + kStatsIntervalCycles;
            std::printf("%s\n", ps2x::iop::spu2StatsLine().c_str());
            std::fflush(stdout);
        }
    }

    uint64_t nextEventCycle() noexcept
    {
        return s_nextEvent;
    }

    const uint64_t *nextEventCyclePtr() noexcept { return &s_nextEvent; }
    const uint32_t *pendingIrqsPtr() noexcept { return &s_pendingIrqs; }

    uint32_t takePendingIrqs() noexcept
    {
        const uint32_t irqs = s_pendingIrqs;
        s_pendingIrqs = 0;
        return irqs;
    }
}

namespace ps2x::iop
{
    void spu2ReadOutput(int16_t *out, uint32_t frames)
    {
        // Runs on the host audio thread. Plays the queue directly (no time-stretching: SoundTouch
        // needed ~60 ms of input buffered before producing anything, which caused dropouts), with
        // a +/-0.5% playback-rate nudge to follow the guest/sound-card clock difference. If the
        // queue runs dry the output goes silent until ~50 ms has built up again.
        static bool rebuffering = true;
        static double frac = 0.0;
        static double fillAvg = kTargetFill;

        s_outputActive.store(true, std::memory_order_relaxed);
        const uint64_t w = s_ringWrite.load(std::memory_order_acquire);
        uint64_t r = s_ringRead.load(std::memory_order_relaxed);
        const uint64_t fill = w - r;
        s_lastFill.store(static_cast<uint32_t>(fill), std::memory_order_relaxed);

        if (rebuffering)
        {
            if (static_cast<double>(fill) < kTargetFill)
            {
                std::fill(out, out + frames * 2u, int16_t{0});
                return;
            }
            rebuffering = false;
            frac = 0.0;
            fillAvg = static_cast<double>(fill);
        }

        fillAvg += (static_cast<double>(fill) - fillAvg) * 0.05;
        const double rate = 1.0 + std::clamp((fillAvg - kTargetFill) / kTargetFill * 0.01, -kMaxRateAdjust, kMaxRateAdjust);
        s_lastRatePpm.store(static_cast<int32_t>((rate - 1.0) * 1e6), std::memory_order_relaxed);

        for (uint32_t i = 0; i < frames; ++i)
        {
            if (r + 1u >= w)
            {
                std::fill(out + i * 2u, out + frames * 2u, int16_t{0});
                s_underruns.fetch_add(1u, std::memory_order_relaxed);
                s_underrunsTotal.fetch_add(1u, std::memory_order_relaxed);
                rebuffering = true;
                break;
            }
            const size_t a = static_cast<size_t>(r % kRingFrames) * 2u;
            const size_t b = static_cast<size_t>((r + 1u) % kRingFrames) * 2u;
            out[i * 2u] = static_cast<int16_t>(s_ring[a] + (s_ring[b] - s_ring[a]) * frac);
            out[i * 2u + 1u] = static_cast<int16_t>(s_ring[a + 1u] + (s_ring[b + 1u] - s_ring[a + 1u]) * frac);
            frac += rate;
            const double whole = std::floor(frac);
            r += static_cast<uint64_t>(whole);
            frac -= whole;
        }
        s_ringRead.store(std::min(r, w), std::memory_order_release);
    }

    void spu2OutputStatus(uint32_t &queuedFrames, uint64_t &underrunsTotal)
    {
        queuedFrames = s_lastFill.load(std::memory_order_relaxed);
        underrunsTotal = s_underrunsTotal.load(std::memory_order_relaxed);
    }

    bool dumpSpu2Ram(const std::string &path)
    {
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
            return false;
        const bool ok = std::fwrite(_spu2mem, sizeof(_spu2mem), 1, file) == 1;
        std::fclose(file);

        if (std::FILE *voices = std::fopen((path + ".voices.txt").c_str(), "w"))
        {
            std::fprintf(voices, "# start_word_address pitch   (sample rate = pitch * 48000 / 4096)\n");
            std::lock_guard lock(s_keyedSoundsMutex);
            for (const auto &[start, pitch] : s_keyedSounds)
                std::fprintf(voices, "0x%05x 0x%04x\n", start, pitch);
            std::fclose(voices);
        }
        return ok;
    }

    std::string spu2StatsLine()
    {
        uint32_t activeVoices = 0;
        for (const V_Core &core : Cores)
            for (const V_Voice &voice : core.Voices)
                if (voice.ADSR.Phase != V_ADSR::PHASE_STOPPED)
                    ++activeVoices;

        std::ostringstream out;
        out << "[spu2] samples=" << s_stats.samples
            << " dmaWrite c0=" << s_stats.dmaWrites[0] << "/" << (s_stats.dmaWriteBytes[0] / 1024) << "KB"
            << " c1=" << s_stats.dmaWrites[1] << "/" << (s_stats.dmaWriteBytes[1] / 1024) << "KB"
            << " dmaRead=" << (s_stats.dmaReads[0] + s_stats.dmaReads[1])
            << " keyOns=" << s_stats.keyOns
            << " irq spu=" << s_stats.irqSpu << " dma4=" << s_stats.irqDma[0] << " dma7=" << s_stats.irqDma[1]
            << " voicesActive=" << activeVoices
            << " peak=" << s_stats.peak
            << " out fill=" << s_lastFill.load() << " rate=" << (s_lastRatePpm.load() / 1e4) << "% underruns="
            << s_underruns.exchange(0u) << " overruns=" << s_overruns.exchange(0u);
        {
            std::lock_guard lock(s_keyedSoundsMutex);
            out << " sounds=" << s_keyedSounds.size();
        }
        s_stats = {};
        return out.str();
    }
}

bool* SPU2_StateIrqDmaFlags();
bool* SPU2_StatePsxMode();

namespace ps2x::iop::spu2
{
    namespace
    {
        constexpr uint64_t kNoPtr = ~0ull;

        uint64_t ramOffset(const void *p)
        {
            if (p == nullptr || s_memory == nullptr)
                return kNoPtr;
            return static_cast<uint64_t>(reinterpret_cast<const uint8_t *>(p) - s_memory->ramData());
        }

        uint64_t cacheOffset(const s16 *p)
        {
            if (p == nullptr)
                return kNoPtr;
            return static_cast<uint64_t>(reinterpret_cast<const uint8_t *>(p) - reinterpret_cast<const uint8_t *>(pcm_cache_data));
        }
    }

    void saveState(ps2_state::Writer &w) noexcept
    {
        w.beginSection(ps2_state::tag("SPU2"), 1u);
        w.blob(spu2regs, sizeof(spu2regs));
        w.blob(_spu2mem, sizeof(_spu2mem));
        // Cores with their host pointers cleared; the pointers follow as offsets.
        std::vector<uint8_t> cores(sizeof(Cores));
        std::memcpy(cores.data(), Cores, sizeof(Cores));
        auto *copy = reinterpret_cast<V_Core *>(cores.data());
        for (int c = 0; c < 2; ++c)
        {
            copy[c].DMAPtr = nullptr;
            copy[c].DMARPtr = nullptr;
            for (auto &v : copy[c].Voices)
                v.SBuffer = nullptr;
        }
        w.blob(cores.data(), cores.size());
        std::vector<uint32_t> cacheBlocks;
        for (int c = 0; c < 2; ++c)
        {
            w.u64(ramOffset(Cores[c].DMAPtr));
            w.u64(ramOffset(Cores[c].DMARPtr));
            for (const auto &v : Cores[c].Voices)
            {
                const uint64_t off = cacheOffset(v.SBuffer);
                w.u64(off);
                if (off != kNoPtr)
                    cacheBlocks.push_back(static_cast<uint32_t>(off / sizeof(PcmCacheEntry)));
            }
        }
        std::sort(cacheBlocks.begin(), cacheBlocks.end());
        cacheBlocks.erase(std::unique(cacheBlocks.begin(), cacheBlocks.end()), cacheBlocks.end());
        w.u32(static_cast<uint32_t>(cacheBlocks.size()));
        for (uint32_t block : cacheBlocks)
        {
            w.u32(block);
            w.pod(pcm_cache_data[block]);
        }
        w.pod(Spdif);
        w.pod(OutPos);
        w.pod(InputPos);
        w.pod(Cycles);
        w.pod(PlayMode);
        const bool *irqDma = SPU2_StateIrqDmaFlags();
        w.boolean(irqDma[0]);
        w.boolean(irqDma[1]);
        w.boolean(*SPU2_StatePsxMode());
        // PCSX2 glue (counters PCSX2 keeps outside SPU2/) and this device's own state.
        w.pod(lClocks);
        w.pod(psxCounters);
        w.pod(psxNextDeltaCounter);
        w.pod(psxNextStartCounter);
        w.pod(psxSoundDataTransferControl);
        w.pod(psxSPUSTAT);
        w.pod(psxSPUCNT);
        w.u32(s_pendingIrqs);
        w.pod(s_dcFilterIn);
        w.pod(s_dcFilterOut);
        w.endSection();
    }

    void loadState(ps2_state::Reader &r) noexcept
    {
        if (r.beginSection(ps2_state::tag("SPU2")) != 1u)
            return;
        r.blob(spu2regs, sizeof(spu2regs));
        r.blob(_spu2mem, sizeof(_spu2mem));
        std::vector<uint8_t> cores(sizeof(Cores));
        r.blob(cores.data(), cores.size());
        if (!r.ok())
            return;
        // In place: regtable holds pointers into Cores and spu2regs.
        std::memcpy(static_cast<void *>(Cores), cores.data(), sizeof(Cores));
        std::memset(static_cast<void *>(pcm_cache_data), 0, sizeof(pcm_cache_data));
        uint8_t *ram = s_memory != nullptr ? s_memory->ramData() : nullptr;
        for (int c = 0; c < 2; ++c)
        {
            const uint64_t dma = r.u64();
            const uint64_t dmar = r.u64();
            Cores[c].DMAPtr = (dma == kNoPtr || ram == nullptr) ? nullptr : reinterpret_cast<u16 *>(ram + dma);
            Cores[c].DMARPtr = (dmar == kNoPtr || ram == nullptr) ? nullptr : reinterpret_cast<u16 *>(ram + dmar);
            for (auto &v : Cores[c].Voices)
            {
                const uint64_t off = r.u64();
                v.SBuffer = off == kNoPtr ? nullptr : reinterpret_cast<s16 *>(reinterpret_cast<uint8_t *>(pcm_cache_data) + off);
            }
        }
        const uint32_t blocks = r.u32();
        for (uint32_t i = 0; i < blocks && r.ok(); ++i)
        {
            const uint32_t block = r.u32();
            PcmCacheEntry entry{};
            r.pod(entry);
            if (block < static_cast<uint32_t>(pcm_BlockCount))
                pcm_cache_data[block] = entry;
        }
        r.pod(Spdif);
        r.pod(OutPos);
        r.pod(InputPos);
        r.pod(Cycles);
        r.pod(PlayMode);
        bool *irqDma = SPU2_StateIrqDmaFlags();
        irqDma[0] = r.boolean();
        irqDma[1] = r.boolean();
        *SPU2_StatePsxMode() = r.boolean();
        r.pod(lClocks);
        r.pod(psxCounters);
        r.pod(psxNextDeltaCounter);
        r.pod(psxNextStartCounter);
        r.pod(psxSoundDataTransferControl);
        r.pod(psxSPUSTAT);
        r.pod(psxSPUCNT);
        s_pendingIrqs = r.u32();
        r.pod(s_dcFilterIn);
        r.pod(s_dcFilterOut);
        s_nextEvent = 0u; // re-evaluated on the next update()
        // Drop queued output (host audio), so the sound restarts from the loaded moment.
        s_ringRead.store(s_ringWrite.load(std::memory_order_acquire), std::memory_order_release);
        r.endSection("SPU2");
    }
}
