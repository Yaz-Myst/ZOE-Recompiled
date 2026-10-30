// PS2Recomp compatibility shim for PCSX2's common/Console.h: printf-style logging to stderr.
#pragma once

#include <cstdarg>
#include <cstdio>

struct Spu2ConsoleShim
{
	void WriteLn(const char* fmt, ...) const { va_list args; va_start(args, fmt); std::fputs("[spu2] ", stderr); std::vfprintf(stderr, fmt, args); std::fputc('\n', stderr); va_end(args); }
	void Warning(const char* fmt, ...) const { va_list args; va_start(args, fmt); std::fputs("[spu2] warning: ", stderr); std::vfprintf(stderr, fmt, args); std::fputc('\n', stderr); va_end(args); }
	void Error(const char* fmt, ...) const { va_list args; va_start(args, fmt); std::fputs("[spu2] error: ", stderr); std::vfprintf(stderr, fmt, args); std::fputc('\n', stderr); va_end(args); }
};

inline constexpr Spu2ConsoleShim Console{};
inline constexpr Spu2ConsoleShim DevCon{};
