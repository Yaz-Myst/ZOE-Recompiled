// Fast VU1 engine - see runtime/ps2_vu_fast.h for the model. Instruction semantics are ported one-for-one from
// ps2_vu1_upper.cpp / ps2_vu1_lower.cpp and the timing rules (latencies, stalls, delay slots, E/D/T bits, XGKICK
// pacing) from ps2_vu1_core.cpp. Differences, all invisible to programs that do not race the hardware:
//  - VF/VI/ACC writes land immediately; a reader still stalls until the write's latency has elapsed.
//  - Stores land immediately (the interpreter made them visible one cycle later).
//  - MAC/status flags are derived from the rounded result (the interpreter re-evaluated each FMAC op in long double
//    to detect overflow/underflow of the exact value; with round-toward-zero that only differs at FLT_MAX).
#include "runtime/ps2_vu_fast.h"

#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu_fast_impl.h"

#include <chrono>

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <immintrin.h>
#include <smmintrin.h>

namespace
{
    std::vector<VuFastEngine::AotImage> &aotRegistry()
    {
        static std::vector<VuFastEngine::AotImage> images;
        return images;
    }

    bool aotEnabled()
    {
        static const bool enabled = []
        {
            const char *env = std::getenv("PS2X_VU_AOT");
            return !(env && *env == '0');
        }();
        return enabled;
    }
}

// ZOE effects investigation (PS2X_VU1_TRACE, see ps2_runtime.cpp): counts XGKICKs and prints the packets of
// traced VU1 runs (GIF tags, primitive, vertex count, vertices with the ADC "don't draw" bit).
uint32_t g_vuFastKicks = 0;
bool g_vuFastTraceKicks = false;
namespace
{
    void traceKick(const uint8_t *p, uint32_t size)
    {
        uint32_t off = 0;
        int tags = 0;
        while (off + 16u <= size && tags < 8)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, p + off, 8);
            std::memcpy(&hi, p + off + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t eop = static_cast<uint32_t>((lo >> 15) & 1u);
            const uint32_t pre = static_cast<uint32_t>((lo >> 46) & 1u);
            const uint32_t prim = static_cast<uint32_t>((lo >> 47) & 0x7FFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            off += 16u;
            uint32_t verts = 0, adc = 0;
            int firstX = -1, firstY = -1, firstZ = -1;
            uint32_t firstRgba = 0;
            uint32_t bytes = 0;
            if (flg == 0u)
            {
                bytes = nloop * nreg * 16u;
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        const uint32_t q = off + (l * nreg + r) * 16u;
                        if (q + 16u > size)
                            break;
                        uint32_t w[4];
                        std::memcpy(w, p + q, 16);
                        const uint32_t reg = static_cast<uint32_t>((hi >> (r * 4u)) & 0xFu);
                        if (reg == 0x1u && verts == 0u)
                            firstRgba = (w[0] & 0xFF) | ((w[1] & 0xFF) << 8) | ((w[2] & 0xFF) << 16) | ((w[3] & 0xFF) << 24);
                        if (reg == 0x4u || reg == 0x5u)
                        {
                            if (verts == 0u)
                            {
                                firstX = static_cast<int>(w[0] & 0xFFFFu);
                                firstY = static_cast<int>(w[1] & 0xFFFFu);
                                firstZ = static_cast<int>(w[2]);
                            }
                            ++verts;
                            if (w[3] & 0x8000u)
                                ++adc;
                        }
                    }
            }
            else if (flg == 1u)
                bytes = ((nloop * nreg + 1u) / 2u) * 16u;
            else
                bytes = nloop * 16u;
            std::cerr << "    [kick] tag" << tags << " nloop=" << nloop << " eop=" << eop << " flg=" << flg << " pre=" << pre
                      << " prim=0x" << std::hex << prim << " regs=0x" << hi << std::dec << " verts=" << verts << " adc=" << adc
                      << " v0=(" << (firstX / 16.0) << "," << (firstY / 16.0) << "," << firstZ << ") rgba0=0x" << std::hex
                      << firstRgba << std::dec << std::endl;
            off += bytes;
            ++tags;
            if (eop)
                break;
        }
        if (off < size)
            std::cerr << "    [kick] " << (size - off) << " bytes after the tags shown" << std::endl;
    }
}

void VuFastEngine::registerAot(const AotImage &image) { aotRegistry().push_back(image); }

VuFastEngine::AotRunFn VuFastEngine::findAot(bool vu0, const uint8_t *code, uint32_t codeSize)
{
    if (!aotEnabled() || code == nullptr)
        return nullptr;
    for (const AotImage &img : aotRegistry())
    {
        if (img.vu0 != vu0)
            continue;
        bool match = true;
        for (uint32_t i = 0; i < img.pairCount && match; ++i)
        {
            const uint32_t pc = img.pairs[i * 3u];
            uint32_t words[2];
            if (pc + 8u > codeSize)
            {
                match = false;
                break;
            }
            std::memcpy(words, code + pc, 8);
            match = words[0] == img.pairs[i * 3u + 1u] && words[1] == img.pairs[i * 3u + 2u];
        }
        if (match)
            return img.run;
    }
    return nullptr;
}

void VuFastEngine::Impl::finishXgkick()
{
    if (!xg.active)
        return;
    ++g_vuFastKicks;
    if (g_vuFastTraceKicks)
        traceKick(xgPacket.data(), xg.total);
    if (sink)
        sink(xgPacket.data(), xg.total);
    else if (memory)
    {
        const auto t0 = std::chrono::steady_clock::now();
        memory->submitGifPacket(GifPathId::Path1, xgPacket.data(), xg.total);
        ps2_frame_log::counters().vu1KickNs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
            std::memory_order_relaxed);
    }
    else if (gs)
        gs->processGIFPacket(xgPacket.data(), xg.total);
    xg.active = false;
}

void VuFastEngine::Impl::begin(VU1State &state, uint8_t *d, uint32_t dSize, PS2Memory *mem, GS *g, const uint8_t *code)
{
    st = &state;
    data = d;
    dataSize = dSize;
    memory = mem;
    gs = g;
    stopRequested = false;
    const uint64_t codeGen = mem ? (vu0 ? mem->getVU0CodeGeneration() : mem->getVU1CodeGeneration()) : externalGeneration;
    const bool known = mem != nullptr || externalGeneration != 0u;
    if (!known || codeGen != cachedCodeGeneration || code != cachedCode)
    {
        ++generation; // lazily re-decode every pair
        cachedCodeGeneration = known ? codeGen : ~0ull;
        cachedCode = code;
    }
    if (aotGeneration != generation)
    {
        aotRun = VuFastEngine::findAot(vu0, code, vu0 ? 0x1000u : 0x4000u);
        aotGeneration = generation;
    }
    if (flagsAlwaysTracked())
        trackMacStatus = true;
    else if (flagScanGeneration != generation)
    {
        scanFlagReaders(code, vu0 ? 0x1000u : 0x4000u);
        flagScanGeneration = generation;
    }
}

VuFastEngine::VuFastEngine(bool vu0) : m_impl(std::make_unique<Impl>())
{
    m_impl->vu0 = vu0;
    m_impl->pcMask = vu0 ? 0x0FFFu : 0x3FFFu;
}
VuFastEngine::~VuFastEngine() = default;

void VuFastEngine::setPacketSink(PacketSink sink) { m_impl->sink = std::move(sink); }
void VuFastEngine::setCodeGeneration(uint64_t generation) { m_impl->externalGeneration = generation; }

void VuFastEngine::execute(VU1State &state, const uint8_t *code, uint32_t codeSize, uint8_t *data, uint32_t dataSize,
                           PS2Memory *memory, GS *gs, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    Impl &m = *m_impl;
    m.begin(state, data, dataSize, memory, gs, code);
    if (m.pipelinesPending())
        m.flush(); // previous program did not finish within its budget: finish its side effects first
    m.workingClip = state.clip;
    m.viBackupValid = false;
    m.viBackupReg = 0;
    m.viBackupValue = 0;
    m.pendingHaltD = m.pendingHaltT = false;
    state.pc = startPC & m.pcMask;
    state.ebit = false;
    state.haltAfterDelaySlot = false;
    state.stoppedByD = false;
    state.stoppedByT = false;
    state.top = top;
    state.itop = itop;
    state.branchPending = false;
    state.branchTarget = 0;
    state.branchDelay = 0;
    state.vf[0][0] = 0.0f;
    state.vf[0][1] = 0.0f;
    state.vf[0][2] = 0.0f;
    state.vf[0][3] = 1.0f;
    m.run(code, codeSize, maxCycles);
}

void VuFastEngine::resume(VU1State &state, const uint8_t *code, uint32_t codeSize, uint8_t *data, uint32_t dataSize,
                          PS2Memory *memory, GS *gs, uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    Impl &m = *m_impl;
    m.begin(state, data, dataSize, memory, gs, code);
    state.top = top;
    state.itop = itop;
    state.stoppedByD = false;
    state.stoppedByT = false;
    m.run(code, codeSize, maxCycles);
}
