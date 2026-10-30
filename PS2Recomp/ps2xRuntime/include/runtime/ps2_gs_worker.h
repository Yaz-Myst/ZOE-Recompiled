#ifndef PS2_GS_WORKER_H
#define PS2_GS_WORKER_H

// Render worker ("GS thread"): runs the GIF (PATH3) and VIF1 DMA work - VIF1 -> VU1 -> GIF -> GS -> draw lists - on
// its own host thread, in the order the game thread kicked it. DMA completion is still reported to the game at kick
// time (as before), so the game keeps running while the worker draws. The game thread waits for the worker (drain)
// before it touches anything the worker owns: GS privileged registers, VU1 memory, VIF1/GIF registers and FIFOs, and
// the GS itself. PS2X_GS_THREAD=1 enables it.
//
// Included by runtime .cpp files only (never by generated code), so it can change without a full rebuild.

#include "runtime/ps2_memory.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace ps2_gs_worker
{
    bool enabled();
    // True on the worker and on the GS draw thread.
    bool onWorker();

    // PS2X_GS_THREAD=2 splits the worker in two: the worker runs VIF1/VU1/GIF arbitration and hands the resulting GIF
    // packet stream to a GS draw thread, which runs the GS (setDrawTarget). captureGifPacket is the hand-off: called
    // with each packet the arbiter releases, it returns true when the packet was queued for the draw thread.
    void setDrawTarget(void (*fn)(void *ctx, const uint8_t *data, uint32_t size), void *ctx);
    bool captureGifPacket(const uint8_t *data, uint32_t size);

    // Why the game thread waited for the worker (for the stats line).
    enum class Sync : uint8_t
    {
        GsPriv,    // GS privileged register read/write (CSR, DISPFB, ...)
        Vu1Mem,    // EE access to VU1 code/data memory
        VifGifReg, // VIF1 / GIF register read/write
        Fifo,      // VIF1 FIFO write
        GsDirect,  // runtime code driving the GS directly (library replacements, native paths)
        FrameLimit, // the game got kMaxFramesAhead display flips ahead of the worker
        Count
    };

    // Hands the pending GIF and VIF1 transfers to the worker. Their data must already be owned (chainData).
    void submit(PS2Memory &mem, std::vector<PS2Memory::PendingTransfer> &&gif, std::vector<PS2Memory::PendingTransfer> &&vif1);

    // Jobs submitted and not finished yet.
    extern std::atomic<uint32_t> g_pending;
    void drainSlow(Sync why);
    // Waits until the worker has finished everything submitted. One load when idle; no-op on the worker itself.
    inline void drain(Sync why)
    {
        if (g_pending.load(std::memory_order_acquire) != 0u)
            drainSlow(why);
    }

    // A write to a display register (PMODE, DISPFB, DISPLAY, BGCOLOR...: offsets below CSR) while the worker is busy:
    // queued behind the drawing already submitted, so the displayed buffer changes only once that drawing is recorded
    // (the game does not wait). Returns false when it can be applied at once. A flip (DISPFB write) counts as a frame;
    // the game waits once it is kMaxFramesAhead flips ahead of the worker.
    bool deferGsPrivWrite(PS2Memory &mem, uint32_t address, uint64_t value, uint32_t size);

    // A GS privileged register access from the game thread; the stats name the registers that made it wait.
    void noteGsPrivWait(uint32_t address, bool write, uint64_t value);
    inline void drainGsPriv(uint32_t address, bool write, uint64_t value)
    {
        if (g_pending.load(std::memory_order_acquire) != 0u)
        {
            noteGsPrivWait(address, write, value);
            drainSlow(Sync::GsPriv);
        }
    }

    // Presentation clock. A flip = the first DISPFB1/DISPFB2 write of a vsync, counted when it is applied (after the
    // drawing queued before it, so the frame is complete). The window can show each new frame once, as it arrives,
    // instead of sampling the vsync counter on its own timer (which skips and repeats frames as the clocks drift).
    // noteDirectGsPrivWrite: a display register written without queuing (nothing in flight, or single-threaded).
    void noteDirectGsPrivWrite(PS2Memory &mem, uint32_t address);
    uint64_t flipCount();
    // Whether a movie was playing (ps2MovieActive) when the game issued the most recently applied flip: recorded on the
    // game thread when the DISPFB write is queued (so it describes the same moment as the frame's drawing), published
    // with the flip. The window shape follows this, never the live flag, so a presented frame is shown in the shape of
    // its own content (movie 4:3 / 3D 16:9).
    bool flipMovie();
    // Waits until flipCount() differs from `seen` or the timeout passes; returns flipCount().
    uint64_t waitForFlip(uint64_t seen, uint32_t timeoutMs);

    void shutdown();
}

// A GS privileged register write outside CSR (ps2_memory.cpp): size 4 merges one half, 8 stores the register.
void ps2ApplyGsPrivWrite(PS2Memory &mem, uint32_t address, uint64_t value, uint32_t size);

// The GIF and VIF1 halves of PS2Memory::processPendingTransfers (ps2_memory.cpp), shared with the worker.
void ps2ProcessGifTransfers(PS2Memory &mem, std::vector<PS2Memory::PendingTransfer> &transfers);
void ps2ProcessVif1Transfers(PS2Memory &mem, std::vector<PS2Memory::PendingTransfer> &transfers);

#endif // PS2_GS_WORKER_H
