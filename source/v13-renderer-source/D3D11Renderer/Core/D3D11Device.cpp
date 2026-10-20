/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Core/D3D11Device.hpp"
#include "Command/D3D11SubmitThread.hpp"
#include "Shader/D3D11ShaderPrewarm.hpp"
#include "Miscellaneous/D3D11CompatReport.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Buffer/D3D11BufferView.hpp"
#include "Command/D3D11CommandPool.hpp"
#include "Command/D3D11Queue.hpp"
#include "Core/D3D11Surface.hpp"
#include "Core/D3D11DummyIndexBuffer.hpp"
#include "Core/D3D11PhysicalDevice.hpp"
#include "Core/D3D11Instance.hpp"
#include "Core/D3D11SwapChain.hpp"
#include "Descriptor/D3D11DescriptorPool.hpp"
#include "Descriptor/D3D11DescriptorSetLayout.hpp"
#include "Image/D3D11Sampler.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11DeviceMemory.hpp"
#include "Miscellaneous/D3D11MemoryTrace.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "Miscellaneous/D3D11RenderDiagnostics.hpp"
#include "Shader/D3D11ShaderBytecodeCache.hpp"
#include "Miscellaneous/D3D11QueryPool.hpp"
#include "Pipeline/D3D11PipelineLayout.hpp"
#include "RenderPass/D3D11RenderPass.hpp"
#include "Shader/D3D11ShaderModule.hpp"
#include "Sync/D3D11Event.hpp"
#include "Sync/D3D11Fence.hpp"
#include "Sync/D3D11Semaphore.hpp"

#include "ashesd3d11_api.hpp"

#include <ashes/common/Hash.hpp>
#include <ashes/common/Exception.hpp>

#include <d3dcommon.h>
#include <d3d11sdklayers.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#pragma warning( push )
#pragma warning( disable: 5262 )
#include <iomanip>
#pragma warning( pop )

namespace ashes::D3D11_NAMESPACE
{
	VkExtent3D getTexelBlockExtent( VkFormat format )
	{
		VkExtent3D texelBlockExtent{ 1u, 1u, 1u };

		if ( ashes::isCompressedFormat( format ) )
		{
			auto extent = ashes::getMinimalExtent2D( format );
			texelBlockExtent.width = extent.width;
			texelBlockExtent.height = extent.height;
		}
		else
		{
			texelBlockExtent.width = 1u;
		}

		return texelBlockExtent;
	}

	uint32_t getTexelBlockByteSize( VkExtent3D const & texelBlockExtent
		, VkFormat format )
	{
		VkDeviceSize texelBlockSize;

		if ( !isDepthStencilFormat( format ) )
		{
			texelBlockSize = getSize( texelBlockExtent, format );
		}
		else
		{
			texelBlockSize = texelBlockExtent.width;
		}

		return uint32_t( texelBlockSize );
	}

		namespace
		{
			VkResult getNativeFailureResult( HRESULT hr )noexcept
			{
				if ( hr == DXGI_ERROR_DEVICE_HUNG
					|| hr == DXGI_ERROR_DEVICE_REMOVED
					|| hr == DXGI_ERROR_DEVICE_RESET
					|| hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR )
				{
					return VK_ERROR_DEVICE_LOST;
				}

				return hr == E_OUTOFMEMORY
					? VK_ERROR_OUT_OF_DEVICE_MEMORY
					: VK_ERROR_INITIALIZATION_FAILED;
			}

			size_t makeKey( VkImageType type
			, VkFormat format
			, VkExtent3D const & extent
			, uint32_t mipLevels )
		{
			auto result = std::hash< VkImageType >{}( type );
			ashes::hashCombine( result, format );
			ashes::hashCombine( result, extent.width );
			ashes::hashCombine( result, extent.height );
			ashes::hashCombine( result, extent.depth );
			ashes::hashCombine( result, mipLevels );
			return result;
		}

		void doCheckEnabledExtensions( VkPhysicalDevice physicalDevice
			, ashes::ArrayView< char const * const > const & extensions )
		{
			auto const & available = get( physicalDevice )->enumerateExtensionProperties();

			for ( auto extension : extensions )
			{
				if ( available.end() == std::find_if( available.begin()
					, available.end()
					, [&extension]( VkExtensionProperties const & lookup )
					{
						return lookup.extensionName == std::string{ extension };
					} ) )
				{
					throw ExtensionNotPresentException{ extension };
				}
			}
		}

		bool doHasEnabledExtensions( VkPhysicalDevice physicalDevice
			, ashes::ArrayView< char const * const > const & extensions )
		{
			try
			{
				doCheckEnabledExtensions( physicalDevice, extensions );
				return true;
			}
			catch ( ExtensionNotPresentException & )
			{
				return false;
			}
		}
	}

	namespace stagingcache
	{
		// RW_HD5500_STAGING_CACHE_MB: cached upload staging budget (default 256 MiB;
		// 0 disables caching so every staging image belongs to its command only).
		static uint64_t capBytes()
		{
			static uint64_t const value = []()
			{
				char text[16]{};
				auto const length = GetEnvironmentVariableA( "RW_HD5500_STAGING_CACHE_MB", text, DWORD( sizeof( text ) ) );
				uint64_t mib = 256u;
				if ( length > 0u && length < sizeof( text ) )
				{
					mib = std::strtoull( text, nullptr, 10 );
				}
				return mib * 1024ull * 1024ull;
			}();
			return value;
		}

		static void summary( StagingCacheStats const & stats )noexcept
		{
			std::fprintf( stderr, "[RW-GPU-STAGING] cap=%llu liveBytes=%llu peakBytes=%llu entries=%llu hits=%llu misses=%llu evictions=%llu uncached=%llu\n"
				, static_cast< unsigned long long >( capBytes() )
				, static_cast< unsigned long long >( stats.bytes )
				, static_cast< unsigned long long >( stats.peakBytes )
				, static_cast< unsigned long long >( stats.entries )
				, static_cast< unsigned long long >( stats.hits )
				, static_cast< unsigned long long >( stats.misses )
				, static_cast< unsigned long long >( stats.evictions )
				, static_cast< unsigned long long >( stats.uncached ) );
			std::fflush( stderr );
		}
	}

	Device::Device( VkInstance instance
		, VkPhysicalDevice physicalDevice
		, VkAllocationCallbacks const * callbacks
		, VkDeviceCreateInfo createInfos )
		: m_instance{ instance }
		, m_physicalDevice{ physicalDevice }
		, m_callbacks{ callbacks }
		, m_createInfos{ std::move( createInfos ) }
	{
		doCheckEnabledExtensions( physicalDevice
			, ashes::makeArrayView( m_createInfos.ppEnabledExtensionNames, m_createInfos.enabledExtensionCount ) );
		doCreateD3D11Device();
		doCreateDummyIndexBuffer();
		doCreateQueues();
		allocate( m_sampler
			, getAllocationCallbacks()
			, get( this )
			, VkSamplerCreateInfo
			{
				VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				nullptr,
				0u,
				VK_FILTER_NEAREST,
				VK_FILTER_NEAREST,
				VK_SAMPLER_MIPMAP_MODE_NEAREST,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				0.0f,
				VK_FALSE,
				1.0f,
				VK_FALSE,
				VK_COMPARE_OP_ALWAYS,
				0.0f,
				1.0f,
				VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
				VK_FALSE,
			} );
		get( m_physicalDevice )->logMemoryBudget( "device-create" );
	}

	Device::~Device()noexcept
	{
		for ( auto const & [_, creates] : m_queues )
		{
			for ( auto queue : creates.queues )
			{
				deallocateNA( queue );
			}
		}
		Fence::reportDiagnostics();

		stagingcache::summary( m_stagingStats );
		// Entries free their image/memory once the last reference goes away.
		m_stagingTextures.clear();
		try
		{
			get( m_physicalDevice )->logMemoryBudget( "device-destroy" );
			memtrace::summary( "device-destroy" );
			rdiag::summary();
			shadercache::summary();
		}
		catch ( ... )
		{
		}

		deallocate( m_sampler, getAllocationCallbacks() );
		deallocate( m_dummyIndexed.memory, getAllocationCallbacks() );
		deallocate( m_dummyIndexed.buffer, getAllocationCallbacks() );

		try
		{
			lock_type lock{ m_mtxDeviceContext };
			safeRelease( m_deviceContext );
		}
		catch ( ... )
		{
			// What to do here ?
		}

		safeRelease( m_waitIdleQuery );
		safeRelease( m_d3dDevice );

	#if !defined( NDEBUG )

		if ( m_debug )
		{
			m_debug->ReportLiveDeviceObjects( D3D11_RLDO_DETAIL );
		}
		safeRelease( m_debug );

	#endif
	}

	VkResult Device::getStatus()const noexcept
	{
		// D3D11's only authoritative removal state is queried from the live native
		// device; cached HRESULTs are not sufficient after a TDR/reset.
		return m_d3dDevice && m_d3dDevice->GetDeviceRemovedReason() == S_OK
			? VK_SUCCESS
			: VK_ERROR_DEVICE_LOST;
	}

	void Device::requireHealthy( char const * operation )const
	{
		if ( getStatus() == VK_SUCCESS )
		{
			return;
		}

		auto const reason = m_d3dDevice
			? m_d3dDevice->GetDeviceRemovedReason()
			: E_FAIL;
		onCheckHResultCommand( reason, operation ? operation : "D3D11 device health check" );
		throw Exception{ VK_ERROR_DEVICE_LOST
			, std::string{ operation ? operation : "D3D11 operation" } + " rejected: native device is removed" };
	}

	DeviceContextLock Device::getImmediateContext()const
	{
		// v13.58: a game thread touching the context first waits for queued submit-thread work.
		submitthread::drain();
		return getImmediateContextNoDrain();
	}

	DeviceContextLock Device::getImmediateContextNoDrain()const
	{
		requireHealthy( "Acquire immediate D3D11 context" );

		if ( !m_deviceContext )
		{
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "D3D11 immediate context is unavailable" };
		}

		return DeviceContextLock{ m_deviceContext, m_mtxDeviceContext };
	}

	bool Device::hasExtension( std::string_view extension )const
	{
		char const * const version = extension.data();
		return doHasEnabledExtensions( m_physicalDevice
			, ashes::makeArrayView( &version, 1u ) );
	}

	VkPhysicalDeviceLimits const & Device::getLimits()const
	{
		return get( m_physicalDevice )->getProperties().limits;
	}

	StagingImageEntry::~StagingImageEntry()noexcept
	{
		if ( device )
		{
			auto callbacks = get( device )->getAllocationCallbacks();
			deallocate( image, callbacks );
			deallocate( memory, callbacks );
		}
	}

	VkImage Device::getStagingImage( VkImage image
		, VkDeviceMemory & memory
		, std::shared_ptr< StagingImageEntry > * owner )
	{
		requireHealthy( "Create staging image" );

		auto const extent = get( image )->getDimensions();
		std::array< uint32_t, 6u > const key
		{
			uint32_t( get( image )->getType() ),
			uint32_t( get( image )->getFormat() ),
			extent.width,
			extent.height,
			extent.depth,
			get( image )->getMipmapLevels(),
		};
		auto const cap = stagingcache::capBytes();
		lock_type lock{ m_mtxStaging };
		++m_stagingTick;

		if ( auto it = m_stagingTextures.find( key ); it != m_stagingTextures.end() )
		{
			++m_stagingStats.hits;
			it->second->lastUse = m_stagingTick;
			memory = it->second->memory;
			if ( owner ) *owner = it->second;
			return it->second->image;
		}

		++m_stagingStats.misses;
		auto entry = std::make_shared< StagingImageEntry >();
		entry->device = get( this );
		auto result = allocate( entry->image
			, getAllocationCallbacks()
			, get( this )
			, VkImageCreateInfo
			{
				VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				nullptr,
				0u,
				get( image )->getType(),
				get( image )->getFormat(),
				extent,
				get( image )->getMipmapLevels(),
				1u, // layerCount, we process images layer per layer.
				VK_SAMPLE_COUNT_1_BIT,
				VK_IMAGE_TILING_LINEAR,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_SHARING_MODE_EXCLUSIVE,
				0u,
				nullptr,
				VK_IMAGE_LAYOUT_UNDEFINED,
			} );
		if ( result != VK_SUCCESS || !entry->image )
		{
			entry->image = nullptr;
			throw Exception{ result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result, "Staging image creation" };
		}
		auto requirements = get( entry->image )->getMemoryRequirements();
		uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
			, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );
		result = allocate( entry->memory
			, getAllocationCallbacks()
			, get( this )
			, VkMemoryAllocateInfo
			{
				VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				nullptr,
				requirements.size,
				deduced
			} );
		if ( result != VK_SUCCESS || !entry->memory )
		{
			entry->memory = nullptr;
			throw Exception{ result == VK_SUCCESS ? VK_ERROR_OUT_OF_DEVICE_MEMORY : result, "Staging image memory allocation" };
		}
		result = get( entry->image )->bindMemory( entry->memory, 0u );
		if ( result != VK_SUCCESS )
		{
			throw Exception{ result, "Staging image memory binding" };
		}
		entry->bytes = requirements.size;
		entry->lastUse = m_stagingTick;
		memory = entry->memory;
		auto const resultImage = entry->image;

		if ( !owner )
		{
			// No owner keeps it alive: it must stay cached for the device lifetime.
			m_stagingStats.bytes += entry->bytes;
			++m_stagingStats.entries;
			m_stagingStats.peakBytes = std::max( m_stagingStats.peakBytes, m_stagingStats.bytes );
			m_stagingTextures.emplace( key, std::move( entry ) );
			return resultImage;
		}

		if ( !cap || entry->bytes > cap )
		{
			// Caching disabled or image larger than the whole budget: owned by the
			// recording command only and freed with it.
			++m_stagingStats.uncached;
			*owner = std::move( entry );
			return resultImage;
		}

		// Evict least recently used entries. Recorded commands keep their own
		// references, so an evicted image is only freed once nothing uses it.
		while ( m_stagingStats.bytes + entry->bytes > cap && !m_stagingTextures.empty() )
		{
			auto lru = m_stagingTextures.begin();
			for ( auto it = m_stagingTextures.begin(); it != m_stagingTextures.end(); ++it )
			{
				if ( it->second->lastUse < lru->second->lastUse ) lru = it;
			}
			m_stagingStats.bytes -= lru->second->bytes;
			--m_stagingStats.entries;
			++m_stagingStats.evictions;
			m_stagingTextures.erase( lru );
		}

		m_stagingStats.bytes += entry->bytes;
		++m_stagingStats.entries;
		m_stagingStats.peakBytes = std::max( m_stagingStats.peakBytes, m_stagingStats.bytes );
		*owner = entry;
		m_stagingTextures.emplace( key, std::move( entry ) );
		return resultImage;
	}

	void Device::getImageSubresourceLayout( VkImage image
		, VkImageSubresource const & subresource
		, VkSubresourceLayout & layout )const
	{
		auto const format = get( image )->getFormat();
		auto const extent = getTexelBlockExtent( format );
		auto const byteSize = getTexelBlockByteSize( extent, format );
		VkDeviceSize layerBytes{};
		VkDeviceSize mipOffset{};
		layout = {};
		for ( uint32_t level = 0u; level < get( image )->getMipmapLevels(); ++level )
		{
			auto const width = getSubresourceValue( get( image )->getDimensions().width, level );
			auto const height = getSubresourceValue( get( image )->getDimensions().height, level );
			auto const depth = getSubresourceValue( get( image )->getDimensions().depth, level );
			auto rowPitch = VkDeviceSize( byteSize ) * width / ( VkDeviceSize( extent.width ) * extent.height * extent.depth );
			auto slicePitch = rowPitch * height * extent.height / ( VkDeviceSize( extent.width ) * extent.depth );
			if ( bc::isFormat( format ) )
			{
				rowPitch = bc::rowBytes( format, width );
				slicePitch = rowPitch * bc::rows( height );
			}
			auto const levelBytes = slicePitch * depth;
			if ( level < subresource.mipLevel ) { mipOffset += levelBytes; }
			if ( level == subresource.mipLevel )
			{
				layout.rowPitch = rowPitch;
				layout.depthPitch = slicePitch;
				layout.size = levelBytes;
			}
			layerBytes += levelBytes;
		}
		layout.arrayPitch = layerBytes;
		layout.offset = subresource.arrayLayer * layerBytes + mipOffset;
	}

#if VK_EXT_debug_utils

	VkResult Device::setDebugUtilsObjectName( VkDebugUtilsObjectNameInfoEXT const & nameInfo )const
	{
		auto hr = S_OK;

		switch ( nameInfo.objectType )
		{
		case VK_OBJECT_TYPE_DEVICE:
			hr = get( VkDevice( nameInfo.objectHandle ) )->getDevice()->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( strnlen( nameInfo.pObjectName, 256u ) )
				, nameInfo.pObjectName );
			break;
		case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
			hr = get( VkSwapchainKHR( nameInfo.objectHandle ) )->getSwapChain()->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( strnlen( nameInfo.pObjectName, 256u ) )
				, nameInfo.pObjectName );
			break;
		case VK_OBJECT_TYPE_BUFFER:
			get( VkBuffer( nameInfo.objectHandle ) )->setDebugName( nameInfo.pObjectName );
			break;
		case VK_OBJECT_TYPE_IMAGE:
			get( VkImage( nameInfo.objectHandle ) )->setDebugName( nameInfo.pObjectName );
			break;
		default:
			{
				auto object = getObject( nameInfo.objectHandle, nameInfo.objectType );

				if ( object )
				{
					hr = object->SetPrivateData( WKPDID_D3DDebugObjectName
						, UINT( strnlen( nameInfo.pObjectName, 256u ) )
						, nameInfo.pObjectName );
				}
			}
				break;
			}

			if ( hr == S_OK && envflags::textureTrace() )   // v13.51: cached
			{
				std::clog << "0x" << std::hex << std::setfill( '0' ) << std::setw( 16u ) << nameInfo.objectHandle << " - " << nameInfo.pObjectName << "\n";
			}

		return checkError( get( this ), hr, "SetPrivateData" )
			? VK_SUCCESS
			: VK_ERROR_INVALID_DEVICE_ADDRESS_EXT;
	}

	VkResult Device::setDebugUtilsObjectTag( VkDebugUtilsObjectTagInfoEXT const & )const
	{
		return VK_SUCCESS;
	}

	void Device::submitDebugUtilsMessenger( VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity
		, VkDebugUtilsMessageTypeFlagsEXT messageTypes
		, VkDebugUtilsMessengerCallbackDataEXT const & callbackData )const
	{
		get( m_instance )->submitDebugUtilsMessenger( messageSeverity
			, messageTypes
			, callbackData );
	}

	void Device::onSubmitDebugUtilsMessenger( VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity
		, VkDebugUtilsMessageTypeFlagsEXT messageTypes
		, VkDebugUtilsMessengerCallbackDataEXT const & callbackData )const
	{
		get( m_instance )->onSubmitDebugUtilsMessenger( messageSeverity
			, messageTypes
			, callbackData );
	}

#endif
#if VK_EXT_debug_marker

	VkResult Device::debugMarkerSetObjectTag( VkDebugMarkerObjectTagInfoEXT const & )const
	{
		return VK_SUCCESS;
	}

	VkResult Device::debugMarkerSetObjectName( VkDebugMarkerObjectNameInfoEXT const & nameInfo )const
	{
		auto hr = S_OK;

		switch ( nameInfo.objectType )
		{
		case VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT:
			hr = get( VkDevice( nameInfo.object ) )->getDevice()->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( strnlen( nameInfo.pObjectName, 256u ) )
				, nameInfo.pObjectName );
			break;
		case VK_DEBUG_REPORT_OBJECT_TYPE_SWAPCHAIN_KHR_EXT:
			hr = get( VkSwapchainKHR( nameInfo.object ) )->getSwapChain()->SetPrivateData( WKPDID_D3DDebugObjectName
				, UINT( strnlen( nameInfo.pObjectName, 256u ) )
				, nameInfo.pObjectName );
			break;
		case VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT:
			get( VkBuffer( nameInfo.object ) )->setDebugName( nameInfo.pObjectName );
			break;
		case VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT:
			get( VkImage( nameInfo.object ) )->setDebugName( nameInfo.pObjectName );
			break;
		default:
			{
				auto object = getObject( nameInfo.object, nameInfo.objectType );

				if ( object )
				{
					hr = object->SetPrivateData( WKPDID_D3DDebugObjectName
						, UINT( strnlen( nameInfo.pObjectName, 256u ) )
						, nameInfo.pObjectName );
				}
			}
				break;
			}

			if ( hr == S_OK && envflags::textureTrace() )   // v13.51: cached
			{
				std::clog << "0x" << std::hex << std::setfill( '0' ) << std::setw( 16u ) << nameInfo.object << " - " << nameInfo.pObjectName << "\n";
			}

		return checkError( get( this ), hr, "SetPrivateData" )
			? VK_SUCCESS
			: VK_ERROR_INVALID_DEVICE_ADDRESS_EXT;
	}

#endif
#if VK_EXT_debug_report

	void Device::reportMessage( VkDebugReportFlagsEXT flags
		, VkDebugReportObjectTypeEXT objectType
		, uint64_t object
		, size_t location
		, int32_t messageCode
		, const char * pLayerPrefix
		, const char * pMessage )const noexcept
	{
		get( m_instance )->reportMessage( flags
			, objectType
			, object
			, location
			, messageCode
			, pLayerPrefix
			, pMessage );
	}

	void Device::onReportMessage( VkDebugReportFlagsEXT flags
		, VkDebugReportObjectTypeEXT objectType
		, uint64_t object
		, size_t location
		, int32_t messageCode
		, const char * pLayerPrefix
		, const char * pMessage )const noexcept
	{
		get( m_instance )->onReportMessage( flags
			, objectType
			, object
			, location
			, messageCode
			, pLayerPrefix
			, pMessage );
	}

#endif

	VkQueue Device::getQueue( uint32_t familyIndex
		, uint32_t index )const
	{
		auto it = m_queues.find( familyIndex );

		if ( it == m_queues.end() )
		{
			get( m_instance )->onReportMessage( VK_DEBUG_REPORT_ERROR_BIT_EXT
				, VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT
				, uint64_t( get( this ) )
				, 0u
				, VK_ERROR_INCOMPATIBLE_DRIVER
				, "Direct3D11"
				, "Couldn't find family index within created queues" );
			return nullptr;
		}

		if ( it->second.queues.size() <= index )
		{
			get( m_instance )->onReportMessage( VK_DEBUG_REPORT_ERROR_BIT_EXT
				, VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT
				, uint64_t( get( this ) )
				, 0u
				, VK_ERROR_INCOMPATIBLE_DRIVER
				, "Direct3D11"
				, "Couldn't find queue with wanted index within its family" );
			return nullptr;
		}

		return it->second.queues[index];
	}

	VkResult Device::waitIdle()const
	{
		if ( auto const status = getStatus(); status != VK_SUCCESS )
		{
			return status;
		}

		auto context = getImmediateContext();
		context->End( m_waitIdleQuery );
		context->Flush();
		BOOL data{ FALSE };
		HRESULT hr{};

		do
		{
			hr = context->GetData( m_waitIdleQuery, &data, UINT( sizeof( data ) ), 0u );
			if ( FAILED( hr ) )
			{
				onCheckHResultCommand( hr, "GetData(wait idle)" );
				return getStatus() == VK_SUCCESS
					? getNativeFailureResult( hr )
					: VK_ERROR_DEVICE_LOST;
			}
			std::this_thread::sleep_for( std::chrono::microseconds{ 1ULL } );
		}
		while ( hr == S_FALSE && !data );

		return data
			? VK_SUCCESS
			: VK_TIMEOUT;
	}

	void Device::onCopyToImageCommand( VkCommandBuffer cmd
		, ArrayView< VkBufferImageCopy const > const & copyInfo
		, VkBuffer src
		, VkImage dst )const
	{
		return get( m_instance )->onCopyToImageCommand( cmd, copyInfo, src, dst );
	}

	void Device::drainDebugMessages( char const * where )const noexcept
	{
		if ( !m_d3dDevice ) return;
		static std::atomic< uint32_t > printed{ 0u };
		static std::atomic< uint64_t > cursor{ 0u };
		if ( printed.load( std::memory_order_relaxed ) >= 400u ) return;
		ID3D11InfoQueue * queue{};
		if ( FAILED( m_d3dDevice->QueryInterface( __uuidof( ID3D11InfoQueue ), reinterpret_cast< void ** >( &queue ) ) ) || !queue ) return;
		auto const count = queue->GetNumStoredMessages();
		for ( auto i = cursor.load( std::memory_order_relaxed ); i < count; ++i )
		{
			SIZE_T length = 0u;
			if ( FAILED( queue->GetMessage( i, nullptr, &length ) ) || !length ) continue;
			std::vector< char > storage( length );
			auto * msg = reinterpret_cast< D3D11_MESSAGE * >( storage.data() );
			if ( FAILED( queue->GetMessage( i, msg, &length ) ) ) continue;
			if ( msg->Severity > D3D11_MESSAGE_SEVERITY_WARNING ) continue;
			if ( printed.fetch_add( 1u, std::memory_order_relaxed ) >= 400u ) break;
			std::fprintf( stderr, "[RW-D3D11-DEBUG] at=%s severity=%d id=%d: %.*s\n", where
				, int( msg->Severity ), int( msg->ID ), int( msg->DescriptionByteLength ), msg->pDescription );
		}
		cursor.store( count, std::memory_order_relaxed );
		queue->Release();
	}

	void Device::onCheckHResultCommand( HRESULT hresult
		, std::string const & message )const
	{
		if ( FAILED( hresult ) )
		{
			drainDebugMessages( message.c_str() );
			static std::atomic_uint failures{};
			auto const ordinal = failures.fetch_add( 1u, std::memory_order_relaxed ) + 1u;
			auto const reason = m_d3dDevice
				? m_d3dDevice->GetDeviceRemovedReason()
				: E_FAIL;
			char const * name = "OTHER_HRESULT";

			switch ( reason )
			{
			case S_OK: name = "S_OK_DEVICE_NOT_REMOVED"; break;
			case DXGI_ERROR_DEVICE_HUNG: name = "DXGI_ERROR_DEVICE_HUNG"; break;
			case DXGI_ERROR_DEVICE_REMOVED: name = "DXGI_ERROR_DEVICE_REMOVED"; break;
			case DXGI_ERROR_DEVICE_RESET: name = "DXGI_ERROR_DEVICE_RESET"; break;
			case DXGI_ERROR_DRIVER_INTERNAL_ERROR: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
			case DXGI_ERROR_INVALID_CALL: name = "DXGI_ERROR_INVALID_CALL"; break;
			}

			// The failure time is deliberately uncapped.  The more expensive memory
			// snapshot is bounded independently, so diagnostics never turn a native
			// failure into a synthetic success or require an exception catch path.
			std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL] failure=%u tick=%llu operation=%s callHr=0x%08lX removedReason=0x%08lX reasonName=%s\n"
				, ordinal, static_cast< unsigned long long >( GetTickCount64() ), message.c_str()
				, static_cast< unsigned long >( hresult ), static_cast< unsigned long >( reason ), name );
			if ( ordinal <= 32u )
			{
				MEMORYSTATUSEX memory{};
				memory.dwLength = sizeof( memory );
				auto const memoryValid = GlobalMemoryStatusEx( &memory );
				std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL-MEMORY] failure=%u physicalAvailable=%llu commitAvailable=%llu snapshotValid=%u\n"
					, ordinal, static_cast< unsigned long long >( memory.ullAvailPhys )
					, static_cast< unsigned long long >( memory.ullAvailPageFile ), unsigned( memoryValid != FALSE ) );
			}
			std::fflush( stderr );
		}

		return get( m_instance )->onCheckHResultCommand( hresult, message );
	}

	void Device::doCreateD3D11Device()
	{
		std::vector< D3D_FEATURE_LEVEL > requestedFeatureLevels
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
			D3D_FEATURE_LEVEL_9_3,
			D3D_FEATURE_LEVEL_9_2,
			D3D_FEATURE_LEVEL_9_1,
		};
		UINT flags = 0;

	#if !defined( NDEBUG )
			flags = D3D11_CREATE_DEVICE_DEBUG;
	#endif

				auto diagnostic = std::getenv( "RW_HD5500_D3D_DIAGNOSTICS" );
				auto const diagnosticEnabled = diagnostic && diagnostic[0] == '1';
				// Keep failure logging independently available in the normal release path.
				// An explicit zero disables optional validation; absent preserves older callers.
				auto debugLayer = std::getenv( "RW_HD5500_D3D_DEBUG_LAYER" );
				auto const debugLayerEnabled = debugLayer ? debugLayer[0] == '1' : diagnosticEnabled;
				if ( debugLayerEnabled ) flags |= D3D11_CREATE_DEVICE_DEBUG;
				D3D_FEATURE_LEVEL supportedFeatureLevel = get( m_physicalDevice )->getFeatureLevel();
				{
					lock_type lock{ m_mtxDeviceContext };
				// v13.19: create the device on the adapter the application selected
				// (vkEnumeratePhysicalDevices order = DXGI adapters). Before, the default
				// adapter was always used, so picking another GPU (dual-GPU laptops, or
				// WAYLITH_VULKAN_DEVICE="Basic Render" for Microsoft's WARP software
				// rasterizer as a reference) silently ran on the first one.
				IDXGIAdapter * chosenAdapter = get( m_physicalDevice )->getAdapter();
				auto hr = D3D11CreateDevice( chosenAdapter
					, chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE
					, nullptr
					, flags
				, &supportedFeatureLevel
				, 1
				, D3D11_SDK_VERSION
						, &m_d3dDevice
						, &m_featureLevel
						, &m_deviceContext );
					if ( debugLayerEnabled && hr == DXGI_ERROR_SDK_COMPONENT_MISSING )
					{
						std::fprintf( stderr, "[RW-GPU-D3D-INIT] optional native debug layer not installed; retaining the same hardware path without it\n" );
						flags &= ~D3D11_CREATE_DEVICE_DEBUG;
					hr = D3D11CreateDevice( chosenAdapter, chosenAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags
						, &supportedFeatureLevel, 1u, D3D11_SDK_VERSION, &m_d3dDevice, &m_featureLevel, &m_deviceContext );
				}
				{
					DXGI_ADAPTER_DESC ad{};
					if ( chosenAdapter ) chosenAdapter->GetDesc( &ad );
					std::fprintf( stderr, "[RW-GPU-D3D-INIT] device adapter=\"%ls\" vendor=0x%04X device=0x%04X debugLayer=%u\n"
						, chosenAdapter ? ad.Description : L"(default)", unsigned( ad.VendorId ), unsigned( ad.DeviceId ), ( flags & D3D11_CREATE_DEVICE_DEBUG ) ? 1u : 0u );
				}
				if ( SUCCEEDED( hr ) && m_d3dDevice ) compat::report( m_d3dDevice, chosenAdapter, m_featureLevel );   // v13.65
				if ( diagnosticEnabled )
				{
					std::fprintf( stderr, "[RW-GPU-D3D-INIT] hardwareCreateHr=0x%08lX nativeDebug=%u featureLevel=0x%X\n"
						, static_cast< unsigned long >( hr ), ( flags & D3D11_CREATE_DEVICE_DEBUG ) ? 1u : 0u, unsigned( m_featureLevel ) );
					std::fflush( stderr );
				}

				if ( FAILED( hr ) || !m_d3dDevice || !m_deviceContext )
				{
					auto const failure = FAILED( hr ) ? hr : E_FAIL;
					onCheckHResultCommand( failure, "D3D11CreateDevice" );
					safeRelease( m_deviceContext );
					safeRelease( m_d3dDevice );
					throw Exception{ getNativeFailureResult( failure ), "D3D11CreateDevice failed" };
				}
			}

			if ( m_d3dDevice )
			{
				if ( diagnosticEnabled )
				{
					ID3D11InfoQueue * queue{};
					if ( SUCCEEDED( m_d3dDevice->QueryInterface( __uuidof( ID3D11InfoQueue ), reinterpret_cast< void ** >( &queue ) ) ) && queue )
					{
						queue->SetMessageCountLimit( 4096u );
						queue->Release();
					}
				}
	#if !defined( NDEBUG )

				m_d3dDevice->QueryInterface( __uuidof( ID3D11Debug ), reinterpret_cast< void ** >( &m_debug ) );

#endif

			dxDebugName( m_d3dDevice, Device );

			{
				// v13.48: how many frames the CPU may run ahead of the GPU. Windows' default is 3:
				// when the GPU is the bottleneck the game then records three frames quickly and
				// blocks for a long time on the fourth (25, 25, 25, 145 ms = visible stutter).
				// 1 spreads the same GPU time evenly over every frame and lowers input lag.
				// RW_GPU_FRAME_LATENCY=1..16 (default 1; 0 = leave Windows' default).
				// v13.61: RW_GPU_PROCESS_PRIORITY=above (Bridge menu option) runs the game process at
				// "above normal" priority class: background programs (updaters, browsers, indexers) get
				// the CPU only when the game leaves time over. Process-local, ends with the game; no
				// system setting is changed. ("high"/"realtime" are deliberately not offered: they can
				// starve Windows' own input and audio threads on a 2-core CPU.)
				{
					char text[16]{};
					if ( envflags::text( "RW_GPU_PROCESS_PRIORITY", text, sizeof( text ) ) > 0u && ( text[0] == 'a' || text[0] == 'A' ) )
					{
						BOOL ok = SetPriorityClass( GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS );
						std::fprintf( stderr, "[RW-GPU-PRIORITY] game process priority class above normal (%s)\n", ok ? "set" : "failed" );
					}
				}
				// v13.73: opt the game process out of Windows' power throttling ("EcoQoS"/LowQoS). On a
				// laptop Windows may run threads of a process it considers background on the efficient,
				// slower clock; a game always wants full speed. Process-local (SetProcessInformation,
				// ProcessPowerThrottling, ControlMask = EXECUTION_SPEED, StateMask = 0 = never throttle),
				// ends with the game, no system setting changed. Windows 10 1709+; resolved at run time.
				// RW_GPU_HIGHQOS=0 = leave Windows' default.
				if ( envflags::on( "RW_GPU_HIGHQOS", true ) )
				{
					struct PowerThrottlingState { ULONG Version; ULONG ControlMask; ULONG StateMask; };
					using SetProcessInformationFn = BOOL ( WINAPI * )( HANDLE, int, LPVOID, DWORD );
					auto fn = reinterpret_cast< SetProcessInformationFn >( reinterpret_cast< void * >( GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "SetProcessInformation" ) ) );
					BOOL ok = FALSE;
					if ( fn )
					{
						PowerThrottlingState state{ 1u /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */, 0x1u /* EXECUTION_SPEED */, 0u };
						ok = fn( GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, DWORD( sizeof( state ) ) );
					}
					std::fprintf( stderr, "[RW-GPU-QOS] Windows power throttling for the game process: off (%s)\n", ok ? "set" : "not available" );
				}
				shaderprewarm::start( m_d3dDevice );   // v13.64: background shader objects from the disk cache
				UINT latency = 1u;
				char text[8]{};
				auto length = GetEnvironmentVariableA( "RW_GPU_FRAME_LATENCY", text, DWORD( sizeof( text ) ) );
				if ( length > 0u && length < sizeof( text ) ) latency = UINT( std::strtoul( text, nullptr, 10 ) );
				if ( latency > 16u ) latency = 16u;
				if ( latency )
				{
					IDXGIDevice1 * dxgi{};
					if ( SUCCEEDED( m_d3dDevice->QueryInterface( __uuidof( IDXGIDevice1 ), reinterpret_cast< void ** >( &dxgi ) ) ) && dxgi )
					{
						auto hr = dxgi->SetMaximumFrameLatency( latency );
						std::fprintf( stderr, "[RW-GPU-LATENCY] maximum frame latency %u (%s)\n", unsigned( latency ), SUCCEEDED( hr ) ? "set" : "failed" );
						dxgi->Release();
					}
				}
			}

			{
				// v13.37: partial dynamic-buffer updates (see canMapNoOverwrite*()).
				D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
				if ( SUCCEEDED( m_d3dDevice->CheckFeatureSupport( D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof( options ) ) ) )
				{
					m_noOverwriteConstantBuffer = options.MapNoOverwriteOnDynamicConstantBuffer != 0;
					m_noOverwriteBufferSrv = options.MapNoOverwriteOnDynamicBufferSRV != 0;
				}
				if ( auto env = std::getenv( "RW_GPU_DYNAMIC_NOOVERWRITE" ); env && env[0] == '0' )
				{
					m_noOverwriteConstantBuffer = false;
					m_noOverwriteBufferSrv = false;
				}
				std::fprintf( stderr, "[RW-GPU-D3D-INIT] dynamicNoOverwrite constantBuffer=%d bufferSrv=%d\n"
					, int( m_noOverwriteConstantBuffer ), int( m_noOverwriteBufferSrv ) );
			}

			D3D11_QUERY_DESC desc{};
			desc.Query = D3D11_QUERY_EVENT;
			auto const queryHr = m_d3dDevice->CreateQuery( &desc, &m_waitIdleQuery );
			if ( FAILED( queryHr ) || !m_waitIdleQuery )
			{
				auto const failure = FAILED( queryHr ) ? queryHr : E_FAIL;
				onCheckHResultCommand( failure, "CreateQuery(wait idle)" );
				safeRelease( m_waitIdleQuery );
				throw Exception{ getStatus() == VK_SUCCESS
					? getNativeFailureResult( failure )
					: VK_ERROR_DEVICE_LOST, "CreateQuery(wait idle) failed" };
			}
		}
	}

	void Device::doCreateDummyIndexBuffer()
	{
		auto size = uint32_t( sizeof( dummyIndex ) );
		auto count = uint32_t( size / sizeof( dummyIndex[0] ) );
		allocate( m_dummyIndexed.buffer
			, getAllocationCallbacks()
			, get( this )
			, VkBufferCreateInfo
			{
				VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				nullptr,
				0u,
				size,
				VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_SHARING_MODE_EXCLUSIVE,
				0u,
				nullptr,
			} );
		auto requirements = get( m_dummyIndexed.buffer )->getMemoryRequirements();
		auto deduced = deduceMemoryType( requirements.memoryTypeBits
			, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );
		allocate( m_dummyIndexed.memory
			, getAllocationCallbacks()
			, get( this )
			, VkMemoryAllocateInfo
			{
				VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				nullptr,
				requirements.size,
				deduced
			} );
		get( m_dummyIndexed.buffer )->bindMemory( m_dummyIndexed.memory, 0u );
		uint32_t * buffer{};

		if ( VK_SUCCESS == get( m_dummyIndexed.memory )->lock( 0u
			, size
			, 0u
			, reinterpret_cast< void ** >( &buffer ) ) )
		{
			std::copy( dummyIndex.begin(), std::next( dummyIndex.begin(), ptrdiff_t( count ) ), buffer );
			auto context = getImmediateContext();
			get( m_dummyIndexed.memory )->flush( context, 0, size );
			get( m_dummyIndexed.memory )->unlock( context );
		}
	}

	void Device::doCreateQueues()
	{
		for ( auto & queueCreateInfo : makeArrayView( m_createInfos.pQueueCreateInfos, m_createInfos.queueCreateInfoCount ) )
		{
			auto & queueCreates = m_queues.try_emplace( queueCreateInfo.queueFamilyIndex, queueCreateInfo ).first->second;

			for ( auto i = 0u; i < queueCreateInfo.queueCount; ++i )
			{
				VkQueue queue;
				allocateNA( queue
					, get( this )
					, queueCreates.createInfo
					, uint32_t( queueCreates.queues.size() ) );
				queueCreates.queues.emplace_back( queue );
			}
		}
	}
}
