// PS2Recomp compatibility shim for PCSX2's common/Assertions.h (assertions compiled out, as in PCSX2 release builds).
#pragma once

#define pxAssert(cond) ((void)0)
#define pxAssertMsg(cond, msg) ((void)0)
#define pxAssume(cond) ((void)0)
#define pxFail(msg) ((void)0)
#define jNO_DEFAULT \
	default: \
	{ \
		break; \
	}
