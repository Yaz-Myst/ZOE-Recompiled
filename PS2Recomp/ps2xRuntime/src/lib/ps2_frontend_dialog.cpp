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
bool ps2FrontEndPickFile(void *, const std::filesystem::path &, std::filesystem::path &)
{
    return false;
}
#endif
