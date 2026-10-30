// PS2Recomp compatibility shim: the subset of PCSX2's common/Pcsx2Types.h used by the ported SPU2 code.
#pragma once

#include <cstddef>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using uint = unsigned int;
using uptr = std::uintptr_t;
using sptr = std::intptr_t;

#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif
#ifndef __fi
#define __fi __forceinline
#endif
#ifndef __ri
#define __ri
#endif

static constexpr bool IsDevBuild = false;
static constexpr bool IsDebugBuild = false;
