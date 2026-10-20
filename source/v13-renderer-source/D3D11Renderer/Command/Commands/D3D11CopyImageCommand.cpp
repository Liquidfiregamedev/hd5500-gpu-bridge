/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11CopyImageCommand.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"

#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11BcCopy.hpp"
#include "Miscellaneous/D3D11RenderDiagnostics.hpp"

#include "Miscellaneous/D3D11EnvFlags.hpp"
#include "ashesd3d11_api.hpp"

#include <ashes/common/Exception.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ashes::D3D11_NAMESPACE
{
		namespace copyimg
		{
			// v12: decoded BC images hold RGBA8 texels, every other image holds its
			// Vulkan format's bytes. A copy across the two representations would
			// move incompatible bytes, so it fails with VK_ERROR_FORMAT_NOT_SUPPORTED.
			static void checkRepresentations( VkImage src, VkImage dst )
			{
				auto const srcDecoded = get( src )->isDecodedBc();
				auto const dstDecoded = get( dst )->isDecodedBc();
				if ( srcDecoded == dstDecoded ) return;
				uint32_t const key[] = { uint32_t( get( src )->getFormat() ), get( src )->getDimensions().width, get( src )->getDimensions().height
					, uint32_t( get( dst )->getFormat() ), get( dst )->getDimensions().width, get( dst )->getDimensions().height };
				rdiag::report( "bc-copy-representation-mismatch", rdiag::hashBytes( 0x5248u, key, sizeof( key ) )
					, "src=%u:%ux%u:%s dst=%u:%ux%u:%s result=VK_ERROR_FORMAT_NOT_SUPPORTED"
					, key[0], key[1], key[2], srcDecoded ? "decoded-rgba8" : "native"
					, key[3], key[4], key[5], dstDecoded ? "decoded-rgba8" : "native" );
				throw Exception{ VK_ERROR_FORMAT_NOT_SUPPORTED, "vkCmdCopyImage between decoded and native BC representations" };
			}

			static VkExtent3D native2DMipExtent( VkImage image, UINT subresource )
			{
				if ( get( image )->getType() != VK_IMAGE_TYPE_2D ) return {};
				auto texture = get( image )->getTexture2D();
				if ( !texture ) return {};
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc( &desc );
				if ( !desc.MipLevels || subresource >= desc.MipLevels * desc.ArraySize ) return {};
				auto mip = subresource % desc.MipLevels;
				return { std::max( 1u, desc.Width >> mip ), std::max( 1u, desc.Height >> mip ), 1u };
			}

			static D3D11_BOX doGetSrcBox( VkImageCopy const & copyInfo )
			{
				return
			{
				UINT( copyInfo.srcOffset.x ),
				UINT( copyInfo.srcOffset.y ),
				UINT( copyInfo.srcOffset.z ),
				UINT( copyInfo.srcOffset.x ) + copyInfo.extent.width,
				UINT( copyInfo.srcOffset.y ) + copyInfo.extent.height,
				UINT( copyInfo.srcOffset.z ) + copyInfo.extent.depth,
			};
		}
	}

	CopyImageCommand::CopyImageCommand( VkDevice device
		, VkImageCopy const & copyInfo
		, VkImage src
		, VkImage dst )
		: CommandBase{ device }
		, m_src{ src }
		, m_dst{ dst }
		, m_copyInfo{ copyInfo }
		, m_srcBox{ copyimg::doGetSrcBox( m_copyInfo ) }
		, m_srcSubresource{ get( m_src )->getNativeSubresource(
			m_copyInfo.srcSubresource.mipLevel
			, m_copyInfo.srcSubresource.baseArrayLayer ) }
		, m_dstSubresource{ get( m_dst )->getNativeSubresource(
			m_copyInfo.dstSubresource.mipLevel
			, m_copyInfo.dstSubresource.baseArrayLayer ) }
	{
		copyimg::checkRepresentations( m_src, m_dst );
	}

	void CopyImageCommand::apply( Context & context )const
	{
		get( m_dst )->bumpContentVersion(); // v13.14
		framediag::onCopyTo( get( m_dst )->getResource(), get( m_src )->getResource(), "vkCmdCopyImage" );
		for ( auto layer = 0u; layer < m_copyInfo.srcSubresource.layerCount; ++layer )
		{
			// D3D resource indices include the hidden BC mips. Shadow-memory indices do not.
			auto const srcSubresource = layer
				? get( m_src )->getNativeSubresource( m_copyInfo.srcSubresource.mipLevel
					, m_copyInfo.srcSubresource.baseArrayLayer + layer )
				: m_srcSubresource;
			auto const dstSubresource = layer
				? get( m_dst )->getNativeSubresource( m_copyInfo.dstSubresource.mipLevel
					, m_copyInfo.dstSubresource.baseArrayLayer + layer )
				: m_dstSubresource;

			if ( isDepthOrStencilFormat( get( m_src )->getFormat() ) )
			{
				context.context->CopySubresourceRegion( get( m_dst )->getResource()
					, dstSubresource
					, 0
					, 0
					, 0
					, get( m_src )->getResource()
					, srcSubresource
					, nullptr );
				}
				else
				{
					auto const format = get( m_src )->getFormat();
					auto const wholeBc = bccopy::wholeSubresource( format, m_copyInfo
						, copyimg::native2DMipExtent( m_src, srcSubresource )
						, copyimg::native2DMipExtent( m_dst, dstSubresource ) );
					if ( format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK )
					{
						static std::atomic_uint traceCount{};
						if ( envflags::textureTrace() && traceCount.fetch_add( 1u ) < 64u )
						{
							std::fprintf( stderr, "[RW-GPU-BC-COPY] mode=%s format=%u srcSub=%u dstSub=%u extent=%ux%ux%u srcOrigin=%d,%d,%d dstOrigin=%d,%d,%d\n"
								, wholeBc ? "whole-null-box" : "explicit-region", unsigned( format ), srcSubresource, dstSubresource
								, m_copyInfo.extent.width, m_copyInfo.extent.height, m_copyInfo.extent.depth
								, m_copyInfo.srcOffset.x, m_copyInfo.srcOffset.y, m_copyInfo.srcOffset.z
								, m_copyInfo.dstOffset.x, m_copyInfo.dstOffset.y, m_copyInfo.dstOffset.z );
								std::fflush( stderr );
						}
					}
					context.context->CopySubresourceRegion( get( m_dst )->getResource()
						, dstSubresource
						, UINT( m_copyInfo.dstOffset.x )
						, UINT( m_copyInfo.dstOffset.y )
						, UINT( m_copyInfo.dstOffset.z )
						, get( m_src )->getResource()
						, srcSubresource
						, wholeBc ? nullptr : &m_srcBox );
				}

				// DeviceMemory/ShadowTexture stores logical subresources; never reuse native D3D indices here.
			auto const dstLogicalSubresource = D3D11CalcSubresource(
				m_copyInfo.dstSubresource.mipLevel
				, m_copyInfo.dstSubresource.baseArrayLayer + layer
				, get( m_dst )->getMipmapLevels() );
			auto dstMemory = get( m_dst )->getMemory();
			get( dstMemory )->updateDownload( context.context, 0u
				, get( m_dst )->getMemoryRequirements().size, dstLogicalSubresource );
		}
	}

	CommandPtr CopyImageCommand::clone()const
	{
		return std::make_unique< CopyImageCommand >( *this );
	}
}
