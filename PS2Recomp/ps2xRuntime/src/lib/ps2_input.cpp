#include "ps2_input.h"
#include "ps2_host_settings.h"

#include "raylib.h"

#if defined(PS2X_SDL_INPUT)
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    // PS2 pad button bits (active low in the pad data), in Ps2PadAction order.
    constexpr uint16_t kPadBits[kPadOpenMenu] = {0x0010u, 0x0040u, 0x0080u, 0x0020u, 0x4000u, 0x2000u, 0x8000u, 0x1000u,
                                                 0x0400u, 0x0800u, 0x0100u, 0x0200u, 0x0002u, 0x0004u, 0x0008u, 0x0001u};

    // SDL2's game controller button and axis numbers (stable ABI), for the defaults.
    enum : int
    {
        kBtnA = 0, kBtnB, kBtnX, kBtnY, kBtnBack, kBtnGuide, kBtnStart, kBtnLeftStick, kBtnRightStick,
        kBtnLeftShoulder, kBtnRightShoulder, kBtnDpadUp, kBtnDpadDown, kBtnDpadLeft, kBtnDpadRight,
        kButtonCount = 21
    };
    enum : int
    {
        kAxisLeftX = 0, kAxisLeftY, kAxisRightX, kAxisRightY, kAxisTriggerLeft, kAxisTriggerRight, kAxisCount
    };
    constexpr int button(int b) { return kPadCodeButton + b; }
    constexpr int axisPositive(int a) { return kPadCodeAxis + a * 2 + 1; }
    constexpr int kCodeCount = kPadCodeAxis + kAxisCount * 2;

    uint64_t pack(const Ps2PadSnapshot &s)
    {
        return static_cast<uint64_t>(s.buttons) | (static_cast<uint64_t>(s.rx) << 16) | (static_cast<uint64_t>(s.ry) << 24) |
               (static_cast<uint64_t>(s.lx) << 32) | (static_cast<uint64_t>(s.ly) << 40);
    }

    // Per pad port (see kPs2InputPorts): the latest snapshot and the motor state the game asked for.
    std::atomic<uint64_t> g_snapshot[kPs2InputPorts] = {pack(Ps2PadSnapshot{}), pack(Ps2PadSnapshot{})};
    std::atomic<bool> g_rumbleSmall[kPs2InputPorts] = {false, false};
    std::atomic<int> g_rumbleLarge[kPs2InputPorts] = {0, 0}; // 0..255
    std::atomic<bool> g_port1Connected{false};
    double g_testRumbleUntil = 0.0;
    bool g_menuPressed = false;
    int g_captured = 0;
    std::array<bool, kCodeCount> g_wasDown{};

    // Pressure: the live level of each PS2 button per port (0 released .. 255 full), and the light-press settings
    // copied from ps2Settings() for the game thread. Each port has its own light-press modifier state.
    std::array<std::array<std::atomic<uint8_t>, kPadOpenMenu>, kPs2InputPorts> g_level{};
    std::atomic<bool> g_lightOn[kPs2InputPorts] = {false, false};
    std::atomic<int> g_lightLevel{102}; // 0..255
    std::atomic<int> g_lightButtons{0x0FFF};
    bool g_lightToggled[kPs2InputPorts] = {};
    bool g_lightWasDown[kPs2InputPorts] = {};

    // PS2X_PAD_PORTS=1: the single-player behaviour from before the second port (every controller and the keyboard
    // drive port 0; port 1 reads port 0's data and reports connected; only port 0's rumble counts).
    bool singlePort()
    {
        static const bool single = []
        {
            const char *env = std::getenv("PS2X_PAD_PORTS");
            return env != nullptr && *env == '1';
        }();
        return single;
    }

    // The pressure byte order of the pad data, as Ps2PadAction.
    constexpr int kPressureActions[kPadPressureCount] = {kPadRight, kPadLeft,     kPadUp,     kPadDown,
                                                         kPadTriangle, kPadCircle, kPadCross, kPadSquare,
                                                         kPadL1,    kPadR1,       kPadL2,     kPadR2};

    // A light touch on an analog trigger: about 6% of its travel.
    constexpr float kTriggerTouch = 0.06f;

    // PS2X_INPUT_LIVE=0: ignore the keyboard and controllers (and never rumble), so an unattended test run driven by
    // PS2X_INPUT_SCRIPT can run beside a game someone is playing with the same controller.
    bool inputLive()
    {
        static const bool live = []
        {
            const char *env = std::getenv("PS2X_INPUT_LIVE");
            return !(env != nullptr && *env == '0');
        }();
        return live;
    }

    uint8_t stickByte(float v)
    {
        return static_cast<uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f));
    }

    // Radial deadzone, then rescale so full travel still reaches the edge.
    void applyDeadzone(float &x, float &y, float deadzone)
    {
        const float magnitude = std::sqrt(x * x + y * y);
        if (magnitude <= deadzone || magnitude <= 0.0f)
        {
            x = y = 0.0f;
            return;
        }
        const float scale = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone)) / magnitude;
        x = std::clamp(x * scale, -1.0f, 1.0f);
        y = std::clamp(y * scale, -1.0f, 1.0f);
    }

#if defined(PS2X_SDL_INPUT)
    bool g_sdlReady = false;
    std::vector<SDL_GameController *> g_controllers;
    // The controllers that drive each pad port, rebuilt from g_controllers every host frame (mapPorts).
    std::array<std::vector<SDL_GameController *>, kPs2InputPorts> g_portControllers;
    uint16_t g_sentLow[kPs2InputPorts] = {}, g_sentHigh[kPs2InputPorts] = {};
    double g_sentAt[kPs2InputPorts] = {};
    double g_testSentAt = -1.0;

    void openController(int deviceIndex)
    {
        if (!SDL_IsGameController(deviceIndex))
            return;
        if (SDL_GameController *controller = SDL_GameControllerOpen(deviceIndex))
        {
            // SDL also sends "device added" for the controllers already open at start-up, and opening one again
            // returns the same handle (reference counted): keep each controller once, or one pad would fill both
            // ports. PS2X_PAD_PORTS=1 keeps the old double entry (its stick values were summed twice).
            if (!singlePort() && std::find(g_controllers.begin(), g_controllers.end(), controller) != g_controllers.end())
            {
                SDL_GameControllerClose(controller);
                return;
            }
            g_controllers.push_back(controller);
            std::printf("[input] controller connected: %s\n", SDL_GameControllerName(controller));
            std::fflush(stdout);
        }
    }

    bool codeDownOn(const std::vector<SDL_GameController *> &controllers, int code)
    {
        for (SDL_GameController *c : controllers)
        {
            if (code >= kPadCodeAxis && code < kCodeCount)
            {
                const int axis = (code - kPadCodeAxis) / 2;
                const bool positive = ((code - kPadCodeAxis) & 1) != 0;
                const int value = SDL_GameControllerGetAxis(c, static_cast<SDL_GameControllerAxis>(axis));
                if (positive ? value > 16384 : value < -16384)
                    return true;
            }
            else if (code >= kPadCodeButton && code < kPadCodeButton + SDL_CONTROLLER_BUTTON_MAX)
            {
                if (SDL_GameControllerGetButton(c, static_cast<SDL_GameControllerButton>(code - kPadCodeButton)))
                    return true;
            }
        }
        return false;
    }

    // Any connected controller (menu button, rebinding, menu navigation).
    bool codeDown(int code)
    {
        return codeDownOn(g_controllers, code);
    }

    SDL_JoystickID instanceId(SDL_GameController *c)
    {
        return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    }

    SDL_GameController *controllerById(SDL_JoystickID id)
    {
        for (SDL_GameController *c : g_controllers)
            if (id >= 0 && instanceId(c) == id)
                return c;
        return nullptr;
    }

    // Each port keeps its controller (SDL instance id, -1 = free) until that controller is unplugged: a pad that
    // drops out and comes back gets a new id and takes the lowest free port, which is its old one when nothing else
    // was plugged in meanwhile. Controllers beyond the ports drive nothing (spares). A spare takes a freed port only
    // after kSpareDelay seconds, so a pad that drops out briefly gets its port back instead of the spare.
    SDL_JoystickID g_portOwner[kPs2InputPorts] = {-1, -1};
    double g_portFreedAt[kPs2InputPorts] = {-1.0e9, -1.0e9};
    std::vector<SDL_JoystickID> g_spares; // listed but on no port at the end of the last mapPorts
    constexpr double kSpareDelay = 3.0;

    // Rebuilds g_portControllers; true when a port's controller changed (for the log). PS2X_PAD_PORTS=1: every
    // controller drives port 0, as before.
    bool mapPorts()
    {
        std::array<SDL_GameController *, kPs2InputPorts> before{};
        for (int port = 0; port < kPs2InputPorts; ++port)
        {
            before[port] = g_portControllers[port].empty() ? nullptr : g_portControllers[port].front();
            g_portControllers[port].clear();
        }
        if (singlePort())
        {
            g_portControllers[0] = g_controllers;
            return false;
        }
        const double now = GetTime();
        for (int port = 0; port < kPs2InputPorts; ++port)
        {
            if (g_portOwner[port] >= 0 && controllerById(g_portOwner[port]) == nullptr)
            {
                g_portOwner[port] = -1; // unplugged: the port is free
                g_portFreedAt[port] = now;
            }
        }
        std::vector<SDL_JoystickID> spares;
        for (SDL_GameController *c : g_controllers)
        {
            const SDL_JoystickID id = instanceId(c);
            if (std::find(std::begin(g_portOwner), std::end(g_portOwner), id) != std::end(g_portOwner))
                continue;
            const bool spare = std::find(g_spares.begin(), g_spares.end(), id) != g_spares.end();
            bool placed = false;
            for (int port = 0; port < kPs2InputPorts && !placed; ++port)
            {
                if (g_portOwner[port] < 0 && (!spare || now - g_portFreedAt[port] >= kSpareDelay))
                {
                    g_portOwner[port] = id;
                    placed = true;
                }
            }
            if (!placed)
                spares.push_back(id);
        }
        g_spares = std::move(spares);
        bool changed = false;
        for (int port = 0; port < kPs2InputPorts; ++port)
        {
            SDL_GameController *c = controllerById(g_portOwner[port]);
            if (c)
                g_portControllers[port].push_back(c);
            changed = changed || c != before[port];
        }
        return changed;
    }

    bool portHasController(int port)
    {
        return !g_portControllers[port].empty();
    }

    bool portCodeDown(int port, int code)
    {
        return codeDownOn(g_portControllers[port], code);
    }

    float portAxisValue(int port, int axis)
    {
        float sum = 0.0f;
        for (SDL_GameController *c : g_portControllers[port])
            sum += static_cast<float>(SDL_GameControllerGetAxis(c, static_cast<SDL_GameControllerAxis>(axis))) / 32767.0f;
        return std::clamp(sum, -1.0f, 1.0f);
    }

    // How far a trigger bound as code is pulled on a port's controllers (0..1), or -1 when code is not a trigger.
    float portTriggerTravel(int port, int code)
    {
        const int axis = (code - kPadCodeAxis) / 2;
        if (code < kPadCodeAxis || code >= kCodeCount || ((code - kPadCodeAxis) & 1) == 0 ||
            (axis != kAxisTriggerLeft && axis != kAxisTriggerRight))
            return -1.0f;
        float travel = 0.0f;
        for (SDL_GameController *c : g_portControllers[port])
            travel = std::max(travel, static_cast<float>(SDL_GameControllerGetAxis(c, static_cast<SDL_GameControllerAxis>(axis))) / 32767.0f);
        return std::clamp(travel, 0.0f, 1.0f);
    }

    SDL_GameControllerType firstControllerType()
    {
        return g_controllers.empty() ? SDL_CONTROLLER_TYPE_UNKNOWN : SDL_GameControllerGetType(g_controllers.front());
    }
#else
    // No SDL (console ports): raylib's first gamepad, with SDL's button numbering mapped over.
    constexpr int kRaylibButtons[] = {GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
                                      GAMEPAD_BUTTON_RIGHT_FACE_LEFT, GAMEPAD_BUTTON_RIGHT_FACE_UP,
                                      GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE, GAMEPAD_BUTTON_MIDDLE_RIGHT,
                                      GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_LEFT_TRIGGER_1,
                                      GAMEPAD_BUTTON_RIGHT_TRIGGER_1, GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN,
                                      GAMEPAD_BUTTON_LEFT_FACE_LEFT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT};

    float axisValue(int axis)
    {
        return IsGamepadAvailable(0) ? GetGamepadAxisMovement(0, axis) : 0.0f;
    }

    bool codeDown(int code)
    {
        if (!IsGamepadAvailable(0))
            return false;
        if (code >= kPadCodeAxis && code < kCodeCount)
        {
            const int axis = (code - kPadCodeAxis) / 2;
            const float value = axisValue(axis);
            const bool trigger = axis == kAxisTriggerLeft || axis == kAxisTriggerRight; // raylib: -1 released, +1 pressed
            return ((code - kPadCodeAxis) & 1) != 0 ? value > (trigger ? 0.0f : 0.5f) : value < -0.5f;
        }
        const int b = code - kPadCodeButton;
        return b >= 0 && b < static_cast<int>(std::size(kRaylibButtons)) && IsGamepadButtonDown(0, kRaylibButtons[b]);
    }

    float triggerTravel(int code)
    {
        const int axis = (code - kPadCodeAxis) / 2;
        if (code < kPadCodeAxis || code >= kCodeCount || ((code - kPadCodeAxis) & 1) == 0 ||
            (axis != kAxisTriggerLeft && axis != kAxisTriggerRight) || !IsGamepadAvailable(0))
            return -1.0f;
        return std::clamp((axisValue(axis) + 1.0f) * 0.5f, 0.0f, 1.0f); // raylib: -1 released, +1 pressed
    }

    // One gamepad here: it drives port 0 only, so port 1 never has a controller.
    bool portHasController(int port) { return port == 0 && IsGamepadAvailable(0); }
    bool portCodeDown(int port, int code) { return port == 0 && codeDown(code); }
    float portAxisValue(int port, int axis) { return port == 0 ? axisValue(axis) : 0.0f; }
    float portTriggerTravel(int port, int code) { return port == 0 ? triggerTravel(code) : -1.0f; }
#endif

    // One pad port's snapshot, pressure levels and light-press modifier from its controllers, plus the keyboard for
    // port 0. blocked (menu open, input not live): an idle pad.
    void buildPort(int port, bool keyboard, bool blocked, const PS2HostSettings &s)
    {
        Ps2PadSnapshot pad{};
        const auto bindingDown = [&](int action, int slot)
        {
            const int key = keyboard ? s.keys[action][slot] : 0;
            return (key != 0 && IsKeyDown(key)) || (s.pads[action][slot] != 0 && portCodeDown(port, s.pads[action][slot]));
        };
        bool lightDown = false;
        for (int slot = 0; slot < 2; ++slot)
            lightDown = lightDown || (!blocked && bindingDown(kPadLightPress, slot));
        if (lightDown && !g_lightWasDown[port])
            g_lightToggled[port] = !g_lightToggled[port];
        g_lightWasDown[port] = lightDown;
        if (!s.lightPressToggle)
            g_lightToggled[port] = false;
        g_lightOn[port].store(s.lightPressToggle ? g_lightToggled[port] : lightDown, std::memory_order_relaxed);

        std::array<uint8_t, kPadOpenMenu> level{};
        if (!blocked)
        {
            for (int action = 0; action < kPadOpenMenu; ++action)
            {
                int best = 0;
                for (int slot = 0; slot < 2; ++slot)
                {
                    const float travel = s.analogTriggers ? portTriggerTravel(port, s.pads[action][slot]) : -1.0f;
                    if (travel >= 0.0f)
                    {
                        if (travel > kTriggerTouch)
                            best = std::max(best, static_cast<int>(std::lround(travel * 255.0f)));
                        if (keyboard && s.keys[action][slot] != 0 && IsKeyDown(s.keys[action][slot]))
                            best = 255;
                    }
                    else if (bindingDown(action, slot))
                        best = 255;
                }
                level[action] = static_cast<uint8_t>(best);
                if (best > 0)
                    pad.buttons &= static_cast<uint16_t>(~kPadBits[action]);
            }
            const float deadzone = static_cast<float>(s.deadzone) / 100.0f;
            float lx = portAxisValue(port, kAxisLeftX), ly = portAxisValue(port, kAxisLeftY);
            float rx = portAxisValue(port, kAxisRightX), ry = portAxisValue(port, kAxisRightY);
            applyDeadzone(lx, ly, deadzone);
            applyDeadzone(rx, ry, deadzone);
            pad.lx = stickByte(lx);
            pad.ly = stickByte(ly);
            pad.rx = stickByte(rx);
            pad.ry = stickByte(ry);
        }
        for (int action = 0; action < kPadOpenMenu; ++action)
            g_level[port][action].store(level[action], std::memory_order_relaxed);
        g_snapshot[port].store(pack(pad), std::memory_order_release);
    }

    // Port 1 is plugged in for the game only with a controller on it and live input (PS2X_INPUT_LIVE=0 test runs
    // see one pad whatever is plugged into the host).
    void updatePortConnection()
    {
        const bool connected = !singlePort() && inputLive() && portHasController(1);
        if (!connected && g_port1Connected.load(std::memory_order_relaxed))
        {
            // Unplugged: forget the motors, so a pad plugged into this port later stays still until the game
            // sends motor values again.
            g_rumbleSmall[1].store(false, std::memory_order_relaxed);
            g_rumbleLarge[1].store(0, std::memory_order_relaxed);
#if defined(PS2X_SDL_INPUT)
            g_sentLow[1] = g_sentHigh[1] = 0;
#endif
        }
        g_port1Connected.store(connected, std::memory_order_relaxed);
    }
}

void ps2DefaultControls(PS2HostSettings &s)
{
    for (auto &slots : s.keys)
        slots = {0, 0};
    for (auto &slots : s.pads)
        slots = {0, 0};
    s.keys[kPadUp] = {KEY_UP, KEY_W};
    s.keys[kPadDown] = {KEY_DOWN, KEY_S};
    s.keys[kPadLeft] = {KEY_LEFT, KEY_A};
    s.keys[kPadRight] = {KEY_RIGHT, KEY_D};
    s.keys[kPadCross] = {KEY_X, KEY_SPACE};
    s.keys[kPadCircle] = {KEY_C, 0};
    s.keys[kPadSquare] = {KEY_Z, KEY_KP_0};
    s.keys[kPadTriangle] = {KEY_V, KEY_KP_1};
    s.keys[kPadL1] = {KEY_Q, 0};
    s.keys[kPadR1] = {KEY_E, 0};
    s.keys[kPadL2] = {KEY_LEFT_SHIFT, 0};
    s.keys[kPadR2] = {KEY_RIGHT_SHIFT, 0};
    s.keys[kPadStart] = {KEY_ENTER, 0};
    s.keys[kPadSelect] = {KEY_TAB, 0};
    s.keys[kPadOpenMenu] = {KEY_ESCAPE, 0};

    s.pads[kPadUp] = {button(kBtnDpadUp), 0};
    s.pads[kPadDown] = {button(kBtnDpadDown), 0};
    s.pads[kPadLeft] = {button(kBtnDpadLeft), 0};
    s.pads[kPadRight] = {button(kBtnDpadRight), 0};
    s.pads[kPadCross] = {button(kBtnA), 0};
    s.pads[kPadCircle] = {button(kBtnB), 0};
    s.pads[kPadSquare] = {button(kBtnX), 0};
    s.pads[kPadTriangle] = {button(kBtnY), 0};
    s.pads[kPadL1] = {button(kBtnLeftShoulder), 0};
    s.pads[kPadR1] = {button(kBtnRightShoulder), 0};
    s.pads[kPadL2] = {axisPositive(kAxisTriggerLeft), 0};
    s.pads[kPadR2] = {axisPositive(kAxisTriggerRight), 0};
    s.pads[kPadL3] = {button(kBtnLeftStick), 0};
    s.pads[kPadR3] = {button(kBtnRightStick), 0};
    s.pads[kPadStart] = {button(kBtnStart), 0};
    s.pads[kPadSelect] = {button(kBtnBack), 0};
    s.pads[kPadOpenMenu] = {button(kBtnGuide), 0};
    s.keys[kPadLightPress] = {KEY_LEFT_CONTROL, 0}; // no controller default: every button is taken

    s.deadzone = 15;
    s.rumble = 60;
    s.lightPressLevel = 40;
    s.lightPressToggle = false;
    s.lightPressButtons = 0x0FFF;
    s.analogTriggers = false;
}

const char *ps2PadActionName(int action)
{
    static const char *names[kPadActionCount] = {"D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Cross", "Circle",
                                                 "Square", "Triangle", "L1", "R1", "L2", "R2", "L3 (left stick press)",
                                                 "R3 (right stick press)", "Start", "Select", "Open this menu",
                                                 "Light press (modifier)"};
    return action >= 0 && action < kPadActionCount ? names[action] : "?";
}

std::string ps2KeyName(int key)
{
    if (key == 0)
        return "-";
    if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE))
        return std::string(1, static_cast<char>(key));
    if (key >= KEY_F1 && key <= KEY_F12)
        return "F" + std::to_string(key - KEY_F1 + 1);
    if (key >= KEY_KP_0 && key <= KEY_KP_9)
        return "Keypad " + std::to_string(key - KEY_KP_0);
    switch (key)
    {
    case KEY_SPACE: return "Space";
    case KEY_ENTER: return "Enter";
    case KEY_TAB: return "Tab";
    case KEY_BACKSPACE: return "Backspace";
    case KEY_ESCAPE: return "Esc";
    case KEY_UP: return "Up";
    case KEY_DOWN: return "Down";
    case KEY_LEFT: return "Left";
    case KEY_RIGHT: return "Right";
    case KEY_LEFT_SHIFT: return "Left Shift";
    case KEY_RIGHT_SHIFT: return "Right Shift";
    case KEY_LEFT_CONTROL: return "Left Ctrl";
    case KEY_RIGHT_CONTROL: return "Right Ctrl";
    case KEY_LEFT_ALT: return "Left Alt";
    case KEY_RIGHT_ALT: return "Right Alt";
    case KEY_INSERT: return "Insert";
    case KEY_DELETE: return "Delete";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case KEY_PAGE_UP: return "Page Up";
    case KEY_PAGE_DOWN: return "Page Down";
    case KEY_KP_ENTER: return "Keypad Enter";
    case KEY_KP_ADD: return "Keypad +";
    case KEY_KP_SUBTRACT: return "Keypad -";
    case KEY_KP_MULTIPLY: return "Keypad *";
    case KEY_KP_DIVIDE: return "Keypad /";
    case KEY_KP_DECIMAL: return "Keypad .";
    case KEY_COMMA: return ",";
    case KEY_PERIOD: return ".";
    case KEY_SLASH: return "/";
    case KEY_SEMICOLON: return ";";
    case KEY_APOSTROPHE: return "'";
    case KEY_LEFT_BRACKET: return "[";
    case KEY_RIGHT_BRACKET: return "]";
    case KEY_BACKSLASH: return "\\";
    case KEY_MINUS: return "-";
    case KEY_EQUAL: return "=";
    case KEY_GRAVE: return "`";
    default: return "Key " + std::to_string(key);
    }
}

std::string ps2ControllerInputName(int code)
{
    if (code == 0)
        return "-";
#if defined(PS2X_SDL_INPUT)
    const SDL_GameControllerType type = firstControllerType();
    const bool sony = type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5;
    const bool nintendo = type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO ||
                          type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
#else
    const bool sony = false, nintendo = false;
#endif
    const auto pick = [&](const char *xbox, const char *playstation, const char *switchPad)
    { return std::string(sony ? playstation : nintendo ? switchPad : xbox); };

    if (code >= kPadCodeAxis && code < kCodeCount)
    {
        const int axis = (code - kPadCodeAxis) / 2;
        const bool positive = ((code - kPadCodeAxis) & 1) != 0;
        switch (axis)
        {
        case kAxisLeftX: return positive ? "Left stick Right" : "Left stick Left";
        case kAxisLeftY: return positive ? "Left stick Down" : "Left stick Up";
        case kAxisRightX: return positive ? "Right stick Right" : "Right stick Left";
        case kAxisRightY: return positive ? "Right stick Down" : "Right stick Up";
        case kAxisTriggerLeft: return pick("LT", "L2", "ZL");
        case kAxisTriggerRight: return pick("RT", "R2", "ZR");
        default: return "Axis " + std::to_string(axis);
        }
    }
    switch (code - kPadCodeButton)
    {
    case kBtnA: return pick("A", "Cross", "B");
    case kBtnB: return pick("B", "Circle", "A");
    case kBtnX: return pick("X", "Square", "Y");
    case kBtnY: return pick("Y", "Triangle", "X");
    case kBtnBack: return pick("View/Back", "Share/Create", "-");
    case kBtnGuide: return pick("Xbox button", "PS button", "Home");
    case kBtnStart: return pick("Menu/Start", "Options", "+");
    case kBtnLeftStick: return "Left stick press";
    case kBtnRightStick: return "Right stick press";
    case kBtnLeftShoulder: return pick("LB", "L1", "L");
    case kBtnRightShoulder: return pick("RB", "R1", "R");
    case kBtnDpadUp: return "D-pad Up";
    case kBtnDpadDown: return "D-pad Down";
    case kBtnDpadLeft: return "D-pad Left";
    case kBtnDpadRight: return "D-pad Right";
    case 15: return pick("Share", "Mic", "Capture");
    case 16: return "Paddle 1";
    case 17: return "Paddle 2";
    case 18: return "Paddle 3";
    case 19: return "Paddle 4";
    case 20: return "Touchpad";
    default: return "Button " + std::to_string(code - kPadCodeButton);
    }
}

std::string ps2InputPortControllerName(int port)
{
    if (port < 0 || port >= kPs2InputPorts)
        return "";
#if defined(PS2X_SDL_INPUT)
    std::string names;
    for (SDL_GameController *c : g_portControllers[port])
    {
        if (!names.empty())
            names += ", ";
        const char *name = SDL_GameControllerName(c);
        names += name ? name : "Controller";
    }
    return names;
#else
    return portHasController(port) ? GetGamepadName(0) : "";
#endif
}

bool ps2InputPadSwapPossible()
{
#if defined(PS2X_SDL_INPUT)
    return !singlePort() && g_portOwner[0] >= 0 && g_portOwner[1] >= 0;
#else
    return false;
#endif
}

namespace
{
    // Which controller drives which pad port: at start-up and whenever a port's controller changes.
    void logPorts()
    {
        const std::string port0 = ps2InputPortControllerName(0), port1 = ps2InputPortControllerName(1);
        std::printf("[input] port 0: keyboard%s%s\n", port0.empty() ? "" : " + ", port0.c_str());
        if (singlePort())
            std::printf("[input] port 1: mirrors port 0 (PS2X_PAD_PORTS=1)\n");
        else
            std::printf("[input] port 1: %s%s\n", port1.empty() ? "none" : port1.c_str(),
                        (!port1.empty() && !inputLive()) ? " (PS2X_INPUT_LIVE=0: reads as unplugged)" : "");
        std::fflush(stdout);
    }

    void initPorts()
    {
#if defined(PS2X_SDL_INPUT)
        mapPorts();
#endif
        updatePortConnection();
        logPorts();
    }
}

void ps2InputSwapPlayers()
{
#if defined(PS2X_SDL_INPUT)
    if (!ps2InputPadSwapPossible())
        return;
    std::swap(g_portOwner[0], g_portOwner[1]);
    initPorts();
#endif
}

void ps2InputInit()
{
#if defined(PS2X_SDL_INPUT)
    // Controllers only; the window, graphics and sound stay with raylib.
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"); // positional: south is always Cross
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
    {
        std::printf("[input] SDL controller support unavailable: %s\n", SDL_GetError());
        std::fflush(stdout);
        initPorts();
        return;
    }
    // Optional extra mappings for unusual controllers (SDL_GameControllerDB format).
    if (FileExists("gamecontrollerdb.txt"))
        SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    g_sdlReady = true;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        openController(i);
#endif
    initPorts();
}

void ps2InputShutdown()
{
#if defined(PS2X_SDL_INPUT)
    for (SDL_GameController *c : g_controllers)
    {
        SDL_GameControllerRumble(c, 0, 0, 0);
        SDL_GameControllerClose(c);
    }
    g_controllers.clear();
    mapPorts();
    if (g_sdlReady)
        SDL_Quit();
    g_sdlReady = false;
#endif
}

void ps2InputUpdate()
{
    const PS2HostSettings &s = ps2Settings();
#if defined(PS2X_SDL_INPUT)
    if (g_sdlReady)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_CONTROLLERDEVICEADDED)
                openController(event.cdevice.which);
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
            {
                SDL_GameController *gone = SDL_GameControllerFromInstanceID(event.cdevice.which);
                if (gone)
                {
                    g_controllers.erase(std::remove(g_controllers.begin(), g_controllers.end(), gone), g_controllers.end());
                    SDL_GameControllerClose(gone);
                    std::printf("[input] controller disconnected\n");
                    std::fflush(stdout);
                }
            }
        }
    }
    const bool remapped = mapPorts();
#else
    const bool remapped = false;
#endif
    updatePortConnection();
    if (remapped)
        logPorts();

    // Edges, for the menu button and for rebinding (any controller).
    g_menuPressed = false;
    g_captured = 0;
    for (int code = kPadCodeButton; code < kCodeCount; ++code)
    {
        const bool down = codeDown(code);
        if (down && !g_wasDown[code])
        {
            if (g_captured == 0)
                g_captured = code;
            if (code == s.pads[kPadOpenMenu][0] || code == s.pads[kPadOpenMenu][1])
                g_menuPressed = true;
        }
        g_wasDown[code] = down;
    }

    const bool s_live = inputLive();
    if (!s_live)
        g_menuPressed = false;
    const bool blocked = !s_live || ps2GameInputBlocked().load(std::memory_order_relaxed);
    g_lightLevel.store(std::clamp(s.lightPressLevel, 0, 100) * 255 / 100, std::memory_order_relaxed);
    g_lightButtons.store(s.lightPressButtons, std::memory_order_relaxed);
    // The same bindings for both ports: port 0 reads the keyboard and its controller, port 1 its controller only.
    buildPort(0, true, blocked, s);
    if (!singlePort())
        buildPort(1, false, blocked, s);

#if defined(PS2X_SDL_INPUT)
    // Rumble: the DualShock 2's large motor drives the low-frequency (heavy) motor, the small one
    // the high-frequency motor. Games set the motors and leave them running, so refresh while on.
    // Each port drives only the controllers mapped to it.
    const double now = GetTime();
    const float strength = static_cast<float>(s.rumble) / 100.0f;
    const bool testing = now < g_testRumbleUntil;
    const uint16_t testOut = static_cast<uint16_t>(std::clamp(strength, 0.0f, 1.0f) * 65535.0f);
    if (testing && testOut != 0u && now - g_testSentAt > 0.1)
    {
        // "Test rumble" (menu): every connected controller, on a port or not.
        for (SDL_GameController *c : g_controllers)
            SDL_GameControllerRumble(c, testOut, testOut, 250u);
        g_testSentAt = now;
    }
    for (int port = 0; port < kPs2InputPorts; ++port)
    {
        if (testing)
        {
            // Counted as sent, so the game's values (or off) go out again when the test ends.
            g_sentLow[port] = g_sentHigh[port] = testOut;
            g_sentAt[port] = now;
            continue;
        }
        const float low = blocked ? 0.0f : static_cast<float>(g_rumbleLarge[port].load(std::memory_order_relaxed)) / 255.0f;
        const float high = (!blocked && g_rumbleSmall[port].load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
        const uint16_t lowOut = static_cast<uint16_t>(std::clamp(low * strength, 0.0f, 1.0f) * 65535.0f);
        const uint16_t highOut = static_cast<uint16_t>(std::clamp(high * strength, 0.0f, 1.0f) * 65535.0f);
        const bool changed = lowOut != g_sentLow[port] || highOut != g_sentHigh[port];
        if (changed || ((lowOut | highOut) != 0u && now - g_sentAt[port] > 0.1))
        {
            for (SDL_GameController *c : g_portControllers[port])
                SDL_GameControllerRumble(c, lowOut, highOut, (lowOut | highOut) != 0u ? 250u : 0u);
            g_sentLow[port] = lowOut;
            g_sentHigh[port] = highOut;
            g_sentAt[port] = now;
        }
    }
#endif
}

Ps2PadSnapshot ps2InputSnapshot()
{
    return ps2InputSnapshot(0);
}

Ps2PadSnapshot ps2InputSnapshot(int port)
{
    if (port < 0 || port >= kPs2InputPorts)
        return Ps2PadSnapshot{};
    const uint64_t v = g_snapshot[singlePort() ? 0 : port].load(std::memory_order_acquire);
    Ps2PadSnapshot s{};
    s.buttons = static_cast<uint16_t>(v);
    s.rx = static_cast<uint8_t>(v >> 16);
    s.ry = static_cast<uint8_t>(v >> 24);
    s.lx = static_cast<uint8_t>(v >> 32);
    s.ly = static_cast<uint8_t>(v >> 40);
    return s;
}

bool ps2InputPortConnected(int port)
{
    if (singlePort())
        return port >= 0;
    if (port == 0)
        return true; // the keyboard is always there
    return port == 1 && g_port1Connected.load(std::memory_order_relaxed);
}

void ps2InputPressure(uint16_t buttons, bool forceLight, uint8_t out[kPadPressureCount])
{
    ps2InputPressure(0, buttons, forceLight, out);
}

void ps2InputPressure(int port, uint16_t buttons, bool forceLight, uint8_t out[kPadPressureCount])
{
    port = (singlePort() || port < 0 || port >= kPs2InputPorts) ? 0 : port;
    const auto &levels = g_level[port];
    const bool light = forceLight || g_lightOn[port].load(std::memory_order_relaxed);
    const int lightLevel = g_lightLevel.load(std::memory_order_relaxed);
    const int lightButtons = g_lightButtons.load(std::memory_order_relaxed);
    for (int i = 0; i < kPadPressureCount; ++i)
    {
        const int action = kPressureActions[i];
        if ((buttons & kPadBits[action]) != 0u)
        {
            out[i] = 0u; // released
            continue;
        }
        int value = levels[action].load(std::memory_order_relaxed);
        if (value == 0)
            value = 255; // pressed by an input script
        if (light && (lightButtons & (1 << action)) != 0)
            value = std::min(value, lightLevel);
        out[i] = static_cast<uint8_t>(std::max(value, 1));
    }
}

bool ps2InputLightPressOn()
{
    return g_lightOn[0].load(std::memory_order_relaxed);
}

bool ps2PadPressureEnabled()
{
    static const bool enabled = []
    {
        const char *env = std::getenv("PS2X_PAD_PRESSURE");
        return !(env != nullptr && *env == '0');
    }();
    return enabled;
}

bool ps2InputMenuPressed()
{
    return g_menuPressed;
}

int ps2InputCapturedControllerInput()
{
    return g_captured;
}

Ps2MenuPad ps2InputMenuPad()
{
    Ps2MenuPad p{};
    if (!inputLive())
        return p;
    const auto axisNegative = [](int a) { return kPadCodeAxis + a * 2; };
    p.up = codeDown(button(kBtnDpadUp)) || codeDown(axisNegative(kAxisLeftY));
    p.down = codeDown(button(kBtnDpadDown)) || codeDown(axisPositive(kAxisLeftY));
    p.left = codeDown(button(kBtnDpadLeft)) || codeDown(axisNegative(kAxisLeftX));
    p.right = codeDown(button(kBtnDpadRight)) || codeDown(axisPositive(kAxisLeftX));
    p.confirm = codeDown(button(kBtnA));
    p.back = codeDown(button(kBtnB));
    p.l1 = codeDown(button(kBtnLeftShoulder));
    p.r1 = codeDown(button(kBtnRightShoulder));
    return p;
}

std::string ps2InputControllerNames()
{
#if defined(PS2X_SDL_INPUT)
    std::string names;
    for (SDL_GameController *c : g_controllers)
    {
        if (!names.empty())
            names += ", ";
        const char *name = SDL_GameControllerName(c);
        names += name ? name : "Controller";
    }
    return names;
#else
    return IsGamepadAvailable(0) ? GetGamepadName(0) : "";
#endif
}

void ps2InputSetRumble(int port, bool smallMotor, float largeMotor)
{
    if (port < 0 || port >= kPs2InputPorts || (port != 0 && singlePort()))
        return;
    // An empty port keeps its motors off: a pad plugged in later stays still until the game sends motor values again.
    if (!ps2InputPortConnected(port))
        return;
    g_rumbleSmall[port].store(smallMotor, std::memory_order_relaxed);
    g_rumbleLarge[port].store(static_cast<int>(std::clamp(largeMotor, 0.0f, 1.0f) * 255.0f), std::memory_order_relaxed);
}

void ps2InputTestRumble()
{
    g_testRumbleUntil = GetTime() + 0.6;
}
