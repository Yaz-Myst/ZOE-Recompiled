#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "ps2_input.h"
#include "ps2_game_clock.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // Debug aid for unattended test runs: PS2X_INPUT_SCRIPT="start@25,cross@31.5,lup@t4800+2,..."
    //   name@<seconds>[+<hold>]  press at that many seconds after the first pad read (input clock, see below)
    //   name@t<tick>[+<hold>]    press at that absolute vsync tick
    // held for <hold> seconds (default 0.25). Names: the buttons below, plus stick directions lup/ldown/lleft/lright
    // and rup/rdown/rleft/rright (full deflection; opposite directions cancel, two axes combine), and "light" (holds the
    // light-press modifier: pressed buttons report the light-press pressure from the Controls settings).
    struct ScriptedPress
    {
        uint16_t mask;   // button, or 0 for a stick direction
        uint8_t stick;   // 0 none, else kStick* bit
        bool light;      // the light-press modifier
        double at;       // seconds (input clock) unless absoluteTick
        double hold;
        bool absoluteTick;
    };

    constexpr uint8_t kStickLUp = 0x01, kStickLDown = 0x02, kStickLLeft = 0x04, kStickLRight = 0x08;
    constexpr uint8_t kStickRUp = 0x10, kStickRDown = 0x20, kStickRLeft = 0x40, kStickRRight = 0x80;
    struct NamedStick
    {
        const char *name;
        uint8_t bit;
    };
    const NamedStick kStickNames[] = {{"lup", kStickLUp},    {"ldown", kStickLDown}, {"lleft", kStickLLeft},
                                      {"lright", kStickLRight}, {"rup", kStickRUp},  {"rdown", kStickRDown},
                                      {"rleft", kStickRLeft}, {"rright", kStickRRight}};

    struct NamedButton
    {
        const char *name;
        uint16_t mask;
    };
    const NamedButton kButtonNames[] = {
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
        {"start", PAD_START}, {"select", PAD_SELECT}, {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE},
        {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE}, {"l1", PAD_L1}, {"r1", PAD_R1},
        {"l2", PAD_L2}, {"r2", PAD_R2}};

    std::atomic<uint64_t> g_lastVsyncTick{0};

    // Seconds since the first pad read: shared by the input script (replay) and the recorder. Host time by default;
    // PS2X_INPUT_CLOCK=ticks uses game time (vsync ticks / 60) so routes survive speed changes.
    double padClockSeconds()
    {
        static const bool useTicks = []
        {
            const char *env = std::getenv("PS2X_INPUT_CLOCK");
            return env != nullptr && std::strcmp(env, "ticks") == 0;
        }();
        if (useTicks)
        {
            static const uint64_t startTick = g_lastVsyncTick.load(std::memory_order_relaxed);
            return static_cast<double>(g_lastVsyncTick.load(std::memory_order_relaxed) - startTick) / 60.0;
        }
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    // PS2X_INPUT_RECORD=<file>: writes every button press and stick direction as "name@start+hold," (PS2X_INPUT_SCRIPT
    // format, written on release), so a session played by hand can be replayed unattended.
    uint8_t stickBits(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        uint8_t bits = 0u;
        if (ly < 64u)
            bits |= kStickLUp;
        if (ly > 192u)
            bits |= kStickLDown;
        if (lx < 64u)
            bits |= kStickLLeft;
        if (lx > 192u)
            bits |= kStickLRight;
        if (ry < 64u)
            bits |= kStickRUp;
        if (ry > 192u)
            bits |= kStickRDown;
        if (rx < 64u)
            bits |= kStickRLeft;
        if (rx > 192u)
            bits |= kStickRRight;
        return bits;
    }

    void recordPresses(uint16_t pressedMask, uint8_t sticks)
    {
        static std::FILE *file = []() -> std::FILE *
        {
            const char *path = std::getenv("PS2X_INPUT_RECORD");
            return (path != nullptr && *path != 0) ? std::fopen(path, "w") : nullptr;
        }();
        if (file == nullptr)
            return;
        static uint16_t previous = 0u;
        static uint8_t previousSticks = 0u;
        static double buttonStart[16] = {};
        static double stickStart[8] = {};
        const double now = padClockSeconds();
        bool wrote = false;
        for (const NamedButton &b : kButtonNames)
        {
            const bool down = (pressedMask & b.mask) != 0u, was = (previous & b.mask) != 0u;
            unsigned long bit = 0;
            for (uint32_t i = 0; i < 16u; ++i)
                if (b.mask == (1u << i))
                    bit = i;
            if (down && !was)
                buttonStart[bit] = now;
            else if (!down && was)
            {
                std::fprintf(file, "%s@%.2f+%.2f,", b.name, buttonStart[bit], now - buttonStart[bit]);
                wrote = true;
            }
        }
        for (uint32_t i = 0; i < 8u; ++i)
        {
            const NamedStick &st = kStickNames[i];
            const bool down = (sticks & st.bit) != 0u, was = (previousSticks & st.bit) != 0u;
            if (down && !was)
                stickStart[i] = now;
            else if (!down && was)
            {
                std::fprintf(file, "%s@%.2f+%.2f,", st.name, stickStart[i], now - stickStart[i]);
                wrote = true;
            }
        }
        previous = pressedMask;
        previousSticks = sticks;
        if (wrote)
            std::fflush(file);
    }

    struct ScriptedInput
    {
        uint16_t buttons = 0u;
        uint8_t sticks = 0u;
        bool light = false;
    };

    ScriptedInput scriptedInput()
    {
        static std::vector<ScriptedPress> presses = []
        {
            std::vector<ScriptedPress> out;
            const char *env = std::getenv("PS2X_INPUT_SCRIPT");
            if (env == nullptr)
                return out;
            std::stringstream ss(env);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                const size_t at = item.find('@');
                if (at == std::string::npos)
                    continue;
                const std::string name = item.substr(0, at);
                std::string when = item.substr(at + 1);
                double hold = 0.25;
                const size_t plus = when.find('+');
                if (plus != std::string::npos)
                {
                    hold = std::atof(when.c_str() + plus + 1);
                    when = when.substr(0, plus);
                }
                const bool absoluteTick = !when.empty() && (when[0] == 't' || when[0] == 'T');
                const double t = std::atof(when.c_str() + (absoluteTick ? 1 : 0));
                for (const NamedButton &n : kButtonNames)
                    if (name == n.name)
                        out.push_back({n.mask, 0u, false, t, hold, absoluteTick});
                for (const NamedStick &n : kStickNames)
                    if (name == n.name)
                        out.push_back({0u, n.bit, false, t, hold, absoluteTick});
                if (name == "light")
                    out.push_back({0u, 0u, true, t, hold, absoluteTick});
            }
            return out;
        }();
        ScriptedInput in;
        if (presses.empty())
            return in;

        const double now = padClockSeconds();
        const double tick = static_cast<double>(g_lastVsyncTick.load(std::memory_order_relaxed));
        for (const ScriptedPress &press : presses)
        {
            const bool active = press.absoluteTick ? (tick >= press.at && tick < press.at + press.hold * 60.0)
                                                   : (now >= press.at && now < press.at + press.hold);
            if (!active)
                continue;
            in.buttons |= press.mask;
            in.sticks |= press.stick;
            in.light = in.light || press.light;
        }
        return in;
    }

    uint8_t applyStick(uint8_t value, bool negative, bool positive)
    {
        if (negative && !positive)
            return 0x00u;
        if (positive && !negative)
            return 0xFFu;
        return value;
    }
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;
    (void)padClockSeconds(); // starts the input clock at the first pad read
    // Port 1 without a controller on it is an empty socket (the pad stand-ins then report it disconnected).
    if (!ps2InputPortConnected(port))
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;

    // Keyboard and controllers are read once per host frame by ps2_input (window thread), through
    // the player's bindings; while the in-game menu is open the snapshot is an idle pad.
    const Ps2PadSnapshot pad = ps2InputSnapshot(port);
    const ScriptedInput scripted = port == 0 ? scriptedInput() : ScriptedInput{};
    const uint16_t btns = static_cast<uint16_t>(pad.buttons & ~scripted.buttons);
    const uint8_t s = scripted.sticks;
    const uint8_t rx = applyStick(pad.rx, (s & kStickRLeft) != 0u, (s & kStickRRight) != 0u);
    const uint8_t ry = applyStick(pad.ry, (s & kStickRUp) != 0u, (s & kStickRDown) != 0u);
    const uint8_t lx = applyStick(pad.lx, (s & kStickLLeft) != 0u, (s & kStickLRight) != 0u);
    const uint8_t ly = applyStick(pad.ly, (s & kStickLUp) != 0u, (s & kStickLDown) != 0u);
    if (port == 0)
        recordPresses(static_cast<uint16_t>(~btns), stickBits(lx, ly, rx, ry));
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    data[4] = rx;
    data[5] = ry;
    data[6] = lx;
    data[7] = ly;
    ps2InputPressure(port, btns, scripted.light, data + 8); // bytes 8..19; the pad stand-in sends them in pressure mode
    return true;
}

void ps2NoteVsyncTick(uint64_t tick)
{
    g_lastVsyncTick.store(tick, std::memory_order_relaxed);
}

uint64_t ps2CurrentVsyncTick()
{
    return g_lastVsyncTick.load(std::memory_order_relaxed);
}
