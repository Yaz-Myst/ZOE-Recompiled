# Glossary (ZOE additions)

This glossary assumes the PS2 basics: EE, IOP, VU0/VU1, GS, SPU2, DMA, VIF/GIF, scratchpad,
overlay, soft-double. These are the words that came up on ZOE.

| Word | Meaning |
|---|---|
| **Stripped** | A program with all its function names removed. ZOE is stripped, so names had to be borrowed or worked out. |
| **Signature matching** | Recognising a known library function by its exact bytes. Used to name 838 of ZOE's Sony library functions against a reference PS2 ELF that ships with its symbols. |
| **Stage stub** | ZOE's small per-stage code file that registers the stage's callback functions. It loads at a different address each time, so a mini-interpreter runs it. |
| **Code resolver** | A new PS2Recomp hook: when the game jumps to code that wasn't translated in advance, a game-specific function can supply it. |
| **Library replacement / stand-in / HLE** | PS2Recomp's own C++ version of a Sony or C library function, used instead of the game's copy. Fast when right; five ZOE bugs came from ones that weren't. |
| **Recompile list** | A new translator option (`recompile = [...]`) that makes listed functions run the game's own code instead of a replacement. |
| **Calling convention / ABI** | The rules for where a function's inputs and outputs go (which registers, or the stack). On the EE, inputs 1–8 go in registers `a0`–`a3` and `t0`–`t3`; getting this wrong silently passes garbage. |
| **VCALLMS** | The main processor telling VU0 to run one of its small resident programs. ZOE uses these for matrix maths (a matrix stack lives in VU0 memory). |
| **Macro mode** | VU0 used directly by main-processor instructions (`vdiv`, `vmul`...), one at a time, instead of running a program. |
| **`rsqrt.s`** | A main-processor maths instruction: "a divided by the square root of b". PS2Recomp translated it wrongly; this broke ZOE's camera. |
| **±max ("FLT_MAX")** | The largest ordinary float. The PS2 has no infinity or "not a number": divide-by-zero and overflow give ±max instead. The PC must copy that behaviour. |
| **Interlaced FRAME mode** | A TV output mode where the game sends two half-height pictures offset by half a line. ZOE uses it; it caused the squashed and then shaking picture. |
| **Deinterlace (blend)** | Mixing each half-picture with the previous one so the half-line offset doesn't show as shaking. |
| **PSS** | Sony's movie file format: video, audio and optional data channels interleaved in 4 KB packs. |
| **DATA channel** | Extra data carried inside a movie. ZOE uses them for movie sound and for in-engine cutscene cameras. |
| **Demuxing** | Splitting a movie stream into its video, audio and data parts. |
| **"Refused" packet** | A movie packet a game's callback says it has no room for. Sony's library keeps it and offers it again; dropping it was a ZOE bug. |
| **JSIF** | Konami's own messaging layer between the main processor and the IOP (sound, disc, data pulls). |
| **SIF command + extra data** | A message to the other processor that can carry an attached block of memory; ZOE's sound driver gets its movie audio this way. |
| **PINE** | PCSX2's interface for other programs to read its memory while a game runs. Our main way to compare against the real PS2. |
| **Watchpoint** | A "stop when this memory address is written" trap, done here with the PC processor's debug registers. |
| **Lockstep verification** | Running two implementations on the same input and comparing every result. Used to prove the new VU engine matches the old one. |
| **vblank / tick** | One TV refresh (1/60 s). "vblanks per second" in the logs is the game's real speed: 60 = full speed. |
| **Stall** | A vector-unit instruction waiting because a value it needs isn't ready yet. The new engine still counts stalls exactly. |
| **Flags (MAC / status / clip)** | Condition bits the vector units set after maths (zero, sign, overflow...; clip = "outside the view"). They appear four cycles late, which the engines model. |
| **XGKICK** | The VU1 instruction that sends a finished list of triangles to the graphics chip. |
| **Texture hashing** | Fingerprinting a texture's pixels to recognise it again without re-decoding. It was a large cost in ZOE's battle. |
