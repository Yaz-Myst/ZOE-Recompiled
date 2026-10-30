#ifndef PS2_MENU_H
#define PS2_MENU_H

#include "ps2_input.h" // Ps2MenuPad

#include <array>
#include <cstddef>
#include <cstdint>

class PS2Runtime;
struct ImFont;

// The player-facing in-game menu (Esc) and the performance overlay. Runs on the window thread,
// inside the Dear ImGui frame shared with the developer debugger (F1).
class PS2Menu
{
public:
    void initialize(); // after rlImGuiSetup
    void update(PS2Runtime &runtime); // every host frame: hotkeys and performance sampling
    bool needsImGuiFrame() const;
    void draw(PS2Runtime &runtime); // inside rlImGuiBegin/rlImGuiEnd

    // Boot front-end (ps2_frontend.h): the menu as its Settings page before the game runs ("BACK" instead of
    // "RESUME"); Esc or BACK closes it as in game.
    void openSettings();
    void closeSettings();
    bool isOpen() const { return m_open; }

    // Passes the controller (SDL) to ImGui navigation. update() does this itself; a screen that draws ImGui without
    // update() (the front-end) calls it once per host frame after ps2InputUpdate(), and only on frames that draw ImGui.
    Ps2MenuPad feedControllerNav();

private:
    // Sections of the themed menu, in L1/R1 order
    enum class Page
    {
        Display,
        Sound,
        Controls,
        Overlay,
        Extras, // the game's own options (ps2RegisterGameOption); skipped when the game registered none
        Quit,
        Count
    };

    void open();
    void close();
    void applyWindowMode();
    void applyVolume();
    void drawOverlay();
    void drawMenu(PS2Runtime &runtime);
    void drawControlsTab(float scale);
    void drawBindingsTable();
    void updateCapture(bool escapePressed);
    float uiScale() const;

    // Themed look (default); PS2X_MENU_CLASSIC=1 keeps the plain ImGui window above
    void drawOverlayThemed();
    void drawEscHint();
    void drawMenuThemed(PS2Runtime &runtime);
    void drawDisplayPage();
    void drawSoundPage();
    void drawControlsPage();
    void drawOverlayPage();
    void drawExtrasPage();
    void drawQuitPage(PS2Runtime &runtime);
    void setPage(Page page);
    void queueControllerNav(const Ps2MenuPad &held);
    bool bindingCaptureBusy() const;
    float escHintAlpha() const;

    bool m_open = false;
    bool m_beforeGame = false; // opened by openSettings, until closed
    bool m_focusOnOpen = false;
    bool m_padBackWasDown = false; // Circle/B edge: backs out like Esc
    bool m_unblockOnRelease = false; // menu closed: game input returns once the closing button is released
    double m_closedAt = 0.0;         // ... or 1.5 s after the close at the latest
    bool m_padHeldAtOpen = false;    // controller buttons held when the menu opened count as released until let go
    bool m_padNav = true;            // PS2X_MENU_PAD=0 turns the controller feed off
    bool m_quitPrompt = false;
    ImFont *m_font = nullptr;

    bool m_classic = false;
    ImFont *m_fontUi = nullptr;   // Bahnschrift: headings and labels
    ImFont *m_fontMono = nullptr; // Consolas: numbers and key caps
    Page m_page = Page::Display;
    Page m_pageBeforeQuit = Page::Display;
    bool m_focusPage = false;
    const char *m_help = nullptr; // explanation of the row under the cursor, this frame
    double m_escHintShownAt = 0.0;
    bool m_escHintStarted = false;
    bool m_windowFocused = true;
    int m_testPage = -1; // PS2X_MENU_TEST: open the menu on this section a few seconds in (window captures)

    // Rebinding: the slot waiting for a key or controller input (-1 = none)
    int m_captureAction = -1;
    int m_captureSlot = 0;
    bool m_captureController = false;
    double m_captureEnded = 0.0;

    // Performance sampling (twice a second)
    double m_sampleTime = 0.0;
    uint64_t m_sampleFrames = 0;
    uint64_t m_sampleVblanks = 0;
    uint64_t m_sampleCycles = 0;
    float m_fps = 0.0f;
    float m_vps = 0.0f;
    float m_speed = 0.0f;

    // Game frame times (ms), sampled when the displayed frame changes
    std::array<float, 120> m_frameTimes{};
    size_t m_frameTimeNext = 0;
    uint64_t m_lastSeenFrames = 0;
    double m_lastFrameTime = 0.0;
};

#endif // PS2_MENU_H
