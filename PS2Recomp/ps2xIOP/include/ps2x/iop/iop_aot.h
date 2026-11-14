#pragma once

// Compiled IOP code (PS2X_IOP_AOT=1). A game registers blocks of R3000 code that tools/iop_aotgen.py translated to
// C++ from an image of IOP RAM. The interpreter's burst loop runs a block instead of interpreting it when the RAM
// words at its address still equal the words it was generated from, the whole block fits in the instruction budget
// and no service (SPU2 event, queued interrupt, guest callback) falls due inside it. A block does exactly what the
// interpreter would for the same instructions; anything it cannot do exactly (an exception, COP0, an import stub)
// it leaves to the interpreter by returning before that instruction.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ps2x::iop::aot
{
    inline constexpr uint32_t kRamSize = 2u * 1024u * 1024u;

    struct Ctx
    {
        uint32_t *r;    // the 32 general registers
        uint32_t *hi;
        uint32_t *lo;
        uint8_t *ram;   // IOP RAM
        uint8_t *owned; // one byte per RAM byte, set to 1 by every write (as IopMemory::markOwned)
        void *mem;
        uint32_t (*slowRead)(void *mem, uint32_t address, uint32_t size);
        void (*slowWrite)(void *mem, uint32_t address, uint32_t value, uint32_t size);
        // Written by the block before it returns: the interpreter state after its last executed instruction.
        uint32_t pc;
        uint32_t branchTarget;
        uint32_t loadReg;   // valid when anyLoad: the last delayed load the block scheduled
        uint32_t loadValue;
        bool branchPending;
        bool pendingLoad;   // that load has not reached its register yet
        bool anyLoad;
        bool io;            // the last executed instruction touched something that is not plain RAM
        // The emulator's cycle and instruction counters. A block leaves them alone except right before an access
        // that is not plain RAM: devices (the SPU2, timers) are clocked from the cycle counter, so it must then
        // already include the instructions the block has executed. counted = how many the block has added.
        uint64_t *cycles;
        uint64_t *instructions;
        uint32_t counted;
    };

    struct Block
    {
        uint32_t pc;           // physical address of the first instruction
        uint32_t count;        // instructions in the block
        const uint32_t *words; // the code it was generated from
        uint32_t (*run)(Ctx &c); // returns how many instructions it executed (0: none, interpret)
    };

    // Called once by the game's start-up code, before the IOP runs; the table must stay alive.
    void registerBlocks(const Block *blocks, size_t count) noexcept;

    // k: how many instructions of the block ran before the current one
    inline void sync(Ctx &c, uint32_t k)
    {
        c.io = true;
        *c.cycles += k - c.counted;
        *c.instructions += k - c.counted;
        c.counted = k;
    }

    inline uint32_t rd8(Ctx &c, uint32_t k, uint32_t a)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p < kRamSize)
            return c.ram[p];
        sync(c, k);
        return c.slowRead(c.mem, a, 1u);
    }
    inline uint32_t rd16(Ctx &c, uint32_t k, uint32_t a)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p + 1u < kRamSize)
        {
            uint16_t v;
            std::memcpy(&v, c.ram + p, 2u);
            return v;
        }
        sync(c, k);
        return c.slowRead(c.mem, a, 2u);
    }
    // a is word aligned (the caller tested or masked it)
    inline uint32_t rd32(Ctx &c, uint32_t k, uint32_t a)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p + 3u < kRamSize)
        {
            uint32_t v;
            std::memcpy(&v, c.ram + p, 4u);
            return v;
        }
        sync(c, k);
        return c.slowRead(c.mem, a, 4u);
    }
    inline void wr8(Ctx &c, uint32_t k, uint32_t a, uint32_t v)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p < kRamSize)
        {
            c.ram[p] = static_cast<uint8_t>(v);
            c.owned[p] = 1u;
            return;
        }
        sync(c, k);
        c.slowWrite(c.mem, a, v, 1u);
    }
    inline void wr16(Ctx &c, uint32_t k, uint32_t a, uint32_t v)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p + 1u < kRamSize)
        {
            const uint16_t h = static_cast<uint16_t>(v);
            std::memcpy(c.ram + p, &h, 2u);
            std::memset(c.owned + p, 1, 2u);
            return;
        }
        sync(c, k);
        c.slowWrite(c.mem, a, v, 2u);
    }
    // a is word aligned
    inline void wr32(Ctx &c, uint32_t k, uint32_t a, uint32_t v)
    {
        const uint32_t p = a & 0x1FFFFFFFu;
        if (p + 3u < kRamSize)
        {
            std::memcpy(c.ram + p, &v, 4u);
            std::memset(c.owned + p, 1, 4u);
            return;
        }
        sync(c, k);
        c.slowWrite(c.mem, a, v, 4u);
    }
}
