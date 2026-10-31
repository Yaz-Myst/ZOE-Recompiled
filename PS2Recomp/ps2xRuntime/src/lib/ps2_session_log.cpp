#include "ps2_session_log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <intrin.h>
#include <io.h>
#endif

namespace
{
    constexpr long long kMaxLogBytes = 64ll * 1024 * 1024;

    // Moves <name> to <older> (replacing it), so one earlier session survives a restart after a crash.
    void keepPrevious(const std::filesystem::path &name, const std::filesystem::path &older)
    {
        std::error_code ec;
        if (std::filesystem::exists(name, ec))
            std::filesystem::rename(name, older, ec);
    }

#if defined(_WIN32)
    // A file (play.bat, battle_run.ps1) or a console (started from a terminal) already shows or keeps the output.
    // A pipe does not count (a launcher that never reads it would block the game once it fills), nor does NUL (a
    // character device like a console, but GetConsoleMode fails on it).
    bool outputAlreadyKept()
    {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            return false;
        DWORD consoleMode = 0;
        const DWORD type = GetFileType(handle);
        return type == FILE_TYPE_DISK || (type == FILE_TYPE_CHAR && GetConsoleMode(handle, &consoleMode));
    }

    std::string cpuName()
    {
#if !defined(_M_X64) && !defined(_M_IX86)
        return "unknown";
#else
        int regs[4]{};
        __cpuid(regs, 0x80000000);
        if (static_cast<unsigned>(regs[0]) < 0x80000004u)
            return "unknown";
        char brand[49]{};
        for (int i = 0; i < 3; ++i)
        {
            __cpuid(regs, 0x80000002 + i);
            std::memcpy(brand + i * 16, regs, sizeof(regs));
        }
        std::string name(brand);
        const size_t first = name.find_first_not_of(' ');
        const size_t last = name.find_last_not_of(' ');
        return first == std::string::npos ? std::string("unknown") : name.substr(first, last - first + 1);
#endif
    }

    long long fileSize(FILE *stream)
    {
        const int fd = _fileno(stream);
        if (fd < 0)
            return 0;
        LARGE_INTEGER size{};
        const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        return handle != INVALID_HANDLE_VALUE && GetFileSizeEx(handle, &size) ? size.QuadPart : 0;
    }

    // Flushes stdout once a second (a crash loses at most that much; stderr is unbuffered) and stops both files at
    // kMaxLogBytes together.
    void watchLogs()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::fflush(stdout);
            if (fileSize(stdout) + fileSize(stderr) < kMaxLogBytes)
                continue;
            std::printf("[log] stopped at %lld MB; later output is dropped\n", kMaxLogBytes >> 20);
            std::fflush(stdout);
            const int nul = _open("NUL", _O_WRONLY);
            if (nul >= 0)
            {
                _dup2(nul, _fileno(stdout));
                _dup2(nul, _fileno(stderr));
                _close(nul);
            }
            return;
        }
    }
#endif
}

void ps2StartSessionLog(const std::filesystem::path &directory, bool frameLog)
{
    if (directory.empty())
        return;
    const char *mode = std::getenv("PS2X_SESSION_LOG");
    const bool off = mode != nullptr && std::strcmp(mode, "0") == 0;
    const bool forced = mode != nullptr && std::strcmp(mode, "1") == 0;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    if (frameLog && std::getenv("PS2X_FRAME_LOG") == nullptr)
    {
        const std::filesystem::path csv = directory / "frames.csv";
        keepPrevious(csv, directory / "frames_previous.csv");
#if defined(_WIN32)
        _wputenv_s(L"PS2X_FRAME_LOG", csv.wstring().c_str());
#else
        setenv("PS2X_FRAME_LOG", csv.string().c_str(), 1);
#endif
    }

#if defined(_WIN32)
    if (off || (!forced && outputAlreadyKept()))
        return;
    // Two files, as play.bat keeps them: threads print line pieces, and one shared file mixed stdout and stderr
    // pieces mid-line.
    const std::filesystem::path out = directory / "last.log";
    const std::filesystem::path err = directory / "last.err.log";
    keepPrevious(out, directory / "previous.log");
    keepPrevious(err, directory / "previous.err.log");
    // _wfreopen closes the old stream before it opens the file, and a closed stdout/stderr ends a release exe at the
    // next print. So both files are opened once first (folder not writable, disk full, file held open: no log, the
    // game runs as before), and a reopen that still fails falls back to NUL.
    FILE *probeOut = _wfopen(out.wstring().c_str(), L"w");
    FILE *probeErr = _wfopen(err.wstring().c_str(), L"w");
    const bool writable = probeOut != nullptr && probeErr != nullptr;
    if (probeOut != nullptr)
        std::fclose(probeOut);
    if (probeErr != nullptr)
        std::fclose(probeErr);
    if (!writable)
        return;
    if (_wfreopen(out.wstring().c_str(), L"w", stdout) == nullptr)
        _wfreopen(L"NUL", L"w", stdout);
    if (_wfreopen(err.wstring().c_str(), L"w", stderr) == nullptr)
        _wfreopen(L"NUL", L"w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // Started from Explorer there are no output handles: the first std::cout line ("Using argv boot path", before the
    // settings load) failed and left the stream in a fail state that drops every later line (seen 30 Oct: no [pace],
    // [state] or [frame:dump] in the log, printf lines only).
    std::cout.clear();
    std::cerr.clear();
    std::clog.clear();
    const std::time_t now = std::time(nullptr);
    char started[32]{};
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::printf("[log] session %s, CPU %s, %u threads\n", started, cpuName().c_str(), std::thread::hardware_concurrency());
    std::thread(watchLogs).detach();
#else
    (void)off;
    (void)forced;
#endif
}
