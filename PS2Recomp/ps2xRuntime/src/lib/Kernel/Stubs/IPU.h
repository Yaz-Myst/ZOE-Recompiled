#pragma once

#include "ps2_stubs.h"

namespace ps2_stubs
{
    void sceIpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuRestartDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuStopDMA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceIpuSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // Model of the IPU input (IPU_TO) DMA ring used by games that stream MPEG bitstream
    // through their own ring buffer. The HLE decoder does not pull data over DMA, so the
    // read position is derived from how much bitstream the presented pictures consumed.
    namespace ipu_ring
    {
        void noteInputChain(const uint8_t *rdram, uint32_t tadr);
        void addConsumedBytes(uint64_t bytes);
    }
}
