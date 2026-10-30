#include "ps2_ui_theme.h"

#include "imgui_internal.h" // SetKeyOwner

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

namespace ps2_ui
{
    const Fonts &fonts()
    {
        static Fonts loaded = []
        {
            Fonts f;
            if (const char *windows = std::getenv("WINDIR"))
            {
                ImGuiIO &io = ImGui::GetIO();
                const auto load = [&](const char *file) -> ImFont *
                {
                    const std::string path = std::string(windows) + "\\Fonts\\" + file;
                    return std::filesystem::exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f) : nullptr;
                };
                f.plain = load("segoeui.ttf");
                f.ui = load("bahnschrift.ttf");
                f.mono = load("consola.ttf");
            }
            if (!f.ui)
                f.ui = f.plain;
            if (!f.mono)
                f.mono = f.plain;
            return f;
        }();
        return loaded;
    }

    ImU32 fade(ImU32 color, float alpha)
    {
        const ImU32 a = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    void panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border)
    {
        const ImVec2 pts[6] = {{a.x + cut, a.y}, {b.x, a.y}, {b.x, b.y - cut}, {b.x - cut, b.y}, {a.x, b.y}, {a.x, a.y + cut}};
        dl->AddConvexPolyFilled(pts, 6, fill);
        dl->AddPolyline(pts, 6, border, ImDrawFlags_Closed, 1.0f);
    }

    void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 border)
    {
        const ImVec2 pts[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
        if (fill)
            dl->AddConvexPolyFilled(pts, 4, fill);
        dl->AddPolyline(pts, 4, border, ImDrawFlags_Closed, 1.0f);
    }

    ImVec2 textSize(ImFont *font, float size, const char *text)
    {
        return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }

    void text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 color, const char *str)
    {
        dl->AddText(font ? font : ImGui::GetFont(), size, pos, color, str);
    }

    float prompt(ImDrawList *dl, ImFont *mono, ImFont *ui, float scale, ImVec2 pos, const char *key, const char *what, float alpha)
    {
        const float keySize = 13.0f * scale;
        const float labelSize = 17.0f * scale;
        const ImVec2 ks = textSize(mono, keySize, key);
        const float padX = 6.0f * scale;
        const float h = 20.0f * scale;
        dl->AddRect(pos, ImVec2(pos.x + ks.x + padX * 2.0f, pos.y + h), fade(kLine, alpha));
        text(dl, mono, keySize, ImVec2(pos.x + padX, pos.y + (h - ks.y) * 0.5f), fade(kLine, alpha), key);
        const float labelX = pos.x + ks.x + padX * 2.0f + 8.0f * scale;
        const ImVec2 ls = textSize(ui, labelSize, what);
        text(dl, ui, labelSize, ImVec2(labelX, pos.y + (h - ls.y) * 0.5f), fade(kTextDim, alpha), what);
        return labelX + ls.x - pos.x;
    }

    int pushStyle(float scale)
    {
        struct Entry
        {
            ImGuiCol index;
            ImU32 color;
        };
        const Entry colors[] = {
            {ImGuiCol_Text, kText},
            {ImGuiCol_TextDisabled, kTextDim},
            {ImGuiCol_WindowBg, 0},
            {ImGuiCol_ChildBg, 0},
            {ImGuiCol_PopupBg, kPanel},
            {ImGuiCol_Border, kLineDim},
            {ImGuiCol_FrameBg, kInk},
            {ImGuiCol_FrameBgHovered, IM_COL32(67, 227, 168, 40)},
            {ImGuiCol_FrameBgActive, IM_COL32(67, 227, 168, 70)},
            {ImGuiCol_Button, 0},
            {ImGuiCol_ButtonHovered, IM_COL32(67, 227, 168, 40)},
            {ImGuiCol_ButtonActive, IM_COL32(67, 227, 168, 80)},
            {ImGuiCol_Header, kHighlight},
            {ImGuiCol_HeaderHovered, IM_COL32(67, 227, 168, 28)},
            {ImGuiCol_HeaderActive, IM_COL32(67, 227, 168, 60)},
            {ImGuiCol_CheckMark, kLine},
            {ImGuiCol_SliderGrab, kLine},
            {ImGuiCol_SliderGrabActive, kText},
            {ImGuiCol_Separator, kLineDim},
            {ImGuiCol_NavCursor, kLine},
            {ImGuiCol_TableHeaderBg, IM_COL32(67, 227, 168, 24)},
            {ImGuiCol_TableBorderLight, kLineDim},
            {ImGuiCol_TableBorderStrong, kLineDim},
            {ImGuiCol_TableRowBg, 0},
            {ImGuiCol_TableRowBgAlt, IM_COL32(67, 227, 168, 10)},
            {ImGuiCol_ScrollbarBg, 0},
            {ImGuiCol_ScrollbarGrab, kLineDim},
            {ImGuiCol_ScrollbarGrabHovered, kLine},
            {ImGuiCol_ScrollbarGrabActive, kLine},
        };
        for (const Entry &e : colors)
            ImGui::PushStyleColor(e.index, e.color);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 6.0f * scale));
        return static_cast<int>(std::size(colors));
    }

    int stepRow(const Rows &r, const char *id, const char *label, const char *value, const char *help, float barFill,
                float *clickFraction, bool arrows)
    {
        const float s = r.scale;
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 40.0f * s;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + w, a.y + h);
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsItemFocused();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        if (hovered || focused)
        {
            dl->AddRectFilled(a, b, kHighlight);
            dl->AddRectFilled(a, ImVec2(a.x + 2.0f * s, b.y), kLine);
            if (help)
                *r.help = help;
        }
        const float labelSize = 19.0f * s;
        const ImVec2 ls = textSize(r.ui, labelSize, label);
        text(dl, r.ui, labelSize, ImVec2(a.x + 14.0f * s, a.y + (h - ls.y) * 0.5f), (hovered || focused) ? kText : kTextDim,
             label);

        const float vx1 = b.x - 8.0f * s;
        const float vx0 = vx1 - std::min(280.0f * s, w * 0.5f);
        const float cy = a.y + h * 0.5f;
        const float arrowZone = 26.0f * s;
        if (arrows)
        {
            const float t = 5.0f * s;
            const float lx = vx0 + arrowZone * 0.5f;
            const float rx = vx1 - arrowZone * 0.5f;
            dl->AddTriangleFilled(ImVec2(lx + t * 0.6f, cy - t), ImVec2(lx + t * 0.6f, cy + t), ImVec2(lx - t * 0.6f, cy), kLine);
            dl->AddTriangleFilled(ImVec2(rx - t * 0.6f, cy - t), ImVec2(rx - t * 0.6f, cy + t), ImVec2(rx + t * 0.6f, cy), kLine);
        }
        const float inner0 = vx0 + (arrows ? arrowZone : 0.0f);
        const float inner1 = vx1 - (arrows ? arrowZone : 0.0f);
        float bar0 = 0.0f, bar1 = 0.0f;
        if (barFill >= 0.0f)
        {
            const float valueSize = 16.0f * s;
            const ImVec2 vs = textSize(r.mono, valueSize, value);
            bar0 = inner0 + 4.0f * s;
            bar1 = inner1 - 58.0f * s;
            const float barH = 8.0f * s;
            dl->AddRect(ImVec2(bar0, cy - barH * 0.5f), ImVec2(bar1, cy + barH * 0.5f), kLineDim);
            dl->AddRectFilled(ImVec2(bar0, cy - barH * 0.5f),
                              ImVec2(bar0 + (bar1 - bar0) * std::clamp(barFill, 0.0f, 1.0f), cy + barH * 0.5f), kLine);
            text(dl, r.mono, valueSize, ImVec2(inner1 - vs.x, cy - vs.y * 0.5f), kText, value);
        }
        else
        {
            const float valueSize = 22.0f * s;
            const ImVec2 vs = textSize(r.ui, valueSize, value);
            text(dl, r.ui, valueSize, ImVec2((inner0 + inner1 - vs.x) * 0.5f, cy - vs.y * 0.5f), arrows ? kText : kLine, value);
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f * s));

        int step = 0;
        if (pressed)
        {
            const float mx = ImGui::GetIO().MousePos.x;
            if (!hovered)
                step = 1; // Enter / A
            else if (barFill >= 0.0f && clickFraction && mx >= bar0 && mx <= bar1)
                *clickFraction = (mx - bar0) / std::max(1.0f, bar1 - bar0);
            else if (arrows && mx >= vx0 && mx < vx0 + arrowZone)
                step = -1;
            else
                step = 1;
        }
        if (arrows && focused && !r.capturing)
        {
            // Claim Left/Right while focused (every frame, ahead of the press) so ImGui's navigation does not also
            // move the focus sideways, e.g. into the bindings table on the Controls page.
            const ImGuiID rowId = ImGui::GetItemID();
            for (const ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                       ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight})
                ImGui::SetKeyOwner(key, rowId);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft))
                step = -1;
            else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                     ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight))
                step = 1;
        }
        return step;
    }

    bool choiceRow(const Rows &r, const char *id, const char *label, int &index, const char *const *choices, int count,
                   const char *help)
    {
        index = std::clamp(index, 0, count - 1);
        const int step = stepRow(r, id, label, choices[index], help);
        if (!step)
            return false;
        index = (index + step + count) % count;
        return true;
    }

    bool toggleRow(const Rows &r, const char *id, const char *label, bool &value, const char *help, const char *off, const char *on)
    {
        const char *choices[2] = {off, on};
        int index = value ? 1 : 0;
        if (!choiceRow(r, id, label, index, choices, 2, help))
            return false;
        value = index != 0;
        return true;
    }

    bool amountRow(const Rows &r, const char *id, const char *label, int &value, int lo, int hi, int step, const char *help,
                   const char *zeroText)
    {
        char text[32];
        if (value == 0 && zeroText)
            std::snprintf(text, sizeof(text), "%s", zeroText);
        else
            std::snprintf(text, sizeof(text), "%d%%", value);
        float click = -1.0f;
        const float fill = static_cast<float>(value - lo) / static_cast<float>(hi - lo);
        const int dir = stepRow(r, id, label, text, help, fill, &click);
        int next = value;
        if (click >= 0.0f)
            next = lo + static_cast<int>(std::lround(click * static_cast<float>(hi - lo) / static_cast<float>(step))) * step;
        else if (dir)
            next = value + dir * step;
        next = std::clamp(next, lo, hi);
        if (next == value)
            return false;
        value = next;
        return true;
    }

    bool actionRow(const Rows &r, const char *id, const char *label, const char *action, const char *help)
    {
        return stepRow(r, id, label, action, help, -1.0f, nullptr, false) != 0;
    }

    void heading(const Rows &r, const char *label)
    {
        const float s = r.scale;
        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float size = 13.0f * s;
        const ImVec2 ts = textSize(r.mono, size, label);
        text(dl, r.mono, size, ImVec2(a.x, a.y), kLine, label);
        dl->AddLine(ImVec2(a.x + ts.x + 10.0f * s, a.y + ts.y * 0.5f), ImVec2(a.x + w, a.y + ts.y * 0.5f), kLineDim);
        ImGui::Dummy(ImVec2(w, ts.y + 6.0f * s));
    }

    bool command(ImFont *ui, float scale, const char *label, ImU32 color)
    {
        ImGui::PushFont(ui, 22.0f * scale);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushStyleColor(ImGuiCol_Border, color);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20.0f * scale, 8.0f * scale));
        const bool pressed = ImGui::Button(label);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        return pressed;
    }
}
