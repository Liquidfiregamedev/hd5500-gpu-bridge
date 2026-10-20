/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/Core/D3D11DeviceContextLock.hpp"
#include "renderer/D3D11Renderer/Core/D3D11Layer.hpp"

#include "renderer/D3D11Renderer/Buffer/D3D11Buffer.hpp"
#include "renderer/D3D11Renderer/Core/D3D11Surface.hpp"

#include <renderer/RendererCommon/IcdObject.hpp>

#include <array>
#include <map>
#include <memory>

namespace ashes::D3D11_NAMESPACE
{
	VkExtent3D getTexelBlockExtent( VkFormat format );
	uint32_t getTexelBlockByteSize( VkExtent3D const & texelBlockExtent
		, VkFormat format );

	// v12: one cached LINEAR upload staging image. Destroyed when the cache and
	// every recorded command that references it have released it.
	struct StagingImageEntry
	{
		VkDevice device{};
		VkImage image{};
		VkDeviceMemory memory{};
		uint64_t bytes{};
		uint64_t lastUse{};
		~StagingImageEntry()noexcept;
	};

	struct StagingCacheStats
	{
		uint64_t bytes{};
		uint64_t peakBytes{};
		uint64_t entries{};
		uint64_t hits{};
		uint64_t misses{};
		uint64_t evictions{};
		uint64_t uncached{};
	};

	template< typename T >
	T getSubresourceValue( T value, uint32_t mipLevel )
	{
		return T( value >> mipLevel );
	};

	class Device
		: public ashes::IcdObject
	{
		using lock_type = std::unique_lock< std::mutex >;

	public:
		Device( VkInstance instance
			, VkPhysicalDevice physicalDevice
			, VkAllocationCallbacks const * callbacks
			, VkDeviceCreateInfo createInfos );
		~Device()noexcept;

		VkResult getStatus()const noexcept;
		void requireHealthy( char const * operation )const;
		// v13.58: like getImmediateContext() but without waiting for the submit thread.
		DeviceContextLock getImmediateContextNoDrain()const;
		DeviceContextLock getImmediateContext()const;

		bool hasExtension( std::string_view extension )const;
		VkPhysicalDeviceLimits const & getLimits()const;
		// v12: exact-key (no hash-collision reuse), byte-bounded staging cache.
		// owner receives a shared reference that keeps the staging image alive
		// after a cache eviction; recorded upload commands hold one.
		VkImage getStagingImage( VkImage image
			, VkDeviceMemory & memory
			, std::shared_ptr< StagingImageEntry > * owner = nullptr );
		void getImageSubresourceLayout( VkImage image
			, VkImageSubresource const & subresource
			, VkSubresourceLayout & layout )const;
#if VK_EXT_debug_utils
		VkResult setDebugUtilsObjectName( VkDebugUtilsObjectNameInfoEXT const & nameInfo )const;
		VkResult setDebugUtilsObjectTag( VkDebugUtilsObjectTagInfoEXT const & tagInfo )const;
		void submitDebugUtilsMessenger( VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity
			, VkDebugUtilsMessageTypeFlagsEXT messageTypes
			, VkDebugUtilsMessengerCallbackDataEXT const & callbackData )const;
		void onSubmitDebugUtilsMessenger( VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity
			, VkDebugUtilsMessageTypeFlagsEXT messageTypes
			, VkDebugUtilsMessengerCallbackDataEXT const & callbackData )const;
#endif
#if VK_EXT_debug_marker
		VkResult debugMarkerSetObjectTag( VkDebugMarkerObjectTagInfoEXT const & nameInfo )const;
		VkResult debugMarkerSetObjectName( VkDebugMarkerObjectNameInfoEXT const & nameInfo )const;
#endif
#if VK_EXT_debug_report
		void reportMessage( VkDebugReportFlagsEXT flags
			, VkDebugReportObjectTypeEXT objectType
			, uint64_t object
			, size_t location
			, int32_t messageCode
			, const char * pLayerPrefix
			, const char * pMessage )const noexcept;
		void onReportMessage( VkDebugReportFlagsEXT flags
			, VkDebugReportObjectTypeEXT objectType
			, uint64_t object
			, size_t location
			, int32_t messageCode
			, const char * pLayerPrefix
			, const char * pMessage )const noexcept;
#endif
		VkQueue getQueue( uint32_t familyIndex
			, uint32_t index )const;
		VkResult waitIdle()const;
		/**
		*name
		*	Layers delegation.
		*/
		/**@{*/
		void onCopyToImageCommand( VkCommandBuffer cmd
			, ArrayView< VkBufferImageCopy const > const & copyInfo
			, VkBuffer src
			, VkImage dst )const;
		// v13.10: drain new D3D11 debug-layer messages (errors/warnings) to stderr.
		// No-op unless the device was created with the debug layer (-VerboseDiagnostics).
		void drainDebugMessages( char const * where )const noexcept;
		void onCheckHResultCommand( HRESULT hresult
			, std::string const & message )const;
		/**@}*/
		/**
		*name
		*	Getters.
		*/
		/**@{*/
		VkInstance getInstance()const
		{
			return m_instance;
		}

		D3D_FEATURE_LEVEL getFeatureLevel()const
		{
			return m_featureLevel;
		}

		// v13.37: D3D11.1 runtime options (Windows 8+): partial Map(NO_OVERWRITE) on dynamic
		// constant buffers / buffer SRVs. Queried once after device creation.
		bool canMapNoOverwriteConstantBuffer()const
		{
			return m_noOverwriteConstantBuffer;
		}

		bool canMapNoOverwriteBufferSrv()const
		{
			return m_noOverwriteBufferSrv;
		}

		ID3D11Device * getDevice()const
		{
			return m_d3dDevice;
		}

		VkBuffer getEmptyIndexedVaoIdx()const
		{
			return m_dummyIndexed.buffer;
		}

		VkPhysicalDevice getPhysicalDevice()const
		{
			return m_physicalDevice;
		}

		VkSampler getSampler()const
		{
			return m_sampler;
		}

		VkAllocationCallbacks const * getAllocationCallbacks()const
		{
			return m_callbacks;
		}

#if !defined( NDEBUG )

		ID3D11Debug * getDebug()
		{
			return m_debug;
		}

#endif
		/**@}*/

	private:
		void doCreateD3D11Device();
		void doCreateDummyIndexBuffer();
		void doCreateQueues();

		friend class DeviceContextLock;

	private:
		struct QueueCreates
		{
			explicit QueueCreates( VkDeviceQueueCreateInfo createInfo
				, std::vector< VkQueue > queues = {}  )
				: createInfo{ std::move( createInfo ) }
				, queues{ std::move( queues ) }
			{
			}

			VkDeviceQueueCreateInfo createInfo;
			std::vector< VkQueue > queues;
		};
		using QueueCreateCountMap = std::map< uint32_t, QueueCreates >;

	private:
		VkInstance m_instance;
		VkPhysicalDevice m_physicalDevice;
		VkAllocationCallbacks const * m_callbacks;
		VkDeviceCreateInfo m_createInfos;
		ID3D11Device * m_d3dDevice{ nullptr };
		ID3D11Query * m_waitIdleQuery{ nullptr };
		D3D_FEATURE_LEVEL m_featureLevel{};
		bool m_noOverwriteConstantBuffer{};
		bool m_noOverwriteBufferSrv{};
		QueueCreateCountMap m_queues;
	#if !defined( NDEBUG )
		ID3D11Debug * m_debug{ nullptr };
#endif
		// Mimic the behavior in Vulkan, when no IBO nor VBO is bound.
		struct Buffer
		{
			VkBuffer buffer;
			VkDeviceMemory memory;
		};
		Buffer m_dummyIndexed;
		VkSampler m_sampler;
		std::map< std::array< uint32_t, 6u >, std::shared_ptr< StagingImageEntry > > m_stagingTextures;
		mutable std::mutex m_mtxStaging;
		StagingCacheStats m_stagingStats{};
		uint64_t m_stagingTick{};
		mutable std::mutex m_mtxDeviceContext;
		ID3D11DeviceContext * m_deviceContext{ nullptr };
	};
}
