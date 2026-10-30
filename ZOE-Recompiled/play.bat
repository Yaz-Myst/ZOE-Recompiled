@echo off
rem Launch the recompiled Zone of the Enders with the OpenGL GS renderer (PS2X_GPU=1). Same as the workspace's play_gpu.bat, ZOE paths.
rem Keyboard: Enter=Start, arrows/WASD=D-pad, X/Space=Cross, C=Circle, Z=Square, V=Triangle, Q/E=L1/R1, Shift=L2/R2, Tab=Select.
rem Esc opens the settings menu. F1 toggles the runtime debugger panel. F9 captures a graphics diagnostic (work\diag + play.log).
rem All paths come from zoe.cfg (next to this file). Logs go to work\play.log / play.err.log under this folder.
rem Runs a copy of the build (work\play_bin) so the build output can be rebuilt while playing.
setlocal
for /f "usebackq eol=# tokens=1* delims==" %%a in ("%~dp0zoe.cfg") do set "CFG_%%a=%%b"
set "BUILD=%CFG_BUILD_DIR:/=\%"
set "WORK=%CFG_WORK_DIR:/=\%"
set "ELF=%CFG_ELF:/=\%"
set "DISC=%CFG_DISC_DIR:/=\%"
set "ISO=%CFG_CD_IMAGE:/=\%"
rem A cfg path without a drive letter is relative to this file's folder (same rule as games\lib.ps1 / lib.sh).
if "%BUILD:~1,1%" neq ":" set "BUILD=%~dp0%BUILD%"
if "%WORK:~1,1%" neq ":" set "WORK=%~dp0%WORK%"
if "%ELF:~1,1%" neq ":" set "ELF=%~dp0%ELF%"
if "%DISC:~1,1%" neq ":" set "DISC=%~dp0%DISC%"
if not exist "%WORK%" mkdir "%WORK%"
if not exist "%WORK%\diag" mkdir "%WORK%\diag"
robocopy "%BUILD%\ps2xRuntime\Release" "%WORK%\play_bin" *.exe *.dll /NJH /NJS /NFL /NDL /NP >nul
set "PS2X_CD_IMAGE=%ISO%"
set "PS2X_DIAG_DIR=%WORK%\diag"
rem Save states: F5 save, F8 load, F6/F7 slot (0-9). Files go to work\states.
set "PS2X_STATE_DIR=%WORK%\states"
rem Per-vsync performance timeline (one CSV line per frame, cheap). Remove to turn off.
set "PS2X_FRAME_LOG=%WORK%\play_frames.csv"
set PS2X_GPU=1
rem Render threads: VIF1/VU1 on one thread and the GS on another, beside the game instead of inside it.
rem PS2X_GS_THREAD=1 = one render thread, 0 = everything on the game thread (the old way).
set PS2X_GS_THREAD=2
rem Show each game frame once, when the game flips to it (no skipped or doubled frames). Remove for the old
rem 60 fps window timer.
set PS2X_PRESENT=flip
rem Every button press is saved as a replayable PS2X_INPUT_SCRIPT.
rem Times are game seconds (vsync ticks / 60), so a recording replays correctly at any speed.
set PS2X_INPUT_CLOCK=ticks
set "PS2X_INPUT_RECORD=%WORK%\input_record.txt"
rem Start in 16:9 widescreen (wider 3D view, HUD at the screen edges, movies and logos boxed 4:3).
rem Esc > Graphics > Widescreen 16:9 switches it while playing. 0 = off and hide the option; remove = saved setting.
set PS2X_ZOE_WIDESCREEN=1
cd /d "%DISC%"
start "" /b cmd /c ""%WORK%\play_bin\ps2EntryRunner.exe" "%ELF%" > "%WORK%\play.log" 2> "%WORK%\play.err.log""
