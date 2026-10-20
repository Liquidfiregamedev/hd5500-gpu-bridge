/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.48: background sampler of the real processor speed, through the Windows performance
// counter "\Processor Information(_Total)\% Processor Performance" (100 = nominal clock; it
// includes turbo and every kind of throttling). PdhAddEnglishCounterW makes the counter name
// independent of the Windows display language. pdh.dll is loaded at run time, no link change.
// Reported in the [RW-GPU-FRAMES] lines: average, minimum and how many samples were below 40%.
// RW_GPU_CPU_SPEED=0 switches the sampler off.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	namespace cpuspeed
	{
		struct Stats
		{
			std::atomic< uint64_t > samples{ 0u };
			std::atomic< uint64_t > sumPercent{ 0u };
			std::atomic< uint64_t > lowSamples{ 0u };   // below 40 %
			std::atomic< uint32_t > minPercent{ 1000u };
			std::atomic< bool > available{ false };
			// v13.49: firmware performance limit (% Performance Limit: >0 = held back by
			// PROCHOT / thermal / power limits), clock in MHz, ACPI thermal zone temperature.
			std::atomic< uint64_t > limitSamples{ 0u };   // samples with a limit active
			std::atomic< uint32_t > maxLimit{ 0u };
			std::atomic< uint32_t > minMhz{ 100000u };
			std::atomic< uint32_t > maxTempC{ 0u };
			std::atomic< bool > tempAvailable{ false };
			// v13.50 Throttle monitor: latest values (for the live file / launcher window).
			std::atomic< uint32_t > nowSpeed{ 0u }, nowLimit{ 0u }, nowMhz{ 0u }, nowTempC{ 0u };
			std::atomic< uint32_t > nowProbeRatioX100{ 100u };   // probe time / fastest probe seen
			std::atomic< bool > throttledNow{ false };           // CPU running clearly slower than its best
			std::atomic< bool > guardActive{ false };            // heat guard currently capping the frame rate
			std::atomic< uint64_t > presents{ 0u };              // incremented per present (fps)
			std::atomic< uint64_t > guardEngagements{ 0u };
			std::atomic< uint64_t > throttledSeconds{ 0u };
		};

		inline Stats & stats()
		{
			static Stats result;
			return result;
		}

		// Fixed integer work timed in microseconds (no memory traffic): ~0.25 ms at full clock on
		// this kind of CPU core; several times longer when the clock collapses (throttling).
		inline double probeUs()
		{
			LARGE_INTEGER f{}, a{}, b{};
			QueryPerformanceFrequency( &f );
			QueryPerformanceCounter( &a );
			volatile uint64_t sink = 0u;
			uint64_t x = 0x9E3779B97F4A7C15ull;
			for ( uint32_t i = 0u; i < 200000u; ++i ) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
			sink = x;
			(void)sink;
			QueryPerformanceCounter( &b );
			return f.QuadPart ? double( b.QuadPart - a.QuadPart ) * 1e6 / double( f.QuadPart ) : 0.0;
		}

		inline bool envFlag( char const * name, bool fallback )
		{
			char text[8]{};
			auto length = GetEnvironmentVariableA( name, text, DWORD( sizeof( text ) ) );
			if ( length == 0u || length >= sizeof( text ) ) return fallback;
			return text[0] == '1';
		}

		inline uint32_t envNumber( char const * name, uint32_t fallback )
		{
			char text[16]{};
			auto length = GetEnvironmentVariableA( name, text, DWORD( sizeof( text ) ) );
			if ( length == 0u || length >= sizeof( text ) ) return fallback;
			return uint32_t( std::strtoul( text, nullptr, 10 ) );
		}

		inline void start()
		{
			static std::atomic< bool > started{ false };
			if ( started.exchange( true ) ) return;
			char text[4]{};
			auto length = GetEnvironmentVariableA( "RW_GPU_CPU_SPEED", text, DWORD( sizeof( text ) ) );
			if ( length == 1u && text[0] == '0' ) return;
			std::thread( []()
				{
					// v13.50: the counters are optional; the probe, heat guard and live file work without them.
					auto module = LoadLibraryA( "pdh.dll" );
					using OpenQuery = LONG ( WINAPI * )( LPCWSTR, DWORD_PTR, HANDLE * );
					using AddEnglish = LONG ( WINAPI * )( HANDLE, LPCWSTR, DWORD_PTR, HANDLE * );
					using Collect = LONG ( WINAPI * )( HANDLE );
					struct FmtValue { DWORD CStatus; DWORD pad; union { LONG longValue; double doubleValue; LONGLONG largeValue; }; };
					using GetFormatted = LONG ( WINAPI * )( HANDLE, DWORD, LPDWORD, FmtValue * );
					auto openQuery = module ? reinterpret_cast< OpenQuery >( GetProcAddress( module, "PdhOpenQueryW" ) ) : nullptr;
					auto addEnglish = module ? reinterpret_cast< AddEnglish >( GetProcAddress( module, "PdhAddEnglishCounterW" ) ) : nullptr;
					auto collect = module ? reinterpret_cast< Collect >( GetProcAddress( module, "PdhCollectQueryData" ) ) : nullptr;
					auto getFormatted = module ? reinterpret_cast< GetFormatted >( GetProcAddress( module, "PdhGetFormattedCounterValue" ) ) : nullptr;
					// Wildcard counters (all thermal zones) need the array form.
					struct FmtItem { LPWSTR name; FmtValue value; };
					using GetArray = LONG ( WINAPI * )( HANDLE, DWORD, LPDWORD, LPDWORD, FmtItem * );
					auto getArray = module ? reinterpret_cast< GetArray >( GetProcAddress( module, "PdhGetFormattedCounterArrayW" ) ) : nullptr;
					HANDLE query{}, counter{};
					bool hasPdh = openQuery && addEnglish && collect && getFormatted && openQuery( nullptr, 0u, &query ) == 0;
					bool const hasSpeed = hasPdh && addEnglish( query, L"\\Processor Information(_Total)\\% Processor Performance", 0u, &counter ) == 0;
					HANDLE limitCounter{}, mhzCounter{}, tempCounter{};
					bool const hasLimit = hasPdh && addEnglish( query, L"\\Processor Information(_Total)\\% Performance Limit", 0u, &limitCounter ) == 0;
					bool const hasMhz = hasPdh && addEnglish( query, L"\\Processor Information(_Total)\\Processor Frequency", 0u, &mhzCounter ) == 0;
					bool const hasTemp = hasPdh && addEnglish( query, L"\\Thermal Zone Information(*)\\Temperature", 0u, &tempCounter ) == 0;
					if ( hasPdh ) collect( query );
					auto & s = stats();
					s.available = hasSpeed;
					// v13.50 Throttle monitor state (once per second).
					static bool const guardEnabled = envFlag( "RW_GPU_THERMAL_GUARD", false );
					static uint32_t const guardTemp = envNumber( "RW_GPU_THERMAL_GUARD_TEMP", 85u );
					wchar_t liveFile[MAX_PATH]{};
					auto liveLength = GetEnvironmentVariableW( L"RW_GPU_LIVE_FILE", liveFile, MAX_PATH );
					bool const liveEnabled = liveLength > 0u && liveLength < MAX_PATH;
					double bestProbe = 1e30;
					uint32_t tick = 0u, badSeconds = 0u, goodSeconds = 0u;
					uint32_t badHistory = 0u;   // v13.71: last 10 seconds, one bit per second (1 = throttled/hot)
					uint64_t lastPresents = 0u;
					for ( ;; )
					{
						Sleep( 250u );
						if ( ( ++tick % 4u ) == 0u )
						{
							double probe = probeUs();
							if ( probe > 0.0 && probe < bestProbe ) bestProbe = probe;
							uint32_t ratio = bestProbe < 1e29 ? uint32_t( probe * 100.0 / bestProbe + 0.5 ) : 100u;
							s.nowProbeRatioX100 = ratio;
							bool const slow = ratio >= 250u;                      // 2.5x slower than its best
							bool const limited = s.nowLimit.load() >= 10u;   // firmware holds the CPU 10%+ below nominal
							bool const hot = s.tempAvailable && s.nowTempC.load() >= guardTemp;
							// v13.71: the HD 5500 laptop throttles in short cycles (run 150140: ~2 s at 22% clock,
							// ~2 s at 115%, repeating). "3 bad seconds in a row" never happened, so the heat guard
							// never engaged. Now: the CPU clock below 50% also counts as throttled, and the guard
							// engages when 3 of the last 10 seconds were bad; it still lifts after 20 good seconds.
							uint32_t const speedNow = s.nowSpeed.load();
							bool const lowClock = speedNow > 0u && speedNow < 50u;
							bool const bad = slow || limited || hot || lowClock;
							s.throttledNow = slow || limited || lowClock;
							if ( slow || limited || lowClock ) ++s.throttledSeconds;
							if ( bad ) { ++badSeconds; goodSeconds = 0u; }
							else { ++goodSeconds; badSeconds = 0u; }
							badHistory = ( ( badHistory << 1u ) | ( bad ? 1u : 0u ) ) & 0x3FFu;
							uint32_t badInWindow = 0u;
							for ( uint32_t b = badHistory; b; b &= b - 1u ) ++badInWindow;
							if ( guardEnabled && !s.guardActive && ( badSeconds >= 3u || badInWindow >= 3u ) ) { s.guardActive = true; ++s.guardEngagements; }
							if ( s.guardActive && goodSeconds >= 20u ) s.guardActive = false;
							if ( liveEnabled )
							{
								uint64_t presents = s.presents.load();
								uint64_t fps = presents - lastPresents;
								lastPresents = presents;
								SYSTEM_POWER_STATUS power{};
								GetSystemPowerStatus( &power );
								char line[512];
								int n = std::snprintf( line, sizeof( line ),
									"fps=%llu\r\nspeed=%u\r\nmhz=%u\r\nlimit=%u\r\ntemp=%s%u\r\nprobe=%u\r\nthrottled=%u\r\nguard=%s\r\nguardEngagements=%llu\r\nthrottledSeconds=%llu\r\nac=%u\r\nbattery=%u\r\ncharging=%u\r\nstamp=%llu\r\n"
									, (unsigned long long)fps, unsigned( s.nowSpeed.load() ), unsigned( s.nowMhz.load() ), unsigned( s.nowLimit.load() )
									, s.tempAvailable ? "" : "-", unsigned( s.nowTempC.load() ), unsigned( ratio ), unsigned( s.throttledNow ? 1u : 0u )
									, guardEnabled ? ( s.guardActive ? "active" : "armed" ) : "off"
									, (unsigned long long)s.guardEngagements.load(), (unsigned long long)s.throttledSeconds.load()
									, unsigned( power.ACLineStatus ), unsigned( power.BatteryLifePercent ), unsigned( ( power.BatteryFlag & 8u ) ? 1u : 0u )
									, (unsigned long long)GetTickCount64() );
								wchar_t temp[MAX_PATH + 8]{};
								std::swprintf( temp, MAX_PATH + 8, L"%ls.tmp", liveFile );
								HANDLE file = CreateFileW( temp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
								if ( file != INVALID_HANDLE_VALUE )
								{
									DWORD written{};
									WriteFile( file, line, DWORD( n > 0 ? n : 0 ), &written, nullptr );
									CloseHandle( file );
									MoveFileExW( temp, liveFile, MOVEFILE_REPLACE_EXISTING );
								}
							}
						}
						if ( !hasPdh || collect( query ) != 0 ) continue;
						FmtValue value{};
						if ( !hasSpeed || getFormatted( counter, 0x00000200 /*PDH_FMT_DOUBLE*/, nullptr, &value ) != 0 ) continue;
						auto percent = uint32_t( value.doubleValue + 0.5 );
						s.nowSpeed = percent;
						++s.samples;
						s.sumPercent += percent;
						if ( percent < 40u ) ++s.lowSamples;
						auto current = s.minPercent.load();
						while ( percent < current && !s.minPercent.compare_exchange_weak( current, percent ) ) {}
						auto lower = []( std::atomic< uint32_t > & a, uint32_t v ){ auto c = a.load(); while ( v < c && !a.compare_exchange_weak( c, v ) ) {} };
						auto raise = []( std::atomic< uint32_t > & a, uint32_t v ){ auto c = a.load(); while ( v > c && !a.compare_exchange_weak( c, v ) ) {} };
						FmtValue extra{};
						if ( hasLimit && getFormatted( limitCounter, 0x00000200, nullptr, &extra ) == 0 )
						{
							// "% Performance Limit" = share of nominal performance the firmware still allows
							// (100 = no limit). Stored as the amount taken away: 0 = none, 60 = held to 40%.
							// (v13.50 first build read it the wrong way round and flagged every second.)
							auto allowed = uint32_t( extra.doubleValue + 0.5 );
							auto limit = allowed >= 100u ? 0u : 100u - allowed;
							s.nowLimit = limit;
							if ( limit >= 10u ) ++s.limitSamples;
							raise( s.maxLimit, limit );
						}
						if ( hasMhz && getFormatted( mhzCounter, 0x00000200, nullptr, &extra ) == 0 && extra.doubleValue > 0.0 )
						{
							s.nowMhz = uint32_t( extra.doubleValue + 0.5 );
							lower( s.minMhz, uint32_t( extra.doubleValue + 0.5 ) );
						}
						if ( hasTemp && getArray )
						{
							DWORD bytes = 0u, count = 0u;
							if ( getArray( tempCounter, 0x00000200, &bytes, &count, nullptr ) == LONG( 0x800007D2 ) /*PDH_MORE_DATA*/ && bytes )
							{
								std::vector< uint8_t > buffer( bytes );
								auto items = reinterpret_cast< FmtItem * >( buffer.data() );
								if ( getArray( tempCounter, 0x00000200, &bytes, &count, items ) == 0 )
								{
									for ( DWORD i = 0u; i < count; ++i )
									{
										double value = items[i].value.doubleValue;
										if ( value <= 0.0 ) continue;
										// Kelvin (some systems report tenths of Kelvin); convert to Celsius.
										double k = value > 1000.0 ? value / 10.0 : value;
										if ( k < 250.0 || k > 400.0 ) continue;
										s.tempAvailable = true;
										s.nowTempC = uint32_t( k - 273.15 + 0.5 );
										raise( s.maxTempC, uint32_t( k - 273.15 + 0.5 ) );
									}
								}
							}
						}
					}
				} ).detach();
		}
	}
}
