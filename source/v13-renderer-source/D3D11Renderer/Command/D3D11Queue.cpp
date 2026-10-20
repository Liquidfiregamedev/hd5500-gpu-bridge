/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/D3D11Queue.hpp"

#include "Command/Commands/D3D11CommandBase.hpp"
#include "Command/D3D11CommandBuffer.hpp"
#include "Core/D3D11Device.hpp"
#include "Core/D3D11SwapChain.hpp"
#include "Sync/D3D11Fence.hpp"
#include "Sync/D3D11Semaphore.hpp"
#include "Command/D3D11SubmitThread.hpp"

#include "ashesd3d11_api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ashes::D3D11_NAMESPACE
{
	namespace
	{
		bool envIsOne( char const * name )noexcept
		{
			char value[2]{};
			return GetEnvironmentVariableA( name, value, UINT( sizeof( value ) ) ) == 1u && value[0] == '1';
		}

		double percentile( std::vector< double > values, double point )
		{
			std::sort( values.begin(), values.end() );
			auto const index = size_t( point * double( values.size() - 1u ) );
			return values[index];
		}
	}

	Queue::Queue( VkDevice device
		, VkDeviceQueueCreateInfo createInfo
		, uint32_t index )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
		, m_index{ index }
		{
			D3D11_QUERY_DESC desc{};
			desc.Query = D3D11_QUERY_EVENT;
			get( m_device )->getDevice()->CreateQuery( &desc, &m_waitIdleQuery );

			// Timing is strictly opt-in: disabled builds add no D3D11 queries to
			// submissions, which keeps the normal renderer path unchanged.
			m_timingEnabled = envIsOne( "RW_HD5500_GPU_TIMING" );
			if ( m_timingEnabled )
			{
				D3D11_QUERY_DESC disjoint{};
				disjoint.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
				D3D11_QUERY_DESC timestamp{};
				timestamp.Query = D3D11_QUERY_TIMESTAMP;
				for ( auto & slot : m_timingSlots )
				{
					if ( FAILED( get( m_device )->getDevice()->CreateQuery( &disjoint, &slot.disjoint ) )
						|| FAILED( get( m_device )->getDevice()->CreateQuery( &timestamp, &slot.begin ) )
						|| FAILED( get( m_device )->getDevice()->CreateQuery( &timestamp, &slot.end ) ) )
					{
						std::fprintf( stderr, "[RW-GPU-TIMING] disabled: D3D11 query creation failed\n" );
						releaseTiming();
						m_timingEnabled = false;
						break;
					}
				}
			}
		}
	
	Queue::~Queue()noexcept
	{
		releaseTiming();
		safeRelease( m_waitIdleQuery );
	}

	VkResult Queue::submit( ArrayView< VkSubmitInfo const > const & infos
		, VkFence fence )const
	{
		// v13.58: RW_GPU_SUBMIT_THREAD=1 -> queue the work for the submit thread and return.
		if ( submitthread::enabled() && !submitthread::onWorker() )
		{
			auto & worker = submitthread::worker();
			if ( auto error = worker.takeError(); error != VK_SUCCESS ) return error;
			if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
			std::vector< std::vector< VkCommandBuffer > > lists;
			lists.reserve( infos.size() );
			for ( auto & info : infos ) lists.emplace_back( info.pCommandBuffers, info.pCommandBuffers + info.commandBufferCount );
			if ( fence ) get( fence )->markQueued();
			worker.push( [this, lists = std::move( lists ), fence]()
				{
					std::vector< VkSubmitInfo > queued( lists.size() );
					for ( size_t i = 0u; i < lists.size(); ++i )
					{
						queued[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
						queued[i].commandBufferCount = uint32_t( lists[i].size() );
						queued[i].pCommandBuffers = lists[i].data();
					}
					VkResult result = VK_ERROR_DEVICE_LOST;
					try
					{
						VkSubmitInfo const * first = queued.data();
						result = submit( makeArrayView( first, first + queued.size() ), fence );
					}
					catch ( ... )
					{
						if ( fence ) { get( fence )->signalFailedSubmit(); get( fence )->unmarkQueued(); }
						throw;
					}
					if ( fence && result != VK_SUCCESS ) get( fence )->signalFailedSubmit();   // v13.62: no hang on a failed queued submit
					if ( fence ) get( fence )->unmarkQueued();
					submitthread::worker().setError( result );
				}, false );
			return VK_SUCCESS;
		}
		get( m_device )->requireHealthy( "Queue submit" );
		LARGE_INTEGER cpuBegin{};
		QueryPerformanceCounter( &cpuBegin );
		Context context{ get( m_device )->getFeatureLevel(), m_device };
		auto const timing = beginTiming( context, cpuBegin );
		VkResult result{ VK_SUCCESS };
		for ( auto & info : infos )
		{
			result = doSubmit( context, makeArrayView( info.pCommandBuffers, info.commandBufferCount ) );
			if ( result != VK_SUCCESS ) break;
		}
		LARGE_INTEGER cpuEnd{};
		QueryPerformanceCounter( &cpuEnd );
		if ( timing ) endTiming( context, cpuEnd );
		if ( result != VK_SUCCESS ) return result;

		// The EVENT marker is appended while Context still owns DeviceContextLock,
		// immediately after every command buffer (also for a zero-CB submit).
		if ( fence ) return get( fence )->signalSubmitted( *context.context );

		return VK_SUCCESS;
	}

	VkResult Queue::bindSparse()const
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkResult Queue::present( VkPresentInfoKHR const & presentInfo )const
	{
		if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
		// v13.58: submit thread - present in order after the queued submits. The result of a
		// queued present (e.g. out-of-date after a resize) is returned by the next present.
		if ( submitthread::enabled() && !submitthread::onWorker() )
		{
			auto & worker = submitthread::worker();
			if ( auto error = worker.takeError(); error != VK_SUCCESS ) return error;
			std::vector< VkSwapchainKHR > swapchains( presentInfo.pSwapchains, presentInfo.pSwapchains + presentInfo.swapchainCount );
			std::vector< uint32_t > indices( presentInfo.swapchainCount, 0u );
			if ( presentInfo.pImageIndices ) std::copy( presentInfo.pImageIndices, presentInfo.pImageIndices + presentInfo.swapchainCount, indices.begin() );
			worker.push( [this, swapchains = std::move( swapchains ), indices = std::move( indices )]()
				{
					VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
					info.swapchainCount = uint32_t( swapchains.size() );
					info.pSwapchains = swapchains.data();
					info.pImageIndices = indices.data();
					submitthread::worker().setError( present( info ) );
				}, true );
			if ( presentInfo.pResults ) std::fill( presentInfo.pResults, presentInfo.pResults + presentInfo.swapchainCount, VK_SUCCESS );
			return VK_SUCCESS;
		}
		VkResult overall = VK_SUCCESS;
		for ( uint32_t i = 0u; i < presentInfo.swapchainCount; ++i )
		{
			auto result = get( presentInfo.pSwapchains[i] )->present( presentInfo.pImageIndices ? presentInfo.pImageIndices[i] : 0u );
			if ( presentInfo.pResults ) presentInfo.pResults[i] = result;
			if ( result != VK_SUCCESS && overall == VK_SUCCESS ) overall = result;
		}
		return overall;

	}

	VkResult Queue::waitIdle()const
	{
		submitthread::drain();   // v13.58
		if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
		if ( !m_waitIdleQuery ) return VK_ERROR_INITIALIZATION_FAILED;
		{
			auto context{ get( m_device )->getImmediateContext() };
			context->End( m_waitIdleQuery );
			context->Flush();
		}
		BOOL data{ FALSE };
		for ( ;; )
		{
			if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
			HRESULT result{};
			{
				auto context{ get( m_device )->getImmediateContext() };
				result = context->GetData( m_waitIdleQuery, &data, UINT( sizeof( data ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			}
			if ( FAILED( result ) )
			{
				if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
				return result == E_OUTOFMEMORY ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
			}
			if ( result == S_OK && data ) return VK_SUCCESS;
			std::this_thread::sleep_for( std::chrono::microseconds{ 50ULL } );
		}

	}

#if VK_EXT_debug_utils

	void Queue::beginDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pLabelName,
		};
	}

	void Queue::endDebugUtilsLabel()const
	{
		m_label = ashes::nullopt;
	}

	void Queue::insertDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pLabelName,
		};
	}

#endif
#if VK_EXT_debug_marker

	void Queue::debugMarkerBegin( VkDebugMarkerMarkerInfoEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pMarkerName,
		};
	}

	void Queue::debugMarkerEnd()const
	{
		m_label = ashes::nullopt;
	}

	void Queue::debugMarkerInsert( VkDebugMarkerMarkerInfoEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pMarkerName,
		};
	}

#endif

	VkResult Queue::doSubmit( Context & context
		, ArrayView< VkCommandBuffer const > const & commandBuffers )const
	{
		for ( auto const & commandBuffer : commandBuffers )
		{
			auto const & dxCommandBuffer = *get( commandBuffer );
			if ( dxCommandBuffer.getRecordingResult() != VK_SUCCESS ) return dxCommandBuffer.getRecordingResult();
			dxCommandBuffer.execute( context );
			context.uavs.clear();
		}

		return VK_SUCCESS;
	}

	bool Queue::beginTiming( Context & context, LARGE_INTEGER cpuBegin )const
	{
		if ( !m_timingEnabled ) return false;
		pollTiming( context );
		auto & slot = m_timingSlots[m_timingNext];
		if ( slot.pending )
		{
			++m_timingDropped;
			m_timingNext = ( m_timingNext + 1u ) % uint32_t( m_timingSlots.size() );
			return false;
		}
		slot.cpuBegin = cpuBegin;
		m_activeTiming = &slot;
		context.context->Begin( slot.disjoint );
		context.context->End( slot.begin );
		return true;
	}

	void Queue::endTiming( Context & context, LARGE_INTEGER cpuEnd )const
	{
		if ( !m_activeTiming ) return;
		auto & slot = *m_activeTiming;
		slot.cpuEnd = cpuEnd;
		context.context->End( slot.end );
		context.context->End( slot.disjoint );
		slot.pending = true;
		m_activeTiming = nullptr;
		m_timingNext = ( m_timingNext + 1u ) % uint32_t( m_timingSlots.size() );
	}

	void Queue::pollTiming( Context & context )const
	{
		for ( auto & slot : m_timingSlots )
		{
			if ( !slot.pending || &slot == m_activeTiming ) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			auto hr = context.context->GetData( slot.disjoint, &disjoint, UINT( sizeof( disjoint ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			if ( hr == S_FALSE ) continue; // never spin or Flush for a profile sample.
			if ( FAILED( hr ) ) { slot.pending = false; ++m_timingDropped; continue; }
			UINT64 begin{};
			UINT64 end{};
			hr = context.context->GetData( slot.begin, &begin, UINT( sizeof( begin ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			if ( hr == S_FALSE ) continue;
			if ( FAILED( hr ) ) { slot.pending = false; ++m_timingDropped; continue; }
			hr = context.context->GetData( slot.end, &end, UINT( sizeof( end ) ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
			if ( hr == S_FALSE ) continue;
			if ( FAILED( hr ) || disjoint.Disjoint || !disjoint.Frequency || end < begin )
			{
				slot.pending = false;
				if ( disjoint.Disjoint ) ++m_timingDisjoint; else ++m_timingDropped;
				continue;
			}
			auto const gpuMs = ( double( end - begin ) * 1000.0 ) / double( disjoint.Frequency );
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency( &frequency );
			auto const cpuMs = ( double( slot.cpuEnd.QuadPart - slot.cpuBegin.QuadPart ) * 1000.0 ) / double( frequency.QuadPart );
			m_gpuTimingSamples.push_back( gpuMs );
			m_cpuTimingSamples.push_back( cpuMs );
			slot.pending = false;
			if ( m_gpuTimingSamples.size() == 120u )
			{
				std::fprintf( stderr, "[RW-GPU-TIMING] samples=120 dropped=%llu disjoint=%llu gpu_ms_median=%.3f gpu_ms_p90=%.3f cpu_submit_ms_median=%.3f\n"
					, static_cast< unsigned long long >( m_timingDropped )
					, static_cast< unsigned long long >( m_timingDisjoint )
					, percentile( m_gpuTimingSamples, 0.50 )
					, percentile( m_gpuTimingSamples, 0.90 )
					, percentile( m_cpuTimingSamples, 0.50 ) );
				m_gpuTimingSamples.clear();
				m_cpuTimingSamples.clear();
			}
		}
	}

	void Queue::releaseTiming()noexcept
	{
		for ( auto & slot : m_timingSlots )
		{
			safeRelease( slot.disjoint );
			safeRelease( slot.begin );
			safeRelease( slot.end );
		}
	}
}
