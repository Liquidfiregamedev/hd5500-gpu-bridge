/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.48: CPU shadow of host-visible Vulkan memory with Windows write tracking.
//
// The bridge keeps a CPU copy ("shadow") of every host-visible VkDeviceMemory; vkMapMemory hands
// the application a pointer into it, and before the GPU uses a buffer the bridge copies the
// shadow into the D3D11 buffer. Persistently mapped, coherent memory gives no hint which bytes
// the application changed, so whole ranges (often entire multi-MB vertex/uniform rings) were
// re-uploaded on every use: ~8 MB per frame on RimWorld 1.7, and the D3D11 driver sometimes
// stalls the frame on those copies (the "freezes").
//
// Large shadows are allocated with VirtualAlloc(MEM_WRITE_WATCH): the OS records which 4 KiB
// pages were written since the last query (GetWriteWatch). The upload path then copies only the
// dirty pages. Small shadows and RW_GPU_WRITE_WATCH=0 use plain heap memory (old behaviour).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	class WatchedBytes
	{
	public:
		WatchedBytes() = default;
		WatchedBytes( WatchedBytes const & ) = delete;
		WatchedBytes & operator=( WatchedBytes const & ) = delete;
		~WatchedBytes()noexcept
		{
			release();
		}

		static bool enabled()
		{
			static bool const result = []()
				{
					char text[4]{};
					auto length = GetEnvironmentVariableA( "RW_GPU_WRITE_WATCH", text, DWORD( sizeof( text ) ) );
					return !( length == 1u && text[0] == '0' );
				}();
			return result;
		}

		void resize( size_t count, uint8_t value )
		{
			release();
			if ( !count ) return;
			if ( enabled() && count >= minWatchedBytes() )
			{
				auto ptr = VirtualAlloc( nullptr, count, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE );
				if ( ptr )
				{
					m_data = static_cast< uint8_t * >( ptr );
					m_size = count;
					m_watched = true;
					std::memset( m_data, value, count );   // all pages start dirty: first upload is full
					return;
				}
			}
			m_heap.assign( count, value );
			m_data = m_heap.data();
			m_size = count;
		}

		bool empty()const noexcept { return m_size == 0u; }
		size_t size()const noexcept { return m_size; }
		uint8_t * data()noexcept { return m_data; }
		uint8_t const * data()const noexcept { return m_data; }
		uint8_t & operator[]( size_t index )noexcept { return m_data[index]; }
		uint8_t const & operator[]( size_t index )const noexcept { return m_data[index]; }
		bool watched()const noexcept { return m_watched; }

		// Dirty runs (byte offsets relative to the shadow start) inside [begin, end), page
		// granular, clipped to the range. Only pages lying completely inside the range are reset
		// (memory is often sub-allocated: an edge page can belong to a neighbouring buffer, whose
		// own dirty bit must survive). The reset happens before the caller copies the bytes, so a
		// write made after this call marks its page again. Writers must not touch the range that
		// is being submitted (Vulkan host-access rules), so nothing is lost between query and reset.
		// Returns false when tracking is unavailable (caller uploads the whole range).
		bool takeDirtyRuns( size_t begin, size_t end, std::vector< std::pair< size_t, size_t > > & runs )const
		{
			runs.clear();
			if ( !m_watched || begin >= end || end > m_size ) return false;
			size_t const page = pageSize();
			size_t const first = begin / page * page;
			size_t const last = std::min( ( end + page - 1u ) / page * page, ( m_size + page - 1u ) / page * page );
			size_t const span = last - first;
			ULONG_PTR count = ULONG_PTR( span / page );
			if ( count == 0u ) return true;
			thread_local std::vector< PVOID > addresses;
			addresses.resize( size_t( count ) );
			ULONG granularity{};
			if ( GetWriteWatch( 0u, m_data + first, span, addresses.data(), &count, &granularity ) != 0u ) return false;
			for ( ULONG_PTR i = 0u; i < count; ++i )
			{
				size_t pageBegin = size_t( static_cast< uint8_t * >( addresses[i] ) - m_data );
				size_t runBegin = std::max( pageBegin, begin );
				size_t runEnd = std::min( pageBegin + size_t( granularity ), end );
				if ( runBegin >= runEnd ) continue;
				if ( !runs.empty() && runs.back().second == runBegin ) runs.back().second = runEnd;
				else runs.emplace_back( runBegin, runEnd );
			}
			if ( !runs.empty() ) resetRange( begin, end );
			return true;
		}

		// Forget writes in the whole pages inside [begin, end): after the bridge copied them, or
		// after it wrote the shadow itself (GPU->CPU downloads).
		void resetRange( size_t begin, size_t end )const noexcept
		{
			if ( !m_watched || begin >= end || end > m_size ) return;
			size_t const page = pageSize();
			size_t const innerFirst = ( begin + page - 1u ) / page * page;
			size_t const innerLast = ( end >= m_size ) ? ( m_size + page - 1u ) / page * page : end / page * page;
			if ( innerFirst < innerLast ) ResetWriteWatch( m_data + innerFirst, innerLast - innerFirst );
		}

		static size_t pageSize()noexcept
		{
			static size_t const result = []()
				{
					SYSTEM_INFO info{};
					GetSystemInfo( &info );
					return size_t( info.dwPageSize ? info.dwPageSize : 4096u );
				}();
			return result;
		}

	private:
		void release()noexcept
		{
			if ( m_watched && m_data ) VirtualFree( m_data, 0u, MEM_RELEASE );
			m_heap.clear();
			m_heap.shrink_to_fit();
			m_data = nullptr;
			m_size = 0u;
			m_watched = false;
		}

		// Smallest shadow that gets write tracking (default 64 KiB; RW_GPU_WRITE_WATCH_MIN=<bytes>
		// lowers it, used by the regression suite so its small test buffers are tracked too).
		static size_t minWatchedBytes()
		{
			static size_t const result = []()
				{
					char text[32]{};
					auto length = GetEnvironmentVariableA( "RW_GPU_WRITE_WATCH_MIN", text, DWORD( sizeof( text ) ) );
					if ( length > 0u && length < sizeof( text ) ) return size_t( std::strtoull( text, nullptr, 10 ) );
					return size_t( 64u * 1024u );
				}();
			return result;
		}
		std::vector< uint8_t > m_heap;
		uint8_t * m_data{};
		size_t m_size{};
		bool m_watched{};
	};

	// Totals for the run log (bytes the upload path was asked for vs. actually copied).
	inline std::atomic< uint64_t > g_watchRequestedBytes{ 0u };
	inline std::atomic< uint64_t > g_watchCopiedBytes{ 0u };
	inline std::atomic< uint64_t > g_watchSkippedUploads{ 0u };
	inline std::atomic< uint64_t > g_watchQueryUs{ 0u };      // time spent asking Windows for dirty pages
	inline std::atomic< uint64_t > g_bridgeExecuteUs{ 0u };   // time spent executing command buffers
	// v13.62: wall time from vkBeginCommandBuffer to vkEndCommandBuffer = the game recording a frame
	// (its own render code between the vkCmd* calls + the bridge's recording work).
	inline std::atomic< uint64_t > g_recordUs{ 0u };
	// v13.63: descriptor-set binds served by the fast path (dynamic offsets only) vs full binds
	inline std::atomic< uint64_t > g_setFast{ 0u };
	// v13.66: index-buffer binds: D3D11 call / same as bound / new offset folded into firstIndex
	inline std::atomic< uint64_t > g_indexBinds{ 0u };
	// v13.69: milliseconds per time-stamp-counter tick, calibrated once against QPC (1 ms spin).
	// Broadwell and later have an invariant TSC; __rdtsc costs ~10 ns against ~25 ns for QPC.
	inline double msPerTsc()
	{
		static double const result = []()
			{
				LARGE_INTEGER f{}, a{}, b{};
				QueryPerformanceFrequency( &f );
				QueryPerformanceCounter( &a );
				uint64_t const t0 = __rdtsc();
				do { QueryPerformanceCounter( &b ); } while ( f.QuadPart && ( b.QuadPart - a.QuadPart ) * 1000 < f.QuadPart );
				uint64_t const t1 = __rdtsc();
				double const ms = f.QuadPart ? double( b.QuadPart - a.QuadPart ) * 1000.0 / double( f.QuadPart ) : 0.0;
				return t1 > t0 ? ms / double( t1 - t0 ) : 0.0;
			}();
		return result;
	}
	// v13.69: sub-page uploads copied without a write-watch query
	inline std::atomic< uint64_t > g_subpageDirect{ 0u };
	inline std::atomic< uint64_t > g_indexSame{ 0u };
	inline std::atomic< uint64_t > g_indexFolded{ 0u };
	inline std::atomic< uint64_t > g_setFull{ 0u };
	inline std::atomic< uint64_t > g_setContentFast{ 0u };
	inline std::atomic< uint64_t > g_uploadsMerged{ 0u };   // v13.73: window uploads folded into an earlier upload at record time   // v13.73: fast binds found by content (new set handle)
	inline std::atomic< uint64_t > g_recordedCommands{ 0u };
	inline std::atomic< uint64_t > g_recordBridgeUs{ 0u };   // v13.64: part of g_recordUs spent inside the bridge
	inline std::atomic< uint64_t > g_bridgeExecuteCpuUs{ 0u };  // thread CPU time of those executions
	inline std::atomic< uint64_t > g_bridgeExecutions{ 0u };
	inline std::atomic< uint64_t > g_bridgeSlowExecutions{ 0u };   // over 100 ms
	// v13.51: command-buffer cleanup actions queued vs. skipped as duplicates (recording side).
	inline std::atomic< uint64_t > g_cleanupKept{ 0u };
	inline std::atomic< uint64_t > g_cleanupSkipped{ 0u };
	// v13.52: push-constant uploads done vs. skipped because the bytes were unchanged.
	inline std::atomic< uint64_t > g_pushUploads{ 0u };
	inline std::atomic< uint64_t > g_pushSkipped{ 0u };
}
