#pragma once

#include <atomic>
#include <cstdint>

// Per-vsync performance timeline: PS2X_FRAME_LOG=<csv path> writes one line per vblank, so a slow moment can be
// pinned on its cause (guest CPU budget, host CPU, VU1/render threads) instead of a 5-second average.
// Counters are cumulative; the writer turns them into per-vblank deltas.
namespace ps2_frame_log
{
    struct Counters
    {
        std::atomic<uint64_t> vu1Programs{0}; // VU1 microprograms started (MSCAL/MSCNT)
        std::atomic<uint64_t> gsWorkerNs{0};  // render thread 1 busy (VIF1 unpack, VU1 programs, GIF)
        std::atomic<uint64_t> gsDrawNs{0};    // render thread 2 busy (GS drawing)
        std::atomic<uint64_t> gameWaitNs{0};  // game thread blocked waiting for the render threads
        std::atomic<uint64_t> pacingNs{0};    // game thread held back because guest time ran ahead of real time
        std::atomic<uint64_t> idleNs{0};      // game thread idle (every guest thread waiting)
        std::atomic<uint64_t> vu1Ns{0};       // VU1 program execution (part of the render thread's busy time)
        std::atomic<uint64_t> vu1Aot{0};      // VU1 runs done entirely by compiled (AOT) code
        std::atomic<uint64_t> vu1Fallback{0}; // VU1 runs that used the fast interpreter (not compiled, or handed over)
        std::atomic<uint64_t> vu1KickNs{0};   // inside vu1Ns: handing XGKICK packets to the GIF/GS (PATH1)
        // GPU renderer, on the GS thread (inside drawMs): texture lookups that missed the fast path (hash, decode, copy),
        // texture decodes and their pixels, image uploads into GS memory, upload pixels mirrored into GPU targets.
        std::atomic<uint64_t> gsTexNs{0}, gsDecodes{0}, gsDecodePx{0}, gsUploadNs{0}, gsUploadBytes{0}, gsMirrorPx{0};
    };
    Counters &counters();
    bool enabled();
    // EE scheduler, at every vblank start. newGameFrame: the displayed frame buffer changed at this vblank.
    void onVblank(uint64_t tick, uint64_t eeCycle, bool newGameFrame);
}
