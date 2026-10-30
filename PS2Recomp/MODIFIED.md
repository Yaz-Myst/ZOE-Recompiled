# Modified copy of PS2Recomp

This folder is a modified copy of [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors, taken
at upstream commit `e563698` ("GIF: treat FLG=3 like IMAGE everywhere a GIFtag is parsed", #384) and changed by Yz
for Zone of the Enders, last changed 29 October 2026. It is distributed under the same licence, the GNU General
Public License v3 ([LICENSE](LICENSE)); the original copyright notices are kept in every file that had them.

What changed: translator fixes (64-bit branch tests, VU load/store, overlays, recompile lists), an OpenGL GS renderer with
render threads, a fast VU1 engine with ahead-of-time compilation, save states, a boot front-end that takes the
player's own disc image, a redesigned in-game menu, SDL2 controllers with pressure buttons and two pad ports, MPEG
streaming fixes, and the PCSX2 SPU2 core for sound (GPL-3.0+, see `ps2xIOP/src/emulator/spu2/README.md`).
