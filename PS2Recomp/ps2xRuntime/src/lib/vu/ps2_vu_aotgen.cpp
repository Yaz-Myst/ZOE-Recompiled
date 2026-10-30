// Ahead-of-time compiler for VU micro programs: turns captured micro memory images into C++ that runs on the fast
// engine's own state (runtime/ps2_vu_fast_impl.h), so a compiled program behaves exactly like the interpreter - same
// stalls, flag/Q/P timing, XGKICK pacing, delay slots - without the per-pair decode and dispatch. Driven by the replay
// benchmark (PS2X_VU_BENCH=<capture>,aotgen=<out.cpp>); the output is meant to be compiled into the game (e.g.
// ZOE-Recompiled/src), where it registers itself with VuFastEngine::registerAot.
//
// Per compiled pair the generated code does what Impl::run does per loop iteration, with everything that depends only
// on the instruction words resolved at generation time. Whatever it does not handle (reserved instructions, D/T bits,
// a branch in a branch delay slot, an unknown JR target, the cycle budget) hands back to the interpreter with st->pc
// and the branch / E-bit state set up, on the same pipeline state.
#include "runtime/ps2_vu_fast_impl.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ps2_vu_aotgen
{
    struct Source
    {
        bool vu0 = false;
        std::vector<uint8_t> code;
        std::vector<uint32_t> entries;
        std::string name;
    };
    bool generate(const std::vector<Source> &sources, const std::string &path);
}

namespace
{
    using Impl = VuFastEngine::Impl;

    std::string hx(uint32_t v, int width = 0)
    {
        char buf[32];
        if (width)
            std::snprintf(buf, sizeof(buf), "0x%0*Xu", width, v);
        else
            std::snprintf(buf, sizeof(buf), "0x%Xu", v);
        return buf;
    }

    std::string label(uint32_t pc)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "L_%04X", pc);
        return buf;
    }

    const char *laneSuffix(uint32_t c) { return c == 0 ? "0" : c == 1 ? "1" : c == 2 ? "2" : "3"; }

    bool isBranchPair(const Pair &p) { return !p.iBit && !p.isNop && p.lowerUsage.pipe == Pipe::Branch; }

    struct Gen
    {
        const ps2_vu_aotgen::Source &src;
        uint32_t codeSize, pcMask;
        std::vector<Pair> pairs;
        std::vector<uint8_t> reach, bail, jumpTarget, entry;
        std::set<uint32_t> dynTargets; // return addresses (JR candidates)
        std::vector<uint32_t> fallbackIndex; // pair index -> K[] slot (+1), 0 = none
        std::vector<uint32_t> fallbackPairs;
        std::ostringstream o;
        std::string fn;

        explicit Gen(const ps2_vu_aotgen::Source &s) : src(s)
        {
            codeSize = s.vu0 ? 0x1000u : 0x4000u;
            pcMask = codeSize - 1u;
            const uint32_t n = codeSize / 8u;
            pairs.resize(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t lower = 0, upper = 0;
                if (i * 8u + 8u <= s.code.size())
                {
                    std::memcpy(&lower, s.code.data() + i * 8u, 4);
                    std::memcpy(&upper, s.code.data() + i * 8u + 4u, 4);
                }
                pairs[i] = Impl::decodePairWords(lower, upper, s.vu0);
            }
            reach.assign(n, 0);
            bail.assign(n, 0);
            jumpTarget.assign(n, 0);
            entry.assign(n, 0);
            fallbackIndex.assign(n, 0);
        }

        uint32_t next(uint32_t pc) const { return pc + 8u >= codeSize ? 0u : pc + 8u; }
        uint32_t prev(uint32_t pc) const { return (pc + codeSize - 8u) & pcMask; }
        const Pair &at(uint32_t pc) const { return pairs[(pc & pcMask) >> 3]; }
        uint32_t staticTarget(uint32_t pc) const
        {
            return (pc + 8u + static_cast<uint32_t>(static_cast<int32_t>(IMM11(at(pc).lower)) * 8)) & pcMask;
        }

        // ---- MAC/status flag liveness ------------------------------------------------------------------------
        // A flag read (FSxxx/FMxxx) sees the newest entry that is ready, i.e. issued at least 4 cycles earlier. A writer
        // at least 4 pairs before the read (each pair takes >= 1 cycle) is therefore ready and hides every older
        // writer on that path; writers closer than that may or may not be ready, depending on stalls. So an FMAC
        // result's flags are needed only when some path reaches a read with no other writer at least 4 pairs before
        // that read. Everything else skips the flag computation (the FIFO entry is never observed; only the final
        // MAC/status registers differ, which nothing reads). Sticky status reads or unknown control flow keep all.
        std::vector<uint8_t> flagsNeeded;
        bool stickyRead = false;

        static bool readsMacStatus(const Pair &p)
        {
            if (p.iBit || p.isNop)
                return false;
            const uint8_t h = p.lowerOpHi;
            return h == 0x14u || h == 0x16u || h == 0x17u || h == 0x18u || h == 0x1Au || h == 0x1Bu;
        }
        static bool writesMacStatus(const Pair &p) { return p.up.flags && p.up.dest != 0u; }

        void computeFlagLiveness()
        {
            const uint32_t n = static_cast<uint32_t>(pairs.size());
            flagsNeeded.assign(n, 1);
            for (uint32_t i = 0; i < n; ++i)
            {
                const Pair &p = pairs[i];
                if (!reach[i] || !readsMacStatus(p))
                    continue;
                const uint8_t h = p.lowerOpHi;
                if (h == 0x14u || h == 0x16u || h == 0x17u)
                {
                    const uint32_t imm12 = (((p.lower >> 21) & 0x1u) << 11) | (p.lower & 0x7FFu);
                    if (h == 0x14u || (imm12 & 0xFC0u) != 0u)
                        stickyRead = true; // FSEQ compares all 12 bits; sticky bits accumulate over every writer
                }
            }
            if (stickyRead)
                return;
            constexpr uint32_t kUnknown = 0xFFFFFFFFu;
            std::vector<std::vector<uint32_t>> succ(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!reach[i] || bail[i])
                    continue;
                const uint32_t pc = i * 8u;
                const uint32_t pv = prev(pc);
                const Pair &pp = at(pv);
                const bool prevBranch = reach[pv >> 3] && !bail[pv >> 3] && isBranchPair(pp);
                if (prevBranch)
                {
                    const uint8_t bop = pp.lowerOpHi;
                    succ[i].push_back((bop == 0x24u || bop == 0x25u) ? kUnknown : (staticTarget(pv) >> 3));
                }
                succ[i].push_back(next(pc) >> 3); // not taken / not after an E-bit / plain fall-through
            }
            std::vector<uint8_t> in(n, 0);
            for (uint32_t i = 0; i < n; ++i)
                if (reach[i] && bail[i])
                    in[i] = 0x1Fu; // the interpreter may read anything
            auto outOf = [&](uint32_t k)
            {
                uint8_t out = 0;
                for (uint32_t t : succ[k])
                    out |= (t == kUnknown || !reach[t]) ? 0x1Fu : in[t];
                return out;
            };
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (uint32_t k = n; k-- > 0;)
                {
                    if (!reach[k] || bail[k])
                        continue;
                    const uint8_t out = outOf(k);
                    // one pair further away: distances 0..3 move up by one, ">= 4" (bit 4) stays
                    uint8_t shifted = static_cast<uint8_t>(((out << 1) & 0x1Eu) | (out & 0x10u));
                    if (writesMacStatus(pairs[k]))
                        shifted &= 0x0Fu; // reads >= 4 pairs away see this writer (or a newer one), nothing older
                    const uint8_t v = static_cast<uint8_t>(shifted | (readsMacStatus(pairs[k]) ? 0x01u : 0x00u));
                    if (v != in[k])
                    {
                        in[k] = v;
                        changed = true;
                    }
                }
            }
            uint32_t kept = 0, skipped = 0;
            for (uint32_t k = 0; k < n; ++k)
            {
                if (!reach[k] || bail[k] || !writesMacStatus(pairs[k]))
                    continue;
                flagsNeeded[k] = outOf(k) != 0u;
                (flagsNeeded[k] ? kept : skipped)++;
            }
            std::cerr << "[vu:aotgen] " << src.name << ": MAC/status flags kept for " << kept << " FMAC ops, skipped for "
                      << skipped << std::endl;
        }

        void explore()
        {
            std::vector<uint32_t> work(src.entries.begin(), src.entries.end());
            for (uint32_t e : src.entries)
                entry[(e & pcMask) >> 3] = 1;
            while (!work.empty())
            {
                const uint32_t pc = work.back() & pcMask & ~7u;
                work.pop_back();
                const uint32_t i = pc >> 3;
                if (reach[i])
                    continue;
                reach[i] = 1;
                const Pair &p = pairs[i];
                // All-zero pairs are never part of a real program (NOP is upper 0x2FF): running into them means
                // falling off the end of the code, which is left to the interpreter instead of compiling all of memory.
                if (p.upperUsage.reserved || p.lowerUsage.reserved || p.dBit || p.tBit || (p.lower == 0u && p.upper == 0u))
                {
                    bail[i] = 1;
                    continue;
                }
                const uint32_t nx = next(pc);
                if (isBranchPair(p))
                {
                    const Pair &slot = at(nx);
                    if (isBranchPair(slot))
                    {
                        bail[i] = 1; // branch in a delay slot: leave to the interpreter
                        continue;
                    }
                    work.push_back(nx);
                    const uint8_t op = p.lowerOpHi;
                    if (op == 0x20u || op == 0x21u || (op >= 0x28u && op <= 0x2Fu))
                    {
                        const uint32_t t = staticTarget(pc);
                        if (!p.eBit)
                        {
                            work.push_back(t);
                            jumpTarget[t >> 3] = 1;
                        }
                    }
                    if (op >= 0x28u && !p.eBit)
                    {
                        work.push_back(next(nx)); // not taken: after the delay slot
                        jumpTarget[next(nx) >> 3] = 1;
                    }
                    if (op == 0x21u || op == 0x25u)
                    {
                        const uint32_t ret = (pc + 16u) & pcMask;
                        dynTargets.insert(ret);
                        work.push_back(ret);
                        jumpTarget[ret >> 3] = 1;
                    }
                    continue;
                }
                if (p.eBit)
                {
                    work.push_back(nx); // delay slot, then the program ends
                    continue;
                }
                work.push_back(nx);
            }
        }

        void emitBail(const std::string &pcExpr, const char *indent)
        {
            o << indent << "{ a.bailPc = " << pcExpr << "; return kBail; }\n";
        }

        // Leave the block for `target`: the dispatcher runs its block, or hands over to the interpreter when there is none.
        void emitJump(uint32_t target, const char *indent)
        {
            o << indent << "return " << hx(target & pcMask) << ";\n";
        }

        // ---- per-pair pieces --------------------------------------------------------------------------------
        void emitReads(const Pair &p)
        {
            // Same maximum as Impl::pairReadyCycle; a register whose newest write is no later than `rdy` cannot raise it.
            for (uint32_t k = 0; k < p.readCount; ++k)
            {
                const VfRead &r = p.reads[k];
                if (r.reg == 0u)
                    continue; // vf0 is never written
                o << "        if (m.vfRegReady[" << int(r.reg) << "] > rdy)\n            rdy = m.aotLanesReady(rdy, " << int(r.reg)
                  << ", " << int(r.lanes) << ");\n";
            }
            for (uint32_t b = 1; b < 16u; ++b)
                if (p.viReadMask & (1u << b))
                    o << "        if (m.viReady[" << b << "] > rdy)\n            rdy = m.viReady[" << b << "];\n";
            if (p.accReadLanes)
                o << "        if (m.accMaxReady > rdy)\n            rdy = m.aotAccReady(rdy, " << int(p.accReadLanes) << ");\n";
            if (p.scalarWait)
            {
                const Usage &lo = p.lowerUsage;
                o << "        rdy = m.aotScalarReady(rdy, " << ((lo.pipe == Pipe::Fdiv || lo.waitQ) ? "true" : "false") << ", "
                  << (lo.pipe == Pipe::Efu ? "true" : "false") << ", " << (lo.waitP ? "true" : "false") << ");\n";
            }
        }

        std::string vfLoad(uint32_t reg) { return "_mm_loadu_ps(s.vf[" + std::to_string(reg) + "])"; }

        // ---- operands known to be normalized ------------------------------------------------------------------
        // normalize4 maps a denormal to a signed zero and exponent 255 to +-max; on a value that is already normal (or
        // zero) it changes nothing. Within a block, every lane of a register last written by an FMAC/ITOF (results are
        // normalized) or copied from such a register is normal, so its operand normalize can be left out with the same
        // result. known[32] is ACC. Reset at each block start (values arriving from elsewhere are unknown).
        bool known[33] = {};
        void resetKnown()
        {
            for (bool &k : known)
                k = false;
            known[0] = true; // vf0 is (0, 0, 0, 1)
        }
        std::string ldNorm(uint32_t reg) { return known[reg] ? vfLoad(reg) : "normalize4(" + vfLoad(reg) + ")"; }

        // After a pair: which registers it left normalized (lower first, the upper's result lands last).
        void updateKnown(const Pair &p)
        {
            if (!p.iBit && !p.isNop && p.loVfReg != 0u)
            {
                const uint32_t r = p.loVfReg;
                bool k = false;
                const uint8_t sp = p.lowerOpHi == 0x40u ? p.lowerSpecial : 0xFFu;
                if (sp == 0x30u || sp == 0x31u) // MOVE / MR32: a copy of fs (MR32 rotates lanes, needs all of fs)
                {
                    const uint32_t fs = FS(p.lower);
                    const bool full = DEST(p.lower) == 0xFu;
                    k = known[fs] && (full || known[r]);
                }
                known[r] = k;
            }
            const Upper &u = p.up;
            if (u.kind == UKind::Nop || u.kind == UKind::Clip)
                return;
            if (u.kind == UKind::Arith)
            {
                if (u.toAcc)
                    known[32] = u.dest == 0xFu ? true : known[32];
                else if (u.fd != 0u)
                    known[u.fd] = u.dest == 0xFu ? true : known[u.fd];
                return;
            }
            if (u.kind == UKind::Itof)
            {
                if (u.ft != 0u)
                    known[u.ft] = u.dest == 0xFu ? true : known[u.ft];
                return;
            }
            if (u.kind == UKind::Abs)
            {
                if (u.ft != 0u)
                    known[u.ft] = known[u.fs] && (u.dest == 0xFu || known[u.ft]);
                return;
            }
            // FTOI (integer bits) and the decoded fallbacks: whatever they write is unknown
            if (p.upVfReg != 0u)
                known[p.upVfReg] = false;
            if (p.accWriteLanes)
                known[32] = false;
        }

        void emitStoreVf(uint32_t reg, uint8_t dest, const std::string &val, const char *indent)
        {
            if (dest == 0xFu)
                o << indent << "_mm_storeu_ps(s.vf[" << reg << "], " << val << ");\n";
            else
                o << indent << "_mm_storeu_ps(s.vf[" << reg << "], _mm_blendv_ps(_mm_loadu_ps(s.vf[" << reg << "]), " << val
                  << ", _mm_castsi128_ps(destMask(" << int(dest) << "))));\n";
        }

        bool emitUpper(const Pair &p, uint32_t idx)
        {
            const Upper &u = p.up;
            if (u.kind == UKind::Nop)
                return true;
            if (u.kind == UKind::Itof)
            {
                o << "        m.opItof(" << int(u.fs) << ", " << int(u.ft) << ", " << int(u.convShift) << ", destMask(" << int(u.dest) << "));\n";
                return true;
            }
            if (u.kind == UKind::Ftoi)
            {
                o << "        m.opFtoi(" << int(u.fs) << ", " << int(u.ft) << ", " << int(u.convShift) << ", destMask(" << int(u.dest) << "));\n";
                return true;
            }
            if (u.kind == UKind::Abs)
            {
                o << "        m.opAbs(" << int(u.fs) << ", " << int(u.ft) << ", destMask(" << int(u.dest) << "));\n";
                return true;
            }
            if (u.kind == UKind::Clip)
            {
                o << "        m.opClip(" << int(u.fs) << ", " << int(u.ft) << ");\n";
                return true;
            }
            if (u.kind != UKind::Arith)
            {
                o << "        m.execUpper(K[" << fallbackSlot(idx) << "].up);\n";
                return true;
            }
            o << "        {\n            const __m128 a = " << ldNorm(u.fs) << ";\n";
            switch (u.bsrc)
            {
            case BSrc::Vt:
                o << "            const __m128 b = " << ldNorm(u.ft) << ";\n";
                break;
            case BSrc::Bc:
                o << "            const __m128 bt = " << ldNorm(u.ft) << ";\n"
                  << "            const __m128 b = _mm_shuffle_ps(bt, bt, _MM_SHUFFLE(" << int(u.bc) << ", " << int(u.bc) << ", "
                  << int(u.bc) << ", " << int(u.bc) << "));\n";
                break;
            case BSrc::Q:
                o << "            const __m128 b = _mm_set1_ps(normalize1(s.q));\n";
                break;
            default:
                o << "            const __m128 b = _mm_set1_ps(normalize1(s.i));\n";
                break;
            }
            switch (u.arith)
            {
            case Arith::Add:
                o << "            __m128 r = _mm_add_ps(a, b);\n";
                break;
            case Arith::Sub:
                o << "            __m128 r = _mm_sub_ps(a, b);\n";
                break;
            case Arith::Mul:
                o << "            __m128 r = _mm_mul_ps(a, b);\n";
                break;
            case Arith::Madd:
                o << "            __m128 r = _mm_add_ps(" << (known[32] ? "_mm_loadu_ps(s.acc)" : "normalize4(_mm_loadu_ps(s.acc))")
                  << ", _mm_mul_ps(a, b));\n";
                break;
            case Arith::Msub:
                o << "            __m128 r = _mm_sub_ps(" << (known[32] ? "_mm_loadu_ps(s.acc)" : "normalize4(_mm_loadu_ps(s.acc))")
                  << ", _mm_mul_ps(a, b));\n";
                break;
            case Arith::Max:
                o << "            __m128 r = _mm_blendv_ps(b, a, _mm_cmpgt_ps(a, b));\n";
                break;
            default:
                o << "            __m128 r = _mm_blendv_ps(b, a, _mm_cmplt_ps(a, b));\n";
                break;
            }
            if (u.flags && (flagsNeeded.empty() || flagsNeeded[idx]))
                o << "            r = m.trackMacStatus ? m.aotFmacFlags(r, " << int(u.dest) << ") : normalize4(r);\n";
            else if (u.flags)
                o << "            r = normalize4(r); // flags never observed\n";
            if (u.toAcc)
            {
                if (u.dest == 0xFu)
                    o << "            _mm_storeu_ps(s.acc, r);\n";
                else
                    o << "            _mm_storeu_ps(s.acc, _mm_blendv_ps(_mm_loadu_ps(s.acc), r, _mm_castsi128_ps(destMask("
                      << int(u.dest) << "))));\n";
            }
            else if (u.fd != 0u)
                emitStoreVf(u.fd, u.dest, "r", "            ");
            o << "        }\n";
            return true;
        }

        uint32_t fallbackSlot(uint32_t idx)
        {
            if (!fallbackIndex[idx])
            {
                fallbackPairs.push_back(idx);
                fallbackIndex[idx] = static_cast<uint32_t>(fallbackPairs.size());
            }
            return fallbackIndex[idx] - 1u;
        }

        std::string vi(uint32_t r) { return "s.vi[" + std::to_string(r) + "]"; }

        // Loads 16 bytes of data memory at `addrExpr` into vf[t] under dest.
        void emitLoadQ(uint32_t t, uint8_t dest, const std::string &addrExpr)
        {
            if (t == 0u)
                return;
            o << "            { const uint32_t addr = " << addrExpr << ";\n";
            if (dest == 0xFu)
                o << "              std::memcpy(s.vf[" << t << "], data + addr, 16); }\n";
            else
            {
                o << "              float tmp[4]; std::memcpy(tmp, data + addr, 16);\n";
                for (uint32_t c = 0; c < 4u; ++c)
                    if (dest & laneBit(c))
                        o << "              s.vf[" << t << "][" << c << "] = tmp[" << c << "];\n";
                o << "            }\n";
            }
        }

        void emitStoreQ(uint32_t sreg, uint8_t dest, const std::string &addrExpr)
        {
            o << "            if (m.xg.active)\n                m.aotProgressXgkick();\n";
            o << "            { const uint32_t addr = " << addrExpr << ";\n";
            if (dest == 0xFu)
                o << "              std::memcpy(data + addr, s.vf[" << sreg << "], 16); }\n";
            else
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if (dest & laneBit(c))
                        o << "              std::memcpy(data + addr + " << c * 4u << ", &s.vf[" << sreg << "][" << c << "], 4);\n";
                o << "            }\n";
            }
        }


        // Instructions without a specialised form go through Impl::execLower with a decoded Pair (K[]).
        void emitLower(const Pair &p, uint32_t idx, uint32_t pc)
        {
            if (p.iBit || p.isNop)
                return;
            const uint32_t in = p.lower;
            const uint8_t op = p.lowerOpHi;
            const std::string mask = hx(src.vu0 ? 0xFFFu : 0x3FFFu);
            const uint8_t it = VIT(in), is = VIS(in), id = VID(in), dest = DEST(in);
            const uint8_t ft = FT(in), fs = FS(in);
            const int32_t imm11 = IMM11(in);
            o << "        {\n";
            switch (op)
            {
            case 0x00: // LQ
                emitLoadQ(ft, dest, "(static_cast<uint32_t>(static_cast<int32_t>(" + vi(is) + " + (" + std::to_string(imm11) +
                                        "))) * 16u) & " + mask);
                break;
            case 0x01: // SQ
                emitStoreQ(fs, dest, "(static_cast<uint32_t>(static_cast<int32_t>(" + vi(it) + " + (" + std::to_string(imm11) +
                                         "))) * 16u) & " + mask);
                break;
            case 0x08:
            case 0x09:
            {
                const int16_t imm = static_cast<int16_t>((in & 0x7FF) | ((in >> 10) & 0x7800));
                if (it != 0u)
                    o << "            " << vi(it) << " = static_cast<int16_t>(" << vi(is) << (op == 0x08 ? " + " : " - ") << "("
                      << imm << "));\n";
                break;
            }
            case 0x20: // B
                o << "            a.brPending = true;\n            a.brTarget = " << hx(staticTarget(pc)) << ";\n";
                break;
            case 0x21: // BAL
                if (it != 0u)
                    o << "            " << vi(it) << " = " << ((pc + 16u) / 8u) << ";\n";
                o << "            a.brPending = true;\n            a.brTarget = " << hx(staticTarget(pc)) << ";\n";
                break;
            case 0x24: // JR
                o << "            a.brPending = true;\n            a.brTarget = (static_cast<uint32_t>(static_cast<uint16_t>(m.readJumpVi("
                  << int(is) << "))) * 8u) & " << mask << ";\n";
                break;
            case 0x25: // JALR
                o << "            a.brTarget = (static_cast<uint32_t>(static_cast<uint16_t>(m.readJumpVi(" << int(is)
                  << "))) * 8u) & " << mask << ";\n";
                if (it != 0u)
                    o << "            " << vi(it) << " = " << ((pc + 16u) / 8u) << ";\n";
                o << "            a.brPending = true;\n";
                break;
            case 0x28:
            case 0x29:
                o << "            if ((static_cast<int16_t>(" << branchVi(is) << ") == static_cast<int16_t>(" << branchVi(it)
                  << ")) == " << (op == 0x28 ? "true" : "false") << ")\n            {\n"
                  << "                a.brPending = true;\n                a.brTarget = " << hx(staticTarget(pc)) << ";\n            }\n";
                break;
            case 0x2C:
            case 0x2D:
            case 0x2E:
            case 0x2F:
            {
                const char *cmp = op == 0x2C ? "< 0" : op == 0x2D ? "> 0" : op == 0x2E ? "<= 0" : ">= 0";
                o << "            if (static_cast<int16_t>(" << branchVi(is) << ") " << cmp << ")\n            {\n"
                  << "                a.brPending = true;\n                a.brTarget = " << hx(staticTarget(pc)) << ";\n            }\n";
                break;
            }
            case 0x40:
            {
                const uint8_t funct = in & 0x3Fu;
                if (funct == 0x30u || funct == 0x31u || funct == 0x34u || funct == 0x35u)
                {
                    if (id != 0u)
                    {
                        if (funct == 0x30u)
                            o << "            " << vi(id) << " = static_cast<int16_t>(" << vi(is) << " + " << vi(it) << ");\n";
                        else if (funct == 0x31u)
                            o << "            " << vi(id) << " = static_cast<int16_t>(" << vi(is) << " - " << vi(it) << ");\n";
                        else if (funct == 0x34u)
                            o << "            " << vi(id) << " = " << vi(is) << " & " << vi(it) << ";\n";
                        else
                            o << "            " << vi(id) << " = " << vi(is) << " | " << vi(it) << ";\n";
                    }
                    break;
                }
                if (funct == 0x32u)
                {
                    const int16_t imm5 = static_cast<int16_t>(static_cast<int32_t>((in >> 6) & 0x1F) << 27 >> 27);
                    if (it != 0u)
                        o << "            " << vi(it) << " = static_cast<int16_t>(" << vi(is) << " + (" << imm5 << "));\n";
                    break;
                }
                switch (p.lowerSpecial)
                {
                case 0x30: // MOVE
                    if (ft != 0u)
                        emitStoreVf(ft, dest, vfLoad(fs), "            ");
                    break;
                case 0x31: // MR32
                    if (ft != 0u)
                        emitStoreVf(ft, dest,
                                    "_mm_shuffle_ps(" + vfLoad(fs) + ", " + vfLoad(fs) + ", _MM_SHUFFLE(0, 3, 2, 1))",
                                    "            ");
                    break;
                case 0x34: // LQI
                    emitLoadQ(ft, dest, "(static_cast<uint32_t>(static_cast<uint16_t>(" + vi(is) + ")) * 16u) & " + mask);
                    if (is != 0u)
                        o << "            " << vi(is) << " = static_cast<int16_t>(" << vi(is) << " + 1);\n";
                    break;
                case 0x35: // SQI
                    emitStoreQ(fs, dest, "(static_cast<uint32_t>(static_cast<uint16_t>(" + vi(it) + ")) * 16u) & " + mask);
                    if (it != 0u)
                        o << "            " << vi(it) << " = static_cast<int16_t>(" << vi(it) << " + 1);\n";
                    break;
                case 0x36: // LQD
                    if (is != 0u)
                        o << "            " << vi(is) << " = static_cast<int16_t>(" << vi(is) << " - 1);\n";
                    emitLoadQ(ft, dest, "(static_cast<uint32_t>(static_cast<uint16_t>(" + vi(is) + ")) * 16u) & " + mask);
                    break;
                case 0x37: // SQD
                    if (it != 0u)
                        o << "            " << vi(it) << " = static_cast<int16_t>(" << vi(it) << " - 1);\n";
                    emitStoreQ(fs, dest, "(static_cast<uint32_t>(static_cast<uint16_t>(" + vi(it) + ")) * 16u) & " + mask);
                    break;
                case 0x3B: // WAITQ
                case 0x7B: // WAITP
                    break;
                case 0x3C: // MTIR
                    if (it != 0u)
                        o << "            { uint32_t fval; std::memcpy(&fval, &s.vf[" << int(fs) << "][" << ((in >> 21) & 3u)
                          << "], 4); " << vi(it) << " = static_cast<int32_t>(static_cast<int16_t>(fval & 0xFFFFu)); }\n";
                    break;
                case 0x3D: // MFIR
                    if (ft != 0u)
                        emitStoreVf(ft, dest,
                                    "_mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(static_cast<int16_t>(" + vi(is) +
                                        " & 0xFFFF))))",
                                    "            ");
                    break;
                case 0x3E: // ILWR
                case 0x3F: // ISWR
                {
                    const uint32_t comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                    const std::string addr = "(static_cast<uint32_t>(static_cast<uint16_t>(" + vi(is) + ")) * 16u) & " + mask;
                    if (p.lowerSpecial == 0x3E)
                    {
                        if (it != 0u)
                            o << "            { uint32_t v; std::memcpy(&v, data + ((" << addr << ") + " << comp * 4u << "), 4); "
                              << vi(it) << " = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu)); }\n";
                    }
                    else
                    {
                        o << "            if (m.xg.active)\n                m.aotProgressXgkick();\n";
                        o << "            { const uint32_t addr = " << addr << "; const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>("
                          << vi(it) << " & 0xFFFF));\n";
                        for (uint32_t c = 0; c < 4u; ++c)
                            if (dest & laneBit(c))
                                o << "              std::memcpy(data + addr + " << c * 4u << ", &v, 4);\n";
                        o << "            }\n";
                    }
                    break;
                }
                case 0x68: // XTOP
                    if (it != 0u)
                        o << "            " << vi(it) << " = static_cast<int32_t>(s.top & 0x3FFu);\n";
                    break;
                case 0x69: // XITOP
                    if (it != 0u)
                        o << "            " << vi(it) << " = static_cast<int32_t>(s.itop & 0x3FFu);\n";
                    break;
                case 0x6C: // XGKICK
                    o << "            m.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(" << vi(is) << ")));\n";
                    break;
                default:
                    o << "            m.execLower(" << hx(in) << ", K[" << fallbackSlot(idx) << "]);\n";
                    break;
                }
                break;
            }
            case 0x04: // ILW
            case 0x05: // ISW
            {
                const uint32_t comp = (dest & 0x8) ? 0 : (dest & 0x4) ? 1 : (dest & 0x2) ? 2 : 3;
                const std::string addr = "(static_cast<uint32_t>(static_cast<int32_t>(" + vi(is) + " + (" + std::to_string(imm11) +
                                         "))) * 16u) & " + mask;
                if (op == 0x04)
                {
                    if (it != 0u)
                        o << "            { uint32_t v; std::memcpy(&v, data + ((" << addr << ") + " << comp * 4u << "), 4); "
                          << vi(it) << " = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu)); }\n";
                }
                else
                {
                    o << "            if (m.xg.active)\n                m.aotProgressXgkick();\n";
                    o << "            { const uint32_t addr = " << addr << "; const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>("
                      << vi(it) << " & 0xFFFF));\n";
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (dest & laneBit(c))
                            o << "              std::memcpy(data + addr + " << c * 4u << ", &v, 4);\n";
                    o << "            }\n";
                }
                break;
            }
            default:
                o << "            m.execLower(" << hx(in) << ", K[" << fallbackSlot(idx) << "]);\n";
                break;
            }
            o << "        }\n";
        }

        void emitMarkWrites(const Pair &p)
        {
            if (p.loVfReg != 0u)
                o << "        m.markVfWrite(" << int(p.loVfReg) << ", " << int(p.loVfLanes) << ", m.cycle + " << int(p.loVfLatency)
                  << "u);\n";
            if (p.upVfReg != 0u)
                o << "        m.markVfWrite(" << int(p.upVfReg) << ", " << int(p.upVfLanes) << ", m.cycle + " << int(p.upVfLatency)
                  << "u);\n";
            if (p.viWriteMask)
            {
                o << "        m.viBusy |= " << hx(p.viWriteMask) << ";\n";
                for (uint32_t b = 0; b < 16u; ++b)
                    if (p.viWriteMask & (1u << b))
                        o << "        m.viReady[" << b << "] = m.cycle + " << int(p.viWriteLatency) << "u;\n";
            }
            if (p.accWriteLanes)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if (p.accWriteLanes & laneBit(c))
                        o << "        m.accReady[" << c << "] = m.cycle + kAccForwardLatency;\n";
                o << "        m.accMaxReady = std::max(m.accMaxReady, m.cycle + kAccForwardLatency);\n";
            }
        }

        std::vector<uint8_t> leader;

        // Block leaders: entries, static branch targets and return addresses (JR targets). Everything else is reached
        // by falling through inside a block.
        void findLeaders()
        {
            leader.assign(pairs.size(), 0);
            for (uint32_t i = 0; i < pairs.size(); ++i)
                if (reach[i] && (entry[i] || jumpTarget[i]))
                    leader[i] = 1;
            for (uint32_t t : dynTargets)
                if (reach[t >> 3])
                    leader[t >> 3] = 1;
            // A pair right after the delay slot of an unconditional jump or of the program end (E-bit) is never reached
            // by falling through, only by a jump (e.g. into a table of `JR vi15` return stubs: ZOE's Tempest attack
            // jumps to 0x1EB8/0x1EC8 between such stubs, which handed ~1,200 programs a frame to the interpreter) or by
            // MSCNT continuing after the end. Starting a block there lets the dispatcher enter it.
            for (uint32_t i = 2; i < pairs.size(); ++i)
            {
                const Pair &j = pairs[i - 2];
                if (!reach[i] || !reach[i - 2] || bail[i - 2])
                    continue;
                const uint8_t op = j.lowerOpHi;
                if (j.eBit || (isBranchPair(j) && (op == 0x20u || op == 0x21u || op == 0x24u || op == 0x25u)))
                    leader[i] = 1;
            }
            // IBxx not-taken continuations were marked as jump targets by explore(); they only need a block when
            // something jumps there, but keeping them as leaders is harmless and keeps blocks short.
        }

        // The pair's register/memory work (upper, then lower; I-bit immediate; upper result held back while the lower reads
        // the old value of the same register).
        void emitOps(const Pair &p, uint32_t idx, uint32_t pc)
        {
            if (p.iBit)
            {
                emitUpper(p, idx);
                o << "        { float imm; const uint32_t bits = " << hx(p.lower) << "; std::memcpy(&imm, &bits, 4); s.i = normalize1(imm); }\n";
            }
            else if (p.upperShadowReg != 0u)
            {
                const int r = p.upperShadowReg;
                o << "        {\n        float oldVf[4], upperVf[4];\n        std::memcpy(oldVf, s.vf[" << r << "], 16);\n";
                emitUpper(p, idx);
                o << "        std::memcpy(upperVf, s.vf[" << r << "], 16);\n        std::memcpy(s.vf[" << r << "], oldVf, 16);\n";
                emitLower(p, idx, pc);
                o << "        std::memcpy(s.vf[" << r << "], upperVf, 16);\n        }\n";
            }
            else
            {
                emitUpper(p, idx);
                emitLower(p, idx, pc);
            }
        }

        // ---- static-timing fast path --------------------------------------------------------------------------
        // A block whose pairs never wait on anything dynamic (no XGKICK, FDIV/EFU, WAITQ/WAITP, no hand-over to the
        // interpreter) has a fixed timing once the registers it reads from earlier blocks are ready in time. Its fast
        // version checks exactly that at entry, plus that no Q/P result lands and the cycle budget cannot run out inside
        // the block, and then runs with every stall worked out here: no per-pair ready checks or cycle bookkeeping; the
        // cycle is stored only where a helper reads it (flags, clip, decoded fallbacks, XGKICK progress before a store)
        // and the ready times of what the block wrote are stored once on the way out. The state after the block is the
        // same as after the per-pair version (the replay bench checks this). Anything else runs the per-pair version.
        // PS2X_VU_AOTGEN_FAST=0 at generation time: per-pair versions only (for comparison).
        static bool fastEnabled()
        {
            static const bool on = []
            {
                const char *e = std::getenv("PS2X_VU_AOTGEN_FAST");
                return !(e && *e == '0');
            }();
            return on;
        }
        // PS2X_VU_AOTGEN_STATS=1 at generation time: count fast/slow runs per block and print the busiest slow ones at
        // exit (analysis only; the counters cost time).
        static bool statsEnabled()
        {
            static const bool on = []
            {
                const char *e = std::getenv("PS2X_VU_AOTGEN_STATS");
                return e && *e == '1';
            }();
            return on;
        }
        std::vector<std::string> statRows; // "pc, pairs, reason"
        std::string slowReason;
        bool fastMode = false, fastFirst = false;
        int fastPrevBackupReg = -1;
        std::string fastPrevBackupVar;
        uint32_t fastBlocks = 0, totalBlocks = 0, statIndex = 0, statBase = 0, statLen = 0;

        // VI value a conditional branch compares: the old value when the previous pair just wrote that register.
        std::string branchVi(uint32_t reg)
        {
            if (!fastMode || fastFirst)
                return "m.readBranchVi(" + std::to_string(reg) + ")";
            if (reg == 0u)
                return "0";
            if (fastPrevBackupReg == static_cast<int>(reg))
                return fastPrevBackupVar;
            return "s.vi[" + std::to_string(reg) + "]";
        }

        bool fastPairOk(uint32_t idx) const
        {
            const Pair &p = pairs[idx];
            if (bail[idx] || p.scalarWait)
                return false;
            if (p.iBit || p.isNop)
                return true;
            const Usage &lo = p.lowerUsage;
            return lo.pipe != Pipe::Xgkick && lo.pipe != Pipe::Fdiv && lo.pipe != Pipe::Efu && !lo.waitQ && !lo.waitP;
        }

        // The VI register whose old value the next pair's branch reads (as emitPair decides), or -1.
        static int backupReg(const Pair &p)
        {
            if (p.iBit || p.lowerUsage.viWrite == 0u || !p.lowerUsage.delaysNextBranchRead)
                return -1;
            for (uint32_t b = 0; b < 16u; ++b)
                if (p.lowerUsage.viWrite & (1u << b))
                    return b > 0u ? static_cast<int>(b) : -1;
            return -1;
        }

        static std::string tag(uint32_t pc)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04X", pc);
            return buf;
        }

        // Ready times (cycles after block entry) written so far inside a block; -1 = not written in the block.
        struct Sched
        {
            int64_t vf[32][4], vfMax[32], vi[16], acc[4], accMax;
            Sched()
            {
                for (auto &r : vf)
                    for (auto &l : r)
                        l = -1;
                for (auto &v : vfMax)
                    v = -1;
                for (auto &v : vi)
                    v = -1;
                for (auto &v : acc)
                    v = -1;
                accMax = -1;
            }
        };

        std::string capture(const std::function<void()> &fn)
        {
            std::ostringstream tmp;
            o.swap(tmp);
            fn();
            o.swap(tmp);
            return tmp.str();
        }

        // Pipeline state on leaving the block after `sc`'s pairs, as the per-pair version leaves it.
        void emitWriteback(const Sched &sc, int64_t cycleOff, int lastBackup, const std::string &lastBackupVar, const char *ind)
        {
            o << ind << "m.cycle = c0 + " << cycleOff << "u;\n";
            uint32_t vfMask = 0;
            for (uint32_t r = 1; r < 32u; ++r)
            {
                bool any = false;
                for (uint32_t c = 0; c < 4u; ++c)
                    if (sc.vf[r][c] >= 0)
                    {
                        o << ind << "m.vfReady[" << r << "][" << c << "] = c0 + " << sc.vf[r][c] << "u;\n";
                        any = true;
                    }
                if (any)
                {
                    o << ind << "m.vfRegReady[" << r << "] = std::max(m.vfRegReady[" << r << "], c0 + " << sc.vfMax[r] << "u);\n";
                    vfMask |= 1u << r;
                }
            }
            if (vfMask)
                o << ind << "m.vfBusy |= " << hx(vfMask) << ";\n";
            uint32_t viMask = 0;
            for (uint32_t b = 0; b < 16u; ++b)
                if (sc.vi[b] >= 0)
                {
                    o << ind << "m.viReady[" << b << "] = c0 + " << sc.vi[b] << "u;\n";
                    viMask |= 1u << b;
                }
            if (viMask)
                o << ind << "m.viBusy |= " << hx(viMask) << ";\n";
            bool accAny = false;
            for (uint32_t c = 0; c < 4u; ++c)
                if (sc.acc[c] >= 0)
                {
                    o << ind << "m.accReady[" << c << "] = c0 + " << sc.acc[c] << "u;\n";
                    accAny = true;
                }
            if (accAny)
                o << ind << "m.accMaxReady = std::max(m.accMaxReady, c0 + " << sc.accMax << "u);\n";
            if (lastBackup > 0)
                o << ind << "m.viBackupValue = " << lastBackupVar << ";\n" << ind << "m.viBackupReg = " << lastBackup << ";\n"
                  << ind << "m.viBackupValid = true;\n";
            else
                o << ind << "m.viBackupValid = false;\n";
        }

        // Emits the fast version of the block at `start` (in front of its per-pair version) when it qualifies.
        bool emitFastBlock(uint32_t start)
        {
            std::vector<uint32_t> pcs;
            for (uint32_t pc = start, guard = 0; guard < pairs.size(); ++guard)
            {
                if (!fastPairOk(pc >> 3))
                {
                    const Pair &bp = pairs[pc >> 3];
                    const Usage &lo = bp.lowerUsage;
                    slowReason = bail[pc >> 3] ? "bail" : lo.pipe == Pipe::Xgkick ? "xgkick" : lo.pipe == Pipe::Fdiv ? "fdiv"
                               : lo.pipe == Pipe::Efu ? "efu" : lo.waitQ ? "waitq" : lo.waitP ? "waitp" : "scalarwait";
                    slowReason += "@" + tag(pc);
                    return false;
                }
                pcs.push_back(pc);
                const uint32_t nx = next(pc);
                if (nx == 0u || leader[nx >> 3] || !reach[nx >> 3])
                    break;
                pc = nx;
            }
            // Static schedule: issue cycle of each pair after entry, external readiness each one needs.
            Sched sc;
            std::vector<Sched> after(pcs.size());
            std::vector<int64_t> t(pcs.size());
            std::map<std::pair<int, uint32_t>, int64_t> ext; // (0 vf / 1 vi / 2 acc, reg) -> latest allowed ready cycle
            auto needExt = [&](int kind, uint32_t reg, int64_t at)
            {
                auto it = ext.find({kind, reg});
                if (it == ext.end() || at < it->second)
                    ext[{kind, reg}] = at;
            };
            for (size_t i = 0; i < pcs.size(); ++i)
            {
                const Pair &p = pairs[pcs[i] >> 3];
                int64_t ready = i == 0 ? 0 : t[i - 1] + 1;
                std::vector<std::pair<int, uint32_t>> exts;
                for (uint32_t k = 0; k < p.readCount; ++k)
                {
                    const VfRead &r = p.reads[k];
                    if (r.reg == 0u)
                        continue;
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (r.lanes & (8u >> c))
                        {
                            if (sc.vf[r.reg][c] >= 0)
                                ready = std::max(ready, sc.vf[r.reg][c]);
                            else
                                exts.push_back({0, r.reg});
                        }
                }
                for (uint32_t b = 1; b < 16u; ++b)
                    if (p.viReadMask & (1u << b))
                    {
                        if (sc.vi[b] >= 0)
                            ready = std::max(ready, sc.vi[b]);
                        else
                            exts.push_back({1, b});
                    }
                if (p.accReadLanes)
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (p.accReadLanes & (8u >> c))
                        {
                            if (sc.acc[c] >= 0)
                                ready = std::max(ready, sc.acc[c]);
                            else
                                exts.push_back({2, 0u});
                        }
                t[i] = ready;
                for (const auto &e : exts)
                    needExt(e.first, e.second, ready);
                auto writeVf = [&](uint32_t reg, uint32_t lanes, int64_t at)
                {
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (lanes & (8u >> c))
                            sc.vf[reg][c] = at;
                    sc.vfMax[reg] = std::max(sc.vfMax[reg], at);
                };
                if (p.loVfReg != 0u)
                    writeVf(p.loVfReg, p.loVfLanes, ready + p.loVfLatency);
                if (p.upVfReg != 0u)
                    writeVf(p.upVfReg, p.upVfLanes, ready + p.upVfLatency);
                for (uint32_t b = 0; b < 16u; ++b)
                    if (p.viWriteMask & (1u << b))
                        sc.vi[b] = ready + p.viWriteLatency;
                if (p.accWriteLanes)
                {
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (p.accWriteLanes & (8u >> c))
                            sc.acc[c] = ready + kAccForwardLatency;
                    sc.accMax = std::max(sc.accMax, ready + static_cast<int64_t>(kAccForwardLatency));
                }
                after[i] = sc;
            }
            const int64_t last = t.back();
            o << "    {\n        const uint64_t c0 = m.cycle;\n        if (c0 + " << last << "u < a.budgetEnd && m.nextScalarCycle > c0 + "
              << last << "u";
            for (const auto &e : ext)
            {
                if (e.first.first == 0)
                    o << "\n            && m.vfRegReady[" << e.first.second << "] <= c0 + " << e.second << "u";
                else if (e.first.first == 1)
                    o << "\n            && m.viReady[" << e.first.second << "] <= c0 + " << e.second << "u";
                else
                    o << "\n            && m.accMaxReady <= c0 + " << e.second << "u";
            }
            o << ")\n        {\n";
            resetKnown();
            if (statsEnabled())
                o << "        ++g_aotStat[" << statIndex << "].fast;\n";
            fastMode = true;
            for (size_t i = 0; i < pcs.size(); ++i)
            {
                const uint32_t pc = pcs[i], idx = pc >> 3;
                const Pair &p = pairs[idx];
                fastFirst = i == 0;
                fastPrevBackupReg = i == 0 ? -1 : backupReg(pairs[pcs[i - 1] >> 3]);
                fastPrevBackupVar = i == 0 ? std::string() : "oldVi_" + tag(pcs[i - 1]);
                const int myBackup = backupReg(p);
                const std::string myVar = "oldVi_" + tag(pc);
                o << "        // fast " << hx(pc, 4) << " at +" << t[i] << "\n";
                if (myBackup > 0)
                    o << "        const int32_t " << myVar << " = s.vi[" << myBackup << "];\n";
                const std::string ops = capture([&] { emitOps(p, idx, pc); });
                updateKnown(p);
                const bool usesCycle = ops.find("m.exec") != std::string::npos || ops.find("m.aotProgressXgkick") != std::string::npos ||
                                       ops.find("m.aotFmacFlags") != std::string::npos || ops.find("m.opClip") != std::string::npos ||
                                       ops.find("m.startXgkick") != std::string::npos;
                o << "        {\n";
                if (usesCycle)
                    o << "        m.cycle = c0 + " << t[i] << "u;\n";
                o << ops << "        }\n";
                // control, as emitPair, leaving the pipeline state behind first
                const uint32_t pv = prev(pc);
                const Pair &pp = at(pv);
                // (also for the first pair: a block can start on a delay slot when the slot is a jump target too)
                const bool prevBranch = reach[pv >> 3] && !bail[pv >> 3] && isBranchPair(pp);
                const bool prevE = reach[pv >> 3] && !bail[pv >> 3] && pp.eBit;
                if (prevBranch)
                {
                    o << "        if (a.brPending)\n        {\n            a.brPending = false;\n";
                    emitWriteback(after[i], t[i] + 1, myBackup, myVar, "            ");
                    if (prevE)
                        o << "            if (a.ending)\n            {\n                s.pc = a.brTarget;\n                return kEnd;\n            }\n";
                    const uint8_t bop = pp.lowerOpHi;
                    if (bop == 0x24u || bop == 0x25u)
                        o << "            return a.brTarget;\n";
                    else
                        emitJump(staticTarget(pv), "            ");
                    o << "        }\n";
                }
                if (prevE)
                {
                    o << "        if (a.ending)\n        {\n";
                    emitWriteback(after[i], t[i] + 1, myBackup, myVar, "            ");
                    o << "            s.pc = " << hx(next(pc)) << ";\n            return kEnd;\n        }\n";
                }
                if (p.eBit)
                    o << "        a.ending = true;\n";
            }
            emitWriteback(after.back(), last + 1, backupReg(pairs[pcs.back() >> 3]), "oldVi_" + tag(pcs.back()), "        ");
            o << "        ";
            emitJump(next(pcs.back()), "");
            o << "        }\n    }\n";
            fastMode = false;
            fastFirst = false;
            return true;
        }

        // Emits one pair; returns true when control can fall through to the next pair.
        bool emitPair(uint32_t pc)
        {
            const uint32_t idx = pc >> 3;
            const Pair &p = pairs[idx];
            o << "    // " << hx(pc, 4) << ": lower " << hx(p.lower, 8) << " upper " << hx(p.upper, 8) << "\n";
            if (bail[idx])
            {
                emitBail(hx(pc), "    ");
                return false;
            }
            o << "    {\n";
            o << "        if (m.cycle >= m.nextScalarCycle)\n            m.aotCommitScalars();\n";
            const bool xgkick = !p.iBit && !p.isNop && p.lowerUsage.pipe == Pipe::Xgkick;
            if (xgkick)
                o << "        if (m.xg.active)\n            m.aotWaitXgkick(a.budgetEnd);\n";
            o << "        {\n        uint64_t rdy = m.cycle;\n";
            emitReads(p);
            o << "        if (rdy > m.cycle)\n        {\n            if (rdy >= a.budgetEnd)\n";
            emitBail(hx(pc), "                ");
            o << "            m.aotAdvanceTo(rdy);\n        }\n";
            if (xgkick)
            {
                o << "        if (m.cycle >= a.budgetEnd)\n";
                emitBail(hx(pc), "            ");
            }
            o << "        }\n";
            // VI written by the lower: old value for the next pair's branch
            int writtenVi = -1;
            if (!p.iBit && p.lowerUsage.viWrite != 0u)
            {
                for (uint32_t b = 0; b < 16u; ++b)
                    if (p.lowerUsage.viWrite & (1u << b))
                    {
                        writtenVi = static_cast<int>(b);
                        break;
                    }
            }
            const bool backup = writtenVi > 0 && p.lowerUsage.delaysNextBranchRead;
            if (backup)
                o << "        const int32_t oldVi = s.vi[" << writtenVi << "];\n";
            emitOps(p, idx, pc);
            updateKnown(p);
            o << "        m.viBackupValid = false;\n";
            emitMarkWrites(p);
            if (backup)
                o << "        m.viBackupValue = oldVi;\n        m.viBackupReg = " << writtenVi << ";\n        m.viBackupValid = true;\n";
            // vf0 / vi0 are never written by compiled code or Impl::execUpper/execLower (every write checks for register 0),
            // so the interpreter's per-pair reset is done once at entry and exit instead.
            o << "        ++m.cycle;\n";

            // control: branch resolution for a delay slot, end after an E-bit delay slot, then fall through
            const uint32_t pv = prev(pc);
            const Pair &pp = at(pv);
            const bool prevBranch = reach[pv >> 3] && !bail[pv >> 3] && isBranchPair(pp);
            const bool prevE = reach[pv >> 3] && !bail[pv >> 3] && pp.eBit;
            if (prevBranch)
            {
                o << "        if (a.brPending)\n        {\n            a.brPending = false;\n";
                if (prevE)
                    o << "            if (a.ending)\n            {\n                s.pc = a.brTarget;\n                return kEnd;\n            }\n";
                const uint8_t bop = pp.lowerOpHi;
                if (bop == 0x24u || bop == 0x25u)
                    o << "            return a.brTarget;\n";
                else
                    emitJump(staticTarget(pv), "            ");
                o << "        }\n";
            }
            const uint32_t nx = next(pc);
            if (prevE)
                o << "        if (a.ending)\n        {\n            s.pc = " << hx(nx) << ";\n            return kEnd;\n        }\n";
            if (p.eBit)
                o << "        a.ending = true;\n";
            o << "    }\n";
            return true;
        }

        std::vector<std::string> blockNames;

        void emitBlocks()
        {
            findLeaders();
            blockNames.assign(pairs.size(), std::string());
            for (uint32_t i = 0; i < pairs.size(); ++i)
            {
                if (!leader[i])
                    continue;
                const uint32_t start = i * 8u;
                char nm[64];
                std::snprintf(nm, sizeof(nm), "b_%s_%04X", src.name.c_str(), start);
                blockNames[i] = nm;
                o << "uint32_t " << nm << "(VuFastEngine::Impl &m, AotState &a)\n{\n"
                  << "    VU1State &s = *m.st;\n    uint8_t *const data = m.data;\n    (void)data;\n"
                  << "    if (m.cycle >= a.budgetEnd)\n";
                emitBail(hx(start), "        ");
                ++totalBlocks;
                statIndex = statBase + static_cast<uint32_t>(statRows.size());
                slowReason = fastEnabled() ? "entry check" : "fast path off";
                {
                    uint32_t len = 0;
                    for (uint32_t q = start, gq = 0; gq < pairs.size(); ++gq)
                    {
                        ++len;
                        if (bail[q >> 3])
                            break;
                        const uint32_t nq = next(q);
                        if (nq == 0u || leader[nq >> 3] || !reach[nq >> 3])
                            break;
                        q = nq;
                    }
                    statLen = len;
                }
                if (fastEnabled() && emitFastBlock(start))
                    ++fastBlocks;
                resetKnown();
                if (statsEnabled())
                {
                    statRows.push_back("{\"" + src.name + " " + tag(start) + "\", " + std::to_string(statLen) + ", \"" + slowReason + "\"}");
                    o << "    ++g_aotStat[" << statIndex << "].slow;\n";
                }
                uint32_t pc = start;
                for (uint32_t guard = 0; guard < pairs.size(); ++guard)
                {
                    if (!emitPair(pc))
                        break;
                    const uint32_t nx = next(pc);
                    if (nx == 0u || leader[nx >> 3] || !reach[nx >> 3])
                    {
                        o << "    ";
                        emitJump(nx, "");
                        break;
                    }
                    pc = nx;
                }
                o << "}\n\n";
            }
        }

        void emitFunction()
        {
            fn = "aot_" + src.name;
            emitBlocks();
            o << "const AotBlockFn T_" << src.name << "[" << pairs.size() << "] = {\n";
            for (uint32_t i = 0; i < pairs.size(); ++i)
                o << "    " << (blockNames[i].empty() ? std::string("nullptr") : blockNames[i]) << ",\n";
            o << "};\n\n";
            o << "bool " << fn << "(VuFastEngine::Impl &m, uint32_t startPC, uint64_t budgetEnd)\n{\n"
              << "    return aotDispatch(m, T_" << src.name << ", " << pairs.size() << "u, startPC, budgetEnd);\n}\n\n";
        }
    };
}

namespace ps2_vu_aotgen
{
    bool generate(const std::vector<Source> &sources, const std::string &path)
    {
        std::ostringstream out;
        out << "// GENERATED by ps2_vu_aotgen.cpp (PS2X_VU_BENCH ...,aotgen=) from captured VU micro memory. Do not edit.\n"
            << "// Each image registers itself with VuFastEngine::registerAot and is used while its pairs match micro memory.\n"
            << "#include \"runtime/ps2_vu_fast_impl.h\"\n#include <algorithm>\n#include <cstdio>\n#include <vector>\n\n"
            << "namespace\n{\n"
            << "    // Branch / E-bit state shared by the blocks of one program run (the interpreter keeps it in VU1State).\n"
            << "    struct AotState\n    {\n        bool brPending = false;\n        uint32_t brTarget = 0;\n        bool ending = false;\n"
            << "        uint32_t bailPc = 0;\n        uint64_t budgetEnd = 0;\n    };\n"
            << "    constexpr uint32_t kEnd = 0xFFFF0000u;  // program ended (s.pc set)\n"
            << "    constexpr uint32_t kBail = 0xFFFF0001u; // hand over to the interpreter at a.bailPc\n"
            << "    using AotBlockFn = uint32_t (*)(VuFastEngine::Impl &, AotState &);\n"
            << "    struct AotStat { uint64_t fast = 0, slow = 0; };\n"
            << "    AotStat g_aotStat[STAT_COUNT];\n\n"
            << "    bool aotDispatch(VuFastEngine::Impl &m, const AotBlockFn *table, uint32_t count, uint32_t pc, uint64_t budgetEnd)\n    {\n"
            << "        VU1State &s = *m.st;\n        AotState a;\n        a.budgetEnd = budgetEnd;\n"
            << "        s.vf[0][0] = 0.0f;\n        s.vf[0][1] = 0.0f;\n        s.vf[0][2] = 0.0f;\n        s.vf[0][3] = 1.0f;\n        s.vi[0] = 0;\n"
            << "        for (;;)\n        {\n            const AotBlockFn f = (pc >> 3) < count ? table[pc >> 3] : nullptr;\n"
            << "            if (f == nullptr)\n            {\n                a.bailPc = pc;\n                break;\n            }\n"
            << "            pc = f(m, a);\n            if (pc == kEnd)\n            {\n                m.flush();\n                s.ebit = false;\n"
            << "                s.haltAfterDelaySlot = false;\n                m.pendingHaltD = false;\n                m.pendingHaltT = false;\n"
            << "                return true;\n            }\n            if (pc == kBail)\n                break;\n        }\n"
            << "        s.pc = a.bailPc;\n        s.branchPending = a.brPending;\n        s.branchTarget = a.brTarget;\n        s.branchDelay = 0u;\n"
            << "        s.ebit = a.ending;\n        return false;\n    }\n\n";
        std::vector<std::string> registrations;
        std::vector<std::string> allStatRows;
        for (const Source &src : sources)
        {
            Gen g(src);
            g.statBase = static_cast<uint32_t>(allStatRows.size());
            g.explore();
            g.computeFlagLiveness();
            // body first (it decides which pairs need a decoded fallback), then K[] in front of it
            g.emitFunction();
            const std::string body = g.o.str();
            std::ostringstream head;
            uint32_t compiled = 0, bails = 0;
            for (uint32_t i = 0; i < g.pairs.size(); ++i)
                if (g.reach[i])
                {
                    ++compiled;
                    bails += g.bail[i];
                }
            head << "    // " << src.name << ": " << (src.vu0 ? "VU0" : "VU1") << ", " << compiled << " pairs (" << bails
                 << " handed to the interpreter), " << src.entries.size() << " captured entries, " << g.fallbackPairs.size()
                 << " instructions via Impl::execUpper/execLower\n";
            head << "    const Pair K_" << src.name << "[] = {\n";
            for (uint32_t idx : g.fallbackPairs)
                head << "        VuFastEngine::Impl::decodePairWords(" << hx(g.pairs[idx].lower) << ", " << hx(g.pairs[idx].upper)
                     << ", " << (src.vu0 ? "true" : "false") << "),\n";
            if (g.fallbackPairs.empty())
                head << "        VuFastEngine::Impl::decodePairWords(0u, 0u, false),\n";
            head << "    };\n";
            head << "    const uint32_t W_" << src.name << "[] = {\n";
            for (uint32_t i = 0; i < g.pairs.size(); ++i)
                if (g.reach[i])
                    head << "        " << hx(i * 8u) << ", " << hx(g.pairs[i].lower, 8) << ", " << hx(g.pairs[i].upper, 8) << ",\n";
            head << "    };\n\n";
            std::string fixedBody = body;
            // K[] references inside the body name the per-image table
            size_t pos = 0;
            while ((pos = fixedBody.find("K[", pos)) != std::string::npos)
            {
                fixedBody.replace(pos, 2, "K_" + src.name + "[");
                pos += 3 + src.name.size();
            }
            for (const std::string &r : g.statRows)
                allStatRows.push_back(r);
            out << head.str() << fixedBody;
            registrations.push_back("    VuFastEngine::registerAot({" + std::string(src.vu0 ? "true" : "false") + ", W_" + src.name +
                                    ", static_cast<uint32_t>(sizeof(W_" + src.name + ") / sizeof(uint32_t) / 3u), &aot_" + src.name +
                                    ", \"" + src.name + "\"});");
            std::cerr << "[vu:aotgen] " << src.name << ": " << compiled << " pairs, " << bails << " bail, " << g.fallbackPairs.size()
                      << " fallback instructions, static-timing fast path for " << g.fastBlocks << " of " << g.totalBlocks << " blocks"
                      << std::endl;
        }
        if (allStatRows.empty())
            allStatRows.push_back("{\"none\", 0, \"\"}");
        out << "    struct AotStatInfo { const char *name; uint32_t pairs; const char *reason; };\n"
            << "    const AotStatInfo g_aotStatInfo[] = {\n";
        for (const std::string &r : allStatRows)
            out << "        " << r << ",\n";
        out << "    };\n"
            << "    struct AotStatReport\n    {\n        ~AotStatReport()\n        {\n"
            << "            uint64_t fastPairs = 0, slowPairs = 0;\n            std::vector<std::pair<uint64_t, uint32_t>> rows;\n"
            << "            for (uint32_t i = 0; i < STAT_COUNT; ++i)\n            {\n"
            << "                fastPairs += g_aotStat[i].fast * g_aotStatInfo[i].pairs;\n"
            << "                const uint64_t sp = g_aotStat[i].slow * g_aotStatInfo[i].pairs;\n"
            << "                slowPairs += sp;\n                if (sp)\n                    rows.push_back({sp, i});\n            }\n"
            << "            if (fastPairs + slowPairs == 0)\n                return;\n"
            << "            std::sort(rows.rbegin(), rows.rend());\n"
            << "            std::fprintf(stderr, \"[vu:aotgen] stats: fast-path pairs %.1f%% of %llu\\n\", 100.0 * fastPairs / (fastPairs + slowPairs),\n"
            << "                         static_cast<unsigned long long>(fastPairs + slowPairs));\n"
            << "            for (size_t k = 0; k < rows.size() && k < 25; ++k)\n"
            << "                std::fprintf(stderr, \"[vu:aotgen] slow %-14s %3u pairs  %5.1f%%  fast runs %llu, slow runs %llu  (%s)\\n\", g_aotStatInfo[rows[k].second].name,\n"
            << "                             g_aotStatInfo[rows[k].second].pairs, 100.0 * rows[k].first / (fastPairs + slowPairs),\n"
            << "                             static_cast<unsigned long long>(g_aotStat[rows[k].second].fast),\n"
            << "                             static_cast<unsigned long long>(g_aotStat[rows[k].second].slow), g_aotStatInfo[rows[k].second].reason);\n"
            << "        }\n    } g_aotStatReport;\n";
        out << "    struct Register\n    {\n        Register()\n        {\n";
        for (const std::string &r : registrations)
            out << "    " << r << "\n";
        out << "        }\n    } g_register;\n}\n";
        std::string text = out.str();
        for (size_t pos = 0; (pos = text.find("STAT_COUNT", pos)) != std::string::npos;)
            text.replace(pos, 10, std::to_string(allStatRows.size()));
        std::ofstream f(path, std::ios::binary);
        if (!f)
            return false;
        f << text;
        return static_cast<bool>(f);
    }
}
