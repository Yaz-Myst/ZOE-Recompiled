# SPU2 (PS2 sound processor)

ps2xIOP runs a game's own sound driver (an IRX module) on its IOP interpreter. Drivers such as
Sony's `rspu2 driver plus` program the SPU2 chip directly, so ps2xIOP emulates the chip.

## Where the code comes from

`pcsx2/SPU2/` is PCSX2's SPU2 core, taken from https://github.com/PCSX2/pcsx2 at commit
`1d102db` (folder `pcsx2/SPU2`). Its license is GPL-3.0+ (see each file's SPDX header), which is
compatible with PS2Recomp's GPLv3. Copyright: the PCSX2 Dev Team.

| File(s) | Status |
|---|---|
| `ADSR.cpp`, `Dma.cpp`, `Dma.h`, `Mixer.cpp`, `ReadInput.cpp`, `RegTable.cpp`, `Reverb.cpp`, `ReverbResample.cpp`, `defs.h`, `regs.h`, `interpolate_table.h`, `spu2.h`, `spu2sys.cpp`, `Debug.h` | Unmodified copies |
| `spu2.cpp`, `Debug.cpp`, `Wavedump_wav.cpp`, `spu2freeze.cpp` | Not used: host audio, logging and save states are replaced by `spu2_device.cpp` |

Everything PCSX2 normally supplies from outside `SPU2/` comes from small shim headers in `pcsx2/`
(`Config.h`, `R3000A.h`, `IopHw.h`, `IopDma.h`, `IopCounters.h`, `IopMem.h`, `SaveState.h`,
`common/*`, `GS/*`, `Host/*`), plus `PrecompiledHeader.h`, which is force-included the way PCSX2's
precompiled header is. `GS/GSVector.h` implements just the SSE operations the reverb resampler uses,
so reverb matches PCSX2's non-AVX build.

`spu2_device.cpp` is the glue (also GPL-3.0+). It replaces PCSX2's `SPU2/spu2.cpp` and the SPU2
parts of `IopDma.cpp`, `IopIrq.cpp` and `IopCounters.cpp`; functions carried over from those files
are marked `(PCSX2)`. `spu2_device.h` is the only interface the rest of ps2xIOP uses.

## How it's wired

- **Registers:** 16-bit reads and writes to `0x1F900000`–`0x1F90FFFF` go to `SPU2read`/`SPU2write` (`iop_memory.cpp`).
- **DMA:** writing IOP DMA channel 4/7 `CHCR` with the start bit calls `psxDmaGeneric`. The SPU2 clears
  the start bit and raises IRQ `0x24`/`0x28` when the transfer's time is up.
- **Clock:** the chip is clocked from the IOP cycle counter: one 48 kHz sample every 768 IOP cycles,
  like PCSX2's counter 6. `IopEmulator` calls `spu2::update()` whenever `nextEventCycle()` is reached.
- **Interrupts:** SPU2 interrupts (IRQ 9) and DMA completions are queued and dispatched through intrman
  between guest instructions.
- **Output:** each mixed stereo sample reaches `spu2Output()`, which feeds a lock-free queue.
  - The runtime's audio stream (raylib, `ps2_runtime.cpp`) plays it live through `ps2x::iop::spu2ReadOutput()`. It keeps about 50 ms queued, nudges the rate by up to 0.5% to follow clock differences, and plays silence while refilling after a stall. `PS2X_AUDIO=0` mutes.
  - `PS2X_SPU2_WAV=<path>` also records the output to a WAV file.
- **Timing:** the chip is only woken per-sample while a DMA is in flight, and otherwise catches up at the end of every IOP batch and on every register access.

## Debugging

- A `[spu2]` stats line is printed every 5 s of IOP time.
- F9 also writes `spu2ram_<tick>.bin` (sound RAM) and `.voices.txt` (every sound the game keyed on,
  with its pitch). `tools/spu2_extract.py` turns those into WAV files.
