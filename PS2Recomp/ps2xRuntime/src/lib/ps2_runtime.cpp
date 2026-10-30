#include "ps2_debug_trace.h"
#include "ps2_host_settings.h"
#include "ps2_frame_log.h"
#include "ps2_input.h"
#include "ps2_profiler.h"
#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_code_resolver.h"
#include "runtime/ps2_vu_fast.h"
#include "runtime/ps2_vu_bench.h"
#include "ps2_game_clock.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "runtime/ps2_gs_worker.h"
#include "ps2_iop_thread.h"
#include "ps2_savestate.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/iop/spu2_debug.h"
#include "rlgl.h"

namespace
{
    void runVu1Program(VU1Interpreter &interp, PS2Memory &memory, GS &gs, bool resume, uint32_t startPC, uint32_t top,
                       uint32_t itop);
    bool vu1FastEnabled();
}

namespace
{
    void stopSpu2AudioStream(); // defined with the SPU2 output stream below
}

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

// Name of the resident code overlay, for diagnostics (read by the pacing stats).
std::atomic<const char *> g_ps2ActiveOverlayName{"MAIN"};

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
// 4:3, like the TV picture the PS2 produced.
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 720;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// PS2X_PRESENT=flip: set when the game flipped to a new frame, so the next UploadFrame latches it.
static bool g_presentFlipPending = false;

static bool presentOnFlip()
{
    static const bool on = []
    {
        const char *v = std::getenv("PS2X_PRESENT");
        return v != nullptr && std::strcmp(v, "flip") == 0;
    }();
    return on;
}

// PS2X_PRESENT=flip: each window pass waits for the game's next flip (ps2_gs_worker::flipCount: the frame is complete
// and became the display buffer) and shows it, so every frame is shown once and as evenly as the game makes them.
// The default polls the vsync counter on raylib's own 60 fps timer; the two clocks drift and jitter against each
// other, so ~1-2 frames a second are never shown and others shown twice. If no flip is noticed for 250 ms (a game
// that changes the display another way) it follows the vsync counter; it never waits more than ~2 frames.
static void waitForNextFrame(PS2Runtime *rt)
{
    static uint64_t s_seen = ps2_gs_worker::flipCount();
    static auto s_lastFlip = std::chrono::steady_clock::now();
    static uint64_t s_lastTick = 0;
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        const uint64_t flips = ps2_gs_worker::waitForFlip(s_seen, 2u);
        const auto now = std::chrono::steady_clock::now();
        const uint64_t tick = rt->eeScheduler().currentVSyncTick();
        if (flips != s_seen)
        {
            s_seen = flips;
            s_lastFlip = now;
            s_lastTick = tick;
            g_presentFlipPending = true;
            return;
        }
        if (now - s_lastFlip > std::chrono::milliseconds(250) && tick != s_lastTick)
        {
            s_lastTick = tick;
            return;
        }
        if (now - start > std::chrono::milliseconds(34) || rt->isStopRequested())
            return;
    }
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    // Poll every host frame: the guest may not present a new picture this frame.
    static bool s_diagRequested = false;
    if (IsKeyPressed(KEY_F9))
        s_diagRequested = true;
    // Save states: F5 save, F8 load, F6/F7 previous/next slot (0-9).
    if (IsKeyPressed(KEY_F5))
        ps2_state::requestSave(ps2_state::slot());
    if (IsKeyPressed(KEY_F8))
        ps2_state::requestLoad(ps2_state::slot());
    if (IsKeyPressed(KEY_F6))
        ps2_state::stepSlot(-1);
    if (IsKeyPressed(KEY_F7))
        ps2_state::stepSlot(+1);
    // PS2X_PROFILE_WINDOW="start,stop" (seconds after first frame) toggles the profiler unattended.
    static double s_profileStart = -1.0, s_profileStop = -1.0;
    static int s_profileWindowState = 0;
    static const auto s_hostStart = std::chrono::steady_clock::now();
    if (s_profileWindowState == 0)
    {
        s_profileWindowState = 1;
        if (const char *w = std::getenv("PS2X_PROFILE_WINDOW"))
            std::sscanf(w, "%lf,%lf", &s_profileStart, &s_profileStop);
    }
    const double hostSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_hostStart).count();
    // PS2X_DIAG_AT="45,60.5,t6120": press F9 automatically at these host seconds, or vsync ticks with a 't' prefix
    // (unattended captures; ticks are reproducible with PS2X_INPUT_CLOCK=ticks routes).
    static std::vector<double> s_diagAt = []
    {
        std::vector<double> times;
        if (const char *env = std::getenv("PS2X_DIAG_AT"))
        {
            std::stringstream ss(env);
            std::string item;
            while (std::getline(ss, item, ','))
                if (!item.empty() && item[0] != 't' && item[0] != 'T')
                    times.push_back(std::atof(item.c_str()));
        }
        return times;
    }();
    static std::vector<double> s_diagAtTick = []
    {
        std::vector<double> ticks;
        if (const char *env = std::getenv("PS2X_DIAG_AT"))
        {
            std::stringstream ss(env);
            std::string item;
            while (std::getline(ss, item, ','))
                if (!item.empty() && (item[0] == 't' || item[0] == 'T'))
                    ticks.push_back(std::atof(item.c_str() + 1));
        }
        return ticks;
    }();
    for (double &t : s_diagAtTick)
    {
        if (t >= 0.0 && static_cast<double>(ps2CurrentVsyncTick()) >= t)
        {
            s_diagRequested = true;
            t = -1.0;
        }
    }
    // PS2X_WATCH="61ad98,61acd0" (hex guest addresses, up to 4), armed from PS2X_WATCH_AFTER host seconds and
    // re-armed every 3 s so threads created later are covered.
    {
        static std::vector<uint32_t> s_watch = []
        {
            std::vector<uint32_t> addrs;
            if (const char *env = std::getenv("PS2X_WATCH"))
            {
                std::stringstream ss(env);
                std::string item;
                while (std::getline(ss, item, ','))
                    addrs.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
            }
            return addrs;
        }();
        static const double s_watchAfter = std::getenv("PS2X_WATCH_AFTER") ? std::atof(std::getenv("PS2X_WATCH_AFTER")) : 0.0;
        static double s_watchNext = -1.0;
        if (!s_watch.empty() && hostSeconds >= s_watchAfter && hostSeconds >= s_watchNext)
        {
            if (s_watchNext < 0.0)
                std::cout << "[watch] armed at " << hostSeconds << "s" << std::endl;
            ps2WatchArm(rt->memory().getRDRAM(), s_watch.data(), static_cast<uint32_t>(s_watch.size()));
            s_watchNext = hostSeconds + 3.0;
        }
    }
    for (double &t : s_diagAt)
    {
        if (t >= 0.0 && hostSeconds >= t)
        {
            s_diagRequested = true;
            t = -1.0;
        }
    }
    const bool windowEdge = (s_profileWindowState == 1 && s_profileStart >= 0.0 && hostSeconds >= s_profileStart) ||
                            (s_profileWindowState == 2 && hostSeconds >= s_profileStop);
    if (windowEdge)
        ++s_profileWindowState;
    if (IsKeyPressed(KEY_F10) || windowEdge)
    {
        const char *diagDir = std::getenv("PS2X_DIAG_DIR");
        const bool started = ps2ProfilerToggle(diagDir != nullptr ? diagDir : ".");
        std::cout << "[profile] " << (started ? "started" : "stopped, report written") << std::endl;
    }

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    ps2NoteVsyncTick(currentTick);
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick || g_presentFlipPending;
    g_presentFlipPending = false;
    // PS2X_PRESENT_LOG=<csv> (diagnostic): one line per window-loop pass: host time, guest vsync tick, whether a new
    // frame was latched and how long latching took. Guest frames never shown = tick steps of 2+ between latches.
    static FILE *s_presentLog = []() -> FILE *
    {
        const char *p = std::getenv("PS2X_PRESENT_LOG");
        FILE *f = (p != nullptr && *p != 0) ? std::fopen(p, "w") : nullptr;
        if (f != nullptr)
            std::fprintf(f, "ms,tick,latched,latchMs,flips\n");
        return f;
    }();
    static const auto s_presentLogStart = std::chrono::steady_clock::now();
    const auto latchStart = std::chrono::steady_clock::now();
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    if (s_presentLog != nullptr)
    {
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(s_presentLog, "%.3f,%llu,%d,%.3f,%llu\n",
                     std::chrono::duration<double, std::milli>(latchStart - s_presentLogStart).count(),
                     static_cast<unsigned long long>(currentTick), needsLatch ? 1 : 0,
                     std::chrono::duration<double, std::milli>(now - latchStart).count(),
                     static_cast<unsigned long long>(ps2_gs_worker::flipCount()));
        static unsigned s_lines = 0;
        if (++s_lines % 120u == 0u)
            std::fflush(s_presentLog);
    }
    if (!needsLatch && s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    // The GPU backend already drew the frame into tex while latching; only read it back for diagnostics.
    const bool gpuPresented = GSGpuBackend::Active();
    static const bool s_wantsReadback = std::getenv("PS2X_FRAME_STATS") != nullptr || std::getenv("PS2X_DUMP_FRAMES") != nullptr;
    if (gpuPresented)
    {
        GSGpuBackend::GetPresentedSize(width, height);
        if (s_diagRequested || s_wantsReadback)
        {
            Image full = LoadImageFromTexture(tex);
            if (full.data != nullptr && full.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            {
                const uint32_t w = std::min<uint32_t>(width, static_cast<uint32_t>(full.width));
                const uint32_t h = std::min<uint32_t>(height, static_cast<uint32_t>(full.height));
                s_scratch.resize(static_cast<size_t>(w) * h * 4u);
                for (uint32_t y = 0; y < h; ++y)
                    std::memcpy(s_scratch.data() + static_cast<size_t>(y) * w * 4u,
                                static_cast<const uint8_t *>(full.data) + static_cast<size_t>(y) * full.width * 4u, w * 4u);
                width = w;
                height = h;
            }
            UnloadImage(full);
        }
    }
    else if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    if (!gpuPresented)
        std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!gpuPresented && !s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    // Debug aid: F9 traces every GS/VIF event for the next few frames and saves this frame.
    if (s_diagRequested && width != 0u && height != 0u)
    {
        s_diagRequested = false;
        const char *diagDir = std::getenv("PS2X_DIAG_DIR");
        const std::string path = std::string(diagDir != nullptr ? diagDir : ".") + "/diag_" + std::to_string(currentTick) + ".png";
        Image image{};
        image.data = s_scratch.data();
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        ExportImage(image, path.c_str());
        std::cout << "[diag] F9 capture tick=" << currentTick << " screenshot=" << path
                  << " dispfbp=" << displayFbp << " size=" << width << "x" << height << std::endl;
        {
            const uint8_t *ram = rt->memory().getRDRAM();
            {
                const std::string ramPath = std::string(diagDir != nullptr ? diagDir : ".") + "/ram_" + std::to_string(currentTick) + ".bin";
                if (FILE *ramFile = std::fopen(ramPath.c_str(), "wb"))
                {
                    std::fwrite(ram, 1, PS2_RAM_SIZE, ramFile);
                    std::fclose(ramFile);
                    std::cout << "[diag] RAM dump " << ramPath << std::endl;
                }
                // GS local memory (4 MB, the CPU copy: textures, CLUTs; GPU-rendered targets may be stale here).
                {
                    const std::string gsPath = std::string(diagDir != nullptr ? diagDir : ".") + "/gsmem_" + std::to_string(currentTick) + ".bin";
                    if (FILE *gsFile = std::fopen(gsPath.c_str(), "wb"))
                    {
                        std::fwrite(rt->memory().getGSVRAM(), 1, PS2_GS_VRAM_SIZE, gsFile);
                        std::fclose(gsFile);
                        std::cout << "[diag] GS memory dump " << gsPath << std::endl;
                    }
                }
                // VU1 micro memory (16 KB) and data memory (16 KB), for checking what the game's MPG/UNPACK uploads left there.
                {
                    const std::string vuBase = std::string(diagDir != nullptr ? diagDir : ".") + "/vu1_";
                    if (FILE *vuFile = std::fopen((vuBase + "code_" + std::to_string(currentTick) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(rt->memory().getVU1Code(), 1, PS2_VU1_CODE_SIZE, vuFile);
                        std::fclose(vuFile);
                    }
                    if (FILE *vuFile = std::fopen((vuBase + "data_" + std::to_string(currentTick) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(rt->memory().getVU1Data(), 1, PS2_VU1_DATA_SIZE, vuFile);
                        std::fclose(vuFile);
                    }
                    // VU0 micro memory and data memory (4 KB each), for games that run VCALLMS micro-programs.
                    const std::string vu0Base = std::string(diagDir != nullptr ? diagDir : ".") + "/vu0_";
                    if (FILE *vuFile = std::fopen((vu0Base + "code_" + std::to_string(currentTick) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(rt->memory().getVU0Code(), 1, PS2_VU0_CODE_SIZE, vuFile);
                        std::fclose(vuFile);
                    }
                    if (FILE *vuFile = std::fopen((vu0Base + "data_" + std::to_string(currentTick) + ".bin").c_str(), "wb"))
                    {
                        std::fwrite(rt->memory().getVU0Data(), 1, PS2_VU0_DATA_SIZE, vuFile);
                        std::fclose(vuFile);
                    }
                }
                const std::string spuPath = std::string(diagDir != nullptr ? diagDir : ".") + "/spu2ram_" + std::to_string(currentTick) + ".bin";
                if (ps2x::iop::dumpSpu2Ram(spuPath))
                    std::cout << "[diag] SPU2 RAM dump " << spuPath << std::endl;
            }
            // Game-specific RAM probes (camera matrices and the like) belong in a game override, not here:
            // the RAM dump above has everything, and PS2X_WATCH follows single addresses live.
            const R5900Context &vu0 = rt->eeScheduler().sharedVu0State();
            for (int r = 24; r < 32; ++r)
            {
                float f[4];
                std::memcpy(f, &vu0.vu0_vf[r], sizeof(f));
                std::cout << "[diag] vf" << r << " = " << f[0] << "," << f[1] << "," << f[2] << "," << f[3] << std::endl;
            }
        }
        static const bool s_noTrace = std::getenv("PS2X_DIAG_NOTRACE") != nullptr; // RAM/VU dumps only, no trace burst
        if (!s_noTrace)
            ps2_debug::g_traceFramesRemaining.store(3, std::memory_order_relaxed);
    }
    else if (ps2_debug::traceBurst() && ps2_debug::g_traceFramesRemaining.fetch_sub(1, std::memory_order_relaxed) == 1)
    {
        std::cout << "[diag] burst trace finished tick=" << currentTick << std::endl;
    }

    // Debug aid: PS2X_DUMP_FRAMES=<dir> saves roughly one presented frame per second.
    static const char *s_dumpDir = std::getenv("PS2X_DUMP_FRAMES");
    static uint32_t s_dumpCounter = 0u;
    static const bool s_frameStats = std::getenv("PS2X_FRAME_STATS") != nullptr;
    if (s_frameStats && width != 0u && height != 0u)
    {
        uint64_t sum = 0;
        for (size_t i = 0; i + 3 < s_scratch.size(); i += 4)
            sum += s_scratch[i] + s_scratch[i + 1] + s_scratch[i + 2];
        std::cout << "[frame:stat] tick=" << currentTick << " fbp=" << displayFbp << " src=" << sourceFbp
                  << " avg=" << (sum / std::max<size_t>(1, s_scratch.size() / 4)) << std::endl;
    }
    // PS2X_DUMP_BURST=<tick>,<count>: also dump every presented frame in that window (to measure frame-to-frame shake).
    static unsigned long long s_burstStart = 0u, s_burstCount = 0u;
    static const bool s_burstParsed = [] {
        if (const char *b = std::getenv("PS2X_DUMP_BURST"))
            std::sscanf(b, "%llu,%llu", &s_burstStart, &s_burstCount);
        return true;
    }();
    (void)s_burstParsed;
    const bool inBurst = s_burstCount != 0u && currentTick >= s_burstStart && currentTick < s_burstStart + s_burstCount;
    if (s_dumpDir != nullptr && s_dumpDir[0] != '\0' && ((s_dumpCounter++ % 60u) == 0u || inBurst) && width != 0u && height != 0u)
    {
        size_t lit = 0;
        for (size_t i = 0; i + 3 < s_scratch.size(); i += 4)
        {
            lit += (s_scratch[i] | s_scratch[i + 1] | s_scratch[i + 2]) != 0 ? 1u : 0u;
        }
        Image image{};
        image.data = s_scratch.data();
        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        const std::string path = std::string(s_dumpDir) + "/frame_" + std::to_string(currentTick) + ".png";
        ExportImage(image, path.c_str());
        std::cout << "[frame:dump] " << path << " fbp=" << displayFbp << " " << width << "x" << height
                  << " lit=" << lit << std::endl;
    }

    if (!gpuPresented)
        UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            stopSpu2AudioStream();
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

namespace
{
    // The IOP call lock (see ps2_iop_thread.h); a no-op wrapper before the host exists.
    std::unique_lock<std::recursive_mutex> lockIopHost(PS2IopHostAdapter *host)
    {
        return host ? host->lockIop() : std::unique_lock<std::recursive_mutex>();
    }
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    if (ps2_iop_thread::enabled())
    {
        // Game thread: skip the time the IOP thread already ran during an idle wait.
        if (!ps2_iop_thread::onIopThread() && (eeCycles = ps2_iop_thread::consumePrepaid(eeCycles)) == 0u)
            return;
        auto lock = lockIopHost(m_iopHost.get());
        m_iopSubsystem->runEeCycles(eeCycles);
        return;
    }
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    auto lock = lockIopHost(m_iopHost.get());
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    auto lock = lockIopHost(m_iopHost.get());
    const bool ok = m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
    if (!ok)
        std::printf("[iop] rejected write 0x%x..0x%llx (0x%zx bytes)\n", address,
                    static_cast<unsigned long long>(address) + size, size);
    return ok;
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    auto lock = lockIopHost(m_iopHost.get());
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (!ps2_gs_worker::captureGifPacket(data, size)) // PS2X_GS_THREAD=2
                                            m_gs.processGIFPacket(data, size); });
    ps2_gs_worker::setDrawTarget([](void *gs, const uint8_t *data, uint32_t size)
                                 { static_cast<GS *>(gs)->processGIFPacket(data, size); },
                                 &m_gs);
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     if (m_vu1NativeProgram != nullptr && runVu1Native(startPC, top, itop))
                                     {
                                         cpuContext->vu0_vpu_stat &= ~0x0600u;
                                         return;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     runVu1Program(m_vu1, m_memory, m_gs, false, startPC, top, itop);
                                     // With the render worker this runs off the game thread: only write VPU_STAT when the
                                     // D/T stop bits change, so the usual no-op never races the game's own updates.
                                     const uint32_t stopBits = (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                               (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                     if ((cpuContext->vu0_vpu_stat & 0x0600u) != stopBits)
                                         cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stopBits; });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     runVu1Program(m_vu1, m_memory, m_gs, true, 0u, top, itop);
                                     // With the render worker this runs off the game thread: only write VPU_STAT when the
                                     // D/T stop bits change, so the usual no-op never races the game's own updates.
                                     const uint32_t stopBits = (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                               (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                     if ((cpuContext->vu0_vpu_stat & 0x0600u) != stopBits)
                                         cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stopBits; });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

namespace
{
    // Live SPU2 output: raylib pulls 48 kHz stereo frames from the emulated SPU2 on its audio thread.
    AudioStream s_spu2Stream{};
    bool s_spu2StreamActive = false;

    void spu2AudioCallback(void *buffer, unsigned int frames)
    {
        ps2x::iop::spu2ReadOutput(static_cast<int16_t *>(buffer), frames);
    }

    void startSpu2AudioStream()
    {
        const char *setting = std::getenv("PS2X_AUDIO");
        if (!IsAudioDeviceReady() || (setting != nullptr && std::strcmp(setting, "0") == 0))
            return;
        SetAudioStreamBufferSizeDefault(512); // ~11 ms device chunks
        s_spu2Stream = LoadAudioStream(48000, 16, 2);
        SetAudioStreamCallback(s_spu2Stream, spu2AudioCallback);
        PlayAudioStream(s_spu2Stream);
        s_spu2StreamActive = true;
        std::cout << "[audio] SPU2 output stream started (48 kHz stereo)" << std::endl;
    }

    void stopSpu2AudioStream()
    {
        if (!s_spu2StreamActive)
            return;
        StopAudioStream(s_spu2Stream);
        UnloadAudioStream(s_spu2Stream);
        s_spu2StreamActive = false;
    }
}

bool PS2Runtime::initialize(const char *title)
{
    ps2InstallCrashReporter();
    ps2_vu_bench::runBenchIfRequested(); // PS2X_VU_BENCH: offline VU replay, exits
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // PS2X_HIDE_WINDOW=1: unattended test runs (frame dumps, routes) without a window on the desktop.
        // PS2X_WINDOW_UNFOCUSED=1: the window opens visible but does not take the keyboard focus (test runs beside a
        // game someone is playing).
        const char *hideWindow = std::getenv("PS2X_HIDE_WINDOW");
        const char *unfocused = std::getenv("PS2X_WINDOW_UNFOCUSED");
        SetConfigFlags(FLAG_WINDOW_RESIZABLE |
                       ((hideWindow != nullptr && *hideWindow != 0 && *hideWindow != '0') ? FLAG_WINDOW_HIDDEN : 0u) |
                       ((unfocused != nullptr && *unfocused != 0 && *unfocused != '0') ? FLAG_WINDOW_UNFOCUSED : 0u));
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        startSpu2AudioStream();
#endif
        SetTargetFPS(60);
        // Esc opens the in-game menu; don't let raylib treat it as "close window".
        SetExitKey(KEY_NULL);
        ps2InputInit();
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);
    ps2_state::attach(*this, std::filesystem::path(elfPath).filename().string());

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::submitVu1Xgkick(const uint8_t *vuData, uint32_t qwordAddress)
{
    if (m_vu1XgkickCaptureOnly || vuData == nullptr)
        return;

    // Walk GIF tags from the kick address until EOP, copying with VU memory wraparound.
    static thread_local std::vector<uint8_t> packet;
    packet.clear();
    uint32_t qword = qwordAddress & 0x3FFu;
    auto copyQword = [&]()
    {
        packet.insert(packet.end(), vuData + qword * 16u, vuData + qword * 16u + 16u);
        qword = (qword + 1u) & 0x3FFu;
    };
    for (uint32_t tags = 0; tags < 1024u && packet.size() < PS2_VU1_DATA_SIZE; ++tags)
    {
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, vuData + qword * 16u, sizeof(tagLo));
        copyQword();
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        uint32_t dataQwords = 0;
        if (format == 0u)
            dataQwords = nloop * nreg;
        else if (format == 1u)
            dataQwords = (nloop * nreg + 1u) / 2u;
        else
            dataQwords = nloop;
        for (uint32_t i = 0; i < dataQwords && packet.size() < PS2_VU1_DATA_SIZE; ++i)
            copyQword();
        if ((tagLo >> 15) & 1u)
            break;
    }
    m_memory.submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
}

namespace
{
    // VU1 program execution: the fast engine (runtime/ps2_vu_fast.h) or the reference interpreter.
    // PS2X_VU_FAST=0 forces the interpreter. PS2X_VU_VERIFY=1 runs the fast engine on a copy of the VU state and data
    // (packets captured, not emitted), then the interpreter for real, and reports the first differences.
    VuFastEngine &vu1FastEngine()
    {
        static VuFastEngine engine;
        return engine;
    }

    bool vu1FastEnabled()
    {
        static const bool enabled = []
        {
            // On by default: verified against the interpreter (PS2X_VU_VERIFY=1) on 1.16 million ZOE programs with
            // zero differences. PS2X_VU_FAST=0 falls back to the interpreter.
            const char *env = std::getenv("PS2X_VU_FAST");
            return !(env != nullptr && std::strcmp(env, "0") == 0);
        }();
        return enabled;
    }

    void compareVu1Results(const VU1State &fast, const uint8_t *fastData, const VU1State &ref, const uint8_t *refData,
                           uint32_t startPC, bool resume)
    {
        static uint64_t s_runs = 0, s_bad = 0;
        static uint32_t s_logged = 0;
        ++s_runs;
        std::string diff;
        char line[256];
        auto note = [&](const char *what, uint32_t index, uint32_t a, uint32_t b)
        {
            if (diff.size() < 900u)
            {
                std::snprintf(line, sizeof(line), " %s[%u] fast=%08x ref=%08x;", what, index, a, b);
                diff += line;
            }
        };
        for (uint32_t r = 1; r < 32u; ++r)
            for (uint32_t c = 0; c < 4u; ++c)
            {
                uint32_t a, b;
                std::memcpy(&a, &fast.vf[r][c], 4);
                std::memcpy(&b, &ref.vf[r][c], 4);
                if (a != b)
                    note("vf", r * 4u + c, a, b);
            }
        for (uint32_t r = 1; r < 16u; ++r)
            if ((fast.vi[r] & 0xFFFF) != (ref.vi[r] & 0xFFFF))
                note("vi", r, static_cast<uint32_t>(fast.vi[r]), static_cast<uint32_t>(ref.vi[r]));
        for (uint32_t c = 0; c < 4u; ++c)
        {
            uint32_t a, b;
            std::memcpy(&a, &fast.acc[c], 4);
            std::memcpy(&b, &ref.acc[c], 4);
            if (a != b)
                note("acc", c, a, b);
        }
        auto cmpf = [&](const char *what, float fa, float fb)
        {
            uint32_t a, b;
            std::memcpy(&a, &fa, 4);
            std::memcpy(&b, &fb, 4);
            if (a != b)
                note(what, 0, a, b);
        };
        cmpf("q", fast.q, ref.q);
        cmpf("p", fast.p, ref.p);
        cmpf("i", fast.i, ref.i);
        if (fast.clip != ref.clip)
            note("clip", 0, fast.clip, ref.clip);
        if (fast.pc != ref.pc)
            note("pc", 0, fast.pc, ref.pc);
        uint32_t memDiff = 0, firstQw = 0;
        for (uint32_t q = 0; q < PS2_VU1_DATA_SIZE / 16u; ++q)
            if (std::memcmp(fastData + q * 16u, refData + q * 16u, 16u) != 0)
            {
                if (memDiff++ == 0u)
                    firstQw = q;
            }
        if (memDiff != 0u)
        {
            uint32_t a[4], b[4];
            std::memcpy(a, fastData + firstQw * 16u, 16);
            std::memcpy(b, refData + firstQw * 16u, 16);
            std::snprintf(line, sizeof(line), " mem: %u qwords differ, first qw 0x%x fast=%08x,%08x,%08x,%08x ref=%08x,%08x,%08x,%08x;",
                          memDiff, firstQw, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
            diff += line;
        }
        if (!diff.empty())
        {
            ++s_bad;
            if (s_logged < 40u)
            {
                ++s_logged;
                std::cerr << "[vu1:verify] " << (resume ? "MSCNT" : "MSCAL") << " pc=0x" << std::hex << startPC << std::dec << diff
                          << std::endl;
            }
        }
        if ((s_runs % 20000u) == 0u)
            std::cerr << "[vu1:verify] runs=" << s_runs << " mismatching=" << s_bad << std::endl;
    }

    // ZOE effects investigation: PS2X_VU1_TRACE=<pc hex>[/<pc hex>...],<firstTick>,<lastTick>[,max] prints the
    // VU1 runs starting (MSCAL) or resuming (MSCNT) at those PCs: TOP, a few VU data words, end PC, cycles and the
    // XGKICK packets (ps2_vu_fast.cpp). Fast engine only.
    struct Vu1Trace
    {
        uint32_t pcs[8] = {};
        uint32_t count = 0;
        uint64_t first = 0, last = 0;
        uint32_t remaining = 300;
        Vu1Trace()
        {
            const char *env = std::getenv("PS2X_VU1_TRACE");
            if (!env)
                return;
            std::string v(env);
            const size_t c1 = v.find(',');
            if (c1 == std::string::npos)
                return;
            std::string list = v.substr(0, c1);
            unsigned long long f = 0, l = 0;
            unsigned m = 300;
            const int n = std::sscanf(v.c_str() + c1 + 1, "%llu,%llu,%u", &f, &l, &m);
            if (n < 2)
                return;
            first = f;
            last = l;
            remaining = m;
            size_t pos = 0;
            while (pos < list.size() && count < 8u)
            {
                const size_t sl = list.find('/', pos);
                pcs[count++] = static_cast<uint32_t>(std::strtoul(list.substr(pos, sl - pos).c_str(), nullptr, 16));
                if (sl == std::string::npos)
                    break;
                pos = sl + 1;
            }
        }
        bool wants(uint32_t pc) const
        {
            if (remaining == 0u)
                return false;
            const uint64_t t = ps2CurrentVsyncTick();
            if (t < first || t > last)
                return false;
            for (uint32_t i = 0; i < count; ++i)
                if (pcs[i] == pc)
                    return true;
            return false;
        }
    };
    Vu1Trace &vu1Trace()
    {
        static Vu1Trace t;
        return t;
    }
}
extern uint32_t g_vuFastKicks;
extern bool g_vuFastTraceKicks;
namespace
{
    void vu1TraceWords(const char *name, const uint8_t *data, uint32_t qword)
    {
        uint32_t w[4];
        std::memcpy(w, data + (qword & 0x3FFu) * 16u, 16);
        float f[4];
        std::memcpy(f, w, 16);
        std::fprintf(stderr, " %s@%03x=[%08x %08x %08x %08x | %g %g %g %g]", name, qword & 0x3FFu, w[0], w[1], w[2], w[3], f[0],
                     f[1], f[2], f[3]);
    }

    void runVu1Program(VU1Interpreter &interp, PS2Memory &memory, GS &gs, bool resume, uint32_t startPC, uint32_t top,
                       uint32_t itop)
    {
        static Vu1Trace &s_trace = vu1Trace();
        const uint32_t tracePc = resume ? interp.state().pc : startPC;
        const bool traced = s_trace.count != 0u && s_trace.wants(tracePc);
        uint32_t kicksBefore = 0;
        uint64_t cyclesBefore = 0;
        if (traced)
        {
            --s_trace.remaining;
            const uint8_t *d = memory.getVU1Data();
            std::fprintf(stderr, "[vu1trace] t=%llu %s pc=%04x top=%03x itop=%03x",
                         static_cast<unsigned long long>(ps2CurrentVsyncTick()), resume ? "MSCNT" : "MSCAL", tracePc, top, itop);
            vu1TraceWords("hdr", d, top + 2u);
            vu1TraceWords("m52", d, 52u);
            vu1TraceWords("m62", d, 62u);
            std::fprintf(stderr, "\n");
            kicksBefore = g_vuFastKicks;
            cyclesBefore = interp.state().cycles;
            g_vuFastTraceKicks = true;
        }
        struct TraceEnd
        {
            bool on;
            VU1Interpreter &interp;
            uint32_t kicksBefore;
            uint64_t cyclesBefore;
            ~TraceEnd()
            {
                if (!on)
                    return;
                g_vuFastTraceKicks = false;
                std::fprintf(stderr, "    [end] pc=%04x cycles=%llu kicks=%u vi8=%d vi12=%d vi13=%d\n", interp.state().pc,
                             static_cast<unsigned long long>(interp.state().cycles - cyclesBefore), g_vuFastKicks - kicksBefore,
                             interp.state().vi[8], interp.state().vi[12], interp.state().vi[13]);
            }
        } traceEnd{traced, interp, kicksBefore, cyclesBefore};

        uint8_t *code = memory.getVU1Code();
        uint8_t *data = memory.getVU1Data();
        if (ps2_vu_bench::captureEnabled())
            ps2_vu_bench::captureCall(1u, resume, startPC, top, itop, 65536u, interp.state(), code, PS2_VU1_CODE_SIZE, data,
                                      PS2_VU1_DATA_SIZE, memory.getVU1CodeGeneration());
        if (!vu1FastEnabled())
        {
            if (resume)
                interp.resume(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, top, itop, 65536);
            else
                interp.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, startPC, top, itop, 65536);
            return;
        }
        static const bool verify = std::getenv("PS2X_VU_VERIFY") != nullptr;
        VuFastEngine &fast = vu1FastEngine();
        if (!verify)
        {
            const auto t0 = std::chrono::steady_clock::now();
            if (resume)
                fast.resume(interp.state(), code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, &memory, &gs, top, itop, 65536);
            else
                fast.execute(interp.state(), code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, &memory, &gs, startPC, top, itop,
                             65536);
            ps2_frame_log::counters().vu1Ns.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                std::memory_order_relaxed);
            return;
        }

        static std::vector<uint8_t> fastData;
        fastData.assign(data, data + PS2_VU1_DATA_SIZE);
        VU1State fastState = interp.state();
        const uint32_t pcBefore = interp.state().pc;
        fast.setPacketSink([](const uint8_t *, uint32_t) {});
        if (resume)
            fast.resume(fastState, code, PS2_VU1_CODE_SIZE, fastData.data(), PS2_VU1_DATA_SIZE, &memory, &gs, top, itop, 65536);
        else
            fast.execute(fastState, code, PS2_VU1_CODE_SIZE, fastData.data(), PS2_VU1_DATA_SIZE, &memory, &gs, startPC, top,
                         itop, 65536);
        fast.setPacketSink({});
        if (resume)
            interp.resume(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, top, itop, 65536);
        else
            interp.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, &memory, startPC, top, itop, 65536);
        compareVu1Results(fastState, fastData.data(), interp.state(), data, resume ? pcBefore : startPC, resume);
    }
}

bool PS2Runtime::runVu1Native(uint32_t startPC, uint32_t top, uint32_t itop)
{
    uint8_t *vuData = m_memory.getVU1Data();
    static const bool verify = std::getenv("PS2X_VU1_NATIVE_VERIFY") != nullptr;
    if (!verify)
        return m_vu1NativeProgram(*this, vuData, startPC, top, itop);

    // Verification: run the native program on a copy (packets suppressed), then the interpreter
    // on the real memory (packets emitted), and report any difference in the resulting VU memory.
    static std::vector<uint8_t> nativeData;
    nativeData.assign(vuData, vuData + PS2_VU1_DATA_SIZE);
    m_vu1XgkickCaptureOnly = true;
    const bool handled = m_vu1NativeProgram(*this, nativeData.data(), startPC, top, itop);
    m_vu1XgkickCaptureOnly = false;
    if (!handled)
        return false;

    m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, vuData, PS2_VU1_DATA_SIZE, m_gs, &m_memory, startPC, top, itop, 65536);

    static uint64_t s_runs = 0, s_mismatchRuns = 0;
    static uint32_t s_logged = 0;
    ++s_runs;
    uint32_t diffQwords = 0;
    for (uint32_t q = 0; q < PS2_VU1_DATA_SIZE / 16u; ++q)
    {
        if (std::memcmp(vuData + q * 16u, nativeData.data() + q * 16u, 16u) == 0)
            continue;
        ++diffQwords;
        if (s_logged < 60u)
        {
            ++s_logged;
            uint32_t a[4], b[4];
            std::memcpy(a, nativeData.data() + q * 16u, 16u);
            std::memcpy(b, vuData + q * 16u, 16u);
            std::cout << "[vu1:verify] pc=0x" << std::hex << startPC << " top=0x" << top << " qw=0x" << q
                      << " native=" << a[0] << "," << a[1] << "," << a[2] << "," << a[3]
                      << " interp=" << b[0] << "," << b[1] << "," << b[2] << "," << b[3] << std::dec << std::endl;
        }
    }
    if (diffQwords != 0u)
        ++s_mismatchRuns;
    if ((s_runs % 2000u) == 0u || (diffQwords != 0u && s_mismatchRuns <= 5u))
        std::cout << "[vu1:verify] runs=" << s_runs << " mismatching=" << s_mismatchRuns
                  << " (this run " << diffQwords << " qwords differ)" << std::endl;
    return true;
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    // Games that read the disc by raw LBN (their own file tables) need the image itself.
    if (const char *cdImage = std::getenv("PS2X_CD_IMAGE"); cdImage != nullptr && cdImage[0] != '\0')
    {
        paths.cdImage = std::filesystem::path(cdImage);
        std::cout << "[io] CD image: " << paths.cdImage.string() << std::endl;
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }

    std::vector<const PS2OverlayTable *> &overlayRegistry()
    {
        static std::vector<const PS2OverlayTable *> registry;
        return registry;
    }

    bool overlayIsResident(const PS2OverlayTable *overlay, const uint8_t *rdram)
    {
        for (uint32_t i = 0; i < overlay->sampleCount; ++i)
        {
            uint64_t value = 0;
            std::memcpy(&value, rdram + (overlay->sampleAddresses[i] & PS2_RAM_MASK), sizeof(value));
            if (value != overlay->sampleValues[i])
            {
                return false;
            }
        }
        return true;
    }
}

void ps2RegisterOverlayTable(const PS2OverlayTable *overlay)
{
    overlayRegistry().push_back(overlay);
}

namespace
{
    // Optional game hook for code that is neither recompiled nor an overlay (see ps2_code_resolver.h).
    std::atomic<PS2CodeResolver> g_ps2CodeResolver{nullptr};

    PS2Runtime::RecompiledFunction resolveExternalCode(PS2Runtime *runtime, uint8_t *rdram, uint32_t address)
    {
        const PS2CodeResolver resolver = g_ps2CodeResolver.load(std::memory_order_acquire);
        return (resolver != nullptr && rdram != nullptr) ? resolver(rdram, address, runtime) : nullptr;
    }
}

void ps2SetCodeResolver(PS2CodeResolver resolver)
{
    g_ps2CodeResolver.store(resolver, std::memory_order_release);
}

PS2Runtime::RecompiledFunction PS2Runtime::findOverlayFunction(uint32_t address) const
{
    if ((address & 3u) != 0u)
    {
        return nullptr;
    }

    const uint8_t *rdram = const_cast<PS2Memory &>(m_memory).getRDRAM();
    if (rdram == nullptr)
    {
        return nullptr;
    }

    auto slotIn = [address](const PS2OverlayTable *overlay) -> RecompiledFunction
    {
        return overlay->table[(address - overlay->base) >> 2];
    };
    auto covers = [address](const PS2OverlayTable *overlay)
    {
        return address >= overlay->base && address < overlay->end;
    };

    const PS2OverlayTable *active = m_activeOverlay.load(std::memory_order_acquire);
    if (active != nullptr && covers(active) && overlayIsResident(active, rdram))
    {
        return slotIn(active);
    }

    for (const PS2OverlayTable *overlay : overlayRegistry())
    {
        if (overlay == active || !covers(overlay) || !overlayIsResident(overlay, rdram))
        {
            continue;
        }

        m_activeOverlay.store(overlay, std::memory_order_release);
        g_ps2ActiveOverlayName.store(overlay->name, std::memory_order_relaxed);
        std::cout << "[overlay] resident: " << overlay->name << std::endl;
        return slotIn(overlay);
    }

    return nullptr;
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr)
    {
        return true;
    }
    if (findOverlayFunction(address) != nullptr)
    {
        return true;
    }
    return resolveExternalCode(const_cast<PS2Runtime *>(this),
                               const_cast<PS2Memory &>(m_memory).getRDRAM(),
                               address) != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    if (RecompiledFunction overlayFn = findOverlayFunction(address))
    {
        return overlayFn;
    }

    if (RecompiledFunction resolvedFn = resolveExternalCode(this, m_memory.getRDRAM(), address))
    {
        return resolvedFn;
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

namespace
{
    // PS2X_CALLCOUNT=<firstTick>,<lastTick>,<file>: counts calls (JAL/JALR) per target function and writes the counts
    // every 10 vsync ticks as "t=<tick> <hex target>:<count> ..." lines; also the caller of each target (first seen).
    // Diagnostics for finding which game code runs around an event.
    struct CallCount
    {
        bool enabled = false;
        uint64_t first = 0, last = 0, bucket = ~0ull;
        FILE *out = nullptr;
        static constexpr uint32_t kBase = 0x00100000u, kSize = 0x00400000u; // code region covered (16 MB of words)
        std::vector<uint32_t> counts;
        std::vector<uint32_t> callers;
        std::vector<uint32_t> touched;
        CallCount()
        {
            const char *env = std::getenv("PS2X_CALLCOUNT");
            unsigned long long f = 0, l = 0;
            char path[512] = {};
            if (!env || std::sscanf(env, "%llu,%llu,%511[^\n]", &f, &l, path) != 3)
                return;
            out = std::fopen(path, "w");
            enabled = out != nullptr;
            first = f;
            last = l;
            counts.assign(kSize / 4u, 0u);
            callers.assign(kSize / 4u, 0u);
        }
        void flush()
        {
            if (!out || touched.empty())
                return;
            std::fprintf(out, "t=%llu", static_cast<unsigned long long>(bucket));
            for (uint32_t idx : touched)
            {
                std::fprintf(out, " %x:%u:%x", kBase + idx * 4u, counts[idx], callers[idx]);
                counts[idx] = 0u;
            }
            std::fprintf(out, "\n");
            std::fflush(out);
            touched.clear();
        }
        void note(uint64_t tick, uint32_t target, uint32_t source)
        {
            if (tick < first || tick > last)
            {
                if (tick > last && !touched.empty())
                    flush();
                return;
            }
            const uint64_t b = tick - tick % 10u;
            if (b != bucket)
            {
                flush();
                bucket = b;
            }
            const uint32_t off = target - kBase;
            if (off >= kSize)
                return;
            const uint32_t idx = off / 4u;
            if (counts[idx]++ == 0u)
            {
                touched.push_back(idx);
                callers[idx] = source;
            }
        }
    };
    CallCount &callCount()
    {
        static CallCount c;
        return c;
    }

    // PS2X_CALLARGS=<hex target>[,<hex target>...][/max[/firstTick]] (';' also separates): prints tick, caller and
    // a0-a3 of calls to those functions (first `max` calls from firstTick on, default 200). Diagnostics.
    struct CallArgs
    {
        uint32_t targets[8] = {};
        uint32_t count = 0;
        uint32_t remaining = 200;
        uint64_t fromTick = 0;
        CallArgs()
        {
            const char *env = std::getenv("PS2X_CALLARGS");
            if (!env)
                return;
            std::string v(env);
            const size_t semi = v.find_first_of(";/");
            if (semi != std::string::npos)
            {
                char *end = nullptr;
                remaining = static_cast<uint32_t>(std::strtoul(v.c_str() + semi + 1, &end, 10));
                if (end && (*end == ';' || *end == '/'))
                    fromTick = std::strtoull(end + 1, nullptr, 10); // ...;max;firstTick
                v = v.substr(0, semi);
            }
            size_t pos = 0;
            while (pos < v.size() && count < 8u)
            {
                const size_t comma = v.find(',', pos);
                targets[count++] = static_cast<uint32_t>(std::strtoul(v.substr(pos, comma - pos).c_str(), nullptr, 16));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
        }
        void note(const R5900Context *ctx, uint32_t target, uint32_t source)
        {
            if (remaining == 0u || ps2CurrentVsyncTick() < fromTick)
                return;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (targets[i] != target)
                    continue;
                --remaining;
                std::printf("[callargs] t=%llu %08x <- %08x a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x sp=%08x\n",
                            static_cast<unsigned long long>(ps2CurrentVsyncTick()), target, source,
                            static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)),
                            static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0)), static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0)),
                            static_cast<uint32_t>(_mm_extract_epi32(ctx->r[8], 0)), static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)));
                return;
            }
        }
    };
    CallArgs &callArgs()
    {
        static CallArgs c;
        return c;
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    {
        static CallCount &s_calls = callCount();
        if (s_calls.enabled)
            s_calls.note(ps2CurrentVsyncTick(), targetPc, sourcePc);
        static CallArgs &s_args = callArgs();
        if (s_args.count != 0u)
            s_args.note(ctx, targetPc, sourcePc);
    }
    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    // VCALLMS programs run on the fast engine too (same switch as VU1: PS2X_VU_FAST=0 falls back). They are short
    // and very frequent in ZOE, so the interpreter's per-call reset alone was a noticeable cost.
    if (vu1FastEnabled())
    {
        static VuFastEngine s_vu0Fast(true);
        static VU1State s_state;
        copyVu0ContextToState(ctx, s_state);
        if (ps2_vu_bench::captureEnabled())
            ps2_vu_bench::captureCall(0u, false, startPC, 0u, ctx->vu0_itop, 4096u, s_state, vu0Code, PS2_VU0_CODE_SIZE, vu0Data,
                                      PS2_VU0_DATA_SIZE, m_memory.getVU0CodeGeneration());
        static const bool verify = std::getenv("PS2X_VU0_VERIFY") != nullptr;
        if (verify)
        {
            // Reference run on copies: the interpreter from the same state and a copy of VU0 data memory.
            static std::vector<uint8_t> refData;
            refData.assign(vu0Data, vu0Data + PS2_VU0_DATA_SIZE);
            m_vu0.reset();
            copyVu0ContextToState(ctx, m_vu0.state());
            m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE, refData.data(), PS2_VU0_DATA_SIZE, m_gs, &m_memory, startPC, 0u,
                          ctx->vu0_itop, 4096);
            s_vu0Fast.execute(s_state, vu0Code, PS2_VU0_CODE_SIZE, vu0Data, PS2_VU0_DATA_SIZE, &m_memory, &m_gs, startPC,
                              0u, ctx->vu0_itop, 4096);
            const VU1State &ref = m_vu0.state();
            static uint64_t s_runs = 0, s_bad = 0;
            ++s_runs;
            bool bad = std::memcmp(refData.data(), vu0Data, PS2_VU0_DATA_SIZE) != 0 ||
                       std::memcmp(ref.vf, s_state.vf, sizeof(ref.vf)) != 0 || std::memcmp(ref.acc, s_state.acc, sizeof(ref.acc)) != 0 ||
                       ref.clip != s_state.clip || std::memcmp(&ref.q, &s_state.q, sizeof(float)) != 0;
            for (uint32_t r = 1; r < 16u && !bad; ++r)
                bad = (ref.vi[r] & 0xFFFF) != (s_state.vi[r] & 0xFFFF);
            if (bad && ++s_bad <= 20u)
                std::cerr << "[vu0:verify] mismatch at vcallms 0x" << std::hex << startPC << std::dec << std::endl;
            if ((s_runs % 50000u) == 0u)
                std::cerr << "[vu0:verify] runs=" << s_runs << " mismatching=" << s_bad << std::endl;
        }
        else
        {
            s_vu0Fast.execute(s_state, vu0Code, PS2_VU0_CODE_SIZE, vu0Data, PS2_VU0_DATA_SIZE, &m_memory, &m_gs, startPC, 0u,
                              ctx->vu0_itop, 4096);
        }
        copyVu0StateToContext(s_state, ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2_gs_worker::drain(ps2_gs_worker::Sync::GsDirect); // the native paths below drive the GS directly
    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    EeCompletionTag tag{};
    tag.kind = EeCompletionTag::SetPc;
    tag.args[0] = resumePc;
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             },
                             tag);
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    // PS2X_GPU=1 rasterizes the GS on the GPU (OpenGL) instead of the CPU.
    if (const char *gpu = std::getenv("PS2X_GPU"); gpu != nullptr && *gpu != 0 && *gpu != '0')
    {
        m_gs.setRasterBackend(std::make_unique<GSGpuBackend>());
        GSGpuBackend::SetPresentTarget(frameTex.id, FB_WIDTH, FB_HEIGHT);
        std::cout << "[gpu] GS rasterizer: OpenGL" << std::endl;
    }

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        {
            // The EE FPU (and VU0 macro mode) rounds toward zero and flushes denormals; translated code runs on SSE,
            // so give this thread the same MXCSR as PCSX2's EE default (0xFFC0: round to zero, FTZ, DAZ, exceptions
            // masked). PS2X_EE_ROUND=nearest keeps the PC default.
            const char *round = std::getenv("PS2X_EE_ROUND");
            if (round == nullptr || std::strcmp(round, "nearest") != 0)
            {
                _mm_setcsr(0xFFC0u);
                std::cout << "[ee] FPU rounding: toward zero, denormals flushed (MXCSR 0xFFC0)" << std::endl;
            }
        }
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    if (presentOnFlip())
    {
        SetTargetFPS(0); // the game's flips pace the window instead of raylib's timer
        std::cout << "[present] showing each frame when the game flips to it (PS2X_PRESENT=flip)" << std::endl;
    }
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        if (presentOnFlip())
            waitForNextFrame(this);
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        ps2InputUpdate();
        if (GSGpuBackend::Active())
        {
            // Internal resolution (Graphics menu / PS2X_GPU_SCALE): the GPU renderer composes the picture into
            // frameTex at that many times 640 x 512, so resize it when the choice changes.
            static uint32_t s_frameScale = 1u;
            const uint32_t wantScale = GSGpuBackend::DesiredScale();
            if (wantScale != s_frameScale)
            {
                const int w = FB_WIDTH * static_cast<int>(wantScale), h = FB_HEIGHT * static_cast<int>(wantScale);
                const unsigned int id = rlLoadTexture(nullptr, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                if (id == 0u)
                {
                    // Keep the current picture (and its scale) rather than a black screen.
                    std::cerr << "[gpu] could not create a " << w << "x" << h << " window picture; internal resolution unchanged"
                              << std::endl;
                }
                else
                {
                    UnloadTexture(frameTex);
                    frameTex.id = id;
                    frameTex.width = w;
                    frameTex.height = h;
                    frameTex.mipmaps = 1;
                    frameTex.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                    GSGpuBackend::SetPresentTarget(frameTex.id, static_cast<uint32_t>(w), static_cast<uint32_t>(h), wantScale);
                    std::cout << "[gpu] window picture " << w << "x" << h << " for internal resolution x" << wantScale << std::endl;
                }
                s_frameScale = wantScale; // tried: do not retry every frame
            }
        }
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        // Movie shape of the frame about to be latched: the flag recorded with the latest applied flip, read before the
        // latch; it belongs to the latched frame only if no flip was applied while latching (checked below).
        const uint64_t latchFlipsBefore = ps2_gs_worker::flipCount();
        const bool latchMovieBefore = ps2_gs_worker::flipMovie();
        UploadFrame(frameTex, this, presentWidth, presentHeight);
        const bool latchFlipStable = ps2_gs_worker::flipCount() == latchFlipsBefore;

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // The PS2 showed every frame buffer size on a 4:3 TV; "stretch" fills the window instead.
        const bool stretch = ps2Settings().stretch;
        // 4/3 unless a game override asked for another shape; movies are 4:3 content and always get 4/3. The shape comes
        // from the frame itself: the flag recorded when the game issued the flip of the frame just latched
        // (ps2_gs_worker::flipMovie), not from the live decoder state, which the game thread may already have moved on
        // (movie deleted, first 3D frame drawn) while this older frame is still the one on screen. The flag is taken only
        // when no flip was applied during the latch (otherwise it may belong to the next frame): then the previous shape
        // is kept for this one window frame. If the game has not flipped for 2 s (it changes the display another way)
        // the live state is used; shorter gaps (loads, fades after a movie) keep the shape of the frame still on screen.
        static uint64_t s_movieFlips = ~0ull;
        static auto s_movieFlipTime = std::chrono::steady_clock::now();
        static bool s_lastMovie = false;
        const uint64_t movieFlips = latchFlipsBefore;
        const auto movieNowTime = std::chrono::steady_clock::now();
        if (movieFlips != s_movieFlips)
        {
            s_movieFlips = movieFlips;
            s_movieFlipTime = movieNowTime;
        }
        const bool movieNow = (movieNowTime - s_movieFlipTime >= std::chrono::seconds(2)) ? ps2MovieActive()
                              : latchFlipStable                                          ? latchMovieBefore
                                                                                         : s_lastMovie;
        if (movieNow != s_lastMovie)
        {
            s_lastMovie = movieNow;
            if (ps2WidescreenActive()) // silent for games / settings that keep the 4:3 shape
                std::cout << "[display] movie " << (movieNow ? "started" : "ended") << ": window aspect "
                          << (movieNow ? 4.0f / 3.0f : ps2GameDisplayAspect()) << " (flip " << movieFlips << ", tick "
                          << ps2CurrentVsyncTick() << ")" << std::endl;
        }
        // The game's wide shape only while the widescreen setting is on (and the game supports it).
        static bool s_lastWide = false;
        const bool wideNow = ps2WidescreenActive();
        if (wideNow != s_lastWide)
        {
            s_lastWide = wideNow;
            std::cout << "[display] widescreen " << (wideNow ? "on" : "off") << ": window aspect "
                      << (wideNow ? ps2GameDisplayAspect() : 4.0f / 3.0f) << std::endl;
        }
        const float displayAspect = (movieNow || !wideNow) ? 4.0f / 3.0f : ps2GameDisplayAspect();
        const float dstWidth = stretch ? screenWidth : std::min(screenWidth, screenHeight * displayAspect);
        const float dstHeight = stretch ? screenHeight : std::min(screenHeight, screenWidth / displayAspect);
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    ps2_iop_thread::shutdown();
    ps2_gs_worker::shutdown();

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    ps2InputShutdown();
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
