#include "ps2_frontend.h"
#include "ps2_host_settings.h"
#include "ps2_input.h"
#include "ps2_runtime.h"
#include "ps2_ui_theme.h"

#include "imgui.h"
#include "imgui_internal.h" // GetFocusID
#include "raylib.h"
#include "rlImGui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    const PS2FrontEndGame *g_game = nullptr;

    void setEnv(const char *name, const std::string &value)
    {
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    bool envSet(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr && value[0] != '\0';
    }

    std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char *>(text.data()), text.size());
    }

    std::filesystem::path fromUtf8(const std::string &text)
    {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
    }

    uint32_t crc32(const uint8_t *data, size_t size)
    {
        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    // ISO 9660 names compare upper case, without the ";1" version and a trailing dot.
    std::string isoName(std::string_view raw)
    {
        std::string out;
        for (const char ch : raw)
        {
            if (ch == ';')
                break;
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        while (!out.empty() && out.back() == '.')
            out.pop_back();
        return out;
    }

    // Read-only view of an ISO 9660 disc image with 2048-byte sectors (what PS2X_CD_IMAGE reads).
    class IsoImage
    {
    public:
        struct Entry
        {
            std::string name;
            uint32_t lba = 0;
            uint32_t size = 0;
            bool directory = false;
        };

        // Empty = a readable image; otherwise the reason, in words for the player.
        std::string open(const std::filesystem::path &path)
        {
            m_file = std::ifstream(path, std::ios::binary);
            if (!m_file)
                return "The file cannot be opened.";
            std::error_code ec;
            m_fileSize = std::filesystem::file_size(path, ec);
            if (ec)
                m_fileSize = 0;
            std::array<uint8_t, kSector> pvd{};
            if (!readAt(16ull * kSector, pvd.data(), pvd.size()) || pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
            {
                // Raw 2352-byte sectors (.bin): the volume descriptor follows the sync and header bytes.
                for (const uint64_t offset : {16ull * 2352ull + 16ull, 16ull * 2352ull + 24ull})
                {
                    std::array<uint8_t, 6> raw{};
                    if (readAt(offset, raw.data(), raw.size()) && raw[0] == 1 && std::memcmp(raw.data() + 1, "CD001", 5) == 0)
                        return "This is a raw .bin image. Convert it to an .iso (2048-byte sectors) first.";
                }
                return "This file is not a disc image (no ISO 9660 volume found).";
            }
            m_root = record(pvd.data() + 156);
            if (!m_root.directory || m_root.size == 0)
                return "The disc's root folder cannot be read.";
            return {};
        }

        // path: "MODULES/PADMAN.IRX" (case and ";1" do not matter)
        bool find(std::string_view path, Entry &out)
        {
            Entry current = m_root;
            size_t begin = 0;
            while (begin < path.size())
            {
                size_t end = path.find_first_of("/\\", begin);
                if (end == std::string_view::npos)
                    end = path.size();
                const std::string wanted = isoName(path.substr(begin, end - begin));
                begin = end + 1;
                if (wanted.empty())
                    continue;
                std::vector<Entry> children;
                if (!list(current, children))
                    return false;
                const auto it = std::find_if(children.begin(), children.end(), [&](const Entry &e) { return e.name == wanted; });
                if (it == children.end())
                    return false;
                current = *it;
            }
            out = current;
            return true;
        }

        bool list(const Entry &directory, std::vector<Entry> &out)
        {
            if (!directory.directory || directory.size > (16u << 20))
                return false;
            std::vector<uint8_t> data;
            if (!read(directory, data))
                return false;
            size_t offset = 0;
            while (offset < data.size())
            {
                const uint8_t length = data[offset];
                if (length == 0) // records never cross a sector: the rest of this one is padding
                {
                    offset = (offset / kSector + 1) * kSector;
                    continue;
                }
                if (length < 34 || offset + length > data.size() || 33u + data[offset + 32] > length)
                    return false;
                const uint8_t nameLength = data[offset + 32];
                const uint8_t first = data[offset + 33];
                if (!(nameLength == 1 && (first == 0 || first == 1))) // skip "." and ".."
                    out.push_back(record(data.data() + offset));
                offset += length;
            }
            return true;
        }

        // Reads a file up to maxBytes long that lies inside the image (a damaged record never allocates gigabytes).
        bool read(const Entry &entry, std::vector<uint8_t> &out, uint32_t maxBytes = 16u << 20)
        {
            const uint64_t offset = static_cast<uint64_t>(entry.lba) * kSector;
            if (entry.size > maxBytes || offset + entry.size > m_fileSize)
                return false;
            out.resize(entry.size);
            return entry.size == 0 || readAt(offset, out.data(), entry.size);
        }

    private:
        static constexpr size_t kSector = 2048;

        static uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        static Entry record(const uint8_t *r)
        {
            Entry e;
            e.lba = le32(r + 2);
            e.size = le32(r + 10);
            e.directory = (r[25] & 0x02u) != 0;
            e.name = isoName(std::string_view(reinterpret_cast<const char *>(r + 33), r[32]));
            return e;
        }

        bool readAt(uint64_t offset, uint8_t *dst, size_t size)
        {
            m_file.clear();
            m_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            m_file.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(size));
            return m_file.gcount() == static_cast<std::streamsize>(size);
        }

        std::ifstream m_file;
        uint64_t m_fileSize = 0;
        Entry m_root;
    };

    // A disc file name is used as a host file name: one plain component, nothing that leaves the target folder.
    bool safeFileName(const std::string &name)
    {
        return !name.empty() && name != "." && name.find("..") == std::string::npos &&
               name.find_first_of("\\/:*?\"<>|") == std::string::npos;
    }

    // The IOP opens the disc image through a narrow (ANSI code page) path, so the image's path must survive that
    // conversion. Empty = fine; otherwise the reason for the player.
    std::string narrowPathProblem(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        try
        {
            if (std::filesystem::path(path.string()) == path)
                return {};
        }
        catch (const std::exception &)
        {
        }
        return "The game cannot open a disc image whose folder or file name has these characters yet. Move the image to "
               "a folder with a plain name (for example C:\\Games) and pick it again.";
#else
        // Linux paths are bytes and are passed to the runtime unchanged, including spaces and
        // non-ASCII characters. Nothing to reject here.
        (void)path;
        return {};
#endif
    }

    // The boot file SYSTEM.CNF names ("BOOT2 = cdrom0:\SLUS_201.48;1" -> "SLUS_201.48"); empty if none.
    std::string bootFileOf(const std::vector<uint8_t> &systemCnf)
    {
        const std::string text(systemCnf.begin(), systemCnf.end());
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find_first_of("\r\n", pos);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || isoName(line.substr(0, line.find_first_of(" \t="))) != "BOOT2")
                continue;
            std::string value = line.substr(eq + 1);
            const size_t colon = value.find(':');
            if (colon != std::string::npos)
                value = value.substr(colon + 1);
            const size_t start = value.find_first_not_of(" \t\\/");
            return start == std::string::npos ? std::string() : isoName(value.substr(start));
        }
        return {};
    }

    bool readHostFile(const std::filesystem::path &path, std::vector<uint8_t> &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    // Written next to the target first, then moved over it, so a failed write never leaves half a file.
    std::string writeHostFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
    {
        std::filesystem::path temp = path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close(); // flushes: a full disk shows up here, not after the rename
            if (!out)
                return "Cannot write " + utf8(path) + " (disk full or not writable).";
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
            return "Cannot write " + utf8(path) + ": " + ec.message();
        return {};
    }

    // Text cut to maxWidth with "..." at the end (or at the start for paths, keeping the file name).
    std::string fitText(ImFont *font, float size, const std::string &text, float maxWidth, bool keepEnd = false)
    {
        if (ps2_ui::textSize(font, size, text.c_str()).x <= maxWidth)
            return text;
        std::string cut = text;
        while (!cut.empty())
        {
            if (keepEnd)
                cut.erase(0, 1);
            else
                cut.pop_back();
            const std::string candidate = keepEnd ? "..." + cut : cut + "...";
            if (ps2_ui::textSize(font, size, candidate.c_str()).x <= maxWidth)
                return candidate;
        }
        return "...";
    }

    class FrontEnd
    {
    public:
        FrontEnd(PS2Runtime &runtime, const PS2FrontEndHooks &hooks, const PS2FrontEndGame &game)
            : m_runtime(runtime), m_hooks(hooks), m_game(game)
        {
            m_settingsDir = ps2SettingsDirectory();
            m_gameDir = m_settingsDir / "game";
            if (const char *env = std::getenv("PS2X_STATE_DIR"); env && *env)
                m_stateDir = std::filesystem::path(env);
            else
                m_stateDir = m_settingsDir / "states";
            if (const char *test = std::getenv("PS2X_FRONTEND_TEST"); test && *test && *test != '0')
                m_test = *test - '0';
            if (const char *pick = std::getenv("PS2X_FRONTEND_PICK"); pick && *pick)
                m_testPick = std::filesystem::path(pick);
        }

        bool run()
        {
            const bool wideWas = ps2GameWidescreenSupported();
            if (m_game.widescreenOption)
                ps2SetGameWidescreenSupported(true);

            const std::string stored = ps2Settings().discImage;
            m_discReady = storedDiscReady(&m_entryProblem);
            m_path = stored.empty() ? std::filesystem::path() : fromUtf8(stored);
            if (m_discReady)
                showBoot();
            else
                showDisc(true);
            std::cout << "[frontend] " << (m_discReady ? "disc ready: " + stored : "first run: no checked disc" +
                                                                                       (m_entryProblem.empty() ? std::string() : " (" + m_entryProblem + ")"))
                      << std::endl;

            bool settingsWereOpen = false;
            while (!m_done)
            {
                if (WindowShouldClose() || m_runtime.isStopRequested())
                    break;
                // Controllers for the screens and the Settings page (connections, rebinding, the menu button); the
                // game's pad stays idle until Start.
                ps2GameInputBlocked().store(true, std::memory_order_relaxed);
                ps2InputUpdate();
                const Ps2MenuPad pad = m_hooks.feedControllerNav ? m_hooks.feedControllerNav() : Ps2MenuPad{};
                m_padBackPressed = pad.back && !m_padBackWasDown;
                m_padBackWasDown = pad.back;
                m_padConfirmDown = pad.confirm;
                const bool settings = m_hooks.settingsOpen && m_hooks.settingsOpen();
                if (settings && m_hooks.updateSettings)
                    m_hooks.updateSettings();
                if (settingsWereOpen && !(m_hooks.settingsOpen && m_hooks.settingsOpen()))
                    m_focusRequest = true; // back from Settings: the cursor returns to the screen
                settingsWereOpen = m_hooks.settingsOpen && m_hooks.settingsOpen();

                BeginDrawing();
                ClearBackground(Color{2, 10, 9, 255});
                rlImGuiBegin();
                drawBackdrop();
                if (m_hooks.settingsOpen && m_hooks.settingsOpen())
                {
                    if (m_hooks.drawSettings)
                        m_hooks.drawSettings();
                    m_screenSince = GetTime(); // test walk-through: the settings time counts from their close
                }
                else
                    drawScreen();
                rlImGuiEnd();
                EndDrawing();

                if (m_startRequested && (!confirmHeld() || GetTime() - m_startRequestedAt > 1.5))
                    finishStart(); // the button that chose START is up: it never reaches the game
                if (m_browseRequested)
                {
                    m_browseRequested = false;
                    browse();
                }
                stepCheck();
                testWalkThrough();
            }

            ps2GameInputBlocked().store(false, std::memory_order_relaxed);
            ps2SetGameWidescreenSupported(wideWas);
            ps2SaveSettings();
            return m_start;
        }

    private:
        enum class Screen
        {
            Boot,
            Disc
        };
        enum Step
        {
            kStepRead,
            kStepGameId,
            kStepVersion,
            kStepCopy,
            kStepFolders,
            kStepCount
        };
        enum class StepState
        {
            Waiting,
            Ok,
            Failed
        };

        // Layout: the mock-up's 1280x720 frame, scaled to fit the window and centred. Positions and box sizes are
        // whole pixels, so 1-pixel borders stay on one pixel column at any scale.
        struct Frame
        {
            float s = 1.0f;
            ImVec2 o{0.0f, 0.0f};
            ImVec2 display{0.0f, 0.0f};
            ImVec2 at(float x, float y) const { return ImVec2(std::round(o.x + x * s), std::round(o.y + y * s)); }
            float px(float v) const { return v * s; }
            float pxr(float v) const { return std::round(v * s); }
        };

        Frame frame() const
        {
            Frame f;
            f.display = ImGui::GetIO().DisplaySize;
            f.s = std::max(0.5f, std::min(f.display.x / 1280.0f, f.display.y / 720.0f));
            f.o = ImVec2((f.display.x - 1280.0f * f.s) * 0.5f, (f.display.y - 720.0f * f.s) * 0.5f);
            return f;
        }

        // ---- disc state -------------------------------------------------------------------------------------------

        std::filesystem::path elfPath() const { return m_gameDir / m_game.elfName; }

        static constexpr uint32_t kMaxSystemCnf = 64u << 10;

        // The copied program is the one this build was made from.
        bool installedProgramMatches() const
        {
            std::vector<uint8_t> elf;
            return readHostFile(elfPath(), elf) && elf.size() == m_game.elfSize && crc32(elf.data(), elf.size()) == m_game.elfCrc32;
        }

        // Every file of the copied module folders is there with the disc's size (a truncated or deleted module
        // would make the IOP fail at Start).
        bool installedModulesMatch(IsoImage &iso) const
        {
            for (const char *const *dir = m_game.copyDirs; dir && *dir; ++dir)
            {
                IsoImage::Entry folder;
                std::vector<IsoImage::Entry> entries;
                if (!iso.find(*dir, folder) || !iso.list(folder, entries))
                    return false;
                for (const IsoImage::Entry &entry : entries)
                {
                    if (entry.directory)
                        continue;
                    std::error_code ec;
                    if (!safeFileName(entry.name) || std::filesystem::file_size(m_gameDir / *dir / entry.name, ec) != entry.size || ec)
                        return false;
                }
            }
            return true;
        }

        // Image readable and booting our program (small reads; the full check ran when it was picked).
        std::string quickDiscProblem(const std::filesystem::path &image, IsoImage &iso)
        {
            if (std::string problem = narrowPathProblem(image); !problem.empty())
                return problem;
            if (std::string problem = iso.open(image); !problem.empty())
                return problem;
            IsoImage::Entry cnf;
            std::vector<uint8_t> bytes;
            if (!iso.find("SYSTEM.CNF", cnf) || !iso.read(cnf, bytes, kMaxSystemCnf) || bootFileOf(bytes) != isoName(m_game.elfName))
                return std::string("The disc image no longer starts ") + m_game.discLabel + ".";
            return {};
        }

        bool storedDiscReady(std::string *problem)
        {
            const std::string &stored = ps2Settings().discImage;
            if (stored.empty())
                return false;
            const std::filesystem::path image = fromUtf8(stored);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(image, ec))
            {
                *problem = "The disc image used last time is gone: " + stored;
                return false;
            }
            IsoImage iso;
            if (std::string why = quickDiscProblem(image, iso); !why.empty())
            {
                *problem = why;
                return false;
            }
            if (!installedProgramMatches() || !installedModulesMatch(iso))
            {
                *problem = "Files copied from the disc are missing or changed. Check the disc again.";
                return false;
            }
            return true;
        }

        void startCheck(const std::filesystem::path &image)
        {
            m_path = image;
            m_steps.fill(StepState::Waiting);
            m_error.clear();
            m_entryProblem.clear();
            m_verified = false;
            m_checking = true;
            m_nextStep = kStepRead;
            m_stepAt = GetTime() + 0.15;
            m_iso = IsoImage();
            m_elf.clear();
            m_screenSince = GetTime();
        }

        // One step per frame, a moment apart, so the player sees the sequence.
        void stepCheck()
        {
            if (!m_checking || GetTime() < m_stepAt)
                return;
            const std::string problem = runStep(m_nextStep);
            if (!problem.empty())
            {
                m_steps[m_nextStep] = StepState::Failed;
                m_error = problem;
                m_checking = false;
                m_discReady = storedDiscReady(&m_entryProblem); // a failed check keeps the disc checked before
                m_entryProblem.clear();
                std::cout << "[frontend] disc check failed at step " << m_nextStep << ": " << problem << std::endl;
                m_focusRequest = true;
                m_screenSince = GetTime();
                return;
            }
            m_steps[m_nextStep] = StepState::Ok;
            ++m_nextStep;
            m_stepAt = GetTime() + 0.15;
            if (m_nextStep == kStepCount)
            {
                m_checking = false;
                m_verified = true;
                m_discReady = true;
                m_focusRequest = true;
                m_screenSince = GetTime();
                std::cout << "[frontend] disc checked and game program copied: " << utf8(m_path) << " -> " << utf8(m_gameDir) << std::endl;
            }
        }

        std::string runStep(int step)
        {
            switch (step)
            {
            case kStepRead:
                if (std::string problem = narrowPathProblem(m_path); !problem.empty())
                    return problem;
                return m_iso.open(m_path);
            case kStepGameId:
            {
                IsoImage::Entry cnf;
                std::vector<uint8_t> bytes;
                if (!m_iso.find("SYSTEM.CNF", cnf) || !m_iso.read(cnf, bytes, kMaxSystemCnf))
                    return "No SYSTEM.CNF on this disc: it is not a PlayStation 2 game disc.";
                const std::string boot = bootFileOf(bytes);
                if (boot != isoName(m_game.elfName))
                    return std::string("This disc starts ") + (boot.empty() ? "no program" : boot) + ", not " + m_game.elfName + ". Pick your " +
                           m_game.discLabel + " disc image.";
                return {};
            }
            case kStepVersion:
            {
                IsoImage::Entry elf;
                if (!m_iso.find(m_game.elfName, elf))
                    return std::string("The game program ") + m_game.elfName + " is not on the disc image.";
                uint32_t crc = 0;
                if (elf.size == m_game.elfSize) // another size is another version: no need to read it
                {
                    if (!m_iso.read(elf, m_elf, m_game.elfSize))
                        return std::string("The game program ") + m_game.elfName + " cannot be read from the disc image.";
                    crc = crc32(m_elf.data(), m_elf.size());
                }
                if (elf.size != m_game.elfSize || crc != m_game.elfCrc32)
                {
                    char detail[112];
                    std::snprintf(detail, sizeof(detail), " (size %u, CRC %08X; this build needs size %u, CRC %08X).", elf.size, crc,
                                  m_game.elfSize, m_game.elfCrc32);
                    return std::string("This disc has a different version of ") + m_game.elfName + detail;
                }
                return {};
            }
            case kStepCopy:
                return copyFromDisc();
            case kStepFolders:
            {
                std::error_code ec;
                std::filesystem::create_directories(m_stateDir, ec);
                if (ec)
                    return "Cannot create the save state folder " + utf8(m_stateDir) + ": " + ec.message();
                ps2Settings().discImage = utf8(m_path);
                if (!ps2SaveSettings())
                    return "Cannot save the settings file in " + utf8(m_settingsDir) + ". The disc would be asked for again next time.";
                return {};
            }
            default:
                return {};
            }
        }

        std::string copyFromDisc()
        {
            std::error_code ec;
            std::filesystem::create_directories(m_gameDir, ec);
            if (ec)
                return "Cannot create " + utf8(m_gameDir) + ": " + ec.message();
            if (std::string problem = writeHostFile(elfPath(), m_elf); !problem.empty())
                return problem;
            IsoImage::Entry cnf;
            std::vector<uint8_t> bytes;
            if (m_iso.find("SYSTEM.CNF", cnf) && m_iso.read(cnf, bytes, kMaxSystemCnf))
                if (std::string problem = writeHostFile(m_gameDir / "SYSTEM.CNF", bytes); !problem.empty())
                    return problem;
            size_t files = 0;
            for (const char *const *dir = m_game.copyDirs; dir && *dir; ++dir)
            {
                IsoImage::Entry folder;
                std::vector<IsoImage::Entry> entries;
                if (!m_iso.find(*dir, folder) || !m_iso.list(folder, entries))
                    return std::string("The folder ") + *dir + " cannot be read from the disc image.";
                const std::filesystem::path target = m_gameDir / *dir;
                std::filesystem::create_directories(target, ec);
                if (ec)
                    return "Cannot create " + utf8(target) + ": " + ec.message();
                for (const IsoImage::Entry &entry : entries)
                {
                    if (entry.directory)
                        continue;
                    if (!safeFileName(entry.name))
                        return std::string("The disc image has a damaged file name in ") + *dir + ".";
                    if (!m_iso.read(entry, bytes))
                        return std::string("Cannot read ") + *dir + "/" + entry.name + " from the disc image.";
                    if (std::string problem = writeHostFile(target / entry.name, bytes); !problem.empty())
                        return problem;
                    ++files;
                }
            }
            if (!installedProgramMatches() || !installedModulesMatch(m_iso))
                return "The copied files do not read back correctly from " + utf8(m_gameDir) + ".";
            std::cout << "[frontend] copied " << m_game.elfName << ", SYSTEM.CNF and " << files << " module files" << std::endl;
            return {};
        }

        void browse()
        {
            std::filesystem::path picked;
            if (!m_testPick.empty())
                picked = m_testPick;
            else if (!ps2FrontEndPickFile(GetWindowHandle(), m_path, picked))
            {
                m_focusRequest = true;
                return;
            }
            startCheck(picked);
        }

        // ---- screens ----------------------------------------------------------------------------------------------

        void showBoot()
        {
            m_screen = Screen::Boot;
            m_focusRequest = true;
            m_screenSince = GetTime();
        }

        void showDisc(bool firstRun)
        {
            m_screen = Screen::Disc;
            m_firstRun = firstRun;
            m_checking = false;
            m_error.clear();
            // Reached from the boot screen the stored disc passed at start: show it as checked.
            m_verified = !firstRun && m_discReady;
            m_steps.fill(m_verified ? StepState::Ok : StepState::Waiting);
            m_focusRequest = true;
            m_screenSince = GetTime();
        }

        // START chosen: the game starts once the confirming key / button is released (finishStart), or after 1.5 s at
        // the latest (a stuck or virtual controller reporting the button as held must not stop the start).
        void start()
        {
            if (!m_startRequested)
                m_startRequestedAt = GetTime();
            m_startRequested = true;
        }

        bool confirmHeld() const
        {
            return m_padConfirmDown || IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE) ||
                   IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        }

        void finishStart()
        {
            // As play.bat runs it: the working folder is the game program's folder, states in their own folder. Path
            // switches given relative to the launch folder keep pointing there.
            for (const char *name : {"PS2X_CD_IMAGE", "PS2X_STATE_DIR", "PS2X_DIAG_DIR", "PS2X_FRAME_LOG", "PS2X_INPUT_RECORD"})
            {
                const char *value = std::getenv(name);
                if (value && *value && std::filesystem::path(value).is_relative())
                    setEnv(name, std::filesystem::absolute(std::filesystem::path(value)).string());
            }
            std::error_code ec;
            std::filesystem::current_path(m_gameDir, ec);
            if (ec)
                std::cerr << "[frontend] cannot change the working folder to " << utf8(m_gameDir) << ": " << ec.message() << std::endl;
            if (!envSet("PS2X_STATE_DIR"))
                setEnv("PS2X_STATE_DIR", m_stateDir.string());
            m_start = true;
            m_done = true;
            std::cout << "[frontend] start: " << utf8(elfPath()) << std::endl;
        }

        void drawBackdrop()
        {
            using namespace ps2_ui;
            const Frame f = frame();
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            bg->AddRectFilled(ImVec2(0.0f, 0.0f), f.display, kInk);
            const ImU32 grid = IM_COL32(67, 227, 168, 13);
            const float step = f.px(40.0f);
            for (float x = std::fmod(f.o.x, step); x < f.display.x; x += step)
                bg->AddLine(ImVec2(x, 0.0f), ImVec2(x, f.display.y), grid);
            for (float y = std::fmod(f.o.y, step); y < f.display.y; y += step)
                bg->AddLine(ImVec2(0.0f, y), ImVec2(f.display.x, y), grid);
        }

        // Top strip: two labels and a line across the window.
        void drawTopBar(const Frame &f, const char *left, const char *right)
        {
            using namespace ps2_ui;
            const Fonts &fn = fonts();
            ImDrawList *dl = ImGui::GetBackgroundDrawList();
            const float h = f.pxr(44.0f);
            const float size = f.px(13.0f);
            const ImVec2 ls = textSize(fn.mono, size, left);
            text(dl, fn.mono, size, ImVec2(f.o.x + f.px(40.0f), (h - ls.y) * 0.5f), kTextDim, left);
            const ImVec2 rs = textSize(fn.mono, size, right);
            text(dl, fn.mono, size, ImVec2(f.o.x + f.px(1240.0f) - rs.x, (h - rs.y) * 0.5f), kTextDim, right);
            dl->AddLine(ImVec2(0.0f, h), ImVec2(f.display.x, h), kLineDim);
        }

        float footerTop(const Frame &f, float height) const { return std::round(f.display.y - f.px(height)); }

        void drawFooterLine(const Frame &f, float height)
        {
            const float y = footerTop(f, height);
            ImGui::GetBackgroundDrawList()->AddLine(ImVec2(0.0f, y), ImVec2(f.display.x, y), ps2_ui::kLineDim);
        }

        // Key prompts right-aligned in the footer.
        void drawPrompts(const Frame &f, float height, std::initializer_list<std::pair<const char *, const char *>> prompts)
        {
            using namespace ps2_ui;
            const Fonts &fn = fonts();
            float x = f.o.x + f.px(1240.0f);
            const float y = footerTop(f, height) + (f.px(height) - f.px(20.0f)) * 0.5f;
            for (const auto &p : prompts)
            {
                const float w = textSize(fn.mono, f.px(13.0f), p.first).x + f.px(20.0f) + textSize(fn.ui, f.px(17.0f), p.second).x;
                x -= w;
                prompt(ImGui::GetBackgroundDrawList(), fn.mono, fn.ui, f.s, ImVec2(x, y), p.first, p.second);
                x -= f.px(28.0f);
            }
        }

        // No keyboard / controller cursor in the screen (the window lost focus, or a click on empty space): the
        // screen's default entry shows as selected, so one entry is always marked.
        static bool noCursor() { return !ImGui::IsWindowFocused() || ImGui::GetFocusID() == 0; }

        // Big menu entry: diamond and label; highlighted under the mouse or the keyboard / controller cursor.
        bool menuItem(const Frame &f, const char *label, ImVec2 a, bool focus, bool isDefault = false)
        {
            using namespace ps2_ui;
            const Fonts &fn = fonts();
            const ImVec2 size(f.pxr(380.0f), f.pxr(52.0f));
            ImGui::SetCursorScreenPos(a);
            if (focus)
                ImGui::SetKeyboardFocusHere();
            const bool idle = noCursor();
            // EnableNav: ImGui skips invisible buttons in keyboard/gamepad navigation by default,
            // which is why the boot menu needed a mouse or the keyboard's direct focus.
            const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused() || (isDefault && idle);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + size.x, a.y + size.y);
            if (hot)
            {
                dl->AddRectFilled(a, b, IM_COL32(67, 227, 168, 31));
                dl->AddRectFilled(a, ImVec2(a.x + f.px(2.0f), b.y), kLine);
            }
            const float cy = a.y + size.y * 0.5f;
            diamond(dl, ImVec2(a.x + f.px(25.0f), cy), f.px(5.0f), hot ? kLine : 0, hot ? kLine : kLineDim);
            const float labelSize = f.px(34.0f);
            const ImVec2 ls = textSize(fn.ui, labelSize, label);
            text(dl, fn.ui, labelSize, ImVec2(a.x + f.px(46.0f), cy - ls.y * 0.5f), hot ? kText : kTextDim, label);
            return pressed;
        }

        // Cut-corner panel behind a screen's content. Its outline is a polyline, which (unlike AddRect) is not moved
        // to pixel centres by ImGui: the half-pixel inset keeps the 1-pixel border on one row / column.
        static void screenPanel(const Frame &f, ImVec2 a, ImVec2 b)
        {
            ps2_ui::panel(ImGui::GetBackgroundDrawList(), ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), f.pxr(18.0f),
                          ps2_ui::kPanel, ps2_ui::kLineDim);
        }

        static float commandWidth(const Frame &f, const char *label)
        {
            return std::round(ps2_ui::textSize(ps2_ui::fonts().ui, f.px(24.0f), label).x + f.px(44.0f));
        }

        // Bordered command (BROWSE, BACK, CONTINUE); filled = the primary action. Returns its width in `width`.
        bool commandButton(const Frame &f, const char *label, ImVec2 a, bool filled, bool focus, float *width = nullptr,
                           ImU32 color = ps2_ui::kLine, bool isDefault = false)
        {
            using namespace ps2_ui;
            const Fonts &fn = fonts();
            const float size = f.px(24.0f);
            const ImVec2 ts = textSize(fn.ui, size, label);
            const ImVec2 box(commandWidth(f, label), f.pxr(44.0f));
            if (width)
                *width = box.x;
            ImGui::SetCursorScreenPos(a);
            if (focus)
                ImGui::SetKeyboardFocusHere();
            const bool idle = noCursor();
            const bool pressed = ImGui::InvisibleButton(label, box, ImGuiButtonFlags_EnableNav);
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused() || (isDefault && idle);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + box.x, a.y + box.y);
            if (filled)
                dl->AddRectFilled(a, b, hot ? kText : color);
            else if (hot)
                dl->AddRectFilled(a, b, fade(color, 0.16f));
            dl->AddRect(a, b, hot && !filled ? kText : color);
            const ImU32 textColor = filled ? kInk : (hot ? kText : color);
            text(dl, fn.ui, size, ImVec2(a.x + (box.x - ts.x) * 0.5f, a.y + (box.y - ts.y) * 0.5f), textColor, label);
            return pressed;
        }

        void beginScreenWindow()
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            if (m_focusRequest)
                ImGui::SetNextWindowFocus();
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, 0); // the entries draw their own highlight
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("##zr-frontend", nullptr, flags);
        }

        void endScreenWindow()
        {
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        void drawScreen()
        {
            beginScreenWindow();
            if (m_screen == Screen::Boot)
                drawBoot();
            else
                drawDisc();
            endScreenWindow();
        }

        int countSaveStates() const
        {
            int count = 0;
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(m_stateDir, ec))
            {
                const std::string name = utf8(entry.path().filename());
                if (entry.is_regular_file(ec) && name.find(".slot") != std::string::npos && entry.path().extension() == ".p2s")
                    ++count;
            }
            return count;
        }

        void drawTitleBlock(const Frame &f, float top)
        {
            using namespace ps2_ui;
            const Fonts &fn = fonts();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            text(dl, fn.mono, f.px(14.0f), f.at(80.0f, top), kLine, m_game.kicker);
            const float titleSize = f.px(92.0f);
            const ImVec2 t1 = f.at(80.0f, top + 18.0f);
            text(dl, fn.ui, titleSize, t1, kText, m_game.titleLine1);
            const float lineStep = textSize(fn.ui, titleSize, m_game.titleLine1).y * 0.86f;
            text(dl, fn.ui, titleSize, ImVec2(t1.x, t1.y + lineStep), kText, m_game.titleLine2);
            const float ry = t1.y + lineStep * 2.0f + f.px(14.0f);
            const float rs = f.px(19.0f);
            const ImVec2 rts = textSize(fn.ui, rs, "RECOMPILED");
            dl->AddLine(ImVec2(t1.x, ry + rts.y * 0.5f), ImVec2(t1.x + f.px(48.0f), ry + rts.y * 0.5f), kLine);
            text(dl, fn.ui, rs, ImVec2(t1.x + f.px(60.0f), ry), kLine, "RECOMPILED");
        }

        void drawBoot()
        {
            using namespace ps2_ui;
            const Frame f = frame();
            const Fonts &fn = fonts();
            const std::string topLeft = std::string("PS2RECOMP // ") + m_game.discLabel;
            drawTopBar(f, topLeft.c_str(), m_discReady ? "DISC READY" : "NO DISC CHECKED");
            drawTitleBlock(f, 108.0f);

            // Menu
            const bool focus = m_focusRequest;
            m_focusRequest = false;
            float y = 372.0f;
            if (menuItem(f, "START", f.at(60.0f, y), focus, true))
                start();
            y += 54.0f;
            if (menuItem(f, "SETTINGS", f.at(60.0f, y), false) && m_hooks.openSettings)
                m_hooks.openSettings();
            y += 54.0f;
            if (menuItem(f, "DISC", f.at(60.0f, y), false))
                showDisc(false);
            y += 54.0f;
            if (menuItem(f, "QUIT", f.at(60.0f, y), false))
                m_done = true;

            // System check panel: what the game will start with
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const PS2HostSettings &s = ps2Settings();
            const float px0 = 780.0f, px1 = 1200.0f, top = 130.0f;
            const float in0 = px0 + 26.0f, in1 = px1 - 26.0f;
            struct Row
            {
                const char *label;
                std::string value;
                bool ok;
            };
            const std::filesystem::path image = fromUtf8(s.discImage);
            const char *gpu = std::getenv("PS2X_GPU");
            const bool gpuOn = gpu && *gpu && *gpu != '0';
            if (GetTime() >= m_panelInfoAt) // folder scan and controller names twice a second, not every frame
            {
                m_panelInfoAt = GetTime() + 0.5;
                m_saveStates = countSaveStates();
                m_controllers = ps2InputControllerNames();
            }
            const int states = m_saveStates;
            const std::string &controllers = m_controllers;
            char renderer[64];
            if (s.upscale > 1)
                std::snprintf(renderer, sizeof(renderer), "%s \xC2\xB7 INTERNAL x%d", gpuOn ? "GPU" : "CPU", s.upscale);
            else
                std::snprintf(renderer, sizeof(renderer), "%s \xC2\xB7 NATIVE", gpuOn ? "GPU" : "CPU");
            const Row rows[] = {
                {"DISC", std::string(m_game.discLabel) + " \xC2\xB7 " + utf8(image.stem()), m_discReady},
                {"RENDERER", renderer, true},
                {"DISPLAY",
                 std::string(s.widescreen && m_game.widescreenOption ? "16:9" : "4:3") + " \xC2\xB7 " +
                     (s.progressive ? "PROGRESSIVE" : "INTERLACED"),
                 true},
                {"INPUT", controllers.empty() ? std::string("KEYBOARD") : controllers, true},
                {"SAVE STATES", states ? std::to_string(states) + " SAVED" : std::string("NONE YET"), states > 0},
            };
            const float rowStep = 30.0f;
            const float rowsTop = top + 74.0f;
            const char *note = "Settings changed here apply when the game starts.";
            const float noteSize = f.px(15.0f);
            const float noteTop = rowsTop + rowStep * 5.0f + 8.0f;
            const float bottom = noteTop + 24.0f + 22.0f;
            screenPanel(f, f.at(px0, top), f.at(px1, bottom));
            text(dl, fn.ui, f.px(28.0f), f.at(in0, top + 18.0f), kText, "SYSTEM CHECK");
            const char *status = m_discReady ? "ALL GREEN" : "CHECK DISC";
            const ImVec2 ss = textSize(fn.mono, f.px(12.0f), status);
            text(dl, fn.mono, f.px(12.0f), ImVec2(f.at(in1, 0.0f).x - ss.x, f.at(0.0f, top + 32.0f).y), m_discReady ? kLine : kAmber, status);
            dl->AddLine(f.at(in0, top + 58.0f), f.at(in1, top + 58.0f), kLineDim);
            for (int i = 0; i < 5; ++i)
            {
                const float ry = rowsTop + rowStep * static_cast<float>(i);
                const float size = f.px(17.0f);
                text(dl, fn.ui, size, f.at(in0, ry), kTextDim, rows[i].label);
                const std::string value = fitText(fn.ui, size, rows[i].value, f.px(in1 - 48.0f - (in0 + 118.0f)));
                text(dl, fn.ui, size, f.at(in0 + 118.0f, ry), kText, value.c_str());
                const char *mark = rows[i].ok ? "OK" : "--";
                const ImVec2 ms = textSize(fn.mono, f.px(13.0f), mark);
                text(dl, fn.mono, f.px(13.0f), ImVec2(f.at(in1, 0.0f).x - ms.x, f.at(0.0f, ry + 3.0f).y), rows[i].ok ? kLine : kTextDim, mark);
            }
            dl->AddText(fn.ui, noteSize, f.at(in0, noteTop), kTextDim, note, nullptr, f.px(in1 - in0));

            drawFooterLine(f, 56.0f);
            drawPrompts(f, 56.0f, {{"ENTER / A", "SELECT"}, {"ARROWS", "MOVE"}});
        }

        void drawDisc()
        {
            using namespace ps2_ui;
            const Frame f = frame();
            const Fonts &fn = fonts();
            drawTopBar(f, m_firstRun ? "FIRST RUN // DISC CHECK" : "DISC // CHECK", "DISC IMAGE");
            ImDrawList *dl = ImGui::GetWindowDrawList();

            // Left: what to pick
            text(dl, fn.mono, f.px(14.0f), f.at(80.0f, 96.0f), kLine, "BOOT SEQUENCE");
            const float titleSize = f.px(72.0f);
            const ImVec2 t1 = f.at(80.0f, 116.0f);
            text(dl, fn.ui, titleSize, t1, kText, "INSERT");
            const float lineStep = textSize(fn.ui, titleSize, "INSERT").y * 0.86f;
            text(dl, fn.ui, titleSize, ImVec2(t1.x, t1.y + lineStep), kText, "YOUR DISC");
            const float paraTop = t1.y + lineStep * 2.0f + f.px(18.0f);
            const float wrap = f.px(460.0f);
            dl->AddText(fn.ui, f.px(19.0f), ImVec2(t1.x, paraTop), IM_COL32(205, 239, 226, 255), m_game.discHelp, nullptr, wrap);
            const ImVec2 helpSize = fn.ui->CalcTextSizeA(f.px(19.0f), FLT_MAX, wrap, m_game.discHelp);
            dl->AddText(fn.ui, f.px(16.0f), ImVec2(t1.x, paraTop + helpSize.y + f.px(14.0f)), kTextDim,
                        "Accepted: .iso disc images. Nothing is sent anywhere.", nullptr, wrap);

            // Right: the check
            const float px0 = 640.0f, px1 = 1200.0f, top = 96.0f;
            const float in0 = px0 + 26.0f, in1 = px1 - 26.0f;
            text(dl, fn.mono, f.px(12.0f), f.at(in0, top + 22.0f), kTextDim, "DISC IMAGE");

            const bool focus = m_focusRequest;
            m_focusRequest = false;
            const float browseW = commandWidth(f, "BROWSE"); // the path box ends next to the button
            const ImVec2 boxA = f.at(in0, top + 42.0f);
            const ImVec2 boxB(f.at(in1, 0.0f).x - browseW - f.pxr(10.0f), boxA.y + f.pxr(44.0f));
            dl->AddRectFilled(boxA, boxB, kInk);
            dl->AddRect(boxA, boxB, kLineDim);
            const std::string shown = m_path.empty() ? std::string("No disc selected") : utf8(m_path);
            const std::string fitted = fitText(fn.mono, f.px(14.0f), shown, boxB.x - boxA.x - f.px(24.0f), true);
            const ImVec2 ps = textSize(fn.mono, f.px(14.0f), fitted.c_str());
            text(dl, fn.mono, f.px(14.0f), ImVec2(boxA.x + f.px(12.0f), boxA.y + (boxB.y - boxA.y - ps.y) * 0.5f),
                 m_path.empty() ? kTextDim : kText, fitted.c_str());
            const bool focusBrowse = focus && !m_verified;
            if (commandButton(f, "BROWSE", ImVec2(boxB.x + f.pxr(10.0f), boxA.y), false, focusBrowse, nullptr, kLine, !m_verified) &&
                !m_checking)
                m_browseRequested = true;

            // Steps
            const char *const kLabels[kStepCount] = {"Read disc image", nullptr, m_game.versionStepLabel, m_game.copyStepLabel,
                                                     "Create settings and save folders"};
            const std::string idLabel = std::string("Game ID ") + m_game.discLabel;
            float y = top + 112.0f;
            dl->AddLine(f.at(in0, y - 12.0f), f.at(in1, y - 12.0f), kLineDim);
            int done = 0;
            for (int i = 0; i < kStepCount; ++i)
            {
                const StepState st = m_steps[i];
                const bool running = m_checking && i == m_nextStep;
                const ImU32 color = st == StepState::Ok ? kLine : st == StepState::Failed ? kRed : running ? kText : IM_COL32(79, 143, 123, 255);
                if (st == StepState::Ok)
                    ++done;
                const float cy = f.at(0.0f, y + 12.0f).y;
                diamond(dl, ImVec2(f.at(in0 + 11.0f, 0.0f).x, cy), f.px(5.0f), st == StepState::Waiting ? 0 : color, color);
                const char *label = i == kStepGameId ? idLabel.c_str() : kLabels[i];
                const ImVec2 ls = textSize(fn.ui, f.px(18.0f), label);
                text(dl, fn.ui, f.px(18.0f), ImVec2(f.at(in0 + 34.0f, 0.0f).x, cy - ls.y * 0.5f), st == StepState::Waiting && !running ? kTextDim : kText,
                     label);
                const char *state = st == StepState::Ok ? "OK" : st == StepState::Failed ? "FAILED" : running ? "CHECKING" : "WAITING";
                const ImVec2 ss = textSize(fn.mono, f.px(13.0f), state);
                text(dl, fn.mono, f.px(13.0f), ImVec2(f.at(in1, 0.0f).x - ss.x, cy - ss.y * 0.5f), color, state);
                y += 32.0f;
            }

            // Progress
            const bool failed = !m_error.empty();
            const char *progressLabel = m_verified ? "DISC VERIFIED" : failed ? "CHECK FAILED" : m_checking ? "CHECKING" : "IDLE";
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%d%%", done * 100 / kStepCount);
            y += 6.0f;
            text(dl, fn.mono, f.px(12.0f), f.at(in0, y), failed ? kRed : kTextDim, progressLabel);
            const ImVec2 pcs = textSize(fn.mono, f.px(12.0f), pct);
            text(dl, fn.mono, f.px(12.0f), ImVec2(f.at(in1, 0.0f).x - pcs.x, f.at(0.0f, y).y), kTextDim, pct);
            y += 20.0f;
            const ImVec2 barA = f.at(in0, y), barB = f.at(in1, y + 6.0f);
            dl->AddRect(barA, barB, kLineDim);
            dl->AddRectFilled(barA, ImVec2(barA.x + (barB.x - barA.x) * static_cast<float>(done) / static_cast<float>(kStepCount), barB.y), failed ? kRed : kLine);
            y += 22.0f;

            const std::string &message = failed ? m_error : m_entryProblem;
            if (!message.empty())
            {
                const float msgWrap = f.px(in1 - in0);
                const ImVec2 ms = fn.ui->CalcTextSizeA(f.px(17.0f), FLT_MAX, msgWrap, message.c_str());
                dl->AddText(fn.ui, f.px(17.0f), f.at(in0, y), failed ? kRed : kAmber, message.c_str(), nullptr, msgWrap);
                y += ms.y / f.s + 10.0f;
            }
            screenPanel(f, f.at(px0, top), f.at(px1, y + 14.0f));

            // Footer: back or quit on the left, continue on the right
            drawFooterLine(f, 72.0f);
            const float fy = footerTop(f, 72.0f) + f.pxr(14.0f);
            const bool canGoBack = !m_firstRun && m_discReady;
            const bool back = canGoBack ? commandButton(f, "BACK", ImVec2(f.at(40.0f, 0.0f).x, fy), false, false, nullptr, kTextDim)
                                        : commandButton(f, "QUIT", ImVec2(f.at(40.0f, 0.0f).x, fy), false, false, nullptr, kTextDim);
            // Esc, or Circle / B (SDL controllers through the menu's pad feed; raylib's own gamepad as well)
            const bool cancel = !m_checking && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || m_padBackPressed ||
                                                IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            if (back || (cancel && canGoBack))
            {
                if (canGoBack)
                    showBoot();
                else
                    m_done = true;
                return;
            }
            if (m_verified)
            {
                const float cw = commandWidth(f, "CONTINUE");
                if (commandButton(f, "CONTINUE", ImVec2(f.at(1240.0f, 0.0f).x - cw, fy), true, focus, nullptr, kLine, true))
                    showBoot();
            }
            else
            {
                const char *hint = m_checking ? "Checking the disc..." : "Choose a disc image to continue";
                const ImVec2 hs = textSize(fn.ui, f.px(17.0f), hint);
                text(dl, fn.ui, f.px(17.0f), ImVec2(f.at(1240.0f, 0.0f).x - hs.x, fy + (f.px(44.0f) - hs.y) * 0.5f), kTextDim, hint);
            }
        }

        // PS2X_FRONTEND_TEST=1: unattended walk-through for window captures (pick PS2X_FRONTEND_PICK, continue, start),
        // a few seconds per screen. 2: also opens Settings on the boot screen for a while before starting.
        void testWalkThrough()
        {
            if (!m_test || m_checking)
                return;
            if (m_hooks.settingsOpen && m_hooks.settingsOpen())
            {
                if (m_testSettingsCloseAt > 0.0 && GetTime() > m_testSettingsCloseAt && m_hooks.closeSettings)
                {
                    m_testSettingsCloseAt = 0.0;
                    m_hooks.closeSettings();
                    m_screenSince = GetTime();
                }
                return;
            }
            if (GetTime() - m_screenSince < 4.0)
                return;
            if (m_screen == Screen::Disc)
            {
                if (m_verified)
                    showBoot();
                else if (m_error.empty())
                    m_browseRequested = true;
                return;
            }
            if (m_test >= 2 && !m_testSettingsShown && m_hooks.openSettings)
            {
                m_testSettingsShown = true;
                m_hooks.openSettings();
                m_testSettingsCloseAt = GetTime() + 5.0;
                return;
            }
            start();
        }

        PS2Runtime &m_runtime;
        PS2FrontEndHooks m_hooks;
        const PS2FrontEndGame &m_game;
        std::filesystem::path m_settingsDir;
        std::filesystem::path m_gameDir;
        std::filesystem::path m_stateDir;

        Screen m_screen = Screen::Boot;
        bool m_done = false;
        bool m_start = false;
        bool m_focusRequest = true;
        bool m_firstRun = true;
        bool m_discReady = false;
        double m_screenSince = 0.0;
        double m_panelInfoAt = 0.0;
        int m_saveStates = 0;
        std::string m_controllers;

        // Disc check
        std::filesystem::path m_path;
        std::array<StepState, kStepCount> m_steps{};
        bool m_checking = false;
        bool m_verified = false;
        int m_nextStep = 0;
        double m_stepAt = 0.0;
        std::string m_error;
        std::string m_entryProblem; // why the stored disc was not accepted at start
        bool m_browseRequested = false;
        bool m_startRequested = false;
        double m_startRequestedAt = 0.0;

        // Controller menu buttons this frame (PS2FrontEndHooks::feedControllerNav)
        bool m_padBackWasDown = false;
        bool m_padBackPressed = false;
        bool m_padConfirmDown = false;
        IsoImage m_iso;
        std::vector<uint8_t> m_elf;

        int m_test = 0;
        std::filesystem::path m_testPick;
        bool m_testSettingsShown = false;
        double m_testSettingsCloseAt = 0.0;
    };
}

void ps2RegisterFrontEnd(const PS2FrontEndGame &game)
{
    g_game = &game;
}

const PS2FrontEndGame *ps2FrontEndGame()
{
    return g_game;
}

bool ps2FrontEndWanted(int argc, char *argv[])
{
    if (!g_game)
        return false;
    if (const char *env = std::getenv("PS2X_FRONTEND"); env && *env)
        return *env != '0';
    return !(argc >= 2 && argv[1] && argv[1][0] != '\0');
}

void ps2FrontEndApplyDefaults()
{
    if (!g_game)
        return;
    for (const char *const *entry = g_game->defaultSwitches; entry && *entry; ++entry)
    {
        const std::string pair = *entry;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string name = pair.substr(0, eq);
        if (envSet(name.c_str()))
            continue;
        setEnv(name.c_str(), pair.substr(eq + 1));
        std::cout << "[frontend] default " << pair << std::endl;
    }
}

std::filesystem::path ps2FrontEndElfPath()
{
    return g_game ? ps2SettingsDirectory() / "game" / g_game->elfName : std::filesystem::path();
}

bool ps2RunFrontEnd(PS2Runtime &runtime, const PS2FrontEndHooks &hooks)
{
    if (!g_game)
        return false;
    FrontEnd frontEnd(runtime, hooks, *g_game);
    return frontEnd.run();
}

void ps2FrontEndConfigureRun()
{
    if (envSet("PS2X_CD_IMAGE"))
        return; // configureIoPathsFromElf took it
    const std::string &stored = ps2Settings().discImage;
    if (stored.empty())
        return;
    PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
    paths.cdImage = fromUtf8(stored);
    PS2Runtime::setIoPaths(paths);
    std::cout << "[io] CD image: " << stored << " (from the boot front-end)" << std::endl;
}
