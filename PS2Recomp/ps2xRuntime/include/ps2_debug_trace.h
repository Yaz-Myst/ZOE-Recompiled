#pragma once

#include <atomic>
#include <cstdlib>

// Debug tracing switches for graphics bring-up.
// - PS2X_TRACE_DRAWS (env): sampled tracing for the whole run.
// - F9 in the game window: "burst" tracing of every event for the next few presented frames.
namespace ps2_debug
{
    inline std::atomic<int> g_traceFramesRemaining{0};

    inline bool traceSampled()
    {
        static const bool enabled = std::getenv("PS2X_TRACE_DRAWS") != nullptr;
        return enabled;
    }

    inline bool traceBurst()
    {
        return g_traceFramesRemaining.load(std::memory_order_relaxed) > 0;
    }

    inline bool traceAny()
    {
        return traceBurst() || traceSampled();
    }
}
