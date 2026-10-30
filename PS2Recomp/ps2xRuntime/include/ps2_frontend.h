#ifndef PS2_FRONTEND_H
#define PS2_FRONTEND_H

#include "ps2_input.h" // Ps2MenuPad

#include <cstdint>
#include <filesystem>
#include <functional>

class PS2Runtime;

// Boot front-end: a title screen (Start, Settings, Disc, Quit) and a first-run disc check, so a player starts the
// game from the exe alone with their own disc image. The check reads the image's ISO 9660 directory, makes sure the
// disc boots the program this build was translated from (name in SYSTEM.CNF, size and CRC-32 of the ELF), copies the
// ELF, SYSTEM.CNF and the module folders the IOP loads from into <settings folder>\game, and stores the image path in
// settings.ini (disc_image). The game data itself is read from the image while playing (PS2X_CD_IMAGE).
//
// Opt-in per game: only a game module that calls ps2RegisterFrontEnd gets it (ZOE: ZOE-Recompiled/src/
// zoe_frontend.cpp), and only for a run started without an ELF argument, so play.bat and test runs boot straight in.
// PS2X_FRONTEND=0 turns it off (the runner then needs the ELF argument, as before); PS2X_FRONTEND=1 shows it even
// when an ELF argument is given (the argument is then ignored).
struct PS2FrontEndGame
{
    const char *elfName;   // boot file named in SYSTEM.CNF ("SLUS_201.48"); the runtime keys settings and states by it
    const char *discLabel; // shown on the screens ("SLUS-201.48")
    const char *kicker;    // small line above the title
    const char *titleLine1;
    const char *titleLine2;
    const char *discHelp;         // first-run explanation: which disc to pick
    const char *versionStepLabel; // disc check rows ("Version: retail or undub", "Copy game program and sound modules")
    const char *copyStepLabel;
    // The ELF this build was translated from, as stored on the disc. Another version would run the wrong code.
    uint32_t elfSize;
    uint32_t elfCrc32;
    const char *const *copyDirs;        // disc folders the IOP loads modules from; nullptr-terminated
    const char *const *defaultSwitches; // "PS2X_NAME=value" set for front-end runs unless the environment has them; nullptr-terminated
    bool widescreenOption; // the game module will offer Widescreen this run: show it in Settings before the game starts
    const char *windowTitle = nullptr; // window title for this game (front-end and ELF-argument runs); nullptr = default
};

void ps2RegisterFrontEnd(const PS2FrontEndGame &game);
const PS2FrontEndGame *ps2FrontEndGame(); // nullptr when no game module registered one

// A game registered a front-end, and the run has no ELF argument (or PS2X_FRONTEND=1); PS2X_FRONTEND=0 = never.
bool ps2FrontEndWanted(int argc, char *argv[]);

// Sets the game's default switches; environment variables keep priority. Call before PS2Runtime::initialize.
void ps2FrontEndApplyDefaults();

// Where the disc check puts the game program: <settings folder>\game\<elfName>. Valid after ps2LoadSettings.
std::filesystem::path ps2FrontEndElfPath();

// The settings menu (Esc menu) as the front-end's Settings page.
struct PS2FrontEndHooks
{
    std::function<void()> openSettings;
    std::function<void()> closeSettings;
    std::function<bool()> settingsOpen;
    std::function<void()> updateSettings; // every frame while open, before the ImGui frame
    std::function<void()> drawSettings;   // inside the ImGui frame
    // Every frame, before the ImGui frame: controller -> ImGui navigation keys (rlImGui does not see SDL controllers);
    // returns the controller's menu buttons.
    std::function<Ps2MenuPad()> feedControllerNav;
};

// Runs the front-end in the runtime's window (after PS2Runtime::initialize, before loadELF). true = the player chose
// Start: the game program is at ps2FrontEndElfPath(), the working folder is its folder (as play.bat sets it) and
// PS2X_STATE_DIR points at <settings folder>\states unless the environment set it. false = Quit or window closed.
bool ps2RunFrontEnd(PS2Runtime &runtime, const PS2FrontEndHooks &hooks);

// After PS2Runtime::loadELF: points the CD image at the checked disc unless PS2X_CD_IMAGE is set.
void ps2FrontEndConfigureRun();

// Windows file dialog for a disc image (ps2_frontend_dialog.cpp, apart because windows.h clashes with raylib).
// false = cancelled or not available.
bool ps2FrontEndPickFile(void *parentWindow, const std::filesystem::path &current, std::filesystem::path &picked);

#endif // PS2_FRONTEND_H
