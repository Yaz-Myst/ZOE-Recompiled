#pragma once

// Internals of VuFastEngine (ps2_vu_fast.cpp), shared with ahead-of-time compiled micro programs: generated C++
// (ps2_vu_aotgen.cpp writes it) runs on the same Impl state, so it can hand a program back to the interpreter at any
// point and the two can be compared call for call. Not part of the generated EE code's include chain.
//
// The helpers live in an anonymous namespace: each including translation unit gets its own copy.

#include "runtime/ps2_vu_fast.h"
#include "ps2_frame_log.h"
#include "runtime/ps2_vu1.h"
#include "../../src/lib/vu/ps2_vu1_detail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <immintrin.h>
#include <smmintrin.h>

// Profiling aid: define as __declspec(noinline) to keep the hot helpers visible to the sampling profiler.
#define VUF_PROFILE_NOINLINE
// The compiled programs are one very large function per image, where the compiler's inlining budget runs out: the
// small helpers they use must be forced inline.
#if defined(_MSC_VER)
#define VUF_FORCEINLINE __forceinline
#else
#define VUF_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace
{
    constexpr uint32_t kFmacLatency = 4u;
    constexpr uint32_t kAccForwardLatency = 1u;
    constexpr uint32_t kMaxPairs = 0x4000u / 8u;
    constexpr uint32_t kXgkickBufferSize = 64u * 1024u;

    constexpr uint8_t laneBit(uint32_t component) { return static_cast<uint8_t>(8u >> component); } // x=8 y=4 z=2 w=1

    // --- operand/result normalization (VU floats: no denormals, no inf/NaN) ---------------------------------------
    VUF_FORCEINLINE __m128 normalize4(__m128 v)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i exp = _mm_and_si128(bits, expMask);
        const __m128i isZeroExp = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
        const __m128i isMaxExp = _mm_cmpeq_epi32(exp, expMask);
        __m128i out = _mm_blendv_epi8(bits, sign, isZeroExp);
        out = _mm_blendv_epi8(out, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), isMaxExp);
        return _mm_castsi128_ps(out);
    }

    VUF_FORCEINLINE float normalize1(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // Per-lane FMAC condition masks, as _mm_movemask_ps bits (bit 0 = x ... bit 3 = w).
    struct FmacFlags
    {
        uint32_t z = 0, s = 0, u = 0, o = 0;
    };

    // Normalizes a result (denormal -> signed zero, exponent 255 -> +/-FLT_MAX) and reports Z/S/U/O per lane:
    // zero or flushed denormal sets Z (and U for the denormal), sign sets S, exponent 255 sets O.
    inline __m128 normalizeResultFast(__m128 v, FmacFlags &f)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i mag = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i exp = _mm_and_si128(bits, expMask);
        const __m128i isZero = _mm_cmpeq_epi32(mag, _mm_setzero_si128());
        const __m128i isExp0 = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
        const __m128i isMaxExp = _mm_cmpeq_epi32(exp, expMask);
        __m128i out = _mm_blendv_epi8(bits, sign, isExp0);
        out = _mm_blendv_epi8(out, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), isMaxExp);
        f.z = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isExp0)));        // zero or denormal
        f.s = static_cast<uint32_t>(_mm_movemask_ps(v));                                // sign bits
        f.u = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_andnot_si128(isZero, isExp0))));
        f.o = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isMaxExp)));
        return _mm_castsi128_ps(out);
    }

    // movemask lane order (bit0 = x) <-> VU dest/MAC lane order (bit3 = x)
    constexpr uint8_t kReverse4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};

    // Normalizes a result and returns per-lane Z/S/U/O (bits 0..3) packed as 4 bits per lane (x in bits 0-3).
    inline __m128 normalizeResult4(__m128 v, uint32_t &laneFlagsPacked)
    {
        alignas(16) uint32_t bits[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(bits), _mm_castps_si128(v));
        laneFlagsPacked = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            uint32_t b = bits[c];
            const uint32_t sign = b & 0x80000000u;
            const uint32_t magnitude = b & 0x7FFFFFFFu;
            const uint32_t exponent = (b >> 23) & 0xFFu;
            uint32_t f = sign ? 0x2u : 0u;
            if (magnitude == 0u)
                f |= 0x1u;
            else if (exponent == 0u)
            {
                f |= 0x5u;
                b = sign;
            }
            else if (exponent == 0xFFu)
            {
                f |= 0x8u;
                b = sign | 0x7F7FFFFFu;
            }
            bits[c] = b;
            laneFlagsPacked |= f << (c * 4u);
        }
        return _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(bits)));
    }

    VUF_FORCEINLINE __m128i destMask(uint8_t dest)
    {
        return _mm_set_epi32((dest & 0x1u) ? -1 : 0, (dest & 0x2u) ? -1 : 0, (dest & 0x4u) ? -1 : 0, (dest & 0x8u) ? -1 : 0);
    }

    inline __m128 loadVf(const VU1State &st, uint32_t reg) { return _mm_loadu_ps(st.vf[reg]); }
    inline void storeVfMasked(VU1State &st, uint32_t reg, __m128 value, __m128i mask)
    {
        const __m128 old = _mm_loadu_ps(st.vf[reg]);
        _mm_storeu_ps(st.vf[reg], _mm_blendv_ps(old, value, _mm_castsi128_ps(mask)));
    }
    inline __m128 splat(__m128 v, uint32_t index)
    {
        switch (index & 3u)
        {
        case 0:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0));
        case 1:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1));
        case 2:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2));
        default:
            return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3));
        }
    }

    int32_t vuFloatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    float vuEatan(float value)
    {
        constexpr float coefficients[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
                                          0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f};
        constexpr float quarterPi = 0.785398185253143f;
        const float squared = value * value;
        float polynomial = coefficients[7];
        for (int index = 6; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return quarterPi + value * polynomial;
    }
    float vuEsin(float value)
    {
        constexpr float coefficients[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
        const float squared = value * value;
        float polynomial = coefficients[4];
        for (int index = 3; index >= 0; --index)
            polynomial = coefficients[index] + squared * polynomial;
        return value * polynomial;
    }
    float vuEexp(float value)
    {
        constexpr float coefficients[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                                          0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
        float polynomial = coefficients[5];
        for (int index = 4; index >= 0; --index)
            polynomial = coefficients[index] + value * polynomial;
        polynomial = 1.0f + value * polynomial;
        polynomial *= polynomial;
        polynomial *= polynomial;
        return polynomial != 0.0f ? 1.0f / polynomial : std::numeric_limits<float>::max();
    }

    // --- decoded form -------------------------------------------------------------------------------------------
    enum class Arith : uint8_t { Add, Sub, Mul, Madd, Msub, Max, Min };
    enum class BSrc : uint8_t { Vt, Bc, Q, I };
    enum class UKind : uint8_t { Nop, Arith, OpMula, OpMsub, Itof, Ftoi, Abs, Clip, Reserved };
    enum class Pipe : uint8_t { None, Fmac, Lsu, Fdiv, Efu, Ialu, Branch, Xgkick };

    struct VfRead
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct Usage
    {
        VfRead vfRead[2];
        uint8_t vfReadCount = 0;
        uint8_t vfWriteReg = 0;
        uint8_t vfWriteLanes = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipe pipe = Pipe::None;
        bool waitQ = false;
        bool waitP = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;

        void addVfRead(uint8_t reg, uint8_t lanes)
        {
            if (lanes == 0u)
                return;
            for (uint32_t i = 0; i < vfReadCount; ++i)
                if (vfRead[i].reg == reg)
                {
                    vfRead[i].lanes |= lanes;
                    return;
                }
            if (vfReadCount < 2u)
                vfRead[vfReadCount++] = {reg, lanes};
        }
        void addVfWrite(uint8_t reg, uint8_t lanes)
        {
            if (reg == 0u || lanes == 0u)
                return;
            if (vfWriteReg == 0u)
            {
                vfWriteReg = reg;
                vfWriteLanes = lanes;
            }
            else if (vfWriteReg == reg)
                vfWriteLanes |= lanes;
        }
        uint8_t readLanes(uint8_t reg) const
        {
            uint8_t lanes = 0;
            for (uint32_t i = 0; i < vfReadCount; ++i)
                if (vfRead[i].reg == reg)
                    lanes |= vfRead[i].lanes;
            return lanes;
        }
    };

    struct Upper
    {
        UKind kind = UKind::Nop;
        Arith arith = Arith::Add;
        BSrc bsrc = BSrc::Vt;
        uint8_t bc = 0;
        bool toAcc = false;
        bool flags = false; // FMAC op that updates MAC/status
        uint8_t fs = 0, ft = 0, fd = 0, dest = 0;
        uint8_t convShift = 0; // ITOF/FTOI: 0,4,12,15
        __m128i mask{};
    };

    struct Pair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        Upper up;
        Usage upperUsage;
        Usage lowerUsage;
        uint64_t generation = ~0ull;
        uint8_t lowerOpHi = 0;
        uint8_t lowerSpecial = 0xFF; // lower1 special selector, 0xFF if not special
        uint8_t upperShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
        bool iBit = false, eBit = false, dBit = false, tBit = false;
        bool readsFlags = false; // lower reads MAC/status/clip
        bool isNop = false;      // lower is NOP

        // Summary of both halves for the stall check and write bookkeeping (see pairReadyCycle / markWrites).
        VfRead reads[4];         // VF registers read, lanes merged per register
        uint8_t readCount = 0;
        uint32_t vfReadMask = 0; // bit per VF register read
        uint16_t viReadMask = 0;
        uint8_t accReadLanes = 0;
        bool scalarWait = false; // FDIV/EFU resource or WAITQ/WAITP: needs the slow checks
        uint8_t loVfReg = 0, loVfLanes = 0, loVfLatency = 0; // lower VF write (0 when suppressed by the upper)
        uint8_t upVfReg = 0, upVfLanes = 0, upVfLatency = 0;
        uint16_t viWriteMask = 0;
        uint8_t viWriteLatency = 0;
        uint8_t accWriteLanes = 0;
        bool storesData = false; // lower writes VU data memory (an active XGKICK must catch up first)
    };

    Usage decodeUpperUsage(uint32_t upper)
    {
        Usage usage;
        usage.pipe = Pipe::Fmac;
        usage.latency = kFmacLatency;
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const uint8_t dest = DEST(upper);
        const uint8_t fs = FS(upper);
        const uint8_t ft = FT(upper);
        const uint8_t fd = FD(upper);
        if (op <= 0x2Fu)
        {
            usage.addVfRead(fs, dest);
            usage.addVfWrite(fd, dest);
            if (op <= 0x1Bu)
                usage.addVfRead(ft, laneBit(op & 3u));
            else if (op >= 0x28u)
                usage.addVfRead(ft, op == 0x2Eu ? 0xEu : dest);
            if ((op >= 0x08u && op <= 0x0Fu) || op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u || op == 0x29u ||
                op == 0x2Du || op == 0x2Eu)
                usage.accRead = dest;
            return usage;
        }
        if (op >= 0x3Cu)
        {
            const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
            const bool writesAcc = special <= 0x0Fu || (special >= 0x18u && special <= 0x1Cu) || special == 0x1Eu ||
                                   (special >= 0x20u && special <= 0x2Au) || (special >= 0x2Cu && special <= 0x2Eu);
            if (writesAcc)
            {
                usage.addVfRead(fs, dest);
                if (special <= 0x1Bu)
                    usage.addVfRead(ft, laneBit(special & 3u));
                else if (special >= 0x28u && special <= 0x2Eu)
                    usage.addVfRead(ft, special == 0x2Eu ? 0xEu : dest);
                usage.accWrite = dest;
                if ((special >= 0x08u && special <= 0x0Fu) || special == 0x21u || special == 0x23u || special == 0x25u ||
                    special == 0x27u || special == 0x29u || special == 0x2Du)
                    usage.accRead = dest;
            }
            else if ((special >= 0x10u && special <= 0x17u) || special == 0x1Du)
            {
                usage.addVfRead(fs, dest);
                usage.addVfWrite(ft, dest);
            }
            else if (special == 0x1Fu)
            {
                usage.addVfRead(fs, 0xEu);
                usage.addVfRead(ft, 0x1u);
            }
            else if (special != 0x2Fu && special != 0x30u)
                usage.reserved = true;
            return usage;
        }
        usage.reserved = true;
        return usage;
    }

    Usage decodeLowerUsage(uint32_t lower)
    {
        Usage usage;
        if (lower == 0u || lower == 0x8000033Cu)
            return usage;
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        const uint8_t vfT = FT(lower), vfS = FS(lower);
        const uint8_t viT = VIT(lower), viS = VIS(lower), viD = VID(lower);
        const uint8_t dest = DEST(lower);
        auto readVi = [&](uint8_t reg)
        {
            if (reg != 0u)
                usage.viRead |= static_cast<uint16_t>(1u << reg);
        };
        auto writeVi = [&](uint8_t reg)
        {
            if (reg != 0u)
                usage.viWrite |= static_cast<uint16_t>(1u << reg);
        };
        switch (opHi)
        {
        case 0x00:
            usage.pipe = Pipe::Lsu;
            usage.latency = 4u;
            readVi(viS);
            usage.addVfWrite(vfT, dest);
            return usage;
        case 0x01:
            usage.pipe = Pipe::Lsu;
            usage.latency = 1u;
            readVi(viT);
            usage.addVfRead(vfS, dest);
            return usage;
        case 0x04:
            usage.pipe = Pipe::Lsu;
            usage.latency = 4u;
            readVi(viS);
            writeVi(viT);
            return usage;
        case 0x05:
            usage.pipe = Pipe::Lsu;
            usage.latency = 1u;
            readVi(viS);
            readVi(viT);
            return usage;
        case 0x08:
        case 0x09:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            usage.delaysNextBranchRead = true;
            readVi(viS);
            writeVi(viT);
            return usage;
        case 0x10:
        case 0x12:
        case 0x13:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            writeVi(1u);
            return usage;
        case 0x11:
        case 0x15:
            usage.pipe = Pipe::Fmac;
            usage.latency = kFmacLatency;
            return usage;
        case 0x14:
        case 0x16:
        case 0x17:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            writeVi(viT);
            return usage;
        case 0x18:
        case 0x1A:
        case 0x1B:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            readVi(viS);
            writeVi(viT);
            return usage;
        case 0x1C:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            writeVi(viT);
            return usage;
        case 0x20:
            usage.pipe = Pipe::Branch;
            return usage;
        case 0x21:
            usage.pipe = Pipe::Branch;
            usage.latency = 1u;
            writeVi(viT);
            return usage;
        case 0x24:
            usage.pipe = Pipe::Branch;
            readVi(viS);
            return usage;
        case 0x25:
            usage.pipe = Pipe::Branch;
            usage.latency = 1u;
            readVi(viS);
            writeVi(viT);
            return usage;
        case 0x28:
        case 0x29:
            usage.pipe = Pipe::Branch;
            readVi(viS);
            readVi(viT);
            return usage;
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
            usage.pipe = Pipe::Branch;
            readVi(viS);
            return usage;
        case 0x40:
            break;
        default:
            usage.reserved = true;
            return usage;
        }

        const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
        if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
        {
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            usage.delaysNextBranchRead = true;
            readVi(viS);
            readVi(viT);
            writeVi(viD);
            return usage;
        }
        if (direct == 0x32u)
        {
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            usage.delaysNextBranchRead = true;
            readVi(viS);
            writeVi(viT);
            return usage;
        }
        if (direct < 0x3Cu)
        {
            usage.reserved = true;
            return usage;
        }
        const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
        switch (special)
        {
        case 0x30:
        case 0x31:
            usage.pipe = Pipe::Fmac;
            usage.latency = 4u;
            usage.addVfRead(vfS, special == 0x31u ? 0xFu : dest);
            usage.addVfWrite(vfT, dest);
            break;
        case 0x34:
        case 0x36:
            usage.pipe = Pipe::Lsu;
            usage.latency = 4u;
            usage.viLatency = 1u;
            usage.delaysNextBranchRead = true;
            readVi(viS);
            writeVi(viS);
            usage.addVfWrite(vfT, dest);
            break;
        case 0x35:
        case 0x37:
            usage.pipe = Pipe::Lsu;
            usage.latency = 1u;
            usage.delaysNextBranchRead = true;
            readVi(viT);
            writeVi(viT);
            usage.addVfRead(vfS, dest);
            break;
        case 0x38:
            usage.pipe = Pipe::Fdiv;
            usage.latency = 7u;
            usage.addVfRead(vfS, laneBit((lower >> 21) & 3u));
            usage.addVfRead(vfT, laneBit((lower >> 23) & 3u));
            break;
        case 0x39:
            usage.pipe = Pipe::Fdiv;
            usage.latency = 7u;
            usage.addVfRead(vfT, laneBit((lower >> 23) & 3u));
            break;
        case 0x3A:
            usage.pipe = Pipe::Fdiv;
            usage.latency = 13u;
            usage.addVfRead(vfS, laneBit((lower >> 21) & 3u));
            usage.addVfRead(vfT, laneBit((lower >> 23) & 3u));
            break;
        case 0x3B:
            usage.pipe = Pipe::Fdiv;
            usage.waitQ = true;
            break;
        case 0x3C:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            usage.delaysNextBranchRead = true;
            usage.addVfRead(vfS, laneBit((lower >> 21) & 3u));
            writeVi(viT);
            break;
        case 0x3D:
            usage.pipe = Pipe::Fmac;
            usage.latency = 4u;
            readVi(viS);
            usage.addVfWrite(vfT, dest);
            break;
        case 0x3E:
            usage.pipe = Pipe::Lsu;
            usage.latency = 4u;
            readVi(viS);
            writeVi(viT);
            break;
        case 0x3F:
            usage.pipe = Pipe::Lsu;
            usage.latency = 1u;
            readVi(viS);
            readVi(viT);
            break;
        case 0x40:
        case 0x41:
        case 0x64:
            usage.pipe = Pipe::Fmac;
            usage.latency = 4u;
            usage.addVfWrite(vfT, dest);
            break;
        case 0x42:
        case 0x43:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            usage.addVfRead(vfS, laneBit((lower >> 21) & 3u));
            break;
        case 0x68:
        case 0x69:
            usage.pipe = Pipe::Ialu;
            usage.latency = 1u;
            writeVi(viT);
            break;
        case 0x6C:
            usage.pipe = Pipe::Xgkick;
            usage.latency = 2u;
            readVi(viS);
            break;
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7C:
        case 0x7D:
            usage.pipe = Pipe::Efu;
            switch (special)
            {
            case 0x70:
                usage.latency = 11u;
                break;
            case 0x71:
            case 0x72:
            case 0x77:
                usage.latency = 18u;
                break;
            case 0x73:
                usage.latency = 24u;
                break;
            case 0x74:
            case 0x75:
            case 0x7C:
                usage.latency = 54u;
                break;
            case 0x76:
            case 0x78:
            case 0x7A:
                usage.latency = 12u;
                break;
            case 0x79:
                usage.latency = 29u;
                break;
            case 0x7D:
                usage.latency = 44u;
                break;
            default:
                break;
            }
            if (special >= 0x70u && special <= 0x73u)
                usage.addVfRead(vfS, 0xEu);
            else if (special == 0x74u)
                usage.addVfRead(vfS, 0xCu);
            else if (special == 0x75u)
                usage.addVfRead(vfS, 0xAu);
            else if (special == 0x76u)
                usage.addVfRead(vfS, 0xFu);
            else
                usage.addVfRead(vfS, laneBit((lower >> 21) & 3u));
            break;
        case 0x7B:
            usage.pipe = Pipe::Efu;
            usage.waitP = true;
            break;
        default:
            usage.reserved = true;
            break;
        }
        return usage;
    }

    Upper decodeUpper(uint32_t instr)
    {
        Upper u;
        u.dest = DEST(instr);
        u.fs = FS(instr);
        u.ft = FT(instr);
        u.fd = FD(instr);
        u.mask = destMask(u.dest);
        const uint8_t op = static_cast<uint8_t>(instr & 0x3Fu);
        auto arith = [&](Arith a, BSrc b, uint8_t bc, bool toAcc, bool flags)
        {
            u.kind = UKind::Arith;
            u.arith = a;
            u.bsrc = b;
            u.bc = bc;
            u.toAcc = toAcc;
            u.flags = flags;
        };
        if (op <= 0x2Fu)
        {
            switch (op)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: arith(Arith::Add, BSrc::Bc, op & 3u, false, true); break;
            case 0x04: case 0x05: case 0x06: case 0x07: arith(Arith::Sub, BSrc::Bc, op & 3u, false, true); break;
            case 0x08: case 0x09: case 0x0A: case 0x0B: arith(Arith::Madd, BSrc::Bc, op & 3u, false, true); break;
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(Arith::Msub, BSrc::Bc, op & 3u, false, true); break;
            case 0x10: case 0x11: case 0x12: case 0x13: arith(Arith::Max, BSrc::Bc, op & 3u, false, false); break;
            case 0x14: case 0x15: case 0x16: case 0x17: arith(Arith::Min, BSrc::Bc, op & 3u, false, false); break;
            case 0x18: case 0x19: case 0x1A: case 0x1B: arith(Arith::Mul, BSrc::Bc, op & 3u, false, true); break;
            case 0x1C: arith(Arith::Mul, BSrc::Q, 0, false, true); break;
            case 0x1D: arith(Arith::Max, BSrc::I, 0, false, false); break;
            case 0x1E: arith(Arith::Mul, BSrc::I, 0, false, true); break;
            case 0x1F: arith(Arith::Min, BSrc::I, 0, false, false); break;
            case 0x20: arith(Arith::Add, BSrc::Q, 0, false, true); break;
            case 0x21: arith(Arith::Madd, BSrc::Q, 0, false, true); break;
            case 0x22: arith(Arith::Add, BSrc::I, 0, false, true); break;
            case 0x23: arith(Arith::Madd, BSrc::I, 0, false, true); break;
            case 0x24: arith(Arith::Sub, BSrc::Q, 0, false, true); break;
            case 0x25: arith(Arith::Msub, BSrc::Q, 0, false, true); break;
            case 0x26: arith(Arith::Sub, BSrc::I, 0, false, true); break;
            case 0x27: arith(Arith::Msub, BSrc::I, 0, false, true); break;
            case 0x28: arith(Arith::Add, BSrc::Vt, 0, false, true); break;
            case 0x29: arith(Arith::Madd, BSrc::Vt, 0, false, true); break;
            case 0x2A: arith(Arith::Mul, BSrc::Vt, 0, false, true); break;
            case 0x2B: arith(Arith::Max, BSrc::Vt, 0, false, false); break;
            case 0x2C: arith(Arith::Sub, BSrc::Vt, 0, false, true); break;
            case 0x2D: arith(Arith::Msub, BSrc::Vt, 0, false, true); break;
            case 0x2E: u.kind = UKind::OpMsub; u.flags = true; break;
            case 0x2F: arith(Arith::Min, BSrc::Vt, 0, false, false); break;
            default: u.kind = UKind::Reserved; break;
            }
            return u;
        }
        if (op < 0x3Cu)
        {
            u.kind = UKind::Reserved;
            return u;
        }
        const uint8_t s = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        switch (s)
        {
        case 0x00: case 0x01: case 0x02: case 0x03: arith(Arith::Add, BSrc::Bc, s & 3u, true, true); break;
        case 0x04: case 0x05: case 0x06: case 0x07: arith(Arith::Sub, BSrc::Bc, s & 3u, true, true); break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: arith(Arith::Madd, BSrc::Bc, s & 3u, true, true); break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(Arith::Msub, BSrc::Bc, s & 3u, true, true); break;
        case 0x10: u.kind = UKind::Itof; u.convShift = 0; break;
        case 0x11: u.kind = UKind::Itof; u.convShift = 4; break;
        case 0x12: u.kind = UKind::Itof; u.convShift = 12; break;
        case 0x13: u.kind = UKind::Itof; u.convShift = 15; break;
        case 0x14: u.kind = UKind::Ftoi; u.convShift = 0; break;
        case 0x15: u.kind = UKind::Ftoi; u.convShift = 4; break;
        case 0x16: u.kind = UKind::Ftoi; u.convShift = 12; break;
        case 0x17: u.kind = UKind::Ftoi; u.convShift = 15; break;
        case 0x18: case 0x19: case 0x1A: case 0x1B: arith(Arith::Mul, BSrc::Bc, s & 3u, true, true); break;
        case 0x1C: arith(Arith::Mul, BSrc::Q, 0, true, true); break;
        case 0x1D: u.kind = UKind::Abs; break;
        case 0x1E: arith(Arith::Mul, BSrc::I, 0, true, true); break;
        case 0x1F: u.kind = UKind::Clip; break;
        case 0x20: arith(Arith::Add, BSrc::Q, 0, true, true); break;
        case 0x21: arith(Arith::Madd, BSrc::Q, 0, true, true); break;
        case 0x22: arith(Arith::Add, BSrc::I, 0, true, true); break;
        case 0x23: arith(Arith::Madd, BSrc::I, 0, true, true); break;
        case 0x24: arith(Arith::Sub, BSrc::Q, 0, true, true); break;
        case 0x25: arith(Arith::Msub, BSrc::Q, 0, true, true); break;
        case 0x26: arith(Arith::Sub, BSrc::I, 0, true, true); break;
        case 0x27: arith(Arith::Msub, BSrc::I, 0, true, true); break;
        case 0x28: arith(Arith::Add, BSrc::Vt, 0, true, true); break;
        case 0x29: arith(Arith::Madd, BSrc::Vt, 0, true, true); break;
        case 0x2A: arith(Arith::Mul, BSrc::Vt, 0, true, true); break;
        case 0x2C: arith(Arith::Sub, BSrc::Vt, 0, true, true); break;
        case 0x2D: arith(Arith::Msub, BSrc::Vt, 0, true, true); break;
        case 0x2E: u.kind = UKind::OpMula; u.flags = true; break;
        case 0x2F: case 0x30: u.kind = UKind::Nop; break;
        default: u.kind = UKind::Reserved; break;
        }
        return u;
    }

    struct FlagEntry
    {
        uint64_t issueCycle = 0;
        uint64_t readyCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t clip = 0;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };
}

// PS2X_VU_FLAGS=1: always compute MAC/status flags (exact final flag registers, e.g. for lockstep verification).
static bool flagsAlwaysTracked()
{
    static const bool always = []
    {
        const char *env = std::getenv("PS2X_VU_FLAGS");
        return (env && *env == '1') || std::getenv("PS2X_VU_VERIFY") != nullptr;
    }();
    return always;
}

// Read-only analysis hooks for the VU replay bench (deps=): called before every pair the interpreter loop executes
// (instruction words and the state it reads), and for every qword an XGKICK reads. Null in normal runs.
namespace ps2_vu_analysis
{
    inline void (*g_pair)(uint32_t lower, uint32_t upper, const VU1State &state) = nullptr;
    inline void (*g_xgkickRead)(uint32_t byteAddr) = nullptr;
}

struct VuFastEngine::Impl
{
    bool vu0 = false;
    uint32_t pcMask = 0x3FFFu;
    // program cache
    std::vector<Pair> pairs = std::vector<Pair>(kMaxPairs);
    uint64_t generation = 1;
    uint64_t cachedCodeGeneration = ~0ull;
    uint64_t externalGeneration = 0; // setCodeGeneration (no PS2Memory)
    const uint8_t *cachedCode = nullptr;

    // timing
    uint64_t cycle = 0;
    uint64_t vfReady[32][4]{};
    uint64_t vfRegReady[32]{}; // upper bound of vfReady[reg][*]: one compare rules out a stall in the common case
    uint64_t viReady[16]{};
    uint64_t accReady[4]{};
    uint64_t accMaxReady = 0;
    // Registers whose last write may still be in flight (ready > cycle). Cleared lazily by pairReadyCycle, so the
    // common no-stall pair costs one AND.
    uint32_t vfBusy = 0;
    uint32_t viBusy = 0;
    // Earliest pending Q/P write (UINT64_MAX when none): commitScalars runs only when it is due.
    uint64_t nextScalarCycle = ~0ull;
    // Micro programs that never read the MAC/status flags (no FSAND/FSEQ/FSOR/FMAND/FMEQ/FMOR anywhere in micro
    // memory) skip computing them. On VU0 the EE could also read them with CFC2 after a VCALLMS; ZOE only reads them
    // after macro-mode instructions (sceVu0ClipScreen & co. clear the status first). PS2X_VU_FLAGS=1 always computes.
    bool trackMacStatus = true;
    uint64_t flagScanGeneration = ~0ull;

    // flags FIFO (MAC/status/sticky/clip), ready order == issue order (constant latency)
    FlagEntry flags[64];
    uint32_t flagHead = 0, flagCount = 0;
    uint32_t workingClip = 0;

    // Q / P
    bool qPending = false;
    uint64_t qReady = 0;
    float qValue = 0.0f;
    uint32_t qStatusDi = 0;
    struct PEntry
    {
        bool valid = false;
        uint64_t ready = 0;
        float value = 0.0f;
    } p[2];
    uint64_t efuResourceReady = 0;

    // branch-read quirk
    bool viBackupValid = false;
    uint8_t viBackupReg = 0;
    int32_t viBackupValue = 0;
    bool pendingHaltD = false, pendingHaltT = false;

    // XGKICK
    struct Xgkick
    {
        bool active = false;
        uint32_t source = 0;
        uint32_t copied = 0;
        uint32_t tagEnd = 0;
        bool tagEop = false;
        uint32_t total = 0;
        uint64_t credit = 0;
        uint64_t lastCycle = 0;
    } xg;
    std::vector<uint8_t> xgPacket = std::vector<uint8_t>(kXgkickBufferSize);

    // active run
    VU1State *st = nullptr;
    uint8_t *data = nullptr;
    uint32_t dataSize = 0;
    PS2Memory *memory = nullptr;
    GS *gs = nullptr;
    PacketSink sink;
    bool stopRequested = false;

    // Ahead-of-time compiled program image matching the current micro memory (checked when the code changes).
    VuFastEngine::AotRunFn aotRun = nullptr;
    uint64_t aotGeneration = ~0ull;

    // ---- decode cache ------------------------------------------------------------------------------------------
    VUF_PROFILE_NOINLINE const Pair &pairAt(const uint8_t *code, uint32_t pc)
    {
        Pair &pr = pairs[(pc >> 3) & (kMaxPairs - 1u)];
        if (pr.generation == generation)
            return pr;
        uint32_t lower, upper;
        std::memcpy(&lower, code + pc, 4);
        std::memcpy(&upper, code + pc + 4, 4);
        pr = decodePairWords(lower, upper, vu0);
        pr.generation = generation;
        return pr;
    }

    static Pair decodePairWords(uint32_t lower, uint32_t upper, bool isVu0)
    {
        Pair pr{};
        pr.lower = lower;
        pr.upper = upper;
        pr.iBit = (pr.upper & 0x80000000u) != 0u;
        pr.eBit = (pr.upper & 0x40000000u) != 0u;
        pr.dBit = (pr.upper & 0x10000000u) != 0u;
        pr.tBit = (pr.upper & 0x08000000u) != 0u;
        pr.up = decodeUpper(pr.upper);
        pr.upperUsage = decodeUpperUsage(pr.upper);
        if (!pr.iBit)
        {
            pr.lowerUsage = decodeLowerUsage(pr.lower);
            pr.lowerOpHi = static_cast<uint8_t>((pr.lower >> 25) & 0x7Fu);
            pr.isNop = pr.lower == 0u || pr.lower == 0x8000033Cu;
            if (pr.lowerOpHi == 0x40u && (pr.lower & 0x3Fu) >= 0x3Cu)
                pr.lowerSpecial = static_cast<uint8_t>((pr.lower & 3u) | ((pr.lower >> 4) & 0x7Cu));
            const uint8_t h = pr.lowerOpHi;
            pr.readsFlags = !pr.isNop && ((h >= 0x10u && h <= 0x1Cu && h != 0x11u && h != 0x15u));
        }
        if (isVu0 && pr.lowerSpecial != 0xFFu &&
            (pr.lowerSpecial == 0x64u || pr.lowerSpecial == 0x6Cu || pr.lowerSpecial == 0x7Bu ||
             (pr.lowerSpecial >= 0x70u && pr.lowerSpecial <= 0x7Du)))
            pr.lowerUsage.reserved = true;
        const uint8_t upperWrite = pr.upperUsage.vfWriteReg;
        if (upperWrite != 0u && (pr.lowerUsage.readLanes(upperWrite) != 0u || pr.lowerUsage.vfWriteReg == upperWrite))
        {
            pr.upperShadowReg = upperWrite;
            if (pr.lowerUsage.vfWriteReg == upperWrite)
                pr.suppressedLowerVf = upperWrite;
        }
        summarize(pr);
        return pr;
    }

    static void summarize(Pair &pr)
    {
        const Usage *usages[2] = {&pr.upperUsage, &pr.lowerUsage};
        for (const Usage *u : usages)
        {
            for (uint32_t i = 0; i < u->vfReadCount; ++i)
            {
                const VfRead &r = u->vfRead[i];
                if (r.lanes == 0u)
                    continue;
                bool merged = false;
                for (uint32_t k = 0; k < pr.readCount; ++k)
                    if (pr.reads[k].reg == r.reg)
                    {
                        pr.reads[k].lanes |= r.lanes;
                        merged = true;
                    }
                if (!merged)
                    pr.reads[pr.readCount++] = r;
                pr.vfReadMask |= 1u << r.reg;
            }
            pr.viReadMask |= u->viRead;
            pr.accReadLanes |= u->accRead;
        }
        const Usage &lo = pr.lowerUsage;
        pr.scalarWait = lo.pipe == Pipe::Fdiv || lo.pipe == Pipe::Efu || lo.waitQ || lo.waitP;
        if (lo.vfWriteReg != 0u && pr.suppressedLowerVf != lo.vfWriteReg)
        {
            pr.loVfReg = lo.vfWriteReg;
            pr.loVfLanes = lo.vfWriteLanes;
            pr.loVfLatency = lo.vfLatency != 0u ? lo.vfLatency : lo.latency;
        }
        const Usage &up = pr.upperUsage;
        if (up.vfWriteReg != 0u)
        {
            pr.upVfReg = up.vfWriteReg;
            pr.upVfLanes = up.vfWriteLanes;
            pr.upVfLatency = up.vfLatency != 0u ? up.vfLatency : up.latency;
        }
        pr.viWriteMask = lo.viWrite;
        pr.viWriteLatency = lo.viLatency != 0u ? lo.viLatency : lo.latency;
        pr.accWriteLanes = up.accWrite;
        if (!pr.iBit && !pr.isNop)
        {
            const uint8_t h = pr.lowerOpHi;
            pr.storesData = h == 0x01u || h == 0x05u ||
                            (h == 0x40u && (pr.lowerSpecial == 0x35u || pr.lowerSpecial == 0x37u || pr.lowerSpecial == 0x3Fu));
        }
    }

    // ---- pipelines ---------------------------------------------------------------------------------------------
    FlagEntry &pushFlag()
    {
        if (flagCount == 64u)
            commitFlags(cycle); // entries still in flight are at most a few cycles' worth
        if (flagCount == 64u)
            commitFlags(~0ull >> 1); // never overflow
        FlagEntry &e = flags[(flagHead + flagCount) & 63u];
        ++flagCount;
        e = FlagEntry{};
        e.issueCycle = cycle;
        e.readyCycle = cycle + kFmacLatency;
        return e;
    }

    void commitFlags(uint64_t upTo)
    {
        while (flagCount != 0u)
        {
            FlagEntry &e = flags[flagHead];
            if (e.readyCycle > upTo)
                break;
            if (e.writesMac)
                st->mac = e.mac;
            if (e.writesStatus)
            {
                const uint32_t current = e.status & 0xFu;
                st->status = (st->status & 0xFF0u) | current | (current << 6);
            }
            if (e.writesSticky)
                st->status = (st->status & 0x03Fu) | (e.status & 0xFC0u);
            if (e.writesClip)
                st->clip = e.clip;
            flagHead = (flagHead + 1u) & 63u;
            --flagCount;
        }
    }

    VUF_PROFILE_NOINLINE void commitScalars()
    {
        if (cycle < nextScalarCycle)
            return;
        if (qPending && qReady <= cycle)
        {
            st->q = qValue;
            const uint32_t currentDi = qStatusDi & 0x30u;
            st->status = (st->status & 0xFCFu) | currentDi | (currentDi << 6);
            qPending = false;
        }
        for (PEntry &e : p)
            if (e.valid && e.ready <= cycle)
            {
                st->p = e.value;
                e.valid = false;
            }
        recomputeNextScalar();
    }

    void recomputeNextScalar()
    {
        uint64_t next = qPending ? qReady : ~0ull;
        for (const PEntry &e : p)
            if (e.valid)
                next = std::min(next, e.ready);
        nextScalarCycle = next;
    }

    void queueQ(float value, uint32_t latency, uint32_t statusDi)
    {
        value = normalize1(value);
        qPending = true;
        qReady = cycle + latency;
        qValue = value;
        qStatusDi = statusDi & 0x30u;
        recomputeNextScalar();
    }

    void queueP(float value, uint32_t latency)
    {
        value = normalize1(value);
        for (PEntry &e : p)
            if (!e.valid)
            {
                e.valid = true;
                e.ready = cycle + latency;
                e.value = value;
                efuResourceReady = cycle + (latency > 0u ? latency - 1u : 0u);
                recomputeNextScalar();
                return;
            }
        // both slots busy: the stall logic prevents this; keep the newest
        p[1] = {true, cycle + latency, value};
        recomputeNextScalar();
    }

    void store(uint32_t address, const uint32_t words[4], uint8_t lanes)
    {
        if (address + 16u > dataSize)
            return;
        uint32_t cur[4];
        std::memcpy(cur, data + address, 16);
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & laneBit(c))
                cur[c] = words[c];
        std::memcpy(data + address, cur, 16);
    }

    // ---- XGKICK ------------------------------------------------------------------------------------------------
    void finishXgkick(); // ps2_vu_fast.cpp (needs PS2Memory / GS)

    VUF_PROFILE_NOINLINE void progressXgkick()
    {
        if (!xg.active)
            return;
        xg.credit += cycle - xg.lastCycle;
        xg.lastCycle = cycle;
        const uint64_t copied = xgkickCopy(xg.credit / 2u);
        xg.credit -= 2u * copied;
    }

    // Copies up to maxQwords of the active XGKICK (fewer when the packet ends); returns how many were copied.
    uint64_t xgkickCopy(uint64_t maxQwords)
    {
        uint64_t n = 0;
        while (xg.active && n < maxQwords)
        {
            ++n;
            if (xg.copied > kXgkickBufferSize - 16u)
            {
                std::cerr << "[vu1:fast] XGKICK packet overflow" << std::endl;
                xg.active = false;
                return n;
            }
            const uint32_t qwordOffset = xg.copied;
            const uint32_t src = (xg.source + xg.copied) % dataSize;
            if (ps2_vu_analysis::g_xgkickRead)
                ps2_vu_analysis::g_xgkickRead(src);
            if (src + 16u <= dataSize)
                std::memcpy(xgPacket.data() + xg.copied, data + src, 16);
            else
                for (uint32_t i = 0; i < 16u; ++i)
                    xgPacket[xg.copied + i] = data[(xg.source + xg.copied + i) % dataSize];
            xg.copied += 16u;
            if (xg.tagEnd == 0u)
            {
                uint64_t tagLo = 0;
                std::memcpy(&tagLo, xgPacket.data() + qwordOffset, sizeof(tagLo));
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                uint64_t tagBytes = 16u;
                if (format == 0u)
                    tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
                else if (format == 1u)
                    tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
                else if (format == 2u)
                    tagBytes += static_cast<uint64_t>(nloop) * 16u;
                else
                {
                    std::cerr << "[vu1:fast] XGKICK bad GIFtag format" << std::endl;
                    xg.active = false;
                    return n;
                }
                if (tagBytes > kXgkickBufferSize - qwordOffset)
                {
                    std::cerr << "[vu1:fast] XGKICK GIFtag too large" << std::endl;
                    xg.active = false;
                    return n;
                }
                xg.tagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
                xg.tagEop = ((tagLo >> 15) & 1u) != 0u;
                if (xg.tagEop)
                    xg.total = xg.tagEnd;
            }
            if (xg.copied >= xg.tagEnd)
            {
                if (xg.tagEop)
                    finishXgkick();
                else
                {
                    xg.tagEnd = 0u;
                    xg.tagEop = false;
                }
            }
        }
        return n;
    }

    void startXgkick(uint32_t qwordAddress)
    {
        if (dataSize < 16u)
            return;
        xg = Xgkick{};
        xg.active = true;
        xg.source = (qwordAddress * 16u) % dataSize;
        xg.credit = 1u; // the issue cycle counts toward PATH1
        xg.lastCycle = cycle;
    }

    // ---- time --------------------------------------------------------------------------------------------------
    // The PATH1 copy of an active XGKICK is brought up to date lazily - before any store to VU data memory, at the
    // next XGKICK and when the program ends - which copies exactly what per-cycle progress would have: the source
    // memory cannot change in between.
    void advanceTo(uint64_t target)
    {
        if (target <= cycle)
            return;
        cycle = target;
        commitScalars();
    }

    bool pipelinesPending() const
    {
        return qPending || xg.active || p[0].valid || p[1].valid || flagCount != 0u;
    }

    void flush()
    {
        // Drain everything, ending on the cycle the interpreter's flushPipelines (one cycle at a time until idle)
        // ends on: at least one step, then the latest completion. Nothing can write VU memory any more, so the rest
        // of an XGKICK is copied at once; its last qword lands 2 cycles per qword after the credit already earned.
        if (!pipelinesPending())
            return;
        uint64_t end = cycle + 1u;
        if (xg.active)
        {
            progressXgkick();
            if (xg.active)
            {
                const uint64_t credit = xg.credit, last = xg.lastCycle;
                const uint64_t n = xgkickCopy(~0ull);
                end = std::max(end, last + 2u * n - credit);
            }
        }
        if (flagCount != 0u)
            end = std::max(end, flags[(flagHead + flagCount - 1u) & 63u].readyCycle);
        if (qPending)
            end = std::max(end, qReady);
        for (const PEntry &e : p)
            if (e.valid)
                end = std::max(end, e.ready);
        cycle = end;
        commitFlags(cycle);
        commitScalars();
    }

    VUF_PROFILE_NOINLINE uint64_t pairReadyCycle(const Pair &pr)
    {
        uint64_t ready = cycle;
        if ((pr.vfReadMask & vfBusy) != 0u)
        {
            for (uint32_t i = 0; i < pr.readCount; ++i)
            {
                const VfRead &r = pr.reads[i];
                const uint32_t bit = 1u << r.reg;
                if ((vfBusy & bit) == 0u)
                    continue;
                if (vfRegReady[r.reg] <= cycle)
                {
                    vfBusy &= ~bit; // every lane landed
                    continue;
                }
                const uint64_t *rdy = vfReady[r.reg];
                if (r.lanes & 8u)
                    ready = std::max(ready, rdy[0]);
                if (r.lanes & 4u)
                    ready = std::max(ready, rdy[1]);
                if (r.lanes & 2u)
                    ready = std::max(ready, rdy[2]);
                if (r.lanes & 1u)
                    ready = std::max(ready, rdy[3]);
            }
        }
        uint32_t viMask = pr.viReadMask & viBusy;
        while (viMask)
        {
#if defined(_MSC_VER)
            unsigned long bit;
            _BitScanForward(&bit, viMask);
#else
            const unsigned bit = static_cast<unsigned>(__builtin_ctz(viMask));
#endif
            viMask &= viMask - 1u;
            if (viReady[bit] <= cycle)
                viBusy &= ~(1u << bit);
            else
                ready = std::max(ready, viReady[bit]);
        }
        if (pr.accReadLanes && accMaxReady > cycle)
        {
            for (uint32_t c = 0; c < 4u; ++c)
                if (pr.accReadLanes & laneBit(c))
                    ready = std::max(ready, accReady[c]);
        }
        if (pr.scalarWait)
        {
            const Usage &lo = pr.lowerUsage;
            if (lo.pipe == Pipe::Fdiv && qPending)
                ready = std::max(ready, qReady);
            if (lo.pipe == Pipe::Efu)
                ready = std::max(ready, efuResourceReady);
            if (lo.waitQ && qPending)
                ready = std::max(ready, qReady);
            if (lo.waitP)
                for (const PEntry &e : p)
                    if (e.valid)
                        ready = std::max(ready, e.ready);
        }
        return ready;
    }

    // Out-of-line slow paths for compiled programs (keeps each compiled pair small enough for the i-cache).
#if defined(_MSC_VER)
#define VUF_NOINLINE __declspec(noinline)
#else
#define VUF_NOINLINE __attribute__((noinline))
#endif
    VUF_NOINLINE void aotCommitScalars() { commitScalars(); }
    VUF_NOINLINE void aotAdvanceTo(uint64_t target) { advanceTo(target); }
    VUF_NOINLINE void aotProgressXgkick() { progressXgkick(); }
    VUF_NOINLINE uint64_t aotLanesReady(uint64_t ready, uint32_t reg, uint32_t lanes) const
    {
        const uint64_t *rdy = vfReady[reg];
        if (lanes & 8u)
            ready = std::max(ready, rdy[0]);
        if (lanes & 4u)
            ready = std::max(ready, rdy[1]);
        if (lanes & 2u)
            ready = std::max(ready, rdy[2]);
        if (lanes & 1u)
            ready = std::max(ready, rdy[3]);
        return ready;
    }
    VUF_NOINLINE uint64_t aotAccReady(uint64_t ready, uint32_t lanes) const
    {
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & laneBit(c))
                ready = std::max(ready, accReady[c]);
        return ready;
    }
    VUF_NOINLINE uint64_t aotScalarReady(uint64_t ready, bool fdivOrWaitQ, bool efu, bool waitP) const
    {
        if (fdivOrWaitQ && qPending)
            ready = std::max(ready, qReady);
        if (efu)
            ready = std::max(ready, efuResourceReady);
        if (waitP)
            for (const PEntry &e : p)
                if (e.valid)
                    ready = std::max(ready, e.ready);
        return ready;
    }
    VUF_NOINLINE __m128 aotFmacFlags(__m128 r, uint8_t dest)
    {
        FmacFlags f;
        r = normalizeResultFast(r, f);
        pushFmacFlagsFast(f, dest);
        return r;
    }
    VUF_NOINLINE void aotWaitXgkick(uint64_t budgetEnd)
    {
        progressXgkick();
        while (xg.active && cycle < budgetEnd)
        {
            advanceTo(cycle + 1u);
            progressXgkick();
        }
    }

    VUF_FORCEINLINE void markVfWrite(uint8_t reg, uint8_t lanes, uint64_t r)
    {
        uint64_t *rdy = vfReady[reg];
        if (lanes & 8u)
            rdy[0] = r;
        if (lanes & 4u)
            rdy[1] = r;
        if (lanes & 2u)
            rdy[2] = r;
        if (lanes & 1u)
            rdy[3] = r;
        vfRegReady[reg] = std::max(vfRegReady[reg], r);
        vfBusy |= 1u << reg;
    }

    VUF_PROFILE_NOINLINE void markWrites(const Pair &pr)
    {
        if (pr.loVfReg != 0u)
            markVfWrite(pr.loVfReg, pr.loVfLanes, cycle + pr.loVfLatency);
        if (pr.upVfReg != 0u)
            markVfWrite(pr.upVfReg, pr.upVfLanes, cycle + pr.upVfLatency);
        uint32_t viMask = pr.viWriteMask;
        if (viMask)
        {
            viBusy |= viMask;
            const uint64_t r = cycle + pr.viWriteLatency;
            while (viMask)
            {
#if defined(_MSC_VER)
                unsigned long bit;
                _BitScanForward(&bit, viMask);
#else
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(viMask));
#endif
                viMask &= viMask - 1u;
                viReady[bit] = r;
            }
        }
        if (pr.accWriteLanes)
        {
            for (uint32_t c = 0; c < 4u; ++c)
                if (pr.accWriteLanes & laneBit(c))
                    accReady[c] = cycle + kAccForwardLatency;
            accMaxReady = std::max(accMaxReady, cycle + kAccForwardLatency);
        }
    }

    // Does any lower instruction in micro memory read the MAC or status flags?
    void scanFlagReaders(const uint8_t *code, uint32_t codeSize)
    {
        trackMacStatus = false;
        for (uint32_t pc = 0; pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lower, upper;
            std::memcpy(&lower, code + pc, 4);
            std::memcpy(&upper, code + pc + 4, 4);
            if (upper & 0x80000000u)
                continue; // I bit: the lower word is an immediate
            const uint8_t h = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
            if (h == 0x14u || h == 0x16u || h == 0x17u || h == 0x18u || h == 0x1Au || h == 0x1Bu)
            {
                trackMacStatus = true;
                return;
            }
        }
    }

    // ---- upper -------------------------------------------------------------------------------------------------
    void pushFmacFlags(uint32_t laneFlagsPacked, uint8_t dest)
    {
        if (dest == 0u)
            return;
        uint32_t mac = 0u, status = 0u;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            const uint8_t lane = laneBit(c);
            if ((dest & lane) == 0u)
                continue;
            const uint32_t f = (laneFlagsPacked >> (c * 4u)) & 0xFu;
            if (f & 0x1u)
                mac |= lane;
            if (f & 0x2u)
                mac |= static_cast<uint32_t>(lane) << 4;
            if (f & 0x4u)
                mac |= static_cast<uint32_t>(lane) << 8;
            if (f & 0x8u)
                mac |= static_cast<uint32_t>(lane) << 12;
            status |= f;
        }
        FlagEntry &e = pushFlag();
        e.mac = mac;
        e.status = status;
        e.writesMac = true;
        e.writesStatus = true;
    }

    void pushFmacFlagsFast(const FmacFlags &f, uint8_t dest)
    {
        if (dest == 0u)
            return;
        const uint32_t d = kReverse4[dest & 0xFu]; // dest in movemask lane order
        const uint32_t z = f.z & d, sg = f.s & d, un = f.u & d, ov = f.o & d;
        FlagEntry &e = pushFlag();
        e.mac = kReverse4[z] | (static_cast<uint32_t>(kReverse4[sg]) << 4) | (static_cast<uint32_t>(kReverse4[un]) << 8) |
                (static_cast<uint32_t>(kReverse4[ov]) << 12);
        e.status = (z ? 0x1u : 0u) | (sg ? 0x2u : 0u) | (un ? 0x4u : 0u) | (ov ? 0x8u : 0u);
        e.writesMac = true;
        e.writesStatus = true;
    }

    // ITOF / FTOI / ABS / CLIP, shared by execUpper and the compiled programs (constant operands fold there).
    VUF_FORCEINLINE void opItof(uint8_t fs, uint8_t ft, uint8_t convShift, __m128i mask)
    {
        VU1State &s = *st;
        if (ft == 0u)
            return;
        alignas(16) float res[4];
        for (uint32_t c = 0; c < 4u; ++c)
        {
            int32_t iv;
            std::memcpy(&iv, &s.vf[fs][c], 4);
            res[c] = convShift == 0 ? static_cast<float>(iv) : static_cast<float>(iv) / static_cast<float>(1u << convShift);
        }
        storeVfMasked(s, ft, _mm_load_ps(res), mask);
    }
    VUF_FORCEINLINE void opFtoi(uint8_t fs, uint8_t ft, uint8_t convShift, __m128i mask)
    {
        VU1State &s = *st;
        if (ft == 0u)
            return;
        alignas(16) float vs[4];
        alignas(16) int32_t res[4];
        _mm_store_ps(vs, normalize4(loadVf(s, fs)));
        const float scale = static_cast<float>(1u << convShift);
        for (uint32_t c = 0; c < 4u; ++c)
            res[c] = vuFloatToInt(vs[c], scale);
        storeVfMasked(s, ft, _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(res))), mask);
    }
    VUF_FORCEINLINE void opAbs(uint8_t fs, uint8_t ft, __m128i mask)
    {
        VU1State &s = *st;
        if (ft == 0u)
            return;
        const __m128 a = normalize4(loadVf(s, fs));
        const __m128 r = _mm_andnot_ps(_mm_set1_ps(-0.0f), a);
        storeVfMasked(s, ft, r, mask);
    }
    VUF_FORCEINLINE void opClip(uint8_t fs, uint8_t ft)
    {
        VU1State &s = *st;
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &s.vf[ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const auto exceeds = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t ordered = 0;
            std::memcpy(&ordered, &bits, sizeof(ordered));
            return ordered > limit;
        };
        uint32_t fl = 0u;
        if (exceeds(s.vf[fs][0], 0x00000000u))
            fl |= 0x01u;
        if (exceeds(s.vf[fs][0], 0x80000000u))
            fl |= 0x02u;
        if (exceeds(s.vf[fs][1], 0x00000000u))
            fl |= 0x04u;
        if (exceeds(s.vf[fs][1], 0x80000000u))
            fl |= 0x08u;
        if (exceeds(s.vf[fs][2], 0x00000000u))
            fl |= 0x10u;
        if (exceeds(s.vf[fs][2], 0x80000000u))
            fl |= 0x20u;
        workingClip = ((workingClip << 6) | fl) & 0xFFFFFFu;
        FlagEntry &e = pushFlag();
        e.clip = workingClip;
        e.writesClip = true;
    }

    VUF_PROFILE_NOINLINE void execUpper(const Upper &u)
    {
        VU1State &s = *st;
        switch (u.kind)
        {
        case UKind::Nop:
            return;
        case UKind::Arith:
        {
            const __m128 a = normalize4(loadVf(s, u.fs));
            __m128 b;
            switch (u.bsrc)
            {
            case BSrc::Vt:
                b = normalize4(loadVf(s, u.ft));
                break;
            case BSrc::Bc:
                b = splat(normalize4(loadVf(s, u.ft)), u.bc);
                break;
            case BSrc::Q:
                b = _mm_set1_ps(normalize1(s.q));
                break;
            default:
                b = _mm_set1_ps(normalize1(s.i));
                break;
            }
            __m128 r;
            switch (u.arith)
            {
            case Arith::Add:
                r = _mm_add_ps(a, b);
                break;
            case Arith::Sub:
                r = _mm_sub_ps(a, b);
                break;
            case Arith::Mul:
                r = _mm_mul_ps(a, b);
                break;
            case Arith::Madd:
                r = _mm_add_ps(normalize4(_mm_loadu_ps(s.acc)), _mm_mul_ps(a, b));
                break;
            case Arith::Msub:
                r = _mm_sub_ps(normalize4(_mm_loadu_ps(s.acc)), _mm_mul_ps(a, b));
                break;
            case Arith::Max:
                // (a > b) ? a : b  == _mm_max_ps(b, a) semantics for equal values (returns second operand)
                r = _mm_blendv_ps(b, a, _mm_cmpgt_ps(a, b));
                break;
            default: // Min
                r = _mm_blendv_ps(b, a, _mm_cmplt_ps(a, b));
                break;
            }
            if (u.flags)
            {
                if (trackMacStatus)
                {
                    FmacFlags f;
                    r = normalizeResultFast(r, f);
                    pushFmacFlagsFast(f, u.dest);
                }
                else
                    r = normalize4(r);
            }
            if (u.toAcc)
            {
                const __m128 old = _mm_loadu_ps(s.acc);
                _mm_storeu_ps(s.acc, _mm_blendv_ps(old, r, _mm_castsi128_ps(u.mask)));
            }
            else if (u.fd != 0u)
                storeVfMasked(s, u.fd, r, u.mask);
            return;
        }
        case UKind::OpMula:
        case UKind::OpMsub:
        {
            alignas(16) float vs[4], vt[4], acc[4], res[4];
            _mm_store_ps(vs, normalize4(loadVf(s, u.fs)));
            _mm_store_ps(vt, normalize4(loadVf(s, u.ft)));
            _mm_store_ps(acc, normalize4(_mm_loadu_ps(s.acc)));
            if (u.kind == UKind::OpMula)
            {
                res[0] = vs[1] * vt[2];
                res[1] = vs[2] * vt[0];
                res[2] = vs[0] * vt[1];
            }
            else
            {
                res[0] = acc[0] - vs[1] * vt[2];
                res[1] = acc[1] - vs[2] * vt[0];
                res[2] = acc[2] - vs[0] * vt[1];
            }
            res[3] = 0.0f;
            __m128 r;
            if (trackMacStatus)
            {
                FmacFlags f;
                r = normalizeResultFast(_mm_load_ps(res), f);
                pushFmacFlagsFast(f, u.dest);
            }
            else
                r = normalize4(_mm_load_ps(res));
            if (u.kind == UKind::OpMula)
            {
                const __m128 old = _mm_loadu_ps(s.acc);
                _mm_storeu_ps(s.acc, _mm_blendv_ps(old, r, _mm_castsi128_ps(u.mask)));
            }
            else if (u.fd != 0u)
                storeVfMasked(s, u.fd, r, u.mask);
            return;
        }
        case UKind::Itof:
            opItof(u.fs, u.ft, u.convShift, u.mask);
            return;
        case UKind::Ftoi:
            opFtoi(u.fs, u.ft, u.convShift, u.mask);
            return;
        case UKind::Abs:
            opAbs(u.fs, u.ft, u.mask);
            return;
        case UKind::Clip:
            opClip(u.fs, u.ft);
            return;
        default:
            std::cerr << "[vu1:fast] reserved upper instruction 0x" << std::hex << u.dest << std::dec << std::endl;
            stopRequested = true;
            return;
        }
    }

    // ---- lower -------------------------------------------------------------------------------------------------
    int32_t readBranchVi(uint8_t reg) const
    {
        if (reg == 0u)
            return 0;
        if (viBackupValid && viBackupReg == reg)
            return viBackupValue;
        return st->vi[reg];
    }

    // JR/JALR read the VI register's current value: only the conditional branches see the value from before an
    // integer write in the previous instruction (PCSX2 microVU applies its branch VI-delay only to IBxx). ZOE's
    // particle program computes its JALR target in the instruction right before the jump.
    int32_t readJumpVi(uint8_t reg) const { return reg == 0u ? 0 : st->vi[reg]; }

    void applyDest(float *dst, const float *src, uint8_t dest)
    {
        if (dest & 0x8u)
            dst[0] = src[0];
        if (dest & 0x4u)
            dst[1] = src[1];
        if (dest & 0x2u)
            dst[2] = src[2];
        if (dest & 0x1u)
            dst[3] = src[3];
    }

    void branchTo(uint32_t target)
    {
        st->branchPending = true;
        st->branchTarget = target;
        st->branchDelay = 1;
    }

    VUF_PROFILE_NOINLINE void execLower(uint32_t instr, const Pair &pr)
    {
        VU1State &s = *st;
        if (pr.isNop)
            return;
        const uint8_t opHi = pr.lowerOpHi;
        switch (opHi)
        {
        case 0x00: // LQ
        {
            const uint8_t it = FT(instr), is = VIS(instr), dest = DEST(instr);
            uint32_t addr = static_cast<uint32_t>(static_cast<int32_t>(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1u);
            if (addr + 16u <= dataSize && it != 0u)
            {
                float tmp[4];
                std::memcpy(tmp, data + addr, 16);
                applyDest(s.vf[it], tmp, dest);
            }
            return;
        }
        case 0x01: // SQ
        {
            if (xg.active)
                progressXgkick();
            const uint8_t is = FS(instr), it = VIT(instr), dest = DEST(instr);
            uint32_t addr = static_cast<uint32_t>(static_cast<int32_t>(s.vi[it] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[is], 16);
                store(addr, words, dest);
            }
            return;
        }
        case 0x04: // ILW
        {
            const uint8_t it = VIT(instr), is = VIS(instr), dest = DEST(instr);
            uint32_t addr = static_cast<uint32_t>(static_cast<int32_t>(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                const int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                uint32_t v;
                std::memcpy(&v, data + addr + comp * 4, 4);
                if (it != 0u)
                    s.vi[it] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
            }
            return;
        }
        case 0x05: // ISW
        {
            if (xg.active)
                progressXgkick();
            const uint8_t it = VIT(instr), is = VIS(instr), dest = DEST(instr);
            uint32_t addr = static_cast<uint32_t>(static_cast<int32_t>(s.vi[is] + IMM11(instr))) * 16u;
            addr &= (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[it] & 0xFFFF));
                const uint32_t words[4] = {val, val, val, val};
                store(addr, words, dest);
            }
            return;
        }
        case 0x08: // IADDIU
        case 0x09: // ISUBIU
        {
            const uint8_t it = VIT(instr), is = VIS(instr);
            const int16_t imm = static_cast<int16_t>((instr & 0x7FF) | ((instr >> 10) & 0x7800));
            if (it != 0u)
                s.vi[it] = static_cast<int16_t>(opHi == 0x08 ? s.vi[is] + imm : s.vi[is] - imm);
            return;
        }
        case 0x10: // FCEQ
            commitFlags(cycle);
            s.vi[1] = ((s.clip & 0xFFFFFFu) == (instr & 0xFFFFFFu)) ? 1 : 0;
            return;
        case 0x11: // FCSET
        {
            workingClip = instr & 0xFFFFFFu;
            for (uint32_t k = 0; k < flagCount; ++k)
            {
                FlagEntry &e = flags[(flagHead + k) & 63u];
                if (e.issueCycle == cycle)
                    e.writesClip = false;
            }
            FlagEntry &e = pushFlag();
            e.clip = workingClip;
            e.writesClip = true;
            return;
        }
        case 0x12: // FCAND
            commitFlags(cycle);
            s.vi[1] = ((s.clip & (instr & 0xFFFFFFu)) != 0u) ? 1 : 0;
            return;
        case 0x13: // FCOR
            commitFlags(cycle);
            s.vi[1] = ((s.clip | (instr & 0xFFFFFFu)) == 0xFFFFFFu) ? 1 : 0;
            return;
        case 0x14: // FSEQ
        case 0x16: // FSAND
        case 0x17: // FSOR
        {
            commitFlags(cycle);
            const uint8_t it = VIT(instr);
            const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
            if (it != 0u)
            {
                const uint32_t st12 = s.status & 0xFFFu;
                s.vi[it] = opHi == 0x14 ? (st12 == imm12 ? 1 : 0)
                                        : static_cast<int32_t>(opHi == 0x16 ? (st12 & imm12) : (st12 | imm12));
            }
            return;
        }
        case 0x15: // FSSET
        {
            const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
            for (uint32_t k = 0; k < flagCount; ++k)
            {
                FlagEntry &e = flags[(flagHead + k) & 63u];
                if (e.issueCycle == cycle)
                    e.writesStatus = false;
            }
            FlagEntry &e = pushFlag();
            e.status = static_cast<uint32_t>(imm12) & 0xFC0u;
            e.writesSticky = true;
            return;
        }
        case 0x18: // FMEQ
        case 0x1A: // FMAND
        case 0x1B: // FMOR
        {
            commitFlags(cycle);
            const uint8_t it = VIT(instr), is = VIS(instr);
            if (it != 0u)
            {
                const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[is]));
                s.vi[it] = opHi == 0x18 ? ((s.mac & 0xFFFFu) == v ? 1 : 0)
                                        : static_cast<int32_t>(opHi == 0x1A ? (s.mac & v) : (s.mac | v));
            }
            return;
        }
        case 0x1C: // FCGET
        {
            commitFlags(cycle);
            const uint8_t it = VIT(instr);
            if (it != 0u)
                s.vi[it] = static_cast<int32_t>(s.clip & 0x0FFFu);
            return;
        }
        case 0x20: // B
            branchTo((s.pc + 8u + static_cast<int32_t>(IMM11(instr)) * 8) & pcMask);
            return;
        case 0x21: // BAL
        {
            const uint8_t it = VIT(instr);
            const uint32_t target = (s.pc + 8u + static_cast<int32_t>(IMM11(instr)) * 8) & pcMask;
            if (it != 0u)
                s.vi[it] = static_cast<int32_t>((s.pc + 16u) / 8u);
            branchTo(target);
            return;
        }
        case 0x24: // JR
            branchTo((static_cast<uint32_t>(static_cast<uint16_t>(readJumpVi(VIS(instr)))) * 8u) & pcMask);
            return;
        case 0x25: // JALR
        {
            const uint8_t it = VIT(instr);
            const uint32_t target = (static_cast<uint32_t>(static_cast<uint16_t>(readJumpVi(VIS(instr)))) * 8u) & pcMask;
            if (it != 0u)
                s.vi[it] = static_cast<int32_t>((s.pc + 16u) / 8u);
            branchTo(target);
            return;
        }
        case 0x28: // IBEQ
        case 0x29: // IBNE
        {
            const bool eq = static_cast<int16_t>(readBranchVi(VIS(instr))) == static_cast<int16_t>(readBranchVi(VIT(instr)));
            if ((opHi == 0x28) == eq)
                branchTo((s.pc + 8u + static_cast<int32_t>(IMM11(instr)) * 8) & pcMask);
            return;
        }
        case 0x2C: // IBLTZ
        case 0x2D: // IBGTZ
        case 0x2E: // IBLEZ
        case 0x2F: // IBGEZ
        {
            const int16_t v = static_cast<int16_t>(readBranchVi(VIS(instr)));
            const bool take = opHi == 0x2C ? v < 0 : opHi == 0x2D ? v > 0 : opHi == 0x2E ? v <= 0 : v >= 0;
            if (take)
                branchTo((s.pc + 8u + static_cast<int32_t>(IMM11(instr)) * 8) & pcMask);
            return;
        }
        case 0x40:
            break;
        default:
            std::cerr << "[vu1:fast] reserved lower instruction 0x" << std::hex << instr << std::dec << std::endl;
            stopRequested = true;
            return;
        }

        const uint8_t funct = instr & 0x3Fu;
        const uint8_t vfT = FT(instr), vfS = FS(instr);
        const uint8_t viT = VIT(instr), viS = VIS(instr), viD = VID(instr);
        const uint8_t dest = DEST(instr);
        switch (funct)
        {
        case 0x30:
            if (viD != 0u)
                s.vi[viD] = static_cast<int16_t>(s.vi[viS] + s.vi[viT]);
            return;
        case 0x31:
            if (viD != 0u)
                s.vi[viD] = static_cast<int16_t>(s.vi[viS] - s.vi[viT]);
            return;
        case 0x32:
        {
            const int16_t imm5 = static_cast<int16_t>(static_cast<int32_t>((instr >> 6) & 0x1F) << 27 >> 27);
            if (viT != 0u)
                s.vi[viT] = static_cast<int16_t>(s.vi[viS] + imm5);
            return;
        }
        case 0x34:
            if (viD != 0u)
                s.vi[viD] = s.vi[viS] & s.vi[viT];
            return;
        case 0x35:
            if (viD != 0u)
                s.vi[viD] = s.vi[viS] | s.vi[viT];
            return;
        default:
            break;
        }
        if (funct < 0x3Cu)
        {
            std::cerr << "[vu1:fast] reserved lower instruction 0x" << std::hex << instr << std::dec << std::endl;
            stopRequested = true;
            return;
        }

        switch (pr.lowerSpecial)
        {
        case 0x30: // MOVE
            if (vfT != 0u)
            {
                float tmp[4];
                std::memcpy(tmp, s.vf[vfS], 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            return;
        case 0x31: // MR32
            if (vfT != 0u)
            {
                const float tmp[4] = {s.vf[vfS][1], s.vf[vfS][2], s.vf[vfS][3], s.vf[vfS][0]};
                applyDest(s.vf[vfT], tmp, dest);
            }
            return;
        case 0x34: // LQI
        {
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viS])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize && vfT != 0u)
            {
                float tmp[4];
                std::memcpy(tmp, data + addr, 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            if (viS != 0u)
                s.vi[viS] = static_cast<int16_t>(s.vi[viS] + 1);
            return;
        }
        case 0x35: // SQI
        {
            if (xg.active)
                progressXgkick();
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viT])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[vfS], 16);
                store(addr, words, dest);
            }
            if (viT != 0u)
                s.vi[viT] = static_cast<int16_t>(s.vi[viT] + 1);
            return;
        }
        case 0x36: // LQD
        {
            if (viS != 0u)
                s.vi[viS] = static_cast<int16_t>(s.vi[viS] - 1);
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viS])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize && vfT != 0u)
            {
                float tmp[4];
                std::memcpy(tmp, data + addr, 16);
                applyDest(s.vf[vfT], tmp, dest);
            }
            return;
        }
        case 0x37: // SQD
        {
            if (xg.active)
                progressXgkick();
            if (viT != 0u)
                s.vi[viT] = static_cast<int16_t>(s.vi[viT] - 1);
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viT])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                uint32_t words[4];
                std::memcpy(words, s.vf[vfS], 16);
                store(addr, words, dest);
            }
            return;
        }
        case 0x38: // DIV
        {
            const float num = normalize1(s.vf[vfS][(instr >> 21) & 0x3]);
            const float den = normalize1(s.vf[vfT][(instr >> 23) & 0x3]);
            uint32_t statusDi = 0u;
            float result;
            if (den == 0.0f)
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                                 : std::numeric_limits<float>::max();
            }
            else
                result = num / den;
            queueQ(result, 7u, statusDi);
            return;
        }
        case 0x39: // SQRT
        {
            const float val = normalize1(s.vf[vfT][(instr >> 23) & 0x3]);
            queueQ(std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);
            return;
        }
        case 0x3A: // RSQRT
        {
            const float num = normalize1(s.vf[vfS][(instr >> 21) & 0x3]);
            const float radicand = normalize1(s.vf[vfT][(instr >> 23) & 0x3]);
            const float den = std::sqrt(std::fabs(radicand));
            uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
            float result;
            if (den != 0.0f)
                result = num / den;
            else
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            }
            queueQ(result, 13u, statusDi);
            return;
        }
        case 0x3B: // WAITQ
            return;
        case 0x3C: // MTIR
        {
            uint32_t fval;
            std::memcpy(&fval, &s.vf[vfS][(instr >> 21) & 0x3u], 4);
            if (viT != 0u)
                s.vi[viT] = static_cast<int32_t>(static_cast<int16_t>(fval & 0xFFFFu));
            return;
        }
        case 0x3D: // MFIR
            if (vfT != 0u)
            {
                const int32_t val = static_cast<int32_t>(static_cast<int16_t>(s.vi[viS] & 0xFFFF));
                float r[4];
                std::memcpy(&r[0], &val, 4);
                r[1] = r[2] = r[3] = r[0];
                applyDest(s.vf[vfT], r, dest);
            }
            return;
        case 0x3E: // ILWR
        {
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viS])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                const int comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                uint32_t v;
                std::memcpy(&v, data + addr + comp * 4, 4);
                if (viT != 0u)
                    s.vi[viT] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
            }
            return;
        }
        case 0x3F: // ISWR
        {
            if (xg.active)
                progressXgkick();
            uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viS])) * 16u) & (dataSize - 1u);
            if (addr + 16u <= dataSize)
            {
                const uint32_t val = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viT] & 0xFFFF));
                const uint32_t words[4] = {val, val, val, val};
                store(addr, words, dest);
            }
            return;
        }
        case 0x40: // RNEXT
        {
            const uint32_t x = (s.r >> 4) & 1u;
            const uint32_t y = (s.r >> 22) & 1u;
            s.r = ((s.r << 1) ^ x ^ y) & 0x007FFFFFu;
            s.r |= 0x3F800000u;
            if (vfT != 0u)
            {
                float v;
                std::memcpy(&v, &s.r, 4);
                const float r[4] = {v, v, v, v};
                applyDest(s.vf[vfT], r, dest);
            }
            return;
        }
        case 0x41: // RGET
            if (vfT != 0u)
            {
                float v;
                std::memcpy(&v, &s.r, 4);
                const float r[4] = {v, v, v, v};
                applyDest(s.vf[vfT], r, dest);
            }
            return;
        case 0x42: // RINIT
        case 0x43: // RXOR
        {
            uint32_t bits;
            std::memcpy(&bits, &s.vf[vfS][(instr >> 21) & 3u], 4);
            s.r = 0x3F800000u | ((pr.lowerSpecial == 0x42 ? bits : (s.r ^ bits)) & 0x007FFFFFu);
            return;
        }
        case 0x64: // MFP
            if (vfT != 0u)
            {
                const float r[4] = {s.p, s.p, s.p, s.p};
                applyDest(s.vf[vfT], r, dest);
            }
            return;
        case 0x68: // XTOP
            if (viT != 0u)
                s.vi[viT] = static_cast<int32_t>(s.top & 0x3FFu);
            return;
        case 0x69: // XITOP
            if (viT != 0u)
                s.vi[viT] = static_cast<int32_t>(s.itop & 0x3FFu);
            return;
        case 0x6C: // XGKICK
            startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(s.vi[viS])));
            return;
        case 0x70: // ESADD
        case 0x71: // ERSADD
        case 0x72: // ELENG
        case 0x73: // ERLENG
        {
            const float x = normalize1(s.vf[vfS][0]), y = normalize1(s.vf[vfS][1]), z = normalize1(s.vf[vfS][2]);
            const float sum = x * x + y * y + z * z;
            switch (pr.lowerSpecial)
            {
            case 0x70:
                queueP(sum, 11u);
                break;
            case 0x71:
                queueP(sum != 0.0f ? 1.0f / sum : sum, 18u);
                break;
            case 0x72:
                queueP(std::sqrt(sum), 18u);
                break;
            default:
            {
                const float len = std::sqrt(sum);
                queueP(len != 0.0f ? 1.0f / len : len, 24u);
                break;
            }
            }
            return;
        }
        case 0x74: // EATANxy
        {
            const float x = normalize1(s.vf[vfS][0]), y = normalize1(s.vf[vfS][1]);
            queueP(x != 0.0f ? vuEatan(y / x) : 0.0f, 54u);
            return;
        }
        case 0x75: // EATANxz
        {
            const float x = normalize1(s.vf[vfS][0]), z = normalize1(s.vf[vfS][2]);
            queueP(x != 0.0f ? vuEatan(z / x) : 0.0f, 54u);
            return;
        }
        case 0x76: // ESUM
        {
            float sum = 0.0f;
            for (uint32_t c = 0; c < 4u; ++c)
                sum += normalize1(s.vf[vfS][c]);
            queueP(sum, 12u);
            return;
        }
        case 0x77: // ERSQRT
        {
            float r = normalize1(s.vf[vfS][(instr >> 21) & 3u]);
            if (r >= 0.0f)
            {
                r = std::sqrt(r);
                if (r != 0.0f)
                    r = 1.0f / r;
            }
            queueP(r, 18u);
            return;
        }
        case 0x78: // ESQRT
        {
            const float v = normalize1(s.vf[vfS][(instr >> 21) & 3u]);
            queueP(v >= 0.0f ? std::sqrt(v) : v, 12u);
            return;
        }
        case 0x79: // ESIN
            queueP(vuEsin(normalize1(s.vf[vfS][(instr >> 21) & 3u])), 29u);
            return;
        case 0x7A: // ERCPR
        {
            const float v = normalize1(s.vf[vfS][(instr >> 21) & 3u]);
            queueP(v != 0.0f ? 1.0f / v : v, 12u);
            return;
        }
        case 0x7B: // WAITP
            return;
        case 0x7C: // EATAN
            queueP(vuEatan(normalize1(s.vf[vfS][(instr >> 21) & 3u])), 54u);
            return;
        case 0x7D: // EEXP
            queueP(vuEexp(normalize1(s.vf[vfS][(instr >> 21) & 3u])), 44u);
            return;
        default:
            std::cerr << "[vu1:fast] reserved lower instruction 0x" << std::hex << instr << std::dec << std::endl;
            stopRequested = true;
            return;
        }
    }

    // ---- main loop ---------------------------------------------------------------------------------------------
    void run(const uint8_t *code, uint32_t codeSize, uint32_t maxCycles)
    {
        // Round toward zero like the VU. MXCSR directly: fesetround also reprograms the x87 unit and was a visible cost
        // for ZOE's many short VCALLMS programs.
        const unsigned previousCsr = _mm_getcsr();
        _mm_setcsr((previousCsr & ~0x6000u) | 0x6000u | 0x1F80u); // round toward zero, exceptions masked
        const uint64_t budgetEnd = cycle + maxCycles;
        bool programEnded = false;
        VU1State &s = *st;
        const uint32_t entryPc = s.pc;
        const bool resumed = s.branchPending || s.ebit || s.haltAfterDelaySlot;
        if (aotRun != nullptr && !stopRequested && !s.branchPending && !s.ebit && !s.haltAfterDelaySlot &&
            aotRun(*this, s.pc, budgetEnd))
        {
            // The compiled program ran to its end (flushed, E-bit state cleared) exactly as the loop below would.
            if (!vu0)
                ps2_frame_log::counters().vu1Aot.fetch_add(1u, std::memory_order_relaxed);
            s.cycles = cycle;
            _mm_setcsr(previousCsr);
            return;
        }
        // Not compiled here, or it handed over mid-program (st->pc / branch / E-bit state set up): interpret.
        if (!vu0)
        {
            ps2_frame_log::counters().vu1Fallback.fetch_add(1u, std::memory_order_relaxed);
            // PS2X_VU_FALLBACK_LOG=1 (diagnostic): which VU1 entries fall back to the interpreter, and why; the cure is
            // usually a capture of that scene plus regenerated AOT code, or a generator fix (see ps2_vu_aotgen findLeaders).
            static const bool s_fallbackLog = std::getenv("PS2X_VU_FALLBACK_LOG") != nullptr;
            if (s_fallbackLog)
            {
                // reason: 0 no compiled code, 1 resumed mid-program, 2 not compiled at this pc, 3 handed over mid-program
                const uint32_t reason = aotRun == nullptr ? 0u : resumed ? 1u : (s.pc == entryPc && !s.branchPending) ? 2u : 3u;
                static std::unordered_map<uint64_t, uint64_t> s_counts;
                static uint64_t s_total = 0;
                ++s_counts[(static_cast<uint64_t>(entryPc) << 8) | reason | (reason == 3u ? static_cast<uint64_t>(s.pc) << 32 : 0u)];
                if (++s_total % 50000u == 0u)
                {
                    std::vector<std::pair<uint64_t, uint64_t>> top(s_counts.begin(), s_counts.end());
                    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    std::fprintf(stderr, "[vu1:fallback] total=%llu", static_cast<unsigned long long>(s_total));
                    for (size_t i = 0; i < top.size() && i < 12u; ++i)
                        std::fprintf(stderr, " | entry=0x%x why=%u at=0x%x n=%llu", static_cast<unsigned>((top[i].first >> 8) & 0xFFFFFFu),
                                     static_cast<unsigned>(top[i].first & 0xFFu), static_cast<unsigned>(top[i].first >> 32),
                                     static_cast<unsigned long long>(top[i].second));
                    std::fprintf(stderr, "\n");
                    s_counts.clear();
                }
            }
        }
        while (cycle < budgetEnd && !stopRequested)
        {
            commitScalars();
            if (s.pc + 8u > codeSize)
                break;
            const Pair &pr = pairAt(code, s.pc);
            if (pr.upperUsage.reserved || pr.lowerUsage.reserved)
            {
                std::cerr << "[vu1:fast] reserved instruction at pc=0x" << std::hex << s.pc << " upper=0x" << pr.upper
                          << " lower=0x" << pr.lower << std::dec << std::endl;
                stopRequested = true;
                break;
            }

            uint64_t ready = pairReadyCycle(pr);
            if (pr.lowerUsage.pipe == Pipe::Xgkick && xg.active)
            {
                // A second XGKICK waits for the previous transfer to finish.
                progressXgkick();
                while (xg.active && cycle < budgetEnd)
                {
                    advanceTo(cycle + 1u);
                    progressXgkick();
                }
                ready = std::max(ready, pairReadyCycle(pr));
            }
            while (ready > cycle)
            {
                if (ready >= budgetEnd)
                {
                    advanceTo(budgetEnd);
                    break;
                }
                advanceTo(ready);
                ready = pairReadyCycle(pr);
            }
            if (cycle >= budgetEnd)
                break;
            if (ps2_vu_analysis::g_pair)
                ps2_vu_analysis::g_pair(pr.lower, pr.upper, s);

            uint8_t writtenVi = 0u;
            int32_t oldVi = 0;
            if (pr.lowerUsage.viWrite != 0u)
            {
#if defined(_MSC_VER)
                unsigned long bit;
                _BitScanForward(&bit, pr.lowerUsage.viWrite);
#else
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(pr.lowerUsage.viWrite));
#endif
                writtenVi = static_cast<uint8_t>(bit);
                oldVi = s.vi[bit];
            }

            if (pr.iBit)
            {
                execUpper(pr.up);
                float imm;
                std::memcpy(&imm, &pr.lower, 4);
                s.i = normalize1(imm);
            }
            else if (pr.upperShadowReg != 0u)
            {
                float oldVf[4], upperVf[4];
                std::memcpy(oldVf, s.vf[pr.upperShadowReg], 16);
                execUpper(pr.up);
                std::memcpy(upperVf, s.vf[pr.upperShadowReg], 16);
                std::memcpy(s.vf[pr.upperShadowReg], oldVf, 16);
                execLower(pr.lower, pr);
                std::memcpy(s.vf[pr.upperShadowReg], upperVf, 16);
            }
            else
            {
                execUpper(pr.up);
                execLower(pr.lower, pr);
            }

            viBackupValid = false;
            markWrites(pr);
            if (writtenVi != 0u && pr.lowerUsage.delaysNextBranchRead)
            {
                viBackupValue = oldVi;
                viBackupReg = writtenVi;
                viBackupValid = true;
            }

            s.vf[0][0] = 0.0f;
            s.vf[0][1] = 0.0f;
            s.vf[0][2] = 0.0f;
            s.vf[0][3] = 1.0f;
            s.vi[0] = 0;

            uint32_t nextPc = s.pc + 8u;
            if (nextPc >= codeSize)
                nextPc = 0u;
            s.pc = nextPc;
            if (s.branchPending)
            {
                if (s.branchDelay == 0u)
                {
                    s.pc = s.branchTarget & pcMask;
                    s.branchPending = false;
                }
                else
                    --s.branchDelay;
            }

            const bool dHalt = pr.dBit && s.dBitEnabled;
            const bool tHalt = pr.tBit && s.tBitEnabled;
            const bool haltBit = dHalt || tHalt;
            const bool haltBranch = haltBit && pr.lowerUsage.pipe == Pipe::Branch;
            if (s.haltAfterDelaySlot)
            {
                s.stoppedByD = pendingHaltD;
                s.stoppedByT = pendingHaltT;
                programEnded = true;
            }
            else if (s.ebit)
                programEnded = true;
            else if (haltBit && !haltBranch)
            {
                s.stoppedByD = dHalt;
                s.stoppedByT = tHalt;
                programEnded = true;
            }
            else if (pr.eBit)
                s.ebit = true;
            else if (haltBranch)
            {
                s.haltAfterDelaySlot = true;
                pendingHaltD = dHalt;
                pendingHaltT = tHalt;
            }

            ++cycle;
            if (programEnded)
                break;
        }

        if (programEnded)
        {
            flush();
            s.ebit = false;
            s.haltAfterDelaySlot = false;
            pendingHaltD = false;
            pendingHaltT = false;
        }
        else if (xg.active)
            progressXgkick();
        s.cycles = cycle;
        _mm_setcsr(previousCsr);
    }

    void begin(VU1State &state, uint8_t *d, uint32_t dSize, PS2Memory *mem, GS *g, const uint8_t *code); // .cpp
};
