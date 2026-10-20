#include "Image/D3D11ImageView.hpp"

#include "Core/D3D11Device.hpp"
#include "Image/D3D11Image.hpp"

#include <ashes/common/Exception.hpp>

#include "ashesd3d11_api.hpp"

namespace ashes::D3D11_NAMESPACE
{
	namespace imgview
	{
		// v12: a decoded BC image is backed by RGBA8; its BC-format views map to
		// the matching RGBA8 UNORM/SRGB native view format.
		inline VkFormat nativeFormat( VkImage image, VkFormat viewFormat )noexcept
		{
			return bc::nativeViewFormat( get( image )->getCreateInfo(), viewFormat );
		}
	}
	namespace
	{
		VkResult getNativeViewFailure( VkDevice device, HRESULT hr )noexcept
		{
			if ( get( device )->getStatus() != VK_SUCCESS || hr == DXGI_ERROR_DEVICE_HUNG
				|| hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
				|| hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ) return VK_ERROR_DEVICE_LOST;
			return hr == E_OUTOFMEMORY ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	ImageView::ImageView( VkDevice device
		, VkImageViewCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
	{
		if ( !m_device || !m_createInfo.image )
		{
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Image view has no device or image" };
		}
		get( m_device )->requireHealthy( "Create image view" );
		auto image = get( m_createInfo.image );
		if ( !image || !image->getResource() )
		{
			throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Create image view from a null native image resource" };
		}
		auto & range = m_createInfo.subresourceRange;

		if ( range.levelCount == RemainingArrayLayers )
		{
			range.levelCount = image->getMipmapLevels() - range.baseMipLevel;
		}

		if ( range.layerCount == RemainingArrayLayers )
		{
			range.layerCount = image->getLayerCount() - range.baseArrayLayer;
		}

		switch ( createInfo.viewType )
		{
		case VK_IMAGE_VIEW_TYPE_1D:
			if ( image->getLayerCount() > 1 )
			{
				doCreate1DArray();
			}
			else
			{
				doCreate1D();
			}
			break;
		case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
			doCreate1DArray();
			break;
		case VK_IMAGE_VIEW_TYPE_2D:
			if ( image->getLayerCount() > 1 )
			{
				doCreate2DArray();
			}
			else
			{
				doCreate2D();
			}
			break;
		case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
			if ( image->getType() == VK_IMAGE_TYPE_3D )
			{
				doCreate3D();
			}
			else
			{
				doCreate2DArray();
			}
			break;
		case VK_IMAGE_VIEW_TYPE_3D:
			doCreate3D();
			break;
		case VK_IMAGE_VIEW_TYPE_CUBE:
			if ( image->getLayerCount() > 1 )
			{
				doCreateCubeArray();
			}
			else
			{
				doCreateCube();
			}
			break;
		case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
			doCreateCubeArray();
			break;
		default:
			break;
		}
	}

	ImageView::~ImageView()noexcept
	{
		doReleaseViews();
	}

	void ImageView::doReleaseViews()noexcept
	{
		safeRelease( m_renderTargetView );
		safeRelease( m_depthStencilView );
		safeRelease( m_unorderedAccessView );
		safeRelease( m_shaderView );
	}

	void ImageView::doCheckViewCreation( HRESULT hr, ID3D11View * created, char const * operation )
	{
		if ( SUCCEEDED( hr ) && created ) return;
		auto const failure = FAILED( hr ) ? hr : E_FAIL;
		checkError( m_device, failure, operation );
		auto const result = getNativeViewFailure( m_device, failure );
		doReleaseViews();
		throw Exception{ result, std::string{ operation } + " failed" };
	}

	UINT ImageView::getSubresource( uint32_t layer )const
	{
		assert( layer >= m_createInfo.subresourceRange.baseArrayLayer
			&& layer < m_createInfo.subresourceRange.baseArrayLayer + m_createInfo.subresourceRange.layerCount );
		return get( m_createInfo.image )->getNativeSubresource(
			m_createInfo.subresourceRange.baseMipLevel
			, layer );
	}

	void ImageView::doCreate1D()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );

		if ( image->isRenderTarget() )
		{
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateRenderTargetView( image->getTexture1D()
				, &desc
				, &m_renderTargetView );
			doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView1D" );
			dxDebugName( m_renderTargetView, RenderTargetView1D );
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipLevels = getSubResourceRange().levelCount;
			desc.Texture1D.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateShaderResourceView( image->getTexture1D()
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView1D" );
			dxDebugName( m_shaderView, ShaderResourceView1D );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateUnorderedAccessView( image->getTexture1D()
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView1D" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessView1D );
		}
	}

	void ImageView::doCreate1DArray()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );

		if ( image->isRenderTarget() )
		{
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture1DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture1DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateRenderTargetView( image->getTexture1D()
				, &desc
				, &m_renderTargetView );
			doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView1DArray" );
			dxDebugName( m_renderTargetView, RenderTargetView1DArray );
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture1DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture1DArray.MipLevels = getSubResourceRange().levelCount;
			desc.Texture1DArray.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateShaderResourceView( image->getTexture1D()
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView1DArray" );
			dxDebugName( m_shaderView, ShaderResourceView1DArray );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture1DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture1DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateUnorderedAccessView( image->getTexture1D()
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView1DArray" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessView1DArray );
		}
	}

	void ImageView::doCreate2D()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );
		auto multisampled = image->getSamplesCount() > VK_SAMPLE_COUNT_1_BIT;

		if ( image->isRenderTarget() )
		{
			if ( image->getType() == VK_IMAGE_TYPE_3D )
			{
				if ( isDepthOrStencilFormat( image->getFormat() ) )
				{
					D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
					desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
					desc.ViewDimension = multisampled
						? D3D11_DSV_DIMENSION_TEXTURE2DMS
						: D3D11_DSV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
					auto hr = device->CreateDepthStencilView( image->getTexture3D()
						, &desc
						, &m_depthStencilView );
					doCheckViewCreation( hr, m_depthStencilView, "CreateDepthStencilView2D" );
					dxDebugName( m_depthStencilView, DepthStencilView2D );
				}
				else
				{
					D3D11_RENDER_TARGET_VIEW_DESC desc{};
					desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
					desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
					desc.Texture3D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
					desc.Texture3D.FirstWSlice = getSubResourceRange().baseArrayLayer;
					desc.Texture3D.WSize = getSubResourceRange().layerCount;
					auto hr = device->CreateRenderTargetView( image->getTexture3D()
						, &desc
						, &m_renderTargetView );
					doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView3D" );
					dxDebugName( m_renderTargetView, RenderTargetView3D );
				}
			}
			else
			{
				if ( isDepthOrStencilFormat( image->getFormat() ) )
				{
					D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
					desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
					desc.ViewDimension = multisampled
						? D3D11_DSV_DIMENSION_TEXTURE2DMS
						: D3D11_DSV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
					auto hr = device->CreateDepthStencilView( image->getTexture2D()
						, &desc
						, &m_depthStencilView );
					doCheckViewCreation( hr, m_depthStencilView, "CreateDepthStencilView2D" );
					dxDebugName( m_depthStencilView, DepthStencilView2D );
				}
				else
				{
					D3D11_RENDER_TARGET_VIEW_DESC desc{};
					desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
					desc.ViewDimension = multisampled
						? D3D11_RTV_DIMENSION_TEXTURE2DMS
						: D3D11_RTV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
					auto hr = device->CreateRenderTargetView( image->getTexture2D()
						, &desc
						, &m_renderTargetView );
					doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView2D" );
					dxDebugName( m_renderTargetView, RenderTargetView2D );
				}
			}
		}

		if ( image->isSamplable() )
		{
			if ( image->getType() == VK_IMAGE_TYPE_3D )
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
				desc.Texture3D.MipLevels = getSubResourceRange().levelCount;
				desc.Texture3D.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateShaderResourceView( image->getTexture3D()
					, &desc
					, &m_shaderView );
				doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView3D" );
				dxDebugName( m_shaderView, ShaderResourceView3D );
			}
			else
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = multisampled
					? D3D11_SRV_DIMENSION_TEXTURE2DMS
					: D3D11_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipLevels = getSubResourceRange().levelCount;
				desc.Texture2D.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateShaderResourceView( image->getTexture2D()
					, &desc
					, &m_shaderView );
				doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView2D" );
				dxDebugName( m_shaderView, ShaderResourceView2D );
			}
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			if ( image->getType() == VK_IMAGE_TYPE_3D )
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
				desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
				desc.Texture3D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				desc.Texture3D.FirstWSlice = getSubResourceRange().baseArrayLayer;
				desc.Texture3D.WSize = getSubResourceRange().layerCount;
				auto hr = device->CreateUnorderedAccessView( image->getTexture3D()
					, &desc
					, &m_unorderedAccessView );
				doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView3D" );
				dxDebugName( m_unorderedAccessView, UnorderedAccessView3D );
			}
			else
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
				desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateUnorderedAccessView( image->getTexture2D()
					, &desc
					, &m_unorderedAccessView );
				doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView2D" );
				dxDebugName( m_unorderedAccessView, UnorderedAccessView2D );
			}
		}
	}

	void ImageView::doCreate2DArray()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );
		auto multisampled = image->getSamplesCount() > VK_SAMPLE_COUNT_1_BIT;
		ID3D11Resource * texture = ( ( image->getType() == VK_IMAGE_TYPE_3D )
			? static_cast< ID3D11Resource * >( image->getTexture3D() )
			: static_cast< ID3D11Resource * >( image->getTexture2D() ) );

		if ( image->isRenderTarget() )
		{
			if ( isDepthOrStencilFormat( image->getFormat() ) )
			{
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = multisampled
					? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY
					: D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateDepthStencilView( texture
					, &desc
					, &m_depthStencilView );
				doCheckViewCreation( hr, m_depthStencilView, "CreateDepthStencilView2D" );
				dxDebugName( m_depthStencilView, DepthStencilView2D );
			}
			else
			{
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = multisampled
					? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY
					: D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateRenderTargetView( texture
					, &desc
					, &m_renderTargetView );
				doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView2DArray" );
				dxDebugName( m_renderTargetView, RenderTargetView2DArray );
			}
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = multisampled
				? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY
				: D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture2DArray.MipLevels = getSubResourceRange().levelCount;
			desc.Texture2DArray.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateShaderResourceView( texture
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView2DArray" );
			dxDebugName( m_shaderView, ShaderResourceView2DArray );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateUnorderedAccessView( texture
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView2DArray" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessView2DArray );
		}
	}

	void ImageView::doCreate3D()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );

		if ( image->isRenderTarget() )
		{
			if ( isDepthOrStencilFormat( image->getFormat() ) )
			{
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = image->getDimensions().depth;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateDepthStencilView( image->getTexture3D()
					, &desc
					, &m_depthStencilView );
				doCheckViewCreation( hr, m_depthStencilView, "CreateDepthStencilView3D" );
				dxDebugName( m_depthStencilView, DepthStencilView3D );
			}
			else
			{
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
				desc.Texture3D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				desc.Texture3D.FirstWSlice = getSubResourceRange().baseArrayLayer;
				desc.Texture3D.WSize = getSubResourceRange().layerCount;
				auto hr = device->CreateRenderTargetView( image->getTexture3D()
					, &desc
					, &m_renderTargetView );
				doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetView3D" );
				dxDebugName( m_renderTargetView, RenderTargetView3D );
			}
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MipLevels = getSubResourceRange().levelCount;
			desc.Texture3D.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateShaderResourceView( image->getTexture3D()
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceView3D" );
			dxDebugName( m_shaderView, UnorderedAccessView3D );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			desc.Texture3D.FirstWSlice = getSubResourceRange().baseArrayLayer;
			desc.Texture3D.WSize = getSubResourceRange().layerCount;
			auto hr = device->CreateUnorderedAccessView( image->getTexture3D()
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessView3D" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessView3D );
		}
	}

	void ImageView::doCreateCube()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );

		if ( image->isRenderTarget() )
		{
			if ( isDepthOrStencilFormat( image->getFormat() ) )
			{
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateDepthStencilView( image->getTexture2D()
					, &desc
					, &m_depthStencilView );
				doCheckViewCreation( hr, m_depthStencilView, "DepthStencilViewCube" );
				dxDebugName( m_depthStencilView, DepthStencilViewCube );
			}
			else
			{
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateRenderTargetView( image->getTexture2D()
					, &desc
					, &m_renderTargetView );
				doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetViewCube" );
				dxDebugName( m_renderTargetView, RenderTargetViewCube );
			}
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			desc.TextureCube.MipLevels = getSubResourceRange().levelCount;
			desc.TextureCube.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateShaderResourceView( image->getTexture2D()
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceViewCube" );
			dxDebugName( m_shaderView, ShaderResourceViewCube );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateUnorderedAccessView( image->getTexture2D()
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessViewCube" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessViewCube );
		}
	}

	void ImageView::doCreateCubeArray()
	{
		auto device = get( m_device )->getDevice();
		auto image = get( m_createInfo.image );

		if ( image->isRenderTarget() )
		{
			if ( isDepthOrStencilFormat( image->getFormat() ) )
			{
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateDepthStencilView( image->getTexture2D()
					, &desc
					, &m_depthStencilView );
				doCheckViewCreation( hr, m_depthStencilView, "DepthStencilViewCube" );
				dxDebugName( m_depthStencilView, DepthStencilViewCube );
			}
			else
			{
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				desc.Format = getRTVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
				desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
				desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
				desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
				auto hr = device->CreateRenderTargetView( image->getTexture2D()
					, &desc
					, &m_renderTargetView );
				doCheckViewCreation( hr, m_renderTargetView, "CreateRenderTargetViewCubeArray" );
				dxDebugName( m_renderTargetView, RenderTargetViewCubeArray );
			}
		}

		if ( image->isSamplable() )
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = getSRVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			desc.TextureCubeArray.MipLevels = getSubResourceRange().levelCount;
			desc.TextureCubeArray.MostDetailedMip = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			desc.TextureCubeArray.First2DArrayFace = getSubResourceRange().baseArrayLayer;
			desc.TextureCubeArray.NumCubes = getSubResourceRange().layerCount / 6u;
			auto hr = device->CreateShaderResourceView( image->getTexture2D()
				, &desc
				, &m_shaderView );
			doCheckViewCreation( hr, m_shaderView, "CreateShaderResourceViewCubeArray" );
			dxDebugName( m_shaderView, ShaderResourceViewCubeArray );
		}

		if ( image->isStorage()
			&& get( m_device )->getFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 )
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = getUAVFormat( imgview::nativeFormat( m_createInfo.image, getFormat() ) );
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = getSubResourceRange().layerCount;
			desc.Texture2DArray.FirstArraySlice = getSubResourceRange().baseArrayLayer;
			desc.Texture2DArray.MipSlice = getSubResourceRange().baseMipLevel + image->getNativeMipPrefix();
			auto hr = device->CreateUnorderedAccessView( image->getTexture2D()
				, &desc
				, &m_unorderedAccessView );
			doCheckViewCreation( hr, m_unorderedAccessView, "CreateUnorderedAccessViewCubeArray" );
			dxDebugName( m_unorderedAccessView, UnorderedAccessViewCubeArray );
		}
	}
}
