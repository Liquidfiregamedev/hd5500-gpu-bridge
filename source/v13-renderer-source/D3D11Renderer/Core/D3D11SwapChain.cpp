#include <cstdlib>
#include <atomic>
#include "renderer/D3D11Renderer/Miscellaneous/D3D11WatchedBytes.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11CpuSpeed.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11FrameStats.hpp"
#include "Command/D3D11SubmitThread.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "Miscellaneous/D3D11UavAlias.hpp"
#include <vector>
#include <cstdio>
#include <algorithm>
#include "Core/D3D11SwapChain.hpp"
#include <dxgi1_5.h>
#include "Command/D3D11CommandStats.hpp"
#include "Command/D3D11CommandReaper.hpp"
#include "Miscellaneous/D3D11ApiTime.hpp"
#include "Miscellaneous/D3D11UniformRing.hpp"
#include "Miscellaneous/D3D11IndexPool.hpp"
#include "Command/D3D11CommandBuffer.hpp"

#include "Core/D3D11Device.hpp"
#include "Core/D3D11Instance.hpp"
#include "Core/D3D11PhysicalDevice.hpp"
#include "Core/D3D11Surface.hpp"
#include "Image/D3D11Image.hpp"
#include "RenderPass/D3D11FrameBuffer.hpp"
#include "RenderPass/D3D11RenderPass.hpp"
#include "Sync/D3D11Semaphore.hpp"

#include "ashesd3d11_api.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#pragma warning( push )
#pragma warning( disable: 5262 )
#include <sstream>
#if defined( Ashes_D3D11_XBox )
#include <winrt/windows.ui.core.h>
#endif
#pragma warning( pop )

#include <chrono>
#include <cstring>
#include <iostream>

namespace ashes::D3D11_NAMESPACE
{
	namespace framepacing
	{
		// v13.48: wait (sleeping, not spinning) after each present until the GPU has finished
		// the previous frame. Without it the CPU runs ahead until the driver's command ring is
		// full, then the Intel driver spin-waits inside ordinary D3D calls: one frame in four
		// or five took 150-250 ms of full CPU instead of ~20 ms (the stutter), and the spinning
		// stole CPU time from the game's own threads. RW_GPU_FRAME_PACING=0 switches it off;
		// =N lets the GPU fall up to N frames behind (default 1).
		inline std::atomic< uint64_t > g_waitUs{ 0u };

		inline void afterPresent( ID3D11Device * device, ID3D11DeviceContext * context )
		{
			static UINT const depth = []()
				{
					char text[8]{};
					auto length = GetEnvironmentVariableA( "RW_GPU_FRAME_PACING", text, DWORD( sizeof( text ) ) );
					if ( length > 0u && length < sizeof( text ) ) return std::min< UINT >( UINT( std::strtoul( text, nullptr, 10 ) ), 4u );
					return 1u;
				}();
			// Optional frame-rate cap (RW_GPU_FPS_CAP=N, default off): less CPU/GPU heat and power
			// per second, so a laptop that throttles under load does it less often.
			static double const capMs = []()
				{
					char text[8]{};
					auto length = GetEnvironmentVariableA( "RW_GPU_FPS_CAP", text, DWORD( sizeof( text ) ) );
					if ( length > 0u && length < sizeof( text ) )
					{
						auto fps = std::strtoul( text, nullptr, 10 );
						if ( fps >= 5u && fps <= 240u ) return 1000.0 / double( fps );
					}
					return 0.0;
				}();
			// v13.50 heat guard: while the Throttle monitor reports throttling or heat, cap the frame
			// rate (RW_GPU_THERMAL_GUARD_FPS, default 30) so the laptop produces less heat.
			static double const guardMs = []()
				{
					auto fps = cpuspeed::envNumber( "RW_GPU_THERMAL_GUARD_FPS", 30u );
					return ( fps >= 10u && fps <= 120u ) ? 1000.0 / double( fps ) : 1000.0 / 30.0;
				}();
			double const effectiveCapMs = cpuspeed::stats().guardActive ? std::max( capMs, guardMs ) : capMs;
			if ( effectiveCapMs > 0.0 )
			{
				static LARGE_INTEGER last{};
				LARGE_INTEGER f{}, now{};
				QueryPerformanceFrequency( &f );
				QueryPerformanceCounter( &now );
				if ( last.QuadPart && f.QuadPart )
				{
					double elapsed = double( now.QuadPart - last.QuadPart ) * 1000.0 / double( f.QuadPart );
					if ( elapsed < effectiveCapMs )
					{
						auto sleepStart = now;
						// v13.71: wait with a high-resolution waitable timer (Windows 10 1803+): one precise
						// sleep instead of Sleep(1) steps plus a SwitchToThread spin for the last 1.5 ms, so
						// the capped frames cost no CPU while waiting (less heat - the point of the cap).
						static HANDLE const timer = []()
							{
								HANDLE h = CreateWaitableTimerExW( nullptr, nullptr, 0x00000002u /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS );
								return h;
							}();
						if ( timer && elapsed < effectiveCapMs - 0.3 )
						{
							LARGE_INTEGER due{};
							due.QuadPart = -LONGLONG( ( effectiveCapMs - 0.3 - elapsed ) * 10000.0 );   // relative, 100 ns units
							if ( SetWaitableTimerEx( timer, &due, 0, nullptr, nullptr, nullptr, 0 ) ) WaitForSingleObject( timer, 1000u );
							QueryPerformanceCounter( &now ); elapsed = double( now.QuadPart - last.QuadPart ) * 1000.0 / double( f.QuadPart );
						}
						while ( elapsed < effectiveCapMs - 1.5 ) { Sleep( 1u ); QueryPerformanceCounter( &now ); elapsed = double( now.QuadPart - last.QuadPart ) * 1000.0 / double( f.QuadPart ); }
						while ( elapsed < effectiveCapMs ) { SwitchToThread(); QueryPerformanceCounter( &now ); elapsed = double( now.QuadPart - last.QuadPart ) * 1000.0 / double( f.QuadPart ); }
						g_waitUs += uint64_t( double( now.QuadPart - sleepStart.QuadPart ) * 1e6 / double( f.QuadPart ) );
					}
				}
				last = now;
			}
			if ( !depth || !device || !context ) return;
			static ID3D11Query * queries[5]{};
			static uint32_t index{};
			static bool failed{};
			if ( failed ) return;
			auto & mine = queries[index % ( depth + 1u )];
			if ( !mine )
			{
				D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0u };
				if ( FAILED( device->CreateQuery( &desc, &mine ) ) ) { failed = true; return; }
			}
			context->End( mine );
			++index;
			auto & oldest = queries[index % ( depth + 1u )];
			if ( !oldest ) return;
			LARGE_INTEGER f{}, a{}, b{};
			QueryPerformanceFrequency( &f );
			QueryPerformanceCounter( &a );
			BOOL done = FALSE;
			if ( context->GetData( oldest, &done, sizeof( done ), 0u ) != S_OK )   // flushes once
			{
				uint32_t spins = 0u;
				while ( context->GetData( oldest, &done, sizeof( done ), D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK )
				{
					if ( ++spins < 50u ) SwitchToThread();
					else Sleep( 1u );
					QueryPerformanceCounter( &b );
					if ( f.QuadPart && double( b.QuadPart - a.QuadPart ) / double( f.QuadPart ) > 2.0 ) break;   // never hang
				}
			}
			QueryPerformanceCounter( &b );
			if ( f.QuadPart ) g_waitUs += uint64_t( double( b.QuadPart - a.QuadPart ) * 1e6 / double( f.QuadPart ) );
		}
	}
}

namespace ashes::D3D11_NAMESPACE
{
	namespace frametiming
	{
		// v13.48: frame pacing record for freeze analysis (works in normal play, no diagnostics
		// needed). Interval = time between two presents as the game sees it. Every 30 s one
		// summary line; every frame over 250 ms its own line (first 150).
		struct State
		{
			LARGE_INTEGER freq{};
			LARGE_INTEGER start{};
			LARGE_INTEGER last{};
			LARGE_INTEGER windowStart{};
			std::vector< float > window;
			uint64_t total{};
			uint64_t over[4]{};
			uint32_t hitchLines{};
			uint64_t lastRequested{}, lastCopied{}, lastSkipped{};
			// GPU time per frame: ring of timestamp queries, read back a few frames later
			// without waiting (D3D11_ASYNC_GETDATA_DONOTFLUSH).
			struct GpuSlot { ID3D11Query * disjoint{}; ID3D11Query * begin{}; ID3D11Query * end{}; bool pending{}; };
			GpuSlot gpu[6]{};
			uint32_t gpuIndex{};
			bool gpuOpen{};
			bool gpuFailed{};
			std::vector< float > gpuWindow;
			uint64_t lastBridgeUs{};
			uint64_t lastWatchUs{};
			uint64_t lastCpuUs{}, lastExecutions{}, lastSlow{};
			uint64_t lastWaitUs{}, lastWaitUsNext{};
			uint64_t lastSpeedSamples{}, lastSpeedSum{}, lastSpeedLow{};
			uint64_t lastLimitSamples{};
			uint64_t lastRecordUs{}, lastRecordedCommands{}, lastRecordBridgeUs{};   // v13.62/64
		};

		inline State & state()
		{
			static State result;
			return result;
		}

		inline double ms( State const & s, LARGE_INTEGER a, LARGE_INTEGER b )
		{
			return s.freq.QuadPart ? double( b.QuadPart - a.QuadPart ) * 1000.0 / double( s.freq.QuadPart ) : 0.0;
		}


		inline void gpuFrame( State & s, ID3D11Device * device, ID3D11DeviceContext * context )
		{
			if ( s.gpuFailed || !device || !context ) return;
			auto & cur = s.gpu[s.gpuIndex];
			if ( s.gpuOpen )
			{
				context->End( cur.end );
				context->End( cur.disjoint );
				cur.pending = true;
				s.gpuIndex = ( s.gpuIndex + 1u ) % 6u;
			}
			// Read every finished slot.
			for ( auto & slot : s.gpu )
			{
				if ( !slot.pending ) continue;
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				if ( context->GetData( slot.disjoint, &dj, sizeof( dj ), D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ) continue;
				UINT64 t0{}, t1{};
				if ( context->GetData( slot.begin, &t0, sizeof( t0 ), D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ) continue;
				if ( context->GetData( slot.end, &t1, sizeof( t1 ), D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ) continue;
				slot.pending = false;
				if ( !dj.Disjoint && dj.Frequency && t1 > t0 ) s.gpuWindow.push_back( float( double( t1 - t0 ) * 1000.0 / double( dj.Frequency ) ) );
			}
			auto & next = s.gpu[s.gpuIndex];
			if ( next.pending ) { s.gpuOpen = false; return; }   // reader is behind: skip this frame
			if ( !next.disjoint )
			{
				D3D11_QUERY_DESC dd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0u }, td{ D3D11_QUERY_TIMESTAMP, 0u };
				if ( FAILED( device->CreateQuery( &dd, &next.disjoint ) ) || FAILED( device->CreateQuery( &td, &next.begin ) ) || FAILED( device->CreateQuery( &td, &next.end ) ) )
				{
					s.gpuFailed = true;
					return;
				}
			}
			context->Begin( next.disjoint );
			context->End( next.begin );
			s.gpuOpen = true;
		}

		inline void onPresent( double presentCallMs, ID3D11Device * device, ID3D11DeviceContext * context )
		{
			static bool const enabled = envflags::on( "RW_GPU_FRAME_TIMING", true );
			if ( !enabled ) return;
			auto & s = state();
			++cpuspeed::stats().presents;
			gpuFrame( s, device, context );
			LARGE_INTEGER now{};
			QueryPerformanceCounter( &now );
			if ( !s.freq.QuadPart )
			{
				cpuspeed::start();
				QueryPerformanceFrequency( &s.freq );
				s.start = s.last = s.windowStart = now;
				return;
			}
			auto interval = ms( s, s.last, now );
			s.last = now;
			s.window.push_back( float( interval ) );
			// v13.53: per-frame record for 1%/0.1% lows, percentiles, stutters, missed refreshes, CSV.
			framestats::onFrame( interval, presentCallMs, ms( s, s.start, now ) / 1000.0, g_bridgeExecuteUs.load(), cpuspeed::stats().nowSpeed.load() );
			++s.total;
			if ( interval > 50.0 ) ++s.over[0];
			if ( interval > 100.0 ) ++s.over[1];
			if ( interval > 250.0 ) ++s.over[2];
			if ( interval > 500.0 ) ++s.over[3];
			if ( interval > 250.0 && s.hitchLines < 150u )
			{
				++s.hitchLines;
				std::fprintf( stderr, "[RW-GPU-HITCH] t=%.1fs frame=%llu interval=%.0fms present=%.0fms\n"
					, ms( s, s.start, now ) / 1000.0, (unsigned long long)s.total, interval, presentCallMs );
			}
			if ( ms( s, s.windowStart, now ) >= 30000.0 && !s.window.empty() )
			{
				framestats::report( ms( s, s.start, now ) / 1000.0 );
				std::vector< float > sorted = s.window;
				std::sort( sorted.begin(), sorted.end() );
				double sum = 0.0;
				for ( auto v : sorted ) sum += v;
				auto pick = [&sorted]( double q ){ return sorted[ size_t( double( sorted.size() - 1u ) * q ) ]; };
				uint64_t requested = g_watchRequestedBytes.load(), copied = g_watchCopiedBytes.load(), skipped = g_watchSkippedUploads.load();
				double seconds = ms( s, s.windowStart, now ) / 1000.0;
				double gpuAvg = 0.0, gpuP95 = 0.0;
				if ( !s.gpuWindow.empty() )
				{
					std::sort( s.gpuWindow.begin(), s.gpuWindow.end() );
					for ( auto v : s.gpuWindow ) gpuAvg += v;
					gpuAvg /= double( s.gpuWindow.size() );
					gpuP95 = s.gpuWindow[ size_t( double( s.gpuWindow.size() - 1u ) * 0.95 ) ];
				}
				auto & cs = cpuspeed::stats();
				uint64_t const speedSamplesNow = cs.samples.load(), speedSumNow = cs.sumPercent.load(), speedLowNow = cs.lowSamples.load();
				uint64_t const speedSamples = speedSamplesNow - s.lastSpeedSamples, speedSum = speedSumNow - s.lastSpeedSum, speedLow = speedLowNow - s.lastSpeedLow;
				uint32_t const speedMin = cs.minPercent.exchange( 1000u );
				s.lastSpeedSamples = speedSamplesNow; s.lastSpeedSum = speedSumNow; s.lastSpeedLow = speedLowNow;
				uint64_t const limitNow = cs.limitSamples.load();
				uint64_t const limitDelta = limitNow - s.lastLimitSamples;
				s.lastLimitSamples = limitNow;
				uint32_t const maxLimitNow = cs.maxLimit.exchange( 0u );
				uint32_t minMhzNow = cs.minMhz.exchange( 100000u );
				if ( minMhzNow == 100000u ) minMhzNow = 0u;
				char tempText[32];
				if ( cs.tempAvailable ) std::snprintf( tempText, sizeof( tempText ), "%u C", unsigned( cs.maxTempC.exchange( 0u ) ) );
				else std::snprintf( tempText, sizeof( tempText ), "not exposed" );
				uint64_t bridgeUs = g_bridgeExecuteUs.load();
				double bridgeMs = double( bridgeUs - s.lastBridgeUs ) / 1000.0 / double( sorted.size() );
				s.lastBridgeUs = bridgeUs;
				uint64_t const cpuNow = g_bridgeExecuteCpuUs.load(), execNow = g_bridgeExecutions.load(), slowNow = g_bridgeSlowExecutions.load();
				double const cpuMsFrame = double( cpuNow - s.lastCpuUs ) / 1000.0 / double( sorted.size() );
				double const execPerFrame = double( execNow - s.lastExecutions ) / double( sorted.size() );
				unsigned long long const slowCount = slowNow - s.lastSlow;
				s.lastCpuUs = cpuNow; s.lastExecutions = execNow; s.lastSlow = slowNow;
				s.lastWaitUsNext = framepacing::g_waitUs.load();
				std::fprintf( stderr, "[RW-GPU-FRAMES] t=%.0fs frames=%zu fps=%.1f avg=%.1fms p50=%.1fms p95=%.1fms p99=%.1fms max=%.0fms | per frame: paced wait=%.1fms bridge=%.1fms (thread CPU %.1fms, %.1f cmd buffers, %llu over 100 ms) GPU span avg=%.1fms p95=%.1fms (%zu timed) | session >50ms=%llu >100ms=%llu >250ms=%llu >500ms=%llu | uploads asked=%.1fMB/s copied=%.1fMB/s unchanged-skipped=%llu watch-query=%.1fms/frame | CPU speed avg=%.0f%% min=%u%% below-40%%=%llu of %llu samples | firmware limit (10%%+ below nominal) in %llu samples (deepest -%u%%) lowest clock %u MHz | max temperature %s | heat guard %s (engaged %llu times), throttled %llu s so far | cleanup actions kept=%llu skipped=%llu, push constants uploaded=%llu unchanged=%llu, UAV alias copies=%llu, repeat uploads not recorded=%llu, recording=%.1fms/frame (bridge part %.1fms, %.0f commands/frame), descriptor sets fast=%llu full=%llu, submit thread %s (jobs=%llu drains=%llu drain-wait=%.0fms present-wait=%.0fms) (session)\n"
					, ms( s, s.start, now ) / 1000.0, sorted.size(), double( sorted.size() ) / seconds, sum / double( sorted.size() )
					, pick( 0.5 ), pick( 0.95 ), pick( 0.99 ), sorted.back()
					, double( framepacing::g_waitUs.load() - s.lastWaitUs ) / 1000.0 / double( sorted.size() )
					, bridgeMs, cpuMsFrame, execPerFrame, slowCount
					, gpuAvg, gpuP95, s.gpuWindow.size()
					, (unsigned long long)s.over[0], (unsigned long long)s.over[1], (unsigned long long)s.over[2], (unsigned long long)s.over[3]
					, double( requested - s.lastRequested ) / 1048576.0 / seconds, double( copied - s.lastCopied ) / 1048576.0 / seconds
					, (unsigned long long)( skipped - s.lastSkipped )
					, double( g_watchQueryUs.load() - s.lastWatchUs ) / 1000.0 / double( sorted.size() )
					, speedSamples ? double( speedSum ) / double( speedSamples ) : 0.0, speedMin, (unsigned long long)speedLow, (unsigned long long)speedSamples
					, (unsigned long long)limitDelta, maxLimitNow, minMhzNow, tempText
					, cpuspeed::envFlag( "RW_GPU_THERMAL_GUARD", false ) ? ( cs.guardActive ? "active" : "armed" ) : "off"
					, (unsigned long long)cs.guardEngagements.load(), (unsigned long long)cs.throttledSeconds.load()
					, (unsigned long long)g_cleanupKept.load(), (unsigned long long)g_cleanupSkipped.load()
					, (unsigned long long)g_pushUploads.load(), (unsigned long long)g_pushSkipped.load()
					, (unsigned long long)uavalias::g_aliasCopies.load()
					, (unsigned long long)CommandBuffer::m_recordedUploadsSkipped.load()
					, double( g_recordUs.load() - s.lastRecordUs ) / 1000.0 / double( sorted.size() ), double( g_recordBridgeUs.load() - s.lastRecordBridgeUs ) / 1000.0 / double( sorted.size() ), double( g_recordedCommands.load() - s.lastRecordedCommands ) / double( sorted.size() )
					, (unsigned long long)g_setFast.load(), (unsigned long long)g_setFull.load()
					, submitthread::enabled() ? "on" : "off", (unsigned long long)submitthread::g_jobs.load(), (unsigned long long)submitthread::g_drains.load()
					, double( submitthread::g_drainWaitUs.load() ) / 1000.0, double( submitthread::g_presentWaitUs.load() ) / 1000.0 );
				cmdstats::printAndReset( sorted.size() );   // v13.66
				apitime::printAndReset( sorted.size() );   // v13.68
				std::fprintf( stderr, "[RW-GPU-IA] index-buffer binds (session): D3D11 calls=%llu unchanged=%llu offset folded into firstIndex=%llu; sub-page uploads without write-watch query=%llu; commands freed in the background=%llu; descriptor sets bound by content match=%llu; uploads merged at recording=%llu; uniform ring: windows=%llu fallbacks=%llu maps=%llu wraps=%llu; index pool: hits=%llu copies=%llu restarts=%llu%s\n"
					, (unsigned long long)g_indexBinds.load(), (unsigned long long)g_indexSame.load(), (unsigned long long)g_indexFolded.load(), (unsigned long long)g_subpageDirect.load(), (unsigned long long)reaper::g_deferred.load(), (unsigned long long)g_setContentFast.load(), (unsigned long long)g_uploadsMerged.load(), (unsigned long long)ubring::g_windows.load(), (unsigned long long)ubring::g_fallbacks.load(), (unsigned long long)ubring::g_maps.load(), (unsigned long long)ubring::g_discards.load(), (unsigned long long)idxpool::g_hits.load(), (unsigned long long)idxpool::g_copies.load(), (unsigned long long)idxpool::g_resets.load(), idxpool::g_disabled.load() ? " (switched off: did not fit)" : "" );
				s.lastWatchUs = g_watchQueryUs.load();
				s.lastRecordUs = g_recordUs.load(); s.lastRecordedCommands = g_recordedCommands.load(); s.lastRecordBridgeUs = g_recordBridgeUs.load();   // v13.62/64
				s.lastRequested = requested; s.lastCopied = copied; s.lastSkipped = skipped;
				s.lastWaitUs = s.lastWaitUsNext;
				s.window.clear();
				s.gpuWindow.clear();
				s.windowStart = now;
			}
		}
	}
}

namespace ashes::D3D11_NAMESPACE
{
	namespace
	{
		// This is deliberately a per-swapchain, opt-in diagnostic.  It must not
		// create a normal-frame synchronization point or retain a global resource.
		namespace frameprobe
		{
			static constexpr uint64_t FnvOffset = 1469598103934665603ull;
			static constexpr uint64_t FnvPrime = 1099511628211ull;
			static constexpr UINT SampleAxis = 5u;
			static constexpr UINT MaxWaitMillis = 250u;
			static constexpr uint64_t MaxExactCompareBytes = 1024ull * 1024ull;
			static constexpr uint64_t ProbeIntervalMillis = 10000u;
			static constexpr uint32_t MaxTimedCaptures = 12u;

			struct Sample
			{
				bool mapped{};
				HRESULT hr{ E_FAIL };
				uint32_t sampleCount{};
				uint32_t nonZeroBytes{};
				uint64_t colourHash{ FnvOffset };
				std::vector< uint8_t > fullFrame;
			};

			struct Capture
			{
				~Capture()noexcept
				{
					safeRelease( fakeReadback );
					safeRelease( realReadback );
					safeRelease( completion );
				}

				D3D11_TEXTURE2D_DESC fakeDesc{};
				D3D11_TEXTURE2D_DESC realDesc{};
				D3D11_TEXTURE2D_DESC fakeReadbackDesc{};
				D3D11_TEXTURE2D_DESC realReadbackDesc{};
				D3D11_BOX copyBox{};
				ID3D11Texture2D * fakeReadback{};
				ID3D11Texture2D * realReadback{};
				ID3D11Query * completion{};
				HRESULT fakeCreateHr{ E_FAIL };
				HRESULT realCreateHr{ E_FAIL };
				HRESULT queryCreateHr{ E_FAIL };
				HRESULT waitHr{ E_FAIL };
				bool descriptors{};
				bool copyCompatible{};
				bool fullFakeReadback{};
				bool fullRealReadback{};
				bool armed{};
				bool completed{};
				bool deviceLost{};
				bool exactCompared{};
				bool exactEqual{};
				Sample pre;
				Sample post;
			};

			static bool isEnabled()
			{
			#if defined( Ashes_D3D11_XBox )
				return false;
			#else
				char value[ 4u ]{};
				auto const length = ::GetEnvironmentVariableA( "RW_HD5500_FRAME_PROBE"
					, value
					, DWORD( sizeof( value ) ) );
				return length == 1u && value[ 0 ] == '1';
			#endif
			}

			static uint64_t getTickMillis()
			{
				return uint64_t( std::chrono::duration_cast< std::chrono::milliseconds >(
					std::chrono::steady_clock::now().time_since_epoch() ).count() );
			}

			static bool shouldCapture( uint64_t frame
				, uint64_t now
				, uint64_t & nextTick
				, uint32_t & timedCaptureCount )
			{
				if ( frame <= 3u )
				{
					if ( frame == 3u )
					{
						nextTick = now + ProbeIntervalMillis;
					}

					return true;
				}

				if ( timedCaptureCount < MaxTimedCaptures && now >= nextTick )
				{
					// Advance from now rather than catching up: prolonged loading remains
					// bounded to one synchronous capture per ten seconds.
					nextTick = now + ProbeIntervalMillis;
					timedCaptureCount += 1u;
					return true;
				}

				return false;
			}

			static bool isDeviceLoss( HRESULT hr )
			{
				return hr == DXGI_ERROR_DEVICE_REMOVED
					|| hr == DXGI_ERROR_DEVICE_RESET
					|| hr == DXGI_ERROR_DEVICE_HUNG
					|| hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
			}

			static std::string asHResult( HRESULT hr )
			{
				std::stringstream stream;
				stream.imbue( std::locale{ "C" } );
				stream << "0x" << std::hex << std::uppercase << uint32_t( hr );
				return stream.str();
			}

			static std::string describe( D3D11_TEXTURE2D_DESC const & desc )
			{
				std::stringstream stream;
				stream.imbue( std::locale{ "C" } );
				stream << "w=" << desc.Width
					<< ",h=" << desc.Height
					<< ",fmt=" << uint32_t( desc.Format )
					<< ",mips=" << desc.MipLevels
					<< ",array=" << desc.ArraySize
					<< ",samples=" << desc.SampleDesc.Count << "/" << desc.SampleDesc.Quality
					<< ",usage=" << uint32_t( desc.Usage )
					<< ",bind=0x" << std::hex << desc.BindFlags << std::dec;
				return stream.str();
			}

			static UINT getPixelBytes( DXGI_FORMAT format )
			{
				switch ( format )
				{
				case DXGI_FORMAT_R8_UNORM:
				case DXGI_FORMAT_R8_UINT:
				case DXGI_FORMAT_R8_SNORM:
				case DXGI_FORMAT_R8_SINT:
				case DXGI_FORMAT_A8_UNORM:
					return 1u;
				case DXGI_FORMAT_R8G8_UNORM:
				case DXGI_FORMAT_R8G8_UINT:
				case DXGI_FORMAT_R8G8_SNORM:
				case DXGI_FORMAT_R8G8_SINT:
				case DXGI_FORMAT_R16_FLOAT:
				case DXGI_FORMAT_R16_UNORM:
				case DXGI_FORMAT_R16_UINT:
				case DXGI_FORMAT_R16_SNORM:
				case DXGI_FORMAT_R16_SINT:
				case DXGI_FORMAT_B5G6R5_UNORM:
				case DXGI_FORMAT_B5G5R5A1_UNORM:
					return 2u;
				// v13.3: swapchain images are created TYPELESS (e.g. format 90 =
				// B8G8R8A8_TYPELESS). Without these cases the frame probe refused to
				// create its staging copy (E_INVALIDARG) and never captured a frame.
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8X8_TYPELESS:
				case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_R8G8B8A8_UINT:
				case DXGI_FORMAT_R8G8B8A8_SNORM:
				case DXGI_FORMAT_R8G8B8A8_SINT:
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				case DXGI_FORMAT_B8G8R8X8_UNORM:
				case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
				case DXGI_FORMAT_R10G10B10A2_UNORM:
				case DXGI_FORMAT_R10G10B10A2_UINT:
				case DXGI_FORMAT_R11G11B10_FLOAT:
				case DXGI_FORMAT_R16G16_FLOAT:
				case DXGI_FORMAT_R16G16_UNORM:
				case DXGI_FORMAT_R16G16_UINT:
				case DXGI_FORMAT_R16G16_SNORM:
				case DXGI_FORMAT_R16G16_SINT:
				case DXGI_FORMAT_R32_FLOAT:
				case DXGI_FORMAT_R32_UINT:
				case DXGI_FORMAT_R32_SINT:
					return 4u;
				case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				case DXGI_FORMAT_R16G16B16A16_UNORM:
				case DXGI_FORMAT_R16G16B16A16_UINT:
				case DXGI_FORMAT_R16G16B16A16_SNORM:
				case DXGI_FORMAT_R16G16B16A16_SINT:
				case DXGI_FORMAT_R32G32_FLOAT:
				case DXGI_FORMAT_R32G32_UINT:
				case DXGI_FORMAT_R32G32_SINT:
					return 8u;
				case DXGI_FORMAT_R32G32B32A32_FLOAT:
				case DXGI_FORMAT_R32G32B32A32_UINT:
				case DXGI_FORMAT_R32G32B32A32_SINT:
					return 16u;
				default:
					return 0u;
				}
			}

			static uint64_t hashBytes( uint64_t hash, uint8_t const * bytes, size_t size )
			{
				for ( size_t index = 0u; index < size; ++index )
				{
					hash ^= bytes[ index ];
					hash *= FnvPrime;
				}

				return hash;
			}

			// Formats in the same typeless group may legally be copied into each
			// other with CopySubresourceRegion (TYPELESS fake image -> UNORM backbuffer).
			static DXGI_FORMAT getCopyFamily( DXGI_FORMAT format )
			{
				switch ( format )
				{
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_R8G8B8A8_UINT:
				case DXGI_FORMAT_R8G8B8A8_SNORM:
				case DXGI_FORMAT_R8G8B8A8_SINT:
					return DXGI_FORMAT_R8G8B8A8_TYPELESS;
				case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					return DXGI_FORMAT_B8G8R8A8_TYPELESS;
				case DXGI_FORMAT_B8G8R8X8_TYPELESS:
				case DXGI_FORMAT_B8G8R8X8_UNORM:
				case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
					return DXGI_FORMAT_B8G8R8X8_TYPELESS;
				case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				case DXGI_FORMAT_R10G10B10A2_UNORM:
				case DXGI_FORMAT_R10G10B10A2_UINT:
					return DXGI_FORMAT_R10G10B10A2_TYPELESS;
				case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				case DXGI_FORMAT_R16G16B16A16_UNORM:
				case DXGI_FORMAT_R16G16B16A16_UINT:
				case DXGI_FORMAT_R16G16B16A16_SNORM:
				case DXGI_FORMAT_R16G16B16A16_SINT:
					return DXGI_FORMAT_R16G16B16A16_TYPELESS;
				default:
					return format;
				}
			}

			static bool isCopyCompatible( D3D11_TEXTURE2D_DESC const & fakeDesc
				, D3D11_TEXTURE2D_DESC const & realDesc
				, D3D11_BOX const & box )
			{
				return getCopyFamily( fakeDesc.Format ) == getCopyFamily( realDesc.Format )
					&& fakeDesc.SampleDesc.Count == realDesc.SampleDesc.Count
					&& fakeDesc.SampleDesc.Quality == realDesc.SampleDesc.Quality
					&& fakeDesc.MipLevels > 0u
					&& fakeDesc.ArraySize > 0u
					&& realDesc.MipLevels > 0u
					&& realDesc.ArraySize > 0u
					&& box.left <= box.right
					&& box.top <= box.bottom
					&& box.right <= fakeDesc.Width
					&& box.bottom <= fakeDesc.Height
					&& ( box.right - box.left ) <= realDesc.Width
					&& ( box.bottom - box.top ) <= realDesc.Height;
			}

			static bool useFullReadback( D3D11_TEXTURE2D_DESC const & desc )
			{
				auto const pixelBytes = getPixelBytes( desc.Format );
				return pixelBytes
					&& desc.SampleDesc.Count == 1u
					&& desc.MipLevels == 1u
					&& desc.ArraySize == 1u
					&& uint64_t( desc.Width ) * desc.Height * pixelBytes <= MaxExactCompareBytes;
			}

			static HRESULT createReadback( ID3D11Device * device
				, D3D11_TEXTURE2D_DESC const & sourceDesc
				, bool fullReadback
				, D3D11_TEXTURE2D_DESC & readbackDesc
				, ID3D11Texture2D *& result )
			{
				if ( !device || sourceDesc.SampleDesc.Count != 1u
					|| !getPixelBytes( sourceDesc.Format ) )
				{
					return E_INVALIDARG;
				}

				readbackDesc = sourceDesc;

				if ( !fullReadback )
				{
					readbackDesc.Width = SampleAxis;
					readbackDesc.Height = SampleAxis;
					readbackDesc.MipLevels = 1u;
					readbackDesc.ArraySize = 1u;
				}

				readbackDesc.Usage = D3D11_USAGE_STAGING;
				readbackDesc.BindFlags = 0u;
				readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				readbackDesc.MiscFlags = 0u;
				return device->CreateTexture2D( &readbackDesc, nullptr, &result );
			}

			static void copyToReadback( ID3D11DeviceContext * context
				, ID3D11Texture2D * readback
				, ID3D11Texture2D * source
				, D3D11_TEXTURE2D_DESC const & sourceDesc
				, bool fullReadback )
			{
				if ( fullReadback )
				{
					context->CopyResource( readback, source );
					return;
				}

				for ( UINT y = 0u; y < SampleAxis; ++y )
				{
					auto const sourceY = sourceDesc.Height <= 1u
						? 0u
						: UINT( uint64_t( sourceDesc.Height - 1u ) * y / ( SampleAxis - 1u ) );

					for ( UINT x = 0u; x < SampleAxis; ++x )
					{
						auto const sourceX = sourceDesc.Width <= 1u
							? 0u
							: UINT( uint64_t( sourceDesc.Width - 1u ) * x / ( SampleAxis - 1u ) );
						D3D11_BOX sourceBox{ sourceX, sourceY, 0u, sourceX + 1u, sourceY + 1u, 1u };
						context->CopySubresourceRegion( readback, 0u, x, y, 0u, source, 0u, &sourceBox );
					}
				}
			}

			static Sample readSample( ID3D11DeviceContext * context
				, ID3D11Texture2D * texture
				, D3D11_TEXTURE2D_DESC const & desc )
			{
				Sample result;
				auto const pixelBytes = getPixelBytes( desc.Format );

				if ( !context || !texture || !pixelBytes )
				{
					result.hr = E_INVALIDARG;
					return result;
				}

				D3D11_MAPPED_SUBRESOURCE mapped{};
				result.hr = context->Map( texture, 0u, D3D11_MAP_READ, 0u, &mapped );

				if ( FAILED( result.hr ) || !mapped.pData )
				{
					if ( SUCCEEDED( result.hr ) )
					{
						result.hr = E_FAIL;
					}
					return result;
				}

				auto const rowBytes = uint64_t( desc.Width ) * pixelBytes;

				if ( rowBytes > mapped.RowPitch )
				{
					result.hr = E_INVALIDARG;
					context->Unmap( texture, 0u );
					return result;
				}

				result.mapped = true;
				auto const * base = static_cast< uint8_t const * >( mapped.pData );

				for ( UINT y = 0u; y < SampleAxis; ++y )
				{
					auto const sampleY = desc.Height <= 1u
						? 0u
						: UINT( uint64_t( desc.Height - 1u ) * y / ( SampleAxis - 1u ) );

					for ( UINT x = 0u; x < SampleAxis; ++x )
					{
						auto const sampleX = desc.Width <= 1u
							? 0u
							: UINT( uint64_t( desc.Width - 1u ) * x / ( SampleAxis - 1u ) );
						auto const * pixel = base + uint64_t( sampleY ) * mapped.RowPitch
							+ uint64_t( sampleX ) * pixelBytes;
						result.colourHash = hashBytes( result.colourHash, pixel, pixelBytes );
						result.sampleCount += 1u;

						for ( UINT component = 0u; component < pixelBytes; ++component )
						{
							result.nonZeroBytes += pixel[ component ] != 0u ? 1u : 0u;
						}
					}
				}

				auto const fullFrameBytes = rowBytes * desc.Height;

				if ( fullFrameBytes <= MaxExactCompareBytes )
				{
					result.fullFrame.resize( size_t( fullFrameBytes ) );

					for ( UINT row = 0u; row < desc.Height; ++row )
					{
						std::memcpy( result.fullFrame.data() + size_t( uint64_t( row ) * rowBytes )
							, base + uint64_t( row ) * mapped.RowPitch
							, size_t( rowBytes ) );
					}
				}

				context->Unmap( texture, 0u );
				return result;
			}

			static void begin( Capture & capture
				, VkDevice device
				, ID3D11DeviceContext * context
				, ID3D11Texture2D * fake
				, ID3D11Texture2D * real
				, D3D11_BOX const & box )
			{
				capture.copyBox = box;

				if ( !context || !fake || !real )
				{
					return;
				}

				fake->GetDesc( &capture.fakeDesc );
				real->GetDesc( &capture.realDesc );
				capture.descriptors = true;
				capture.copyCompatible = isCopyCompatible( capture.fakeDesc, capture.realDesc, box );
				auto nativeDevice = get( device )->getDevice();
				capture.fullFakeReadback = useFullReadback( capture.fakeDesc );
				capture.fullRealReadback = useFullReadback( capture.realDesc );
				capture.fakeCreateHr = createReadback( nativeDevice
					, capture.fakeDesc
					, capture.fullFakeReadback
					, capture.fakeReadbackDesc
					, capture.fakeReadback );
				capture.realCreateHr = createReadback( nativeDevice
					, capture.realDesc
					, capture.fullRealReadback
					, capture.realReadbackDesc
					, capture.realReadback );

				if ( FAILED( capture.fakeCreateHr ) || FAILED( capture.realCreateHr )
					|| !capture.fakeReadback || !capture.realReadback )
				{
					return;
				}

				D3D11_QUERY_DESC queryDesc{};
				queryDesc.Query = D3D11_QUERY_EVENT;
				capture.queryCreateHr = nativeDevice->CreateQuery( &queryDesc, &capture.completion );

				if ( FAILED( capture.queryCreateHr ) || !capture.completion )
				{
					return;
				}

				// Staging copies are read-only with respect to the fake swapchain image.
				copyToReadback( context, capture.fakeReadback, fake, capture.fakeDesc, capture.fullFakeReadback );
				capture.armed = true;
			}

			static void finish( Capture & capture
				, VkDevice device
				, ID3D11DeviceContext * context
				, ID3D11Texture2D * real )
			{
				if ( !capture.armed || !context || !real )
				{
					return;
				}

				// This observes the native backbuffer after the existing fake-to-real
				// copy.  It never writes either render target.
				copyToReadback( context, capture.realReadback, real, capture.realDesc, capture.fullRealReadback );
				context->End( capture.completion );
				context->Flush(); // Probe-only: no normal frame flush or polling.

				capture.waitHr = S_FALSE;

				for ( UINT elapsed = 0u; elapsed < MaxWaitMillis; ++elapsed )
				{
					capture.waitHr = context->GetData( capture.completion
						, nullptr
						, 0u
						, D3D11_ASYNC_GETDATA_DONOTFLUSH );

					if ( capture.waitHr == S_OK || FAILED( capture.waitHr ) )
					{
						break;
					}

					::Sleep( 1u );
				}

				capture.completed = capture.waitHr == S_OK;
				capture.deviceLost = isDeviceLoss( capture.waitHr )
					|| get( device )->getStatus() == VK_ERROR_DEVICE_LOST;

				if ( !capture.completed )
				{
					return;
				}

				capture.pre = readSample( context, capture.fakeReadback, capture.fakeReadbackDesc );
				capture.post = readSample( context, capture.realReadback, capture.realReadbackDesc );
				capture.deviceLost = capture.deviceLost
					|| isDeviceLoss( capture.pre.hr )
					|| isDeviceLoss( capture.post.hr )
					|| get( device )->getStatus() == VK_ERROR_DEVICE_LOST;

				if ( capture.copyCompatible
					&& capture.fullFakeReadback && capture.fullRealReadback
					&& capture.pre.mapped && capture.post.mapped
					&& !capture.pre.fullFrame.empty()
					&& capture.pre.fullFrame.size() == capture.post.fullFrame.size() )
				{
					capture.exactCompared = true;
					capture.exactEqual = capture.pre.fullFrame == capture.post.fullFrame;
				}
			}

			static void report( Capture const & capture
				, uint64_t frame
				, VkExtent2D const & logicalExtent
				, VkExtent2D const & wrapperExtent
				, HRESULT presentHr )
			{
				std::clog << "RW_HD5500_FRAME_PROBE"
					<< " frame=" << frame
					<< " pre{mapped=" << uint32_t( capture.pre.mapped )
					<< ",hr=" << asHResult( capture.pre.hr )
					<< ",samples=" << capture.pre.sampleCount
					<< ",nonzeroBytes=" << capture.pre.nonZeroBytes
					<< ",colourHash=0x" << std::hex << std::uppercase << uint64_t( capture.pre.colourHash ) << std::dec
					<< "} post{mapped=" << uint32_t( capture.post.mapped )
					<< ",hr=" << asHResult( capture.post.hr )
					<< ",samples=" << capture.post.sampleCount
					<< ",nonzeroBytes=" << capture.post.nonZeroBytes
					<< ",colourHash=0x" << std::hex << std::uppercase << uint64_t( capture.post.colourHash ) << std::dec
					<< "} exact{compared=" << uint32_t( capture.exactCompared )
					<< ",equal=" << uint32_t( capture.exactEqual )
					<< "} readback{pre=" << ( capture.fullFakeReadback ? "full" : "sampled" )
					<< ",post=" << ( capture.fullRealReadback ? "full" : "sampled" )
					<< "} copy{armed=" << uint32_t( capture.armed )
					<< ",completed=" << uint32_t( capture.completed )
					<< ",compatible=" << uint32_t( capture.copyCompatible )
					<< ",box=" << capture.copyBox.left << "," << capture.copyBox.top
					<< "," << capture.copyBox.right << "," << capture.copyBox.bottom
					<< ",logical=" << logicalExtent.width << "x" << logicalExtent.height
					<< ",wrapper=" << wrapperExtent.width << "x" << wrapperExtent.height;

				if ( capture.descriptors )
				{
					std::clog << ",fakeDesc={" << describe( capture.fakeDesc )
						<< "},realDesc={" << describe( capture.realDesc ) << "}";
				}

				std::clog << ",fakeCreateHr=" << asHResult( capture.fakeCreateHr )
					<< ",realCreateHr=" << asHResult( capture.realCreateHr )
					<< ",queryCreateHr=" << asHResult( capture.queryCreateHr )
					<< ",waitHr=" << asHResult( capture.waitHr )
					<< ",deviceLost=" << uint32_t( capture.deviceLost )
					<< "} presentHr=" << asHResult( presentHr )
					<< "\n";
			}
		}
	}

	namespace swapchain
	{
		static VkImage createImage( VkDevice device
			, VkFormat format
			, VkExtent2D dimensions
			, VkDeviceMemory & deviceMemory )
		{
			VkImage result;
			allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, format
				, std::move( dimensions )
				, nullptr );
			auto requirements = get( result )->getMemoryRequirements();
			uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
				, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
			allocate( deviceMemory
				, get( device )->getAllocationCallbacks()
				, device
				, VkMemoryAllocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, deduced } );
			get( result )->bindMemory( deviceMemory, 0u );
			return result;
		}

		static VkImageView createImageView( VkDevice device
			, VkImage image
			, VkFormat format )
		{
			VkImageView result;
			allocate( result
				, get( device )->getAllocationCallbacks()
				, device
				, VkImageViewCreateInfo
				{
					VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					nullptr,
					0,
					image,
					VK_IMAGE_VIEW_TYPE_2D,
					format,
					VkComponentMapping{},
					VkImageSubresourceRange{ VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
				} );
			return result;
		}
	}

	SwapchainKHR::SwapchainKHR( VkDevice device
		, VkSwapchainCreateInfoKHR createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
	{
		try
		{
			// v13.60: flip presentation model (DXGI_SWAP_EFFECT_FLIP_DISCARD, Windows 10) instead of
			// the legacy "blt" model. With blt, every frame is copied once more into the window's
			// redirection surface before the desktop compositor uses it; with flip the compositor
			// uses the back buffer directly (one full-screen copy less per frame, shorter path to
			// the screen = less input delay; borderless fullscreen can be shown without the
			// compositor). RW_GPU_FLIP_MODEL=0 restores the old model; if Windows refuses a flip
			// swapchain the old model is used automatically.
			static bool const flipWanted = envflags::on( "RW_GPU_FLIP_MODEL", true );
			bool const flip = flipWanted && !get( m_createInfo.surface )->isDisplay();
			// v13.60c: a window keeps its flip swapchain; a replacement (resize) takes it over and
			// resizes it (DXGI's intended way) - creating a second flip swapchain for the same
			// window is refused (E_ACCESSDENIED) while the old one still exists.
			IDXGISwapChain * inherited{};
			if ( flip && m_createInfo.oldSwapchain )
			{
				inherited = get( m_createInfo.oldSwapchain )->takeFlipSwapChain();
				if ( !inherited ) get( m_createInfo.oldSwapchain )->retireNative();
			}
			doInitPresentParameters( flip );
			auto factory = get( get( m_device )->getInstance() )->getDXGIFactory();
			auto d3ddevice = get( m_device )->getDevice();
			if ( inherited && m_flipModel )
			{
				HRESULT rhr = inherited->ResizeBuffers( m_presentDesc.BufferCount
					, m_presentDesc.BufferDesc.Width, m_presentDesc.BufferDesc.Height
					, m_presentDesc.BufferDesc.Format, m_presentDesc.Flags );
				if ( SUCCEEDED( rhr ) )
				{
					m_swapChain = inherited;
					inherited = nullptr;
					std::fprintf( stderr, "[RW-GPU-WSI] flip swapchain taken over from the replaced swapchain and resized to %ux%u\n"
						, unsigned( m_presentDesc.BufferDesc.Width ), unsigned( m_presentDesc.BufferDesc.Height ) );
				}
				else
				{
					std::fprintf( stderr, "[RW-GPU-WSI] ResizeBuffers on the inherited flip swapchain failed (hr=0x%08lX)\n", static_cast< unsigned long >( rhr ) );
				}
			}
			if ( inherited )
			{
				safeRelease( inherited );
				try { auto context = get( m_device )->getImmediateContext(); context->ClearState(); context->Flush(); } catch ( ... ) {}
			}
#if defined( Ashes_D3D11_XBox )

			auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
			HRESULT hr = static_cast< IDXGIFactory2 * >( factory )->CreateSwapChainForCoreWindow( d3ddevice
				, static_cast< IUnknown * >( winrt::get_abi( window ) )
				, &m_presentDesc
				, nullptr
				, reinterpret_cast< IDXGISwapChain1 ** >( &m_swapChain ) );

			if ( !checkError( m_device, hr, "CreateSwapChain" )
				|| !m_swapChain )
			{
				throw ashes::BaseException{ "Could not create the swapchain" };
			}

#else

			HRESULT hr = m_swapChain ? S_OK : factory->CreateSwapChain( d3ddevice
				, &m_presentDesc
				, &m_swapChain );
			if ( m_flipModel && ( FAILED( hr ) || !m_swapChain ) )
			{
				std::fprintf( stderr, "[RW-GPU-WSI] flip-model swapchain refused (hr=0x%08lX), using the legacy blt model\n", static_cast< unsigned long >( hr ) );
				safeRelease( m_swapChain );
				doInitPresentParameters( false );
				hr = factory->CreateSwapChain( d3ddevice
					, &m_presentDesc
					, &m_swapChain );
			}
			{
				static std::atomic< uint32_t > logged{ 0u };
				if ( SUCCEEDED( hr ) && logged++ < 4u )
				{
					std::fprintf( stderr, "[RW-GPU-WSI] swapchain %ux%u, presentation model %s (%u buffers, format %d)\n"
						, unsigned( m_presentDesc.BufferDesc.Width ), unsigned( m_presentDesc.BufferDesc.Height )
						, m_flipModel ? "flip-discard" : "blt (legacy)", unsigned( m_presentDesc.BufferCount ), int( m_presentDesc.BufferDesc.Format ) );
				}
			}

			if ( !checkError( m_device, hr, "CreateSwapChain" )
				|| !m_swapChain )
			{
				throw ashes::BaseException{ "Could not create the swapchain" };
			}

			if ( get( m_createInfo.surface )->isDisplay() )
			{
				hr = m_swapChain->SetFullscreenState( TRUE, get( get( device )->getPhysicalDevice() )->getOutput() );

				if ( !checkError( m_device, hr, "SetFullscreenState" ) )
				{
					throw ashes::BaseException{ "Could not set the swapchain to fullscreen mode" };
				}

				hr = m_swapChain->ResizeTarget( &m_displayMode );

				if ( !checkError( m_device, hr, "ResizeTarget" ) )
				{
					m_swapChain->SetFullscreenState( FALSE, get( get( device )->getPhysicalDevice() )->getOutput() );
					throw ashes::BaseException{ "Could not resize the swapchain" };
				}
			}

#endif
			dxDebugName( m_swapChain, SwapChain );
			ID3D11Texture2D * rtTex = nullptr;
			hr = m_swapChain->GetBuffer( 0
				, __uuidof( ID3D11Texture2D )
				, reinterpret_cast< void ** >( &rtTex ) );

			if ( !checkError( m_device, hr, "SwapChain::GetBuffer" ) )
			{
				throw ashes::BaseException( "GetBuffer() failed" );
			}

			m_swapchainExtent = VkExtent2D{ m_displayMode.Width, m_displayMode.Height };
			allocate( m_swapChainImage
				, get( device )->getAllocationCallbacks()
				, m_device
				, m_createInfo.imageFormat
				, m_swapchainExtent
				, rtTex );
			dxDebugName( get( m_swapChainImage )->getResource(), SwapChainImage );

			m_windowExtent = m_createInfo.imageExtent;
			auto const imageCount = std::min( std::max( m_createInfo.minImageCount, 1u )
				, framediag::swapchainImageSetting() );

			for ( uint32_t index = 0u; index < imageCount; ++index )
			{
				VkDeviceMemory memory{};
				auto image = swapchain::createImage( device
					, m_createInfo.imageFormat
					, m_windowExtent
					, memory );
				m_images.push_back( image );
				m_deviceMemories.push_back( memory );
				dxDebugName( get( image )->getResource(), SwapChainFakeImage );
				auto view = swapchain::createImageView( device
					, image
					, m_createInfo.imageFormat );
				m_views.push_back( view );
				dxDebugName( get( view )->getRenderTargetView(), SwapChainFakeImageView );
				framediag::registerSwapImage( get( image )->getResource() );
			}
			m_frameProbeEnabled = frameprobe::isEnabled();

			if ( m_frameProbeEnabled )
			{
				std::clog << "RW_HD5500_FRAME_PROBE enabled captures=first3+12x10sec"
					<< " mode=read-only-staging\n";
			}
		}
		catch ( std::exception & exc )
		{
			doReleaseImages();
			deallocate( m_swapChainImage, get( m_device )->getAllocationCallbacks() );
			safeRelease( m_swapChain );

			std::stringstream stream;
			stream << "Swapchain creation failed: " << exc.what() << std::endl;
			get( device )->onReportMessage( VK_DEBUG_REPORT_ERROR_BIT_EXT
				, VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT
				, uint64_t( device )
				, 0u
				, VK_ERROR_INCOMPATIBLE_DRIVER
				, "Direct3D11"
				, stream.str().c_str() );
			std::cerr << stream.str() << std::endl;
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, stream.str() };
		}
	}

	namespace
	{
		// v13.60c: the Image wrapper around the back buffer does not own (release) the texture it
		// got from GetBuffer(0) - that reference kept every replaced DXGI swapchain alive (a leak
		// per resize, and the reason a new flip swapchain was refused for the same window).
		void releaseBackBuffer( VkDevice device, VkImage & image )noexcept
		{
			if ( !image ) return;
			if ( auto texture = get( image )->getTexture2D() ) texture->Release();
			deallocate( image, get( device )->getAllocationCallbacks() );
			image = VK_NULL_HANDLE;
		}
	}

	IDXGISwapChain * SwapchainKHR::takeFlipSwapChain()noexcept
	{
		if ( !m_flipModel || !m_swapChain ) return nullptr;
		// The back buffer reference must be gone before ResizeBuffers.
		releaseBackBuffer( m_device, m_swapChainImage );
		try
		{
			auto context = get( m_device )->getImmediateContext();
			context->ClearState();
			context->Flush();
		}
		catch ( ... )
		{
		}
		auto result = m_swapChain;
		m_swapChain = nullptr;
		return result;
	}

	void SwapchainKHR::retireNative()noexcept
	{
		releaseBackBuffer( m_device, m_swapChainImage );
		safeRelease( m_swapChain );
		// D3D11 destroys objects lazily: the swapchain only really goes away (and frees the
		// window for a new flip-model swapchain) after the immediate context let go of every
		// reference and flushed. Without this the new swapchain was refused (E_ACCESSDENIED)
		// and the window kept showing the old swapchain's last frame (v13.60 laptop test).
		try
		{
			auto context = get( m_device )->getImmediateContext();
			context->ClearState();
			context->Flush();
		}
		catch ( ... )
		{
		}
	}

	SwapchainKHR::~SwapchainKHR()noexcept
	{
		doReleaseImages();
		releaseBackBuffer( m_device, m_swapChainImage );
		safeRelease( m_swapChain );
	}

	void SwapchainKHR::doReleaseImages()noexcept
	{
		for ( auto & view : m_views )
		{
			deallocate( view, get( m_device )->getAllocationCallbacks() );
		}

		for ( auto & image : m_images )
		{
			if ( image )
			{
				framediag::unregisterSwapImage( get( image )->getResource() );
			}

			deallocate( image, get( m_device )->getAllocationCallbacks() );
		}

		for ( auto & memory : m_deviceMemories )
		{
			deallocate( memory, get( m_device )->getAllocationCallbacks() );
		}

		m_views.clear();
		m_images.clear();
		m_deviceMemories.clear();
	}

	uint32_t SwapchainKHR::getImageCount()const
	{
		return uint32_t( m_images.size() );
	}

	VkImageArray SwapchainKHR::getImages()const
	{
		VkImageArray result;

		for ( auto & image : m_images )
		{
			result.emplace_back( image );
		}

		return result;
	}

	VkResult SwapchainKHR::present( uint32_t imageIndex )const
	{
		if ( !m_swapChain || !m_swapChainImage ) return VK_ERROR_OUT_OF_DATE_KHR;   // v13.60: retired
		if ( imageIndex >= m_images.size() )
		{
			return VK_ERROR_OUT_OF_DATE_KHR;
		}

		auto const & presentedImage = m_images[imageIndex];
		auto const t0 = apitime::now();   // v13.69b: present path split
		auto context{ get( m_device )->getImmediateContext() };
		auto const t1 = apitime::now();
		apitime::add( apitime::PLock, t0, t1 );
		D3D11_BOX srcBox{};
		srcBox.right = m_windowExtent.width;
		srcBox.bottom = m_windowExtent.height;
		srcBox.back = 1u;
		++m_presentCount;
		// v13.4: the probe schedule is global (first 3 presents of the process, then one per
		// 10 s), not per swapchain. With per-frame swapchain recreation the per-swapchain
		// schedule captured, and stalled, every single frame.
		static uint64_t globalPresents{};
		static uint64_t globalNextTick{};
		static uint32_t globalTimedCaptures{};
		auto const frame = ++globalPresents;
		auto const probeThisFrame = m_frameProbeEnabled
			&& frameprobe::shouldCapture( frame
				, frameprobe::getTickMillis()
				, globalNextTick
				, globalTimedCaptures );
		frameprobe::Capture probe;
		auto fake = get( presentedImage )->getTexture2D();
		framediag::onPresent( get( m_device )->getDevice()
			, *context
			, get( presentedImage )->getResource()
			, imageIndex
			, uint32_t( m_images.size() ) );
		auto real = get( m_swapChainImage )->getTexture2D();
		auto const t2 = apitime::now();
		apitime::add( apitime::PDiag, t1, t2 );

		if ( probeThisFrame )
		{
			frameprobe::begin( probe, m_device, *context, fake, real, srcBox );
		}

		context->CopySubresourceRegion( get( m_swapChainImage )->getResource()
			, 0u
			, 0u
			, 0u
			, 0u
			, get( presentedImage )->getResource()
			, 0u
			, &srcBox );

		if ( probeThisFrame )
		{
			frameprobe::finish( probe, m_device, *context, real );
		}

		LARGE_INTEGER presentBegin{}, presentEnd{}, presentFreq{};
		QueryPerformanceCounter( &presentBegin );
		apitime::add( apitime::PCopy, t2, presentBegin.QuadPart );
		auto const presentHr = getSwapChain()->Present( 0u, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u );   // v13.70
		QueryPerformanceCounter( &presentEnd );
		QueryPerformanceFrequency( &presentFreq );
		apitime::add( apitime::PCall, presentBegin.QuadPart, presentEnd.QuadPart );
		framepacing::afterPresent( get( m_device )->getDevice(), *context );
		auto const t3 = apitime::now();
		apitime::add( apitime::PPacing, presentEnd.QuadPart, t3 );
		frametiming::onPresent( presentFreq.QuadPart ? double( presentEnd.QuadPart - presentBegin.QuadPart ) * 1000.0 / double( presentFreq.QuadPart ) : 0.0
			, get( m_device )->getDevice(), *context );
		auto const t4 = apitime::now();
		apitime::add( apitime::PStats, t3, t4 );
		get( m_device )->drainDebugMessages( "present" );
		apitime::add( apitime::PDrain, t4, apitime::now() );

		if ( probeThisFrame )
		{
			frameprobe::report( probe, frame, m_windowExtent, m_swapchainExtent, presentHr );
		}

		if ( SUCCEEDED( presentHr ) )
		{
			// checkError still forwards genuine failures to validation layers, but
			// statuses such as DXGI_STATUS_OCCLUDED are successful HRESULTs.
			checkError( m_device, presentHr, "Presentation" );
			return VK_SUCCESS;
		}

		checkError( m_device, presentHr, "Presentation" );

		if ( frameprobe::isDeviceLoss( presentHr )
			|| get( m_device )->getStatus() == VK_ERROR_DEVICE_LOST )
		{
			return VK_ERROR_DEVICE_LOST;
		}

		return VK_ERROR_SURFACE_LOST_KHR;
	}

	VkResult SwapchainKHR::acquireNextImage( uint32_t & imageIndex )const
	{
		++m_acquireCount;
		imageIndex = m_nextImage;
		m_nextImage = m_images.empty() ? 0u : ( m_nextImage + 1u ) % uint32_t( m_images.size() );
		return VK_SUCCESS;
	}

	void SwapchainKHR::doInitPresentParameters( bool flipModel )
	{
		auto caps = get( m_createInfo.surface )->getCapabilities( get( m_device )->getPhysicalDevice() );
		m_windowExtent = caps.maxImageExtent;
		m_displayMode = get( m_createInfo.surface )->getMatchingDesc( m_createInfo.imageFormat );
#if defined( Ashes_D3D11_XBox )

		// Initialize the swap chain description.
		auto result = DXGI_SWAP_CHAIN_DESC1{};

		// Use triple buffering.
		result.BufferCount = 3;

		// Set the back buffer format and size.
		result.Width = m_createInfo.imageExtent.width;
		result.Height = m_createInfo.imageExtent.height;
		result.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		result.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

		// Turn multisampling off.
		result.SampleDesc.Count = 1;
		result.SampleDesc.Quality = 0;

		result.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		result.Scaling = DXGI_SCALING_STRETCH;
		result.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		result.Flags = 0;

#else

		auto hWnd = get( m_createInfo.surface )->getHwnd();

		// Initialize the swap chain description.
		auto result = DXGI_SWAP_CHAIN_DESC{};

		// Set to a single back buffer.
		result.BufferCount = 1;

		// Set the back buffer desc to the surface matching one.
		result.BufferDesc = m_displayMode;
		// Except for the dimensions.
		result.BufferDesc.Width = m_createInfo.imageExtent.width;
		result.BufferDesc.Height = m_createInfo.imageExtent.height;

		// Set the usage of the back buffer.
		result.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

		// Set the handle for the window to render to.
		result.OutputWindow = hWnd;

		// Turn multisampling off.
		result.SampleDesc.Count = 1;
		result.SampleDesc.Quality = 0;

		// Set to windowed mode.
		result.Windowed = TRUE;

		// Discard the back buffer contents after presenting.
		result.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		m_flipModel = false;
		if ( flipModel )
		{
			// Flip model: 2+ buffers, no sRGB buffer formats (the copy from the sRGB image into
			// the UNORM buffer of the same family keeps the bytes unchanged).
			auto format = result.BufferDesc.Format;
			if ( format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ) format = DXGI_FORMAT_B8G8R8A8_UNORM;
			if ( format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ) format = DXGI_FORMAT_R8G8B8A8_UNORM;
			if ( format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM
				|| format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT )
			{
				result.BufferDesc.Format = format;
				// v13.70: 3 buffers (was 2). In fullscreen/borderless the window is shown by flipping
				// the back buffers directly (no desktop compositor); with only 2 buffers the next
				// frame's copy into the back buffer had to wait for the screen to release it, and
				// vkQueuePresentKHR took 8-17 ms per frame (run 140535, after switching to
				// fullscreen: present 0.5 -> 7-10 ms, p95 frame 20 -> 55-62 ms). RW_GPU_FLIP_BUFFERS=2 = old.
				static UINT const buffers = []() { UINT n = UINT( envflags::number( "RW_GPU_FLIP_BUFFERS", 3u ) ); return n < 2u ? 2u : ( n > 4u ? 4u : n ); }();
				result.BufferCount = buffers;
				result.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				m_flipModel = true;
			}
		}

		// Set the advanced flags, if surface is a display surface.
		result.Flags = UINT( get( m_createInfo.surface )->isDisplay()
			? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
			: 0 );
		// v13.70: tearing (variable refresh / "vsync off") for flip swapchains when Windows and the
		// driver support it (DXGI 1.5, Windows 10 1607+). Without it, a flip-model Present in
		// fullscreen/borderless waits for the display's refresh even with sync interval 0, so a
		// 45 fps frame waits for the next 60 Hz slot (judder) and the render thread blocks.
		// Possible visible tear lines in fullscreen. RW_GPU_ALLOW_TEARING=0 = off.
		m_tearing = false;
		if ( m_flipModel && envflags::on( "RW_GPU_ALLOW_TEARING", true ) )
		{
			IDXGIFactory5 * factory5{};
			auto factory = get( get( m_device )->getInstance() )->getDXGIFactory();
			if ( factory && SUCCEEDED( factory->QueryInterface( __uuidof( IDXGIFactory5 ), reinterpret_cast< void ** >( &factory5 ) ) ) && factory5 )
			{
				BOOL allow = FALSE;
				if ( SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof( allow ) ) ) && allow )
				{
					result.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
					m_tearing = true;
				}
				factory5->Release();
			}
			static std::atomic< uint32_t > logged{ 0u };
			if ( logged++ < 2u ) std::fprintf( stderr, "[RW-GPU-WSI] tearing (present without waiting for the display refresh): %s\n", m_tearing ? "supported, on" : "not supported by Windows/driver" );
		}

#endif

		m_presentDesc = result;
	}
}
