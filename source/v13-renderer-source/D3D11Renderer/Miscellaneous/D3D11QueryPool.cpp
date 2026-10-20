#include "Miscellaneous/D3D11QueryPool.hpp"

#include "Core/D3D11Device.hpp"

#include "ashesd3d11_api.hpp"

#include <cstring>

namespace ashes::D3D11_NAMESPACE
{
	namespace qurypool
	{
		static D3D11_QUERY convert( VkQueryType type )
		{
			switch ( type )
			{
			case VK_QUERY_TYPE_OCCLUSION:
				return D3D11_QUERY_OCCLUSION;
			case VK_QUERY_TYPE_PIPELINE_STATISTICS:
				return D3D11_QUERY_PIPELINE_STATISTICS;
			case VK_QUERY_TYPE_TIMESTAMP:
				return D3D11_QUERY_TIMESTAMP;
			default:
				assert( false );
				return D3D11_QUERY_TIMESTAMP;
			}
		}

		static UINT64 getPipelineStatistic( uint32_t index
			, D3D11_QUERY_DATA_PIPELINE_STATISTICS const & stats )
		{
			switch ( index )
			{
			case 0:
				return stats.IAVertices;
			case 1:
				return stats.IAPrimitives;
			case 2:
				return stats.VSInvocations;
			case 3:
				return stats.CInvocations;
			case 4:
				return stats.CPrimitives;
			case 5:
				return stats.PSInvocations;
			case 6:
				return stats.GSPrimitives;
			case 7:
				return stats.GSInvocations;
			case 8:
				return stats.HSInvocations;
			case 9:
				return stats.DSInvocations;
			case 10:
				return stats.CSInvocations;
			default:
				return 0ULL;
			}
		}

		static uint32_t adjustQueryCount( uint32_t count
			, VkQueryType type )
		{
			if ( type != VK_QUERY_TYPE_PIPELINE_STATISTICS )
			{
				return count;
			}

			return 1u;
		}
	}

	QueryPool::QueryPool( VkDevice device
		, VkQueryPoolCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
	{
		D3D11_QUERY_DESC desc{};
		desc.MiscFlags = 0u;
		desc.Query = qurypool::convert( m_createInfo.queryType );
		m_queries.resize( qurypool::adjustQueryCount( m_createInfo.queryCount, m_createInfo.queryType ) );

		for ( auto & query : m_queries )
		{
			if ( auto hr = get( m_device )->getDevice()->CreateQuery( &desc, &query );
				checkError( m_device, hr, "CreateQuery" ) )
			{
				dxDebugName( query, Query );
			}
		}

		switch ( m_createInfo.queryType )
		{
		case VK_QUERY_TYPE_OCCLUSION:
			m_data.resize( sizeof( uint64_t ) );
			getUint32 = [this]( uint32_t )
			{
				return uint32_t( *reinterpret_cast< uint64_t * >( m_data.data() ) );
			};
			getUint64 = [this]( uint32_t )
			{
				return *reinterpret_cast< uint64_t * >( m_data.data() );
			};
			break;

		case VK_QUERY_TYPE_PIPELINE_STATISTICS:
			m_data.resize( sizeof( D3D11_QUERY_DATA_PIPELINE_STATISTICS ) );
			getUint32 = [this]( uint32_t index )
			{
				D3D11_QUERY_DATA_PIPELINE_STATISTICS data = *reinterpret_cast< D3D11_QUERY_DATA_PIPELINE_STATISTICS * >( m_data.data() );
				return uint32_t( qurypool::getPipelineStatistic( index, data ) );
			};
			getUint64 = [this]( uint32_t index )
			{
				D3D11_QUERY_DATA_PIPELINE_STATISTICS data = *reinterpret_cast< D3D11_QUERY_DATA_PIPELINE_STATISTICS * >( m_data.data() );
				return qurypool::getPipelineStatistic( index, data );
			};
			break;

		case VK_QUERY_TYPE_TIMESTAMP:
			m_data.resize( sizeof( uint64_t ) );
			getUint32 = [this]( uint32_t )
			{
				return uint32_t( *reinterpret_cast< uint64_t * >( m_data.data() ) );
			};
			getUint64 = [this]( uint32_t )
			{
				return *reinterpret_cast< uint64_t * >( m_data.data() );
			};
			break;

		default:
			break;
		}
	}

	QueryPool::~QueryPool()noexcept
	{
		for ( auto & query : m_queries )
		{
			safeRelease( query );
		}
	}

	VkResult QueryPool::getResults( uint32_t firstQuery
		, uint32_t queryCount
		, VkDeviceSize stride
		, VkQueryResultFlags flags
		, VkDeviceSize dataSize
		, void * data )const
	{
		auto max = firstQuery + queryCount;
		assert( max <= m_queries.size() );
		std::lock_guard< std::mutex > resultLock{ m_resultsMutex };
		if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
		auto const wait = checkFlag( flags, VK_QUERY_RESULT_WAIT_BIT );
		auto const availability = checkFlag( flags, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
		auto const use64 = checkFlag( flags, VK_QUERY_RESULT_64_BIT );
		auto const values = m_createInfo.queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS
			? uint32_t( m_data.size() / sizeof( uint64_t ) )
			: 1u;
		auto const scalarSize = use64 ? VkDeviceSize( sizeof( uint64_t ) ) : VkDeviceSize( sizeof( uint32_t ) );
		auto const recordSize = scalarSize * VkDeviceSize( values + ( availability ? 1u : 0u ) );
		stride = stride ? stride : recordSize;
		auto buffer = reinterpret_cast< uint8_t * >( data );
		bool flushed{};
		VkResult overall{ VK_SUCCESS };

		auto writeScalar = [&]( uint8_t * destination, uint32_t valueIndex, uint64_t value )
		{
			if ( !buffer ) return;
			auto const byteOffset = VkDeviceSize( destination - buffer ) + scalarSize * valueIndex;
			if ( byteOffset + scalarSize > dataSize ) return;
			if ( use64 ) std::memcpy( destination + scalarSize * valueIndex, &value, sizeof( uint64_t ) );
			else
			{
				auto const value32 = uint32_t( value );
				std::memcpy( destination + scalarSize * valueIndex, &value32, sizeof( uint32_t ) );
			}
		};

		for ( auto i = firstQuery; i < max; ++i )
		{
			HRESULT hr{ S_FALSE };
			uint32_t iteration{};
			for ( ;; )
			{
				if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
				{
					auto context{ get( m_device )->getImmediateContext() };
					hr = context->GetData( m_queries[i], m_data.data(), UINT( m_data.size() ), D3D11_ASYNC_GETDATA_DONOTFLUSH );
					if ( hr == S_FALSE && wait && !flushed )
					{
						context->Flush(); // one deliberate progress kick for this API call.
						flushed = true;
					}
				}
				if ( hr != S_FALSE || !wait ) break;
				if ( iteration++ < 16u ) Sleep( 0u ); else Sleep( 1u );
			}

			auto destination = buffer ? buffer + VkDeviceSize( i - firstQuery ) * stride : nullptr;
			if ( FAILED( hr ) )
			{
				return get( m_device )->getStatus() == VK_SUCCESS
					? ( hr == E_OUTOFMEMORY ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_INITIALIZATION_FAILED )
					: VK_ERROR_DEVICE_LOST;
			}
			if ( hr == S_FALSE )
			{
				if ( availability ) writeScalar( destination, values, 0u );
				// PARTIAL permits callers to accept any native partial result.  D3D11
				// exposes none for an unavailable query, so leave value bytes untouched.
				overall = VK_NOT_READY;
				continue;
			}

			for ( uint32_t value = 0u; value < values; ++value )
			{
				writeScalar( destination, value, use64 ? getUint64( value ) : getUint32( value ) );
			}
			if ( availability ) writeScalar( destination, values, 1u );
		}

		return overall;
	}
}
