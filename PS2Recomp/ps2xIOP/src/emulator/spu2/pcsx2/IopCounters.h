// PS2Recomp compatibility shim for PCSX2's IopCounters.h. Only counter 6 (the SPU2 event counter)
// is used: CounterUpdate() writes the cycle the SPU2 next needs servicing, and spu2_device.cpp
// reports it to the IOP scheduler.
#pragma once

#include "common/Pcsx2Types.h"

struct psxCounter
{
	u32 startCycle;
	s32 deltaCycles;
};

extern psxCounter psxCounters[8];
extern s32 psxNextDeltaCounter;
extern u32 psxNextStartCounter;
