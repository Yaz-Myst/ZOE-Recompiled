// VU capture / replay benchmark - see runtime/ps2_vu_bench.h.
#include "runtime/ps2_vu_bench.h"

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu_fast.h"
#include "runtime/ps2_vu_fast_impl.h" // deps=: decodePairWords and the analysis hooks
#include "ps2_game_clock.h"
#include "ps2_profiler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
    constexpr uint32_t kMagic = 0x31435556u; // "VUC1"

    struct RecordHeader
    {
        uint32_t magic;
        uint8_t unit;
        uint8_t resume;
        uint8_t hasCode;
        uint8_t reserved;
        uint32_t startPC, top, itop, maxCycles;
        uint32_t codeSize, dataSize, stateSize;
        uint64_t tick;
    };

    struct CaptureConfig
    {
        bool enabled = false;
        std::string path;
        std::vector<std::pair<uint64_t, uint64_t>> windows; // [first, last] vsync ticks
        uint64_t lastTick = 0;
        bool inWindow(uint64_t tick) const
        {
            for (const auto &w : windows)
                if (tick >= w.first && tick <= w.second)
                    return true;
            return false;
        }
    };

    const CaptureConfig &captureConfig()
    {
        static const CaptureConfig cfg = []
        {
            CaptureConfig c;
            const char *env = std::getenv("PS2X_VU_CAPTURE");
            if (!env || !*env)
                return c;
            std::string s(env);
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos <= s.size())
            {
                size_t comma = s.find(',', pos);
                if (comma == std::string::npos)
                    comma = s.size();
                parts.push_back(s.substr(pos, comma - pos));
                pos = comma + 1;
            }
            if (parts.size() < 3u || (parts.size() % 2u) == 0u)
            {
                std::cerr << "[vu:capture] expected <file>,<firstTick>,<lastTick>[,<firstTick>,<lastTick>...]" << std::endl;
                return c;
            }
            c.path = parts[0];
            for (size_t i = 1; i + 1 < parts.size(); i += 2)
            {
                const uint64_t a = std::strtoull(parts[i].c_str(), nullptr, 10), b = std::strtoull(parts[i + 1].c_str(), nullptr, 10);
                c.windows.emplace_back(a, b);
                c.lastTick = std::max(c.lastTick, b);
            }
            c.enabled = true;
            return c;
        }();
        return cfg;
    }

    struct CaptureState
    {
        std::mutex mutex;
        FILE *file = nullptr;
        bool done = false;
        uint64_t lastGeneration[2] = {~0ull, ~0ull};
        const uint8_t *lastCode[2] = {nullptr, nullptr};
        uint64_t calls = 0, bytes = 0;
    };

    CaptureState &captureState()
    {
        static CaptureState s;
        return s;
    }

    uint64_t fnv(uint64_t h, const void *p, size_t n)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }

    // Fast 64-bit mixing hash for large buffers (data memory, packets).
    uint64_t hashBytes(uint64_t h, const uint8_t *p, size_t n)
    {
        constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
        size_t i = 0;
        for (; i + 8 <= n; i += 8)
        {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            h = (h ^ v) * K;
            h ^= h >> 29;
        }
        for (; i < n; ++i)
            h = (h ^ p[i]) * K;
        return h ^ (h >> 31);
    }

    // relaxed: without the MAC/status registers, which VU1 programs that never read them may leave stale.
    uint64_t hashState(const VU1State &s, bool relaxed = false)
    {
        uint64_t h = 1469598103934665603ull;
        h = fnv(h, s.vf, sizeof(s.vf));
        h = fnv(h, s.vi, sizeof(s.vi));
        h = fnv(h, s.acc, sizeof(s.acc));
        h = fnv(h, &s.q, sizeof(s.q));
        h = fnv(h, &s.p, sizeof(s.p));
        h = fnv(h, &s.i, sizeof(s.i));
        h = fnv(h, &s.pc, sizeof(s.pc));
        if (!relaxed)
            h = fnv(h, &s.mac, sizeof(s.mac));
        h = fnv(h, &s.clip, sizeof(s.clip));
        if (!relaxed)
            h = fnv(h, &s.status, sizeof(s.status));
        const uint8_t bits[] = {s.ebit, s.haltAfterDelaySlot, s.stoppedByD, s.stoppedByT, s.branchPending};
        h = fnv(h, bits, sizeof(bits));
        return h;
    }

    struct Record
    {
        RecordHeader hdr{};
        uint32_t codeIndex = 0; // into codes
        VU1State state{};
        std::vector<uint8_t> data;
    };

    struct BenchOptions
    {
        std::string capture, writeSums, checkSums, writeRelaxed, checkRelaxed, profileDir, aotgen, deps;
        size_t maxCalls = ~size_t(0);
        uint32_t iterations = 5;
        int unitFilter = -1;
    };

    BenchOptions parseBench(const char *env)
    {
        BenchOptions o;
        std::string s(env);
        size_t pos = 0;
        bool first = true;
        while (pos <= s.size())
        {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos)
                comma = s.size();
            const std::string item = s.substr(pos, comma - pos);
            if (first)
                o.capture = item;
            else if (item.rfind("iters=", 0) == 0)
                o.iterations = static_cast<uint32_t>(std::max(1, std::atoi(item.c_str() + 6)));
            else if (item.rfind("write=", 0) == 0)
                o.writeSums = item.substr(6);
            else if (item.rfind("check=", 0) == 0)
                o.checkSums = item.substr(6);
            else if (item.rfind("writer=", 0) == 0)
                o.writeRelaxed = item.substr(7);
            else if (item.rfind("checkr=", 0) == 0)
                o.checkRelaxed = item.substr(7);
            else if (item.rfind("aotgen=", 0) == 0)
                o.aotgen = item.substr(7);
            else if (item.rfind("deps=", 0) == 0)
                o.deps = item.substr(5);
            else if (item.rfind("prof=", 0) == 0)
                o.profileDir = item.substr(5);
            else if (item.rfind("max=", 0) == 0)
                o.maxCalls = static_cast<size_t>(std::strtoull(item.c_str() + 4, nullptr, 10));
            else if (item.rfind("unit=", 0) == 0)
                o.unitFilter = std::atoi(item.c_str() + 5);
            first = false;
            pos = comma + 1;
        }
        return o;
    }
}

namespace ps2_vu_bench
{
    bool captureEnabled()
    {
        return captureConfig().enabled && !captureState().done;
    }

    void captureCall(uint32_t unit, bool resume, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles,
                     const VU1State &state, const uint8_t *code, uint32_t codeSize, const uint8_t *data, uint32_t dataSize,
                     uint64_t codeGeneration)
    {
        const CaptureConfig &cfg = captureConfig();
        if (!cfg.enabled)
            return;
        const uint64_t tick = ps2CurrentVsyncTick();
        CaptureState &cs = captureState();
        std::lock_guard<std::mutex> lock(cs.mutex);
        if (cs.done || (tick <= cfg.lastTick && !cfg.inWindow(tick)))
            return;
        if (tick > cfg.lastTick)
        {
            if (cs.file)
            {
                std::fclose(cs.file);
                cs.file = nullptr;
                std::cerr << "[vu:capture] done: " << cs.calls << " calls, " << cs.bytes / (1024 * 1024) << " MB -> " << cfg.path
                          << std::endl;
            }
            cs.done = true;
            return;
        }
        if (!cs.file)
        {
            cs.file = std::fopen(cfg.path.c_str(), "wb");
            if (!cs.file)
            {
                std::cerr << "[vu:capture] cannot open " << cfg.path << std::endl;
                cs.done = true;
                return;
            }
            std::cerr << "[vu:capture] recording " << cfg.windows.size() << " window(s) up to tick " << cfg.lastTick << ", first call at tick "
                      << tick << std::endl;
        }
        unit &= 1u;
        RecordHeader h{};
        h.magic = kMagic;
        h.unit = static_cast<uint8_t>(unit);
        h.resume = resume ? 1u : 0u;
        h.hasCode = (codeGeneration != cs.lastGeneration[unit] || code != cs.lastCode[unit]) ? 1u : 0u;
        h.startPC = startPC;
        h.top = top;
        h.itop = itop;
        h.maxCycles = maxCycles;
        h.codeSize = codeSize;
        h.dataSize = dataSize;
        h.stateSize = sizeof(VU1State);
        h.tick = tick;
        cs.lastGeneration[unit] = codeGeneration;
        cs.lastCode[unit] = code;
        std::fwrite(&h, sizeof(h), 1, cs.file);
        std::fwrite(&state, sizeof(VU1State), 1, cs.file);
        if (h.hasCode)
            std::fwrite(code, 1, codeSize, cs.file);
        std::fwrite(data, 1, dataSize, cs.file);
        ++cs.calls;
        cs.bytes += sizeof(h) + sizeof(VU1State) + (h.hasCode ? codeSize : 0u) + dataSize;
    }

    // ---- deps=<report file>: which VU1 program runs depend on which (read-only; can VU1 work run in parallel?) --------
    // A task is an MSCAL run plus the MSCNT runs that continue it. The interpreter loop reports every pair before it
    // executes (ps2_vu_analysis hooks), so each task's inputs are known: VF lanes, VI, ACC, Q, P, I, R, MAC flags (unless
    // the task set them first), clip flags (unless it did 4 CLIPs first) and data qwords it reads before writing them
    // itself, and which earlier task last wrote each one. Qwords that change between runs without a task writing them
    // were uploaded by VIF1: no dependency. Status sticky bits are not tracked (ZOE's VU1 code never reads them).
    namespace deps
    {
        enum : int { kVf = 0, kVi = 128, kAcc = 144, kQ = 148, kP = 149, kI = 150, kR = 151, kMac = 152, kClip = 153, kMem = 256,
                     kItems = 256 + 1024 };
        std::vector<int64_t> lastWriter;
        std::vector<uint32_t> stamp;
        uint32_t curStamp = 0;
        int64_t curTask = -1;
        bool firstPair = false, flagWritten = false;
        uint32_t clips = 0;
        std::set<int64_t> *curDeps = nullptr;
        std::map<int64_t, std::vector<int>> *curCause = nullptr;
        uint64_t lateXgkickReads = 0, silentWrites = 0, realWrites = 0;
        // Writes count only when they change the value (a task that rewrites a constant GIF tag or register with the
        // same value is no dependency: an earlier copy of memory already holds it). Decided per run, after it ends.
        std::vector<uint32_t> runStamp;
        uint32_t curRun = 0;
        std::vector<int> writtenRun;

        void read(int item)
        {
            if (stamp[item] == curStamp)
                return;
            const int64_t w = lastWriter[item];
            if (w >= 0 && w != curTask)
            {
                curDeps->insert(w);
                std::vector<int> &v = (*curCause)[w];
                if (v.size() < 16 && std::find(v.begin(), v.end(), item) == v.end())
                    v.push_back(item);
            }
        }
        void write(int item)
        {
            stamp[item] = curStamp;
            if (runStamp[item] != curRun)
            {
                runStamp[item] = curRun;
                writtenRun.push_back(item);
            }
        }

        bool changed(int it, const VU1State &a, const uint8_t *da, const VU1State &b, const uint8_t *db)
        {
            if (it < kVi)
                return std::memcmp(&a.vf[it / 4][it % 4], &b.vf[it / 4][it % 4], 4) != 0;
            if (it < kAcc)
                return a.vi[it - kVi] != b.vi[it - kVi];
            if (it < kQ)
                return std::memcmp(&a.acc[it - kAcc], &b.acc[it - kAcc], 4) != 0;
            if (it >= kMem)
                return std::memcmp(da + (it - kMem) * 16, db + (it - kMem) * 16, 16) != 0;
            switch (it)
            {
            case kQ: return std::memcmp(&a.q, &b.q, 4) != 0;
            case kP: return std::memcmp(&a.p, &b.p, 4) != 0;
            case kI: return std::memcmp(&a.i, &b.i, 4) != 0;
            case kR: return a.r != b.r;
            case kMac: return a.mac != b.mac || a.status != b.status;
            case kClip: return a.clip != b.clip;
            default: return true;
            }
        }
        int qw(int32_t qwordIndex) { return kMem + static_cast<int>(static_cast<uint32_t>(qwordIndex) & 1023u); }

        void onPair(uint32_t lower, uint32_t upper, const VU1State &s)
        {
            firstPair = true;
            const Pair pr = VuFastEngine::Impl::decodePairWords(lower, upper, false);
            for (uint32_t k = 0; k < pr.readCount; ++k)
                if (pr.reads[k].reg != 0u)
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (pr.reads[k].lanes & (8u >> c))
                            read(kVf + pr.reads[k].reg * 4 + static_cast<int>(c));
            for (uint32_t b = 1; b < 16u; ++b)
                if (pr.viReadMask & (1u << b))
                    read(kVi + static_cast<int>(b));
            for (uint32_t c = 0; c < 4u; ++c)
                if (pr.accReadLanes & (8u >> c))
                    read(kAcc + static_cast<int>(c));
            if (pr.up.kind == UKind::Arith && pr.up.bsrc == BSrc::Q)
                read(kQ);
            if (pr.up.kind == UKind::Arith && pr.up.bsrc == BSrc::I)
                read(kI);
            if (!pr.iBit && !pr.isNop)
            {
                const uint32_t in = pr.lower;
                const uint8_t op = pr.lowerOpHi, sp = op == 0x40u ? pr.lowerSpecial : 0xFFu;
                const uint8_t it = VIT(in), is = VIS(in);
                const int32_t imm11 = IMM11(in);
                const auto vi = [&](uint8_t r) { return r == 0u ? 0 : s.vi[r]; };
                const auto u16 = [](int32_t v) { return static_cast<int32_t>(static_cast<uint16_t>(v)); };
                if (op == 0x00u || op == 0x04u) // LQ / ILW
                    read(qw(vi(is) + imm11));
                else if (sp == 0x34u || sp == 0x3Eu) // LQI / ILWR
                    read(qw(u16(vi(is))));
                else if (sp == 0x36u) // LQD
                    read(qw(u16(vi(is) - 1)));
                if (op >= 0x14u && op <= 0x1Bu && op != 0x15u && op != 0x19u && !flagWritten) // FS*/FM* reads
                    read(kMac);
                if ((op == 0x10u || op == 0x12u || op == 0x13u || op == 0x1Cu) && clips < 4u) // FCEQ/FCAND/FCOR/FCGET
                    read(kClip);
                if (sp == 0x64u) // MFP
                    read(kP);
                if (sp == 0x40u || sp == 0x41u || sp == 0x43u) // RNEXT / RGET / RXOR
                    read(kR);
                // writes of the lower
                if (op == 0x01u || op == 0x05u) // SQ / ISW
                    write(qw(vi(it) + imm11));
                else if (sp == 0x35u || sp == 0x3Fu) // SQI / ISWR
                    write(qw(u16(vi(it))));
                else if (sp == 0x37u) // SQD
                    write(qw(u16(vi(it) - 1)));
                if (op == 0x11u) // FCSET
                {
                    write(kClip);
                    clips = 4u;
                }
                if (op == 0x15u) // FSSET
                    write(kMac);
                if (sp == 0x38u || sp == 0x39u || sp == 0x3Au) // DIV / SQRT / RSQRT
                    write(kQ);
                if (sp >= 0x70u && sp <= 0x7Au)
                    write(kP);
                if (sp == 0x40u || sp == 0x42u || sp == 0x43u)
                    write(kR);
            }
            if (pr.iBit)
                write(kI);
            if (pr.up.kind == UKind::Clip)
            {
                write(kClip);
                ++clips;
            }
            if (pr.up.flags && pr.up.dest != 0u)
            {
                write(kMac);
                flagWritten = true;
            }
            const auto writeVf = [](uint8_t reg, uint8_t lanes)
            {
                if (reg == 0u)
                    return;
                for (uint32_t c = 0; c < 4u; ++c)
                    if (lanes & (8u >> c))
                        write(kVf + reg * 4 + static_cast<int>(c));
            };
            writeVf(pr.loVfReg, pr.loVfLanes);
            writeVf(pr.upVfReg, pr.upVfLanes);
            for (uint32_t b = 1; b < 16u; ++b)
                if (pr.viWriteMask & (1u << b))
                    write(kVi + static_cast<int>(b));
            for (uint32_t c = 0; c < 4u; ++c)
                if (pr.accWriteLanes & (8u >> c))
                    write(kAcc + static_cast<int>(c));
        }

        void onXgkick(uint32_t addr)
        {
            if (!firstPair)
            {
                ++lateXgkickReads; // the previous run's packet finishing at this run's start
                return;
            }
            static const bool noKick = std::getenv("PS2X_VU_DEPS_NOKICK") != nullptr; // XGKICK reads are no dependency
            if (noKick)
                return;
            read(kMem + static_cast<int>((addr >> 4) & 1023u));
        }

        std::string itemName(int it)
        {
            static const char *lane = "xyzw";
            if (it < kVi)
                return "vf" + std::to_string(it / 4) + "." + lane[it % 4];
            if (it < kAcc)
                return "vi" + std::to_string(it - kVi);
            if (it < kQ)
                return std::string("acc.") + lane[it - kAcc];
            if (it >= kMem)
            {
                char b[32];
                std::snprintf(b, sizeof(b), "mem 0x%03X", static_cast<unsigned>((it - kMem) & ~31) * 16u);
                return b; // 512-byte blocks
            }
            static const char *names[] = {"Q", "P", "I", "R", "MAC", "clip"};
            return it - kQ < 6 ? names[it - kQ] : "?";
        }
        std::string itemGroup(int it)
        {
            if (it < kVi)
                return "vf" + std::to_string(it / 4);
            if (it < kAcc)
                return "vi" + std::to_string(it - kVi);
            if (it < kQ)
                return "acc";
            return itemName(it);
        }

        struct Task
        {
            uint64_t tick = 0;
            uint32_t pc = 0, runs = 0;
            uint64_t cycles = 0;
            std::set<int64_t> deps;
            std::map<int64_t, std::vector<int>> cause;
        };

        void run(const std::vector<Record> &records, const std::vector<std::vector<uint8_t>> &codes, const std::string &reportPath)
        {
            const char *aot = std::getenv("PS2X_VU_AOT");
            if (!aot || *aot != '0')
            {
                std::cerr << "[vu:bench] deps= needs the interpreter (vu_bench.ps1 -NoAot)" << std::endl;
                std::exit(2);
            }
            lastWriter.assign(kItems, -1);
            stamp.assign(kItems, 0);
            runStamp.assign(kItems, 0);
            std::vector<Task> tasks;
            VuFastEngine engine(false);
            engine.setPacketSink([](const uint8_t *, uint32_t) {});
            ps2_vu_analysis::g_pair = &onPair;
            ps2_vu_analysis::g_xgkickRead = &onXgkick;
            std::vector<uint8_t> work, prevEnd;
            uint64_t prevCycles = 0, vifQwords = 0, regMismatch = 0;
            VU1State prevState{};
            bool havePrev = false;
            for (const Record &r : records)
            {
                if (r.hdr.unit != 1u)
                    continue;
                if (!r.hdr.resume || tasks.empty())
                {
                    tasks.emplace_back();
                    tasks.back().tick = r.hdr.tick;
                    tasks.back().pc = r.hdr.resume ? r.state.pc : r.hdr.startPC;
                    curTask = static_cast<int64_t>(tasks.size()) - 1;
                    curStamp = static_cast<uint32_t>(curTask) + 1u;
                    flagWritten = false;
                    clips = 0;
                }
                Task &t = tasks.back();
                curDeps = &t.deps;
                curCause = &t.cause;
                ++t.runs;
                if (havePrev)
                {
                    for (uint32_t q = 0; q < 1024u && (q + 1u) * 16u <= r.data.size() && (q + 1u) * 16u <= prevEnd.size(); ++q)
                        if (std::memcmp(prevEnd.data() + q * 16u, r.data.data() + q * 16u, 16) != 0)
                        {
                            lastWriter[kMem + static_cast<int>(q)] = -2; // VIF1
                            ++vifQwords;
                        }
                    if (std::memcmp(prevState.vf, r.state.vf, sizeof(prevState.vf)) != 0 ||
                        std::memcmp(prevState.vi, r.state.vi, sizeof(prevState.vi)) != 0)
                        ++regMismatch;
                }
                work.assign(r.data.begin(), r.data.end());
                VU1State st = r.state;
                firstPair = false;
                ++curRun;
                writtenRun.clear();
                engine.setCodeGeneration(static_cast<uint64_t>(r.codeIndex) + 1u);
                const std::vector<uint8_t> &code = codes[r.codeIndex];
                if (r.hdr.resume)
                    engine.resume(st, code.data(), r.hdr.codeSize, work.data(), r.hdr.dataSize, nullptr, nullptr, r.hdr.top, r.hdr.itop,
                                  r.hdr.maxCycles);
                else
                    engine.execute(st, code.data(), r.hdr.codeSize, work.data(), r.hdr.dataSize, nullptr, nullptr, r.hdr.startPC,
                                   r.hdr.top, r.hdr.itop, r.hdr.maxCycles);
                t.cycles += st.cycles - prevCycles;
                prevCycles = st.cycles;
                for (int item : writtenRun)
                {
                    if (changed(item, r.state, r.data.data(), st, work.data()))
                    {
                        lastWriter[item] = curTask;
                        ++realWrites;
                    }
                    else
                        ++silentWrites;
                }
                prevEnd = work;
                prevState = st;
                havePrev = true;
            }
            ps2_vu_analysis::g_pair = nullptr;
            ps2_vu_analysis::g_xgkickRead = nullptr;

            const size_t n = tasks.size();
            uint64_t total = 0;
            for (const Task &t : tasks)
                total += t.cycles;
            // nearest dependency, critical path, list scheduling on P workers (tasks issued in program order)
            std::map<std::string, uint64_t> histo;
            std::map<std::string, uint64_t> causeNear; // what makes a task wait for the one right before it
            std::vector<uint64_t> cp(n, 0);
            uint64_t critical = 0;
            for (size_t k = 0; k < n; ++k)
            {
                const Task &t = tasks[k];
                uint64_t before = 0;
                int64_t nearest = -1;
                for (int64_t d : t.deps)
                {
                    before = std::max(before, cp[static_cast<size_t>(d)]);
                    nearest = std::max(nearest, d);
                }
                cp[k] = before + t.cycles;
                critical = std::max(critical, cp[k]);
                const int64_t dist = nearest < 0 ? -1 : static_cast<int64_t>(k) - nearest;
                histo[dist < 0 ? "a none" : dist == 1 ? "b 1 (previous task)" : dist <= 4 ? "c 2-4" : dist <= 16 ? "d 5-16" : "e >16"] +=
                    t.cycles;
                if (dist == 1)
                {
                    std::set<std::string> groups;
                    for (int item : t.cause.at(nearest))
                        groups.insert(itemGroup(item));
                    for (const std::string &g : groups)
                        causeNear[g] += t.cycles;
                }
            }
            FILE *rep = std::fopen(reportPath.c_str(), "w");
            const auto out = [&](const std::string &line)
            {
                std::cerr << "[vu:bench] " << line << std::endl;
                if (rep)
                    std::fprintf(rep, "%s\n", line.c_str());
            };
            char buf[512];
            std::snprintf(buf, sizeof(buf), "deps: %zu VU1 tasks, %llu VU1 cycles; VIF-written qwords %llu; register mismatches between runs %llu; "
                                            "late XGKICK reads %llu; writes that changed a value %llu, same-value writes %llu",
                          n, static_cast<unsigned long long>(total), static_cast<unsigned long long>(vifQwords),
                          static_cast<unsigned long long>(regMismatch), static_cast<unsigned long long>(lateXgkickReads),
                          static_cast<unsigned long long>(realWrites), static_cast<unsigned long long>(silentWrites));
            out(buf);
            out("nearest dependency, weighted by task cycles:");
            for (const auto &h : histo)
            {
                std::snprintf(buf, sizeof(buf), "  %-22s %5.1f%%", h.first.c_str() + 2, 100.0 * h.second / std::max<uint64_t>(1, total));
                out(buf);
            }
            std::snprintf(buf, sizeof(buf), "critical path %llu cycles = %.1f%% of all work (upper bound on speed-up %.2fx)",
                          static_cast<unsigned long long>(critical), 100.0 * critical / std::max<uint64_t>(1, total),
                          static_cast<double>(total) / std::max<uint64_t>(1, critical));
            out(buf);
            for (uint32_t P : {1u, 2u, 3u, 4u, 6u, 8u})
            {
                std::vector<uint64_t> freeAt(P, 0), fin(n, 0);
                uint64_t makespan = 0;
                for (size_t k = 0; k < n; ++k)
                {
                    uint64_t ready = 0;
                    for (int64_t d : tasks[k].deps)
                        ready = std::max(ready, fin[static_cast<size_t>(d)]);
                    size_t w = 0;
                    for (size_t j = 1; j < P; ++j)
                        if (freeAt[j] < freeAt[w])
                            w = j;
                    const uint64_t start = std::max(ready, freeAt[w]);
                    fin[k] = start + tasks[k].cycles;
                    freeAt[w] = fin[k];
                    makespan = std::max(makespan, fin[k]);
                }
                std::snprintf(buf, sizeof(buf), "  %u thread(s): %.2fx", P, static_cast<double>(total) / std::max<uint64_t>(1, makespan));
                out(buf);
            }
            out("what ties a task to the one right before it (cycles of such tasks):");
            std::vector<std::pair<uint64_t, std::string>> causes;
            for (const auto &c : causeNear)
                causes.push_back({c.second, c.first});
            std::sort(causes.rbegin(), causes.rend());
            for (size_t k = 0; k < causes.size() && k < 20; ++k)
            {
                std::snprintf(buf, sizeof(buf), "  %-14s %5.1f%%", causes[k].second.c_str(), 100.0 * causes[k].first / std::max<uint64_t>(1, total));
                out(buf);
            }
            // per program
            struct Prog
            {
                uint64_t tasks = 0, cycles = 0, tied = 0;
            };
            std::map<uint32_t, Prog> progs;
            for (size_t k = 0; k < n; ++k)
            {
                Prog &pg = progs[tasks[k].pc];
                ++pg.tasks;
                pg.cycles += tasks[k].cycles;
                if (tasks[k].deps.count(static_cast<int64_t>(k) - 1))
                    pg.tied += tasks[k].cycles;
            }
            std::vector<std::pair<uint64_t, uint32_t>> order;
            for (const auto &pg : progs)
                order.push_back({pg.second.cycles, pg.first});
            std::sort(order.rbegin(), order.rend());
            out("programs by cycles (share of work, tasks, share of its cycles tied to the previous task):");
            for (size_t k = 0; k < order.size() && k < 16; ++k)
            {
                const Prog &pg = progs[order[k].second];
                std::snprintf(buf, sizeof(buf), "  pc 0x%04X  %5.1f%%  %7llu tasks  tied %5.1f%%", order[k].second,
                              100.0 * pg.cycles / std::max<uint64_t>(1, total), static_cast<unsigned long long>(pg.tasks),
                              100.0 * pg.tied / std::max<uint64_t>(1, pg.cycles));
                out(buf);
            }
            // examples of the items behind tied tasks, for the biggest program
            if (!order.empty())
            {
                const uint32_t pc0 = order[0].second;
                uint32_t shown = 0;
                for (size_t k = 1; k < n && shown < 6; ++k)
                    if (tasks[k].pc == pc0 && tasks[k].deps.count(static_cast<int64_t>(k) - 1))
                    {
                        std::string items;
                        for (int it : tasks[k].cause.at(static_cast<int64_t>(k) - 1))
                            items += " " + itemName(it);
                        std::snprintf(buf, sizeof(buf), "  example task %zu (pc 0x%04X after pc 0x%04X) reads from it:%s", k, pc0,
                                      tasks[k - 1].pc, items.c_str());
                        out(buf);
                        ++shown;
                    }
            }
            if (rep)
                std::fclose(rep);
            // every task: index, tick, start pc, cycles, then "dep:item item ..." groups (for offline analysis)
            if (FILE *tf = std::fopen((reportPath + ".tasks.csv").c_str(), "w"))
            {
                for (size_t k = 0; k < n; ++k)
                {
                    const Task &t = tasks[k];
                    std::fprintf(tf, "%zu,%llu,%u,%llu,%u", k, static_cast<unsigned long long>(t.tick), t.pc,
                                 static_cast<unsigned long long>(t.cycles), t.runs);
                    for (const auto &c : t.cause)
                    {
                        std::fprintf(tf, ",%lld:", static_cast<long long>(c.first));
                        for (size_t j = 0; j < c.second.size(); ++j)
                            std::fprintf(tf, "%s%s", j ? " " : "", c.second[j] >= kMem ? ("m" + std::to_string((c.second[j] - kMem) * 16)).c_str() : itemName(c.second[j]).c_str());
                    }
                    std::fprintf(tf, "\n");
                }
                std::fclose(tf);
            }
        }
    }

    void runBenchIfRequested()
    {
        const char *env = std::getenv("PS2X_VU_BENCH");
        if (!env || !*env)
            return;
        const BenchOptions opt = parseBench(env);
        std::vector<std::vector<uint8_t>> codes;
        std::vector<Record> records;
        std::vector<std::string> files;
        {
            size_t pos = 0;
            while (pos <= opt.capture.size())
            {
                size_t bar = opt.capture.find('|', pos);
                if (bar == std::string::npos)
                    bar = opt.capture.size();
                files.push_back(opt.capture.substr(pos, bar - pos));
                pos = bar + 1;
            }
        }
        for (const std::string &path : files)
        {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            std::cerr << "[vu:bench] cannot open " << path << std::endl;
            std::exit(2);
        }
        uint32_t lastCodeIndex[2] = {~0u, ~0u};
        for (;;)
        {
            Record r;
            if (std::fread(&r.hdr, sizeof(r.hdr), 1, f) != 1)
                break;
            if (r.hdr.magic != kMagic || r.hdr.stateSize != sizeof(VU1State))
            {
                std::cerr << "[vu:bench] bad record " << records.size() << std::endl;
                std::exit(2);
            }
            std::fread(&r.state, sizeof(VU1State), 1, f);
            if (r.hdr.hasCode)
            {
                std::vector<uint8_t> code(r.hdr.codeSize);
                std::fread(code.data(), 1, r.hdr.codeSize, f);
                uint32_t index = ~0u;
                for (uint32_t k = 0; k < codes.size() && index == ~0u; ++k)
                    if (codes[k] == code)
                        index = k; // same image seen before (another capture file): one entry, one generated image
                if (index == ~0u)
                {
                    codes.push_back(std::move(code));
                    index = static_cast<uint32_t>(codes.size() - 1u);
                }
                lastCodeIndex[r.hdr.unit] = index;
            }
            if (lastCodeIndex[r.hdr.unit] == ~0u)
            {
                std::cerr << "[vu:bench] record without code" << std::endl;
                std::exit(2);
            }
            r.codeIndex = lastCodeIndex[r.hdr.unit];
            r.data.resize(r.hdr.dataSize);
            if (std::fread(r.data.data(), 1, r.hdr.dataSize, f) != r.hdr.dataSize)
            {
                std::cerr << "[vu:bench] " << path << ": last record truncated (capture ended by a kill), dropped" << std::endl;
                break;
            }
            if (opt.unitFilter < 0 || opt.unitFilter == r.hdr.unit)
                records.push_back(std::move(r));
            if (records.size() >= opt.maxCalls)
                break;
        }
        std::fclose(f);
        }
        uint64_t perUnit[2] = {0, 0};
        for (const Record &r : records)
            ++perUnit[r.hdr.unit];
        const uint64_t ticks = records.empty() ? 0 : records.back().hdr.tick - records.front().hdr.tick + 1u;
        std::cerr << "[vu:bench] " << records.size() << " calls (VU0 " << perUnit[0] << ", VU1 " << perUnit[1] << ") over "
                  << ticks << " ticks, " << codes.size() << " code uploads" << std::endl;

        if (!opt.deps.empty())
        {
            deps::run(records, codes, opt.deps);
            std::exit(0);
        }

        if (!opt.aotgen.empty())
        {
            // One source per captured code image, entries = every program start seen with it.
            std::vector<std::set<uint32_t>> entrySets(codes.size());
            std::vector<int> unitOf(codes.size(), -1);
            for (const Record &r : records)
            {
                unitOf[r.codeIndex] = r.hdr.unit;
                entrySets[r.codeIndex].insert(r.hdr.resume ? r.state.pc : r.hdr.startPC);
            }
            std::vector<ps2_vu_aotgen::Source> used;
            for (size_t c = 0; c < codes.size(); ++c)
            {
                if (unitOf[c] < 0)
                    continue;
                ps2_vu_aotgen::Source src;
                src.vu0 = unitOf[c] == 0;
                src.code = codes[c];
                src.entries.assign(entrySets[c].begin(), entrySets[c].end());
                src.name = std::string(src.vu0 ? "vu0" : "vu1") + "_c" + std::to_string(c);
                used.push_back(std::move(src));
            }
            const bool ok = ps2_vu_aotgen::generate(used, opt.aotgen);
            std::cerr << "[vu:bench] aotgen " << (ok ? "wrote " : "FAILED ") << opt.aotgen << std::endl;
            std::exit(ok ? 0 : 1);
        }

        std::vector<uint64_t> sums(records.size(), 0), relaxedSums(records.size(), 0);
        std::vector<uint8_t> work;
        uint64_t packetHash = 0, packetBytes = 0;
        uint64_t cyclesTotal[2] = {0, 0};
        double best = 1e30;
        if (!opt.profileDir.empty())
            ps2ProfilerToggle(opt.profileDir.c_str()); // sample the replay passes
        for (uint32_t it = 0; it < opt.iterations; ++it)
        {
            // Fresh engines each pass: identical starting timing state, so checksums are reproducible.
            std::unique_ptr<VuFastEngine> engines[2] = {std::make_unique<VuFastEngine>(true), std::make_unique<VuFastEngine>(false)};
            for (auto &e : engines)
                e->setPacketSink([&](const uint8_t *d, uint32_t n)
                                {
                                    packetHash = hashBytes(packetHash, d, n);
                                    packetBytes += n;
                                });
            uint64_t unitCycles[2] = {0, 0};
            double engineSeconds = 0.0;
            for (size_t i = 0; i < records.size(); ++i)
            {
                const Record &r = records[i];
                VU1State st = r.state;
                work.assign(r.data.begin(), r.data.end());
                packetHash = 0x1234567u;
                VuFastEngine &e = *engines[r.hdr.unit];
                e.setCodeGeneration(static_cast<uint64_t>(r.codeIndex) + 1u);
                const std::vector<uint8_t> &code = codes[r.codeIndex];
                const auto t0 = std::chrono::steady_clock::now();
                if (r.hdr.resume)
                    e.resume(st, code.data(), r.hdr.codeSize, work.data(), r.hdr.dataSize, nullptr, nullptr, r.hdr.top, r.hdr.itop,
                             r.hdr.maxCycles);
                else
                    e.execute(st, code.data(), r.hdr.codeSize, work.data(), r.hdr.dataSize, nullptr, nullptr, r.hdr.startPC,
                              r.hdr.top, r.hdr.itop, r.hdr.maxCycles);
                engineSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                unitCycles[r.hdr.unit] = st.cycles; // engine clock: ends at the pass's total VU cycles (incl. stalls)
                if (it == 0)
                {
                    sums[i] = hashBytes(hashState(st) ^ packetHash, work.data(), work.size());
                    relaxedSums[i] = hashBytes(hashState(st, true) ^ packetHash, work.data(), work.size());
                }
            }
            if (it == 0)
            {
                cyclesTotal[0] = unitCycles[0];
                cyclesTotal[1] = unitCycles[1];
            }
            best = std::min(best, engineSeconds);
            std::cerr << "[vu:bench] pass " << it << ": " << engineSeconds * 1000.0 << " ms ("
                      << (ticks ? engineSeconds * 1000.0 / static_cast<double>(ticks) : 0.0) << " ms per captured tick)"
                      << std::endl;
        }
        if (!opt.profileDir.empty())
            ps2ProfilerToggle(opt.profileDir.c_str());
        std::cerr << "[vu:bench] VU cycles per pass: VU0 " << cyclesTotal[0] << ", VU1 " << cyclesTotal[1] << std::endl;
        std::cerr << "[vu:bench] best " << best * 1000.0 << " ms; per captured tick "
                  << (ticks ? best * 1000.0 / static_cast<double>(ticks) : 0.0) << " ms (60 fps budget 16.7 ms)" << std::endl;

        int rc = 0;
        const auto writeFile = [&](const std::string &path, const std::vector<uint64_t> &v)
        {
            if (path.empty())
                return;
            FILE *o = std::fopen(path.c_str(), "wb");
            if (o)
            {
                std::fwrite(v.data(), sizeof(uint64_t), v.size(), o);
                std::fclose(o);
                std::cerr << "[vu:bench] wrote " << v.size() << " checksums to " << path << std::endl;
            }
        };
        const auto checkFile = [&](const std::string &path, const std::vector<uint64_t> &v, const char *what)
        {
            if (path.empty())
                return;
            std::vector<uint64_t> ref(v.size(), 0);
            FILE *c = std::fopen(path.c_str(), "rb");
            size_t n = 0;
            if (c)
            {
                n = std::fread(ref.data(), sizeof(uint64_t), ref.size(), c);
                std::fclose(c);
            }
            size_t bad = 0;
            for (size_t i = 0; i < v.size(); ++i)
                if (i >= n || v[i] != ref[i])
                {
                    if (bad < 10)
                        std::cerr << "[vu:bench] " << what << " MISMATCH call " << i << " unit "
                                  << static_cast<int>(records[i].hdr.unit) << " pc=0x" << std::hex << records[i].hdr.startPC
                                  << std::dec << (records[i].hdr.resume ? " (MSCNT)" : "") << std::endl;
                    ++bad;
                }
            std::cerr << "[vu:bench] " << what << " check: " << (v.size() - bad) << "/" << v.size() << " calls identical"
                      << std::endl;
            if (bad != 0)
                rc = 1;
        };
        writeFile(opt.writeSums, sums);
        writeFile(opt.writeRelaxed, relaxedSums);
        checkFile(opt.checkSums, sums, "full");
        checkFile(opt.checkRelaxed, relaxedSums, "relaxed");
        std::cerr.flush();
        std::exit(rc);
    }
}
