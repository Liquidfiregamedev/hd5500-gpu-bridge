#include "Buffer/D3D11Buffer.hpp"

#include "Core/D3D11Device.hpp"
#include "Miscellaneous/D3D11DeviceMemory.hpp"
#include "Miscellaneous/D3D11MemoryTrace.hpp"

#include "ashesd3d11_api.hpp"
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	Buffer::Buffer( VkDevice device
		, VkBufferCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
	{
	}

	Buffer::~Buffer()noexcept
	{
		doClearRangeUavs();
		safeRelease( m_unorderedAccessView );
		// Pointer identity only: Vulkan permits parent memory to be freed first.
		memtrace::log( "VULKAN_BUFFER_DESTROY", m_memory, 0u, m_memoryOffset,
			m_createInfo.size, 0u, 0u, S_OK );
		if ( m_objectMemory && !m_memoryLifetime.expired() )
		{
			// Keep the parent-owned metadata alive for recorded-pointer stability.
			// Only its unused COM backing is released; parent shadow loops skip it.
			m_objectMemory->releaseResource();
		}
		m_objectMemory = nullptr;
	}

	VkMemoryRequirements Buffer::getMemoryRequirements()const
	{
		auto physicalDevice = get( get( m_device )->getPhysicalDevice() );
		VkMemoryRequirements result{};
		result.memoryTypeBits = physicalDevice->getMemoryTypeBits( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
			, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT );
		result.alignment = get( m_device )->getLimits().nonCoherentAtomSize;

		if ( checkFlag( m_createInfo.usage, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT ) )
		{
			result.alignment = get( m_device )->getLimits().minUniformBufferOffsetAlignment;
		}

		result.size = ashes::getAlignedSize( m_createInfo.size, result.alignment );
		return result;
	}

	bool Buffer::isMapped()const
	{
		assert( m_memory != nullptr );
		return get( m_memory )->isMapped();
	}

	void Buffer::copyFrom( ID3D11DeviceContext * context
		, VkBuffer src
		, D3D11_BOX const & srcBox
		, UINT dstOffset )const
	{
		VkDeviceSize size = VkDeviceSize( srcBox.right ) - srcBox.left;
		auto srcOffset = get( src )->getMemoryOffset();
		auto dstMemoryOffset = getMemoryOffset();
		auto nativeSrcOffset = get( src )->getNativeOffset();
		auto nativeDstOffset = getNativeOffset();
		assert( nativeSrcOffset + srcBox.right <= std::numeric_limits< UINT >::max() );
		assert( nativeDstOffset + dstOffset <= std::numeric_limits< UINT >::max() );
		D3D11_BOX resourceSrcBox
		{
			UINT( nativeSrcOffset + srcBox.left ),
			srcBox.top,
			srcBox.front,
			UINT( nativeSrcOffset + srcBox.right ),
			srcBox.bottom,
			srcBox.back,
		};
		++getObjectMemory().contentVersion;   // v13.74: index pool
		get( m_memory )->updateData( get( src )->m_memory
			, srcOffset + srcBox.left
			, dstMemoryOffset + dstOffset
			, size );
		context->CopySubresourceRegion( getResource()
			, 0u
			, UINT( nativeDstOffset + dstOffset )
			, 0u
			, 0u
			, get( src )->getResource()
			, 0u
			, &resourceSrcBox );
	}

	VkResult Buffer::bindMemory( VkDeviceMemory memory
		, VkDeviceSize memoryOffset )
	{
		if ( get( m_device )->getStatus() != VK_SUCCESS )
		{
			return VK_ERROR_DEVICE_LOST;
		}

		m_memory = memory;
		m_memoryOffset = memoryOffset;
		m_memoryLifetime = get( memory )->getLifetimeToken();
		auto result = get( m_memory )->bindToBuffer( get( this )
			, m_memoryOffset
			, m_objectMemory );

		if ( result != VK_SUCCESS
			|| !m_objectMemory
			|| !m_objectMemory->resource )
		{
			return result != VK_SUCCESS
				? result
				: VK_ERROR_INITIALIZATION_FAILED;
		}

		if ( !m_debugName.empty() )
		{
			m_objectMemory->resource->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( m_debugName.size() )
				, m_debugName.c_str() );
		}

		if ( checkFlag( m_createInfo.usage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT )
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			doClearRangeUavs();
			// SPIRV-Cross emits RWByteAddressBuffer for Vulkan storage buffers.
			// D3D11 raw views require DWORD units and an R32_TYPELESS UAV.
			if ( ( getNativeOffset() % sizeof( uint32_t ) ) != 0u )
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}

			auto firstElement = getNativeOffset() / sizeof( uint32_t );
			auto numElements = m_createInfo.size / sizeof( uint32_t )
				+ ( ( m_createInfo.size % sizeof( uint32_t ) ) != 0u
					? 1u
					: 0u );
			D3D11_BUFFER_DESC bufferDesc{};
			getBuffer()->GetDesc( &bufferDesc );

			if ( firstElement > std::numeric_limits< UINT >::max()
				|| numElements > std::numeric_limits< UINT >::max()
				|| numElements == 0u
				|| firstElement + numElements > VkDeviceSize( bufferDesc.ByteWidth / sizeof( uint32_t ) ) )
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}

			auto device = get( m_device )->getDevice();
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_R32_TYPELESS;
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = UINT( firstElement );
			desc.Buffer.NumElements = UINT( numElements );
			desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			auto hr = device->CreateUnorderedAccessView( m_objectMemory->resource
				, &desc
				, &m_unorderedAccessView );

			if ( FAILED( hr ) || !m_unorderedAccessView )
			{
				auto const failure = FAILED( hr ) ? hr : E_FAIL;
				checkError( m_device, failure, "CreateUnorderedAccessViewBuffer" );
				safeRelease( m_unorderedAccessView );
				if ( get( m_device )->getStatus() != VK_SUCCESS
					|| failure == DXGI_ERROR_DEVICE_HUNG
					|| failure == DXGI_ERROR_DEVICE_REMOVED
					|| failure == DXGI_ERROR_DEVICE_RESET
					|| failure == DXGI_ERROR_DRIVER_INTERNAL_ERROR )
				{
					return VK_ERROR_DEVICE_LOST;
				}
				return failure == E_OUTOFMEMORY
					? VK_ERROR_OUT_OF_DEVICE_MEMORY
					: VK_ERROR_INITIALIZATION_FAILED;
			}

			dxDebugName( m_unorderedAccessView, UnorderedAccessViewBuffer );
		}

		return result;
	}

	static std::mutex & rangeUavMutex()
	{
		static std::mutex result;
		return result;
	}

	void Buffer::doClearRangeUavs()const noexcept
	{
		std::lock_guard< std::mutex > lock{ rangeUavMutex() };
		doClearRangeUavsLocked();
	}

	void Buffer::doClearRangeUavsLocked()const noexcept
	{
		for ( auto & [key, view] : m_rangeUavs ) safeRelease( view );
		m_rangeUavs.clear();
	}

	ID3D11UnorderedAccessView * Buffer::getUnorderedAccessView( VkDeviceSize offset, VkDeviceSize range )const
	{
		static bool const enabled = []()
			{
				char const * env = std::getenv( "RW_GPU_UAV_RANGE" );
				return !( env && env[0] == '0' );
			}();
		if ( !m_unorderedAccessView ) return nullptr;
		auto size = m_createInfo.size;
		if ( range == VK_WHOLE_SIZE || offset >= size || range > size - offset ) range = offset < size ? size - offset : 0u;
		if ( !enabled || ( offset == 0u && range >= size ) || ( offset % sizeof( uint32_t ) ) != 0u || range == 0u )
		{
			return m_unorderedAccessView;
		}
		std::lock_guard< std::mutex > lock{ rangeUavMutex() };
		auto key = std::make_pair( offset, range );
		if ( auto it = m_rangeUavs.find( key ); it != m_rangeUavs.end() ) return it->second;
		if ( m_rangeUavs.size() >= 2048u )
		{
			// Commands that captured a view keep their own reference (keepAlive); the D3D
			// context keeps bound ones alive. Dropping the cache only drops our reference.
			doClearRangeUavsLocked();
		}
		D3D11_BUFFER_DESC bufferDesc{};
		getBuffer()->GetDesc( &bufferDesc );
		auto totalElements = VkDeviceSize( bufferDesc.ByteWidth / sizeof( uint32_t ) );
		auto firstElement = ( getNativeOffset() + offset ) / sizeof( uint32_t );
		auto numElements = ( range + sizeof( uint32_t ) - 1u ) / sizeof( uint32_t );
		if ( firstElement >= totalElements ) return m_unorderedAccessView;
		if ( firstElement + numElements > totalElements ) numElements = totalElements - firstElement;
		D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_R32_TYPELESS;
		desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		desc.Buffer.FirstElement = UINT( firstElement );
		desc.Buffer.NumElements = UINT( numElements );
		desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		ID3D11UnorderedAccessView * view{};
		auto hr = get( m_device )->getDevice()->CreateUnorderedAccessView( m_objectMemory->resource, &desc, &view );
		if ( FAILED( hr ) || !view )
		{
			static std::atomic< uint32_t > warned{ 0u };
			if ( warned++ < 8u ) std::fprintf( stderr, "[RW-GPU-UAV-RANGE] view at offset %llu range %llu failed (0x%08X), using the whole buffer\n"
				, (unsigned long long)offset, (unsigned long long)range, unsigned( hr ) );
			return m_unorderedAccessView;
		}
		m_rangeUavs.emplace( key, view );
		return view;
	}
}
