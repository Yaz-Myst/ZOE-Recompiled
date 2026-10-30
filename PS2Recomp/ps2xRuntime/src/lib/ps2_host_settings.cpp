#include "ps2_host_settings.h"
#include "ps2_game_clock.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    PS2HostSettings g_settings;
    std::filesystem::path g_settingsPath;

    // One saved setting: a flag, a number kept within [min, max], or a line of text.
    struct Entry
    {
        std::string name;
        bool *flag;
        int *number;
        int min;
        int max;
        std::string *text = nullptr;
    };

    // Names used for the bindings in settings.ini, in Ps2PadAction order.
    constexpr const char *kActionKeys[kPadActionCount] = {"up", "down", "left", "right", "cross", "circle", "square",
                                                          "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "start",
                                                          "select", "menu", "light_press"};

    std::vector<Entry> entries()
    {
        PS2HostSettings &s = g_settings;
        std::vector<Entry> list = {
            {"overlay_fps", &s.overlayFps, nullptr, 0, 0},
            {"overlay_vps", &s.overlayVps, nullptr, 0, 0},
            {"overlay_speed", &s.overlaySpeed, nullptr, 0, 0},
            {"overlay_frame_time", &s.overlayFrameTime, nullptr, 0, 0},
            {"overlay_audio", &s.overlayAudio, nullptr, 0, 0},
            {"overlay_corner", nullptr, &s.overlayCorner, 0, 3},
            {"fullscreen", &s.fullscreen, nullptr, 0, 0},
            {"stretch", &s.stretch, nullptr, 0, 0},
            {"widescreen", &s.widescreen, nullptr, 0, 0},
            {"progressive", &s.progressive, nullptr, 0, 0},
            {"upscale", nullptr, &s.upscale, 1, 4},
            {"volume", nullptr, &s.volume, 0, 100},
            {"mute", &s.mute, nullptr, 0, 0},
            {"deadzone", nullptr, &s.deadzone, 0, 90},
            {"rumble", nullptr, &s.rumble, 0, 100},
            {"light_press_level", nullptr, &s.lightPressLevel, 5, 95},
            {"light_press_toggle", &s.lightPressToggle, nullptr, 0, 0},
            {"light_press_buttons", nullptr, &s.lightPressButtons, 0, 0x0FFF},
            {"analog_triggers", &s.analogTriggers, nullptr, 0, 0},
            {"disc_image", nullptr, nullptr, 0, 0, &s.discImage},
        };
        for (int action = 0; action < kPadActionCount; ++action)
        {
            for (int slot = 0; slot < 2; ++slot)
            {
                const std::string suffix = std::string(kActionKeys[action]) + "_" + std::to_string(slot + 1);
                list.push_back({"key_" + suffix, nullptr, &s.keys[action][slot], 0, 511});
                list.push_back({"pad_" + suffix, nullptr, &s.pads[action][slot], 0, 127});
            }
        }
        return list;
    }

    std::string trim(const std::string &text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        const size_t last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    }

    // Game options (see ps2RegisterGameOption). g_gameValues holds the saved "game_<key>" values, registered or not;
    // g_gameOptions[0, count) are the registered ones, never changed after registration except their on flag.
    constexpr const char *kGameOptionPrefix = "game_";
    struct GameOption
    {
        std::string key;
        std::string label;
        std::string help;
        std::atomic<bool> on{false};
    };
    std::mutex g_gameOptionMutex;
    std::map<std::string, bool> g_gameValues;
    GameOption g_gameOptions[kMaxGameOptions];
    std::atomic<int> g_gameOptionCount{0};

    GameOption *gameOption(int index)
    {
        return index >= 0 && index < g_gameOptionCount.load(std::memory_order_acquire) ? &g_gameOptions[index] : nullptr;
    }
}

PS2HostSettings &ps2Settings()
{
    return g_settings;
}

namespace
{
    // Where per-game settings, extracted game files, memory cards and save states live.
    // Windows keeps the existing %LOCALAPPDATA%\PS2Recomp layout. Linux follows the XDG base
    // directory spec: $XDG_DATA_HOME/PS2Recomp (default ~/.local/share/PS2Recomp), so an AppImage
    // and a native install share the same persistent data even though the AppDir is read-only.
    std::filesystem::path settingsBaseDirectory()
    {
#if defined(_WIN32)
        if (const char *appData = std::getenv("LOCALAPPDATA"); appData && *appData)
            return std::filesystem::path(appData) / "PS2Recomp";
#else
        if (const char *dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome)
            return std::filesystem::path(dataHome) / "PS2Recomp";
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".local" / "share" / "PS2Recomp";
#endif
        // Last resort: the current directory (portable/USB runs). Never assume the executable's
        // own folder is writable: AppImage mounts it read-only.
        return std::filesystem::current_path() / "PS2Recomp";
    }
}

void ps2LoadSettings(const std::string &gameId)
{
    const std::filesystem::path base = settingsBaseDirectory();
    g_settingsPath = base / (gameId.empty() ? std::string("default") : gameId) / "settings.ini";

    ps2DefaultControls(g_settings);
    const std::vector<Entry> known = entries();
    std::ifstream in(g_settingsPath);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key.rfind(kGameOptionPrefix, 0) == 0 && key.size() > std::strlen(kGameOptionPrefix))
        {
            std::lock_guard<std::mutex> lock(g_gameOptionMutex);
            g_gameValues[key.substr(std::strlen(kGameOptionPrefix))] = value == "1";
            continue;
        }
        for (const Entry &entry : known)
        {
            if (key != entry.name)
                continue;
            if (entry.text)
                *entry.text = value;
            else if (entry.flag)
                *entry.flag = value == "1";
            else
                *entry.number = std::clamp(std::atoi(value.c_str()), entry.min, entry.max);
        }
    }
    ps2ProgressiveDisplay().store(g_settings.progressive, std::memory_order_relaxed);
    ps2WidescreenOn().store(g_settings.widescreen, std::memory_order_relaxed);
    std::cout << "[settings] " << (in.is_open() ? "loaded " : "defaults; will save to ") << g_settingsPath.string() << std::endl;
}

bool ps2SaveSettings()
{
    if (g_settingsPath.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(g_settingsPath.parent_path(), ec);
    const std::filesystem::path temp = g_settingsPath.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
        {
            std::cerr << "[settings] could not write " << temp.string() << std::endl;
            return false;
        }
        out << "# PS2Recomp settings (edited by the in-game menu)\n";
        for (const Entry &entry : entries())
        {
            if (entry.text)
                out << entry.name << " = " << *entry.text << "\n";
            else
                out << entry.name << " = " << (entry.flag ? (*entry.flag ? 1 : 0) : *entry.number) << "\n";
        }
        std::lock_guard<std::mutex> lock(g_gameOptionMutex);
        for (const auto &[key, on] : g_gameValues)
            out << kGameOptionPrefix << key << " = " << (on ? 1 : 0) << "\n";
    }
    std::filesystem::rename(temp, g_settingsPath, ec);
    if (ec)
    {
        std::cerr << "[settings] could not save " << g_settingsPath.string() << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

std::filesystem::path ps2SettingsDirectory()
{
    return g_settingsPath.parent_path();
}

PS2PerfCounters &ps2Perf()
{
    static PS2PerfCounters counters;
    return counters;
}

std::atomic<bool> &ps2GameInputBlocked()
{
    static std::atomic<bool> blocked{false};
    return blocked;
}

std::atomic<bool> &ps2ProgressiveDisplay()
{
    static std::atomic<bool> progressive{true};
    return progressive;
}

bool ps2ProgressiveDisplayActive()
{
    static const int forced = []
    {
        const char *e = std::getenv("PS2X_DISPLAY");
        if (!e)
            return -1;
        const std::string v(e);
        return v == "progressive" ? 1 : v == "interlaced" ? 0 : -1;
    }();
    return forced >= 0 ? forced == 1 : ps2ProgressiveDisplay().load(std::memory_order_relaxed);
}

namespace
{
    std::atomic<float> &gameDisplayAspect()
    {
        static std::atomic<float> aspect{4.0f / 3.0f};
        return aspect;
    }
}

float ps2GameDisplayAspect()
{
    return gameDisplayAspect().load(std::memory_order_relaxed);
}

void ps2SetGameDisplayAspect(float aspect)
{
    if (aspect >= 1.0f && aspect <= 4.0f)
        gameDisplayAspect().store(aspect, std::memory_order_relaxed);
}

namespace
{
    std::atomic<bool> &widescreenSupported()
    {
        static std::atomic<bool> supported{false};
        return supported;
    }
}

void ps2SetGameWidescreenSupported(bool supported)
{
    widescreenSupported().store(supported, std::memory_order_relaxed);
}

bool ps2GameWidescreenSupported()
{
    return widescreenSupported().load(std::memory_order_relaxed);
}

std::atomic<bool> &ps2WidescreenOn()
{
    static std::atomic<bool> on{false};
    return on;
}

bool ps2WidescreenActive()
{
    return ps2GameWidescreenSupported() && ps2WidescreenOn().load(std::memory_order_relaxed);
}

void ps2SetWidescreen(bool on)
{
    g_settings.widescreen = on; // the value ps2SaveSettings writes
    ps2WidescreenOn().store(on, std::memory_order_relaxed);
    std::cout << "[settings] widescreen " << (on ? "on" : "off") << std::endl;
}

void ps2SetWidescreenSession(bool on)
{
    // g_settings.widescreen (the saved value) is left alone: an environment switch or a test must not change the
    // player's settings.ini
    ps2WidescreenOn().store(on, std::memory_order_relaxed);
    std::cout << "[settings] widescreen " << (on ? "on" : "off") << " (this session only)" << std::endl;
}

int ps2RegisterGameOption(const char *key, const char *label, const char *help)
{
    std::lock_guard<std::mutex> lock(g_gameOptionMutex);
    const int count = g_gameOptionCount.load(std::memory_order_relaxed);
    if (key == nullptr || key[0] == '\0' || count >= kMaxGameOptions)
    {
        std::cerr << "[settings] game option \"" << (key ? key : "") << "\" not registered (empty key or "
                  << kMaxGameOptions << " already)" << std::endl;
        return -1;
    }
    for (int i = 0; i < count; ++i)
        if (g_gameOptions[i].key == key)
            return i;
    GameOption &option = g_gameOptions[count];
    option.key = key;
    option.label = label ? label : key;
    option.help = help ? help : "";
    const auto saved = g_gameValues.find(option.key);
    option.on.store(saved != g_gameValues.end() && saved->second, std::memory_order_relaxed);
    g_gameOptionCount.store(count + 1, std::memory_order_release); // the menu sees the slot only once it is filled
    std::cout << "[settings] game option " << kGameOptionPrefix << option.key << " "
              << (option.on.load(std::memory_order_relaxed) ? "on" : "off") << std::endl;
    return count;
}

int ps2GameOptionCount()
{
    return g_gameOptionCount.load(std::memory_order_acquire);
}

const char *ps2GameOptionLabel(int index)
{
    const GameOption *option = gameOption(index);
    return option ? option->label.c_str() : "";
}

const char *ps2GameOptionHelp(int index)
{
    const GameOption *option = gameOption(index);
    return option ? option->help.c_str() : "";
}

bool ps2GameOptionOn(int index)
{
    const GameOption *option = gameOption(index);
    return option && option->on.load(std::memory_order_relaxed);
}

void ps2SetGameOption(int index, bool on)
{
    GameOption *option = gameOption(index);
    if (!option)
        return;
    {
        std::lock_guard<std::mutex> lock(g_gameOptionMutex);
        g_gameValues[option->key] = on; // the value ps2SaveSettings writes
    }
    option->on.store(on, std::memory_order_relaxed);
    std::cout << "[settings] game option " << kGameOptionPrefix << option->key << " " << (on ? "on" : "off") << std::endl;
}

void ps2SetGameOptionSession(int index, bool on)
{
    // g_gameValues (the saved value) is left alone: an environment switch or a test must not change settings.ini
    GameOption *option = gameOption(index);
    if (!option)
        return;
    option->on.store(on, std::memory_order_relaxed);
    std::cout << "[settings] game option " << kGameOptionPrefix << option->key << " " << (on ? "on" : "off")
              << " (this session only)" << std::endl;
}

namespace
{
    std::mutex g_movieMutex;
    std::vector<uint32_t> g_moviePlaying; // decoders that asked for pictures (sceMpegGetPicture) and are not deleted
    std::atomic<bool> g_movieActive{false};
}

void ps2MovieSetPlaying(uint32_t instance, bool playing)
{
    std::lock_guard<std::mutex> lock(g_movieMutex);
    const auto it = std::find(g_moviePlaying.begin(), g_moviePlaying.end(), instance);
    // one line per change (a few per movie): the decoder, the new count and the guest vsync tick, so a window-shape
    // change can be lined up with the game's own timeline
    if (playing && it == g_moviePlaying.end())
    {
        g_moviePlaying.push_back(instance);
        std::cout << "[display] movie decoder 0x" << std::hex << instance << std::dec << " playing (first GetPicture call), decoders playing "
                  << g_moviePlaying.size() << ", tick " << ps2CurrentVsyncTick() << std::endl;
    }
    else if (!playing && it != g_moviePlaying.end())
    {
        g_moviePlaying.erase(it);
        std::cout << "[display] movie decoder 0x" << std::hex << instance << std::dec << " ended, decoders playing "
                  << g_moviePlaying.size() << ", tick " << ps2CurrentVsyncTick() << std::endl;
    }
    g_movieActive.store(!g_moviePlaying.empty(), std::memory_order_release);
}

void ps2MovieReset()
{
    std::lock_guard<std::mutex> lock(g_movieMutex);
    g_moviePlaying.clear();
    g_movieActive.store(false, std::memory_order_release);
}

bool ps2MovieActive()
{
    return g_movieActive.load(std::memory_order_acquire);
}
