/*
This file belongs to Ashes.
See LICENSE file in root folder

v13.12 (HD5500 GPU bridge): draw-based vkCmdBlitImage.

Why a second blit path: the compute blit needs a typed UAV on the destination,
and D3D11's UAV cast rules forbid viewing a B8G8R8A8 (RimWorld's scene format)
or an sRGB texture through R32_UINT, while a typed B8G8R8A8_UNORM UAV is optional
and rejected by the Intel HD 5500 driver. A full-screen-triangle draw into a
render-target view has none of these limits: every colour format a D3D11 device
can render to works, scaling/flipping/filtering come from the sampler, in-place
mip chains (SRV on mip N, RTV on mip N+1) are legal, and the whole thing is plain
feature-level-10 D3D11, which is the point of the bridge's "older GPUs" goal.
*/
#pragma once

#include "Command/Commands/D3D11CommandBase.hpp"

#include <memory>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	struct DrawBlitShared;

	class DrawBlitCommand
		: public CommandBase
	{
	public:
		DrawBlitCommand( VkDevice device
			, VkImage srcImage
			, VkImage dstImage
			, ArrayView< VkImageBlit const > const & regions
			, VkFilter filter );
		~DrawBlitCommand()noexcept override;
		bool destroyOnOwnerThread()const override { return true; }   // v13.69

		void apply( Context & context )const override;
		CommandPtr clone()const override;

		// True when this device/format pair can be handled by the draw path.
		static bool isSupported( VkDevice device
			, VkImage srcImage
			, VkImage dstImage )noexcept;

	private:
		struct Op
		{
			ID3D11ShaderResourceView * srv{};
			ID3D11RenderTargetView * rtv{};
			D3D11_VIEWPORT viewport{};
			float cb[8]{}; // uvOrigin.xy, uvExtent.xy, flip.xy, unused
		};
		struct Copy
		{
			UINT srcSubresource{};
			UINT dstSubresource{};
			D3D11_BOX srcBox{};
			UINT dstX{}, dstY{};
		};

		std::shared_ptr< DrawBlitShared > m_shared;
		std::shared_ptr< std::vector< Op > > m_ops;
		std::vector< Copy > m_copies;
		ID3D11Resource * m_srcResource{};
		ID3D11Resource * m_dstResource{};
		VkImage m_dstImage{};
		VkFilter m_filter{};
	};
}
