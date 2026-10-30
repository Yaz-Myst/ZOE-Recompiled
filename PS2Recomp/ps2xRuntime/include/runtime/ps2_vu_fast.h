#pragma once

// Fast VU1 micro-program engine.
//
// Same architectural results as VU1Interpreter (ps2_vu1.h) at a fraction of the cost: each instruction pair is
// decoded once per microcode upload into a compact form, VF/VI/ACC results are written immediately (every read of
// those registers interlocks on the hardware, so a reader can only ever see the new value once its stall is paid),
// and only the state whose visibility really is delayed keeps a timed queue: MAC/status/clip flags (FIFO, four-cycle
// latency), Q and P, and the PATH1 transfer of XGKICK. Stall cycles are still counted per lane so that flag, Q/P and
// XGKICK timing match the interpreter.
//
// Deliberately NOT included by ps2_runtime.h: that header is part of the generated code's include chain.

#include <cstdint>
#include <functional>
#include <memory>

struct VU1State;
class GS;
class PS2Memory;

namespace ps2_state
{
    struct Access; // save states (src/lib/ps2_savestate.cpp)
}

class VuFastEngine
{
    friend struct ps2_state::Access;

public:
    // Receives each finished XGKICK packet. Unset: packets go to PS2Memory::submitGifPacket (PATH1) or GS directly.
    using PacketSink = std::function<void(const uint8_t *data, uint32_t size)>;

    // vu0 = true runs VU0 micro programs (4 KB code/data, no XGKICK/EFU, VCALLMS from the EE).
    explicit VuFastEngine(bool vu0 = false);
    ~VuFastEngine();

    // MSCAL: start a program at startPC. Runs until the E-bit ends it (or a D/T halt), or maxCycles elapse.
    void execute(VU1State &state, const uint8_t *code, uint32_t codeSize, uint8_t *data, uint32_t dataSize,
                 PS2Memory *memory, GS *gs, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles);

    // MSCNT: continue from state.pc.
    void resume(VU1State &state, const uint8_t *code, uint32_t codeSize, uint8_t *data, uint32_t dataSize,
                PS2Memory *memory, GS *gs, uint32_t top, uint32_t itop, uint32_t maxCycles);

    void setPacketSink(PacketSink sink);

    // Without a PS2Memory the engine cannot see microcode uploads and re-decodes on every call; a non-zero value
    // here names the current code instead (change it whenever the code changes). Used by the replay benchmark.
    void setCodeGeneration(uint64_t generation);

    struct Impl; // runtime/ps2_vu_fast_impl.h

    // Ahead-of-time compiled micro programs: C++ generated from captured micro memory (PS2X_VU_BENCH=...,aotgen=<file>,
    // see ps2_vu_aotgen.cpp) and linked into the game. An image is used while every pair it compiled still matches
    // micro memory. run() executes from startPC on the engine's own pipeline state and returns true when the program
    // ended (flushed, exactly as the interpreter ends it), or false after setting st->pc and the branch / E-bit state
    // for the interpreter to carry on (unknown entry, unsupported pair, cycle budget).
    using AotRunFn = bool (*)(Impl &m, uint32_t startPC, uint64_t budgetEnd);
    struct AotImage
    {
        bool vu0;
        const uint32_t *pairs; // {pc, lower, upper} for every compiled pair
        uint32_t pairCount;
        AotRunFn run;
        const char *name;
    };
    static void registerAot(const AotImage &image);
    static AotRunFn findAot(bool vu0, const uint8_t *code, uint32_t codeSize);

private:
    std::unique_ptr<Impl> m_impl;
};
