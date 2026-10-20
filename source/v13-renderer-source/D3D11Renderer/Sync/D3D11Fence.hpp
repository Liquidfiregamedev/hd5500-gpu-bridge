/*
	This file belongs to Ashes.
	See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <atomic>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	class Fence
		: public NonCopyable
	{
	public:
		explicit Fence( VkDevice device, bool signaled = false )noexcept;
		~Fence()noexcept;

		// Called by Queue while it still owns the immediate-context lock, so the
		// marker is ordered directly after every command in this vkQueueSubmit.
		VkResult signalSubmitted( ID3D11DeviceContext * context )const noexcept;
		VkResult wait()const noexcept;
		void reset()const noexcept;
		VkResult getStatus()const noexcept;
		// v13.58 submit thread: a fence of a submit that is queued but not executed yet.
		void markQueued()const noexcept { m_queued.fetch_add( 1u, std::memory_order_acq_rel ); }
		void unmarkQueued()const noexcept { m_queued.fetch_sub( 1u, std::memory_order_acq_rel ); }
		// v13.62: a queued submit failed before its marker was written: report the fence as
		// signalled so a waiting game thread is not stuck forever (the error itself is returned
		// by the next submit/present).
		void signalFailedSubmit()const noexcept { std::lock_guard< std::mutex > lock{ m_mutex }; m_pending = false; m_signaled = true; }

		VkDevice getDevice()const noexcept
		{
			return m_device;
		}

		static VkResult waitFor( std::vector< Fence * > const & fences
			, bool waitAll
			, uint64_t timeout )noexcept;
		static void reportDiagnostics()noexcept;

	private:
		VkResult poll( bool allowFlush, bool & flushed )const noexcept;
		void finishQuery( ID3D11Query * query )const noexcept;

	private:
		VkDevice m_device;
		bool m_eventsEnabled{};
		mutable std::mutex m_mutex;
		mutable ID3D11Query * m_query{};
		// Reset never releases a query that may still be referenced by D3D11.  It
		// is abandoned until fence destruction (or D3D11 device teardown).
		mutable std::vector< ID3D11Query * > m_abandonedQueries;
		mutable bool m_signaled{};
		mutable bool m_pending{};
		mutable bool m_flushed{};
		mutable std::atomic< uint32_t > m_queued{ 0u };
	};
}
