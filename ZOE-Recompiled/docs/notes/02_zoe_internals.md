# ZOE internals learned so far

Technical reference. Addresses are EE (main processor) addresses in SLUS_201.48 unless noted.
Heap addresses were stable across our runs and matched PCSX2's layout.

## Program layout

- One ELF segment: `0x00200000`–`0x004E85DC`. C++ (gcc 2 typeinfo names like `DG_*`, `EV_JOB`,
  `EFF_FIELD`), Sony SDK 2000 generation, stripped.
- ~37,386 functions after `CreateMissingFunctions.java sweep` (5,811 `jr ra` all covered). Names:
  838 from `tools\sigmatch.py` (signatures from a reference PS2 ELF with symbols) plus `ApplyZoeNames.java`.
- `ZOE.DAT` (ISO LBA 300 / 0x12C): archive of stage folders with 32-byte entries (`.pss` movies,
  `.pak`, `.sdx`, `.scx`, `.mts`, and a ~3 KB stage `.bin`).

## Stage files

- Each stage `.bin` is straight-line MIPS code: `FUN_0043c8e8` then N × `FUN_0043bb90(hash, fn)`
  (registers callback hash → function in the main ELF). 22 files, 3,295 registrations,
  351 distinct targets, all already translated.
- Loaded into heap kind 2 `[0x900000, 0x1700000)` at a variable address; `FUN_0043c8d8` stores the
  buffer at `*0x49DE50`; `FUN_0043c890` calls it with `jalr` at `0x43C8B0`.
- Runtime: code-resolver hook returns the native stub interpreter (`zoe_overrides.cpp`) when the
  address equals `*0x49DE50` or a resume point inside it; strict instruction whitelist.
  Log line: `[zoe] stage stub @0x........ registered N callbacks` (title 131, init 70, first
  stage 161).

## IOP modules

LIBSD, SIO2MAN, PADMAN, MCMAN, MCSERV, **JSIFMAN** (Konami SIF manager), **CDBIOS** (disc),
NOUSBFS, **IOP_MAIN** (Konami sound driver), plus an IOPRP20.IMG reboot. All run emulated (real IOP
code).

- JSIF command ids: 0 JSIFMAN (data pulls), 1 IOP_MAIN, 3 status "ping" to EE, 4 CDBIOS.
- EE handlers: cid 0 → `0x467730` (arg `0x4dfec0`), cid 3 → `0x466fd0` (arg `0x204dfd40`).
- EE ↔ IOP status block: EE `0x4DFCC0` (uncached `0x204DFCC0`), 32 bytes DMA'd by the IOP every
  5 ms from IOP `0x6adcc`. `+4` busy flags (EE sets bit 31 on send, 0x1000 for `0xF1` commands,
  0x2000 for `0xF3`), `+0xC` bytes played, `+0x14` IOP time.
- Sound commands: `FUN_00466f18(cmd)` → `FUN_004674c0` → `isceSifSendCmd` (`0x48B718`).
  Data pulls: the IOP sends cid 0 `{size, 0, cid, 1, reqId, iopDst, eeSrc, bytes}`; the EE handler
  answers with `isceSifSendCmd(cid, pkt, 0x10, eeSrc, $t0=iopDst, $t1=bytes)`.

## Movies (PSS)

- 4 KB packs. Video stream `0xE0`; `private_stream_1` sub-stream `0xFF` = DATA with a 4-byte header
  `ff 90 00 <channel>`.
- Callbacks: M2V str cb type 0 → `0x4354e8` (data `0x6064e0`); DATA ch 1 and ch 7 → `0x437c88` →
  `FUN_00439058` (data `0x5f6410` and `0x636c50`); plain callbacks types 1, 2, 3, 5.
- `FUN_00439058` turns the data pointer into an **offset inside the game's PSS input ring**
  (`func_434F78`, ring object at `*(cb+0x1C)`), so the pointer must point into that ring.
- Intro (`title\ttlr1001.pss`): audio on **DATA channel 1**. Cutscenes: camera/motion records on a
  DATA channel, voice on another.
- Ring object layout (`FUN_00434bc8` init, constructor `FUN_00438ee0(obj, size, mirror, a3)`):
  `+0` base, `+4` size, `+8` position, `+0xC` bytes used, `+0x10`/`+0x14` counters, `+0x18` allocation,
  `+0x1C` source-ring object, `+0x20` mirror size, `+0x24` state (audio manager), `+0x34` channels,
  `+0x38` total bytes, `+0x3C` sent, `+0x40` acknowledged.
- Audio stream manager: `FUN_00437710` switch on `+0x24`. State 0 parses a 2 KB header (sends
  `0xF2…`, `0xF4…`); state 1 sends the first 16 KB (`0xF3 | addr>>4`); state 5 starts (`0xFF000010`);
  state 6 sends the next 16 KB when `sent - played < 0x4800` and the status block isn't busy.
- IOP side: core 1 voices 22/23 at pitch `0xEB4` (44.1 kHz), buffers at SPU `0x7020/0x7820` and
  `0x8020/0x8820`, refilled with `sceSdVoiceTrans` from IOP `0x15e850..0x16684f` (two 16 KB halves).
  Polls voice 22 ENVX/NAX every 5 ms.

## Camera

- Renderer camera pointer: `*0x4D8B14` = `0x70000000` (scratchpad).
- Camera object (cutscene example at `0x61AB50`): `+0x00` world matrix, `+0x40` inverse,
  `+0x80`/`+0xC0` screen matrices, `+0x100`/`+0x140` combined (built by `FUN_00450F58`),
  `+0x180` pending matrix, `+0x224..0x260` parameters (`+0x244` FOV, `+0x248` pending FOV),
  `+0x26A` "pending" flag, `+0x270` keyframe track.
- `FUN_00450e50` init; `FUN_00451978` applies pending matrix/FOV; `FUN_004514e0` = look-at
  (uses `rsqrt.s`); `FUN_00201f30` copies a matrix into `+0x180`.
- Cutscene keyframes: track header (`+0` stride-0x10, `+6` index, `+0x14` data) with a ring of 4
  slots; `FUN_0043e1e8` interpolates; `FUN_0043d350` parses DATA records `{u16 track, …}`
  (track table at `*0x49DE54`); `FUN_0043d5a0` copies a record into its slot.

## VU0 micro programs (resident at VU0 `0x800`–`0xFFF`)

Byte-identical to PCSX2. A matrix stack lives in VU0 data memory with `vi14` as the stack pointer:

| Address | Does |
|---|---|
| `0x800` | vf28..31 = vf28..31 × vf4..7 |
| `0x880` | vf4 = M(vf28..31) × vf4 |
| `0x8A0` | vf4 = M(vf16..19) × vf4 |
| `0x8C0` | push vf28..31 |
| `0x8E0` | pop vf28..31 |
| `0x900` | vi14 = 0x100 (reset stack) |
| `0x910` | vf24..27 = M(vf16..19) × vf28..31 |
| `0x990` | vf24..27 = M(vf20..23) × vf28..31 |

The object renderer `FUN_0044f7e0` uses them and writes the packet matrix with `FUN_0045a8d0`
(`qmfc2.i vf24; sqc2 vf25..27`). Camera load: `FUN_00451a48` (vf16..19 ← cam+0x100,
vf20..23 ← cam+0x140).

## VU1

- Around ten programs. Stage renderers resume at `0x1ed8` (world, clip-culled), `0x1cc8` (HUD/2D),
  and many more (`0x100b8`, `0x102f0`, `0x11278`, `0x133b0`, …).
- Scheme: UNPACK header (q0.x = renderer address / 8 into vi15), `MSCAL 0xa8`, `MSCAL 0x0` (load
  constants, `ilw vi15`, `jr vi15` with the E-bit → the resume point is the renderer), then per
  batch UNPACK + `MSCNT`.
- VIF1 BASE 0x40, OFFSET 0x1e0 (PCSX2 truth), TOP alternates 0x40/0x220.
- Workload: about 1.5 million VU1 cycles per frame in the first cutscene; ~26,000 programs per
  second at 5 fps.

## Display

- Libgraph sets SMODE2=3 (interlaced FRAME), DISPLAY DH=445 MAGH=4, two 512×224 buffers
  (FBP 0 / 0x38), PMODE 0x66, circuit 1 DBY=1. `sceGsSetHalfOffset` alternates XYOFFSET.OFY
  between 1936.5 and 1936.

## Recompile list (in `work\SLUS_201.48.toml`)

All 29 `libvu0` functions (`sceVpu0Reset`, `sceVu0*`), `_sceVu0ecossin` / `sceVu0ecossin`,
`sin`, `cos`, `fabs`, `floor`, `sceGsResetGraph`, `sceGsGetGParam`, `sceGsSetDefLoadImage`,
`sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2`, `sceGsSetDefClear`, `sceGsSetDefDispEnv`,
`sceGszbufaddr`, `sceVif1PkOpenDirectCode`, `sceVif1PkOpenGifTag`, `sceVif1PkCloseGifTag`,
`sceVif1PkAlign`, `sceVif1PkAddGsAD`, `sceVif1PkSize`, `sceVif1PkOpenDirectHLCode`.

## Pad pressure

- Pad loop `0x4617A0` (called for 2 ports by `0x461C20`). Per-port struct at `0x4AA3C0`, stride `0x140`: stored pad mode at `+0x125`; pressure bytes at `+0x104..+0x10F` (pad data bytes 8..19); buttons at `+0x110` / `+0x114`.
- Mode `0x79` copies the pressure bytes. Any other mode synthesises 0xFF or 0 from the button table at `0x4AA380` (order 0x20, 0x80, 0x10, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800, 0x100, 0x200).
- The init sequence calls `scePadInfoPressMode` and `scePadEnterPressMode` (`0x47ABE0`).
- `FUN_00238b70` copies 8 pressure values into the player object (pointer at `0x48EE94`; `0x630F20` in battle1_t4740): `+0x1064` Triangle, `+0x1065` Cross, `+0x1066` R2, `+0x1067` Square, `+0x1068` Circle, `+0x1069` R1, `+0x106A` L1, `+0x106B` (mask 0x1 slot, L2 byte). Each copy is gated by a bit of the global at `0x48EEFC`.
- Only `+0x1067` (Square) and `+0x1068` (Circle) are read back, by attack states `0x2D0F98`, `0x2D11E0`, `0x2D1378`, `0x2D1570`. They save the pressure into player `+0xBE0`. Flag `+0xC9C` selects: 0 -> Circle pressure and the ammo counter at `0x48EF2E` / `0x48EF30` / `0x48EF32` is decremented; nonzero -> Square pressure. They start only with ammo > 0, so these are sub-weapon attacks.
- Use of the saved pressure p (0..255): `0x2DCC98` (when `+0xC9C` == 0) sets a projectile value = base + (p/128) x range (full press ~ base + 2 x range; 40% ~ base + 0.8 x range). `0x2DF878` computes 3 + 2p/256 (3.0 .. 5.0). Which sub-weapons, and what the values mean (distance, speed, count), is not measured yet.
- Not read: Square's normal shot, melee and homing lasers (no read of the pressure bytes found). Triangle, Cross, R1, R2 and L1 pressure is copied but never read.
- Measured in battle1 (no sub-weapon, HUD "NONE"): Square and Circle taps, full or light, leave `+0xBE0` at 0. Only `+0xBE1` (a combo counter, 0..3) changed on Square.
- Outside sources: no web guide found documents ZOE's pressure use. A Steam user report about the HD port says light vs hard Square gave different shots (unverified; the code search found no pressure read for the normal Square shot).

## Versus mode unlock (29 Oct)

- Unlock words (game code `0x466550..0x466668`): `0x4DFC90` saved flags (`FUN_00466578` ORs in), `0x4DFC94` clear count (cap 999), `0x4DFC98` second counter (cap 9999, 272 in the user's save), `0x4DFC9C` session flags (`FUN_00466590` ORs in). `FUN_004665a8` returns `0x4DFC90 | 0x4DFC9C`; InitSys (`FUN_00466550`) zeroes all four at boot.
- Bit 0 = Versus in the title menu (`FUN_00414c88` sets the entry count `+0x5C14` to 4 instead of 3). Bit 1 = full roster: `FUN_004290e8` uses frame mask 0x7F / stage mask 0x7FF instead of 0x1F / 0x1FF, then ORs 0x80 / 0x800 (the "?" random slot). The ending (`FUN_0042ce08`) and `FUN_00414088` set bit 0 at 1 clear and bit 1 at 2 clears.
- Saves: `FUN_00462328` writes `FUN_004665a8()` (saved | session), the clear count and the counter into the save block (magic 0x10004); `FUN_004622b0` reads them back through `FUN_00466630`. So session bits get stored by the next save (from the code, not tested).
- Built-in cheat: table `0x49DCB0`, pointers at `0x49DD00`, reader object pointer at `0x49DD14` (task `0x42E700` reads the pressed word `0x4AA4D6`, Select/Start masked off). Codes 1=Up 2=Down 3=Left 4=Right 5=Circle 6=Cross 7=Triangle 8=Square 9=L1 A=L2 B=L3 C=R1 D=R2 E=R3. Code 0: Circle Cross Right Left Right Left Down Down Up Up, each press within 60 frames of the last. The title states 2 (logo fade-in) and 4 (PRESS START) poll it (`0x414f28`, `0x414fdc`); on success the game plays a sound and calls `FUN_00466590(3)` (both bits). Codes 1-4 (Up Up Down Down Left Right Left Right R3 L3 + L1/L2/R1/R2) are never checked: both callers pass index 0.
- Menu option (29 Oct): Esc > EXTRAS > Unlock Versus mode applies the same two writes (session word |= 3, title count 3 -> 4) before each call of `FUN_00414c88` (`zoeTitleStateVersus`; `PS2X_ZOE_VERSUS_UNLOCK=1` on for one run, `0` off and hidden).
- Checked 29 Oct (route `work/route_versus_code.txt`, state `work/states/test_versus_code_t2640.p2s` = title after the code): session word 3, menu NEW GAME / LOAD GAME / VERSUS / OPTIONS, Versus screen 7 frames + "?" and 11 stages + "?" (the user's 1-clear `ws_versus.p2s`: 5 + 2 empty and 9 + 2 empty). Scripted presses of 0.1 s were dropped or cut short (the input record showed a missing Cross and a 0.01 s Down); 0.25 s holds 0.5 s apart worked.

## Frame objects (character swap scoping, 29 Oct)

- One constructor builds every orbital frame: `FUN_00235b88` (frame index in a3, 0 Jehuty, 1 Raptor, 2 Mummy, 3 Cyclops, 4 Neith1, 5 Neith2). It picks the model from `0x491460` (`jehuty.mdl`...) or `0x491478` (`raptor_st.mdl`, `mummy_st.mdl`, `cyclops_st.mdl`; branch at `0x2365b0`, condition not read) and the motion set from `0x491490`. It sets the player pointer `0x48EE94` at `0x2362b8` when `0x48EE90` == 0 and `+0xC94` != 0, independent of the frame index.
- Callers: story script command `FUN_002359c0` (script args 0..10, frame index = arg 5), story enemy spawns from stage records `FUN_0021cdd0` -> `FUN_00235910` (record +5 = frame type, controller 2), Versus `FUN_00441858` (frame = byte `0x49DE90 + player`, variant byte `0x49DEA0 + player`, player 1 / 2), and `FUN_004419c0` (from `FUN_0043f050`).
- Stage load lists at `0x48F420` (pak, motion sets, bin, scx per stage): every story stage loads `jehuty.mts`, plus the `.mts` of the enemies it uses (e.g. fa2: jehuty, mummy, raptor, cyclops, neith1). `.mts` files are 0.6-2 KB; models are inside the paks.
- Versus loads separate `<stage>_vs.pak` files (1-3 MB larger than the story pak in most stages; fb1, gl1, ra1, rb1, sh1, ta1, vr1 have none). Neith has her own paks (`neith1.pak` 1.25 MB, `neith2.pak` 1.30 MB) in most stage folders.
- Zero-filled RAM above `0x500000` (rough headroom, not a heap measurement): battle1 10.1 MB, hub bomb 5.3 MB, Viola 2.6 MB, Tempest fight 0.3 MB, Versus screen 6.4 MB.
