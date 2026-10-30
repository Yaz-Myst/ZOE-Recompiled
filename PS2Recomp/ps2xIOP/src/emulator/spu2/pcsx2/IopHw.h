// PS2Recomp compatibility shim for PCSX2's IopHw.h: the IOP DMA channel 4/7 registers and PS1-mode
// SPU registers the SPU2 code touches. They live in ps2xIOP's IopMemory hardware map.
#pragma once

#include "common/Pcsx2Types.h"

u32& spu2HwReg32(u32 address);

#define HW_DMA4_MADR (spu2HwReg32(0x1F8010C0u))
#define HW_DMA4_BCR (spu2HwReg32(0x1F8010C4u))
#define HW_DMA4_CHCR (spu2HwReg32(0x1F8010C8u))
#define HW_DMA4_TADR (spu2HwReg32(0x1F8010CCu))
#define HW_DMA7_MADR (spu2HwReg32(0x1F801500u))
#define HW_DMA7_BCR (spu2HwReg32(0x1F801504u))
#define HW_DMA7_CHCR (spu2HwReg32(0x1F801508u))
#define HW_DMA7_TADR (spu2HwReg32(0x1F80150Cu))

extern u16 psxSoundDataTransferControl;
extern u16 psxSPUSTAT;
extern u16 psxSPUCNT;
