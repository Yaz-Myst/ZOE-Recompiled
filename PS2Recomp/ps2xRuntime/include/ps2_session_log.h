#pragma once

#include <filesystem>

// Player logs. A release build is a window program without a console, so everything the runtime prints (start-up,
// warnings, the crash reporter's stack) was lost unless the game was started from a script that redirected it. This
// sends stdout to <directory>/last.log and stderr to last.err.log (the session before is kept as previous*.log).
// stdout is flushed every second and stderr is unbuffered, so a crash still leaves its last lines; both stop at
// 64 MB together so a message flood cannot fill the disk. Nothing changes when the output already goes to a file
// (play.bat's play.log) or a console window; NUL and pipes are replaced. PS2X_SESSION_LOG=0 turns it off, =1 redirects even then.
// frameLog (the "Performance log" setting): also writes the per-vblank timing CSV (ps2_frame_log.h) to
// <directory>/frames.csv, unless PS2X_FRAME_LOG already names a file.
void ps2StartSessionLog(const std::filesystem::path &directory, bool frameLog);
