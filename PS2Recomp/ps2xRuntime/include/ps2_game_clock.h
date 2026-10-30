#pragma once
// Game-time clock for unattended runs. The frame loop reports the current vsync tick; with PS2X_INPUT_CLOCK=ticks,
// PS2X_INPUT_SCRIPT / PS2X_INPUT_RECORD times are game seconds (ticks / 60) since the first pad read instead of host
// seconds, so a route keeps working when the build gets faster or slower. Not included by generated code.
#include <cstdint>

void ps2NoteVsyncTick(uint64_t tick);
uint64_t ps2CurrentVsyncTick();
