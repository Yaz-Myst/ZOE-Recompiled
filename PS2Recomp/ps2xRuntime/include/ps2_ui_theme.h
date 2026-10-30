#ifndef PS2_UI_THEME_H
#define PS2_UI_THEME_H

#include "imgui.h"

// Look shared by the in-game menu (Esc) and the boot front-end: mint lines on near-black, panels with two cut
// corners, diamond markers, condensed capitals. Window thread only, inside an ImGui frame.
namespace ps2_ui
{
    constexpr ImU32 kInk = IM_COL32(2, 10, 9, 255);
    constexpr ImU32 kPanel = IM_COL32(3, 16, 14, 235);
    constexpr ImU32 kLine = IM_COL32(67, 227, 168, 255);
    constexpr ImU32 kLineDim = IM_COL32(30, 107, 85, 255);
    constexpr ImU32 kText = IM_COL32(214, 255, 240, 255);
    constexpr ImU32 kTextDim = IM_COL32(143, 201, 180, 255);
    constexpr ImU32 kHighlight = IM_COL32(67, 227, 168, 34);
    constexpr ImU32 kAmber = IM_COL32(255, 181, 71, 255);
    constexpr ImU32 kRed = IM_COL32(255, 107, 117, 255);

    // Segoe UI (plain), Bahnschrift (headings and labels; a condensed DIN close to ZOE's HUD lettering) and Consolas
    // (numbers, key caps) from %WINDIR%\Fonts; all three ship with Windows 10 and 11, so nothing is bundled. Loaded
    // on the first call (after rlImGuiSetup); a missing font falls back to the plain one, then to ImGui's default.
    struct Fonts
    {
        ImFont *plain = nullptr;
        ImFont *ui = nullptr;
        ImFont *mono = nullptr;
    };
    const Fonts &fonts();

    ImU32 fade(ImU32 color, float alpha);

    // Panel with the top-left and bottom-right corners cut, like ZOE's HUD frames.
    void panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border);
    void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 border);
    ImVec2 textSize(ImFont *font, float size, const char *text);
    void text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 color, const char *str);

    // A key cap ("ESC") followed by what it does; returns the width used.
    float prompt(ImDrawList *dl, ImFont *mono, ImFont *ui, float scale, ImVec2 pos, const char *key, const char *what,
                 float alpha = 1.0f);

    // Pushes the theme's colours and the kStyleVars style variables; returns the number of colours to pop.
    int pushStyle(float scale);
    constexpr int kStyleVars = 12;

    // Drawing context for setting rows.
    struct Rows
    {
        float scale;
        ImFont *ui;
        ImFont *mono;
        const char **help;
        bool capturing; // a binding slot is waiting for input: arrows belong to it
    };

    // One setting row: label on the left, the value between two arrows on the right. Left/Right (keys, D-pad,
    // stick) or a click on an arrow steps it (rows with arrows claim those keys while focused); Enter/A or a click
    // elsewhere steps it forward. barFill >= 0 draws an amount bar instead of centred text; a click on the bar is
    // returned in clickFraction (0..1).
    int stepRow(const Rows &r, const char *id, const char *label, const char *value, const char *help,
                float barFill = -1.0f, float *clickFraction = nullptr, bool arrows = true);
    bool choiceRow(const Rows &r, const char *id, const char *label, int &index, const char *const *choices, int count,
                   const char *help);
    bool toggleRow(const Rows &r, const char *id, const char *label, bool &value, const char *help, const char *off = "OFF",
                   const char *on = "ON");
    bool amountRow(const Rows &r, const char *id, const char *label, int &value, int lo, int hi, int step, const char *help,
                   const char *zeroText = nullptr);
    bool actionRow(const Rows &r, const char *id, const char *label, const char *action, const char *help);
    void heading(const Rows &r, const char *label);

    // A bordered command button (CANCEL, QUIT TO DESKTOP).
    bool command(ImFont *ui, float scale, const char *label, ImU32 color);
}

#endif // PS2_UI_THEME_H
