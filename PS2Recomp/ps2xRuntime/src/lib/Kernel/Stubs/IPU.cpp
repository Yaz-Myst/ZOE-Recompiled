#include "../../ps2_savestate.h"
#include "Common.h"
#include "IPU.h"
#include "runtime/ee_scheduler.h"

namespace
{
    constexpr uint32_t REG_IPU_CTRL = 0x10002010u;
    constexpr uint32_t REG_IPU_CMD = 0x10002000u;
    constexpr uint32_t REG_IPU_IN_FIFO = 0x10007010u;
    constexpr uint32_t IQVAL_BASE = 0x1721e0u;
    constexpr uint32_t VQVAL_BASE = 0x172230u;
    constexpr uint32_t SETD4_CHCR_ENTRY = 0x126428u;

    void completeIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2Memory &mem = runtime->memory();
        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);

        __m128i v;
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x00u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x10u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x20u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x30u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, IQVAL_BASE + 0x40u);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);
        mem.write128(REG_IPU_IN_FIFO, v);

        mem.write32(REG_IPU_CMD, 0x50000000u);
        mem.write32(REG_IPU_CMD, 0x58000000u);

        v = runtime->Load128(rdram, ctx, VQVAL_BASE + 0x00u);
        mem.write128(REG_IPU_IN_FIFO, v);
        v = runtime->Load128(rdram, ctx, VQVAL_BASE + 0x10u);
        mem.write128(REG_IPU_IN_FIFO, v);

        mem.write32(REG_IPU_CMD, 0x60000000u);
        mem.write32(REG_IPU_CMD, 0x90000000u);
        mem.write32(REG_IPU_CTRL, 0x40000000u);
        mem.write32(REG_IPU_CMD, 0u);
        setReturnS32(ctx, 0);
    }
}

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!runtime)
            return;

        if (!runtime->memory().getRDRAM())
        {
            if (!runtime->memory().initialize())
            {
                setReturnS32(ctx, -1);
                return;
            }
        }

        if (!runtime->syncCoreSubsystems())
        {
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime->hasFunction(SETD4_CHCR_ENTRY))
        {
            EeScheduler &scheduler = runtime->eeScheduler();
            scheduler.bindMainContextForSyscall(*ctx, rdram);
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::HleCall;
            invocation.context = *ctx;
            invocation.context.pc = SETD4_CHCR_ENTRY;
            SET_GPR_U32(&invocation.context, 4, 1u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [rdram, runtime](const R5900Context &, R5900Context &parent)
            {
                completeIpuInit(rdram, &parent, runtime);
            };
            scheduler.invokeCurrent(std::move(invocation));
        }

        completeIpuInit(rdram, ctx, runtime);
    }

    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    namespace ipu_ring
    {
        namespace
        {
            struct Segment
            {
                uint32_t addr;
                uint32_t bytes;
                uint32_t nextTag;
            };

            std::mutex g_ringMutex;
            std::vector<Segment> g_segments;
            uint64_t g_ringBytes = 0u;
            uint64_t g_consumedBytes = 0u;

            constexpr uint32_t kTagRefe = 0u;
            constexpr uint32_t kTagCnt = 1u;
            constexpr uint32_t kTagNext = 2u;
            constexpr uint32_t kTagRef = 3u;
            constexpr uint32_t kTagRefs = 4u;
            constexpr uint32_t kTagEnd = 7u;
            constexpr size_t kMaxTags = 256u;
        }

        void noteInputChain(const uint8_t *rdram, uint32_t tadr)
        {
            std::vector<Segment> segments;
            std::unordered_set<uint32_t> visited;
            uint32_t tag = tadr & PS2_RAM_MASK & ~0xFu;
            while (segments.size() < kMaxTags && visited.insert(tag).second)
            {
                uint64_t raw = 0;
                std::memcpy(&raw, rdram + tag, sizeof(raw));
                const uint32_t qwc = static_cast<uint32_t>(raw & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((raw >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>(raw >> 32) & PS2_RAM_MASK;
                const uint32_t bytes = qwc * 16u;
                if (id == kTagRef || id == kTagRefs || id == kTagRefe)
                {
                    segments.push_back({addr, bytes, tag + 16u});
                    if (id == kTagRefe)
                        break;
                    tag += 16u;
                }
                else if (id == kTagCnt || id == kTagEnd)
                {
                    segments.push_back({tag + 16u, bytes, tag + 16u + bytes});
                    if (id == kTagEnd)
                        break;
                    tag += 16u + bytes;
                }
                else if (id == kTagNext)
                {
                    segments.push_back({tag + 16u, bytes, addr});
                    tag = addr;
                }
                else
                {
                    break; // CALL/RET are not used for bitstream rings.
                }
                tag &= PS2_RAM_MASK;
            }

            uint64_t total = 0;
            for (const Segment &segment : segments)
                total += segment.bytes;

            std::lock_guard<std::mutex> lock(g_ringMutex);
            g_segments = std::move(segments);
            g_ringBytes = total;
            g_consumedBytes = 0u;
        }

        void addConsumedBytes(uint64_t bytes)
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            g_consumedBytes += bytes;
        }

        // Current IPU_TO read position within the ring: MADR, remaining QWC in the current
        // segment, and the tag that follows it.
        bool currentPosition(uint32_t &madr, uint32_t &qwc, uint32_t &tadr)
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            if (g_ringBytes == 0u)
                return false;
            uint64_t offset = g_consumedBytes % g_ringBytes;
            for (const Segment &segment : g_segments)
            {
                if (offset < segment.bytes)
                {
                    const uint32_t within = static_cast<uint32_t>(offset) & ~0xFu;
                    madr = segment.addr + within;
                    qwc = (segment.bytes - within) / 16u;
                    tadr = segment.nextTag;
                    return true;
                }
                offset -= segment.bytes;
            }
            return false;
        }
    }

    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // sceIpuDmaEnv: d4madr, d4tadr, d4qwc, d4chcr, d3madr, d3qwc, d3chcr, ipubp, ipuctrl.
        const uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t madr = 0, qwc = 0, tadr = 0;
        if (envAddr != 0u && ipu_ring::currentPosition(madr, qwc, tadr))
        {
            if (uint8_t *env = getMemPtr(rdram, envAddr))
            {
                const uint32_t values[9] = {madr, tadr, qwc, 0x145u, 0u, 0u, 0u, 0u, 0u};
                std::memcpy(env, values, sizeof(values));
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}

namespace ps2_stubs::ipu_ring
{
    namespace
    {
        void saveIpuPart(ps2_state::Writer &w)
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            w.podVector(g_segments);
            w.u64(g_ringBytes);
            w.u64(g_consumedBytes);
        }
        void loadIpuPart(ps2_state::Reader &r)
        {
            std::lock_guard<std::mutex> lock(g_ringMutex);
            r.podVector(g_segments);
            g_ringBytes = r.u64();
            g_consumedBytes = r.u64();
        }
        const bool s_ipuPart = ps2_state::registerPart({"IPU", &saveIpuPart, &loadIpuPart, nullptr});
    }
}
