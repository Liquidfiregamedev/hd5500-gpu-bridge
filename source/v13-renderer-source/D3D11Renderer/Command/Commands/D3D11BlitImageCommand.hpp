/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "Command/Commands/D3D11CommandBase.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	struct BlitSamplerOwner
	{
		VkDevice device{};
		VkSampler sampler{};

		~BlitSamplerOwner()noexcept;
	};

	struct BlitPipeline
	{
		VkDevice device{};
		VkDescriptorSetLayout descriptorLayout{};
		VkPipelineLayout pipelineLayout{};
		UInt32Array spirv{};
		VkShaderModule shader{};
		VkPipeline pipeline{};
		// The cache deliberately holds weak references only.  Commands retain the
		// owning shared pointer so flattened/cloned command buffers never observe
		// a sampler that a later recording has released.
		mutable std::mutex samplerMutex;
		mutable std::weak_ptr< BlitSamplerOwner > nearestSampler;
		mutable std::weak_ptr< BlitSamplerOwner > linearSampler;

		BlitPipeline( Device const & device
			, VkFormat src
			, VkFormat dst );
		BlitPipeline( BlitPipeline const & ) = delete;
		BlitPipeline( BlitPipeline && )noexcept = delete;
		BlitPipeline & operator=( BlitPipeline const & ) = delete;
		BlitPipeline & operator=( BlitPipeline && )noexcept = delete;
		~BlitPipeline()noexcept;
	};

	class BlitImageCommand
		: public CommandBase
	{
	public:
		struct Attachment
			: public NonCopyable
		{
			Attachment( VkDevice device
				, VkImageSubresourceLayers const & subresource
				, VkImage image
				, uint32_t layer
				, bool asUav );
			~Attachment()noexcept;

			VkDevice device{};
			VkImageView view{};
			ID3D11Resource * image{};
			UINT subResourceIndex{};
		};
		struct LayerCopy
		{
			LayerCopy( VkDevice device
				, VkDescriptorPool pool
				, VkDescriptorSetLayout descriptorLayout
				, VkBuffer ubo
				, VkDeviceSize offset
				, VkDeviceSize range
				, VkSampler sampler
				, VkImageBlit const & region
				, VkImage srcImage
				, VkImage dstImage
				, uint32_t layer );

			VkOffset3D dstOffset{};
			D3D11_BOX srcBox{};
			D3D11_BOX dstBox{};
			Attachment src;
			Attachment dst;
			VkDescriptorSet set{};
			uint8_t * buffer{};
			struct BlitBox
			{
				std::array< float, 4u > srcBox{};
				std::array< float, 4u > dstBox{};
			};
			BlitBox blitBox{};
		};

		// The compact destination is private to this command.  The shared command
		// lifetime below makes this record's accounting token single-owner too.
		struct CompactScratch
		{
			bool allocated{};
			uint64_t id{};
			uint64_t payloadBytes{};
			VkExtent3D atlasExtent{};
			VkExtent3D scratchExtent{};
			VkOffset3D bboxOrigin{};
			uint32_t mipLevel{};
			uint32_t baseArrayLayer{};
			uint32_t layerCount{};
		};

	public:
		BlitImageCommand( VkCommandPool pool
			, VkCommandBuffer cb
			, VkDevice device
			, BlitPipeline const & pipeline
			, VkImage srcImage
			, VkImage dstImage
			, ArrayView< VkImageBlit const > const & regions
			, VkFilter filter );
		~BlitImageCommand()noexcept override;
		bool destroyOnOwnerThread()const override { return true; }   // v13.69

		void apply( Context & context )const override;
		CommandPtr clone()const override;

	private:
		void doCleanup()noexcept;
		void doInitialiseStretchUbo( VkDevice device
			, VkDescriptorSetLayout descriptorLayout
			, uint32_t count );
		void doInitialiseStretches( VkDevice device
			, VkDescriptorSetLayout descriptorLayout
			, uint32_t count );
		VkImage doUpdateTmpSrc();
		void doUpdateTmpDst();
		void doAddBlits( VkImage srcTexture );
		void doAddStretches( VkPipeline pipeline
			, VkPipelineLayout pipelineLayout );
		void doCreateCommandBuffer( VkCommandPool pool
			, VkPipeline pipeline
			, VkPipelineLayout pipelineLayout );

	private:
		// Commands are copied while executeCommands is flattened.  Every copy has
		// the same raw Vulkan/D3D resources, so the final surviving copy alone
		// releases them.
		std::shared_ptr< uint8_t > m_lifetime{ std::make_shared< uint8_t >( 0u ) };
		VkCommandPool m_commandPool{};
		VkImage m_srcTexture{};
		VkImage m_dstTexture{};
		VkDeviceMemory m_srcMemory{};
		VkDeviceMemory m_dstMemory{};
		VkImage m_tmpSrcTexture{};
		VkImage m_tmpDstTexture{};
		VkSampler m_sampler{};
		std::shared_ptr< BlitSamplerOwner > m_samplerOwner;
		VkDescriptorPool m_pool{};
		std::vector< std::shared_ptr< LayerCopy > > m_layerStretches;
		ashes::VkImageCopyArray m_layerBlitsToTmp;
		ashes::VkImageCopyArray m_layerBlitsFromTmp;
		ashes::VkImageCopyArray m_layerBlits;
		VkDeviceMemory m_uboMemory{};
		VkBuffer m_ubo{};
		VkCommandBuffer m_commandBuffer{};
		VkImageBlitArray m_blitRegions;
		VkImageBlitArray m_stretchRegions;
		// Original stretch regions are kept above.  These only remap the private
		// compact destination to mip zero / local array layers.
		VkImageBlitArray m_scratchStretchRegions;
		CompactScratch m_compactScratch{};
	};
}
