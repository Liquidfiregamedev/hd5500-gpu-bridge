/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Core/D3D11Device.hpp"

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
#include "Miscellaneous/D3D11QueryPool.hpp"
#include "Pipeline/D3D11PipelineLayout.hpp"
#include "RenderPass/D3D11RenderPass.hpp"
#include "Shader/D3D11ShaderModule.hpp"
#include "Sync/D3D11Event.hpp"
#include "Sync/D3D11Fence.hpp"
#include "Sync/D3D11Semaphore.hpp"

#include "ashesd3d11_api.hpp"

#include <ashes/common/Hash.hpp>

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

		for ( auto & [_, staging] : m_stagingTextures )
		{
			deallocate( staging.first, getAllocationCallbacks() );
			deallocate( staging.second, getAllocationCallbacks() );
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

		m_debug->ReportLiveDeviceObjects( D3D11_RLDO_DETAIL );
		safeRelease( m_debug );

#endif
	}

	DeviceContextLock Device::getImmediateContext()const
	{
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

	VkImage Device::getStagingImage( VkImage image
		, VkDeviceMemory & memory )
	{
		auto key = makeKey( get( image )->getType()
			, get( image )->getFormat()
			, get( image )->getDimensions()
			, get( image )->getMipmapLevels() );
		auto it = m_stagingTextures.find( key );

		if ( it == m_stagingTextures.end() )
		{
			VkImage result;
			allocate( result
				, getAllocationCallbacks()
				, get( this )
				, VkImageCreateInfo
				{
					VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
					nullptr,
					0u,
					get( image )->getType(),
					get( image )->getFormat(),
					get( image )->getDimensions(),
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
			auto requirements = get( result )->getMemoryRequirements();
			uint32_t deduced = deduceMemoryType( requirements.memoryTypeBits
				, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT );
			VkDeviceMemory resMem;
			allocate( resMem
				, getAllocationCallbacks()
				, get( this )
				, VkMemoryAllocateInfo
				{
					VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					nullptr,
					requirements.size,
					deduced
				} );
			get( result )->bindMemory( resMem, 0u );
			it = m_stagingTextures.try_emplace( key, result, resMem ).first;
		}

		memory = it->second.second;
		return it->second.first;
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

		if ( hr == S_OK )
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

		if ( hr == S_OK )
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
		auto context = getImmediateContext();
		context->End( m_waitIdleQuery );
		context->Flush();
		BOOL data{ FALSE };

		while ( ( S_FALSE == context->GetData( m_waitIdleQuery
				, &data
				, UINT( sizeof( data ) )
				, 0u ) )
			&& !data )
		{
			std::this_thread::sleep_for( std::chrono::microseconds{ 1ULL } );
		}

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

		void Device::onCheckHResultCommand( HRESULT hresult
			, std::string const & message )const
		{
			if ( FAILED( hresult ) && m_d3dDevice )
			{
				try
				{
				static std::atomic_uint errors{};
				auto diagnostic = std::getenv( "RW_HD5500_D3D_DIAGNOSTICS" );
				if ( diagnostic && diagnostic[0] == '1' && errors.fetch_add( 1u ) < 32u )
				{
					auto const reason = m_d3dDevice->GetDeviceRemovedReason();
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
					std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL] tick=%llu operation=%s callHr=0x%08lX removedReason=0x%08lX reasonName=%s\n"
						, static_cast< unsigned long long >( GetTickCount64() ), message.c_str()
						, static_cast< unsigned long >( hresult ), static_cast< unsigned long >( reason ), name );
					ID3D11InfoQueue * queue{};
					auto const queueHr = m_d3dDevice->QueryInterface( __uuidof( ID3D11InfoQueue ), reinterpret_cast< void ** >( &queue ) );
					if ( SUCCEEDED( queueHr ) && queue )
					{
						auto const count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
						unsigned printed{};
						for ( UINT64 index = 0u; index < count && index < 256u && printed < 64u; ++index )
						{
							SIZE_T bytes{};
							if ( FAILED( queue->GetMessage( index, nullptr, &bytes ) ) || bytes < sizeof( D3D11_MESSAGE ) || bytes > 65536u ) continue;
							std::vector< uint64_t > storage( ( bytes + 7u ) / 8u );
							auto nativeMessage = reinterpret_cast< D3D11_MESSAGE * >( storage.data() );
							if ( SUCCEEDED( queue->GetMessage( index, nativeMessage, &bytes ) ) && nativeMessage->Severity <= D3D11_MESSAGE_SEVERITY_WARNING )
							{
								std::fprintf( stderr, "[RW-GPU-D3D-MESSAGE] index=%llu id=%u severity=%u text=%.*s\n"
									, static_cast< unsigned long long >( index ), unsigned( nativeMessage->ID ), unsigned( nativeMessage->Severity )
									, int( std::min< SIZE_T >( nativeMessage->DescriptionByteLength, 1536u ) ), nativeMessage->pDescription ? nativeMessage->pDescription : "" );
								++printed;
							}
						}
						std::fprintf( stderr, "[RW-GPU-D3D-QUEUE] stored=%llu warningErrorPrinted=%u\n", static_cast< unsigned long long >( count ), printed );
						queue->Release();
					}
					else std::fprintf( stderr, "[RW-GPU-D3D-QUEUE] unavailable hr=0x%08lX; removedReason above is still a real device query\n", static_cast< unsigned long >( queueHr ) );
					std::fflush( stderr );
				}
				}
				catch ( ... )
				{
					std::fprintf( stderr, "[RW-GPU-DEVICE-FAIL] optional diagnostic capture failed; original HRESULT 0x%08lX remains unchanged\n", static_cast< unsigned long >( hresult ) );
					std::fflush( stderr );
				}
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
			if ( diagnosticEnabled ) flags |= D3D11_CREATE_DEVICE_DEBUG;
			D3D_FEATURE_LEVEL supportedFeatureLevel = get( m_physicalDevice )->getFeatureLevel();
			{
				lock_type lock{ m_mtxDeviceContext };
				auto hr = D3D11CreateDevice( nullptr
					, D3D_DRIVER_TYPE_HARDWARE
					, nullptr
					, flags
				, &supportedFeatureLevel
				, 1
				, D3D11_SDK_VERSION
					, &m_d3dDevice
					, &m_featureLevel
					, &m_deviceContext );
				if ( diagnosticEnabled && hr == DXGI_ERROR_SDK_COMPONENT_MISSING )
				{
					std::fprintf( stderr, "[RW-GPU-D3D-INIT] optional native debug layer not installed; retaining the same hardware path without it\n" );
					flags &= ~D3D11_CREATE_DEVICE_DEBUG;
					hr = D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags
						, &supportedFeatureLevel, 1u, D3D11_SDK_VERSION, &m_d3dDevice, &m_featureLevel, &m_deviceContext );
				}
				if ( diagnosticEnabled )
				{
					std::fprintf( stderr, "[RW-GPU-D3D-INIT] hardwareCreateHr=0x%08lX nativeDebug=%u featureLevel=0x%X\n"
						, static_cast< unsigned long >( hr ), ( flags & D3D11_CREATE_DEVICE_DEBUG ) ? 1u : 0u, unsigned( m_featureLevel ) );
					std::fflush( stderr );
				}
			}

			if ( m_d3dDevice )
			{
				if ( diagnosticEnabled )
				{
					ID3D11InfoQueue * queue{};
					if ( SUCCEEDED( m_d3dDevice->QueryInterface( __uuidof( ID3D11InfoQueue ), reinterpret_cast< void ** >( &queue ) ) ) && queue )
					{
						queue->SetMessageCountLimit( 256u );
						queue->Release();
					}
				}
	#if !defined( NDEBUG )

				m_d3dDevice->QueryInterface( __uuidof( ID3D11Debug ), reinterpret_cast< void ** >( &m_debug ) );

#endif

			dxDebugName( m_d3dDevice, Device );

			D3D11_QUERY_DESC desc{};
			desc.Query = D3D11_QUERY_EVENT;
			m_d3dDevice->CreateQuery( &desc, &m_waitIdleQuery );
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
