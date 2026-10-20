/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.69: free recorded commands on a background thread.
// [RW-GPU-APITIME] (run 140535) showed vkResetCommandBuffer costing ~1 ms per frame on the game's
// render thread - the measured bottleneck: it destroys the 2,000-12,000 heap-allocated commands
// of the previous frame. Their destructors only free memory and drop references (shared_ptr,
// COM Release - both thread-safe), so the whole array is handed to one below-normal-priority
// thread that frees it while the render thread goes on. Commands whose destructors give bridge
// objects back to the device (blits, clears, staged texture copies) are still destroyed on the
// calling thread (CommandBase::destroyOnOwnerThread). At most 8 arrays wait; beyond that the
// caller frees itself (back-pressure). RW_GPU_DEFERRED_FREE=0 = free on the calling thread.

#include "Command/Commands/D3D11CommandBase.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ashes::D3D11_NAMESPACE::reaper
{
	inline std::atomic< uint64_t > g_deferred{ 0u };   // commands freed on the reaper thread

	class Reaper
	{
	public:
		Reaper()
		{
			std::thread{ [this]() { run(); } }.detach();   // never joined; process exit ends it
		}

		// Takes the commands out of `commands` (left empty). Returns false when the queue is full.
		bool push( CommandArray & commands )
		{
			std::unique_lock< std::mutex > lock{ m_mutex };
			if ( m_queue.size() >= 8u ) return false;
			m_queue.emplace_back( std::move( commands ) );
			commands.clear();
			lock.unlock();
			m_cv.notify_one();
			return true;
		}

	private:
		void run()
		{
			SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
			for ( ;; )
			{
				CommandArray batch;
				{
					std::unique_lock< std::mutex > lock{ m_mutex };
					m_cv.wait( lock, [this]() { return !m_queue.empty(); } );
					batch = std::move( m_queue.front() );
					m_queue.pop_front();
				}
				g_deferred.fetch_add( batch.size(), std::memory_order_relaxed );
				batch.clear();
			}
		}

		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::deque< CommandArray > m_queue;
	};

	inline Reaper & instance()
	{
		static Reaper * const result = new Reaper{};   // intentionally leaked (detached thread)
		return *result;
	}

	inline bool enabled()
	{
		static bool const result = envflags::on( "RW_GPU_DEFERRED_FREE", true );
		return result;
	}

	// Empties `commands`: owner-thread commands are destroyed here, the rest on the reaper thread.
	inline void retire( CommandArray & commands )
	{
		if ( commands.empty() ) return;
		if ( !enabled() || commands.size() < 64u ) { commands.clear(); return; }
		bool mixed = false;
		for ( auto const & command : commands )
		{
			if ( command && command->destroyOnOwnerThread() ) { mixed = true; break; }
		}
		if ( mixed )
		{
			for ( auto & command : commands )
			{
				if ( command && command->destroyOnOwnerThread() ) command.reset();
			}
		}
		if ( !instance().push( commands ) ) commands.clear();
	}
}
