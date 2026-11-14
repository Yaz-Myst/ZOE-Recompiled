@echo off
rem build.bat <game build dir> <generated aot .cpp> <register function>   e.g.
rem build.bat <repo>\ZOE-Recompiled\build <repo>\ZOE-Recompiled\src\zoe_iop_aot.cpp zoeRegisterIopAot
rem Uses the game's already built IOP libraries: build the game (or the ps2_iop target) first.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set I=%~dp0..\..\PS2Recomp\ps2xIOP
cl /nologo /c /O2 /EHsc /std:c++20 /MD /bigobj /I %I%\include %2 /Fo:aot.obj || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MD /DIOP_AOT_ENTRY=%3 /I %I%\src\emulator /I %I%\include %~dp0replay.cpp aot.obj %1\ps2xIOP\Release\ps2_iop.lib %1\ps2xIOP\Release\ps2_iop_spu2.lib /Fe:replay.exe /link /MAP:replay.map
