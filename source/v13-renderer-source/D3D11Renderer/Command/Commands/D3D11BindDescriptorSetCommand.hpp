/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/Command/Commands/D3D11CommandBase.hpp"

#include <memory>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	/**
	*\brief
	*	Commande d'activation d'un set de descripteurs.
	*/
	class BindDescriptorSetCommand
		: public CommandBase
	{
	public:
		/**
		*\brief
		*	Constructeur.
		*\param[in] descriptorSet
		*	Le descriptor set.
		*\param[in] layout
		*	Le layout de pipeline.
		*\param[in] bindingPoint
		*	Le point d'attache du set.
		*/
		BindDescriptorSetCommand( VkDevice device
			, VkDescriptorSet descriptorSet
			, VkPipelineLayout layout
			, uint32_t setIndex
			, ArrayView< uint32_t const > const & dynamicOffsets
			, VkPipelineBindPoint bindingPoint );

		void apply( Context & context )const override;
		bool isStateOnly()const override { return true; }   // v13.63
		void remove( Context const & context )const override;
		uint64_t removeKey()const override;   // v13.51
		void fillContext( Context & context )const override;
		CommandPtr clone()const override;
		// v13.74: uniform ring (see Miscellaneous/D3D11UniformRing.hpp)
		void setUniformRing( std::shared_ptr< ubring::View > view ) { m_ring = std::move( view ); }

	private:
		VkDescriptorSet m_descriptorSet;
		VkPipelineLayout m_layout;
		uint32_t m_setIndex{};
		VkPipelineBindPoint m_bindingPoint;
		UInt32Array m_dynamicOffsets;
		std::map< UINT, ID3D11UnorderedAccessView * > m_uavs;
		std::vector< ID3D11UnorderedAccessView * > m_hazardUavs;
		std::vector< std::shared_ptr< ID3D11UnorderedAccessView > > m_keep;   // v13.47b
		std::vector< uint64_t > m_content;   // v13.73: exact list of what this set binds (no dynamic offsets)
		bool m_contentValid{};
		bool m_contentUboRebind{};   // v13.75
		std::shared_ptr< ubring::View > m_ring;   // v13.74
	};

	/**
	*\brief
	*	v13.47: rebuilds the complete output-merger UAV array right before a graphics draw,
	*	from the descriptor sets currently bound and the layout of the pipeline that will
	*	draw. Ashes used to key UAVs by the layout given to vkCmdBindDescriptorSets and to
	*	merge them into whatever was bound before, so a UAV of an earlier pipeline could stay
	*	in a slot the next shader reads (the forbidden-X instance buffer was read from a stale
	*	8 MB buffer while its own buffer sat one slot higher).
	*/
	struct ResolvedUavs
	{
		std::map< UINT, ID3D11UnorderedAccessView * > uavs;
		std::vector< std::shared_ptr< ID3D11UnorderedAccessView > > keep;   // v13.47b: own a reference
		uint64_t serial{};
	};

	class ResolveOutputUavsCommand
		: public CommandBase
	{
	public:
		ResolveOutputUavsCommand( VkDevice device
			, std::shared_ptr< ResolvedUavs const > resolved
			, uint32_t declaredUavMask = 0u
			, VkPipeline pipeline = VK_NULL_HANDLE );

		void apply( Context & context )const override;
		bool isStateOnly()const override { return true; }   // v13.63
		CommandPtr clone()const override;

		static std::shared_ptr< ResolvedUavs const > resolve( VkPipelineLayout layout
			, std::vector< std::pair< VkDescriptorSet, UInt32Array > > const & sets );
		static bool enabled();

	private:
		std::shared_ptr< ResolvedUavs const > m_resolved;
		uint32_t m_declaredUavMask{};   // graphics shaders' u-registers (absolute slots)
		VkPipeline m_pipeline{};
	};
}
