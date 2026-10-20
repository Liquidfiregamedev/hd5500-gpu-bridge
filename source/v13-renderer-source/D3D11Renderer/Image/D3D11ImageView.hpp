/**
*\file
*	Image.h
*\author
*	Sylvain Doremus
*/
#ifndef ___D3D11Renderer_TextureView_HPP___
#define ___D3D11Renderer_TextureView_HPP___
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

namespace ashes::D3D11_NAMESPACE
{
	class ImageView
		: public NonCopyable
	{
	public:
		ImageView( VkDevice device
			, VkImageViewCreateInfo createInfo );
		~ImageView()noexcept;

		UINT getSubresource( uint32_t layer )const;

		ID3D11View * getView()const
		{
			using ViewPtr = ID3D11View*;
			return m_renderTargetView
				? ViewPtr( m_renderTargetView )
				: ( m_depthStencilView
					? ViewPtr( m_depthStencilView )
					: ( m_unorderedAccessView
						? ViewPtr( m_unorderedAccessView )
						: ( m_shaderView 
							? ViewPtr( m_shaderView )
							: nullptr ) ) );
		}

		ID3D11RenderTargetView * getRenderTargetView()const
		{
			return m_renderTargetView;
		}

		ID3D11DepthStencilView * getDepthStencilView()const
		{
			return m_depthStencilView;
		}

		ID3D11UnorderedAccessView * getUnorderedAccessView()const
		{
			return m_unorderedAccessView;
		}

		ID3D11ShaderResourceView * getShaderView()const
		{
			return m_shaderView;
		}

		// v13.14: the SRV to bind for sampling. For a view with a non-identity
		// component swizzle (D3D11 has none natively) this is a shadow texture
		// holding the swizzled texels, rebuilt when the image content changes.
		// v13.51: *rendered (optional) is set to true only when the swizzle shadow was redrawn,
		// i.e. when the device context state was changed behind the bridge's state cache.
		ID3D11ShaderResourceView * getSampledView( ID3D11DeviceContext * context, bool * rendered = nullptr )const;

		bool hasSwizzle()const noexcept
		{
			return m_swizzled;
		}

		VkFormat getFormat()const
		{
			return m_createInfo.format;
		}

		VkImage getImage()const
		{
			return m_createInfo.image;
		}

		VkImageSubresourceRange const & getSubResourceRange()const
		{
			return m_createInfo.subresourceRange;
		}

	private:
		void doCheckViewCreation( HRESULT hr
			, ID3D11View * created
			, char const * operation );
		void doReleaseViews()noexcept;
		void doCreate1D();
		void doCreate1DArray();
		void doCreate2D();
		void doCreate2DArray();
		void doCreate3D();
		void doCreateCube();
		void doCreateCubeArray();

	private:
		VkDevice m_device;
		VkImageViewCreateInfo m_createInfo;
		ID3D11RenderTargetView * m_renderTargetView{ nullptr };
		ID3D11DepthStencilView * m_depthStencilView{ nullptr };
		ID3D11UnorderedAccessView * m_unorderedAccessView{ nullptr };
		ID3D11ShaderResourceView * m_shaderView{ nullptr };
		// v13.14 swizzle emulation.
		bool m_swizzled{ false };
		uint32_t m_swizzleSelect[4]{ 0u, 1u, 2u, 3u }; // 0..3 = R,G,B,A; 4 = zero; 5 = one
		mutable ID3D11Texture2D * m_swizzleTexture{ nullptr };
		mutable ID3D11ShaderResourceView * m_swizzleView{ nullptr };
		mutable uint64_t m_swizzleVersion{ 0u };
		mutable bool m_swizzleFailed{ false };
	};
}

#endif
