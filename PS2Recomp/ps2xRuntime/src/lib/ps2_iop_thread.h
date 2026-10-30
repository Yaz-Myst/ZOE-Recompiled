#pragma once

// IOP thread (PS2X_IOP_THREAD=1): runs the IOP while the game thread sleeps.
//
// When every EE thread is waiting, the scheduler sleeps until its next deadline (usually the vblank) and then credits
// the whole sleep as EE time at once; single-threaded, the IOP then runs that time in one burst (~1-2 ms) before the
// next frame can start. Here the IOP thread runs that time during the sleep instead (beginIdle / endIdle), and the
// game thread skips it later (consumePrepaid). The IOP never runs at the same time as EE guest code, so the EE and
// the IOP see each other exactly as before; IOP time accrued while the game runs is still run inline.
//
// - Both threads run the IOP under the IOP host's call lock, which every EE -> IOP call already takes.
// - IOP -> EE SIF commands raised on the IOP thread queue EE work (EeScheduler::queueInvocation is game-thread-only):
//   they are deferred and dispatched by the game thread in endIdle, the point where the burst used to run.

#include <cstddef>
#include <cstdint>
#include <mutex>

class PS2Runtime;

namespace ps2_iop_thread
{
    bool enabled();
    bool onIopThread();

    using RunFn = void (*)(PS2Runtime &runtime, uint64_t eeCycles);

    // Game thread, before an idle wait that would credit up to budget EE cycles: the IOP thread starts running them
    // through run (PS2Runtime::advanceIopEeCycles).
    void beginIdle(PS2Runtime &runtime, uint64_t budgetEeCycles, RunFn run);
    // Game thread, after the wait: stops the IOP thread at its next chunk, waits for it, records the time it ran as
    // prepaid and dispatches the SIF commands it raised.
    void endIdle(PS2Runtime &runtime);
    // Game thread: takes prepaid time out of eeCycles and returns what is left to run inline.
    uint64_t consumePrepaid(uint64_t eeCycles);

    // IOP thread: an IOP -> EE SIF command, dispatched by the game thread in endIdle.
    void deferSifCommand(uint32_t commandId, const void *packet, size_t packetSize);
    // Game thread: dispatches the deferred IOP -> EE SIF commands.
    void runDeferred(PS2Runtime &runtime);

    // Locks the IOP call mutex; from the game thread it also makes the IOP thread yield before its next chunk.
    void lockFromGame(std::unique_lock<std::recursive_mutex> &lock);

    void shutdown();
}
