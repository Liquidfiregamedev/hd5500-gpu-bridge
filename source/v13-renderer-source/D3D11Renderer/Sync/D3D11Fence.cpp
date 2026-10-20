/*
	This file belongs to Ashes.
	See LICENSE file in root folder.
*/
#include "Sync/D3D11Fence.hpp"

#include "Core/D3D11Device.hpp"

#include "ashesd3d11_api.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ashes::D3D11_NAMESPACE
{
	namespace
	{
		std::atomic_uint64_t g_fencedSubmits{};
		std::atomic_uint64_t g_fenceFlushes{};
		std::atomic_uint64_t g_fencePolls{};
		std::atomic_uint64_t g_fenceTimeouts{};

		bool envIsOne( char const * name )noexcept
		{
			char value[2]{};
			return GetEnvironmentVariableA( name, value, UINT( sizeof( value ) ) ) == 1u && value[0] == '1';
		}

		bool fenceEventsEnabled()noexcept
		{
			char value[2]{};
			return GetEnvironmentVariableA( "RW_HD5500_FENCE_EVENTS", value, UINT( sizeof( value ) ) ) != 1u || value[0] != '0';
		}

		VkResult failureResult( VkDevice device, HRESULT hr )noexcept
		{
			if ( !device || get( device )->getStatus() != VK_SUCCESS ) return VK_ERROR_DEVICE_LOST;
			return hr == E_OUTOFMEMORY ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
		}

		uint64_t elapsedNanoseconds( LARGE_INTEGER begin, LARGE_INTEGER frequency )noexcept
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter( &now );
			return uint64_t( ( double( now.QuadPart - begin.QuadPart ) * 1000000000.0 ) / double( frequency.QuadPart ) );
		}
	}

	Fence::Fence( VkDevice device, bool signaled )noexcept
		: m_device{ device }
		, m_eventsEnabled{ fenceEventsEnabled() }
		, m_signaled{ signaled || !m_eventsEnabled }
	{
	}

	Fence::~Fence()noexcept
	{
		std::lock_guard< std::mutex > lock{ m_mutex };
		safeRelease( m_query );
		for ( auto & query : m_abandonedQueries ) safeRelease( query );
	}

	VkResult Fence::signalSubmitted( ID3D11DeviceContext * context )const noexcept
	{
		if ( !context || !m_device ) return VK_ERROR_INITIALIZATION_FAILED;
		if ( !m_eventsEnabled )
		{
			std::lock_guard< std::mutex > lock{ m_mutex };
			m_pending = false;
			m_flushed = false;
			m_signaled = true; // documented emergency compatibility fallback.
			return VK_SUCCESS;
		}

		ID3D11Query * query{};
		D3D11_QUERY_DESC desc{};
		desc.Query = D3D11_QUERY_EVENT;
		auto const hr = get( m_device )->getDevice()->CreateQuery( &desc, &query );
		if ( FAILED( hr ) || !query ) return failureResult( m_device, hr );

		{
			std::lock_guard< std::mutex > lock{ m_mutex };
			if ( m_query )
			{
				// A resubmit without reset is invalid Vulkan use, but never recycle an
				// in-flight D3D11 marker early.
				m_abandonedQueries.push_back( m_query );
				m_query = nullptr;
			}
			m_query = query;
			m_signaled = false;
			m_pending = true;
			m_flushed = false;
		}
		context->End( query );
		g_fencedSubmits.fetch_add( 1u, std::memory_order_relaxed );
		return VK_SUCCESS;
	}

	void Fence::finishQuery( ID3D11Query * query )const noexcept
	{
		std::lock_guard< std::mutex > lock{ m_mutex };
		if ( query == m_query )
		{
			safeRelease( m_query );
			m_pending = false;
			m_signaled = true;
		}
	}

	VkResult Fence::poll( bool allowFlush, bool & flushed )const noexcept
	{
		flushed = false;
		if ( m_queued.load( std::memory_order_acquire ) ) return VK_NOT_READY;   // v13.58: still queued
		if ( !m_eventsEnabled ) return VK_SUCCESS;

		ID3D11Query * query{};
		bool canFlush{};
		{
			std::lock_guard< std::mutex > lock{ m_mutex };
			if ( m_signaled || !m_pending ) return m_signaled ? VK_SUCCESS : VK_NOT_READY;
			query = m_query;
			canFlush = !m_flushed;
			if ( query ) query->AddRef();
		}
		if ( !query ) return VK_NOT_READY;

		HRESULT hr{};
		BOOL data{ FALSE };
		try
		{
			auto context = get( m_device )->getImmediateContextNoDrain();   // v13.58: never waits for the submit thread
			g_fencePolls.fetch_add( 1u, std::memory_order_relaxed );
			hr = context->GetData( query, &data, UINT( sizeof( data ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			if ( hr == S_FALSE && allowFlush && canFlush )
			{
				context->Flush();
				flushed = true;
				g_fenceFlushes.fetch_add( 1u, std::memory_order_relaxed );
				std::lock_guard< std::mutex > lock{ m_mutex };
				if ( query == m_query ) m_flushed = true;
			}
		}
		catch ( ... )
		{
			query->Release();
			return get( m_device )->getStatus() == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_DEVICE_LOST;
		}

		query->Release();
		if ( FAILED( hr ) ) return failureResult( m_device, hr );
		if ( hr == S_OK && data )
		{
			finishQuery( query );
			return VK_SUCCESS;
		}
		return VK_NOT_READY;
	}

	VkResult Fence::getStatus()const noexcept
	{
		bool flushed{};
		return poll( true, flushed );
	}

	VkResult Fence::wait()const noexcept
	{
		return waitFor( { const_cast< Fence * >( this ) }, true, UINT64_MAX );
	}

	void Fence::reset()const noexcept
	{
		std::lock_guard< std::mutex > lock{ m_mutex };
		if ( m_pending && m_query )
		{
			m_abandonedQueries.push_back( m_query );
			m_query = nullptr;
		}
		else
		{
			safeRelease( m_query );
		}
		m_pending = false;
		m_flushed = false;
		m_signaled = false;
	}

	VkResult Fence::waitFor( std::vector< Fence * > const & fences
		, bool waitAll
		, uint64_t timeout )noexcept
	{
		if ( fences.empty() ) return VK_SUCCESS;
		LARGE_INTEGER begin{};
		LARGE_INTEGER frequency{};
		QueryPerformanceCounter( &begin );
		QueryPerformanceFrequency( &frequency );
		bool progressKicked{};
		uint32_t iteration{};

		for ( ;; )
		{
			bool anySignaled{};
			bool allSignaled{ true };
			for ( auto * fence : fences )
			{
				if ( !fence ) return VK_ERROR_INITIALIZATION_FAILED;
				bool flushed{};
				auto const result = fence->poll( !progressKicked, flushed );
				progressKicked = progressKicked || flushed;
				if ( result == VK_SUCCESS ) anySignaled = true;
				else if ( result == VK_NOT_READY ) allSignaled = false;
				else return result;
			}
			if ( waitAll ? allSignaled : anySignaled ) return VK_SUCCESS;
			if ( timeout == 0u || ( timeout != UINT64_MAX && elapsedNanoseconds( begin, frequency ) >= timeout ) )
			{
				g_fenceTimeouts.fetch_add( 1u, std::memory_order_relaxed );
				return VK_TIMEOUT;
			}
			// Do not retain DeviceContextLock while yielding.  A short cooperative
			// spin avoids a full millisecond sleep for near-complete EVENT queries.
			if ( iteration++ < 16u ) Sleep( 0u ); else Sleep( 1u );
		}
	}

	void Fence::reportDiagnostics()noexcept
	{
		if ( !envIsOne( "RW_HD5500_D3D_DIAGNOSTICS" ) ) return;
		std::fprintf( stderr, "[RW-GPU-FENCE] fenced_submits=%llu flushes=%llu polls=%llu timeouts=%llu\n"
			, static_cast< unsigned long long >( g_fencedSubmits.load( std::memory_order_relaxed ) )
			, static_cast< unsigned long long >( g_fenceFlushes.load( std::memory_order_relaxed ) )
			, static_cast< unsigned long long >( g_fencePolls.load( std::memory_order_relaxed ) )
			, static_cast< unsigned long long >( g_fenceTimeouts.load( std::memory_order_relaxed ) ) );
	}
}
