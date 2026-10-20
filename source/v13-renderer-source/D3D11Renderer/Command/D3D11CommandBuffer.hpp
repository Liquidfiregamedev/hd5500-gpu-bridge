/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include <atomic>
#include <unordered_map>

#include "renderer/D3D11Renderer/Command/Commands/D3D11BlitImageCommand.hpp"

#include <renderer/RendererCommon/IcdObject.hpp>

#include <memory>
#include <unordered_set>

namespace ashes::D3D11_NAMESPACE
{
	namespace ubring { struct View; }   // v13.74
	struct ResolvedUavs;

	class CommandBuffer
		: public ashes::IcdObject
		, public NonCopyable
	{
	public:
		CommandBuffer( VkDevice device
			, VkCommandPool commandPool
			, bool primary );
		~CommandBuffer()noexcept;

		void execute( Context & context )const;

		VkResult begin( VkCommandBufferBeginInfo info )const;
		VkResult end()const;
		VkResult reset()const;
		VkResult getRecordingResult()const noexcept { return m_recordingResult; }
		void recordFailure( VkResult result )const noexcept { if ( m_recordingResult == VK_SUCCESS && result != VK_SUCCESS ) m_recordingResult = result; }
		void beginRenderPass( VkRenderPassBeginInfo beginInfo )const;
		void nextSubpass()const;
		void endRenderPass()const;
		void executeCommands( ArrayView< VkCommandBuffer const > commands )const;
		void clearColorImage( VkImage image
			, VkClearColorValue colour
			, ArrayView < VkImageSubresourceRange const > ranges )const;
		void clearDepthStencilImage( VkImage image
			, VkClearDepthStencilValue value
			, ArrayView< VkImageSubresourceRange const > ranges )const;
		void clearAttachments( ArrayView< VkClearAttachment const > clearAttachments
			, ArrayView< VkClearRect const > clearRects );
		void bindPipeline( VkPipeline pipeline
			, VkPipelineBindPoint bindingPoint )const;
		void bindVertexBuffers( uint32_t firstBinding
			, ArrayView< VkBuffer const > buffers
			, ArrayView< VkDeviceSize const > offsets )const;
		void bindIndexBuffer( VkBuffer buffer
			, VkDeviceSize offset
			, VkIndexType indexType )const;
		void bindDescriptorSets( VkPipelineBindPoint bindingPoint
			, VkPipelineLayout layout
			, uint32_t firstSet
			, ArrayView< VkDescriptorSet const > descriptorSets
			, ArrayView< uint32_t const > dynamicOffsets )const;
		void setViewport( uint32_t firstViewport
			, ArrayView< VkViewport const > viewports )const;
		void setScissor( uint32_t firstScissor
			, ArrayView< VkRect2D const > scissors )const;
		void draw( uint32_t vtxCount
			, uint32_t instCount
			, uint32_t firstVertex
			, uint32_t firstInstance )const;
		void drawIndexed( uint32_t indexCount
			, uint32_t instCount
			, uint32_t firstIndex
			, uint32_t vertexOffset
			, uint32_t firstInstance )const;
		void drawIndirect( VkBuffer buffer
			, VkDeviceSize offset
			, uint32_t drawCount
			, uint32_t stride )const;
		void drawIndexedIndirect( VkBuffer buffer
			, VkDeviceSize offset
			, uint32_t drawCount
			, uint32_t stride )const;
		void copyToImage( VkBuffer src
			, VkImage dst
			, ArrayView< VkBufferImageCopy const > copyInfos )const;
		void copyToBuffer( VkImage src
			, VkBuffer dst
			, ArrayView< VkBufferImageCopy const > copyInfos )const;
		void updateBuffer( VkBuffer dstBuffer
			, VkDeviceSize dstOffset
			, ArrayView< uint8_t const > data );
		void fillBuffer( VkBuffer dstBuffer
			, VkDeviceSize dstOffset
			, VkDeviceSize size
			, uint32_t data );
		void copyBuffer( VkBuffer src
			, VkBuffer dst
			, ArrayView< VkBufferCopy const > copyInfos )const;
		void copyImage( VkImage src
			, VkImage dst
			, ArrayView< VkImageCopy const > copyInfos )const;
		void blitImage( VkImage srcImage
			, VkImage dstImage
			, ArrayView< VkImageBlit const > regions
			, VkFilter filter )const;
		void resolveImage( VkImage srcImage
			, VkImage dstImage
			, ArrayView< VkImageResolve const > regions )const;
		void resetQueryPool( VkQueryPool pool
			, uint32_t firstQuery
			, uint32_t queryCount )const;
		void beginQuery( VkQueryPool pool
			, uint32_t query
			, VkQueryControlFlags flags )const;
		void endQuery( VkQueryPool pool
			, uint32_t query )const;
		void writeTimestamp( VkPipelineStageFlagBits pipelineStage
			, VkQueryPool pool
			, uint32_t query )const;
		void copyQueryPoolResults( VkQueryPool queryPool
			, uint32_t firstQuery
			, uint32_t queryCount
			, VkBuffer dstBuffer
			, VkDeviceSize dstOffset
			, VkDeviceSize stride
			, VkQueryResultFlags flags )const;
		void pushConstants( VkPipelineLayout layout
			, VkShaderStageFlags stageFlags
			, uint32_t offset
			, uint32_t size
			, void const * data )const;
		void dispatch( uint32_t groupCountX
			, uint32_t groupCountY
			, uint32_t groupCountZ )const;
		void dispatchIndirect( VkBuffer buffer
			, VkDeviceSize offset )const;
		void setLineWidth( float width )const;
		void setDepthBias( float constantFactor
			, float clamp
			, float slopeFactor )const;
		void setBlendConstants( const float blendConstants[4] );
		void setDepthBounds( float minDepthBounds
			, float maxDepthBounds );
		void setStencilCompareMask( VkStencilFaceFlags faceMask
			, uint32_t compareMask );
		void setStencilWriteMask( VkStencilFaceFlags faceMask
			, uint32_t writeMask );
		void setStencilReference( VkStencilFaceFlags faceMask
			, uint32_t reference );
		void setEvent( VkEvent event
			, VkPipelineStageFlags stageMask )const;
		void resetEvent( VkEvent event
			, VkPipelineStageFlags stageMask )const;
		void waitEvents( ArrayView< VkEvent const > events
			, VkPipelineStageFlags srcStageMask
			, VkPipelineStageFlags dstStageMask
			, ArrayView< VkBufferMemoryBarrier const > bufferMemoryBarriers
			, ArrayView< VkImageMemoryBarrier const > imageMemoryBarriers )const;
		void pipelineBarrier( VkPipelineStageFlags after
			, VkPipelineStageFlags before
			, ArrayView< VkMemoryBarrier const > memoryBarriers
			, ArrayView< VkBufferMemoryBarrier const > bufferMemoryBarriers
			, ArrayView< VkImageMemoryBarrier const > imageMemoryBarriers )const;
		void generateMipmaps( VkImage texture )const;
#if VK_EXT_debug_utils
		void beginDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const;
		void endDebugUtilsLabel()const;
		void insertDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const;
#endif
#if VK_EXT_debug_marker
		void debugMarkerBegin( VkDebugMarkerMarkerInfoEXT const & labelInfo )const;
		void debugMarkerEnd()const;
		void debugMarkerInsert( VkDebugMarkerMarkerInfoEXT const & labelInfo )const;
#endif

		inline CommandArray const & getCommands()const
		{
			return m_commands;
		}

		inline ID3D11CommandList * getCommandList()const
		{
			return m_commandList;
		}

		inline VkDevice getDevice()const
		{
			return m_device;
		}

	private:
		struct ResourceIndex
		{
			ResourceIndex( ObjectMemory const * memory
				, size_t index
				, DeviceMemoryDestroyConnection connection )
				: memory{ memory }
				, index{ index }
				, connection{ std::move( connection ) }
			{
			}

			ObjectMemory const * memory;
			size_t index;
			DeviceMemoryDestroyConnection connection;
		};


	private:
		void doFillVboStrides()const;
		// v13.60: buffer ranges already recorded for upload in this recording.
		struct RecordedUpload { ObjectMemory const * memory; VkDeviceSize offset; VkDeviceSize range; };
		mutable std::unordered_map< uint64_t, RecordedUpload > m_recordedUploads;
		mutable RecordedUpload m_recentUploads[4]{};   // v13.72: last recorded uploads (cleared with m_recordedUploads)
		mutable uint32_t m_recentUploadsNext{};
		// v13.73: small host->GPU buffer uploads recorded in this recording, merged in end()
		struct UploadSpan { ObjectMemory const * memory; VkDeviceSize begin; VkDeviceSize end; uint32_t seq; class UploadMemoryCommand * command; };
		mutable std::vector< UploadSpan > m_uploadSpans;
		mutable bool m_hasBufferWrites{};
		mutable bool m_hasExecuteCommands{};   // v13.76
		void doMergeUploads()const;
		// v13.74: uniform ring (Miscellaneous/D3D11UniformRing.hpp)
		mutable std::vector< std::shared_ptr< ubring::View > > m_ringViews;
		std::shared_ptr< ubring::View > doPrepareUniformRing( VkDescriptorSet descriptor, ArrayView< uint32_t const > const & dynamicOffsets, std::vector< uint8_t > & skip, std::vector< uint8_t > & skipStatic )const;
		void doFillUniformRing( Context & context )const;
		void doMarkGpuWritten( VkBuffer buffer )const;
		static bool uploadMerge();
		mutable LARGE_INTEGER m_recordStart{};   // v13.62
		mutable uint64_t m_recordTscStart{};    // v13.64
		mutable uint64_t m_recordTscBridge{};   // v13.64: rdtsc ticks spent inside the bridge's vkCmd* handling
		mutable uint32_t m_recordCalls{};       // v13.64
	public:
		void addRecordTsc( uint64_t ticks )const noexcept { m_recordTscBridge += ticks; }
		bool tickRecordHealthCheck()const noexcept { return ( ++m_recordCalls & 255u ) == 0u; }
	private:
	public:
		static inline std::atomic< uint64_t > m_recordedUploadsSkipped{ 0u };
	private:
		SharedVbos currentVbos()const
		{
			static SharedVbos const empty = std::make_shared< VbosBindingArray const >();
			return m_state.vbosShared ? m_state.vbosShared : empty;
		}
		void doAddAfterSubmitAction()const;
		void doFinishAfterSubmitActions()const;
		void doProcessMappedBoundDescriptorResourcesIn( VkDescriptorSet descriptor
			, ArrayView< uint32_t const > const & dynamicOffsets
			, std::vector< uint8_t > const * skipDynamic = nullptr
			, std::vector< uint8_t > const * skipStatic = nullptr )const;
		void doProcessMappedBoundDescriptorsResourcesOut( VkPipeline pipeline )const;
		void doProcessMappedBoundVaoBuffersIn()const;
		void doProcessMappedBoundIndexBufferIn()const;
		void doProcessMappedBoundResourceIn( VkBuffer buffer
			, VkDeviceSize offset
			, VkDeviceSize range )const;
		void doProcessMappedBoundResourceIn( VkImageView image )const;
		void doProcessMappedBoundResourceOut( VkBuffer buffer
			, VkDeviceSize offset
			, VkDeviceSize range )const;
		void doProcessMappedBoundResourceOut( VkImageView image )const;
		ResourceIndex & doAddMappedResource( ObjectMemory const * memory
			, VkDeviceSize offset
			, VkDeviceSize range
			, UINT subresource
			, bool isInput )const;
		ResourceIndex & doAddMappedResource( ObjectMemory const * memory )const;
		void doRemoveMappedResource( ObjectMemory const * memory )const;
		BlitPipeline const & doGetBlitPipeline( VkFormat src, VkFormat dst )const;
		void doApplyDynamicStates()const;
		void doResolveOutputUavs()const;
		void doBindDynamicStates()const;

		private:
			VkDevice m_device;
			VkCommandPool m_commandPool;
			mutable VkResult m_recordingResult{ VK_SUCCESS };
			mutable CommandArray m_commands;
			mutable std::vector< ResourceIndex > m_mappedResources;
			struct State
		{
			VkCommandBufferBeginInfo beginInfo{};
			VkPipeline currentPipeline{};
			std::vector< std::pair < VkPipelineLayout, PushConstantsDesc > > pushConstantBuffers;
			VkPipeline currentComputePipeline{};
			VkSubpassDescription const * currentSubpass{};
			VkRenderPass currentRenderPass{};
			VkFramebuffer currentFrameBuffer{};
			uint32_t currentSubpassIndex{};
			mutable VbosBindingArray vbos;
			// v13.60: shared copy of `vbos` for the draw commands; rebuilt only when a vertex
			// buffer bind or the pipeline's vertex input changed since the last draw.
			mutable SharedVbos vbosShared;
			mutable uint64_t vboVersion{ 1u };
			mutable uint64_t vboBuiltVersion{ 0u };
			mutable void const * vboBuiltState{};
			// v13.33: per-slot vertex buffer bindings, as Vulkan keeps them: they survive
			// pipeline binds and render passes. `vbos` is rebuilt from these before each draw.
			struct VboSlot
			{
				VkBuffer buffer{};
				ID3D11Buffer * d3dBuffer{};
				UINT offset{};
			};
			mutable std::vector< VboSlot > vboSlots;
			VkIndexType indexType{};
			VkDescriptorSetArray boundDescriptors;
			// v13.47: graphics descriptor sets per set index and the UAV array resolved from them.
			std::vector< std::pair< VkDescriptorSet, UInt32Array > > graphicsSets;
			std::shared_ptr< ResolvedUavs const > resolvedUavs;
			VkPipelineLayout resolvedLayout{};
			VkBuffer newlyBoundIbo{};
			VkBuffer boundIbo{};
			VkDeviceSize boundIboOffset{};
			Optional< float > lineWidth{};
			Optional< DepthBias > depthBias{};
			Optional< std::array< float, 4u > > blendConstants{};
			Optional< DepthBounds > depthBounds{};
			std::array< Optional< uint32_t >, 2u > stencilCompareMask;
			std::array< Optional< uint32_t >, 2u > stencilWriteMask;
			std::array< Optional< uint32_t >, 2u > stencilReference;
		};
		mutable State m_state;
		mutable std::vector< std::function< void( Context const & ) > > m_afterSubmitActions;
		// v13.51: removeKey() of every cleanup already queued (duplicates are skipped), and
		// whether m_afterSubmitActions is still in recording order (reversed once in end()).
		mutable std::unordered_set< uint64_t > m_afterSubmitKeys;
		mutable bool m_afterSubmitRecordingOrder{ true };
		mutable ID3D11DeviceContext * m_deferredContext{ nullptr };
		mutable ID3D11CommandList * m_commandList{ nullptr };
		mutable std::map< uint64_t, std::unique_ptr< BlitPipeline > > m_blitPipelines;
		mutable Optional< DebugLabel > m_label;
	};
}
