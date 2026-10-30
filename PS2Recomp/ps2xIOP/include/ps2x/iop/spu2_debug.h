#pragma once

// Debug access to the emulated SPU2 (sound processor), for diagnostics tooling.

#include <cstdint>
#include <string>

namespace ps2x::iop
{
    // Live audio: fills `frames` interleaved stereo 16-bit 48 kHz frames with SPU2 output. Called
    // from the host audio thread. Keeps ~50 ms buffered, nudging the playback rate by up to
    // 0.5% to follow the difference between the guest and the sound card clocks, and plays
    // silence (while refilling) if the game stalls.
    void spu2ReadOutput(int16_t *interleaved, uint32_t frames);

    // For the performance overlay: 48 kHz frames waiting to be played, and dropouts since start.
    void spu2OutputStatus(uint32_t &queuedFrames, uint64_t &underrunsTotal);

    // Writes the 2 MiB SPU2 sound RAM to `path`, plus `path` + ".voices.txt" listing every
    // sound start address the game keyed on with the pitch it used (for tools/spu2_extract.py).
    bool dumpSpu2Ram(const std::string &path);

    // One-line activity summary (DMA traffic, key-ons, IRQs, output level) since the last call.
    std::string spu2StatsLine();
}
