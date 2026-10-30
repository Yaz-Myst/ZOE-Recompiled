// PS2Recomp compatibility shim for PCSX2's R3000A.h: the IOP cycle counter the SPU2 is clocked from.
#pragma once

#include "common/Pcsx2Types.h"

struct Spu2PsxRegsView
{
	u32 cycle;
};

// Current IOP cycle (low 32 bits, like PCSX2's psxRegs.cycle), supplied by spu2_device.cpp.
Spu2PsxRegsView spu2PsxRegs();
#define psxRegs (spu2PsxRegs())
