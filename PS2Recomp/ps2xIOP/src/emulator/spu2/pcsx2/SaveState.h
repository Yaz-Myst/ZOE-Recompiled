// PS2Recomp compatibility shim for PCSX2's SaveState.h (save states are not supported).
#pragma once

#include "common/Pcsx2Types.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

struct freezeData
{
	int size;
	u8* data;
};
