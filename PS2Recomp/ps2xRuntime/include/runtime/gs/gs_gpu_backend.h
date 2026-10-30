#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2_state
{
    class Writer;
    class Reader;
}

// OpenGL rasterizer for the GS.
//
// Local memory stays on the CPU (an embedded GSCpuBackend handles transfers, CLUT loads and
// readbacks). Primitives submitted on the EE thread are converted to GPU vertices and batched
// by render state; textures are decoded from local memory when first used (and re-decoded when
// the memory backing them changes). Present() runs on the thread that owns the GL context: it
// replays the recorded batches into per-framebuffer render targets and blits the displayed one
// into the window texture registered with SetPresentTarget().
namespace ps2_state
{
    struct Access; // save states (src/lib/ps2_savestate.cpp)
}

class GSGpuBackend final : public GSRasterBackend
{
    friend struct ps2_state::Access;

public:
    GSGpuBackend();
    ~GSGpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Save states. LoadState also drops every cache derived from local memory (decoded textures, render-target page
    // ownership): after a load the CPU copy of local memory is the truth again.
    void SaveState(ps2_state::Writer &w) const;
    void LoadState(ps2_state::Reader &r);

    // GL-thread API (called from the window loop). `scale` is the internal resolution the target was sized for
    // (width x height = 640 x 512 times scale); render targets are drawn at that scale from the next present on.
    static void SetPresentTarget(uint32_t glTexture, uint32_t width, uint32_t height, uint32_t scale = 1);
    // Internal resolution asked for: PS2X_GPU_SCALE, else the Graphics menu setting (1, 2, 3 or 4).
    static uint32_t DesiredScale();
    static bool Active();
    static void GetPresentedSize(uint32_t &width, uint32_t &height);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};
