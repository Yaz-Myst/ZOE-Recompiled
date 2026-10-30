#include "ps2_runtime.h"
#include "games_database.h"
#include "ps2_host_settings.h"
#if defined(PS2X_ENABLE_MENU)
#include "ps2_frontend.h"
#include "ps2_menu.h"
#include "rlImGui.h"
#endif
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();

    try
    {
#if defined(PS2X_ENABLE_MENU)
        // Boot front-end (games that register one, run without an ELF argument): the game program comes from the
        // player's disc image, checked and copied on first run; see ps2_frontend.h.
        const bool frontEnd = ps2FrontEndWanted(argc, argv);
        std::filesystem::path pathObj = frontEnd ? std::filesystem::path(ps2FrontEndGame()->elfName) : getExecutablePath(argc, argv);
#else
        std::filesystem::path pathObj = getExecutablePath(argc, argv);
#endif

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }
#if defined(PS2X_ENABLE_MENU)
        // A game module that registered a front-end may name its own window (also for runs with the ELF argument).
        if (const PS2FrontEndGame *game = ps2FrontEndGame(); game != nullptr && game->windowTitle != nullptr &&
                                                             elfName == game->elfName)
        {
            windowTitle = game->windowTitle;
        }
#endif

        ps2LoadSettings(normalizedId);
#if defined(PS2X_ENABLE_MENU)
        if (frontEnd)
        {
            filePathStr = ps2FrontEndElfPath().string();
            ps2FrontEndApplyDefaults();
        }
#endif

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_MENU)
        // The in-game menu (Esc) and the developer debugger (F1) share one ImGui context. The hook
        // keeps the rlImGui dependency out of the runtime library.
        struct HostUi
        {
            PS2Menu menu;
#if defined(PS2X_ENABLE_DEBUG_UI)
            PS2DebugPanel debugPanel;
#endif
        } hostUi;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                auto *ui = static_cast<HostUi *>(userData);
                rlImGuiSetup(true);
                ui->menu.initialize();
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.initialize();
#endif
            },
            [](PS2Runtime &rt, void *userData)
            {
                auto *ui = static_cast<HostUi *>(userData);
                ui->menu.update(rt);
                bool debugVisible = false;
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.handleHotkeys();
                debugVisible = ui->debugPanel.isVisible();
#endif
                if (!ui->menu.needsImGuiFrame() && !debugVisible)
                    return;
                rlImGuiBegin();
                ui->menu.draw(rt);
#if defined(PS2X_ENABLE_DEBUG_UI)
                ui->debugPanel.draw(rt);
#endif
                rlImGuiEnd();
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                ps2SaveSettings();
#if defined(PS2X_ENABLE_DEBUG_UI)
                static_cast<HostUi *>(userData)->debugPanel.shutdown();
#endif
                rlImGuiShutdown();
            },
            &hostUi);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

#if defined(PS2X_ENABLE_MENU)
        if (frontEnd)
        {
            PS2Menu &menu = hostUi.menu;
            const PS2FrontEndHooks hooks{
                [&menu]() { menu.openSettings(); },
                [&menu]() { menu.closeSettings(); },
                [&menu]() { return menu.isOpen(); },
                [&menu, &runtime]() { menu.update(runtime); },
                [&menu, &runtime]() { menu.draw(runtime); },
                [&menu]() { return menu.feedControllerNav(); },
            };
            if (!ps2RunFrontEnd(runtime, hooks))
            {
                std::cout << "[frontend] quit" << std::endl;
                std::cout.flush();
                std::cerr.flush();
                std::_Exit(0);
            }
        }
#endif

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }
#if defined(PS2X_ENABLE_MENU)
        if (frontEnd)
            ps2FrontEndConfigureRun();
#endif

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
