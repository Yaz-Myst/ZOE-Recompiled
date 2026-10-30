#include "ps2_frame_log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ps2_frame_log
{
    Counters &counters()
    {
        static Counters c;
        return c;
    }

    namespace
    {
        FILE *open()
        {
            const char *path = std::getenv("PS2X_FRAME_LOG");
            if (!path || !*path)
                return nullptr;
            FILE *f = std::fopen(path, "w");
            if (!f)
            {
                std::fprintf(stderr, "[framelog] cannot write %s\n", path);
                return nullptr;
            }
            // hostMs: real time since the previous vblank (16.7 = on time). newFrame: the game showed a new frame.
            // eeMcycles: guest CPU cycles charged in this interval (a PS2 frame is 4.92). pacingMs: the game thread was
            // held because guest time ran ahead of real time (the PS2 CPU-speed cap). idleMs: every guest thread
            // waiting. busyMs: the rest (game code, library code, waits on the render threads = gameWaitMs).
            // vu1Programs, workerMs, drawMs: VU1 microprograms started and the two render threads' busy time.
            // vu1Ms: VU1 program execution inside workerMs; vu1Aot/vu1Fallback: runs by compiled code / interpreter.
            // texMs/decodes/decodeKpx: GPU texture lookups that missed the fast path, texture decodes, decoded pixels (k);
            // uploadMs/uploadKB: image uploads into GS memory; mirrorKpx: upload pixels copied into GPU render targets.
            std::fprintf(f, "tick,hostMs,newFrame,eeMcycles,pacingMs,idleMs,busyMs,gameWaitMs,vu1Programs,workerMs,drawMs,vu1Ms,"
                            "vu1Aot,vu1Fallback,vu1KickMs,texMs,decodes,decodeKpx,uploadMs,uploadKB,mirrorKpx\n");
            std::fprintf(stderr, "[framelog] writing %s\n", path);
            return f;
        }

        FILE *file()
        {
            static FILE *f = open();
            return f;
        }
    }

    bool enabled()
    {
        return file() != nullptr;
    }

    void onVblank(uint64_t tick, uint64_t eeCycle, bool newGameFrame)
    {
        FILE *f = file();
        if (!f)
            return;
        struct Snap
        {
            std::chrono::steady_clock::time_point t{};
            uint64_t ee = 0, vu1 = 0, worker = 0, draw = 0, wait = 0, pacing = 0, idle = 0, vu1Ns = 0, aot = 0, fallback = 0, kick = 0;
            uint64_t tex = 0, decodes = 0, decodePx = 0, upload = 0, uploadBytes = 0, mirror = 0;
        };
        static Snap last;
        static unsigned lines = 0;
        Counters &c = counters();
        Snap now;
        now.t = std::chrono::steady_clock::now();
        now.ee = eeCycle;
        now.vu1 = c.vu1Programs.load(std::memory_order_relaxed);
        now.worker = c.gsWorkerNs.load(std::memory_order_relaxed);
        now.draw = c.gsDrawNs.load(std::memory_order_relaxed);
        now.wait = c.gameWaitNs.load(std::memory_order_relaxed);
        now.pacing = c.pacingNs.load(std::memory_order_relaxed);
        now.idle = c.idleNs.load(std::memory_order_relaxed);
        now.vu1Ns = c.vu1Ns.load(std::memory_order_relaxed);
        now.aot = c.vu1Aot.load(std::memory_order_relaxed);
        now.fallback = c.vu1Fallback.load(std::memory_order_relaxed);
        now.kick = c.vu1KickNs.load(std::memory_order_relaxed);
        now.tex = c.gsTexNs.load(std::memory_order_relaxed);
        now.decodes = c.gsDecodes.load(std::memory_order_relaxed);
        now.decodePx = c.gsDecodePx.load(std::memory_order_relaxed);
        now.upload = c.gsUploadNs.load(std::memory_order_relaxed);
        now.uploadBytes = c.gsUploadBytes.load(std::memory_order_relaxed);
        now.mirror = c.gsMirrorPx.load(std::memory_order_relaxed);
        if (last.t != std::chrono::steady_clock::time_point{} && now.ee >= last.ee)
        {
            const double hostMs = std::chrono::duration<double, std::milli>(now.t - last.t).count();
            const double pacingMs = (now.pacing - last.pacing) / 1e6, idleMs = (now.idle - last.idle) / 1e6;
            std::fprintf(f, "%llu,%.2f,%d,%.3f,%.2f,%.2f,%.2f,%.2f,%llu,%.2f,%.2f,%.2f,%llu,%llu,%.2f,%.2f,%llu,%.1f,%.2f,%.1f,%.1f\n",
                         static_cast<unsigned long long>(tick), hostMs, newGameFrame ? 1 : 0, (now.ee - last.ee) / 1e6, pacingMs, idleMs,
                         hostMs - pacingMs - idleMs, (now.wait - last.wait) / 1e6, static_cast<unsigned long long>(now.vu1 - last.vu1),
                         (now.worker - last.worker) / 1e6, (now.draw - last.draw) / 1e6, (now.vu1Ns - last.vu1Ns) / 1e6,
                         static_cast<unsigned long long>(now.aot - last.aot), static_cast<unsigned long long>(now.fallback - last.fallback),
                         (now.kick - last.kick) / 1e6, (now.tex - last.tex) / 1e6,
                         static_cast<unsigned long long>(now.decodes - last.decodes), (now.decodePx - last.decodePx) / 1e3,
                         (now.upload - last.upload) / 1e6, (now.uploadBytes - last.uploadBytes) / 1024.0, (now.mirror - last.mirror) / 1e3);
            if (++lines % 60u == 0u)
                std::fflush(f);
        }
        last = now;
    }
}
