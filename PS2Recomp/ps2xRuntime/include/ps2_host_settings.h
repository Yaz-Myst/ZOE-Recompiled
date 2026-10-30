#ifndef PS2_HOST_SETTINGS_H
#define PS2_HOST_SETTINGS_H

#include "ps2_input.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// Player-facing settings (the in-game menu edits these). Saved per game in
// %LOCALAPPDATA%\PS2Recomp\<game id>\settings.ini.
struct PS2HostSettings
{
    // Performance overlay
    bool overlayFps = false;
    bool overlayVps = false;
    bool overlaySpeed = false;
    bool overlayFrameTime = false;
    bool overlayAudio = false;
    int overlayCorner = 0; // 0 top left, 1 top right, 2 bottom left, 3 bottom right

    // Graphics
    bool fullscreen = false;
    bool stretch = false; // false: keep the PS2's 4:3 picture
    // Widescreen 16:9 (only for games whose override registered support, see ps2SetGameWidescreenSupported): the
    // game renders a wider view and the window shows it at the game's wide aspect. Off = the stock 4:3 game.
    bool widescreen = false;
    // Games that draw alternate frames half a line apart for an interlaced TV: true draws every frame at
    // the same place (steady, like a PC game); false keeps the offset and blends frames like a TV.
    bool progressive = true;
    // Internal resolution: the GPU renderer draws the picture this many times larger in each direction
    // (1 = the PS2's own; 2, 3, 4). Higher costs graphics-card time.
    int upscale = 1;

    // Sound
    int volume = 100; // percent
    bool mute = false;

    // Controls: two keyboard keys (raylib key codes) and two controller inputs (see ps2_input.h)
    // per PS2 button. Filled by ps2DefaultControls.
    std::array<std::array<int, 2>, kPadActionCount> keys{};
    std::array<std::array<int, 2>, kPadActionCount> pads{};
    int deadzone = 15; // percent of stick travel ignored around the centre
    int rumble = 60;   // percent, 0 = off

    // Pressure-sensitive buttons (DualShock 2). The light-press modifier (binding kPadLightPress) lowers the
    // pressure of the pressed buttons in lightPressButtons (bits = 1 << Ps2PadAction) to lightPressLevel percent.
    int lightPressLevel = 40;
    bool lightPressToggle = false;  // false: hold the modifier; true: each press switches it on/off
    int lightPressButtons = 0x0FFF; // D-pad, face buttons, L1/R1/L2/R2
    // Analog triggers: a button bound to a controller trigger reports how far the trigger is pulled, and counts
    // as pressed from a light touch instead of half way.
    bool analogTriggers = false;

    // Disc image (UTF-8 path) the boot front-end checked and took the game program from; empty = not set up yet
    // (see ps2_frontend.h). PS2X_CD_IMAGE still wins for a run that sets it.
    std::string discImage;
};

void ps2DefaultControls(PS2HostSettings &settings);

// Only touched from the window thread (the menu and the presentation code).
PS2HostSettings &ps2Settings();
void ps2LoadSettings(const std::string &gameId);
bool ps2SaveSettings(); // false = settings.ini could not be written (the reason is on stderr)
// Folder that holds settings.ini (%LOCALAPPDATA%\PS2Recomp\<game id>); empty before ps2LoadSettings.
std::filesystem::path ps2SettingsDirectory();

// Counters the performance overlay samples. Written by the EE scheduler at every vblank.
struct PS2PerfCounters
{
    std::atomic<uint64_t> vblanks{0};
    std::atomic<uint64_t> gameFrames{0}; // vblanks at which the displayed framebuffer changed
    std::atomic<uint64_t> eeCycles{0};
};
PS2PerfCounters &ps2Perf();

// Set while the menu is open: the game sees an idle controller.
std::atomic<bool> &ps2GameInputBlocked();

// PS2HostSettings::progressive for the render threads (kept in step by the window thread).
// PS2X_DISPLAY=progressive|interlaced overrides the setting.
std::atomic<bool> &ps2ProgressiveDisplay();
bool ps2ProgressiveDisplayActive();

// Width/height the window shows the picture at (letterboxed; the "stretch" setting still fills the window).
// Default 4/3, the PS2's TV shape. A game override that renders a wider view (e.g. a widescreen camera patch)
// calls ps2SetGameDisplayAspect(16.0f / 9.0f) at start-up. Values outside 1..4 are ignored.
float ps2GameDisplayAspect();
void ps2SetGameDisplayAspect(float aspect);

// Widescreen option. A game override that can render a wider view calls ps2SetGameWidescreenSupported(true) at
// start-up (and ps2SetGameDisplayAspect with the wide shape); only then does the menu show the checkbox. The "on"
// state is an atomic for the render, game and window threads: ps2WidescreenOn() is kept in step with
// PS2HostSettings::widescreen by the menu and by ps2SetWidescreen() (which a game override may call to apply an
// environment switch or a test). ps2WidescreenActive() = supported && on: the game override's wrappers and the
// window aspect read it.
void ps2SetGameWidescreenSupported(bool supported);
bool ps2GameWidescreenSupported();
std::atomic<bool> &ps2WidescreenOn();
bool ps2WidescreenActive();
void ps2SetWidescreen(bool on);        // a change the player made (menu): also saved in settings.ini
void ps2SetWidescreenSession(bool on); // an environment switch or a test: this session only, never saved

// Game options: on/off extras a game override offers (an unlock, a gameplay switch). The override registers each one
// at start-up; the menu then shows an EXTRAS section with one row per option (no section when the game registered
// none). Each option is saved in settings.ini as "game_<key>" and is off until the player turns it on. Lines of
// options not registered this session are kept as they are. ps2GameOptionOn() may be called from any thread (the
// game thread reads it while running); an index of -1 reads as off.
constexpr int kMaxGameOptions = 8;
int ps2RegisterGameOption(const char *key, const char *label, const char *help); // index, or -1 (table full)
int ps2GameOptionCount();
const char *ps2GameOptionLabel(int index);
const char *ps2GameOptionHelp(int index);
bool ps2GameOptionOn(int index);
void ps2SetGameOption(int index, bool on);        // a change the player made (menu): also saved in settings.ini
void ps2SetGameOptionSession(int index, bool on); // an environment switch or a test: this session only, never saved

// Movie (MPEG) playback signal, by the lifetime of the decoder: an MPEG decoder instance (the address the game passes
// to the sceMpeg* calls) is "playing" from its first sceMpegGetPicture call (it asks for pictures, so the movie screen is up even before the first picture is ready) until the game deletes
// it (sceMpegDelete) or the stand-in state is reset. No timer: a stall in the stream does not end a movie, and the end
// of a movie is seen at once. A decoder that is only created and fed (audio-only streams, a second decoder kept for a
// cutscene) never plays. ps2MovieSetPlaying() is called by the MPEG stand-ins; ps2MovieActive() = at least one decoder
// is playing (the game-thread view: game overrides use it to leave their 16:9 adjustments off). Movies are 4:3 content:
// the WINDOW does not read ps2MovieActive() directly but the value recorded with the displayed frame (see
// ps2_gs_worker::flipMovie), so one presented frame is never shown in the wrong shape.
void ps2MovieSetPlaying(uint32_t instance, bool playing);
void ps2MovieReset();
bool ps2MovieActive();

#endif // PS2_HOST_SETTINGS_H
