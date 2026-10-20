/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.58 (experiment, off by default): a "submit thread" in the style of DXVK's CS thread.
//
// Without it, vkQueueSubmit and vkQueuePresentKHR translate and execute every recorded command
// on the game's render thread (~8-10 ms per frame on the HD 5500 laptop). With
// RW_GPU_SUBMIT_THREAD=1 they only queue the work and return; one worker thread executes the
// queued submits and presents in order on the D3D11 immediate context, while the game already
// simulates and records the next frame. At most one present is queued (the game can be one
// frame ahead, no more - so input latency grows by at most one frame).
//
// Why this is correct under Vulkan's rules: a submitted command buffer and the memory it reads
// are "in flight" until its fence signals; the game may not change them before that. The
// fences of queued work report "not ready" until the worker has executed the submit. Every
// other use of the immediate context from a game thread (memory flush/unmap, query results,
// wait-idle, swapchain create/destroy, event status) first waits until the worker is idle
// ("drain"), so it sees exactly the state it would have seen without the thread.
// Fence polling does not drain (it only looks at the fence's own D3D11 event query).

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace ashes::D3D11_NAMESPACE::submitthread
{
	inline bool enabled()
	{
		static bool const result = envflags::on( "RW_GPU_SUBMIT_THREAD", false );
		return result;
	}

	// Counters for the [RW-GPU-FRAMES] line (session totals).
	inline std::atomic< uint64_t > g_jobs{ 0u };
	inline std::atomic< uint64_t > g_drains{ 0u };
	inline std::atomic< uint64_t > g_drainWaitUs{ 0u };
	inline std::atomic< uint64_t > g_presentWaitUs{ 0u };

	class Worker
	{
	public:
		Worker()
		{
			std::thread{ [this]() { run(); } }.detach();   // never joined: process exit ends it
		}

		bool onWorker()const noexcept
		{
			return GetCurrentThreadId() == m_threadId.load( std::memory_order_acquire );
		}

		// Queue a job. isPresent: blocks while an earlier present is still queued.
		void push( std::function< void() > job, bool isPresent )
		{
			std::unique_lock< std::mutex > lock{ m_mutex };
			if ( isPresent && m_pendingPresents >= 1u )
			{
				LARGE_INTEGER a{}, b{}, f{};
				QueryPerformanceCounter( &a );
				m_changed.wait( lock, [this]() { return m_pendingPresents < 1u; } );
				QueryPerformanceCounter( &b );
				QueryPerformanceFrequency( &f );
				if ( f.QuadPart ) g_presentWaitUs.fetch_add( uint64_t( double( b.QuadPart - a.QuadPart ) * 1e6 / double( f.QuadPart ) ), std::memory_order_relaxed );
			}
			if ( isPresent ) ++m_pendingPresents;
			m_jobs.push_back( Job{ std::move( job ), isPresent } );
			g_jobs.fetch_add( 1u, std::memory_order_relaxed );
			m_changed.notify_all();
		}

		// Wait until every queued job has run. Called by game threads before they touch the
		// immediate context. No-op on the worker itself.
		void drain()
		{
			if ( onWorker() ) return;
			std::unique_lock< std::mutex > lock{ m_mutex };
			if ( m_jobs.empty() && !m_busy ) return;
			LARGE_INTEGER a{}, b{}, f{};
			QueryPerformanceCounter( &a );
			m_changed.wait( lock, [this]() { return m_jobs.empty() && !m_busy; } );
			QueryPerformanceCounter( &b );
			QueryPerformanceFrequency( &f );
			g_drains.fetch_add( 1u, std::memory_order_relaxed );
			if ( f.QuadPart ) g_drainWaitUs.fetch_add( uint64_t( double( b.QuadPart - a.QuadPart ) * 1e6 / double( f.QuadPart ) ), std::memory_order_relaxed );
		}

		// First error of a queued submit/present, returned by the next submit/present call.
		VkResult takeError()
		{
			std::lock_guard< std::mutex > lock{ m_mutex };
			auto result = m_error;
			if ( result != VK_ERROR_DEVICE_LOST ) m_error = VK_SUCCESS;   // device lost stays
			return result;
		}

		void setError( VkResult result )
		{
			if ( result == VK_SUCCESS ) return;
			std::lock_guard< std::mutex > lock{ m_mutex };
			if ( m_error == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST ) m_error = result;
		}

	private:
		struct Job
		{
			std::function< void() > run;
			bool isPresent{};
		};

		void run()
		{
			m_threadId.store( GetCurrentThreadId(), std::memory_order_release );
			// Same priority class as the game's render thread; slightly above normal so the
			// game's worker threads do not starve the frame that is being presented.
			SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL );
			for ( ;; )
			{
				Job job;
				{
					std::unique_lock< std::mutex > lock{ m_mutex };
					m_changed.wait( lock, [this]() { return !m_jobs.empty(); } );
					job = std::move( m_jobs.front() );
					m_jobs.pop_front();
					m_busy = true;
				}
				try
				{
					job.run();
				}
				catch ( std::exception const & exc )
				{
					std::fprintf( stderr, "[RW-GPU-SUBMIT-THREAD] queued job failed: %s\n", exc.what() );
					setError( VK_ERROR_DEVICE_LOST );
				}
				catch ( ... )
				{
					setError( VK_ERROR_DEVICE_LOST );
				}
				{
					std::lock_guard< std::mutex > lock{ m_mutex };
					m_busy = false;
					if ( job.isPresent && m_pendingPresents ) --m_pendingPresents;
				}
				m_changed.notify_all();
			}
		}

		std::mutex m_mutex;
		std::condition_variable m_changed;
		std::deque< Job > m_jobs;
		bool m_busy{};
		uint32_t m_pendingPresents{};
		VkResult m_error{ VK_SUCCESS };
		std::atomic< DWORD > m_threadId{ 0u };
	};

	inline Worker & worker()
	{
		static Worker * const result = new Worker{};   // intentionally leaked (detached thread)
		return *result;
	}

	// Game-thread synchronisation point: no-op when the experiment is off.
	inline void drain()
	{
		if ( enabled() ) worker().drain();
	}

	inline bool onWorker()
	{
		return enabled() && worker().onWorker();
	}
}
