ZOE-Recompiled — native Linux x86-64 build
==========================================

This folder contains ZOE-Recompiled, a native Linux build of the static recompilation of
Zone of the Enders (PS2, USA, SLUS_201.48).

You need your own copy of the game. No disc image or game assets are included. On first start
choose DISC -> BROWSE and select your own USA .iso (SLUS_201.48, 3,001,304 bytes,
CRC-32 A0A48DD1; the undub release works too).

Run:
    ./ZOE-Recompiled

The executable dynamically uses the system FFmpeg libraries (libavcodec, libavformat,
libavutil, libswresample, libswscale) and the desktop OpenGL/X11/audio stack. On
Debian/Ubuntu install:
    sudo apt-get install libavcodec58 libavformat58 libavutil56 libswresample3 libswscale5
On Arch/SteamOS install:
    sudo pacman -S ffmpeg

Settings, extracted game files, memory cards and save states live in
$XDG_DATA_HOME/PS2Recomp/SLUS-20148/ (default ~/.local/share/PS2Recomp/SLUS-20148/).

Default controls: Enter = Start, arrows/WASD = D-pad, X/Space = Cross, C = Circle,
Z = Square, V = Triangle, Q/E = L1/R1, Shift = L2/R2, Tab = Select. Esc opens the menu.
F5/F8 save/load state, F6/F7 change slot. SDL2 controllers are picked up live.

For the portable AppImage see the release page. Full documentation:
https://github.com/omegakatana92/ZOE-Recompiled/blob/steamdeck-linux-port/docs/steamdeck-build.md

Licensed under GPL-3.0; see LICENSE. Zone of the Enders is (c) Konami. This project is not
affiliated with or endorsed by Konami.
