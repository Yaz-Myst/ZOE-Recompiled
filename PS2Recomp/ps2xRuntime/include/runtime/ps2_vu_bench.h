#pragma once

// VU micro-program capture and offline replay, for measuring and regression-checking VuFastEngine without playing the
// game up to the scene that stresses it.
//
//   PS2X_VU_CAPTURE=<file>,<firstTick>,<lastTick>[,<firstTick>,<lastTick>...]
//                                                   record every VU0/VU1 program call made during those vsync ticks
//                                                   (inputs only: registers, code when it changed, data memory)
//   PS2X_VU_BENCH=<file>[|<file>...][,iters=N][,write=<sums>][,check=<sums>][,writer=..][,checkr=..][,unit=0|1]
//                [,max=N][,prof=<dir>][,aotgen=<out.cpp>]
//                                                   replay the calls through fresh engines, print the time per pass,
//                                                   and write or compare a checksum of every call's results
//                                                   (registers, data memory, XGKICK packets); or, with aotgen=, write
//                                                   C++ for every code image seen (identical images across files are
//                                                   merged, entries united). The process exits after.
//
// Deliberately NOT included by ps2_runtime.h (generated-code include chain).

#include <cstdint>

struct VU1State;

namespace ps2_vu_bench
{
    bool captureEnabled();
    void captureCall(uint32_t unit, bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles,
                     const VU1State &state, const uint8_t *code, uint32_t codeSize, const uint8_t *data, uint32_t dataSize,
                     uint64_t codeGeneration);

    // Runs the benchmark and exits the process when PS2X_VU_BENCH is set; returns otherwise.
    void runBenchIfRequested();
}
