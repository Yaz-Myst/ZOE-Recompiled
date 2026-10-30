// PS2Recomp compatibility shim for PCSX2's IopDma.h: interrupts the SPU2 raises on the IOP.
#pragma once

void spu2DMA4Irq();
void spu2DMA7Irq();
void spu2Irq();
