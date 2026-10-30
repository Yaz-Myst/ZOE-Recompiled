#include "ps2_frontend.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <algorithm>
#include <string>
#include <vector>

bool ps2FrontEndPickFile(void *parentWindow, const std::filesystem::path &current, std::filesystem::path &picked)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    std::wstring folder;
    std::error_code ec;
    if (!current.empty() && std::filesystem::is_regular_file(current, ec))
    {
        const std::wstring name = current.filename().wstring();
        if (name.size() < buffer.size())
            std::copy(name.begin(), name.end(), buffer.begin());
        folder = current.parent_path().wstring();
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(parentWindow);
    ofn.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrTitle = L"Choose your disc image";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return false;
    picked = std::filesystem::path(buffer.data());
    return true;
}
#else
// Linux/Steam Deck: an in-game file browser instead of a native dialog. Steam Gaming Mode
// runs the game inside gamescope, where spawning zenity/kdialog is unreliable (and often
// unavailable); this picker needs only the window the game already has, works with the
// controller, and keeps the same look as the boot screen.

#include "imgui.h"
#include "raylib.h"
#include "rlImGui.h"
#include "ps2_ui_theme.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    std::string lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    struct Entry
    {
        std::filesystem::path path;
        std::string name;
        bool directory = false;
        bool parent = false;
    };

    std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char *>(text.data()), text.size());
    }

    // The folder a first run should start in: next to the last image, else ~/Downloads,
    // else the user's home, else the working directory.
    std::filesystem::path startDirectory(const std::filesystem::path &current)
    {
        std::error_code ec;
        if (!current.empty())
        {
            if (std::filesystem::is_directory(current, ec))
                return current;
            if (std::filesystem::is_regular_file(current, ec))
                return current.parent_path();
        }
        if (const char *home = std::getenv("HOME"); home && *home)
        {
            const std::filesystem::path downloads = std::filesystem::path(home) / "Downloads";
            if (std::filesystem::is_directory(downloads, ec))
                return downloads;
            return home;
        }
        return std::filesystem::current_path(ec);
    }

    std::vector<Entry> listDirectory(const std::filesystem::path &directory)
    {
        std::vector<Entry> entries;
        std::error_code ec;

        Entry parent;
        parent.path = directory.parent_path();
        parent.name = "..";
        parent.directory = true;
        parent.parent = true;
        if (!directory.parent_path().empty() && directory.parent_path() != directory)
            entries.push_back(parent);

        std::vector<Entry> folders;
        std::vector<Entry> images;
        for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            Entry entry;
            entry.path = it->path();
            entry.name = utf8(entry.path.filename());
            if (entry.name.empty() || entry.name[0] == '.')
                continue;
            std::error_code typeError;
            entry.directory = it->is_directory(typeError);
            if (typeError)
                continue;
            if (entry.directory)
                folders.push_back(std::move(entry));
            else if (lower(entry.path.extension().string()) == ".iso")
                images.push_back(std::move(entry));
        }
        const auto byName = [](const Entry &a, const Entry &b)
        { return lower(a.name) < lower(b.name); };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(images.begin(), images.end(), byName);
        entries.insert(entries.end(), std::make_move_iterator(folders.begin()), std::make_move_iterator(folders.end()));
        entries.insert(entries.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
        return entries;
    }

    bool gamepadPressed(int button)
    {
        return IsGamepadAvailable(0) && IsGamepadButtonPressed(0, button);
    }
}

bool ps2FrontEndPickFile(void *parentWindow, const std::filesystem::path &current, std::filesystem::path &picked)
{
    (void)parentWindow;
    std::filesystem::path directory = startDirectory(current);
    std::vector<Entry> entries = listDirectory(directory);
    int selected = 0;
    bool done = false;
    bool cancelled = false;
    bool confirmReleased = false; // the button that opened BROWSE may still be held

    while (!done && !cancelled)
    {
        if (WindowShouldClose())
        {
            cancelled = true;
            break;
        }

        // SDL2 drives the controllers (same source as the game); raylib's own gamepad is kept
        // for keyboards/setups where SDL did not open the pad. Mouse works through Selectables.
        ps2InputUpdate();
        const Ps2MenuPad pad = ps2InputMenuPad();
        if (!pad.confirm)
            confirmReleased = true;

        const bool up = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || gamepadPressed(GAMEPAD_BUTTON_LEFT_FACE_UP) || pad.up;
        const bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || gamepadPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN) || pad.down;
        const bool back = IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_LEFT) ||
                          IsKeyPressed(KEY_B) || gamepadPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                          gamepadPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT) || pad.back;
        const bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE) ||
                              IsKeyPressed(KEY_RIGHT) || gamepadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                              gamepadPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || (confirmReleased && pad.confirm);

        if (!entries.empty())
        {
            if (up)
                selected = (selected + static_cast<int>(entries.size()) - 1) % static_cast<int>(entries.size());
            if (down)
                selected = (selected + 1) % static_cast<int>(entries.size());
            selected = std::clamp(selected, 0, static_cast<int>(entries.size()) - 1);
        }

        if (back)
        {
            if (directory.has_parent_path() && directory.parent_path() != directory)
            {
                directory = directory.parent_path();
                entries = listDirectory(directory);
                selected = 0;
            }
            else
                cancelled = true;
        }

        if (activate && !entries.empty())
        {
            const Entry &entry = entries[static_cast<size_t>(selected)];
            if (entry.directory)
            {
                directory = entry.path;
                entries = listDirectory(directory);
                selected = 0;
            }
            else
            {
                picked = entry.path;
                done = true;
            }
        }

        BeginDrawing();
        ClearBackground(Color{2, 10, 9, 255});
        rlImGuiBegin();

        const ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(980.0f, io.DisplaySize.x - 40.0f),
                                       std::min(620.0f, io.DisplaySize.y - 40.0f)),
                                 ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.015f, 0.06f, 0.05f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.26f, 0.89f, 0.66f, 0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        if (ImGui::Begin("Choose your disc image", nullptr, flags))
        {
            ImGui::TextUnformatted("DISC IMAGE");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", utf8(directory).c_str());
            ImGui::Separator();
            ImGui::BeginChild("##files", ImVec2(0.0f, -44.0f), false);
            for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            {
                const Entry &entry = entries[static_cast<size_t>(i)];
                const std::string label = entry.directory ? ("[DIR]  " + entry.name) : entry.name;
                if (ImGui::Selectable(label.c_str(), i == selected,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                {
                    selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (entry.directory)
                        {
                            directory = entry.path;
                            entries = listDirectory(directory);
                            selected = 0;
                        }
                        else
                        {
                            picked = entry.path;
                            done = true;
                        }
                    }
                }
                if (i == selected)
                    ImGui::SetScrollHereY(0.5f);
            }
            if (entries.empty())
                ImGui::TextDisabled("No folders or .iso files here.");
            ImGui::EndChild();
            ImGui::Separator();
            ImGui::TextDisabled("Enter / A: open or choose    Backspace / B: up or cancel    Esc: cancel");
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        rlImGuiEnd();
        EndDrawing();
    }

    return done && !cancelled;
}
#endif
