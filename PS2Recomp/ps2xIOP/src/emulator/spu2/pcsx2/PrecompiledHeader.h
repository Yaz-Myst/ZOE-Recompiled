// PS2Recomp compatibility shim: what PCSX2's precompiled header makes visible to every SPU2 source.
// Force-included for the ps2_iop_spu2 library (see ps2xIOP/CMakeLists.txt).
#pragma once

#include "common/Pcsx2Types.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "IopMem.h"

#include <algorithm>
#include <array>
#include <cstring>
