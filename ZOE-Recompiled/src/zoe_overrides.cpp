// Zone of the Enders (USA, SLUS_201.48) game-specific runtime hooks.
// Linked into the runner via PS2X_RUNNER_EXTRA_SOURCES (EXTRA_SOURCES in zoe.cfg); keeps game-specific code out of PS2Recomp.
//
// Stage stubs. Every stage loads a small code stub (<stage>.bin) into a heap buffer at a variable address:
// FUN_0043c8d8 (".bin" handler) stores the buffer start at *0x49DE50 and FUN_0043c890 (".scx" handler) does
// `jalr *0x49DE50` at 0x43C8B0. The stub is straight-line code that calls FUN_0043c8e8 once and then
// FUN_0043bb90(a0 = hash, a1 = main-ELF function) once per entry, the last one as a tail `j`. The recompiled image
// has no function at a heap address, so the runtime's code-resolver hook (ps2_code_resolver.h) hands that address to
// resolveZoeCode(), which returns zoeStageStub(): a native interpreter for exactly the whitelisted instruction forms
// below. See work/stages/NOTES.md for the analysis; tools/stage_stub_check.py is a Python model of the same
// whitelist and verifies all 22 bins (it also parses the k* constants below and fails if they drift).
#include "game_overrides.h"
#include "ps2_code_resolver.h"
#include "ps2_game_clock.h"
#include "ps2_host_settings.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
    // ---- Guest addresses (main ELF) ----
    constexpr uint32_t kStubPtrAddr = 0x0049DE50u; // u32: start of the loaded .bin buffer (stored by FUN_0043c8d8, loaded at 0x43C8AC)
    constexpr uint32_t kInitFn = 0x0043C8E8u;      // FUN_0043c8e8: registry.count = 0
    constexpr uint32_t kRegisterFn = 0x0043BB90u;  // FUN_0043bb90(a0 = hash, a1 = fn): appends {hash, fn} to the registry
    constexpr uint32_t kMaxStubWords = 0x1000u;    // scan limit (largest real stub is 894 words)

    // ---- Whitelist: the only instruction forms present in the 22 stage bins (plus `jr ra`, an alternative terminator) ----
    // Full-word forms.
    constexpr uint32_t kWordNop = 0x00000000u;         // sll zero,zero,0
    constexpr uint32_t kWordAddiuSpDown = 0x27BDFFF0u; // addiu sp,sp,-0x10
    constexpr uint32_t kWordAddiuSpUp = 0x27BD0010u;   // addiu sp,sp,0x10
    constexpr uint32_t kWordSdRa = 0xFFBF0000u;        // sd ra,0(sp)
    constexpr uint32_t kWordLdRa = 0xDFBF0000u;        // ld ra,0(sp)
    constexpr uint32_t kWordJalInit = 0x0C10F23Au;     // jal 0x43C8E8
    constexpr uint32_t kWordJalRegister = 0x0C10EEE4u; // jal 0x43BB90
    constexpr uint32_t kWordJRegister = 0x0810EEE4u;   // j   0x43BB90 (tail call)
    constexpr uint32_t kWordJrRa = 0x03E00008u;        // jr ra
    // Forms with a free 16-bit immediate: upper halfword fixed.
    constexpr uint32_t kHiLuiA0 = 0x3C04u;   // lui a0,imm
    constexpr uint32_t kHiLuiA1 = 0x3C05u;   // lui a1,imm
    constexpr uint32_t kHiAddiuA1 = 0x24A5u; // addiu a1,a1,imm
    constexpr uint32_t kHiOriA0 = 0x3484u;   // ori a0,a0,imm

    static_assert(kWordJalInit == (0x0C000000u | (kInitFn >> 2)), "jal kInitFn encoding");
    static_assert(kWordJalRegister == (0x0C000000u | (kRegisterFn >> 2)), "jal kRegisterFn encoding");
    static_assert(kWordJRegister == (0x08000000u | (kRegisterFn >> 2)), "j kRegisterFn encoding");

    enum class Op : uint8_t
    {
        Bad,
        Nop,
        AddiuSpDown,
        AddiuSpUp,
        SdRa,
        LdRa,
        LuiA0,
        LuiA1,
        AddiuA1,
        OriA0,
        JalInit,
        JalRegister,
        JRegister,
        JrRa,
    };

    Op decodeStubWord(uint32_t w)
    {
        switch (w)
        {
        case kWordNop: return Op::Nop;
        case kWordAddiuSpDown: return Op::AddiuSpDown;
        case kWordAddiuSpUp: return Op::AddiuSpUp;
        case kWordSdRa: return Op::SdRa;
        case kWordLdRa: return Op::LdRa;
        case kWordJalInit: return Op::JalInit;
        case kWordJalRegister: return Op::JalRegister;
        case kWordJRegister: return Op::JRegister;
        case kWordJrRa: return Op::JrRa;
        default: break;
        }
        switch (w >> 16)
        {
        case kHiLuiA0: return Op::LuiA0;
        case kHiLuiA1: return Op::LuiA1;
        case kHiAddiuA1: return Op::AddiuA1;
        case kHiOriA0: return Op::OriA0;
        default: break;
        }
        return Op::Bad;
    }

    // Loads a0/a1 (the arguments of FUN_0043bb90).
    bool isArgOp(Op op)
    {
        return op == Op::LuiA0 || op == Op::LuiA1 || op == Op::AddiuA1 || op == Op::OriA0;
    }

    bool readStubWord(const uint8_t *rdram, uint32_t addr, uint32_t &word)
    {
        if ((addr & 3u) != 0u || addr > PS2_RAM_SIZE - 4u)
            return false;
        std::memcpy(&word, rdram + addr, sizeof(word));
        return true;
    }

    uint32_t readStubPtr(const uint8_t *rdram)
    {
        uint32_t base = 0;
        std::memcpy(&base, rdram + kStubPtrAddr, sizeof(base));
        return base;
    }

    struct StubScan
    {
        uint32_t callbacks = 0; // registrations: `jal 0x43BB90` count, plus the tail `j 0x43BB90`
        uint32_t endPc = 0;     // first address past the terminator's delay slot
        uint32_t badPc = 0;
        uint32_t badWord = 0;
        const char *why = nullptr; // nullptr = accepted
    };

    // Grammar (identical in tools/stage_stub_check.py):
    //   word 0..3 : addiu sp,-0x10 ; sd ra,0(sp) ; jal 0x43C8E8 ; nop
    //   then      : { nop | lui a0/a1 | addiu a1,a1 | ori a0,a0 | jal 0x43BB90 + delay slot (nop or a0/a1 load) | ld ra,0(sp) }
    //   end       : (j 0x43BB90 | jr ra) + delay slot `addiu sp,sp,0x10`
    //   rules     : ld ra only once, after sd ra; no jal 0x43BB90 after ld ra; the terminator needs ld ra done.
    // Anything else (any other opcode, register, target or immediate form) is rejected.
    bool scanStub(const uint8_t *rdram, uint32_t base, StubScan &out)
    {
        out = StubScan{};
        bool raRestored = false;
        uint32_t pc = base;

        auto fail = [&out](uint32_t at, uint32_t word, const char *why)
        {
            out.badPc = at;
            out.badWord = word;
            out.why = why;
            return false;
        };

        for (uint32_t n = 0; n < kMaxStubWords; ++n, pc += 4u)
        {
            uint32_t w = 0;
            if (!readStubWord(rdram, pc, w))
                return fail(pc, 0u, "outside guest RAM or unaligned");
            const Op op = decodeStubWord(w);

            if (n == 0u && op != Op::AddiuSpDown)
                return fail(pc, w, "prologue word 0 is not addiu sp,sp,-0x10");
            if (n == 1u && op != Op::SdRa)
                return fail(pc, w, "prologue word 1 is not sd ra,0(sp)");
            if (n == 2u && op != Op::JalInit)
                return fail(pc, w, "prologue word 2 is not jal 0x43C8E8");

            switch (op)
            {
            case Op::Nop:
            case Op::LuiA0:
            case Op::LuiA1:
            case Op::AddiuA1:
            case Op::OriA0:
                break;
            case Op::AddiuSpDown:
                if (n != 0u)
                    return fail(pc, w, "addiu sp,sp,-0x10 outside the prologue");
                break;
            case Op::AddiuSpUp:
                return fail(pc, w, "addiu sp,sp,0x10 outside a terminator delay slot");
            case Op::SdRa:
                if (n != 1u)
                    return fail(pc, w, "sd ra outside the prologue");
                break;
            case Op::LdRa:
                if (n < 4u || raRestored)
                    return fail(pc, w, "unexpected ld ra,0(sp)");
                raRestored = true;
                break;
            case Op::JalInit:
            case Op::JalRegister:
            {
                if (op == Op::JalInit ? (n != 2u) : (n < 4u || raRestored))
                    return fail(pc, w, "unexpected call");
                uint32_t dw = 0;
                if (!readStubWord(rdram, pc + 4u, dw))
                    return fail(pc + 4u, 0u, "delay slot outside guest RAM");
                const Op dop = decodeStubWord(dw);
                if (op == Op::JalInit ? (dop != Op::Nop) : (dop != Op::Nop && !isArgOp(dop)))
                    return fail(pc + 4u, dw, "delay slot of a call is not a whitelisted a0/a1 load or nop");
                if (op == Op::JalRegister)
                    ++out.callbacks;
                ++n;
                pc += 4u; // the loop increment skips the delay slot
                break;
            }
            case Op::JRegister:
            case Op::JrRa:
            {
                if (n < 4u || !raRestored)
                    return fail(pc, w, "terminator before ld ra,0(sp)");
                uint32_t dw = 0;
                if (!readStubWord(rdram, pc + 4u, dw))
                    return fail(pc + 4u, 0u, "delay slot outside guest RAM");
                if (dw != kWordAddiuSpUp)
                    return fail(pc + 4u, dw, "terminator delay slot is not addiu sp,sp,0x10");
                if (op == Op::JRegister)
                    ++out.callbacks;
                out.endPc = pc + 8u;
                return true;
            }
            case Op::Bad:
            default:
                return fail(pc, w, "instruction not in the stage-stub whitelist");
            }
        }
        return fail(pc, 0u, "no terminator within the scan limit");
    }

    // The stub is entered at its start (`jalr *0x49DE50`), or resumed after the EE scheduler preempted a callee:
    // the callee then returns to ra (the address after a jal's delay slot) and the scheduler dispatches that PC.
    bool acceptStubAt(const uint8_t *rdram, uint32_t base, uint32_t address, StubScan &scan)
    {
        if (!scanStub(rdram, base, scan))
            return false;
        if (address == base)
            return true;
        uint32_t callWord = 0;
        const bool resumePoint = address >= base + 16u && address < scan.endPc && (address & 3u) == 0u &&
                                 readStubWord(rdram, address - 8u, callWord) &&
                                 (callWord == kWordJalInit || callWord == kWordJalRegister);
        if (!resumePoint)
        {
            uint32_t word = 0;
            readStubWord(rdram, address, word);
            scan.badPc = address;
            scan.badWord = word;
            scan.why = "address is neither the stub entry nor the return address of one of its calls";
            return false;
        }
        return true;
    }

    // Logged once per distinct failure (the runtime asks about an address twice: hasFunction, then lookupFunction).
    void logReject(uint32_t base, uint32_t address, const StubScan &scan)
    {
        static std::atomic<uint64_t> s_lastKey{0};
        const uint64_t key = (static_cast<uint64_t>(scan.badPc) << 32) ^ scan.badWord ^ (static_cast<uint64_t>(address) * 0x9E3779B1ull);
        if (s_lastKey.exchange(key) == key)
            return;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "[zoe] stage stub @0x%08x rejected (call target 0x%08x): %s: word 0x%08x at 0x%08x",
                      base, address, scan.why != nullptr ? scan.why : "?", scan.badWord, scan.badPc);
        std::cerr << line << std::endl;
    }

    // Executes one whitelisted non-control word exactly like the generated code for it (registers, memory, sign extension).
    // Parameter names are fixed: the READ64/WRITE64 macros refer to rdram, ctx and runtime.
    void execStubWord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Op op, uint32_t word)
    {
        switch (op)
        {
        case Op::AddiuSpDown:
            SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 4294967280u));
            break;
        case Op::AddiuSpUp:
            SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 16u));
            break;
        case Op::SdRa:
            WRITE64(ADD32(GPR_U32(ctx, 29), 0), GPR_U64(ctx, 31));
            break;
        case Op::LdRa:
            SET_GPR_U64(ctx, 31, READ64(ADD32(GPR_U32(ctx, 29), 0)));
            break;
        case Op::LuiA0:
            SET_GPR_S32(ctx, 4, (int32_t)((uint32_t)(word & 0xFFFFu) << 16));
            break;
        case Op::LuiA1:
            SET_GPR_S32(ctx, 5, (int32_t)((uint32_t)(word & 0xFFFFu) << 16));
            break;
        case Op::AddiuA1:
            SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 5), (uint32_t)(int32_t)(int16_t)(word & 0xFFFFu)));
            break;
        case Op::OriA0:
            SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | (uint64_t)(uint16_t)(word & 0xFFFFu));
            break;
        default: // Op::Nop
            break;
        }
    }

    // Defensive only: resolveZoeCode() has already accepted the whole stub, so this cannot trigger unless guest RAM
    // changed in between. Logs once, then reports through the runtime's missing-function path.
    void bailOut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t base, uint32_t entryPc,
                 uint32_t badPc, uint32_t badWord, const char *why)
    {
        StubScan scan;
        scan.badPc = badPc;
        scan.badWord = badWord;
        scan.why = why;
        logReject(base, entryPc, scan);
        runtime->reportMissingFunction(rdram, ctx, badPc, 0u, PS2Runtime::GuestBranchKind::IndirectCall, "zoe-stage-stub");
        // dispatchGuestBranch reads "ctx->pc still equals the entry" as a normal return; make the failure unwind instead.
        if (ctx->pc == entryPc)
            ctx->pc = 0u;
    }

    // Native stand-in for the stage stub. Called like any generated function: ctx->pc is the stub entry, or a resume
    // point (the return address of one of its calls) after a scheduler preemption; ra = return address into
    // FUN_0043c890 (0x43C8B8), sp = caller's stack.
    void zoeStageStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entryPc = ctx->pc;
        const uint32_t base = readStubPtr(rdram);
        StubScan scan;
        if (!acceptStubAt(rdram, base, entryPc, scan))
        {
            bailOut(rdram, ctx, runtime, base, entryPc, scan.badPc, scan.badWord, scan.why);
            return;
        }

        uint32_t pc = entryPc;
        for (;;)
        {
            uint32_t word = 0;
            const Op op = readStubWord(rdram, pc, word) ? decodeStubWord(word) : Op::Bad;
            ctx->pc = pc;

            switch (op)
            {
            case Op::Nop:
            case Op::AddiuSpDown:
            case Op::SdRa:
            case Op::LdRa:
            case Op::LuiA0:
            case Op::LuiA1:
            case Op::AddiuA1:
            case Op::OriA0:
                execStubWord(rdram, ctx, runtime, op, word);
                pc += 4u;
                continue;

            case Op::JalInit:
            case Op::JalRegister:
            {
                // Same sequence as the generated JAL: link, delay slot (in_delay_slot set), dispatch, resume after it.
                const uint32_t target = (op == Op::JalInit) ? kInitFn : kRegisterFn;
                uint32_t slotWord = 0;
                const Op slotOp = readStubWord(rdram, pc + 4u, slotWord) ? decodeStubWord(slotWord) : Op::Bad;
                if (slotOp != Op::Nop && !isArgOp(slotOp))
                {
                    bailOut(rdram, ctx, runtime, base, entryPc, pc + 4u, slotWord, "delay slot of a call is not whitelisted");
                    return;
                }
                SET_GPR_U32(ctx, 31, pc + 8u);
                ctx->pc = pc + 4u;
                ctx->in_delay_slot = true;
                ctx->branch_pc = pc;
                execStubWord(rdram, ctx, runtime, slotOp, slotWord);
                ctx->in_delay_slot = false;
                ctx->pc = target;
                if (!runtime->dispatchGuestBranch(rdram, ctx, target, pc, pc + 8u, PS2Runtime::GuestBranchKind::DirectCall, "JAL"))
                    return; // callee yielded / stop requested: ctx->pc is left where the scheduler resumes it
                pc += 8u;
                ctx->pc = pc;
                continue;
            }

            case Op::JRegister:
            case Op::JrRa:
            {
                uint32_t slotWord = 0;
                if (!readStubWord(rdram, pc + 4u, slotWord) || slotWord != kWordAddiuSpUp)
                {
                    bailOut(rdram, ctx, runtime, base, entryPc, pc + 4u, slotWord, "terminator delay slot is not whitelisted");
                    return;
                }
                const uint32_t jumpTarget = GPR_U32(ctx, 31); // jr ra reads ra before the delay slot, like generated code
                char line[96];
                std::snprintf(line, sizeof(line), "[zoe] stage stub @0x%08x registered %u callbacks", base, scan.callbacks);
                std::cout << line << std::endl;

                ctx->pc = pc + 4u;
                ctx->in_delay_slot = true;
                ctx->branch_pc = pc;
                execStubWord(rdram, ctx, runtime, Op::AddiuSpUp, slotWord); // sp back to the caller's value
                ctx->in_delay_slot = false;
                if (op == Op::JRegister)
                {
                    // Tail `j 0x43BB90`: generated code calls the target directly and returns; FUN_0043bb90's own
                    // `jr ra` then leaves ctx->pc = ra (0x43C8B8 in FUN_0043c890).
                    ctx->pc = kRegisterFn;
                    runtime->lookupFunction(kRegisterFn)(rdram, ctx, runtime);
                    return;
                }
                ctx->pc = jumpTarget;
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
                (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, pc, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
#endif
                return;
            }

            case Op::AddiuSpUp: // only legal in a terminator delay slot (handled above)
            case Op::Bad:
            default:
                bailOut(rdram, ctx, runtime, base, entryPc, pc, word, "instruction not in the stage-stub whitelist");
                return;
            }
        }
    }

    // Code-resolver hook: called when the address is in neither the function table nor an overlay.
    PS2Runtime::RecompiledFunction resolveZoeCode(uint8_t *rdram, uint32_t address, PS2Runtime *)
    {
        const uint32_t base = readStubPtr(rdram);
        if (base == 0u || (address & 3u) != 0u || address < base || (address - base) >= kMaxStubWords * 4u)
            return nullptr;

        StubScan scan;
        if (!acceptStubAt(rdram, base, address, scan))
        {
            logReject(base, address, scan); // then the runtime's normal missing-function report follows
            return nullptr;
        }
        return &zoeStageStub;
    }

    // isceSifSendCmd (0x48B718, the interrupt-context twin of sceSifSendCmd) is not a name the runtime knows, so the
    // recompiled SDK code would run: it builds a SIF DMA to the IOP command buffer the HLE'd sceSifInitCmd never
    // set up (destination 0). Route it to the runtime's sceSifSendCmd, which hands the packet to the IOP's sifcmd.
    // ZOE's own SIF layer (EE handler FUN_00466fd0 -> FUN_004674c0) replies to the IOP through it.
    constexpr uint32_t kIsceSifSendCmd = 0x0048B718u;

    void zoeIsceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = GPR_U32(ctx, 31);
        ps2_syscalls::sceSifSendCmd(rdram, ctx, runtime);
    }

    // DIAGNOSTIC (PS2X_ZOE_MOTION_LOG=1): FUN_004436b8 ("%d motions") turns the
    // offsets of a motion block into pointers (+= block base: table at +4, then +8 / +0xC of each 16-byte record of
    // every entry). Logs each call's block before it runs (warehouse slowdown before the 2nd Neith fight, 24 Oct).
    constexpr uint32_t kMotionRelocFn = 0x004436B8u;
    PS2Runtime::RecompiledFunction g_origMotionReloc = nullptr;

    void zoeMotionRelocLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto rd = [rdram](uint32_t a) {
            uint32_t v;
            std::memcpy(&v, rdram + (a & 0x1FFFFFCu), 4);
            return v;
        };
        const uint32_t base = GPR_U32(ctx, 4);
        const uint32_t count = rd(base);
        std::fprintf(stderr, "[zoe:motion] vblank=%llu base=0x%08x ra=0x%08x count=%d words:",
                     static_cast<unsigned long long>(ps2Perf().vblanks.load(std::memory_order_relaxed)), base,
                     GPR_U32(ctx, 31), static_cast<int32_t>(count));
        for (uint32_t i = 0; i < 8u; ++i)
            std::fprintf(stderr, " %08x", rd(base + 4u * i));
        std::fprintf(stderr, "\n");
        const uint32_t shown = std::min<uint32_t>(static_cast<int32_t>(count) > 0 ? count : 0u, 48u);
        for (uint32_t i = 0; i < shown; ++i)
        {
            const uint32_t off = rd(base + 4u + 4u * i);
            const uint32_t entry = off != 0u ? base + off : 0u; // off == 0: the game's beql reads its count from 0x4
            std::fprintf(stderr, "[zoe:motion]   #%u off=0x%08x records=%d first=%08x %08x\n", i, off,
                         static_cast<int32_t>(rd(entry + 4u)), rd(entry + 0x18u), rd(entry + 0x1Cu));
        }
        g_origMotionReloc(rdram, ctx, runtime);
    }

    // ---- 16:9 widescreen (Esc > Graphics > Widescreen 16:9; PS2X_ZOE_WIDESCREEN=1 starts on, =0 removes it) ----
    // CAMERA. ZOE's camera struct keeps a projection scale pair: +0x224 = 1.0 (horizontal) and +0x228 = 0.58333
    // (vertical), stored by the setup FUN_00450e50. FUN_00450c40 builds the camera's +0x80 / +0xC0 matrices from them
    // (x scale = 0x224 / 512, y scale = 0x228 / 224) and caches them; FUN_00450f58 (every frame a camera is used, once
    // per frame stamp +0x26C) composes the cached matrices with the view into +0x100 / +0x140. A 16:9 picture needs
    // the horizontal scale times (4/3)/(16/9) = 0.75; the window then stretches the 4:3 render
    // (ps2SetGameDisplayAspect).
    // The stored field is never changed permanently (a state saved without the switch behaves the same): the
    // consumers are wrapped, write 0.75 on entry and put 1.0 back when the original really finished (ctx->pc == the
    // ra seen on entry; a call interrupted by a scheduler yield leaves pc elsewhere and resumes later in the
    // original code, which reads the field again, so it must stay 0.75 until then: g_widePending, restored at the
    // next wrapper entry and only if the word still holds the 0.75 we wrote). FUN_00450f58 rebuilds the cached
    // matrices every frame stamp, because after a state load the cache still holds the 1.0 build and 0x450c40 is
    // not called again. Callers that tail-call 0x450c40 directly (FUN_00215568, FUN_002156a0) bypass the table: their
    // build stays 1.0 until the next frame stamp, i.e. at most one frame.
    // Only cameras that carry the stock pair are widened (the log, PS2X_ZOE_WIDESCREEN_LOG, prints every distinct
    // pair seen).
    constexpr uint32_t kCameraMatrixFn = 0x00450C40u;
    constexpr uint32_t kCameraViewFn = 0x00450F58u;
    constexpr uint32_t kViewResumePc = 0x00450F5Cu; // unused word slot inside FUN_00450f58: our continuation
    constexpr uint32_t kSizeFnA1 = 0x00459F40u;     // leaf: pixel size -> camera units, camera index in a1
    constexpr uint32_t kCamHScale = 0x224u;
    constexpr uint32_t kCamVScale = 0x228u;
    constexpr uint32_t kCamStamp = 0x26Cu;
    constexpr uint32_t kFrameCounterAddr = 0x004AA288u; // read at 0x450f74 (lw -0x5D78 from 0x4B0000)
    constexpr uint32_t kBitsOne = 0x3F800000u;          // 1.0f: ZOE's own value
    constexpr uint32_t kBitsWide = 0x3F400000u;         // 0.75f
    constexpr uint32_t kBitsStockV = 0x3F155555u;       // 0.58333f, ZOE's stock +0x228
    constexpr uint32_t kViewMagic = 0x45444957u;        // 'WIDE'
    PS2Runtime::RecompiledFunction g_origCameraMatrix = nullptr;
    PS2Runtime::RecompiledFunction g_origCameraView = nullptr;
    PS2Runtime::RecompiledFunction g_origSizeA1 = nullptr;
    uint32_t g_widePending = 0u; // camera whose field is still 0.75 after an interrupted original
    uint32_t g_wideCount[3] = {0u, 0u, 0u}; // 0x450c40 widened builds, 0x450f58 rebuilds, interrupted originals
    bool g_wideLog = false;
    // Live switch: ps2WidescreenActive() (the shared setting, offered in the menu, and the game's support flag). The
    // wrappers are installed once; each one starts with wideActiveNow(): off = the original is called directly,
    // exactly as stock. When the state flips to off, pending restores are flushed, and a camera whose cached matrices
    // were built wide is rebuilt once at stock scale at its next 0x450f58 call; the test uses guest data only (see
    // cacheBuiltWide), so it also covers a state saved with widescreen on and loaded with it off. Flipping on needs
    // nothing (the per-frame-stamp rebuild picks cameras up).
    bool g_prevActive = false;

    inline float wideBitsToF(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    inline uint32_t wideFToBits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    inline bool wideCamOk(uint32_t cam)
    {
        return cam >= 0x00100000u && cam < PS2_RAM_SIZE - 0x300u;
    }

    void wideNote(unsigned which, uint32_t cam)
    {
        const uint32_t n = ++g_wideCount[which];
        if (g_wideLog && (n & (n - 1u)) == 0u) // 1, 2, 4, 8, ...
        {
            static const char *const names[3] = {"0x450c40 build", "0x450f58 rebuild", "INTERRUPTED original"};
            std::printf("[zoe:wide] %s camera=0x%08x count=%u\n", names[which], cam, n);
        }
    }

    // Log each distinct (+0x224, +0x228) pair once, with whether the rule widens it.
    void wideLogFov(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        static uint32_t seen[16][2];
        static uint32_t seenN = 0u;
        if (!wideCamOk(cam))
            return;
        const uint32_t h = READ32(cam + kCamHScale), v = READ32(cam + kCamVScale);
        for (uint32_t i = 0; i < seenN; ++i)
            if (seen[i][0] == h && seen[i][1] == v)
                return;
        if (seenN < 16u)
        {
            seen[seenN][0] = h;
            seen[seenN][1] = v;
            ++seenN;
        }
        float hf, vf;
        std::memcpy(&hf, &h, 4);
        std::memcpy(&vf, &v, 4);
        std::printf("[zoe:wide] camera 0x%08x fov pair h=%g v=%g (%s)\n", cam, hf, vf,
                    (h == kBitsOne && v == kBitsStockV) ? "stock, widened" : "NOT stock, left alone");
    }

    // Put 1.0 back on the camera left at 0.75 by an interrupted original (it has had its chance to resume), but only
    // if the word still holds the 0.75 we wrote; anything else (a state load, a game write) is left alone.
    void widePendingRestore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_widePending == 0u)
            return;
        if (READ32(g_widePending + kCamHScale) == kBitsWide)
            WRITE32(g_widePending + kCamHScale, kBitsOne);
        g_widePending = 0u;
    }

    // Scale the camera's field if it carries ZOE's stock pair. Returns true when it wrote 0.75.
    bool wideBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        if (!wideCamOk(cam) || READ32(cam + kCamHScale) != kBitsOne || READ32(cam + kCamVScale) != kBitsStockV)
            return false;
        WRITE32(cam + kCamHScale, kBitsWide);
        return true;
    }

    void wideEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam, bool finished)
    {
        if (!finished)
        {
            g_widePending = cam; // the resumed original still needs 0.75
            wideNote(2, cam);
            return;
        }
        if (READ32(cam + kCamHScale) == kBitsWide)
            WRITE32(cam + kCamHScale, kBitsOne);
    }

    // The save-state loader has no public hook for game overrides, so a load is recognised by a jump of the game's
    // own frame counter (0x4AA288 goes back or leaps ahead): the host-side pending lists are then dropped (they
    // describe memory that has just been replaced).
    uint32_t g_lastFrame = 0u;
    bool g_lastFrameValid = false;
    void hudDropPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void wideCheckLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t frame = READ32(kFrameCounterAddr);
        if (g_lastFrameValid && (frame < g_lastFrame || frame - g_lastFrame > 600u))
        {
            g_widePending = 0u;
            hudDropPending(rdram, ctx, runtime);
        }
        g_lastFrame = frame;
        g_lastFrameValid = true;
    }

    void hudRestore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void clipRestoreOld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool all);

    bool wideActiveNow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Test only: PS2X_ZOE_WIDE_TOGGLE_AT=<tick>[,<tick>...] flips the setting when the window's vsync tick passes
        // each listed tick (the menu cannot be driven by scripts).
        static const char *const toggleEnv = std::getenv("PS2X_ZOE_WIDE_TOGGLE_AT");
        if (toggleEnv != nullptr)
        {
            static unsigned long long ticks[16];
            static uint32_t n = 0u, next = 0u;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                const char *s = toggleEnv;
                while (*s != '\0' && n < 16u)
                {
                    char *e = nullptr;
                    const unsigned long long v = std::strtoull(s, &e, 10);
                    if (e == s)
                        break;
                    ticks[n++] = v;
                    if (*e != ',')
                        break;
                    s = e + 1;
                }
            }
            if (next < n && ps2CurrentVsyncTick() >= ticks[next])
            {
                ++next;
                ps2SetWidescreenSession(!ps2WidescreenOn().load(std::memory_order_relaxed)); // test: never saved
            }
        }
        const bool active = ps2WidescreenActive();
        if (active != g_prevActive)
        {
            g_prevActive = active;
            if (!active)
            {
                // leave no 0.75 / zone-mapped value behind in game data
                widePendingRestore(rdram, ctx, runtime);
                hudRestore(rdram, ctx, runtime);
                clipRestoreOld(rdram, ctx, runtime, true); // before the drop: a load must not restore, a switch-off must
                hudDropPending(rdram, ctx, runtime);
            }
            std::printf("[zoe:wide] widescreen %s\n", active ? "ON" : "OFF");
        }
        return active;
    }

    void zoeCameraMatrixWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wideActiveNow(rdram, ctx, runtime))
        {
            g_origCameraMatrix(rdram, ctx, runtime);
            return;
        }
        wideCheckLoad(rdram, ctx, runtime);
        widePendingRestore(rdram, ctx, runtime);
        const uint32_t cam = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        if (g_wideLog)
            wideLogFov(rdram, ctx, runtime, cam);
        const bool scaled = wideBegin(rdram, ctx, runtime, cam);
        g_origCameraMatrix(rdram, ctx, runtime);
        if (scaled)
        {
            wideNote(0, cam);
            wideEnd(rdram, ctx, runtime, cam, ctx->pc == ra);
        }
    }

    // Second half of the 0x450f58 wrapper: runs the original 0x450f58 body for the camera after the widened 0x450c40
    // rebuild finished. The camera, the caller's ra and a magic word live in a 16-byte guest stack record (so a state
    // saved while the rebuild is interrupted still carries them); the record sits where the rebuild's own frame will
    // be built below it. This function is both called inline (rebuild finished at once) and reached through the
    // function-table slot kViewResumePc when an interrupted rebuild resumes later and "returns" there.
    void zoeViewResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        if (READ32(sp + 8u) != kViewMagic)
        {
            std::printf("[zoe:wide] ERROR: view continuation entered without its record (sp=%08x)\n", sp);
            ctx->pc = GPR_U32(ctx, 31); // nothing sensible to resume; leave through ra
            return;
        }
        const uint32_t cam = READ32(sp);
        const uint32_t realRa = READ32(sp + 4u);
        WRITE32(sp + 8u, 0u);
        SET_GPR_U32(ctx, 29, sp + 16u);
        wideEnd(rdram, ctx, runtime, cam, true);
        SET_GPR_U32(ctx, 4, cam);
        SET_GPR_U32(ctx, 31, realRa);
        ctx->pc = kCameraViewFn;
        g_origCameraView(rdram, ctx, runtime); // the real body, in order; returns to realRa
    }

    // Rebuild a camera's cached matrices with 0x450c40 (at whatever scale the field holds right now) and then run the
    // real 0x450f58 body, in order and safe against a scheduler yield (see zoeViewResume).
    void viewRebuild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        const uint32_t sp = GPR_U32(ctx, 29) - 16u;
        WRITE32(sp, cam);
        WRITE32(sp + 4u, GPR_U32(ctx, 31));
        WRITE32(sp + 8u, kViewMagic);
        SET_GPR_U32(ctx, 29, sp);
        SET_GPR_U32(ctx, 31, kViewResumePc); // the rebuild returns into zoeViewResume
        ctx->pc = kCameraMatrixFn;
        g_origCameraMatrix(rdram, ctx, runtime);
        if (ctx->pc == kViewResumePc) // finished at once
            zoeViewResume(rdram, ctx, runtime);
        // else: interrupted; the scheduler resumes it, it returns to kViewResumePc and zoeViewResume runs then
    }

    // True if the camera's cached +0x80 matrix was built with a horizontal scale 0.75 of what its stored field implies:
    // +0x80[0][0] is -(+0x224) * (+0x244) (-500 for the stock 1.0 / 500.0). Only the 0.75 fingerprint of our own
    // widened build counts, so a stock game quirk is never "corrected".
    bool cacheBuiltWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        if (!wideCamOk(cam) || READ32(cam + kCamHScale) != kBitsOne || READ32(cam + kCamVScale) != kBitsStockV)
            return false;
        const float cached = wideBitsToF(READ32(cam + 0x80u));
        const float expected = -wideBitsToF(READ32(cam + kCamHScale)) * wideBitsToF(READ32(cam + 0x244u));
        if (expected == 0.0f)
            return false;
        const float ratio = cached / expected;
        return ratio > 0.74f && ratio < 0.76f;
    }

    void zoeCameraViewWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool active = wideActiveNow(rdram, ctx, runtime);
        const uint32_t cam = GPR_U32(ctx, 4);
        if (!active)
        {
            // Stock, except one stock-scale rebuild of a camera whose cache was built wide (widescreen just turned off,
            // or a state saved with it on).
            if (cacheBuiltWide(rdram, ctx, runtime, cam))
            {
                wideNote(1, cam);
                viewRebuild(rdram, ctx, runtime, cam);
                return;
            }
            g_origCameraView(rdram, ctx, runtime);
            return;
        }
        wideCheckLoad(rdram, ctx, runtime);
        widePendingRestore(rdram, ctx, runtime);
        if (g_wideLog)
            wideLogFov(rdram, ctx, runtime, cam);
        // The original skips its body when the stamp equals the frame counter; the cache is then already this frame's.
        if (wideCamOk(cam) && READ32(cam + kCamStamp) != READ32(kFrameCounterAddr) &&
            wideBegin(rdram, ctx, runtime, cam))
        {
            wideNote(1, cam);
            viewRebuild(rdram, ctx, runtime, cam);
            return;
        }
        g_origCameraView(rdram, ctx, runtime);
    }

    // ---- HUD sprite objects (FUN_00459908) ----
    // FUN_00459908(a0 = 2D sprite object) is the draw routine for HUD sprites (called from the renderer FUN_0044f38c,
    // about 24 objects per frame: text pieces, gauges, reticle arcs, weapon name). Early on it loads the object's
    // 4x4 matrix into VU0 (FUN_0045a8a0 on a0 + 0x20 -> vf28..31); FUN_00459230 later builds the VIF packet from that.
    // Object layout (floats): +0x20 and +0x30 the matrix x/y/z axes (ordinary sprites (1,0,0), (0,1,0); the reticle
    // arcs are rotated), +0x50/+0x54 the position, +0x94/+0x98 the two packet buffers (byte +0x92 == 1 selects +0x98,
    // as FUN_00459d60 does) whose float at +0x50 is the sprite's width in HUD pixels and +0x54 its height. Measured
    // in destroy_flash2: the weapon bracket at x=428 has width 84 (ends at 512), the energy gauge frame at x=0 has
    // 184, SQUADS at 380 has 44. Objects are linked in draw order (+8 prev, +0xC next); there is no group id: a
    // string or gauge is several objects built by one HUD builder (FUN_0029e8xx), so grouping is by their centres.
    // Positions are HUD pixels (0..512 x 0..448) and 0x459230 adds 1791.5 / 1983.0 itself, unless object flag
    // +0x02 & 0x20 is set: then the position is already in GS coordinates (centre 2048), which projected things
    // (lock-on reticle arcs, the red enemy marker) use. The 3D objects take another route (FUN_004597f0).
    // On-screen messages (subtitles) are drawn by FUN_0045b668 (zoeTextObject, text objects), "-PAUSE-" through this routine
    // (one sprite with an offset rectangle). Menu panels (item / passcode box, mission result, map-screen menu) are container
    // objects (type 3, FUN_0045abe0) with child objects (FUN_0045a000 sprites, FUN_0045b6c8 text): zoeContainerHud below.
    // Rule (PS3 HD style; S = 0.75 undoes the final 4:3 -> 16:9 stretch), by the element's horizontal centre
    // c = x + width / 2:
    //   scene with a 3D view : c < zoneL anchors at 0, c >= zoneR at 512 (x' = anchor + (x - anchor) * S), the
    //                          centre part shrinks about its own centre; axis x components * S
    //   no 3D scene (boot, logos): every sprite shrinks about the screen centre (a pillarboxed 4:3 picture)
    //   width >= 90% of 512  : untouched (backdrops, fades, bars)
    //   flag 0x20 sprite     : GS coordinates, the (projected) position stays, axis x components * S
    // Every field written is saved with the value written and put back after the call only if it still holds that
    // value (objects are reused between frames). If the call is interrupted (scheduler yield) the original resumes
    // later and still needs the changed values; the restore then happens at the next wrapper entry.
    constexpr uint32_t kSpriteFn = 0x00459908u;
    constexpr uint32_t kSprite3dFn = 0x004597F0u;
    constexpr uint32_t kMarkerFn = 0x00280F08u;
    constexpr uint32_t kMarkerWidth = 0x1B0u;
    constexpr float kWideX = 0.75f;
    PS2Runtime::RecompiledFunction g_origSprite = nullptr;
    PS2Runtime::RecompiledFunction g_origMarker = nullptr;
    PS2Runtime::RecompiledFunction g_orig3d = nullptr;
    uint32_t g_hudLogSkip = 0u, g_hudLogCount = 0u, g_hudCalls = 0u;
    uint64_t g_hudTickLo = 1u, g_hudTickHi = 0u;
    uint32_t g_hudTickLines = 0u, g_hudTickMax = 400u;
    bool g_hudOn = true, g_hudMarker = true, g_have3dHook = false;
    bool g_gsExact = true; // PS2X_ZOE_WIDE_GSBLOCK=0: the old block rule (see hudBuildGsBlocks)
    bool g_gsFixed = true; // PS2X_ZOE_WIDE_GSFIXED=0: screen-fixed GS pieces never form blocks (see hudGsCandidate)
    float g_hudZoneL = 215.0f, g_hudZoneR = 297.0f, g_hudFullWidth = 460.8f;
    struct HudSaved
    {
        uint32_t addr;
        uint32_t written;
        uint32_t orig;
    };
    HudSaved g_hudSaved[8]; // a sprite writes 3 words, a window container 6
    // Vertex words written by hudClipBuffer. The packet the sprite routine builds only points at the vertex buffer (the
    // DMA reads it when the frame's list is sent), so these cannot be put back when the call returns: they are put back
    // (newest first, each only if it still holds what was written) once the game's frame counter has moved on, at the
    // next sprite-wrapper entry. A state load drops them (the memory was replaced); switching widescreen off restores
    // them all. Known limit: an F5 taken while a clip is applied stores the clipped words, and the list is not part of
    // the state (a new save-state part would make every existing state file unloadable: the part count is checked).
    // Harmless for buffers the game rebuilds every frame (the GAME OVER sweep); a static buffer whose object later moves
    // its clipped part into view would keep the clip.
    struct ClipSaved
    {
        uint32_t addr;
        uint32_t written;
        uint32_t orig;
        uint32_t frame;
    };
    constexpr uint32_t kClipSavedCap = 512u; // 8 vertices x 6 words per object; a few clipped objects per frame
    ClipSaved g_clipSaved[kClipSavedCap];
    uint32_t g_clipSavedN = 0u;
    uint32_t g_hudSavedN = 0u;
    uint32_t g_hudStat[6] = {0u, 0u, 0u, 0u, 0u, 0u}; // left, right, middle, GS-coordinate, markers, untouched
    uint64_t g_last3dVblank = 0u;

    // Projected markers. FUN_00280f08 (the pickup / target marker box) fills its sprite object through 0x459f40 (two
    // calls: a0 = a row block of the marker's own packet, then a0 + 0x30; return addresses 0x2810c4 and 0x2810e4).
    // The 2D object that draws that packet (identity axes, position (0,0)) owns two packet buffers (+0x94 / +0x98) that
    // alternate every frame, and a0 is a block of whichever one the builder got that frame (seen: object 0x5dfea0,
    // blocks 0x5dff50 and 0x5dffb0). An offset from a0 to the object only holds for one of the two buffers, which made
    // the marker flicker between the corrected and the zone-mapped position every other frame. So the match is by how
    // the object was built: it is a marker if o itself, or either of its packet buffers (+0x94, +0x98), is a block the
    // marker builder filled. Those objects keep their projected position (the HUD rule skips them). Tag rule, so a tag
    // cannot outlive its marker or flicker:
    //  - only calls whose ra is one of the two marker call sites tag (other 0x459f40 users do not);
    //  - a tag holds the block (a0, or a0 - 0x30 for the second call), the address a0 + 0x20 the helper just wrote and
    //    the value written; it only counts while that word still holds that value (a sprite that reuses the memory
    //    overwrites it);
    //  - a tag is NOT consumed: every draw of the object within 4 guest vblanks (ps2Perf().vblanks, the EE
    //    scheduler's counter) of the build matches, so a marker drawn twice, or a frame counter that does not step
    //    once per frame, cannot drop it; a load (frame-counter jump) drops all tags.
    constexpr uint32_t kMarkerRa1 = 0x002810C4u;
    constexpr uint32_t kMarkerRa2 = 0x002810E4u;
    struct HudTag
    {
        uint32_t obj;
        uint32_t row;
        uint32_t bits;
        uint64_t vblank;
    };
    constexpr uint32_t kTagCap = 128; // (object, row) keyed: ~2 entries per marker call, far above the markers on screen
    HudTag g_hudTags[kTagCap];
    uint32_t g_hudTagNext = 0u;
    uint32_t g_hudMarkerObjs = 0u;

    // Enemy target boxes (hudBoxPoolOf / zoeEnemyBoxTask below): one entry per unit task seen running.
    constexpr uint32_t kEnemyBoxFn = 0x00295660u;
    constexpr uint32_t kBoxPoolCap = 64u; // one per unit that has a target box; far above what a battle shows
    struct HudBoxPool
    {
        uint32_t base; // the unit's HUD task object (a0 of FUN_00295660)
        uint32_t ctxp; // its +0x10 (camera context) and +0x14 (unit id): unchanged while the same task lives there
        uint32_t unit;
        uint64_t cVb; // vblank the cached box centre belongs to
        float c;
        bool cOk;
        uint64_t runVb; // vblank of the task's last run: a full pool replaces the entry that ran longest ago
    };
    HudBoxPool g_boxPools[kBoxPoolCap];
    uint32_t g_boxPoolN = 0u, g_boxPoolNext = 0u;
    bool g_enemyBox = true; // PS2X_ZOE_WIDE_ENEMYBOX=0: enemy target boxes under the zone rule (the old behaviour)
    PS2Runtime::RecompiledFunction g_origEnemyBox = nullptr;

    // Put the saved words back, each only if it still holds exactly what we wrote.
    void hudRestore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        for (uint32_t i = 0; i < g_hudSavedN; ++i)
            if (READ32(g_hudSaved[i].addr) == g_hudSaved[i].written)
                WRITE32(g_hudSaved[i].addr, g_hudSaved[i].orig);
        g_hudSavedN = 0u;
    }

    void hudDropPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        g_hudSavedN = 0u;
        g_clipSavedN = 0u;
        for (uint32_t i = 0; i < kTagCap; ++i)
            g_hudTags[i].row = 0u;
        g_boxPoolN = 0u; // refilled by the next run of each unit task
        g_boxPoolNext = 0u;
    }

    void hudPut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, float value)
    {
        if (g_hudSavedN >= sizeof(g_hudSaved) / sizeof(g_hudSaved[0]))
            return;
        g_hudSaved[g_hudSavedN].addr = addr;
        g_hudSaved[g_hudSavedN].orig = READ32(addr);
        g_hudSaved[g_hudSavedN].written = wideFToBits(value);
        ++g_hudSavedN;
        WRITE32(addr, wideFToBits(value));
    }

    // Same as hudPut for a non-float word (a flags word): saved with the value written, put back by hudRestore only if it
    // still holds it.
    void hudPutWord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t bits)
    {
        if (g_hudSavedN >= sizeof(g_hudSaved) / sizeof(g_hudSaved[0]))
            return;
        g_hudSaved[g_hudSavedN].addr = addr;
        g_hudSaved[g_hudSavedN].orig = READ32(addr);
        g_hudSaved[g_hudSavedN].written = bits;
        ++g_hudSavedN;
        WRITE32(addr, bits);
    }

    void clipPut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, float value)
    {
        if (g_clipSavedN >= kClipSavedCap)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                std::printf("[zoe:wide] clip list full (%u words in one game frame): later clips are incomplete\n", kClipSavedCap);
            }
            return;
        }
        g_clipSaved[g_clipSavedN] = {addr, wideFToBits(value), READ32(addr), READ32(kFrameCounterAddr)};
        ++g_clipSavedN;
        WRITE32(addr, wideFToBits(value));
    }

    // Put back the clip writes of earlier game frames (all of them with all = true).
    void clipRestoreOld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool all)
    {
        const uint32_t frame = READ32(kFrameCounterAddr);
        // entries are appended in frame order (a load empties the list): oldest from this frame = nothing to restore yet
        if (!all && (g_clipSavedN == 0u || g_clipSaved[0].frame == frame))
            return;
        uint32_t keep = 0u;
        for (uint32_t i = g_clipSavedN; i-- > 0u;)
        {
            const ClipSaved &c = g_clipSaved[i];
            if (all || c.frame != frame)
            {
                if (READ32(c.addr) == c.written)
                    WRITE32(c.addr, c.orig);
            }
            else
                ++keep;
        }
        if (keep == 0u)
            g_clipSavedN = 0u;
        else if (keep != g_clipSavedN)
        {
            uint32_t n = 0u;
            for (uint32_t i = 0; i < g_clipSavedN; ++i)
                if (!all && g_clipSaved[i].frame == frame)
                    g_clipSaved[n++] = g_clipSaved[i];
            g_clipSavedN = n;
        }
    }

    // The packet buffer a 2D object is DRAWN from. FUN_00459230 passes the object to FUN_00459d80, which returns +0x98 when
    // byte +0x92 is 0 and +0x94 otherwise; the builder side (FUN_00459d60, "byte == 1 selects +0x98") uses the other
    // buffer of the pair. Objects whose byte is 0xFF (every subtitle, its box, the pause text) use +0x94 either way; for
    // the ones that flip between 0 and 1 the drawn buffer is the one not being rewritten. Extents and widths are read from
    // this buffer so they describe what the packet actually contains.
    uint32_t hudDrawBuf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s)
    {
        return (READ8(s + 0x92u) == 0u) ? READ32(s + 0x98u) : READ32(s + 0x94u);
    }

    // Width of a sprite in HUD pixels from its packet buffer, 0 if it does not look like one.
    float hudSpriteWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t o)
    {
        const uint32_t buf = hudDrawBuf(rdram, ctx, runtime, o);
        if (!wideCamOk(buf))
            return 0.0f;
        const float w = wideBitsToF(READ32(buf + 0x50u));
        return (w > 0.5f && w <= 1024.0f) ? w : 0.0f;
    }

    // Horizontal extent [lo, hi] of the vertices in a 2D object's packet buffer, in the object's local units. `s` is the
    // sprite-object base (the 0x459908 object itself, or the sub-object at +0x20 of a text object): the vertex count is
    // the word at s + 0x8C (FUN_00459230 clamps it to 0x30) and the vertices are 0x30-byte entries (s, t, q / rgba / x, y,
    // z) in the packet buffer chosen by byte s + 0x92 as for the width. A subtitle is one object whose vertices are two
    // corners per text line (destroy_flash2: 4 vertices, x -57.5..57.5 around the object position (255, 383)); the dark
    // box behind it is a 2-vertex sprite (x -59.5..59.5).
    bool hudObjExtent(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, float &lo, float &hi,
                      float *ylo = nullptr, float *yhi = nullptr, float *x0 = nullptr, uint32_t maxCnt = 48u)
    {
        const uint32_t cnt = READ32(s + 0x8Cu);
        const uint32_t buf = hudDrawBuf(rdram, ctx, runtime, s);
        if (cnt < 1u || cnt > maxCnt || !wideCamOk(buf) || buf + cnt * 0x30u + 0x30u > PS2_RAM_SIZE)
            return false;
        lo = 1e9f;
        hi = -1e9f;
        float yl = 1e9f, yh = -1e9f;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            const float x = wideBitsToF(READ32(buf + i * 0x30u + 0x20u));
            const float y = wideBitsToF(READ32(buf + i * 0x30u + 0x24u));
            if (!(std::fabs(x) < 1024.0f) || !(std::fabs(y) < 1024.0f))
                return false;
            lo = std::min(lo, x);
            hi = std::max(hi, x);
            yl = std::min(yl, y);
            yh = std::max(yh, y);
        }
        if (ylo != nullptr)
            *ylo = yl;
        if (yhi != nullptr)
            *yhi = yh;
        if (x0 != nullptr)
            *x0 = wideBitsToF(READ32(buf + 0x20u)); // x of the first vertex
        return hi > lo;
    }

    // The screen-centre-origin translucent overlay (flags 0x54, position (256, 224), vertices across the whole screen):
    // never part of a group, never moved. Recognised by its position AND its real extent: the ADA cockpit overlay pieces
    // also sit at (256, 224) (exactly for ~110 ticks after loading ws_item_cockpit, then 255.999 / 223.999) but span
    // 82 px (boxes x 119..201), and an exact-position test alone left them at 4:3 width until the position drifted.
    // An extent that cannot be read counts as the overlay (left alone, as before).
    bool hudIsScreenOverlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t o, float px, float py)
    {
        if (std::fabs(px - 256.0f) > 0.01f || std::fabs(py - 224.0f) > 0.01f)
            return false;
        float lo, hi;
        if (!hudObjExtent(rdram, ctx, runtime, o, lo, hi))
            return true;
        return (hi - lo) * std::fabs(wideBitsToF(READ32(o + 0x20u))) >= g_hudFullWidth;
    }

    // Pass-through wrapper of FUN_00459f40 that tags the marker's packet blocks (see the tag rule above).
    void zoeSizeTagWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origSizeA1(rdram, ctx, runtime);
        if ((ra == kMarkerRa1 || ra == kMarkerRa2) && wideCamOk(a0))
        {
            const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
            const uint32_t row = a0 + 0x20u;
            const uint32_t bits = READ32(row);
            for (uint32_t k = 0; k < 2u; ++k)
            {
                const uint32_t blk = (k == 0u) ? a0 : a0 - 0x30u; // the block: a0 itself, or a0 - 0x30 for the second call
                uint32_t slot = kTagCap;
                for (uint32_t i = 0; i < kTagCap && slot == kTagCap; ++i)
                    if (g_hudTags[i].obj == blk && g_hudTags[i].row == row)
                        slot = i; // same (object, row) again: refresh it
                if (slot == kTagCap)
                    slot = g_hudTagNext++ % kTagCap;
                HudTag &t = g_hudTags[slot];
                t.obj = blk;
                t.row = row;
                t.bits = bits;
                t.vblank = vb;
            }
        }
    }

    // True if o is the 2D object that draws a marker packet. Not consumed: valid for every draw of the object for 4 vblanks
    // after the marker was built (it is rebuilt every frame), as long as the tagged word still holds the value written.
    bool hudIsMarker(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t o)
    {
        const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        if (g_hudTagNext == 0u)
            return false; // no marker was ever built: nothing to scan
        const uint32_t buf94 = READ32(o + 0x94u);
        const uint32_t buf98 = READ32(o + 0x98u);
        const uint32_t used = std::min(g_hudTagNext, kTagCap); // slots are filled in order, the rest are empty
        for (uint32_t i = 0; i < used; ++i)
        {
            const HudTag &t = g_hudTags[i];
            if (t.row == 0u || vb < t.vblank || vb - t.vblank > 4u)
                continue;
            if ((t.obj == o || t.obj == buf94 || t.obj == buf98) && READ32(t.row) == t.bits)
                return true;
        }
        return false;
    }

    // ---- Enemy target boxes: mapped about their own centre ----
    // The green box round an enemy ("Lv.1"; "UNKNOWN" under it while the unit is not identified) is built by one task per
    // unit, FUN_00295660(a0 = the unit's HUD task object), called from 0x460480 once per vblank (PS2X_CALLARGS=295660,
    // ws_enemy_box: a0 = 0x72a0f0 and 0x72c520 every tick). The task owns its sprite objects inside itself: +0x50 top bar,
    // +0x100 bottom bar, +0x1B0 right edge, +0x260 left edge, +0x920 / +0xB60 / +0xDB0 the "Lv." glyphs, +0x1000 the name
    // (flags 0x14, position (0, 0); before this rule the log printed them as "offset-rectangle" objects). It fills their vertices with
    // ABSOLUTE HUD coordinates: the screen box of the unit's projected points (at least the point +- 16; FUN_00450fd0 +
    // FUN_0045a0f0 through the widened camera, so the position is already right for 16:9), widened by 60000 / (depth +
    // 500) (0x2959e0..0x295c48; the edges are f23 left, f22 right, f24 top, f28 bottom). The corner-HUD group rule does
    // not fit such a box: it picks left anchor (x' = 0.75 x) below a group centre
    // of 215 and right anchor (x' = 128 + 0.75 x) from 297 (PS2X_ZOE_WIDE_ZONES), so the moment the box centre c crosses
    // a zone edge the box jumps by 0.25 c (= 53.75 px, 10 % of the picture) away from its enemy, and back when it
    // crosses again. ws_enemy_box.p2s + the recorded stick taps: the group's mapped x offset went 55.82 -> 0 (c 223 -> left
    // zone) at tick 74145..74147 and 73.85 -> 128 (c 295 -> right zone) at 74221..74223; the drawn box centre stepped
    // -74 / -73 px and +58..+70 px in one frame against a smooth motion of at most 27 px per frame (stock and
    // PS2X_ZOE_WIDE_HUD=0 never more than 27.5). So each piece goes through x' = c + 0.75 (x - c) with c = the centre of the
    // box (the x extent of the four edge sprites, equal to (f22 + f23) / 2), whatever the zone: continuous in c, the
    // centre stays on the enemy. The same affine map for every piece keeps glyphs and name at the box's proportions. What
    // it does not undo (nor did the zone rule): the box half-width is the unit's projected half-width u (already narrowed
    // by the camera) plus a margin, and the whole box is narrowed once more, so each side sits 0.25 u closer to the enemy
    // than ideal: small for a distant unit, up to ~20 px per side for a close one (u ~ 80; inferred from the code, not
    // measured).
    // The task function is wrapped only to learn each task object (a pass-through); its sprites are then recognised by
    // address (task + one of the offsets), and stay recognised for as long as the task's +0x10 / +0x14 words are the ones
    // recorded (a paused game keeps drawing the last box, so entries do not expire by age; a full pool replaces the entry
    // whose task ran longest ago). The pieces are not group members (hudBuildGroups skips them).
    void zoeEnemyBoxTask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (wideActiveNow(rdram, ctx, runtime))
        {
            wideCheckLoad(rdram, ctx, runtime); // a load drops the pool before this run records into it
            const uint32_t task = GPR_U32(ctx, 4);
            if (wideCamOk(task))
            {
                const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
                uint32_t slot = kBoxPoolCap;
                for (uint32_t i = 0; i < g_boxPoolN && slot == kBoxPoolCap; ++i)
                    if (g_boxPools[i].base == task)
                        slot = i;
                if (slot == kBoxPoolCap && g_boxPoolN < kBoxPoolCap)
                    slot = g_boxPoolN++;
                else if (slot == kBoxPoolCap)
                {
                    slot = 0u;
                    for (uint32_t i = 1; i < kBoxPoolCap; ++i)
                        if (g_boxPools[i].runVb < g_boxPools[slot].runVb)
                            slot = i;
                }
                // a fresh record also drops the cached centre: the task is about to rebuild its pieces
                g_boxPools[slot] = HudBoxPool{task, READ32(task + 0x10u), READ16(task + 0x14u), ~0ull, 0.0f, false, vb};
            }
        }
        g_origEnemyBox(rdram, ctx, runtime);
    }

    // The pool entry of the task that owns sprite object o, or nullptr.
    HudBoxPool *hudBoxPoolOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t o)
    {
        for (uint32_t i = 0; i < g_boxPoolN; ++i)
        {
            HudBoxPool &p = g_boxPools[i];
            const uint32_t d = o - p.base; // wraps for an object below the task
            if (d > 0x1000u || (d != 0x50u && d != 0x100u && d != 0x1B0u && d != 0x260u && d != 0x920u && d != 0xB60u &&
                                d != 0xDB0u && d != 0x1000u))
                continue;
            if (READ32(p.base + 0x10u) == p.ctxp && READ16(p.base + 0x14u) == p.unit)
                return &p;
        }
        return nullptr;
    }

    // Centre (HUD x) of a task's box: the x extent of its four edge sprites, from their vertices (unrotated, identity
    // scale). Computed once per task run: the first piece drawn after the task rebuilt them asks, the others reuse it.
    bool hudBoxCentre(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, HudBoxPool &p, float &c)
    {
        const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        if (p.cVb != vb)
        {
            static const uint32_t kEdge[4] = {0x50u, 0x100u, 0x1B0u, 0x260u};
            p.cVb = vb;
            p.cOk = false;
            float lo = 1e9f, hi = -1e9f;
            bool ok = true;
            for (uint32_t k = 0; k < 4u && ok; ++k)
            {
                const uint32_t s = p.base + kEdge[k];
                const float ax = wideBitsToF(READ32(s + 0x20u)), px = wideBitsToF(READ32(s + 0x50u));
                float l, h;
                ok = ax > 0.5f && ax < 1.5f && hudObjExtent(rdram, ctx, runtime, s, l, h);
                if (ok)
                {
                    lo = std::min(lo, px + ax * l);
                    hi = std::max(hi, px + ax * h);
                }
            }
            if (ok && hi - lo > 1.0f && hi - lo < g_hudFullWidth)
            {
                p.c = 0.5f * (lo + hi);
                p.cOk = true;
            }
        }
        c = p.c;
        return p.cOk;
    }

    // Screens without a 3D scene (boot text, logos, title) have no HUD corners to anchor: the whole picture is 4:3
    // content, so every sprite shrinks around the screen centre (a pillarboxed 4:3 picture, centred text and logos stay
    // centred). "3D scene" = FUN_004597f0 (3D object draw) ran in the last 3 vblanks of the EE scheduler counter
    // (ps2Perf().vblanks, written on the guest side; the window loop's tick is not used).
    void zoeSprite3dMark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_last3dVblank = ps2Perf().vblanks.load(std::memory_order_relaxed);
        g_orig3d(rdram, ctx, runtime);
    }

    // ---- grouping: strings, digit rows and gauges move as one unit ----
    // The 2D objects form one doubly linked list in draw order (+8 prev, +0xC next, head 0x4A6920 in destroy_flash2) and
    // carry no group id: a number is one object per digit (e.g. x = 438, 447, 456, 465 at y 398, width 7), the energy
    // gauge is five objects (frame x 0 w 184, two bars x 40 w 105, x 187 w 12, x 202 w 7), a word one object. Group
    // = objects whose rectangles (x, y, width, height from the packet buffer: +0x50 width, +0xC4 height) overlap or
    // are within 6 HUD pixels of each other, found once per vblank by walking that list. Objects with flag
    // +0x02 (hidden, never drawn), flag 0x20 (GS coordinates), marker objects and anything without a sane width are
    // not part of it. One mapping per group, from the group's centre c:
    //   c < zoneL : x' = x * S;  c >= zoneR : x' = 512 - S * (512 - x);  otherwise x' = c + S * (x - c)
    // (the same affine map for every member, so the spacing inside a group is only ever scaled by S).
    // A group at least 90% of the screen wide is left alone.
    struct HudPlace
    {
        uint32_t obj;
        float nx;
        float scale; // multiplier of the axis x components (S, or the stretch of a spanning bar)
        bool keep;
        unsigned mode;
        float lo; // x of the object's rectangle relative to its position (0 for ordinary sprites, see hudBuildGroups)
    };
    // The renderer draws several 2D lists per frame (FUN_0044ee90 calls FUN_0044f348 from five sites, each with its own
    // circular list and sentinel at a0 + 0x20); a list's groups are built on the first object of it drawn in a vblank
    // and kept for that vblank. An object is looked up in every list built so far.
    constexpr uint32_t kPlaceCap = 192u; // 160 group members / GS blocks + up to kCentreCap centre-attached objects
    struct HudList
    {
        uint32_t n;
        HudPlace place[kPlaceCap];
    };
    constexpr uint32_t kMaxLists = 8;
    HudList g_lists[kMaxLists];
    uint32_t g_listN = 0u;
    uint64_t g_listVblank = ~0ull;
    // Every node of every 2D list walked this vblank (groupable or not), so an object that has no place in a list that was
    // already built costs one hash lookup instead of another list build. Stamped with the vblank it was filled in: an entry
    // of an older vblank counts as empty.
    constexpr uint32_t kMemberCap = 4096u;
    struct HudMember
    {
        uint32_t obj;
        uint64_t vb;
    };
    HudMember g_members[kMemberCap];
    uint32_t g_buildsThisVb = 0u, g_buildsMax = 0u, g_buildsOverflow = 0u;

    void hudMemberAdd(uint32_t o, uint64_t vb)
    {
        uint32_t h = (o >> 4) * 2654435761u >> 20;
        for (uint32_t k = 0; k < 32u; ++k)
        {
            HudMember &m = g_members[(h + k) & (kMemberCap - 1u)];
            if (m.vb != vb || m.obj == 0u)
            {
                m.obj = o;
                m.vb = vb;
                return;
            }
            if (m.obj == o)
                return;
        }
    }

    bool hudMemberHas(uint32_t o, uint64_t vb)
    {
        const uint32_t h = (o >> 4) * 2654435761u >> 20;
        for (uint32_t k = 0; k < 32u; ++k)
        {
            const HudMember &m = g_members[(h + k) & (kMemberCap - 1u)];
            if (m.vb != vb || m.obj == 0u)
                return false;
            if (m.obj == o)
                return true;
        }
        return false;
    }

    float hudMapGroupX(float x, float c, unsigned mode)
    {
        if (mode == 0u)
            return x * kWideX;
        if (mode == 1u)
            return 512.0f - kWideX * (512.0f - x);
        return c + kWideX * (x - c);
    }

    // Debug: PS2X_ZOE_WIDE_CLASSES=<mask> enables only some mapping classes (default all) to bisect artifacts:
    // 1 edge groups, 2 world-attached blocks and enemy target boxes (zoeEnemyBoxTask; clearing 2 leaves the boxes 4:3 wide,
    // PS2X_ZOE_WIDE_ENEMYBOX=0 puts them back under the zone rule), 4 rotated GS pieces (reticle arcs), 8 objects outside 0..512, 16 stretched
    // spanning bars, 32 origin-centred sprites (flag 0x8000), 64 text objects (FUN_0045b668), 128 window containers
    // (FUN_0045abe0, zoeContainerHud), 256 centre-attached mirror twins (cockpit overlay, hudPlaceCentre; clearing 256 stops
    // the twin placement, the placed objects are then drawn under class 1, so clearing 1 also removes them), 512 identity-parent
    // containers (ADA cockpit voice indicators, zoeContainerHud; PS2X_ZOE_WIDE_CLASSES=511 = the previous behaviour).
    unsigned g_classMask = 0xFFFFFFFFu;
    inline bool clsOn(unsigned bit)
    {
        return (g_classMask & bit) != 0u;
    }

    void mapLog(uint32_t o, unsigned cls, float nx, float scale)
    {
        if (!g_wideLog)
            return;
        const uint64_t tick = ps2CurrentVsyncTick();
        static uint32_t lines = 0u;
        if (tick >= g_hudTickLo && tick <= g_hudTickHi && lines < 600u)
        {
            ++lines;
            std::printf("[zoe:map] t=%llu obj=%08x class=%u nx=%g scale=%g\n", static_cast<unsigned long long>(tick), o, cls, nx,
                        scale);
        }
    }

    // World-attached blocks (flag 0x20, GS coordinates): the area panel "S04 | TOWN.2" with its icon row and digits, the
    // red enemy marker pieces. Each object has its own projected position (base point + offset), so the rows of one
    // panel differ in both x and width (label row x 1995.68 w 72, icon row x 2017.68 w 48 in ws_town2_panel); narrowing
    // every object about its own left edge shifts the rows against each other. Unrotated projected objects (axes
    // (1,0,0), (0,>0,0), depth z >= 1000, so not the screen-fixed reticle arcs) whose rectangles touch (x gap <= 6, y
    // distance <= 32) form one block, narrowed about the block's centre: x' = c + S * (x - c), axes * S. Rotated objects
    // (reticle arcs) keep the old rule. Unrotated screen-fixed objects (z < 1000) form blocks only among themselves, from
    // list neighbours of the same kind, two pieces or more (see hudGsCandidate); a lone one keeps the old rule.
    // hudGsCandidate is the one test for "can be part of a block": the wrapper asks it before looking a flag-0x20 object up
    // (pieces that can never form blocks, such as the w = 0 marker pieces and the arcs, would otherwise make the lookup
    // build lists again and again) and the block builder uses it to collect members.
    // Screen-fixed pieces (fixed = true): the same unrotated sprites, but the +0x58 word is no projected depth (z < 1000).
    // The Hub bomb timer is drawn next to the bomb while the bomb is on screen (projected, a normal block) and at the top
    // centre while it is not; there it is two screen-fixed pieces from one anchor (hub_bomb_timer_top.p2s, RAM at the save
    // tick: digits 0x640c90 at (2025, 1966) with 10 vertices, x 0..56, and the dot 0x641100 at (2055, 1973), x 0..2, both
    // flags 0x74, axes (1,0) (0,0.5), next to each other in the draw list: digits +0xC = the dot). As non-candidates each
    // piece got the arc rule (axes narrowed about its own position): the digits shrank towards their left edge while the
    // dot stayed, 7.5 HUD px right of its place between the second and third digit (the user's "27 80"). So screen-fixed
    // pieces also form blocks, but only with each other, only list neighbours with the same flags word and the same axes
    // (one element built in one go), and only blocks of two or more pieces are placed: a lone screen-fixed piece keeps
    // the arc rule exactly as before. The +0x58 word is no anchor test here: it is left over (0x2a290600 in the save,
    // 2.7e-34 at tick 19600, then exactly 0 from tick ~19640 at 3x), and the free-flight reticle ring and its two labels
    // (ws_town2_panel, flags 0x34 / 0x64) carry 0 as well.
    bool hudGsCandidate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t p, float &x, float &y, float &z,
                        float &w, bool *fixed = nullptr)
    {
        const float ax = wideBitsToF(READ32(p + 0x20u)), ay = wideBitsToF(READ32(p + 0x24u));
        const float bx = wideBitsToF(READ32(p + 0x30u)), by = wideBitsToF(READ32(p + 0x34u));
        x = wideBitsToF(READ32(p + 0x50u));
        y = wideBitsToF(READ32(p + 0x54u));
        z = wideBitsToF(READ32(p + 0x58u));
        w = hudSpriteWidth(rdram, ctx, runtime, p);
        if (!(w > 0.0f && w < g_hudFullWidth && ax > 0.5f && std::fabs(ay) < 0.001f && std::fabs(bx) < 0.001f && by > 0.0f &&
              x > 1000.0f && x < 3000.0f))
            return false;
        const bool fx = !(z >= 1000.0f); // a NaN word is screen-fixed too
        if (fixed != nullptr)
            *fixed = fx;
        return !fx || g_gsFixed;
    }

    void hudBuildGsBlocks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t head, HudList &L)
    {
        struct Gs
        {
            uint32_t obj;
            float x, y, z, w;
            float lo0, hi0; // x extent of the piece relative to x (its real vertices; 0..w when they cannot be read)
            int parent;
            float lo, hi; // x extent of the block rooted here (valid at roots)
            bool fixed;     // screen-fixed (see hudGsCandidate): merges only with a fixed list neighbour of the same kind
            unsigned flags; // the flags word (+2)
            uint32_t next;  // the next object in the draw list (+0xC)
            float ax, by;   // axis scales (+0x20, +0x34)
            uint32_t count; // pieces in the block rooted here (valid at roots)
        };
        static Gs g[96];
        uint32_t n = 0u;
        uint32_t p = head;
        for (uint32_t steps = 0; steps < 600u && wideCamOk(p) && n < 96u; ++steps)
        {
            const unsigned fl = READ16(p + 2u);
            if ((fl & 0x20u) && !(fl & 0x02u) && !hudIsMarker(rdram, ctx, runtime, p))
            {
                float x, y, z, w;
                bool fixed = false;
                if (hudGsCandidate(rdram, ctx, runtime, p, x, y, z, w, &fixed))
                {
                    float l0 = 0.0f, h0 = w, vl, vh;
                    if (g_gsExact && hudObjExtent(rdram, ctx, runtime, p, vl, vh))
                    {
                        const float ax = wideBitsToF(READ32(p + 0x20u)); // > 0.5, checked by hudGsCandidate
                        l0 = ax * vl;
                        h0 = ax * vh;
                    }
                    g[n] = Gs{p, x, y, z, w, l0, h0, static_cast<int>(n), x + l0, x + h0, fixed, fl, READ32(p + 0xCu),
                              wideBitsToF(READ32(p + 0x20u)), wideBitsToF(READ32(p + 0x34u)), 1u};
                    ++n;
                }
            }
            const uint32_t next = READ32(p + 0xCu);
            if (next == head)
                break;
            p = next;
        }
        auto find = [&](int i)
        {
            while (g[i].parent != i)
            {
                g[i].parent = g[g[i].parent].parent;
                i = g[i].parent;
            }
            return i;
        };
        // Projected pieces merge when their rectangles touch AND they sit at the same depth (screen-fixed pieces: list
        // neighbours of the same kind instead, see hudGsCandidate). The depth is the projected z of the
        // piece's anchor: pieces the game places from one anchor (a panel's rows, the glyphs of a "Lv. 3" label, the three
        // pieces of a lock-on bracket) carry the SAME float. Measured on battle1_t4740 (661 frames, one lock-on cursor,
        // 3966 touching pairs): 699 pairs with |dz| = 0, none below 1e-6 z, the other 3267 spread over a continuum
        // (1e-6..1e-5 z: 85, to 1e-4: 642, to 1e-3: 828, to 5e-3: 856, to 5e-2: 758, beyond: 98): pairs of DIFFERENT
        // builders (arc, label, bracket) whose depths drift past each other. The old tolerance of 0.5 % z cut through that
        // continuum, so a block flipped between "label + arc + bracket" and "label alone" (4 partitions, 10 changes in 661
        // frames, the label sliding up to 2.5 px against the ring from one frame to the next; with several enemies the chains
        // reach 200 px). So only the same anchor merges: |dz| <= 1e-6 z, below every measured pair of different builders
        // (same-anchor pairs are exactly equal). ws_town2_panel: all 6249 touching pairs are either identical (3063) or
        // >= 5 % apart, so panels do not change. A merged block may not grow wider than 200 HUD px.
        // The rectangle of a piece is its real vertex extent (hudObjExtent): the arc top piece and the bracket base are
        // centred on their position (x -28..28, -12..12 in the battle1 RAM dump) while the old [x, x + w] put their centre
        // w/2 to the right, which moved them by 0.125 w (3.5 and 1.5 HUD px) whenever they were alone in a block.
        const float zAbs = g_gsExact ? 0.0f : 0.5f, zRel = g_gsExact ? 1.0e-6f : 0.005f;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j)
            {
                const bool xo =
                    !(g[i].x + g[i].hi0 + 6.0f < g[j].x + g[j].lo0 || g[j].x + g[j].hi0 + 6.0f < g[i].x + g[i].lo0);
                // fixed pieces: one element = list neighbours of the same kind (their +0x58 word is no anchor, see hudGsCandidate)
                const bool zo = (g[i].fixed || g[j].fixed)
                                    ? (g[i].fixed && g[j].fixed && g[i].flags == g[j].flags &&
                                       std::fabs(g[i].ax - g[j].ax) < 0.001f && std::fabs(g[i].by - g[j].by) < 0.001f &&
                                       (g[i].next == g[j].obj || g[j].next == g[i].obj))
                                    : std::fabs(g[i].z - g[j].z) <= zAbs + zRel * std::fabs(g[i].z);
                if (!(xo && zo && std::fabs(g[i].y - g[j].y) <= 32.0f))
                    continue;
                const int ri = find(static_cast<int>(i)), rj = find(static_cast<int>(j));
                if (ri == rj)
                    continue;
                const float lo = std::min(g[ri].lo, g[rj].lo), hi = std::max(g[ri].hi, g[rj].hi);
                if (hi - lo > 200.0f)
                    continue;
                g[rj].parent = ri;
                g[ri].lo = lo;
                g[ri].hi = hi;
                g[ri].count += g[rj].count;
            }
        for (uint32_t i = 0; i < n && L.n < kPlaceCap; ++i)
        {
            const int root = find(static_cast<int>(i));
            if (g[i].fixed && g[root].count < 2u)
                continue; // a lone screen-fixed piece: no place, the wrapper keeps the arc rule
            if (g_wideLog && g[i].fixed)
            {
                static uint32_t seenF[16];
                static uint32_t seenFN = 0u;
                bool seen = false;
                for (uint32_t s = 0; s < seenFN && !seen; ++s)
                    seen = (seenF[s] == g[i].obj);
                if (!seen && seenFN < 16u)
                {
                    seenF[seenFN++] = g[i].obj;
                    std::printf("[zoe:hud] screen-fixed GS block: object 0x%08x at hud x %g (GS y %g), block hud x %g..%g (%u pieces)\n",
                                g[i].obj, g[i].x - 1791.5f, g[i].y, g[root].lo - 1791.5f, g[root].hi - 1791.5f, g[root].count);
                }
            }
            const float c = 0.5f * (g[root].lo + g[root].hi);
            HudPlace &pl = L.place[L.n++];
            pl.obj = g[i].obj;
            pl.keep = false;
            pl.scale = kWideX;
            pl.lo = 0.0f;
            pl.mode = 3u;
            pl.nx = c + kWideX * (g[i].x - c);
        }
    }

    // Centre-attached objects (class 256). The ADA cockpit overlay (two trapezoid boxes, two large arcs, tick marks, bars) is
    // laid out about the screen centre from pairs of objects with mirrored origins: boxes at (256, 224), arcs at x 200 and
    // 312, ticks at 190 and 322, the same y and axes, vertex lists mirrored (ws_item_cockpit, tick 63900, RAM dump). All of it
    // belongs to the camera-squeezed 3D cockpit (console, scenery), so it takes the centre map x' = 256 + S (x - 256) with
    // the axes * S and never joins a group. The group rule treated the twins unequally: hudSpriteWidth is the x of the
    // SECOND vertex, so a twin whose vertices lie left of its origin has "width" 0, is no group member and was mapped alone
    // by the zone of its origin (left arc and tick: x * S, left-anchored), while the right twins (width > 0) were chained
    // through the two 396 px sprites 0x7f0380 / 0x7f04f0 into one 604 px group that the "at least 90 % wide" rule leaves at
    // 4:3 (right box and right arc: unsqueezed, 1.33x too wide in the 16:9 window).
    // Twin = another list object (hidden ones count, so a pair does not split while one half is hidden) with the origin
    // mirrored about 256 (x + x' = 512 within 1), the same y within 1 and the same axis scales. Only origins within
    // kCentreReach of x 256 qualify, so corner pairs (origins near the edges) are never touched; objects the plain rule
    // leaves alone (width >= 90 % of the screen, the (256, 224) overlay) are neither candidates nor twins.
    // The overlay sways when the cockpit turns (user report 11 Oct, ws_cockpit_turn.p2s + route_cockpit_turn.txt): the
    // boxes' shared origin moved to x 252.45 and the console bars' to 251.56, so "x + x' = 512" failed for pieces that
    // share one origin, and they fell back to the group rule: the right box right-anchored (x 317.3 instead of ~253.3),
    // the left box about its own origin, the two 396 px console bars stretched as a spanning bar (scale 1.07): the side
    // boxes split apart while turning. Two more shapes therefore count as centre-attached (origin still within
    // kCentreReach of 256): a piece that shares its origin (x within 0.01, y within 1) and axes with another piece lying
    // on the other side of that origin (one vertex x extent at or left of it, the other at or right of it, within 1:
    // the left and right box drawn from one origin; not required to be exact mirror images, the boxes' shapes animate
    // while turning), and a piece whose own extent is symmetric about its origin (lo = -hi within 1, at least 8 px
    // wide: the console bars, x -198..198). Both keep the centre map wherever the sway puts them.
    // Appends one place per centre-attached visible object to L (mode 2) and returns how many; `rects` receives the
    // rectangle of each one whose extent was read (rectN of them, for hudBuildGroups' groups lying on them).
    constexpr float kCentreReach = 80.0f;
    constexpr uint32_t kCentreCap = 32u;
    struct HudRect
    {
        uint32_t obj;
        float x0, x1, y0, y1; // HUD x (axis x applied) and y (local y unscaled, as hudBuildGroups measures its nodes)
    };

    // A group lying on a centre-attached piece moves with it. The Hub bomb's DISARMING display (hub_bomb_disarm.p2s +
    // route_bomb_hold.txt, RAM dump at tick 19291): the word "DISARMING" 0x640b10 at (256, 352), vertices -96..96, is
    // centre-attached (symmetric about its own origin) and so is no group member; the bright fill under it 0x6409a0 is a
    // sprite at (156, 350) whose width runs down from 199 to 0 with the countdown, and it touches the countdown digits
    // 0x640540 (240, 330, x 0..36) and their dot 0x640830 (250, 345). That group's centre therefore shrank with the fill:
    // its centre map drifted every frame (fill at x 181 -> 172.7 in 100 ticks, the digits with it), and at tick 19303 the
    // centre crossed x 215, the group switched to the left-edge map and fill + digits jumped ~54 px left, off the word
    // (the digits came back at tick 19364 when the fill no longer reached them; the fill stayed off). Kind B (wrong
    // classification flipping between frames), from a group centre that depends on a member's changing width. Rule: a
    // group whose rectangle lies inside a centre-attached piece's x extent (6 px slack) and touches it vertically (6 px)
    // takes the centre map, like the piece. Only word- or strip-sized pieces anchor (narrower than half the screen, under
    // 48 local y units: "DISARMING" is 192 x 16): the ADA cockpit's console bars (x -198..198, y -116..116 about ~256) would
    // otherwise cover the whole lower middle of the screen and could capture a corner HUD piece such as the one at
    // x 316..368, y 398 (review, 29 Oct; not seen together on screen). PS2X_ZOE_WIDE_CENTREGROUP=0: old rule (the group's
    // own centre and zone).
    constexpr float kAnchorMaxW = 256.0f, kAnchorMaxH = 48.0f;
    bool g_hudCentreGroups = true;

    uint32_t hudPlaceCentre(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t head, HudList &L, HudRect *rects,
                            uint32_t &rectN)
    {
        struct Cand
        {
            uint32_t obj;
            float x, y, ax, by;
            bool visible;
            bool ext;     // vertex x extent read (hudObjExtent: at most 48 vertices)
            float lo, hi; // that extent, relative to the origin
            float ylo, yhi; // vertex y extent (local, unscaled)
        };
        static Cand cand[256];
        uint32_t n = 0u;
        uint32_t p = head;
        for (uint32_t steps = 0; steps < 600u && wideCamOk(p) && n < 256u; ++steps)
        {
            const unsigned fl = READ16(p + 2u);
            const uint32_t cnt = READ32(p + 0x8Cu);
            if (!(fl & 0x20u) && !(fl & 0x8000u) && cnt >= 2u && cnt <= 96u && wideCamOk(hudDrawBuf(rdram, ctx, runtime, p)))
            {
                const float x = wideBitsToF(READ32(p + 0x50u)), y = wideBitsToF(READ32(p + 0x54u));
                if (!(hudSpriteWidth(rdram, ctx, runtime, p) >= g_hudFullWidth || hudIsScreenOverlay(rdram, ctx, runtime, p, x, y)))
                {
                    float lo = 0.0f, hi = 0.0f, ylo = 0.0f, yhi = 0.0f;
                    const bool ext = hudObjExtent(rdram, ctx, runtime, p, lo, hi, &ylo, &yhi);
                    cand[n++] = Cand{p, x, y, wideBitsToF(READ32(p + 0x20u)), wideBitsToF(READ32(p + 0x34u)), !(fl & 0x02u), ext,
                                     lo, hi, ylo, yhi};
                }
            }
            const uint32_t next = READ32(p + 0xCu);
            if (next == head)
                break;
            p = next;
        }
        uint32_t added = 0u;
        for (uint32_t i = 0; i < n && added < kCentreCap; ++i)
        {
            const Cand &a = cand[i];
            if (!a.visible || std::fabs(a.x - 256.0f) > kCentreReach || hudIsMarker(rdram, ctx, runtime, a.obj))
                continue;
            uint32_t twin = 0u;
            for (uint32_t k = 0; k < n && twin == 0u; ++k)
            {
                const Cand &b = cand[k];
                if (k == i || std::fabs(a.y - b.y) > 1.0f || std::fabs(a.ax - b.ax) >= 0.01f || std::fabs(a.by - b.by) >= 0.01f)
                    continue;
                if (std::fabs(a.x + b.x - 512.0f) <= 1.0f) // origins mirrored about the screen centre
                    twin = b.obj;
                else if (std::fabs(a.x - b.x) <= 0.01f && a.ext && b.ext && a.hi - a.lo >= 2.0f && b.hi - b.lo >= 2.0f &&
                         ((a.hi <= 1.0f && b.lo >= -1.0f) || (a.lo >= -1.0f && b.hi <= 1.0f))) // one origin, opposite sides
                    twin = b.obj;
            }
            if (twin == 0u && a.ext && a.hi - a.lo >= 8.0f && std::fabs(a.lo + a.hi) <= 1.0f)
                twin = a.obj; // symmetric about its own origin
            if (twin == 0u)
                continue;
            HudPlace &pl = L.place[L.n++];
            pl.obj = a.obj;
            pl.keep = false;
            pl.scale = kWideX;
            pl.lo = 0.0f;
            pl.mode = 2u;
            pl.nx = hudMapGroupX(a.x, 256.0f, 2u);
            ++added;
            const float e0 = a.x + a.ax * a.lo, e1 = a.x + a.ax * a.hi;
            if (a.ext && std::fabs(e1 - e0) < kAnchorMaxW && a.yhi - a.ylo < kAnchorMaxH)
                rects[rectN++] = HudRect{a.obj, std::min(e0, e1), std::max(e0, e1), a.y + a.ylo, a.y + a.yhi};
            if (g_wideLog)
            {
                static uint32_t seenC[32];
                static uint32_t seenCN = 0u;
                bool seen = false;
                for (uint32_t s = 0; s < seenCN && !seen; ++s)
                    seen = (seenC[s] == a.obj);
                if (!seen && seenCN < 32u)
                {
                    seenC[seenCN++] = a.obj;
                    std::printf("[zoe:hud] centre-attached object 0x%08x: pos (%g,%g), twin 0x%08x -> x %g\n", a.obj, a.x, a.y, twin,
                                pl.nx);
                }
            }
        }
        return added;
    }

    void hudBuildGroups(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t start)
    {
        struct Node
        {
            uint32_t obj;
            float x, y, w, h;
            int parent;
            float lo; // the rectangle starts lo to the right of the object position (x is the rectangle's left edge)
        };
        static Node nodes[160];
        uint32_t n = 0u;
        HudList &L = g_lists[g_listN++]; // hudPlaceFor guarantees a free slot
        L.n = 0u;
        const uint64_t buildVb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        ++g_buildsThisVb;
        // back to the head of the list (bounded: a corrupt or circular list just ends the walk)
        uint32_t head = start;
        for (uint32_t i = 0; i < 600u; ++i)
        {
            const uint32_t prev = READ32(head + 8u);
            if (!wideCamOk(prev) || prev == start)
                break;
            head = prev;
        }
        // centre-attached mirror twins (cockpit overlay) are placed first, in L.place[0 .. centreN), and are not group members
        static HudRect centreRects[kCentreCap];
        uint32_t centreRectN = 0u;
        const uint32_t centreN = clsOn(256) ? hudPlaceCentre(rdram, ctx, runtime, head, L, centreRects, centreRectN) : 0u;
        auto isCentre = [&](uint32_t o)
        {
            for (uint32_t k = 0; k < centreN; ++k)
                if (L.place[k].obj == o)
                    return true;
            return false;
        };
        uint32_t p = head;
        for (uint32_t steps = 0; steps < 600u && wideCamOk(p); ++steps)
        {
            const unsigned fl = READ16(p + 2u);
            hudMemberAdd(p, buildVb);
            // Flag 0x8000 objects (origin-centred: the subtitle box) are never group members: their rectangle is
            // centred on the position, not [x, x + w], and they pulled neighbours into a group, shifting its centre.
            // enemy target box pieces are mapped about their own box (hudBoxCentre), not as group members
            if (n < 160u && !(fl & 0x02u) && !(fl & 0x20u) && !(fl & 0x8000u) && !isCentre(p) && !hudIsMarker(rdram, ctx, runtime, p) &&
                !(g_enemyBox && hudBoxPoolOf(rdram, ctx, runtime, p) != nullptr))
            {
                const float w = hudSpriteWidth(rdram, ctx, runtime, p);
                const float x = wideBitsToF(READ32(p + 0x50u));
                const float y = wideBitsToF(READ32(p + 0x54u));
                // the screen-centre-origin translucent overlay (hudIsScreenOverlay): never part of a group, never moved
                // (full-width backdrops, fades and bars stay untouched individually and never join a group)
                if (w > 0.0f && w < g_hudFullWidth && x >= -512.0f && x <= 1024.0f && y >= 0.0f && y <= 448.0f &&
                    !hudIsScreenOverlay(rdram, ctx, runtime, p, x, y))
                {
                    const uint32_t buf = hudDrawBuf(rdram, ctx, runtime, p);
                    float h = wideBitsToF(READ32(buf + 0xC4u));
                    if (!(h > 0.5f && h <= 448.0f))
                        h = 16.0f;
                    // The sprite rectangle is taken as [x, x + w] (first vertex at the origin, w = x of the second
                    // vertex). The "-PAUSE-" text is a sprite at position (0, 0) whose vertices are at x 185..327,
                    // y 217..231: it looked 327 px wide, so a spanning-bar rule stretched it. An object whose first
                    // vertex is not at x 0 (and with unrotated axes) uses its real rectangle instead.
                    float lo = 0.0f;
                    float vl, vh, yl, yh, v0x;
                    if (std::fabs(wideBitsToF(READ32(p + 0x20u)) - 1.0f) < 0.01f &&
                        hudObjExtent(rdram, ctx, runtime, p, vl, vh, &yl, &yh, &v0x) && std::fabs(v0x) > 0.5f)
                    {
                        lo = vl;
                        nodes[n] = Node{p, x + vl, y + yl, vh - vl, std::max(yh - yl, 1.0f), static_cast<int>(n), lo};
                        if (g_wideLog)
                        {
                            static uint32_t seenOff[16];
                            static uint32_t seenOffN = 0u;
                            bool seen = false;
                            for (uint32_t i = 0; i < seenOffN && !seen; ++i)
                                seen = (seenOff[i] == p);
                            if (!seen && seenOffN < 16u)
                            {
                                seenOff[seenOffN++] = p;
                                std::printf("[zoe:hud] offset-rectangle object 0x%08x: pos (%g,%g) x %g..%g y %g..%g\n", p, x, y, vl,
                                            vh, yl, yh);
                            }
                        }
                    }
                    else
                        nodes[n] = Node{p, x, y, w, h, static_cast<int>(n), 0.0f};
                    ++n;
                }
            }
            const uint32_t next = READ32(p + 0xCu);
            if (next == head)
                break;
            p = next;
        }
        if (g_wideLog)
        {
            // which 2D lists carry sprites (one line per list); the sentinel node (hidden, no packet buffer) is filtered
            static uint32_t seenHeads[16];
            static uint32_t seenN = 0u;
            bool seen = false;
            for (uint32_t i = 0; i < seenN && !seen; ++i)
                seen = (seenHeads[i] == head);
            if (!seen && seenN < 16u)
            {
                seenHeads[seenN++] = head;
                std::printf("[zoe:hud] 2D list head=0x%08x (first drawn object 0x%08x): %u groupable sprites\n", head, start, n);
            }
        }
        auto find = [&](int i)
        {
            while (nodes[i].parent != i)
            {
                nodes[i].parent = nodes[nodes[i].parent].parent;
                i = nodes[i].parent;
            }
            return i;
        };
        constexpr float kGap = 6.0f;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j)
            {
                const bool xo = !(nodes[i].x + nodes[i].w + kGap < nodes[j].x || nodes[j].x + nodes[j].w + kGap < nodes[i].x);
                const bool yo = !(nodes[i].y + nodes[i].h + kGap < nodes[j].y || nodes[j].y + nodes[j].h + kGap < nodes[i].y);
                if (xo && yo)
                    nodes[find(static_cast<int>(j))].parent = find(static_cast<int>(i));
            }
        for (uint32_t i = 0; i < n; ++i)
        {
            const int root = find(static_cast<int>(i));
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t k = 0; k < n; ++k)
                if (find(static_cast<int>(k)) == root)
                {
                    x0 = std::min(x0, nodes[k].x);
                    x1 = std::max(x1, nodes[k].x + nodes[k].w);
                    y0 = std::min(y0, nodes[k].y);
                    y1 = std::max(y1, nodes[k].y + nodes[k].h);
                }
            const float c = 0.5f * (x0 + x1);
            HudPlace &pl = L.place[L.n++];
            pl.obj = nodes[i].obj;
            pl.keep = (x1 - x0) >= g_hudFullWidth;
            pl.scale = kWideX;
            pl.lo = nodes[i].lo;
            pl.mode = (c < g_hudZoneL) ? 0u : (c >= g_hudZoneR) ? 1u : 2u;
            pl.nx = hudMapGroupX(nodes[i].x, c, pl.mode);
            // a group lying on a centre-attached piece takes its centre map (see g_hudCentreGroups)
            uint32_t onCentre = 0u;
            for (uint32_t k = 0; g_hudCentreGroups && !pl.keep && k < centreRectN && onCentre == 0u; ++k)
            {
                const HudRect &r = centreRects[k];
                if (x0 >= r.x0 - kGap && x1 <= r.x1 + kGap && !(y1 + kGap < r.y0 || r.y1 + kGap < y0))
                    onCentre = r.obj;
            }
            if (onCentre != 0u)
            {
                pl.mode = 2u;
                pl.nx = hudMapGroupX(nodes[i].x, 256.0f, 2u);
                if (g_wideLog && static_cast<int>(i) == root)
                {
                    static uint32_t seenG[32];
                    static uint32_t seenGN = 0u;
                    bool seen = false;
                    for (uint32_t s = 0; s < seenGN && !seen; ++s)
                        seen = (seenG[s] == nodes[i].obj);
                    if (!seen && seenGN < 32u)
                    {
                        seenG[seenGN++] = nodes[i].obj;
                        std::printf("[zoe:hud] group of 0x%08x (x %g..%g, y %g..%g) lies on centre-attached 0x%08x: centre map\n",
                                    nodes[i].obj, x0, x1, y0, y1, onCentre);
                    }
                }
            }
            // A group whose bar (its widest member: frame or backdrop of a health bar) runs from the left part into the
            // right part of the screen is stretched like a PS3 HD boss bar: the bar's left edge takes the left-anchor
            // mapping and its right edge the right-anchor mapping, and every member inside the bar gets that one
            // affine map (positions and widths), so a fill keeps the frame's mapping however far it is depleted.
            // A member outside the bar (a text label joined by the 6 px rule) is not stretched: it keeps its own zone
            // mapping (left / right / centre by its own centre) at S.
            float b0 = 0.0f, b1 = 0.0f;
            if (!pl.keep && onCentre == 0u)
            {
                float bigW = -1.0f;
                for (uint32_t k = 0; k < n; ++k)
                    if (find(static_cast<int>(k)) == root && nodes[k].w > bigW)
                    {
                        bigW = nodes[k].w;
                        b0 = nodes[k].x;
                        b1 = nodes[k].x + nodes[k].w;
                    }
                // A bar is stretched like a PS3 HD boss bar only if it reaches both outer quarters of the screen (starts left
                // of x 128 and ends right of x 384). Reaching from the left zone into the right zone (215 / 297) is not
                // enough: the centred "-PAUSE-" text (x 185..327) did that and was stretched 1.65x; such centred elements
                // map by their centre like any other group.
                if (b0 < 128.0f && b1 > 384.0f && (b1 - b0) < g_hudFullWidth)
                {
                    const float X0 = kWideX * b0;
                    const float X1 = 512.0f - kWideX * (512.0f - b1);
                    const float a = (X1 - X0) / (b1 - b0);
                    const float xi = nodes[i].x, xe = xi + nodes[i].w;
                    if (xi >= b0 - 1.0f && xe <= b1 + 1.0f)
                    {
                        pl.nx = X0 + a * (xi - b0);
                        pl.scale = a;
                        pl.mode = 2u;
                    }
                    else
                    {
                        const float ci = xi + 0.5f * nodes[i].w;
                        pl.mode = (ci < g_hudZoneL) ? 0u : (ci >= g_hudZoneR) ? 1u : 2u;
                        pl.nx = hudMapGroupX(xi, ci, pl.mode);
                    }
                }
            }
        }
        for (uint32_t i = 0; i < L.n; ++i)
            L.place[i].nx -= L.place[i].scale * L.place[i].lo; // from the rectangle's left edge back to the position
        hudBuildGsBlocks(rdram, ctx, runtime, head, L);
    }

    const HudPlace *hudFindPlace(uint32_t o)
    {
        for (uint32_t l = 0; l < g_listN; ++l)
            for (uint32_t i = 0; i < g_lists[l].n; ++i)
                if (g_lists[l].place[i].obj == o)
                    return &g_lists[l].place[i];
        return nullptr;
    }

    // Place of object o in the group map of its list. A list is built once per vblank (on the first object of it drawn):
    // all its nodes go into the member table, so objects that are not groupable (hidden, flag 0x20 pieces, markers) are
    // answered by one lookup. Without a free list slot nothing is built (the object then gets the per-object rule).
    const HudPlace *hudPlaceFor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t o)
    {
        const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        if (vb != g_listVblank)
        {
            if (g_wideLog && g_buildsThisVb > g_buildsMax)
            {
                g_buildsMax = g_buildsThisVb;
                std::printf("[zoe:hud] list builds in one vblank: new maximum %u (overflow refusals so far %u)\n", g_buildsMax,
                            g_buildsOverflow);
            }
            g_buildsThisVb = 0u;
            g_listVblank = vb;
            g_listN = 0u;
        }
        if (const HudPlace *p = hudFindPlace(o))
            return p;
        if (hudMemberHas(o, vb))
            return nullptr; // its list was already built this vblank and it is not a placed member
        if (g_listN >= kMaxLists)
        {
            ++g_buildsOverflow;
            return nullptr;
        }
        hudBuildGroups(rdram, ctx, runtime, o);
        const HudPlace *r = hudFindPlace(o);
        if (r == nullptr)
            hudMemberAdd(o, vb); // not found by the walk (corrupt list): do not build again for it this vblank
        return r;
    }

    // True when no 3D object was drawn in the last 3 vblanks (boot text, logos, menus on a black screen).
    bool hudPure2d()
    {
        const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        return g_have3dHook && (vb < g_last3dVblank || vb - g_last3dVblank > 3u);
    }

    // Centre (hud x) of an unrotated 2D object's packet rectangle; false for anything else.
    bool hudBlockCentre(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, float &c)
    {
        const float ax = wideBitsToF(READ32(s + 0x20u)), ay = wideBitsToF(READ32(s + 0x24u));
        const float bx = wideBitsToF(READ32(s + 0x30u));
        const float px = wideBitsToF(READ32(s + 0x50u));
        float lo, hi;
        if (!(ax > 0.5f) || std::fabs(ay) > 0.001f || std::fabs(bx) > 0.001f || !hudObjExtent(rdram, ctx, runtime, s, lo, hi))
            return false;
        c = px + ax * 0.5f * (lo + hi);
        return true;
    }

    // Horizontal span [lo, hi] of an object packet vertices on screen relative to its position, with the object axes
    // applied (x = A.x * vx + B.x * vy), and the largest local |y| (the ring radius of a reticle arc).
    bool hudObjScreenSpan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, float ax, float bx, float &lo,
                          float &hi, float &rMax)
    {
        const uint32_t cnt = READ32(s + 0x8Cu);
        const uint32_t buf = hudDrawBuf(rdram, ctx, runtime, s);
        if (cnt < 1u || cnt > 48u || !wideCamOk(buf) || buf + cnt * 0x30u + 0x30u > PS2_RAM_SIZE)
            return false;
        lo = 1e9f;
        hi = -1e9f;
        rMax = 0.0f;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            const float vx = wideBitsToF(READ32(buf + i * 0x30u + 0x20u));
            const float vy = wideBitsToF(READ32(buf + i * 0x30u + 0x24u));
            if (!(std::fabs(vx) < 4096.0f) || !(std::fabs(vy) < 4096.0f))
                return false;
            const float sx = ax * vx + bx * vy;
            lo = std::min(lo, sx);
            hi = std::max(hi, sx);
            rMax = std::max(rMax, std::fabs(vy));
        }
        return true;
    }

    // 2D-only screens (pillarbox): the 4:3 picture ends at hud x 0 and 512 and the GS clips whatever the game draws past
    // that. Squeezing about the centre pulls such parts into the side areas: on the GAME OVER screen a light band that
    // sweeps over the title (object 0x528b60 in ws_game_over, three columns of two vertices at x 430 / 548 / 599, alpha
    // 0 / 120 / 0, u 0.84 / 1.071 / 1.17) showed the texture's repeat (a second "G") and its bottom edge right of the
    // title. So before squeezing, an unrotated object's vertices are clipped to the original window, in local x: a
    // column (vertices sharing x) past an edge is moved onto it with u and RGBA interpolated, per row, between the last
    // column inside and the first one outside (v, q and y stay: rows run horizontally, and a 2-vertex sprite's corners
    // keep their own v); a buffer with nothing inside the window has every vertex moved onto the nearer edge (zero
    // width, nothing drawn), so the game's calls stay exactly as in stock. Only the buffer this call draws is clipped
    // (hudBufferAboutToDraw); the writes go through clipPut and are put back in a later game frame (the packet only
    // points at the buffer). Buffers with more than 8 vertices, ragged columns or no x scale are left alone.
    // PS2X_ZOE_WIDE_CLIP=0: off.
    bool g_hudClip = true;

    // The sprite "width" (hudSpriteWidth: x of the second vertex) is a width only for a 2-vertex sprite. The Versus
    // screen's selection brackets are 8-12 vertex strips (an 8-px bar plus a 512-px line); for the right-hand pieces the
    // second vertex is the line's far end (520), so they passed as full-width backdrops and kept their 4:3 place while
    // the left-hand pieces, text and icons were squeezed (bars off the text and stage icons in 16:9, ws_versus.p2s). On
    // 2D-only screens the backdrop test applies to 2-vertex sprites only. PS2X_ZOE_WIDE_STRIP=0: old test.
    bool g_hudStripRule = true;
    bool hudClipBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t buf, uint32_t cnt, float px, float ax)
    {
        if (!(ax > 0.01f) || cnt < 2u || cnt > 8u || !wideCamOk(buf) || buf + cnt * 0x30u > PS2_RAM_SIZE)
            return false;
        constexpr uint32_t kAttr[5] = {0x00u, 0x10u, 0x14u, 0x18u, 0x1Cu}; // u, R, G, B, A
        float vx[8], vy[8], at[8][5];
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            const uint32_t v = buf + i * 0x30u;
            vx[i] = wideBitsToF(READ32(v + 0x20u));
            vy[i] = wideBitsToF(READ32(v + 0x24u));
            if (!(std::fabs(vx[i]) < 4096.0f) || !(std::fabs(vy[i]) < 4096.0f))
                return false;
            for (uint32_t k = 0; k < 5u; ++k)
                at[i][k] = wideBitsToF(READ32(v + kAttr[k]));
            lo = std::min(lo, px + ax * vx[i]);
            hi = std::max(hi, px + ax * vx[i]);
        }
        const float edgeL = (0.0f - px) / ax, edgeR = (512.0f - px) / ax; // the window in local x
        // 0.01 px slack: a buffer already moved onto an edge (px + ax * edge rounds either way) counts as done
        if (lo >= -0.01f && hi <= 512.01f)
            return false;
        if (hi <= 0.01f || lo >= 511.99f)
        {
            for (uint32_t i = 0; i < cnt; ++i)
                clipPut(rdram, ctx, runtime, buf + i * 0x30u + 0x20u, hi <= 0.0f ? edgeL : edgeR);
            return true;
        }
        // columns, sorted by x; rows inside a column sorted by y
        uint32_t col[8][8], colN[8] = {}, nCols = 0u;
        float colX[8];
        for (uint32_t i = 0; i < cnt; ++i)
        {
            uint32_t c = 0u;
            while (c < nCols && std::fabs(colX[c] - vx[i]) > 0.01f)
                ++c;
            if (c == nCols)
                colX[nCols++] = vx[i];
            col[c][colN[c]++] = i;
        }
        for (uint32_t a = 0; a < nCols; ++a)
            for (uint32_t b = a + 1u; b < nCols; ++b)
                if (colX[b] < colX[a])
                {
                    std::swap(colX[a], colX[b]);
                    std::swap(colN[a], colN[b]);
                    for (uint32_t r = 0; r < 8u; ++r)
                        std::swap(col[a][r], col[b][r]);
                }
        for (uint32_t c = 0; c < nCols; ++c)
        {
            if (colN[c] != colN[0])
                return false;
            std::sort(col[c], col[c] + colN[c], [&](uint32_t p, uint32_t q) { return vy[p] < vy[q]; });
        }
        uint32_t first = 0u, last = nCols; // inside columns: [first, last)
        while (first < nCols && colX[first] < edgeL)
            ++first;
        while (last > first && colX[last - 1u] > edgeR)
            --last;
        if (first == last)
            return false; // columns on both sides, none inside: wider than the window (backdrops; the width test keeps them)
        const auto clipSide = [&](uint32_t in, uint32_t out, float edge, uint32_t from, uint32_t to)
        {
            const float t = (edge - colX[in]) / (colX[out] - colX[in]);
            for (uint32_t r = 0; r < colN[in]; ++r)
            {
                float e[5];
                for (uint32_t k = 0; k < 5u; ++k)
                    e[k] = at[col[in][r]][k] + t * (at[col[out][r]][k] - at[col[in][r]][k]);
                for (uint32_t c = from; c < to; ++c)
                {
                    const uint32_t v = buf + col[c][r] * 0x30u;
                    clipPut(rdram, ctx, runtime, v + 0x20u, edge);
                    for (uint32_t k = 0; k < 5u; ++k)
                        clipPut(rdram, ctx, runtime, v + kAttr[k], e[k]);
                }
            }
        };
        if (first > 0u)
            clipSide(first, first - 1u, edgeL, 0u, first);
        if (last < nCols)
            clipSide(last - 1u, last, edgeR, last, nCols);
        return true;
    }

    // The buffer the sprite routine is about to draw. FUN_00459908 sets +0x93 = 2 and, unless byte 0x4aa28c is set, calls
    // FUN_004599e0 first, which flips a selector 0 / 1 (+0x92 = 1 - +0x92; 0xFF stays); FUN_00459230 then reads the
    // buffer through FUN_00459d80 (+0x94 if +0x92 != 0, else +0x98). The builder FUN_00459d60 writes the buffer of the
    // selector before the flip, so this is the one the game just filled. (hudDrawBuf reads the selector before the flip:
    // for flipping objects it describes the previous frame's buffer.)
    constexpr uint32_t kSpriteNoFlipAddr = 0x004AA28Cu; // lbu -0x5D74 from 0x4B0000 at 0x45992c
    uint32_t hudBufferAboutToDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s)
    {
        int8_t sel = static_cast<int8_t>(READ8(s + 0x92u));
        if (READ32(s + 0x8Cu) != 0u && READ8(kSpriteNoFlipAddr) == 0u && sel >= 0)
            sel = static_cast<int8_t>(1 - sel);
        return sel != 0 ? READ32(s + 0x94u) : READ32(s + 0x98u);
    }

    // The buffer this call will draw, clipped (see hudClipBuffer); true if anything was written.
    bool hudClipToWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, float px, float ax)
    {
        const uint32_t cnt = READ32(s + 0x8Cu);
        const uint32_t buf = hudBufferAboutToDraw(rdram, ctx, runtime, s);
        const bool any = hudClipBuffer(rdram, ctx, runtime, buf, cnt, px, ax);
        if (g_wideLog)
        {
            const uint64_t tick = ps2CurrentVsyncTick();
            static uint32_t clipLines = 0u;
            if (tick >= g_hudTickLo && tick <= g_hudTickHi && clipLines < 400u)
            {
                ++clipLines;
                std::printf("[zoe:clip] t=%llu obj=%08x cnt=%u buffer %08x (selector before the draw %u) clipped=%d\n",
                            static_cast<unsigned long long>(tick), s, cnt, buf, READ8(s + 0x92u), any ? 1 : 0);
            }
        }
        return any;
    }

    // ---- Widescreen audit (PS2X_ZOE_WIDE_AUDIT=<csv path>,<tickLo>,<tickHi>; read by work/scripts/wsaudit.py) ----
    // One CSV row per 2D piece the wrappers see inside the tick window (vsync ticks of the window loop): which wrapper
    // (S sprite, T text object, C window container), the object, its flags, what the widescreen code did with it (stat:
    // 0 left zone, 1 right zone, 2 middle / centre map, 3 GS-coordinate piece, 4 marker, 5 untouched, 6 not processed
    // (classes off, hidden), 7 movie playing, 8 skipped (not drawn)), whether the screen counted as 2D-only, and the
    // piece's position, x axes and horizontal vertex span on screen (hud pixels: GS-coordinate pieces converted with
    // x - 1791.5) BEFORE and AFTER the mapping (and the clip). The analyser looks for pieces never mapped, pieces whose
    // invisible 4:3 part lands inside the 16:9 picture, and mapped positions that jump while the 4:3 position does not.
    // Costs one compare per call when off.
    FILE *g_auditFile = nullptr;
    uint64_t g_auditLo = 1u, g_auditHi = 0u;
    struct AuditPiece
    {
        bool on = false;
        uint32_t s = 0u, buf = 0u;
        float px = 0.0f, ax = 0.0f, bx = 0.0f, lo = NAN, hi = NAN;
    };

    // Horizontal screen span of buffer buf (cnt vertices) with position px and x axes ax / bx; false if unreadable.
    bool auditSpan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t buf, uint32_t cnt, float px, float ax, float bx,
                   float &lo, float &hi)
    {
        if (cnt < 1u || cnt > 512u || !wideCamOk(buf) || buf + cnt * 0x30u > PS2_RAM_SIZE)
            return false;
        lo = 1e9f;
        hi = -1e9f;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            const float vx = wideBitsToF(READ32(buf + i * 0x30u + 0x20u)), vy = wideBitsToF(READ32(buf + i * 0x30u + 0x24u));
            if (!(std::fabs(vx) < 4096.0f) || !(std::fabs(vy) < 4096.0f))
                return false;
            const float sx = px + ax * vx + bx * vy;
            lo = std::min(lo, sx);
            hi = std::max(hi, sx);
        }
        return true;
    }

    // s = the object's 0x459908 layout (the object, or a text object's sub-object at +0x20); buf = the buffer it draws.
    AuditPiece auditBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, uint32_t buf)
    {
        AuditPiece a;
        if (g_auditFile == nullptr || !wideCamOk(s))
            return a;
        const uint64_t tick = ps2CurrentVsyncTick();
        if (tick > g_auditHi)
        {
            std::fclose(g_auditFile); // the window is over: complete on disk before the run is stopped
            g_auditFile = nullptr;
            return a;
        }
        if (tick < g_auditLo)
            return a;
        a.on = true;
        a.s = s;
        a.buf = buf;
        a.px = wideBitsToF(READ32(s + 0x50u));
        a.ax = wideBitsToF(READ32(s + 0x20u));
        a.bx = wideBitsToF(READ32(s + 0x30u));
        if (!auditSpan(rdram, ctx, runtime, buf, READ32(s + 0x8Cu), a.px, a.ax, a.bx, a.lo, a.hi))
            a.lo = a.hi = NAN;
        return a;
    }

    void auditEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const AuditPiece &a, char kind, uint32_t o, unsigned stat)
    {
        if (!a.on)
            return;
        const float px = wideBitsToF(READ32(a.s + 0x50u));
        const float ax = wideBitsToF(READ32(a.s + 0x20u));
        const float bx = wideBitsToF(READ32(a.s + 0x30u));
        float lo = NAN, hi = NAN;
        if (kind != 'C' && !auditSpan(rdram, ctx, runtime, a.buf, READ32(a.s + 0x8Cu), px, ax, bx, lo, hi))
            lo = hi = NAN;
        const unsigned flags = READ16(a.s + 2u);
        const float off = (flags & 0x20u) ? 1791.5f : 0.0f;
        std::fprintf(g_auditFile, "%llu,%u,%c,%08x,%04x,%u,%d,%.3f,%.4f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.3f,%.3f,%.3f\n",
                     static_cast<unsigned long long>(ps2CurrentVsyncTick()), READ32(kFrameCounterAddr), kind, o, flags, stat,
                     hudPure2d() ? 1 : 0, a.px - off, a.ax, a.bx, a.lo - off, a.hi - off, px - off, ax, bx, lo - off, hi - off,
                     wideBitsToF(READ32(a.s + 0x54u)));
    }

    // Map an unrotated, screen-fixed 2D object (hud pixels, position at s + 0x50, x axes at s + 0x20 / s + 0x30) as one
    // block, so that a message, its box and the pause / menu text keep their proportions in 16:9: with a 3D scene up the
    // block's centre c picks the zone (left edge, right edge or about its own centre) exactly as the corner HUD does, and
    // the origin follows the same affine map (x' = map(x) maps every vertex of the block, so no glyph moves on its own);
    // with no 3D scene the object shrinks about the screen centre. Writes through hudPut (restored after the draw).
    // Returns false (nothing written) for an object it does not understand.
    // cOverride: use this block centre instead of the object's own (the subtitle box takes its text's centre, so the two
    // can never pick different zones, whatever their extents are).
    bool hudMapBlock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t s, bool pure2d, unsigned cls,
                     const float *cOverride = nullptr)
    {
        const float ax = wideBitsToF(READ32(s + 0x20u)), ay = wideBitsToF(READ32(s + 0x24u));
        const float bx = wideBitsToF(READ32(s + 0x30u));
        const float px = wideBitsToF(READ32(s + 0x50u));
        if (!(ax > 0.5f) || std::fabs(ay) > 0.001f || std::fabs(bx) > 0.001f || !(px > -512.0f && px < 1024.0f))
            return false;
        float nx;
        if (pure2d)
            nx = 256.0f + kWideX * (px - 256.0f);
        else
        {
            float lo, hi;
            if (!hudObjExtent(rdram, ctx, runtime, s, lo, hi))
                return false;
            const float l = px + ax * lo, r = px + ax * hi;
            if (r - l >= g_hudFullWidth)
                return false;
            const float c = (cOverride != nullptr) ? *cOverride : 0.5f * (l + r);
            const unsigned mode = (c < g_hudZoneL) ? 0u : (c >= g_hudZoneR) ? 1u : 2u;
            nx = hudMapGroupX(px, c, mode);
        }
        hudPut(rdram, ctx, runtime, s + 0x50u, nx);
        hudPut(rdram, ctx, runtime, s + 0x20u, ax * kWideX);
        hudPut(rdram, ctx, runtime, s + 0x30u, bx * kWideX);
        mapLog(s, cls, nx, kWideX);
        return true;
    }

    // Always-on guard: per EE vblank the widescreen sprite wrapper adds up its own work (everything before the original
    // runs: lookups, list builds, mapping; the original's time is not included) and its calls. When one vblank costs more
    // than 2 ms, or has more than 3000 calls (a runaway re-entry like the skipped-arc loop shows up as thousands of calls
    // for one object), a line goes to play.log (at most 3 lines per run) with the counts. Cost: two clock reads per call.
    uint64_t g_budVb = ~0ull;
    uint64_t g_budNs = 0u;
    uint32_t g_budCalls = 0u, g_budSkips = 0u, g_budBuilds = 0u, g_budLines = 0u;
    uint32_t g_budLastObj = 0u, g_budSameObj = 0u, g_budMaxSame = 0u;

    void hudBudgetNote(std::chrono::steady_clock::time_point t0, bool skipped, uint32_t o)
    {
        const uint64_t ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        const uint64_t vb = ps2Perf().vblanks.load(std::memory_order_relaxed);
        if (vb != g_budVb)
        {
            if (g_budVb != ~0ull && g_budLines < 3u && (g_budNs > 2000000ull || g_budCalls > 3000u))
            {
                ++g_budLines;
                std::printf("[zoe:wide] HUD budget exceeded in vblank %llu: %.2f ms in the sprite wrapper over %u calls, %u arc skips, "
                            "max list builds %u, member-table refusals %u, longest run of one object %u\n",
                            static_cast<unsigned long long>(g_budVb), static_cast<double>(g_budNs) / 1.0e6, g_budCalls, g_budSkips,
                            g_budBuilds, g_buildsOverflow, g_budMaxSame);
            }
            g_budVb = vb;
            g_budNs = 0u;
            g_budCalls = g_budSkips = g_budBuilds = g_budMaxSame = 0u;
        }
        g_budNs += ns;
        ++g_budCalls;
        if (skipped)
            ++g_budSkips;
        g_budBuilds = std::max(g_budBuilds, g_buildsThisVb);
        g_budSameObj = (o == g_budLastObj) ? g_budSameObj + 1u : 1u;
        g_budLastObj = o;
        g_budMaxSame = std::max(g_budMaxSame, g_budSameObj);
    }

    void zoeSpriteHud(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wideActiveNow(rdram, ctx, runtime))
        {
            if (g_clipSavedN != 0u)
                clipRestoreOld(rdram, ctx, runtime, true);
            g_origSprite(rdram, ctx, runtime);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        wideCheckLoad(rdram, ctx, runtime);
        hudRestore(rdram, ctx, runtime);
        if (g_clipSavedN != 0u)
            clipRestoreOld(rdram, ctx, runtime, false);
        const uint32_t o = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint32_t n = ++g_hudCalls;
        const bool ok = wideCamOk(o);
        const bool marker = ok && g_hudMarker && hudIsMarker(rdram, ctx, runtime, o);
        if (marker)
            ++g_hudMarkerObjs;
        const unsigned flags = ok ? READ16(o + 2u) : 0u;
        const float width = ok ? hudSpriteWidth(rdram, ctx, runtime, o) : 0.0f;
        const AuditPiece aud = ok ? auditBegin(rdram, ctx, runtime, o, hudBufferAboutToDraw(rdram, ctx, runtime, o)) : AuditPiece{};
        unsigned auditStat = marker ? 4u : (ok && ps2MovieActive()) ? 7u : 6u;
        if (ok && g_wideLog)
        {
            const uint64_t tick = ps2CurrentVsyncTick(); // log only
            const bool inCount = n > g_hudLogSkip && n <= g_hudLogSkip + g_hudLogCount;
            const bool inTicks = tick >= g_hudTickLo && tick <= g_hudTickHi && g_hudTickLines < g_hudTickMax;
            if (inCount || inTicks)
            {
                if (inTicks)
                    ++g_hudTickLines;
                float f[16];
                for (uint32_t i = 0; i < 16u; ++i)
                    f[i] = wideBitsToF(READ32(o + 0x20u + i * 4u));
                std::printf("[zoe:hud] t=%llu #%u obj=%08x ra=%08x f2=%04x w=%g tag=%d A=(%g,%g,%g) B=(%g,%g,%g) "
                            "50=(%g,%g,%g,%g)\n",
                            static_cast<unsigned long long>(tick), n, o, ra, flags, width, marker ? 1 : 0, f[0], f[1], f[2],
                            f[4], f[5], f[6], f[12], f[13], f[14], f[15]);
            }
        }
        bool skipDraw = false;
        // a movie (4:3 content, shown boxed by the window) and everything drawn over it stays as the game made it
        if (g_hudOn && ok && !marker && !ps2MovieActive()) // marker objects: as the game built them (width: zoeMarkerWide)
        {
            const float ax = wideBitsToF(READ32(o + 0x20u));
            const float bx = wideBitsToF(READ32(o + 0x30u));
            const float px = wideBitsToF(READ32(o + 0x50u));
            const float py = wideBitsToF(READ32(o + 0x54u));
            const bool pure2d = hudPure2d();
            if (g_wideLog)
            {
                static bool lastPure = false;
                if (pure2d != lastPure)
                {
                    lastPure = pure2d;
                    std::printf("[zoe:hud] vblank %llu: %s mode (last 3D object draw at vblank %llu)\n",
                                static_cast<unsigned long long>(ps2Perf().vblanks.load(std::memory_order_relaxed)),
                                pure2d ? "2D-only (pillarbox)" : "3D-scene (HUD edges)",
                                static_cast<unsigned long long>(g_last3dVblank));
                }
            }
            unsigned stat = 5;
            HudBoxPool *boxPool = (g_enemyBox && !(flags & 0x20u) && !pure2d) ? hudBoxPoolOf(rdram, ctx, runtime, o) : nullptr;
            if (flags & 0x20u)
            {
                // GS coordinates: projected things keep their position. An unrotated projected object belongs to a block
                // (hudBuildGsBlocks) narrowed about the block's centre; other objects (rotated reticle arcs) just get
                // their axes narrowed; with no 3D scene everything shrinks about the GS centre.
                const HudPlace *gp = nullptr;
                float cx, cy, cz, cw;
                // only an object that can be a block member is looked up (see hudGsCandidate)
                if (!pure2d && hudGsCandidate(rdram, ctx, runtime, o, cx, cy, cz, cw))
                    gp = hudPlaceFor(rdram, ctx, runtime, o);
                if (gp != nullptr && gp->mode == 3u)
                {
                    if (clsOn(2))
                    {
                        hudPut(rdram, ctx, runtime, o + 0x50u, gp->nx);
                        hudPut(rdram, ctx, runtime, o + 0x20u, ax * gp->scale);
                        hudPut(rdram, ctx, runtime, o + 0x30u, bx * gp->scale);
                        mapLog(o, 2u, gp->nx, gp->scale);
                    }
                }
                else if (clsOn(4))
                {
                    // Rotated GS pieces are the lock-on reticle arcs. Their packet holds the ring radius (local y at
                    // packet +0x24): 25..68 while locked on, 336..512 while parked (no target). A parked ring lies
                    // outside the 4:3 window, where the GS clips it, but squeezing x pulls its ends into the picture (the
                    // short diagonal lines near the NONE bracket and the gauge). So a rotated piece (a y component on the A
                    // axis, an x component on the B axis, or a flipped A axis for the 180 degree arc) none of whose
                    // vertices lies inside the original 4:3 window (GS x 1791.5..2303.5; screen x of a vertex =
                    // position + A.x * vx + B.x * vy) is not drawn; stock draws nothing of it either. All vertices are
                    // tested, and scaled-down unrotated sprites are never skipped.
                    const float ay2 = wideBitsToF(READ32(o + 0x24u));
                    const bool rotated = std::fabs(ay2) > 0.01f || std::fabs(bx) > 0.01f || ax < 0.0f;
                    float sLo = 0.0f, sHi = 0.0f, rMax = 0.0f;
                    const bool span = rotated && hudObjScreenSpan(rdram, ctx, runtime, o, ax, bx, sLo, sHi, rMax);
                    const bool outside = span && (px + sHi < 1791.5f || px + sLo > 2303.5f);
                    if (g_wideLog)
                    {
                        const uint64_t tick = ps2CurrentVsyncTick();
                        static uint32_t arcLines = 0u;
                        if (tick >= g_hudTickLo && tick <= g_hudTickHi && arcLines < 4000u)
                        {
                            ++arcLines;
                            std::printf("[zoe:arc] t=%llu obj=%08x pos=(%g,%g,%g) A=(%g,%g) B=(%g,%g) rotated=%d radius=%g "
                                        "screen-x=[%g,%g] outside=%d\n",
                                        static_cast<unsigned long long>(tick), o, px, py, wideBitsToF(READ32(o + 0x58u)), ax,
                                        ay2, bx, wideBitsToF(READ32(o + 0x34u)), rotated ? 1 : 0, rMax, px + sLo, px + sHi,
                                        outside ? 1 : 0);
                        }
                    }
                    if (outside)
                    {
                        skipDraw = true;
                        mapLog(o, 4u, -1.0f, 0.0f);
                    }
                    else
                    {
                        if (pure2d)
                            hudPut(rdram, ctx, runtime, o + 0x50u, 2048.0f + kWideX * (px - 2048.0f));
                        hudPut(rdram, ctx, runtime, o + 0x20u, ax * kWideX);
                        hudPut(rdram, ctx, runtime, o + 0x30u, bx * kWideX);
                        mapLog(o, 4u, px, kWideX);
                    }
                }
                stat = 3;
            }
            else if ((flags & 0x8000u) && !pure2d)
            {
                // Flag 0x8000 sprites are origin-centred. Only the subtitle box is mapped: a 0x8000 object that is
                // directly followed in the list by a text object (type byte 4, FUN_0045b668) at the same position. It
                // takes the text block centre (same zone, same map as the text, whatever the two extents are) and the
                // text wrapper maps the text; any other 0x8000 object is left as the game draws it (and logged once).
                // Class 32.
                stat = 5;
                float tc = 0.0f;
                bool paired = false;
                const uint32_t nxo = READ32(o + 0xCu);
                if (wideCamOk(nxo) && READ8(nxo) == 4u && READ32(nxo + 0xD0u) != 0u && !(READ16(nxo + 0x22u) & 0x22u) &&
                    std::fabs(wideBitsToF(READ32(nxo + 0x70u)) - px) < 0.5f &&
                    std::fabs(wideBitsToF(READ32(nxo + 0x74u)) - py) < 0.5f)
                    paired = hudBlockCentre(rdram, ctx, runtime, nxo + 0x20u, tc);
                if (g_wideLog)
                {
                    static uint32_t seen8[32];
                    static uint32_t seen8N = 0u;
                    bool seen = false;
                    for (uint32_t i = 0; i < seen8N && !seen; ++i)
                        seen = (seen8[i] == o);
                    if (!seen && seen8N < 32u)
                    {
                        seen8[seen8N++] = o;
                        std::printf("[zoe:hud] flag-0x8000 object 0x%08x f2=%04x pos=(%g,%g) w=%g paired-with-text=%d\n", o,
                                    flags, px, py, width, paired ? 1 : 0);
                    }
                }
                if (paired && clsOn(32) && hudMapBlock(rdram, ctx, runtime, o, false, 32u, &tc))
                    stat = 2;
            }
            else if (boxPool != nullptr)
            {
                // A piece of an enemy target box (zoeEnemyBoxTask): about the box's own centre whatever its zone, so it
                // cannot jump when the centre crosses 215 / 297. Placed before the width test: an absolute-coordinate
                // piece's "width" is the x of its second vertex, which is >= 460.8 for a box in the far right. Class 2.
                // A box whose edges cannot be read is left as built (right place, 4:3 wide).
                stat = 5;
                float bc = 0.0f;
                if (clsOn(2) && ax > 0.5f && std::fabs(wideBitsToF(READ32(o + 0x24u))) < 0.001f && std::fabs(bx) < 0.001f &&
                    hudBoxCentre(rdram, ctx, runtime, *boxPool, bc))
                {
                    const float nx = hudMapGroupX(px, bc, 2u);
                    hudPut(rdram, ctx, runtime, o + 0x50u, nx);
                    hudPut(rdram, ctx, runtime, o + 0x20u, ax * kWideX);
                    hudPut(rdram, ctx, runtime, o + 0x30u, bx * kWideX);
                    mapLog(o, 2u, nx, kWideX);
                    stat = 2;
                }
            }
            else if ((width >= g_hudFullWidth && !(pure2d && g_hudStripRule && READ32(o + 0x8Cu) > 2u)) ||
                     hudIsScreenOverlay(rdram, ctx, runtime, o, px, py))
            {
                stat = 5; // backdrop, fade, bar, centre-origin overlay: untouched
            }
            else if (px >= -512.0f && px <= 1024.0f) // outside 0..512 too: the mapping is continuous there
            {
                float nx;
                float axisScale = kWideX;
                bool keep = false;
                if (pure2d)
                {
                    nx = 256.0f + kWideX * (px - 256.0f);
                    stat = 2;
                }
                else
                {
                    // Whole elements move as one: every object of a group gets the same mapping (hudBuildGroups). An
                    // object no list walk found is its own group.
                    const HudPlace *plp = hudPlaceFor(rdram, ctx, runtime, o);
                    if (plp != nullptr)
                    {
                        const HudPlace &pl = *plp;
                        keep = pl.keep;
                        nx = pl.nx;
                        axisScale = pl.scale;
                        stat = pl.mode;
                    }
                    else
                    {
                        const float c = px + 0.5f * width;
                        stat = (c < g_hudZoneL) ? 0u : (c >= g_hudZoneR) ? 1u : 2u;
                        nx = hudMapGroupX(px, c, stat);
                    }
                }
                const unsigned cls = (px < 0.0f || px > 512.0f) ? 8u : (axisScale > 0.76f ? 16u : 1u);
                // pillarbox: what lies past the 4:3 window stays invisible, as the GS clips it in stock
                if (pure2d && g_hudClip && !keep && clsOn(cls) && std::fabs(wideBitsToF(READ32(o + 0x24u))) < 0.001f &&
                    std::fabs(bx) < 0.001f)
                    (void)hudClipToWindow(rdram, ctx, runtime, o, px, ax);
                if (keep || !clsOn(cls))
                    stat = 5;
                else
                {
                    hudPut(rdram, ctx, runtime, o + 0x50u, nx);
                    hudPut(rdram, ctx, runtime, o + 0x20u, ax * axisScale);
                    hudPut(rdram, ctx, runtime, o + 0x30u, bx * axisScale);
                    mapLog(o, cls, nx, axisScale);
                }
            }
            ++g_hudStat[stat];
            auditStat = stat;
        }
        auditEnd(rdram, ctx, runtime, aud, 'S', o, skipDraw ? 8u : auditStat);
        if (g_wideLog && (n & (n - 1u)) == 0u && n >= 256u)
            std::printf("[zoe:hud] sprite objects=%u left=%u right=%u middle=%u gs=%u marker-builds=%u untouched=%u "
                        "marker-objects=%u\n",
                        n, g_hudStat[0], g_hudStat[1], g_hudStat[2], g_hudStat[3], g_hudStat[4], g_hudStat[5],
                        g_hudMarkerObjs);
        if (skipDraw)
        {
            // A parked reticle arc: not drawn (the game's 4:3 window would have clipped it). The call must still END the
            // way the original does, with pc = the return address. The wrapper can be entered fresh by the scheduler
            // (the dispatcher yielded before the call, ctx->pc is the entry address and $ra the caller's return address);
            // returning with pc left at the entry made the scheduler call this wrapper again for the same object, forever
            // (ws_town2_panel, tick 51311: one arc object skipped 3900+ times in one tick, every frame 154 ms). From a
            // normal dispatch pc = ra is what the runtime sets itself.
            ctx->pc = ra;
            hudBudgetNote(t0, true, o);
            return;
        }
        hudBudgetNote(t0, false, o);
        g_origSprite(rdram, ctx, runtime);
        if (g_hudSavedN != 0u && ctx->pc == ra)
            hudRestore(rdram, ctx, runtime); // else: interrupted, restored at the next entry
    }

    // FUN_0045b668(a0 = object): the renderer's text object: the on-screen message (subtitle: one object, two vertices per
    // text line in one packet buffer), and other text blocks. It calls FUN_0045a8a0(a0 + 0x40) to load the matrix and
    // FUN_0045b600, which builds the packet from the sub-object at a0 + 0x20 through FUN_00459230, so the sub-object has
    // the 0x459908 layout shifted by 0x20 (flags +0x22, x axes +0x40 / +0x50, position +0x70, vertex count +0xAC, packet
    // buffers +0xB4 / +0xB8, byte +0xB2); nothing is drawn when a0 + 0xD0 is 0. Measured in destroy_flash2 (RAM dump at
    // tick 14967): object 0x6001d0, sub-object position (255, 383), x -57.5..57.5, the box behind it (a 0x459908 object,
    // flag 0xc044) is at the same position with x -59.5..59.5. The block is mapped by hudMapBlock (class 64).
    PS2Runtime::RecompiledFunction g_origText = nullptr;
    constexpr uint32_t kTextFn = 0x0045B668u;
    uint32_t g_textLines = 0u;

    void zoeTextObject(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wideActiveNow(rdram, ctx, runtime))
        {
            g_origText(rdram, ctx, runtime);
            return;
        }
        wideCheckLoad(rdram, ctx, runtime);
        hudRestore(rdram, ctx, runtime);
        const uint32_t o = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        bool mapped = false;
        // text objects: the drawn buffer is read with the selector as it is (whether FUN_0045b600 flips it is not checked)
        const AuditPiece aud = (wideCamOk(o) && READ32(o + 0xD0u) != 0u) ? auditBegin(rdram, ctx, runtime, o + 0x20u, hudDrawBuf(rdram, ctx, runtime, o + 0x20u))
                                                                          : AuditPiece{};
        if (g_hudOn && wideCamOk(o) && READ32(o + 0xD0u) != 0u && !ps2MovieActive() && clsOn(64))
        {
            const uint32_t s = o + 0x20u;
            const unsigned sf = READ16(s + 2u);
            if (!(sf & 0x22u)) // not hidden, not GS coordinates (projected)
                mapped = hudMapBlock(rdram, ctx, runtime, s, hudPure2d(), 64u);
        }
        if (g_wideLog)
        {
            const uint64_t tick = ps2CurrentVsyncTick();
            if (tick >= g_hudTickLo && tick <= g_hudTickHi && g_textLines < g_hudTickMax && wideCamOk(o))
            {
                ++g_textLines;
                const uint32_t s = o + 0x20u;
                float lo = 0.0f, hi = 0.0f;
                const bool ext = hudObjExtent(rdram, ctx, runtime, s, lo, hi);
                std::printf("[zoe:text] t=%llu obj=%08x ra=%08x f2=%04x sub-flags=%04x d0=%08x count=%u pos=(%g,%g) "
                            "axes=(%g,%g,%g,%g) extent=%s[%g,%g] mapped=%d pure2d=%d\n",
                            static_cast<unsigned long long>(tick), o, ra, static_cast<unsigned>(READ16(o + 2u)),
                            static_cast<unsigned>(READ16(s + 2u)), READ32(o + 0xD0u), READ32(s + 0x8Cu),
                            wideBitsToF(READ32(s + 0x50u)), wideBitsToF(READ32(s + 0x54u)), wideBitsToF(READ32(s + 0x20u)),
                            wideBitsToF(READ32(s + 0x24u)), wideBitsToF(READ32(s + 0x30u)), wideBitsToF(READ32(s + 0x34u)),
                            ext ? "" : "none", lo, hi, mapped ? 1 : 0, hudPure2d() ? 1 : 0);
            }
        }
        auditEnd(rdram, ctx, runtime, aud, 'T', o, mapped ? 2u : ps2MovieActive() ? 7u : 5u);
        g_origText(rdram, ctx, runtime);
        if (g_hudSavedN != 0u && ctx->pc == ra)
            hudRestore(rdram, ctx, runtime);
    }

    // ---- Menu panels: window containers (FUN_0045abe0) ----
    // The item / passcode box, the mission result panel, the map-screen menu and its map window are "windows": one type-3
    // object in the 2D list (draw table 0x4A6CF8 slot 3 = FUN_0045abe0, called by the list walker FUN_0044f348 at 0x44f38c)
    // whose children hang in a list of their own (head +0x80, first child +0x8C, sentinel +0xA0). FUN_0045abe0 = begin
    // (FUN_0044e510), FUN_0045aa98, end (FUN_0044e588). FUN_0045aa98 loads the container's matrix (+0x20..+0x5F, only with
    // flag +2 & 0x40; otherwise VU0 microprogram 0xE38 loads identity) into VU0 vf28..31, then for every child that is not
    // hidden (+2 & 2): push vf28..31 (VU0 0x8C0), call the child through the type table at 0x4A9C58 (1 sprite FUN_0045a000,
    // 4 text FUN_0045b6c8, 2 callback FUN_004588e0, 0 model FUN_00450998, 3 nested = an error stub), pop (0x8E0). A sprite
    // child composes its own matrix with vf28..31 (FUN_0045a900 -> VU0 0x800: vf28..31 = child * parent, row vectors; with
    // child flag 0x400 its matrix replaces vf28..31 instead), a text child always composes its +0x40 matrix; FUN_00459230
    // then stores vf28..31 as the packet matrix. So the container matrix P is the parent of every piece. Measured in
    // ws_item_cockpit: container 0x7e7b20 has P = identity axes, position (160, 112); the window rectangle (x, y, w, h =
    // 160, 112, 192, 48) sits just before it, at c - 0x20; the backdrop child spans local x 0..192 (hud 160..352), the bars
    // and the text (local centre 96) lie inside it, all children have axes (1,0,0) / (0,0.5,0) and position 0. The children
    // never pass through FUN_00459908 / FUN_0045b668 (every [zoe:hud] / [zoe:text] line has ra 0x44f394, the list walker),
    // so nothing is mapped twice.
    // One affine map for the whole panel: P' = P * T with T = (x' = 0.75 x + b), so the x component of the four matrix rows
    // becomes 0.75 x + b w (w is 0 on the axis rows and 1 on the position row). Every child inherits it: frame, bars,
    // backdrop and text; no piece or glyph is looked at. The anchor follows the corner HUD rule by the panel's centre c
    // (its window rectangle, below): c < zoneL anchors at the left edge (b = 0), c >= zoneR at the right edge (b = 128),
    // otherwise about c (b = 0.25 c); a panel without a rectangle is mapped about the screen centre (b = 64). Before, every
    // panel was mapped about the screen centre, which moved the map-screen menu (24..174) to 82..194.5, over the left edge
    // of the 3D map window beside it (180.., stock: it has its own camera), user report 10 Oct; left-anchored it is
    // 18..130.5. The window's clip rectangle belongs to the panel: the type-2 child FUN_002137A0 builds SCISSOR_1 from
    // the window rectangle (a0 = child +0xA4: x, y, w, h floats), so the same map goes to its x and w (a window on one side
    // of the screen would clip its own content otherwise); its partner FUN_00213800 resets the scissor to the whole picture.
    // Mapped only when the container has its own matrix (flag 0x40) and every visible child inherits it: sprites with flag
    // 0x4 and none of 0x400 (absolute), 0x80 (direct copy), 0x20 (GS coordinates); text objects whose sub-object (+0x22)
    // has 0x4 and not 0x22; the scissor pair. Left as the game draws them: full-screen layers (letterbox bars, containers
    // with flags 0x224 and no own matrix) and the map-screen's 3D map window (type 0 models, 0x400 sprites, callbacks
    // 0x214940 / 0x214A78 / 0x44E050, its own camera). PS2X_ZOE_WIDE_PANELS=0 removes the hook, =2 maps every window
    // container (also the map window, whose 3D content then stays 4:3), default 1. Class bit 128. Containers without flag
    // 0x40 are left alone here unless they are identity-parent containers (class 512, hudIdentityMatrix below).
    PS2Runtime::RecompiledFunction g_origContainer = nullptr;
    constexpr uint32_t kContainerFn = 0x0045ABE0u;
    constexpr uint32_t kScissorFn = 0x002137A0u;
    constexpr uint32_t kScissorEndFn = 0x00213800u;
    unsigned g_panelMode = 1u;

    // True when every visible child of container c inherits its matrix; `scissor` receives the window rectangle address of the
    // clip callback (0 = none), `blame` the first child that does not inherit and `span` the widest sprite child (its vertex
    // extent times its own x axis, in the container's units; 0 when none can be read).
    bool hudPanelInherits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t c, uint32_t &scissor, uint32_t &blame,
                          float &span)
    {
        scissor = 0u;
        blame = 0u;
        span = 0.0f;
        const uint32_t sentinel = c + 0xA0u;
        uint32_t n = READ32(c + 0x8Cu);
        for (uint32_t guard = 0u; n != sentinel; ++guard)
        {
            if (guard > 256u || !wideCamOk(n))
            {
                blame = n;
                return false;
            }
            const unsigned f = READ16(n + 2u);
            if (!(f & 2u)) // hidden children are not drawn
            {
                const unsigned type = READ8(n);
                bool ok = false;
                if (type == 1u)
                {
                    ok = (f & 0x4u) != 0u && (f & (0x400u | 0x80u | 0x20u)) == 0u;
                    float lo, hi;
                    if (ok && hudObjExtent(rdram, ctx, runtime, n, lo, hi))
                        span = std::max(span, (hi - lo) * std::fabs(wideBitsToF(READ32(n + 0x20u))));
                }
                else if (type == 4u)
                {
                    const unsigned sf = READ16(n + 0x22u);
                    ok = (sf & 0x4u) != 0u && (sf & 0x22u) == 0u;
                }
                else if (type == 2u)
                {
                    const uint32_t fn = READ32(n + 0xA0u);
                    ok = fn == kScissorEndFn;
                    if (fn == kScissorFn && wideCamOk(READ32(n + 0xA4u)))
                    {
                        scissor = READ32(n + 0xA4u);
                        ok = true;
                    }
                }
                if (!ok)
                {
                    blame = n;
                    return false;
                }
            }
            n = READ32(n + 0xCu);
        }
        return true;
    }

    // Debug (PS2X_ZOE_WIDESCREEN_LOG + PS2X_ZOE_WIDE_HUDTICKS): one line per window container draw in the tick range with
    // its rectangle and position, then one per child (type, flags, axis x, position, vertex x extent), before mapping.
    void hudPanelDump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t c)
    {
        const uint64_t tick = ps2CurrentVsyncTick();
        static uint32_t lines = 0u;
        if (tick < g_hudTickLo || tick > g_hudTickHi || lines >= g_hudTickMax)
            return;
        const uint32_t r = c - 0x20u;
        ++lines;
        std::printf("[zoe:panel] t=%llu c=%08x flags %04x rect (%g,%g,%g,%g) pos (%g,%g)\n", static_cast<unsigned long long>(tick), c,
                    static_cast<unsigned>(READ16(c + 2u)), wideBitsToF(READ32(r)), wideBitsToF(READ32(r + 4u)),
                    wideBitsToF(READ32(r + 8u)), wideBitsToF(READ32(r + 12u)), wideBitsToF(READ32(c + 0x50u)),
                    wideBitsToF(READ32(c + 0x54u)));
        const uint32_t sentinel = c + 0xA0u;
        uint32_t n = READ32(c + 0x8Cu);
        for (uint32_t guard = 0u; n != sentinel && guard <= 64u && wideCamOk(n) && lines < g_hudTickMax; ++guard)
        {
            const unsigned type = READ8(n);
            const uint32_t s = (type == 4u) ? n + 0x20u : n;
            float lo = 0.0f, hi = 0.0f;
            const bool ext = (type == 1u || type == 4u) && hudObjExtent(rdram, ctx, runtime, s, lo, hi);
            ++lines;
            std::printf("[zoe:panel]   child %08x type %u flags %04x sub %04x ax %g pos (%g,%g) count %u extent %s[%g,%g] fn %08x\n", n,
                        type, static_cast<unsigned>(READ16(n + 2u)), static_cast<unsigned>(READ16(s + 2u)),
                        wideBitsToF(READ32(s + 0x20u)), wideBitsToF(READ32(s + 0x50u)), wideBitsToF(READ32(s + 0x54u)),
                        READ32(s + 0x8Cu), ext ? "" : "none", lo, hi, type == 2u ? READ32(n + 0xA0u) : 0u);
            if (ext && READ32(s + 0x8Cu) <= 8u && lines < g_hudTickMax)
            {
                // the vertices themselves (x, y; packet buffer entries of 0x30 bytes, x at +0x20, y at +0x24)
                const uint32_t buf = hudDrawBuf(rdram, ctx, runtime, s);
                ++lines;
                std::printf("[zoe:panel]     vertices");
                for (uint32_t v = 0; v < READ32(s + 0x8Cu); ++v)
                    std::printf(" (%g,%g)", wideBitsToF(READ32(buf + v * 0x30u + 0x20u)), wideBitsToF(READ32(buf + v * 0x30u + 0x24u)));
                std::printf("\n");
            }
            n = READ32(n + 0xCu);
        }
    }

    // IDENTITY-PARENT CONTAINERS (class 512). A window container without flag 0x40 has no matrix of its own: FUN_0045AA98 loads
    // the container matrix into VU0 vf28..31 only when the flag is set and otherwise runs VU0 0xE38 (vf28..31 = identity,
    // measured in the VU0 program of ws_ada_speak.p2s: mulx.yzw / addw.x / mr32), so its children are drawn at their own
    // coordinates and never reach zoeSpriteHud (they are drawn by FUN_0045A000, called from 0x45AB18). In the ADA cockpit the
    // voice indicators are such children: container 0x8D9390 (type 3, flags 0x0224, matrix exactly identity) holds the two
    // 66-vertex ring sprites (flags 0x0044, position (226, 323), radius 14..38.8 and 6..13.9: ADA's spectrogram and the inner
    // orb), and container 0x868660 (same flags) holds the left / right speaker orbs and level bars (positions (112, 276.2) and
    // (432, 276.2)) shown while Leo / Celvice speak. They stayed at their 4:3 coordinates while the console model, the boxes
    // and the dial (camera x scale 0.75, class 256 twins) moved to x' = 256 + 0.75 (x - 256). Rule: a container WITHOUT flag
    // 0x40 whose matrix is exactly the identity, every visible child inheriting (hudPanelInherits) and none of them
    // full-width (widest child extent, read from up to 512 vertices, and packet width, under 90 % of the screen: the fade /
    // scan-line layer 0x510DE0 / 0x510F00 has children spanning x 0..512 and stays untouched) is given the centre map as its
    // matrix and the flag 0x40 for the duration of the draw (both put back afterwards, like every other hudPut word).
    bool hudIdentityMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t c)
    {
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            const float want = ((i & 3u) == (i >> 2)) ? 1.0f : 0.0f;
            if (!(std::fabs(wideBitsToF(READ32(c + 0x20u + i * 4u)) - want) < 1e-4f))
                return false;
        }
        return true;
    }

    // Widest visible sprite child of container c (its x extent over up to 512 vertices times |axis x|, or its packet width
    // word times |axis x|), in the container's units.
    float hudWidestChild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t c)
    {
        float widest = 0.0f;
        const uint32_t sentinel = c + 0xA0u;
        uint32_t n = READ32(c + 0x8Cu);
        for (uint32_t guard = 0u; n != sentinel && guard <= 256u && wideCamOk(n); ++guard, n = READ32(n + 0xCu))
        {
            if ((READ16(n + 2u) & 2u) != 0u || READ8(n) != 1u)
                continue;
            const float ax = std::fabs(wideBitsToF(READ32(n + 0x20u)));
            float lo, hi;
            const bool ext = hudObjExtent(rdram, ctx, runtime, n, lo, hi, nullptr, nullptr, nullptr, 512u);
            if (ext)
                widest = std::max(widest, (hi - lo) * ax);
            const float pw = hudSpriteWidth(rdram, ctx, runtime, n);
            if (!ext && !(pw > 0.0f))
                return 1e9f; // a visible child that cannot be measured: treat as full-width (the container is left alone)
            widest = std::max(widest, pw * ax);
        }
        return widest;
    }

    void zoeContainerHud(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wideActiveNow(rdram, ctx, runtime))
        {
            g_origContainer(rdram, ctx, runtime);
            return;
        }
        wideCheckLoad(rdram, ctx, runtime);
        hudRestore(rdram, ctx, runtime);
        const uint32_t c = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        const bool own = wideCamOk(c) && (READ16(c + 2u) & 0x40u) != 0u;
        const bool idc = g_hudOn && !own && wideCamOk(c) && clsOn(512) && READ8(c) == 3u && hudIdentityMatrix(rdram, ctx, runtime, c);
        const AuditPiece aud = wideCamOk(c) ? auditBegin(rdram, ctx, runtime, c, 0u) : AuditPiece{}; // position row only
        const uint32_t auditSaved = g_hudSavedN;
        if (g_hudOn && wideCamOk(c) && !ps2MovieActive() && clsOn(128) && (own || idc))
        {
            const float px = wideBitsToF(READ32(c + 0x50u));
            uint32_t scissor = 0u, blame = 0u;
            float span = 0.0f;
            const bool inherits = hudPanelInherits(rdram, ctx, runtime, c, scissor, blame, span);
            if (idc)
                span = std::max(span, hudWidestChild(rdram, ctx, runtime, c)); // up to 512 vertices (the fade layer has 448)
            if (g_wideLog)
                hudPanelDump(rdram, ctx, runtime, c);
            // A container that covers (nearly) the whole screen is a backdrop / fade layer, not a panel: left alone, like
            // the full-width sprites (its widest sprite child, or its clip rectangle, at least 90 % of the screen wide).
            const float pax = std::fabs(wideBitsToF(READ32(c + 0x20u)));
            const bool fullScreen = span * pax >= g_hudFullWidth ||
                                    (scissor != 0u && wideBitsToF(READ32(scissor + 8u)) >= g_hudFullWidth);
            if (g_wideLog)
            {
                static uint32_t seenM[32];
                static uint32_t seenMN = 0u;
                bool seen = false;
                for (uint32_t i = 0; i < seenMN && !seen; ++i)
                    seen = (seenM[i] == c);
                if (!seen && seenMN < 32u)
                {
                    seenM[seenMN++] = c;
                    std::printf("[zoe:panel] container 0x%08x flags %04x pos (%g,%g) widest child %g clip %s inherits %d "
                                "full-screen %d\n",
                                c, static_cast<unsigned>(READ16(c + 2u)), px, wideBitsToF(READ32(c + 0x54u)), span * pax,
                                scissor != 0u ? "yes" : "no", inherits ? 1 : 0, fullScreen ? 1 : 0);
                }
            }
            // An identity-parent container is mapped only when every visible child inherits (never by PS2X_ZOE_WIDE_PANELS=2).
            if (idc ? (inherits && !fullScreen && scissor == 0u)
                    : ((inherits || g_panelMode >= 2u) && !fullScreen && px > -512.0f && px < 1024.0f))
            {
                // anchor by the panel centre (window rectangle), as the corner HUD; no rectangle: about the screen centre
                float b = 256.0f * (1.0f - kWideX);
                if (scissor != 0u)
                {
                    const float rx = wideBitsToF(READ32(scissor)), rw = wideBitsToF(READ32(scissor + 8u));
                    if (rw > 0.0f && rx > -512.0f && rx < 1024.0f)
                    {
                        const float pc = rx + 0.5f * rw;
                        b = (pc < g_hudZoneL) ? 0.0f : (pc >= g_hudZoneR) ? 512.0f * (1.0f - kWideX) : (1.0f - kWideX) * pc;
                    }
                }
                for (uint32_t r = 0u; r < 4u; ++r)
                {
                    const uint32_t a = c + 0x20u + r * 0x10u; // row r of P: x at a, w at a + 0xC
                    hudPut(rdram, ctx, runtime, a, kWideX * wideBitsToF(READ32(a)) + b * wideBitsToF(READ32(a + 0xCu)));
                }
                if (scissor != 0u)
                {
                    hudPut(rdram, ctx, runtime, scissor, kWideX * wideBitsToF(READ32(scissor)) + b);
                    hudPut(rdram, ctx, runtime, scissor + 8u, kWideX * wideBitsToF(READ32(scissor + 8u)));
                }
                if (idc) // the container now has "its own matrix" (the map above): children compose with it instead of identity
                    hudPutWord(rdram, ctx, runtime, c, READ32(c) | 0x00400000u);
                mapLog(c, idc ? 512u : 128u, kWideX * px + b, kWideX);
            }
            else if (g_wideLog && !inherits)
            {
                static uint32_t seenC[16];
                static uint32_t seenCN = 0u;
                bool seen = false;
                for (uint32_t i = 0; i < seenCN && !seen; ++i)
                    seen = (seenC[i] == c);
                if (!seen && seenCN < 16u)
                {
                    seenC[seenCN++] = c;
                    std::printf("[zoe:panel] container 0x%08x left as the game draws it: child 0x%08x (type %u flags %04x) does not "
                                "inherit its matrix\n",
                                c, blame, wideCamOk(blame) ? static_cast<unsigned>(READ8(blame)) : 99u,
                                wideCamOk(blame) ? static_cast<unsigned>(READ16(blame + 2u)) : 0u);
                }
            }
        }
        const uint32_t pending = g_hudSavedN;
        auditEnd(rdram, ctx, runtime, aud, 'C', c, pending != auditSaved ? 2u : ps2MovieActive() ? 7u : 5u);
        g_origContainer(rdram, ctx, runtime);
        if (g_hudSavedN != 0u && ctx->pc == ra)
            hudRestore(rdram, ctx, runtime); // else: interrupted, restored at the next wrapper entry
        else if (pending != 0u && ctx->pc != ra)
        {
            // Not expected (no yield inside a panel draw has been seen); counted so a run shows it if it happens.
            static uint32_t interrupted = 0u;
            if (++interrupted <= 3u)
                std::printf("[zoe:panel] container 0x%08x draw interrupted with the mapping pending (%u so far)\n", c, interrupted);
        }
    }

    void zoeMarkerWide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wideActiveNow(rdram, ctx, runtime))
        {
            g_origMarker(rdram, ctx, runtime);
            return;
        }
        hudRestore(rdram, ctx, runtime);
        const uint32_t o = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        if (wideCamOk(o) && !ps2MovieActive())
        {
            hudPut(rdram, ctx, runtime, o + kMarkerWidth, wideBitsToF(READ32(o + kMarkerWidth)) * kWideX);
            ++g_hudStat[4];
        }
        g_origMarker(rdram, ctx, runtime);
        if (g_hudSavedN != 0u && ctx->pc == ra)
            hudRestore(rdram, ctx, runtime);
    }

    void applyZoeWidescreen(PS2Runtime &runtime)
    {
        // PS2X_ZOE_WIDESCREEN: unset = use the saved setting (Graphics > Widescreen 16:9), 1 = start in widescreen (the
        // menu can still turn it off), 0 = force off: nothing is installed and the option is not offered. The wrappers
        // are installed whenever the game's functions exist; each one is a straight call to the original while the
        // setting is off.
        const char *env = std::getenv("PS2X_ZOE_WIDESCREEN");
        int envMode = -1; // -1 unset or ignored, 0 off + option hidden, 1 start on for this session (never saved)
        if (env != nullptr && env[0] != '\0')
        {
            if (std::strcmp(env, "0") == 0)
                envMode = 0;
            else if (std::strcmp(env, "1") == 0)
                envMode = 1;
            else
                std::printf("[zoe] PS2X_ZOE_WIDESCREEN=\"%s\" ignored (use 1 or 0)\n", env);
        }
        if (envMode == 0)
            return;
        // Everything is checked before anything is installed; one clear line says what was missing.
        static const uint32_t required[] = {kCameraMatrixFn, kCameraViewFn, kSizeFnA1, kSpriteFn, kMarkerFn};
        for (uint32_t addr : required)
        {
            if (!runtime.hasFunction(addr))
            {
                std::printf("[zoe] widescreen off: missing function 0x%08x\n", addr);
                return;
            }
        }
        if (runtime.hasFunction(kViewResumePc))
        {
            std::printf("[zoe] widescreen off: function-table slot 0x%08x (continuation) is already in use\n", kViewResumePc);
            return;
        }
        g_wideLog = std::getenv("PS2X_ZOE_WIDESCREEN_LOG") != nullptr;
        g_origCameraMatrix = runtime.lookupFunction(kCameraMatrixFn);
        g_origCameraView = runtime.lookupFunction(kCameraViewFn);
        g_origSizeA1 = runtime.lookupFunction(kSizeFnA1);
        g_origSprite = runtime.lookupFunction(kSpriteFn);
        g_origMarker = runtime.lookupFunction(kMarkerFn);
        const char *hud = std::getenv("PS2X_ZOE_WIDE_HUD");
        g_hudOn = !(hud != nullptr && hud[0] == '0');
        const char *gsx = std::getenv("PS2X_ZOE_WIDE_GSBLOCK"); // 0 = old block rule (0.5 % depth tolerance, [x, x + w] rectangles)
        g_gsExact = !(gsx != nullptr && gsx[0] == '0');
        const char *gsf = std::getenv("PS2X_ZOE_WIDE_GSFIXED"); // 0 = screen-fixed GS pieces never form blocks (bomb timer dot)
        g_gsFixed = !(gsf != nullptr && gsf[0] == '0');
        const char *ebx = std::getenv("PS2X_ZOE_WIDE_ENEMYBOX"); // 0 = enemy target boxes under the zone rule (steps at 215 / 297)
        g_enemyBox = !(ebx != nullptr && ebx[0] == '0') && runtime.hasFunction(kEnemyBoxFn);
        const char *clp = std::getenv("PS2X_ZOE_WIDE_CLIP"); // 0 = 2D-only screens not clipped to the 4:3 window
        g_hudClip = !(clp != nullptr && clp[0] == '0');
        const char *stp = std::getenv("PS2X_ZOE_WIDE_STRIP"); // 0 = multi-vertex strips can pass as full-width backdrops
        g_hudStripRule = !(stp != nullptr && stp[0] == '0');
        const char *cgr = std::getenv("PS2X_ZOE_WIDE_CENTREGROUP"); // 0 = groups lying on a centre-attached piece keep their own zone
        g_hudCentreGroups = !(cgr != nullptr && cgr[0] == '0');
        if (const char *au = std::getenv("PS2X_ZOE_WIDE_AUDIT")) // <csv path>,<tickLo>,<tickHi>: see auditBegin
        {
            const std::string spec(au);
            const size_t c2 = spec.rfind(','), c1 = c2 == std::string::npos ? c2 : spec.rfind(',', c2 - 1u);
            if (c1 != std::string::npos && c1 > 0u)
            {
                g_auditLo = std::strtoull(spec.c_str() + c1 + 1u, nullptr, 10);
                g_auditHi = std::strtoull(spec.c_str() + c2 + 1u, nullptr, 10);
                g_auditFile = std::fopen(spec.substr(0, c1).c_str(), "w");
            }
            if (g_auditFile != nullptr)
                std::fprintf(g_auditFile, "tick,frame,kind,obj,flags,stat,pure2d,px0,ax0,bx0,lo0,hi0,px1,ax1,bx1,lo1,hi1,py\n");
            std::printf("[zoe] widescreen audit %s (ticks %llu..%llu)\n", g_auditFile ? "on" : "FAILED to open its file",
                        static_cast<unsigned long long>(g_auditLo), static_cast<unsigned long long>(g_auditHi));
        }
        const char *marker = std::getenv("PS2X_ZOE_WIDE_MARKER");
        g_hudMarker = !(marker != nullptr && marker[0] == '0');
        if (const char *zones = std::getenv("PS2X_ZOE_WIDE_ZONES"))
        {
            float l = 0.0f, r = 0.0f;
            if (std::sscanf(zones, "%f,%f", &l, &r) == 2 && l < r)
            {
                g_hudZoneL = l;
                g_hudZoneR = r;
            }
        }
        if (const char *cm = std::getenv("PS2X_ZOE_WIDE_CLASSES"))
            g_classMask = static_cast<unsigned>(std::strtoul(cm, nullptr, 0));
        if (const char *hl = std::getenv("PS2X_ZOE_WIDE_HUDLOG")) // <skip>,<count> sprite-object calls (needs the log switch)
        {
            unsigned a = 0, b = 0;
            if (std::sscanf(hl, "%u,%u", &a, &b) == 2)
            {
                g_hudLogSkip = a;
                g_hudLogCount = b;
            }
        }
        if (const char *ht = std::getenv("PS2X_ZOE_WIDE_HUDTICKS")) // <tickLo>,<tickHi>[,<maxLines>] (needs the log switch)
        {
            unsigned long long lo = 0, hi = 0;
            unsigned mx = 400;
            if (std::sscanf(ht, "%llu,%llu,%u", &lo, &hi, &mx) >= 2)
            {
                g_hudTickLo = lo;
                g_hudTickHi = hi;
                g_hudTickMax = mx;
            }
        }
        runtime.replaceFunction(kCameraMatrixFn, &zoeCameraMatrixWide);
        runtime.replaceFunction(kCameraViewFn, &zoeCameraViewWide);
        runtime.replaceFunction(kViewResumePc, &zoeViewResume);
        runtime.replaceFunction(kSizeFnA1, &zoeSizeTagWide); // pass-through that tags marker objects
        runtime.replaceFunction(kSpriteFn, &zoeSpriteHud);
        if (g_enemyBox)
        {
            g_origEnemyBox = runtime.lookupFunction(kEnemyBoxFn);
            runtime.replaceFunction(kEnemyBoxFn, &zoeEnemyBoxTask); // pass-through that learns the unit task objects
        }
        if (runtime.hasFunction(kTextFn))
        {
            g_origText = runtime.lookupFunction(kTextFn);
            runtime.replaceFunction(kTextFn, &zoeTextObject);
        }
        if (const char *pn = std::getenv("PS2X_ZOE_WIDE_PANELS")) // 0 = off, 1 = panels whose children all inherit (default), 2 = all windows
        {
            if (pn[0] >= '0' && pn[0] <= '2' && pn[1] == '\0')
                g_panelMode = static_cast<unsigned>(pn[0] - '0');
            else
                std::printf("[zoe] PS2X_ZOE_WIDE_PANELS=\"%s\" ignored (use 0, 1 or 2)\n", pn);
        }
        bool panelsHooked = false;
        if (g_panelMode != 0u && runtime.hasFunction(kContainerFn))
        {
            g_origContainer = runtime.lookupFunction(kContainerFn);
            runtime.replaceFunction(kContainerFn, &zoeContainerHud);
            panelsHooked = true;
        }
        if (g_hudMarker)
            runtime.replaceFunction(kMarkerFn, &zoeMarkerWide);
        if (runtime.hasFunction(kSprite3dFn))
        {
            g_orig3d = runtime.lookupFunction(kSprite3dFn);
            runtime.replaceFunction(kSprite3dFn, &zoeSprite3dMark);
            g_have3dHook = true;
        }
        else
            std::printf("[zoe] widescreen: 0x%08x (3D object draw) missing, 2D-only screens get the HUD rule\n", kSprite3dFn);
        ps2SetGameDisplayAspect(16.0f / 9.0f);
        ps2SetGameWidescreenSupported(true);
        if (envMode == 1)
            ps2SetWidescreenSession(true);
        g_prevActive = ps2WidescreenActive();
        std::cout << "[zoe] PS2X_ZOE_WIDESCREEN: camera horizontal scale x0.75 while building (hooks 0x450c40, 0x450f58), HUD "
                  << (g_hudOn ? "on" : "off") << " zones " << g_hudZoneL << "/" << g_hudZoneR << ", marker width "
                  << (g_hudMarker ? "on" : "off") << ", menu panels " << (panelsHooked ? (g_panelMode >= 2u ? "all windows" : "on") : "off")
                  << ", world-attached blocks " << (g_gsExact ? "same-anchor" : "old rule") << ", enemy boxes "
                  << (g_enemyBox ? "own centre" : "zone rule") << ", window aspect 16:9" << std::endl;
    }

    // Versus unlock (menu Extras). The game's own title-screen code (Circle Cross Right Left Right Left Down Down Up Up,
    // table 0x49DCB0) ORs 3 into the session unlock word 0x4DFC9C (bit 0 = Versus in the title menu, bit 1 = all 7
    // frames and 11 stages, normally earned by a first and a second clear) and sets the title menu's entry count
    // (+0x5C14) from 3 to 4. With the option on, this pre-step does the same at every call of the title state machine
    // FUN_00414c88 (a0 = the title object, only caller 0x414674), then runs it unchanged. As with the code, the bits
    // last until the game restarts and the next memory card save stores them (docs/notes/02_zoe_internals.md).
    constexpr uint32_t kTitleStateFn = 0x00414C88u;
    constexpr uint32_t kUnlockSessionWord = 0x004DFC9Cu;
    constexpr uint32_t kUnlockAll = 3u;
    constexpr uint32_t kTitleMenuCount = 0x5C14u; // 3 entries, 4 with VERSUS
    PS2Runtime::RecompiledFunction g_origTitleState = nullptr;
    int g_versusOption = -1;

    void zoeTitleStateVersus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ps2GameOptionOn(g_versusOption))
        {
            WRITE32(kUnlockSessionWord, READ32(kUnlockSessionWord) | kUnlockAll);
            const uint32_t title = GPR_U32(ctx, 4);
            if (title >= 0x00100000u && title < PS2_RAM_SIZE - kTitleMenuCount - 4u && READ32(title + kTitleMenuCount) == 3u)
                WRITE32(title + kTitleMenuCount, 4u);
        }
        g_origTitleState(rdram, ctx, runtime);
    }

    void applyZoeVersusUnlock(PS2Runtime &runtime)
    {
        // PS2X_ZOE_VERSUS_UNLOCK: unset = the saved setting (Extras > Unlock Versus mode), 1 = on for this session
        // (never saved), 0 = off: nothing is installed and the option is not offered.
        const char *env = std::getenv("PS2X_ZOE_VERSUS_UNLOCK");
        if (env != nullptr && std::strcmp(env, "0") == 0)
            return;
        if (!runtime.hasFunction(kTitleStateFn))
        {
            std::printf("[zoe] versus unlock off: missing function 0x%08x\n", kTitleStateFn);
            return;
        }
        g_origTitleState = runtime.lookupFunction(kTitleStateFn);
        runtime.replaceFunction(kTitleStateFn, &zoeTitleStateVersus);
        g_versusOption = ps2RegisterGameOption("versus_unlock", "Unlock Versus mode",
                                               "Versus mode with all frames and stages from the start, like the game's own "
                                               "title-screen code. A memory card save made while it is on may keep the "
                                               "unlock. Turning it off takes effect after a restart.");
        if (env != nullptr && std::strcmp(env, "1") == 0)
            ps2SetGameOptionSession(g_versusOption, true);
        else if (env != nullptr && env[0] != '\0')
            std::printf("[zoe] PS2X_ZOE_VERSUS_UNLOCK=\"%s\" ignored (use 1 or 0)\n", env);
    }

    void applyZoe(PS2Runtime &runtime)
    {
        applyZoeWidescreen(runtime);
        applyZoeVersusUnlock(runtime);
        if (std::getenv("PS2X_ZOE_MOTION_LOG") != nullptr && runtime.hasFunction(kMotionRelocFn))
        {
            g_origMotionReloc = runtime.lookupFunction(kMotionRelocFn);
            runtime.replaceFunction(kMotionRelocFn, &zoeMotionRelocLog);
        }
        if (runtime.hasFunction(kIsceSifSendCmd))
            runtime.replaceFunction(kIsceSifSendCmd, &zoeIsceSifSendCmd);
        if (!runtime.hasFunction(kInitFn) || !runtime.hasFunction(kRegisterFn))
        {
            std::cerr << "[zoe] stage-stub resolver NOT installed: FUN_0043c8e8 / FUN_0043bb90 are not in the function table"
                      << std::endl;
            return;
        }
        ps2SetCodeResolver(&resolveZoeCode);
        std::cout << "[zoe] overrides applied (stage-stub resolver installed)" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Zone of the Enders (USA)", "SLUS_201.48", 0x00200008u, 0u, applyZoe)
