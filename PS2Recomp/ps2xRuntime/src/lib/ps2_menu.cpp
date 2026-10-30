#include "ps2_savestate.h"
#include "ps2_menu.h"
#include "ps2_ui_theme.h"
#include "ps2_host_settings.h"
#include "ps2_input.h"
#include "ps2_runtime.h"
#include "ps2x/iop/spu2_debug.h"

#include "imgui.h"
#include "imgui_internal.h" // SetKeyOwner
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
    constexpr double kPs2EeClockHz = 294912000.0;
    constexpr double kSampleSeconds = 0.5;
    // Below this game speed (percent of a real PS2) the themed readout shows it, in amber.
    constexpr float kSlowSpeedPercent = 97.0f;

    // "Player 1: ..." / "Player 2: ...": which controller drives each pad port (ps2_input.h).
    std::string playerLine(int port)
    {
        const std::string name = ps2InputPortControllerName(port);
        const char *none = port == 0                     ? "keyboard only"
                           : ps2InputPortConnected(port) ? "same as player 1 (PS2X_PAD_PORTS=1)"
                                                         : "no controller";
        return "Player " + std::to_string(port + 1) + ": " + (name.empty() ? std::string(none) : name);
    }
}

void PS2Menu::initialize()
{
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    // A readable system font for the menu; the debugger keeps ImGui's default one. The themed menu uses Bahnschrift
    // and Consolas: loaded once from %WINDIR%\Fonts and shared with the boot front-end (ps2_ui::fonts).
    const ps2_ui::Fonts &fonts = ps2_ui::fonts();
    m_font = fonts.plain;
    m_fontUi = fonts.ui;
    m_fontMono = fonts.mono;
    const char *classic = std::getenv("PS2X_MENU_CLASSIC");
    m_classic = classic && classic[0] == '1';
    const char *padNav = std::getenv("PS2X_MENU_PAD"); // 0: controllers do not drive the menu (as before 29 Oct)
    m_padNav = !(padNav && padNav[0] == '0');
    if (const char *test = std::getenv("PS2X_MENU_TEST"); test && test[0] >= '0' && test[0] < '0' + static_cast<int>(Page::Count))
        m_testPage = test[0] - '0';

    applyWindowMode();
    applyVolume();
}

float PS2Menu::uiScale() const
{
    return std::clamp(static_cast<float>(GetScreenHeight()) / 720.0f, 1.0f, 3.0f);
}

void PS2Menu::open()
{
    m_open = true;
    m_focusOnOpen = true;
    m_quitPrompt = false;
    m_unblockOnRelease = false;
    m_padHeldAtOpen = true;
    ps2GameInputBlocked().store(true, std::memory_order_relaxed);
}

void PS2Menu::openSettings()
{
    open();
    m_beforeGame = true;
    setPage(Page::Display);
}

void PS2Menu::closeSettings()
{
    if (m_open)
        close();
}

void PS2Menu::close()
{
    m_open = false;
    m_beforeGame = false;
    m_quitPrompt = false;
    m_captureAction = -1;
    // The game gets input back in update(), once the button that closed the menu (B, A, Enter, Space) is released,
    // so that press does not also reach the game.
    m_unblockOnRelease = true;
    m_closedAt = GetTime();
    ps2SaveSettings();
}

void PS2Menu::applyWindowMode()
{
    if (ps2Settings().fullscreen != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        ToggleBorderlessWindowed();
}

void PS2Menu::applyVolume()
{
    const PS2HostSettings &s = ps2Settings();
    SetMasterVolume(s.mute ? 0.0f : static_cast<float>(s.volume) / 100.0f);
}

void PS2Menu::updateCapture(bool escapePressed)
{
    PS2HostSettings &s = ps2Settings();
    int &slot = m_captureController ? s.pads[m_captureAction][m_captureSlot] : s.keys[m_captureAction][m_captureSlot];
    bool done = false;
    if (escapePressed)
        done = true; // cancel
    else if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE))
    {
        slot = 0; // clear
        done = true;
    }
    else if (m_captureController)
    {
        if (const int code = ps2InputCapturedControllerInput())
        {
            slot = code;
            done = true;
        }
    }
    else
    {
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
        {
            if (key == KEY_ESCAPE || key == KEY_BACKSPACE || key == KEY_DELETE)
                continue;
            slot = key;
            done = true;
            break;
        }
    }
    if (done)
    {
        m_captureAction = -1;
        m_captureEnded = GetTime();
    }
}

// Controllers reach ImGui through the runtime's own input (SDL): rlImGui only reads raylib's gamepad, which does not
// see them. ImGui applies queued events only in its next frame, so callers queue only when an ImGui frame follows;
// otherwise presses made while playing would pile up and replay as ghost input when the menu opens. Buttons already
// held when the menu opens count as released until let go once, so the press that opened it does not act in it.
void PS2Menu::queueControllerNav(const Ps2MenuPad &held)
{
    if (!m_padNav)
        return;
    Ps2MenuPad pad = held;
    if (m_padHeldAtOpen)
    {
        if (held.up || held.down || held.left || held.right || held.confirm || held.back || held.l1 || held.r1)
            pad = Ps2MenuPad{};
        else
            m_padHeldAtOpen = false;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, pad.up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, pad.down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, pad.left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pad.right);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, pad.confirm);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pad.back);
    io.AddKeyEvent(ImGuiKey_GamepadL1, pad.l1);
    io.AddKeyEvent(ImGuiKey_GamepadR1, pad.r1);
}

Ps2MenuPad PS2Menu::feedControllerNav()
{
    const Ps2MenuPad pad = ps2InputMenuPad();
    queueControllerNav(pad);
    return pad;
}

void PS2Menu::update(PS2Runtime &runtime)
{
    (void)runtime;

    // Circle/B backs out of the menu like Esc.
    const Ps2MenuPad pad = ps2InputMenuPad();
    const bool padBack = pad.back && !m_padBackWasDown;
    m_padBackWasDown = pad.back;
    // After a close, wait for the closing button to be released; give up after 1.5 s (a stuck or virtual
    // controller must not keep the game without input).
    const bool released = !pad.back && !pad.confirm && !IsKeyDown(KEY_ENTER) && !IsKeyDown(KEY_SPACE);
    if (m_unblockOnRelease && (released || GetTime() - m_closedAt > 1.5))
    {
        m_unblockOnRelease = false;
        if (!m_open)
            ps2GameInputBlocked().store(false, std::memory_order_relaxed);
    }

    const bool escape = IsKeyPressed(KEY_ESCAPE);
    if (m_open && m_captureAction >= 0)
        updateCapture(escape);
    else if (escape || ps2InputMenuPressed() || (m_open && padBack && !bindingCaptureBusy()))
    {
        if (!m_open)
            open();
        else if (m_quitPrompt)
            m_quitPrompt = false;
        else
            close();
    }

    const PS2PerfCounters &perf = ps2Perf();
    const double now = GetTime();

    // The "Esc menu" hint shows for a moment at start, when the mouse moves and when the window gets focus back.
    if (!m_escHintStarted)
    {
        m_escHintShownAt = now;
        m_escHintStarted = true;
    }
    const bool windowFocused = IsWindowFocused();
    if (!m_open && ((windowFocused && !m_windowFocused) || GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
        m_escHintShownAt = now;
    m_windowFocused = windowFocused;

    // Test runs only: the menu opens by itself; nothing is saved unless it is closed by hand.
    if (m_testPage >= 0 && !m_open && now > 8.0)
    {
        open();
        const Page page = static_cast<Page>(m_testPage);
        setPage(page == Page::Extras && ps2GameOptionCount() == 0 ? Page::Display : page); // no EXTRAS section to show
        m_testPage = -1;
    }

    // Only when the host draws an ImGui frame after this update (main.cpp skips it otherwise).
    if (needsImGuiFrame())
        queueControllerNav(pad);

    const uint64_t frames = perf.gameFrames.load(std::memory_order_relaxed);
    if (frames != m_lastSeenFrames)
    {
        if (m_lastFrameTime > 0.0)
        {
            const double perFrame = (now - m_lastFrameTime) * 1000.0 / static_cast<double>(frames - m_lastSeenFrames);
            m_frameTimes[m_frameTimeNext] = static_cast<float>(perFrame);
            m_frameTimeNext = (m_frameTimeNext + 1) % m_frameTimes.size();
        }
        m_lastSeenFrames = frames;
        m_lastFrameTime = now;
    }

    if (now - m_sampleTime >= kSampleSeconds)
    {
        const uint64_t vblanks = perf.vblanks.load(std::memory_order_relaxed);
        const uint64_t cycles = perf.eeCycles.load(std::memory_order_relaxed);
        if (m_sampleTime > 0.0)
        {
            const double elapsed = now - m_sampleTime;
            m_fps = static_cast<float>((frames - m_sampleFrames) / elapsed);
            m_vps = static_cast<float>((vblanks - m_sampleVblanks) / elapsed);
            m_speed = static_cast<float>((cycles - m_sampleCycles) / elapsed / kPs2EeClockHz * 100.0);
        }
        m_sampleTime = now;
        m_sampleFrames = frames;
        m_sampleVblanks = vblanks;
        m_sampleCycles = cycles;
    }
}

namespace
{
    constexpr double kStateMessageSeconds = 3.0;

    bool stateMessageVisible(std::string *text = nullptr)
    {
        double age = 0.0;
        std::string message = ps2_state::lastMessage(age);
        if (message.empty() || age > kStateMessageSeconds)
            return false;
        if (text)
            *text = std::move(message);
        return true;
    }
}

bool PS2Menu::needsImGuiFrame() const
{
    const PS2HostSettings &s = ps2Settings();
    // The themed readout shows game speed only when the game runs slow, so speed alone needs no frame at full speed.
    const bool speed = s.overlaySpeed && (m_classic || (m_speed > 0.0f && m_speed < kSlowSpeedPercent));
    return m_open || s.overlayFps || s.overlayVps || speed || s.overlayFrameTime || s.overlayAudio ||
           stateMessageVisible() || (!m_classic && escHintAlpha() > 0.0f);
}

void PS2Menu::draw(PS2Runtime &runtime)
{
    if (m_classic)
    {
        drawOverlay();
        if (m_open)
            drawMenu(runtime);
        return;
    }
    drawOverlayThemed();
    drawEscHint();
    if (m_open)
        drawMenuThemed(runtime);
}

void PS2Menu::drawOverlay()
{
    const PS2HostSettings &s = ps2Settings();
    // Save-state result (F5/F8), top centre for a few seconds.
    if (std::string message; stateMessageVisible(&message))
    {
        const float scale = uiScale();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, 10.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        constexpr ImGuiWindowFlags messageFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##savestate", nullptr, messageFlags))
        {
            ImGui::PushFont(m_font, 18.0f * scale);
            ImGui::TextUnformatted(message.c_str());
            ImGui::PopFont();
        }
        ImGui::End();
    }
    if (!(s.overlayFps || s.overlayVps || s.overlaySpeed || s.overlayFrameTime || s.overlayAudio))
        return;

    const float scale = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = 10.0f * scale;
    const bool right = (s.overlayCorner & 1) != 0;
    const bool bottom = (s.overlayCorner & 2) != 0;
    ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin, bottom ? display.y - margin : margin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##performance", nullptr, flags))
    {
        ImGui::PushFont(m_font, 16.0f * scale);
        if (s.overlayFps)
            ImGui::Text("FPS %.1f", m_fps);
        if (s.overlayVps)
            ImGui::Text("VPS %.1f", m_vps);
        if (s.overlaySpeed)
            ImGui::Text("Speed %.0f%%", m_speed);
        if (s.overlayFrameTime)
        {
            const float latest = m_frameTimes[(m_frameTimeNext + m_frameTimes.size() - 1) % m_frameTimes.size()];
            char label[32];
            std::snprintf(label, sizeof(label), "%.1f ms", latest);
            ImGui::PlotLines("##frametime", m_frameTimes.data(), static_cast<int>(m_frameTimes.size()),
                             static_cast<int>(m_frameTimeNext), label, 0.0f, 50.0f, ImVec2(160.0f * scale, 40.0f * scale));
        }
        if (s.overlayAudio)
        {
            uint32_t queued = 0;
            uint64_t dropouts = 0;
            ps2x::iop::spu2OutputStatus(queued, dropouts);
            ImGui::Text("Sound %u ms, %llu dropouts", queued / 48u, static_cast<unsigned long long>(dropouts));
        }
        ImGui::PopFont();
    }
    ImGui::End();
}

void PS2Menu::drawMenu(PS2Runtime &runtime)
{
    PS2HostSettings &s = ps2Settings();
    const float scale = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 170));

    ImGui::PushFont(m_font, 20.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 9.0f * scale));

    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(display.x * 0.92f, 760.0f * scale), std::min(display.y * 0.88f, 520.0f * scale)),
                             ImGuiCond_Always);
    if (m_focusOnOpen)
    {
        ImGui::SetNextWindowFocus();
        m_focusOnOpen = false;
    }
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Settings", nullptr, flags))
    {
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("##tabs", ImVec2(0.0f, -footer)))
        {
            if (ImGui::BeginTabBar("##settings-tabs"))
            {
                if (ImGui::BeginTabItem("Controls"))
                {
                    drawControlsTab(scale);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Performance"))
                {
                    ImGui::TextWrapped("Show these in a corner of the screen while you play.");
                    ImGui::Checkbox("FPS: frames the game draws per second", &s.overlayFps);
                    ImGui::Checkbox("VPS: screen refreshes (vblanks) per second", &s.overlayVps);
                    ImGui::Checkbox("Game speed (100% = a real PS2)", &s.overlaySpeed);
                    ImGui::Checkbox("Frame time graph", &s.overlayFrameTime);
                    ImGui::Checkbox("Sound buffer and dropouts", &s.overlayAudio);
                    ImGui::SetNextItemWidth(220.0f * scale);
                    ImGui::Combo("Corner", &s.overlayCorner, "Top left\0Top right\0Bottom left\0Bottom right\0");
                    ImGui::Separator();
                    ImGui::TextDisabled("Now: %.1f FPS, %.1f VPS, %.0f%% speed", m_fps, m_vps, m_speed);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics"))
                {
                    if (ImGui::Checkbox("Fullscreen", &s.fullscreen))
                        applyWindowMode();
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Picture shape");
                    if (ImGui::RadioButton("4:3, like the original TV picture", !s.stretch))
                        s.stretch = false;
                    if (ImGui::RadioButton("Stretch to fill the window", s.stretch))
                        s.stretch = true;
                    if (ps2GameWidescreenSupported())
                    {
                        // Live switch: the game's wrappers read ps2WidescreenOn() every frame.
                        bool wide = ps2WidescreenOn().load(std::memory_order_relaxed); // what is in effect (an environment switch included)
                        if (ImGui::Checkbox("Widescreen 16:9", &wide))
                            ps2SetWidescreen(wide);
                        ImGui::TextDisabled("Shows more of the scene; the HUD stays at the screen edges. Movies stay 4:3.");
                    }
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Picture lines");
                    if (ImGui::RadioButton("Progressive: every frame steady and sharp", s.progressive))
                        s.progressive = true;
                    if (ImGui::RadioButton("Interlaced, like a TV: frames blended", !s.progressive))
                        s.progressive = false;
                    ps2ProgressiveDisplay().store(s.progressive, std::memory_order_relaxed);
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Internal resolution");
                    {
                        // Each choice draws the PS2 picture that many times larger in each direction.
                        static constexpr int kScales[] = {1, 2, 3, 4};
                        static constexpr const char *kLabels[] = {"Native (PS2)", "x2", "x3", "x4"};
                        int current = 0;
                        for (int i = 0; i < 4; ++i)
                            if (s.upscale >= kScales[i])
                                current = i;
                        ImGui::SetNextItemWidth(260.0f * scale);
                        if (ImGui::Combo("##internal-resolution", &current, kLabels, 4))
                            s.upscale = kScales[current];
                        ImGui::TextDisabled("Sharper 3D and smoother edges; costs graphics-card time.");
                        ImGui::TextDisabled("Above your screen's size it only smooths edges further.");
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Sound"))
                {
                    ImGui::SetNextItemWidth(320.0f * scale);
                    if (ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%"))
                        applyVolume();
                    if (ImGui::Checkbox("Mute", &s.mute))
                        applyVolume();
                    ImGui::EndTabItem();
                }
                if (ps2GameOptionCount() > 0 && ImGui::BeginTabItem("Extras"))
                {
                    for (int i = 0; i < ps2GameOptionCount(); ++i)
                    {
                        ImGui::PushID(i);
                        bool on = ps2GameOptionOn(i);
                        if (ImGui::Checkbox(ps2GameOptionLabel(i), &on))
                            ps2SetGameOption(i, on);
                        ImGui::TextDisabled("%s", ps2GameOptionHelp(i));
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Resume"))
            close();
        ImGui::SameLine();
        if (ImGui::Button("Quit game"))
        {
            m_quitPrompt = true;
            ImGui::OpenPopup("Quit game?");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Esc or the controller's menu button closes this");

        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Quit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextUnformatted("Anything not saved in the game will be lost.");
            if (ImGui::Button("Quit"))
            {
                ps2SaveSettings();
                runtime.requestStop();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || !m_quitPrompt)
            {
                m_quitPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopFont();
}

void PS2Menu::drawControlsTab(float scale)
{
    PS2HostSettings &s = ps2Settings();
    const std::string controllers = ps2InputControllerNames();
    if (controllers.empty())
        ImGui::TextUnformatted("No controller found. Plug one in at any time.");
    else
        ImGui::Text("Controller: %s", controllers.c_str());
    ImGui::TextUnformatted(playerLine(0).c_str());
    ImGui::TextUnformatted(playerLine(1).c_str());
    const bool swapPossible = ps2InputPadSwapPossible();
    ImGui::BeginDisabled(!swapPossible);
    if (ImGui::Button(swapPossible ? "Swap players" : "Swap players (needs two controllers)"))
        ps2InputSwapPlayers();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Click a slot, then press the key or button to use. Backspace clears it, Esc cancels.");
    drawBindingsTable();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(320.0f * scale);
    ImGui::SliderInt("Stick deadzone", &s.deadzone, 0, 50, "%d%%");
    ImGui::SetNextItemWidth(320.0f * scale);
    ImGui::SliderInt("Rumble strength", &s.rumble, 0, 100, s.rumble == 0 ? "Off" : "%d%%");
    ImGui::SameLine();
    if (ImGui::Button("Test rumble"))
        ps2InputTestRumble();

    // Pressure-sensitive buttons: the DualShock 2 reports how hard each button is pressed. Keys and most modern
    // controllers only know on/off, so the light-press modifier stands in for a soft press.
    ImGui::Spacing();
    ImGui::SeparatorText("Pressure-sensitive buttons");
    if (!ps2PadPressureEnabled())
        ImGui::TextDisabled("Off for this session (PS2X_PAD_PRESSURE=0): every press reads as a full press.");
    ImGui::TextDisabled("Hold the \"Light press (modifier)\" binding above to press buttons softly.");
    ImGui::TextUnformatted("Modifier:");
    ImGui::SameLine();
    int toggle = s.lightPressToggle ? 1 : 0;
    ImGui::RadioButton("Hold", &toggle, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Toggle on/off", &toggle, 1);
    s.lightPressToggle = toggle != 0;
    ImGui::SameLine();
    if (ps2InputLightPressOn())
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  (on now)");
    ImGui::SetNextItemWidth(320.0f * scale);
    ImGui::SliderInt("Light press strength", &s.lightPressLevel, 5, 95, "%d%%");
    ImGui::TextUnformatted("Buttons the modifier softens:");
    constexpr int kSoftenable[] = {kPadTriangle, kPadCross, kPadSquare, kPadCircle, kPadL1, kPadR1, kPadL2, kPadR2,
                                   kPadUp, kPadDown, kPadLeft, kPadRight};
    if (ImGui::BeginTable("##soften", 4, ImGuiTableFlags_SizingStretchSame))
    {
        for (const int action : kSoftenable)
        {
            ImGui::TableNextColumn();
            bool on = (s.lightPressButtons & (1 << action)) != 0;
            if (ImGui::Checkbox(ps2PadActionName(action), &on))
                s.lightPressButtons = on ? (s.lightPressButtons | (1 << action)) : (s.lightPressButtons & ~(1 << action));
        }
        ImGui::EndTable();
    }
    ImGui::Checkbox("Analog triggers (buttons bound to a controller trigger report how far it is pulled)", &s.analogTriggers);

    if (ImGui::Button("Reset controls to defaults"))
    {
        ps2DefaultControls(s);
        m_captureAction = -1;
    }
}

// The key/controller binding table, shared by the classic and themed menus.
void PS2Menu::drawBindingsTable()
{
    PS2HostSettings &s = ps2Settings();

    // A slot button: shows the binding, or a prompt while waiting for input.
    const auto slotButton = [&](int action, int slot, bool controller, const std::string &name)
    {
        const bool capturing = m_captureAction == action && m_captureSlot == slot && m_captureController == controller;
        const std::string label = (capturing ? std::string(controller ? "Press a button..." : "Press a key...") : name) + "##" +
                                  std::to_string(action) + (controller ? "p" : "k") + std::to_string(slot);
        // Ignore the activation that the bound key or button itself can cause right after capture.
        if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)) && GetTime() - m_captureEnded > 0.3)
        {
            m_captureAction = action;
            m_captureSlot = slot;
            m_captureController = controller;
        }
    };

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame;
    if (ImGui::BeginTable("##bindings", 5, tableFlags))
    {
        ImGui::TableSetupColumn("PS2 button", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Key 2");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableSetupColumn("Controller 2");
        ImGui::TableHeadersRow();
        for (int action = 0; action < kPadActionCount; ++action)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ps2PadActionName(action));
            for (int slot = 0; slot < 2; ++slot)
            {
                ImGui::TableNextColumn();
                if (action == kPadOpenMenu) // the keyboard menu key is always Esc
                    ImGui::TextDisabled(slot == 0 ? "Esc" : "");
                else
                    slotButton(action, slot, false, ps2KeyName(s.keys[action][slot]));
            }
            for (int slot = 0; slot < 2; ++slot)
            {
                ImGui::TableNextColumn();
                slotButton(action, slot, true, ps2ControllerInputName(s.pads[action][slot]));
            }
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Themed menu and overlay: ZOE's HUD language (mint lines on near-black, cut corners, diamond markers, condensed
// capitals). Built from ImGui primitives so keyboard, mouse and controller navigation keep working.

namespace
{
    namespace theme = ps2_ui; // shared with the boot front-end (ps2_ui_theme.h)
}

float PS2Menu::escHintAlpha() const
{
    const double age = GetTime() - m_escHintShownAt;
    if (!m_escHintStarted || age < 0.0)
        return 0.0f;
    if (age < 1.0)
        return 1.0f;
    return std::max(0.0f, 1.0f - static_cast<float>((age - 1.0) / 0.4));
}

void PS2Menu::drawEscHint()
{
    const float alpha = escHintAlpha();
    if (m_open || alpha <= 0.0f)
        return;
    using namespace theme;
    const PS2HostSettings &s = ps2Settings();
    const float scale = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList *dl = ImGui::GetForegroundDrawList();

    // Top right, unless the performance readout already sits there.
    const bool overlayTopRight = s.overlayCorner == 1 && (s.overlayFps || s.overlayVps || s.overlayFrameTime || s.overlayAudio);
    const float pad = 10.0f * scale;
    const float h = 34.0f * scale;
    const float keyW = textSize(m_fontMono, 13.0f * scale, "ESC").x + 12.0f * scale;
    const float w = keyW + 8.0f * scale + textSize(m_fontUi, 17.0f * scale, "MENU").x + pad * 2.0f;
    const float x = overlayTopRight ? 14.0f * scale : display.x - 14.0f * scale - w;
    const ImVec2 a(x, 12.0f * scale);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), fade(IM_COL32(3, 16, 14, 140), alpha));
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), fade(kLineDim, alpha));
    prompt(dl, m_fontMono, m_fontUi, scale, ImVec2(a.x + pad, a.y + (h - 20.0f * scale) * 0.5f), "ESC", "MENU", alpha);
}

void PS2Menu::drawOverlayThemed()
{
    using namespace theme;
    const PS2HostSettings &s = ps2Settings();
    const float scale = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList *dl = ImGui::GetForegroundDrawList();

    // Save-state result (F5/F8), top centre for a few seconds.
    if (std::string message; stateMessageVisible(&message))
    {
        const float size = 20.0f * scale;
        const ImVec2 ts = textSize(m_fontUi, size, message.c_str());
        const float h = 36.0f * scale;
        const float w = ts.x + 52.0f * scale;
        const ImVec2 a((display.x - w) * 0.5f, 14.0f * scale);
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(2, 12, 10, 200));
        dl->AddLine(a, ImVec2(a.x + w, a.y), kLine);
        dl->AddLine(ImVec2(a.x, a.y + h), ImVec2(a.x + w, a.y + h), kLine);
        diamond(dl, ImVec2(a.x + 18.0f * scale, a.y + h * 0.5f), 4.0f * scale, kLine, kLine);
        text(dl, m_fontUi, size, ImVec2(a.x + 34.0f * scale, a.y + (h - ts.y) * 0.5f), kText, message.c_str());
    }

    // Performance readout: one quiet line of numbers. Game speed appears only when the game runs slower than a PS2.
    const bool slow = s.overlaySpeed && m_speed > 0.0f && m_speed < kSlowSpeedPercent;
    const bool spark = s.overlayFrameTime;
    if (!(s.overlayFps || s.overlayVps || spark || s.overlayAudio || slow))
        return;

    struct Piece
    {
        char value[32];
        const char *unit;
        ImU32 color;
    };
    Piece pieces[4];
    int count = 0;
    const auto add = [&](const char *unit, ImU32 color, const char *fmt, double v)
    {
        std::snprintf(pieces[count].value, sizeof(pieces[count].value), fmt, v);
        pieces[count].unit = unit;
        pieces[count].color = color;
        ++count;
    };
    constexpr ImU32 kNumber = IM_COL32(205, 239, 226, 210);
    constexpr ImU32 kUnit = IM_COL32(67, 227, 168, 200);
    if (s.overlayFps)
        add(" FPS", kNumber, "%.1f", m_fps);
    if (s.overlayVps)
        add(" VPS", kNumber, "%.1f", m_vps);
    if (spark)
        add(" MS", kNumber, "%.1f", m_frameTimes[(m_frameTimeNext + m_frameTimes.size() - 1) % m_frameTimes.size()]);
    if (slow)
        add("", fade(kAmber, 0.9f), "GAME SPEED %.0f%%", m_speed);

    char audio[64] = {};
    if (s.overlayAudio)
    {
        uint32_t queued = 0;
        uint64_t dropouts = 0;
        ps2x::iop::spu2OutputStatus(queued, dropouts);
        std::snprintf(audio, sizeof(audio), "SND %u MS \xC2\xB7 %llu DROP%s", queued / 48u, static_cast<unsigned long long>(dropouts),
                      dropouts == 1 ? "" : "S");
    }

    const float size = 13.0f * scale;
    const float gap = 10.0f * scale;
    const float lineH = textSize(m_fontMono, size, "0").y;
    float lineW = 0.0f;
    for (int i = 0; i < count; ++i)
        lineW += textSize(m_fontMono, size, pieces[i].value).x + textSize(m_fontMono, size, pieces[i].unit).x + (i ? gap : 0.0f);
    const float sparkW = 112.0f * scale;
    const float sparkH = 10.0f * scale;
    const float audioW = audio[0] ? textSize(m_fontMono, size, audio).x : 0.0f;
    const float padX = 8.0f * scale;
    const float padY = 4.0f * scale;
    const float w = std::max({lineW, spark ? sparkW : 0.0f, audioW}) + padX * 2.0f;
    const float h = padY * 2.0f + (count ? lineH : 0.0f) + (spark ? sparkH + 3.0f * scale : 0.0f) +
                    (audio[0] ? lineH + 2.0f * scale : 0.0f);

    const bool right = (s.overlayCorner & 1) != 0;
    const bool bottom = (s.overlayCorner & 2) != 0;
    const float mx = 14.0f * scale;
    const float my = 10.0f * scale;
    const ImVec2 a(right ? display.x - mx - w : mx, bottom ? display.y - my - h : my);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(2, 12, 10, 82));
    const float edgeX = right ? a.x + w - 1.0f : a.x;
    dl->AddLine(ImVec2(edgeX, a.y), ImVec2(edgeX, a.y + h), IM_COL32(67, 227, 168, 115));

    float x = a.x + padX;
    float y = a.y + padY;
    for (int i = 0; i < count; ++i)
    {
        text(dl, m_fontMono, size, ImVec2(x, y), pieces[i].color, pieces[i].value);
        x += textSize(m_fontMono, size, pieces[i].value).x;
        text(dl, m_fontMono, size, ImVec2(x, y), kUnit, pieces[i].unit);
        x += textSize(m_fontMono, size, pieces[i].unit).x + gap;
    }
    if (count)
        y += lineH;
    if (spark)
    {
        y += 3.0f * scale;
        // Frame times from oldest to newest; the middle of the strip is one 60 Hz frame, the top two.
        ImVec2 pts[std::tuple_size_v<decltype(m_frameTimes)>];
        const size_t n = m_frameTimes.size();
        for (size_t i = 0; i < n; ++i)
        {
            const float ms = m_frameTimes[(m_frameTimeNext + i) % n];
            const float t = std::clamp(ms / 33.4f, 0.0f, 1.0f);
            pts[i] = ImVec2(a.x + padX + sparkW * static_cast<float>(i) / static_cast<float>(n - 1), y + sparkH * (1.0f - t));
        }
        dl->AddPolyline(pts, static_cast<int>(n), IM_COL32(67, 227, 168, 180), ImDrawFlags_None, 1.0f);
        y += sparkH;
    }
    if (audio[0])
        text(dl, m_fontMono, size, ImVec2(a.x + padX, y + 2.0f * scale), IM_COL32(143, 201, 180, 200), audio);
}

void PS2Menu::setPage(Page page)
{
    if (page == Page::Quit)
    {
        if (m_page != Page::Quit)
            m_pageBeforeQuit = m_page;
        m_quitPrompt = true;
    }
    else
        m_quitPrompt = false;
    m_page = page;
    m_focusPage = true;
    m_captureAction = -1; // a binding slot waiting for input would be hidden on the new page
}

bool PS2Menu::bindingCaptureBusy() const
{
    // Also the moment just after a capture: the key that finished it must not step a row or switch the section.
    return m_captureAction >= 0 || GetTime() - m_captureEnded < 0.3;
}

void PS2Menu::drawMenuThemed(PS2Runtime &runtime)
{
    using namespace theme;
    const float scale = uiScale();
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    if (m_page == Page::Quit && !m_quitPrompt) // Esc answered the quit question
    {
        m_page = m_pageBeforeQuit;
        m_focusPage = true;
    }

    // Q/E or L1/R1 step through the sections.
    if (!bindingCaptureBusy())
    {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
            dir = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
            dir = 1;
        if (dir)
        {
            constexpr int count = static_cast<int>(Page::Count);
            int next = (static_cast<int>(m_page) + dir + count) % count;
            if (next == static_cast<int>(Page::Extras) && ps2GameOptionCount() == 0)
                next = (next + dir + count) % count;
            setPage(static_cast<Page>(next));
        }
    }

    const float w = std::min(display.x - 48.0f * scale, 1160.0f * scale);
    const float h = std::min(display.y - 40.0f * scale, 620.0f * scale);
    const ImVec2 o((display.x - w) * 0.5f, (display.y - h) * 0.5f);
    const float headerH = 52.0f * scale;
    const float footerH = 30.0f * scale;
    const float navW = 250.0f * scale;
    const float gap = 22.0f * scale;
    const float bodyTop = o.y + headerH + 20.0f * scale;
    const float bodyBottom = o.y + h - footerH - 12.0f * scale;

    // Display settings change the picture live, so that page dims the game less.
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(1, 8, 7, m_page == Page::Display ? 97 : 168));

    // Header
    {
        const float titleSize = 46.0f * scale;
        const ImVec2 ts = textSize(m_fontUi, titleSize, "MENU");
        text(bg, m_fontUi, titleSize, ImVec2(o.x, o.y + headerH - ts.y), kText, "MENU");
        char info[96];
        if (m_beforeGame) // boot front-end: no game frames yet
            std::snprintf(info, sizeof(info), "BEFORE START  \xC2\xB7  INTERNAL x%d", ps2Settings().upscale);
        else
            std::snprintf(info, sizeof(info), "%.0f FPS  \xC2\xB7  INTERNAL x%d", m_fps, ps2Settings().upscale);
        const float infoSize = 13.0f * scale;
        const ImVec2 is = textSize(m_fontMono, infoSize, info);
        text(bg, m_fontMono, infoSize, ImVec2(o.x + w - is.x, o.y + headerH - is.y - 6.0f * scale), kTextDim, info);
        bg->AddLine(ImVec2(o.x, o.y + headerH + 4.0f * scale), ImVec2(o.x + w, o.y + headerH + 4.0f * scale), kLine);
    }

    // Panels
    const float cut = 16.0f * scale;
    panel(bg, ImVec2(o.x, bodyTop), ImVec2(o.x + navW, bodyBottom), cut, kPanel, kLineDim);
    const float pageX = o.x + navW + gap;
    panel(bg, ImVec2(pageX, bodyTop), ImVec2(o.x + w, bodyBottom), cut, kPanel, kLineDim);

    // Footer prompts
    {
        float x = o.x + w;
        const float y = o.y + h - footerH + 6.0f * scale;
        const char *prompts[][2] = {{"ESC", m_beforeGame ? "BACK" : "RESUME"}, {"ARROWS", "CHANGE"}, {"Q / E", "SECTION"}};
        for (const auto &p : prompts)
        {
            const float pw = textSize(m_fontMono, 13.0f * scale, p[0]).x + 20.0f * scale + textSize(m_fontUi, 17.0f * scale, p[1]).x;
            x -= pw;
            prompt(bg, m_fontMono, m_fontUi, scale, ImVec2(x, y), p[0], p[1]);
            x -= 26.0f * scale;
        }
    }

    const int colors = pushStyle(scale);
    constexpr ImGuiWindowFlags baseFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;

    // Section list (mouse only; the keyboard and controller use Q/E, L1/R1)
    ImGui::SetNextWindowPos(ImVec2(o.x, bodyTop + 14.0f * scale));
    ImGui::SetNextWindowSize(ImVec2(navW, bodyBottom - bodyTop - 28.0f * scale));
    if (ImGui::Begin("##zr-nav", nullptr, baseFlags | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const auto entry = [&](const char *label, bool active, ImU32 accent) -> bool
        {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 size(navW - 1.0f, 44.0f * scale);
            const bool pressed = ImGui::InvisibleButton(label, size);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 b(a.x + size.x, a.y + size.y);
            if (active || hovered)
                dl->AddRectFilled(a, b, active ? fade(accent, 0.14f) : fade(accent, 0.07f));
            if (active)
                dl->AddRectFilled(a, ImVec2(a.x + 2.0f * scale, b.y), accent);
            const float cy = a.y + size.y * 0.5f;
            diamond(dl, ImVec2(a.x + 24.0f * scale, cy), 4.5f * scale, active ? accent : 0, accent);
            const float labelSize = 27.0f * scale;
            const ImVec2 ls = textSize(m_fontUi, labelSize, label);
            text(dl, m_fontUi, labelSize, ImVec2(a.x + 40.0f * scale, cy - ls.y * 0.5f),
                 (active || hovered) ? kText : fade(kTextDim, 0.95f), label);
            return pressed;
        };
        const auto separator = [&]()
        {
            const ImVec2 a = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(a.x + 18.0f * scale, a.y + 6.0f * scale), ImVec2(a.x + navW - 18.0f * scale, a.y + 6.0f * scale), kLineDim);
            ImGui::Dummy(ImVec2(navW, 12.0f * scale));
        };

        if (entry(m_beforeGame ? "BACK" : "RESUME", false, kLine))
            close();
        separator();
        const struct
        {
            const char *label;
            Page page;
        } sections[] = {{"DISPLAY", Page::Display}, {"SOUND", Page::Sound}, {"CONTROLS", Page::Controls}, {"OVERLAY", Page::Overlay}};
        for (const auto &sec : sections)
            if (entry(sec.label, m_page == sec.page, kLine))
                setPage(sec.page);
        if (ps2GameOptionCount() > 0 && entry("EXTRAS", m_page == Page::Extras, kLine))
            setPage(Page::Extras);
        separator();
        if (entry("QUIT", m_page == Page::Quit, kRed))
            setPage(Page::Quit);
    }
    ImGui::End();

    // Page
    const float pagePadX = 26.0f * scale;
    const float pagePadY = 16.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(pageX + pagePadX, bodyTop + pagePadY));
    ImGui::SetNextWindowSize(ImVec2(o.x + w - pageX - pagePadX * 2.0f, bodyBottom - bodyTop - pagePadY * 2.0f));
    if (m_focusOnOpen || m_focusPage)
    {
        ImGui::SetNextWindowFocus();
        m_focusOnOpen = false;
        m_focusPage = false;
    }
    if (ImGui::Begin("##zr-page", nullptr, baseFlags))
    {
        m_help = nullptr;
        if (m_page == Page::Quit)
            drawQuitPage(runtime);
        else
        {
            static constexpr const char *kTitles[] = {"DISPLAY", "SOUND", "CONTROLS", "OVERLAY", "EXTRAS"};
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float pw = ImGui::GetContentRegionAvail().x;
            const float titleSize = 32.0f * scale;
            const ImVec2 ts = textSize(m_fontUi, titleSize, kTitles[static_cast<int>(m_page)]);
            text(dl, m_fontUi, titleSize, a, kText, kTitles[static_cast<int>(m_page)]);
            const char *saved = "SAVED AUTOMATICALLY";
            const ImVec2 ss = textSize(m_fontMono, 12.0f * scale, saved);
            text(dl, m_fontMono, 12.0f * scale, ImVec2(a.x + pw - ss.x, a.y + ts.y - ss.y - 4.0f * scale), kTextDim, saved);
            dl->AddLine(ImVec2(a.x, a.y + ts.y + 6.0f * scale), ImVec2(a.x + pw, a.y + ts.y + 6.0f * scale), kLineDim);
            ImGui::Dummy(ImVec2(pw, ts.y + 14.0f * scale));

            const float helpH = 52.0f * scale;
            if (ImGui::BeginChild("##zr-rows", ImVec2(0.0f, -helpH), ImGuiChildFlags_NavFlattened))
            {
                switch (m_page)
                {
                case Page::Display:
                    drawDisplayPage();
                    break;
                case Page::Sound:
                    drawSoundPage();
                    break;
                case Page::Controls:
                    drawControlsPage();
                    break;
                case Page::Extras:
                    drawExtrasPage();
                    break;
                default:
                    drawOverlayPage();
                    break;
                }
            }
            ImGui::EndChild();

            // Explanation of the row under the cursor
            const ImVec2 f = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(f.x, f.y + 6.0f * scale), ImVec2(f.x + pw, f.y + 6.0f * scale), kLineDim);
            text(dl, m_fontMono, 12.0f * scale, ImVec2(f.x, f.y + 18.0f * scale), kLine, "INFO");
            const char *help = m_help ? m_help : "Pick a setting to see what it does.";
            const float helpSize = 17.0f * scale;
            const float helpX = f.x + 52.0f * scale;
            dl->AddText(m_fontUi ? m_fontUi : ImGui::GetFont(), helpSize, ImVec2(helpX, f.y + 15.0f * scale), IM_COL32(205, 239, 226, 255),
                        help, nullptr, f.x + pw - helpX);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(kStyleVars);
    ImGui::PopStyleColor(colors);
}

void PS2Menu::drawDisplayPage()
{
    using namespace theme;
    PS2HostSettings &s = ps2Settings();
    const Rows r{uiScale(), m_fontUi, m_fontMono, &m_help, bindingCaptureBusy()};

    if (toggleRow(r, "##window", "Window mode", s.fullscreen, "Borderless fullscreen covers the screen and keeps alt-tab instant.",
                  "WINDOWED", "FULLSCREEN"))
        applyWindowMode();
    toggleRow(r, "##shape", "Picture shape", s.stretch,
              "Original keeps the picture's own shape. Stretch fills the window. For a real 16:9 view, use Widescreen.", "ORIGINAL",
              "STRETCH");
    if (ps2GameWidescreenSupported())
    {
        // Live switch: the game's wrappers read ps2WidescreenOn() every frame.
        bool wide = ps2WidescreenOn().load(std::memory_order_relaxed); // what is in effect (an environment switch included)
        if (toggleRow(r, "##wide", "Widescreen", wide, "Shows more of the scene; the HUD stays at the screen edges. Movies stay 4:3.",
                      "OFF", "16:9"))
            ps2SetWidescreen(wide);
    }
    toggleRow(r, "##lines", "Picture lines", s.progressive,
              "Progressive shows every frame steady and sharp. Interlaced blends frames, like a TV.", "INTERLACED (TV)", "PROGRESSIVE");
    ps2ProgressiveDisplay().store(s.progressive, std::memory_order_relaxed);

    // Each choice draws the PS2 picture that many times larger in each direction.
    static constexpr int kScales[] = {1, 2, 3, 4};
    static constexpr const char *kLabels[] = {"NATIVE (PS2)", "x2", "x3", "x4"};
    int current = 0;
    for (int i = 0; i < 4; ++i)
        if (s.upscale >= kScales[i])
            current = i;
    if (choiceRow(r, "##upscale", "Internal resolution", current, kLabels, 4,
                  "Sharper 3D and smoother edges; costs graphics-card time. Above your screen's size it only smooths edges."))
        s.upscale = kScales[current];
}

void PS2Menu::drawSoundPage()
{
    using namespace theme;
    PS2HostSettings &s = ps2Settings();
    const Rows r{uiScale(), m_fontUi, m_fontMono, &m_help, bindingCaptureBusy()};
    if (amountRow(r, "##volume", "Volume", s.volume, 0, 100, 5, "Overall game volume."))
        applyVolume();
    if (toggleRow(r, "##mute", "Mute", s.mute, "Silences the game without losing the volume setting."))
        applyVolume();
}

void PS2Menu::drawOverlayPage()
{
    using namespace theme;
    PS2HostSettings &s = ps2Settings();
    const Rows r{uiScale(), m_fontUi, m_fontMono, &m_help, bindingCaptureBusy()};
    toggleRow(r, "##fps", "Frames per second", s.overlayFps, "Frames the game draws each second.");
    toggleRow(r, "##vps", "Screen refreshes", s.overlayVps, "Vblanks per second: how often the PS2 would refresh the TV.");
    toggleRow(r, "##ftime", "Frame time", s.overlayFrameTime, "How long the last frame took, with a small line of recent frames.");
    toggleRow(r, "##speed", "Game speed warning", s.overlaySpeed,
              "Shows the game speed in amber when the game runs slower than a real PS2. Hidden at full speed.");
    toggleRow(r, "##audio", "Sound buffer", s.overlayAudio, "Sound buffer length and dropouts.");
    static constexpr const char *kCorners[] = {"TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT"};
    choiceRow(r, "##corner", "Corner", s.overlayCorner, kCorners, 4, "Where the readout sits while you play.");
}

void PS2Menu::drawExtrasPage()
{
    using namespace theme;
    const Rows r{uiScale(), m_fontUi, m_fontMono, &m_help, bindingCaptureBusy()};
    for (int i = 0; i < ps2GameOptionCount(); ++i)
    {
        char id[24];
        std::snprintf(id, sizeof(id), "##game%d", i);
        bool on = ps2GameOptionOn(i);
        if (toggleRow(r, id, ps2GameOptionLabel(i), on, ps2GameOptionHelp(i)))
            ps2SetGameOption(i, on);
    }
}

void PS2Menu::drawControlsPage()
{
    using namespace theme;
    PS2HostSettings &s = ps2Settings();
    const float scale = uiScale();
    const Rows r{scale, m_fontUi, m_fontMono, &m_help, bindingCaptureBusy()};

    const std::string controllers = ps2InputControllerNames();
    heading(r, "CONTROLLER");
    ImGui::PushFont(m_fontUi, 18.0f * scale);
    ImGui::TextUnformatted(controllers.empty() ? "No controller found. Plug one in at any time." : controllers.c_str());
    ImGui::TextDisabled("%s", playerLine(0).c_str());
    ImGui::TextDisabled("%s", playerLine(1).c_str());
    ImGui::PopFont();
    const bool swapPossible = ps2InputPadSwapPossible();
    ImGui::BeginDisabled(!swapPossible);
    if (actionRow(r, "##padswap", swapPossible ? "Swap players" : "Swap players (needs two controllers)", "SWAP",
                  "Exchanges the controllers of player 1 and player 2 (for this session). The first controller plugged in is "
                  "player 1 (with the keyboard), the second is player 2."))
        ps2InputSwapPlayers();
    ImGui::EndDisabled();
    amountRow(r, "##deadzone", "Stick deadzone", s.deadzone, 0, 50, 1, "Stick travel ignored around the centre.");
    amountRow(r, "##rumble", "Rumble strength", s.rumble, 0, 100, 5, "Vibration strength on controllers that have it.", "OFF");
    if (actionRow(r, "##testrumble", "Test rumble", "RUMBLE", "Shakes the controller once at the chosen strength."))
        ps2InputTestRumble();

    // Pressure-sensitive buttons: the DualShock 2 reports how hard each button is pressed. Keys and most modern
    // controllers only know on/off, so the light-press modifier stands in for a soft press.
    heading(r, "LIGHT PRESS");
    if (!ps2PadPressureEnabled())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("Off for this session (PS2X_PAD_PRESSURE=0): every press reads as a full press.");
        ImGui::PopStyleColor();
    }
    static constexpr const char *kModes[] = {"HOLD", "TOGGLE"};
    int mode = s.lightPressToggle ? 1 : 0;
    if (choiceRow(r, "##lpmode", ps2InputLightPressOn() ? "Light press modifier (on now)" : "Light press modifier", mode, kModes, 2,
                  "Hold the \"Light press (modifier)\" binding to press buttons softly, or switch it on and off."))
        s.lightPressToggle = mode != 0;
    amountRow(r, "##lplevel", "Light press strength", s.lightPressLevel, 5, 95, 5, "How hard a light press is. 40% is a gentle press.");
    toggleRow(r, "##triggers", "Analog triggers", s.analogTriggers,
              "Buttons bound to a controller trigger report how far it is pulled.");

    ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
    ImGui::PushFont(m_fontUi, 17.0f * scale);
    ImGui::TextDisabled("Buttons the modifier softens:");
    constexpr int kSoftenable[] = {kPadTriangle, kPadCross, kPadSquare, kPadCircle, kPadL1, kPadR1, kPadL2, kPadR2,
                                   kPadUp, kPadDown, kPadLeft, kPadRight};
    if (ImGui::BeginTable("##soften", 4, ImGuiTableFlags_SizingStretchSame))
    {
        for (const int action : kSoftenable)
        {
            ImGui::TableNextColumn();
            bool on = (s.lightPressButtons & (1 << action)) != 0;
            if (ImGui::Checkbox(ps2PadActionName(action), &on))
                s.lightPressButtons = on ? (s.lightPressButtons | (1 << action)) : (s.lightPressButtons & ~(1 << action));
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();

    heading(r, "BUTTON MAPPING");
    ImGui::PushFont(m_fontUi, 17.0f * scale);
    ImGui::TextDisabled("Pick a slot, then press the key or button to use. Backspace clears it, Esc cancels.");
    drawBindingsTable();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));
    if (actionRow(r, "##resetcontrols", "Reset controls to defaults", "RESET", "Puts every binding and control setting back to default."))
    {
        ps2DefaultControls(s);
        m_captureAction = -1;
    }
}

void PS2Menu::drawQuitPage(PS2Runtime &runtime)
{
    using namespace theme;
    const float scale = uiScale();
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, avail * 0.5f - 120.0f * scale)));

    ImGui::PushFont(m_fontMono, 13.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextUnformatted("CONFIRM");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(m_fontUi, 52.0f * scale);
    ImGui::TextUnformatted("LEAVE THE GAME?");
    ImGui::PopFont();
    ImGui::PushFont(m_fontUi, 19.0f * scale);
    ImGui::TextDisabled("Anything not saved in the game will be lost.");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 14.0f * scale));

    if (command(m_fontUi, scale, "CANCEL", kLine))
        setPage(m_pageBeforeQuit);
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0.0f, 14.0f * scale);
    if (command(m_fontUi, scale, "QUIT TO DESKTOP", kRed))
    {
        ps2SaveSettings();
        runtime.requestStop();
    }
}
