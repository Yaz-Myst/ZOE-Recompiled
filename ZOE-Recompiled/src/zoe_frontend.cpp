// Boot front-end for Zone of the Enders (see ps2_frontend.h): a run of the exe without an ELF argument opens the
// title screen, and on first run the disc check that copies SLUS_201.48 and MODULES from the player's own disc image.
// play.bat (ELF argument) boots straight in as before; PS2X_FRONTEND=0 turns the front-end off.
#if defined(PS2X_ENABLE_MENU)
#include "ps2_frontend.h"

#include <cstdlib>
#include <cstring>

namespace
{
    // Folders the IOP loads modules from (cdrom0:\MODULES\*.IRX); ZOE.DAT stays on the disc image.
    const char *const kZoeCopyDirs[] = {"MODULES", nullptr};

    // The switches play.bat sets for the configuration the game is tested in: GPU renderer, render threads, frames
    // shown on the game's flips. Environment variables still win.
    const char *const kZoeDefaultSwitches[] = {"PS2X_GPU=1", "PS2X_GS_THREAD=2", "PS2X_PRESENT=flip", nullptr};

    PS2FrontEndGame zoeFrontEnd()
    {
        // PS2X_ZOE_WIDESCREEN=0 makes zoe_overrides.cpp install nothing and hide the option; match it.
        const char *wide = std::getenv("PS2X_ZOE_WIDESCREEN");
        const bool wideOffered = !(wide != nullptr && std::strcmp(wide, "0") == 0);
        return PS2FrontEndGame{
            "SLUS_201.48",
            "SLUS-201.48",
            "ORBITAL FRAME SYSTEM",
            "ZONE OF",
            "THE ENDERS",
            "Point to a dump of your own Zone of the Enders disc (USA, SLUS-201.48). The undub release works too. The "
            "game data is read from it each time you play.",
            "Version: retail or undub",
            "Copy game program and sound modules",
            // SLUS_201.48 as on the retail USA disc (VER 1.01) and the undub, which carries the same file:
            // 3,001,304 bytes, CRC-32 A0A48DD1 (checked on both images, 29 Oct 2026).
            3001304u,
            0xA0A48DD1u,
            kZoeCopyDirs,
            kZoeDefaultSwitches,
            wideOffered,
            "Zone of the Enders Recompiled | ver. 1.0",
        };
    }

    struct ZoeFrontEndRegistration
    {
        PS2FrontEndGame game = zoeFrontEnd();
        ZoeFrontEndRegistration() { ps2RegisterFrontEnd(game); }
    } g_zoeFrontEndRegistration;
}
#endif
