// PS2Recomp compatibility shim for PCSX2's GS/MultiISA.h. PCSX2 compiles some code once per CPU
// feature level and picks one at runtime; we build a single baseline (SSE4.1) copy in isa_native.
#pragma once

#include "common/Pcsx2Types.h"

#define MULTI_ISA_COMPILE_ONCE 1
#define MULTI_ISA_DEF(...) namespace isa_native { __VA_ARGS__ }
#define MULTI_ISA_UNSHARED_START namespace isa_native {
#define MULTI_ISA_UNSHARED_END }
