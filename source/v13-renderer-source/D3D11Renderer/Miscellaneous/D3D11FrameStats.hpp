/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.53: frame-time statistics like CapFrameX / PresentMon / MSI Afterburner.
//
// Average FPS hides stutter: 40 fps with one 300 ms frame every few seconds feels worse than a
// steady 30 fps. These are the measures benchmark tools use, computed from the time between two
// presents (what the player sees):
//   * frame time percentiles  p50 / p90 / p95 / p99 / p99.9 (ms)
//   * 1% low and 0.1% low FPS  = 1000 / (average of the slowest 1% / 0.1% frame times)
//     ("average" variant as in CapFrameX and Afterburner; the percentile variant is 1000/p99)
//   * frame-time standard deviation (ms) and consistency = share of frames within +-20% of
//     the median frame time
//   * stutters = frames longer than 2.5x the median frame time of the whole session
//   * missed refreshes ("dropped/skipped frames" against the display): a frame that takes
//     longer than one refresh interval (60 Hz = 16.7 ms by default, RW_GPU_REFRESH_HZ) keeps
//     the old picture on screen for round(interval / refresh) - 1 extra refreshes.
// Output:
//   * [RW-GPU-FRAMESTATS] every 30 s, cumulative for the whole session (so a crash keeps the
//     last numbers), read by Summarize-Run.ps1;
//   * a per-frame CSV when RW_GPU_FRAMETIME_CSV=<path> is set (the launcher puts it in the run
//     folder): frame, time_s, frametime_ms, present_ms, bridge_ms, cpu_speed_pct. Opens in
//     Excel/LibreOffice or CapFrameX-style graphing.

#include <windows.h>

#include <atomic>
#include <locale.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ashes::D3D11_NAMESPACE::framestats
{
	// v13.55: incremented by the frame diagnostics for every picture they save. A frame whose
	// interval contains such work is a diagnostic frame (map tests: ~1.3 s every 20 s) and is
	// left out of the gameplay statistics, like loading pauses over 2 s.
	inline std::atomic< uint64_t > g_diagnosticWork{ 0u };
	inline constexpr double LoadingPauseMs = 2000.0;

	// Numbers in the CSV always use a dot: the game process runs with the user's locale
	// (Dutch: decimal comma), which broke the comma-separated columns in v13.53.
	inline _locale_t cLocale()
	{
		static _locale_t const result = _create_locale( LC_NUMERIC, "C" );
		return result;
	}
	struct Summary
	{
		size_t frames{};
		double avgFps{}, low1{}, low01{};
		double p50{}, p90{}, p95{}, p99{}, p999{}, maxMs{};
		double stddev{}, consistency{};
		uint64_t stutters{};
		uint64_t missedFrames{}, missedRefreshes{};
	};

	inline double refreshMs()
	{
		static double const result = []()
			{
				char text[16]{};
				auto length = GetEnvironmentVariableA( "RW_GPU_REFRESH_HZ", text, DWORD( sizeof( text ) ) );
				double hz = ( length > 0u && length < sizeof( text ) ) ? std::atof( text ) : 60.0;
				if ( hz < 20.0 || hz > 360.0 ) hz = 60.0;
				return 1000.0 / hz;
			}();
		return result;
	}

	// All statistics from a list of frame times (ms). The list is copied and sorted.
	inline Summary summarise( std::vector< float > const & frameTimes )
	{
		Summary r;
		r.frames = frameTimes.size();
		if ( frameTimes.empty() ) return r;
		std::vector< float > sorted = frameTimes;
		std::sort( sorted.begin(), sorted.end() );
		double sum = 0.0;
		for ( auto v : sorted ) sum += v;
		double const mean = sum / double( sorted.size() );
		auto pick = [&sorted]( double q ) { return double( sorted[ size_t( double( sorted.size() - 1u ) * q ) ] ); };
		r.avgFps = mean > 0.0 ? 1000.0 / mean : 0.0;
		r.p50 = pick( 0.50 ); r.p90 = pick( 0.90 ); r.p95 = pick( 0.95 ); r.p99 = pick( 0.99 ); r.p999 = pick( 0.999 );
		r.maxMs = sorted.back();
		auto lowAverage = [&sorted]( double share )
			{
				size_t const count = std::max< size_t >( 1u, size_t( double( sorted.size() ) * share ) );
				double worst = 0.0;
				for ( size_t i = sorted.size() - count; i < sorted.size(); ++i ) worst += sorted[i];
				worst /= double( count );
				return worst > 0.0 ? 1000.0 / worst : 0.0;
			};
		r.low1 = lowAverage( 0.01 );
		r.low01 = lowAverage( 0.001 );
		double var = 0.0;
		size_t within = 0u;
		double const refresh = refreshMs();
		for ( auto v : frameTimes )
		{
			var += ( v - mean ) * ( v - mean );
			if ( v >= 0.8 * r.p50 && v <= 1.2 * r.p50 ) ++within;
			if ( v > 2.5 * r.p50 ) ++r.stutters;
			if ( v > 1.5 * refresh )
			{
				++r.missedFrames;
				r.missedRefreshes += uint64_t( std::lround( v / refresh ) ) - 1u;
			}
		}
		r.stddev = std::sqrt( var / double( frameTimes.size() ) );
		r.consistency = 100.0 * double( within ) / double( frameTimes.size() );
		return r;
	}

	inline void report( double sessionSeconds );

	struct State
	{
		std::vector< float > session;   // every frame time since start (~4 bytes per frame)
		std::FILE * csv{};
		bool csvTried{};
		uint64_t frame{};
		uint64_t lastBridgeUs{};
		// v13.55: "gameplay" starts at the first run of 120 frames in a row under 100 ms, so
		// loading screens and the map build (single frames of several seconds) do not distort
		// the 1%/0.1% lows of what the player feels while playing.
		size_t gameplayStart{ size_t( -1 ) };
		uint32_t calmRun{};
		std::vector< uint8_t > excluded;   // 1 = diagnostic frame, 2 = loading pause (> 2 s)
		uint64_t lastDiagnosticWork{};

		// Final line when the process ends (short sessions never reach a 30 s report).
		~State()
		{
			if ( session.size() < 2u ) return;
			double total = 0.0;
			for ( auto v : session ) total += v;
			report( total / 1000.0 );
			if ( csv ) std::fclose( csv );
			csv = nullptr;
		}
	};

	inline State & state()
	{
		static State result;
		return result;
	}

	// Called once per present with the interval since the previous present.
	inline void onFrame( double intervalMs, double presentMs, double timeSeconds, uint64_t bridgeUsTotal, uint32_t cpuSpeedPercent )
	{
		auto & s = state();
		++s.frame;
		s.session.push_back( float( intervalMs ) );
		uint64_t const diagNow = g_diagnosticWork.load( std::memory_order_relaxed );
		uint8_t const why = diagNow != s.lastDiagnosticWork ? uint8_t( 1u ) : ( intervalMs > LoadingPauseMs ? uint8_t( 2u ) : uint8_t( 0u ) );
		s.lastDiagnosticWork = diagNow;
		s.excluded.push_back( why );
		if ( s.gameplayStart == size_t( -1 ) )
		{
			s.calmRun = intervalMs < 100.0 ? s.calmRun + 1u : 0u;
			if ( s.calmRun >= 120u ) s.gameplayStart = s.session.size() - 120u;
		}
		double const bridgeMs = double( bridgeUsTotal - s.lastBridgeUs ) / 1000.0;
		s.lastBridgeUs = bridgeUsTotal;
		if ( !s.csvTried )
		{
			s.csvTried = true;
			char path[1024]{};
			auto length = GetEnvironmentVariableA( "RW_GPU_FRAMETIME_CSV", path, DWORD( sizeof( path ) ) );
			if ( length > 0u && length < sizeof( path ) )
			{
				s.csv = std::fopen( path, "w" );
				if ( s.csv )
				{
					static char buffer[1u << 16];
					std::setvbuf( s.csv, buffer, _IOFBF, sizeof( buffer ) );
					std::fprintf( s.csv, "frame,time_s,frametime_ms,present_ms,bridge_ms,cpu_speed_pct,excluded\n" );
				}
			}
		}
		if ( s.csv )
		{
			// excluded: 0 = gameplay, 1 = diagnostic picture saved in this frame, 2 = loading pause (> 2 s)
			_fprintf_l( s.csv, "%llu,%.3f,%.3f,%.3f,%.3f,%u,%u\n", cLocale(), (unsigned long long)s.frame, timeSeconds, intervalMs, presentMs, bridgeMs, cpuSpeedPercent, unsigned( why ) );
		}
	}

	// Called every 30 s: cumulative session line (+ flush the CSV).
	inline void report( double sessionSeconds )
	{
		auto & s = state();
		if ( s.csv ) std::fflush( s.csv );
		auto r = summarise( s.session );
		if ( !r.frames ) return;
		if ( s.gameplayStart != size_t( -1 ) && s.gameplayStart < s.session.size() )
		{
			std::vector< float > play;
			play.reserve( s.session.size() - s.gameplayStart );
			uint64_t diagFrames = 0u, loadFrames = 0u;
			double loadSeconds = 0.0;
			for ( size_t i = s.gameplayStart; i < s.session.size(); ++i )
			{
				if ( s.excluded[i] == 1u ) { ++diagFrames; continue; }
				if ( s.excluded[i] == 2u ) { ++loadFrames; loadSeconds += s.session[i] / 1000.0; continue; }
				play.push_back( s.session[i] );
			}
			auto g = summarise( play );
			double skipped = 0.0;
			for ( size_t i = 0u; i < s.gameplayStart; ++i ) skipped += s.session[i];
			std::fprintf( stderr, "[RW-GPU-FRAMESTATS] excluded from gameplay: start-up %.0f s, %llu loading pause(s) over 2 s (%.1f s), %llu frame(s) with diagnostic pictures\n"
				, skipped / 1000.0, (unsigned long long)loadFrames, loadSeconds, (unsigned long long)diagFrames );
			std::fprintf( stderr, "[RW-GPU-FRAMESTATS] gameplay frames=%zu avg=%.1ffps 1%%low=%.1ffps 0.1%%low=%.1ffps"
				" frametime p50=%.1f p90=%.1f p95=%.1f p99=%.1f p99.9=%.1f max=%.0fms stddev=%.1fms consistency=%.0f%%"
				" stutters(>2.5x median)=%llu missed-refreshes=%llu in %llu frames (%.0f Hz)\n"
				, g.frames, g.avgFps, g.low1, g.low01
				, g.p50, g.p90, g.p95, g.p99, g.p999, g.maxMs, g.stddev, g.consistency
				, (unsigned long long)g.stutters, (unsigned long long)g.missedRefreshes, (unsigned long long)g.missedFrames
				, 1000.0 / refreshMs() );
		}
		std::fprintf( stderr, "[RW-GPU-FRAMESTATS] session t=%.0fs frames=%zu avg=%.1ffps 1%%low=%.1ffps 0.1%%low=%.1ffps"
			" frametime p50=%.1f p90=%.1f p95=%.1f p99=%.1f p99.9=%.1f max=%.0fms stddev=%.1fms consistency=%.0f%%"
			" stutters(>2.5x median)=%llu missed-refreshes=%llu in %llu frames (%.0f Hz)\n"
			, sessionSeconds, r.frames, r.avgFps, r.low1, r.low01
			, r.p50, r.p90, r.p95, r.p99, r.p999, r.maxMs, r.stddev, r.consistency
			, (unsigned long long)r.stutters, (unsigned long long)r.missedRefreshes, (unsigned long long)r.missedFrames
			, 1000.0 / refreshMs() );
		std::fflush( stderr );
	}
}
