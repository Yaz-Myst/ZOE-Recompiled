#pragma once

// Toggle the in-process sampling profiler. Returns true when sampling has started, false when it
// stopped and the report was written to <outputDir>/profile.txt.
bool ps2ProfilerToggle(const char *outputDir);

// Installs a last-chance crash reporter: on an unhandled fault it prints the exception code, the
// faulting instruction as a module-relative RVA (resolve with the linker map) and, for access
// violations, the address that was touched.
void ps2InstallCrashReporter();

#include <cstdint>
// Debug aid: hardware write watchpoints on up to four guest RAM words (all current threads; call again
// to pick up threads created later). Each hit logs the new value and the writer's host RVAs to stderr.
void ps2WatchArm(uint8_t *rdram, const uint32_t *guestAddrs, uint32_t count);

// Debug aid (PS2X_FPTRAP): unmask SSE overflow / invalid-operation exceptions on the calling thread; the first
// instructions that produce infinity or NaN (which the PS2 never does) are logged with their host RVAs.
void ps2FpTrapArm();
