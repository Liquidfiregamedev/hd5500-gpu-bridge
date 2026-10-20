/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.68: time spent inside the frame-level Vulkan entry points, per frame.
// v13.69b: vkQueuePresentKHR took 12-17 ms per frame in run 140535 while the DXGI Present call
// itself took < 1 ms: the present path is split into its parts (present.*) to find the rest.
// RimWorld's own profiler (HD5500 Optimizer, level 3) showed 12 ms per frame of "self" time in
// Waylith's RenderThread.RenderFrame - outside the recording of draws - which is where it calls
// vkAcquireNextImageKHR, vkWaitForFences (inside its acquire), vkResetCommandBuffer,
// vkBegin/EndCommandBuffer, vkQueueSubmit and vkQueuePresentKHR. The bridge only measured the
// execution part of vkQueueSubmit. This adds a QPC timer around each of these calls (a few
// calls per frame, so no measurable cost) and prints them every 30 s:
//   [RW-GPU-APITIME] per frame: submit=..ms acquire=.. waitFences=.. present=.. begin=.. end=.. reset=..

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ashes::D3D11_NAMESPACE::apitime
{
	enum Kind : uint32_t { Submit, Present, Acquire, WaitFences, Begin, End, Reset, ResetPool, ResetFences, PLock, PDiag, PCopy, PCall, PPacing, PStats, PDrain, SubmitLock, Count };

	inline std::atomic< uint64_t > g_us[Count]{};
	inline std::atomic< uint64_t > g_calls[Count]{};

	inline LONGLONG freq()
	{
		static LONGLONG const f = []() { LARGE_INTEGER v{}; QueryPerformanceFrequency( &v ); return v.QuadPart ? v.QuadPart : 1; }();
		return f;
	}

	struct Scope
	{
		explicit Scope( Kind k ) : kind{ k } { QueryPerformanceCounter( &start ); }
		~Scope()
		{
			LARGE_INTEGER end{};
			QueryPerformanceCounter( &end );
			g_us[kind].fetch_add( uint64_t( ( end.QuadPart - start.QuadPart ) * 1000000 / freq() ), std::memory_order_relaxed );
			g_calls[kind].fetch_add( 1u, std::memory_order_relaxed );
		}
		Kind kind;
		LARGE_INTEGER start{};
	};

	inline LONGLONG now() { LARGE_INTEGER v{}; QueryPerformanceCounter( &v ); return v.QuadPart; }
	inline void add( Kind k, LONGLONG from, LONGLONG to )
	{
		g_us[k].fetch_add( uint64_t( ( to - from ) * 1000000 / freq() ), std::memory_order_relaxed );
		g_calls[k].fetch_add( 1u, std::memory_order_relaxed );
	}

	// Window print; called from the 30 s frame statistics with that window's frame count.
	inline void printAndReset( size_t frames )
	{
		if ( !frames ) return;
		static char const * const names[Count] = { "submit", "present", "acquire", "waitFences", "begin", "end", "reset", "resetPool", "resetFences", "present.lock", "present.diag", "present.copy", "present.Present", "present.pacing", "present.stats", "present.drain", "submit.lock" };
		char line[1400];
		int n = std::snprintf( line, sizeof( line ), "[RW-GPU-APITIME] per frame (last window, %zu frames):", frames );
		for ( uint32_t i = 0u; i < Count && n > 0 && n < int( sizeof( line ) ); ++i )
		{
			uint64_t const us = g_us[i].exchange( 0u );
			uint64_t const calls = g_calls[i].exchange( 0u );
			n += std::snprintf( line + n, sizeof( line ) - size_t( n ), " %s=%.2fms (%.1fx)", names[i], double( us ) / 1000.0 / double( frames ), double( calls ) / double( frames ) );
		}
		std::fprintf( stderr, "%s\n", line );
	}
}
