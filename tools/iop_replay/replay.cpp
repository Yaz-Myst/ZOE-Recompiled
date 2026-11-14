// Offline IOP replay: load the IOP section of a .p2s, run it alone for <seconds> of PS2 time, time it, and write the
// IOP state afterwards (for byte compares between interpreter modes).
// usage: replay <state.p2s> <seconds> [out.iops]      env: PS2X_IOP_AOT, PS2X_IOP_FAST_LOOP as in the game
// Build: build.bat <game build dir> <generated aot .cpp> <its register function>. Also writes replay.rip, a 1 ms
// sample of where the host time went (prof.py prints it by function). compare.sh runs the exactness check.
#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/state_io.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#define NOMINMAX
#include <windows.h>
using namespace ps2x::iop;
uint64_t ps2CurrentVsyncTick() { return 0; }
void IOP_AOT_ENTRY(); // the game's generated block table (tools/iop_aotgen.py)
struct Host final : IopHost
{
    std::vector<uint8_t> guest = std::vector<uint8_t>(32u << 20);
    bool readGuest(uint32_t a, void *d, size_t n) const override { a &= 0x1FFFFFFu; if (n > guest.size() - a) return false; std::memcpy(d, guest.data() + a, n); return true; }
    bool writeGuest(uint32_t a, const void *s, size_t n) override { a &= 0x1FFFFFFu; if (n > guest.size() - a) return false; std::memcpy(guest.data() + a, s, n); return true; }
    bool zeroGuest(uint32_t a, size_t n) override { a &= 0x1FFFFFFu; if (n > guest.size() - a) return false; std::memset(guest.data() + a, 0, n); return true; }
    bool normalizeGuestAddress(uint32_t a, uint32_t &o) const override { o = a & 0x1FFFFFFu; return true; }
    uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
    uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
    void freeGuest(uint32_t) override {}
    void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
    std::string hostPath(HostPathKind) const override { return {}; }
    std::string translateGuestPath(std::string_view p) const override { return std::string(p); }
    uint64_t openHostFile(std::string_view) override { return 0u; }
    bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
    bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
    void closeHostFile(uint64_t) override {}
    int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
    bool hasGuestFunction(uint32_t) const override { return false; }
    bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
    void log(LogLevel, std::string_view m) override { if (logs++ < 20) std::fprintf(stderr, "[log] %.*s\n", (int)m.size(), m.data()); }
    int logs = 0;
};
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint8_t> d(64u << 20);
    d.resize(std::fread(d.data(), 1, d.size(), f));
    std::fclose(f);
    size_t at = 0;
    static const uint8_t kTag[8] = {'I', 'O', 'P', 'S', 1, 0, 0, 0};
    for (; at + 8 < d.size() && std::memcmp(d.data() + at, kTag, 8) != 0; ++at) {}
    IOP_AOT_ENTRY();
    Host host;
    IopSubsystem iop(host);
    ps2_state::Reader r(d.data() + at, d.size() - at);
    iop.loadState(r);
    if (!r.ok()) { std::fprintf(stderr, "load failed: %s\n", r.error().c_str()); return 1; }
    const double seconds = std::atof(argv[2]);
    const uint64_t total = static_cast<uint64_t>(seconds * 294912000.0);
    // RIP sampler for the main thread (resolve with the .map): replay.rip gets one hex address per sample.
    HANDLE self; DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::atomic<bool> stop{false};
    std::vector<uint64_t> rips;
    std::thread sampler([&] {
        while (!stop) {
            Sleep(1);
            SuspendThread(self);
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(self, &c)) rips.push_back(c.Rip - reinterpret_cast<uint64_t>(GetModuleHandle(nullptr)));
            ResumeThread(self);
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t done = 0; done < total; done += 64u)
        iop.runEeCycles(64u); // the EE reports a few dozen cycles per checkpoint
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    stop = true; sampler.join();
    if (FILE *p = std::fopen("replay.rip", "w")) { for (uint64_t v : rips) { std::fprintf(p, "%llx", (unsigned long long)v); std::fputc(10, p); } std::fclose(p); }
    std::printf("%.1f s of PS2 time in %.1f ms host (%.3f ms per 60 Hz tick)", seconds, ms, ms / (seconds * 60.0)); std::putchar(10);
    if (argc > 3)
    {
        ps2_state::Writer w;
        iop.saveState(w);
        FILE *o = std::fopen(argv[3], "wb");
        std::fwrite(w.data().data(), 1, w.data().size(), o);
        std::fclose(o);
    }
}
