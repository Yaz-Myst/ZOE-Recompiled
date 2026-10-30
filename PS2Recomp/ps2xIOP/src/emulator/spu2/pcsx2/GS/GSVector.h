// PS2Recomp compatibility shim: the few GSVector4i operations ReverbResample.cpp's SSE path uses,
// mapped 1:1 onto SSE2/SSSE3 intrinsics so the reverb resampler matches PCSX2's non-AVX build.
#pragma once

#include "common/Pcsx2Types.h"

#include <emmintrin.h>
#include <tmmintrin.h>

#ifndef _M_SSE
#define _M_SSE 0x401
#endif

struct GSVector4i
{
	union
	{
		__m128i m;
		s16 I16[8];
	};

	GSVector4i() = default;
	explicit GSVector4i(__m128i v) : m(v) {}

	template <bool aligned>
	static GSVector4i load(const void* p)
	{
		return GSVector4i(aligned ? _mm_load_si128(static_cast<const __m128i*>(p)) : _mm_loadu_si128(static_cast<const __m128i*>(p)));
	}

	GSVector4i mul16hrs(const GSVector4i& v) const { return GSVector4i(_mm_mulhrs_epi16(m, v.m)); }
	GSVector4i adds16(const GSVector4i& v) const { return GSVector4i(_mm_adds_epi16(m, v.m)); }
	GSVector4i hadds16(const GSVector4i& v) const { return GSVector4i(_mm_hadds_epi16(m, v.m)); }
};
