/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <vector>

namespace ashes::D3D11_NAMESPACE::hazards
{
	enum class WriteKind : uint8_t
	{
		computeUav,
		outputMergerUav,
		rtv,
		dsv,
	};

	struct Range
	{
		ID3D11Resource * resource{};
		UINT firstMip{};
		UINT mipCount{ 1u };
		UINT firstSlice{};
		UINT sliceCount{ 1u };
		bool wholeResource{};
	};

	inline Range whole( ID3D11View * view )
	{
		Range result;
		if ( view )
		{
			view->GetResource( &result.resource );
			if ( result.resource ) result.resource->Release();
			result.wholeResource = true;
		}
		return result;
	}

	inline Range getRange( ID3D11ShaderResourceView * view )
	{
		if ( !view ) return {};
		Range result = whole( view );
		D3D11_SHADER_RESOURCE_VIEW_DESC d{};
		view->GetDesc( &d );
		result.wholeResource = false;
		switch ( d.ViewDimension )
		{
		case D3D11_SRV_DIMENSION_TEXTURE1D: result.firstMip = d.Texture1D.MostDetailedMip; result.mipCount = d.Texture1D.MipLevels; break;
		case D3D11_SRV_DIMENSION_TEXTURE1DARRAY: result.firstMip = d.Texture1DArray.MostDetailedMip; result.mipCount = d.Texture1DArray.MipLevels; result.firstSlice = d.Texture1DArray.FirstArraySlice; result.sliceCount = d.Texture1DArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE2D: result.firstMip = d.Texture2D.MostDetailedMip; result.mipCount = d.Texture2D.MipLevels; break;
		case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: result.firstMip = d.Texture2DArray.MostDetailedMip; result.mipCount = d.Texture2DArray.MipLevels; result.firstSlice = d.Texture2DArray.FirstArraySlice; result.sliceCount = d.Texture2DArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE2DMS: break;
		case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: result.firstSlice = d.Texture2DMSArray.FirstArraySlice; result.sliceCount = d.Texture2DMSArray.ArraySize; break;
		case D3D11_SRV_DIMENSION_TEXTURE3D: result.firstMip = d.Texture3D.MostDetailedMip; result.mipCount = d.Texture3D.MipLevels; result.firstSlice = 0u; result.sliceCount = std::numeric_limits< UINT >::max(); break;
		case D3D11_SRV_DIMENSION_TEXTURECUBE: result.firstMip = d.TextureCube.MostDetailedMip; result.mipCount = d.TextureCube.MipLevels; result.sliceCount = 6u; break;
		case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: result.firstMip = d.TextureCubeArray.MostDetailedMip; result.mipCount = d.TextureCubeArray.MipLevels; result.firstSlice = d.TextureCubeArray.First2DArrayFace; result.sliceCount = d.TextureCubeArray.NumCubes * 6u; break;
		default: result.wholeResource = true; break;
		}
		return result;
	}

	inline Range getRange( ID3D11UnorderedAccessView * view )
	{
		if ( !view ) return {};
		Range result = whole( view );
		D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
		view->GetDesc( &d );
		result.wholeResource = false;
		switch ( d.ViewDimension )
		{
		case D3D11_UAV_DIMENSION_TEXTURE1D: result.firstMip = d.Texture1D.MipSlice; break;
		case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: result.firstMip = d.Texture1DArray.MipSlice; result.firstSlice = d.Texture1DArray.FirstArraySlice; result.sliceCount = d.Texture1DArray.ArraySize; break;
		case D3D11_UAV_DIMENSION_TEXTURE2D: result.firstMip = d.Texture2D.MipSlice; break;
		case D3D11_UAV_DIMENSION_TEXTURE2DARRAY: result.firstMip = d.Texture2DArray.MipSlice; result.firstSlice = d.Texture2DArray.FirstArraySlice; result.sliceCount = d.Texture2DArray.ArraySize; break;
		case D3D11_UAV_DIMENSION_TEXTURE3D: result.firstMip = d.Texture3D.MipSlice; result.firstSlice = d.Texture3D.FirstWSlice; result.sliceCount = d.Texture3D.WSize; break;
		default: result.wholeResource = true; break;
		}
		return result;
	}

	inline Range getRange( ID3D11RenderTargetView * view )
	{
		if ( !view ) return {};
		Range result = whole( view );
		D3D11_RENDER_TARGET_VIEW_DESC d{};
		view->GetDesc( &d );
		result.wholeResource = false;
		switch ( d.ViewDimension )
		{
		case D3D11_RTV_DIMENSION_TEXTURE1D: result.firstMip = d.Texture1D.MipSlice; break;
		case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: result.firstMip = d.Texture1DArray.MipSlice; result.firstSlice = d.Texture1DArray.FirstArraySlice; result.sliceCount = d.Texture1DArray.ArraySize; break;
		case D3D11_RTV_DIMENSION_TEXTURE2D: result.firstMip = d.Texture2D.MipSlice; break;
		case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: result.firstMip = d.Texture2DArray.MipSlice; result.firstSlice = d.Texture2DArray.FirstArraySlice; result.sliceCount = d.Texture2DArray.ArraySize; break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMS: break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: result.firstSlice = d.Texture2DMSArray.FirstArraySlice; result.sliceCount = d.Texture2DMSArray.ArraySize; break;
		case D3D11_RTV_DIMENSION_TEXTURE3D: result.firstMip = d.Texture3D.MipSlice; result.firstSlice = d.Texture3D.FirstWSlice; result.sliceCount = d.Texture3D.WSize; break;
		default: result.wholeResource = true; break;
		}
		return result;
	}

	inline Range getRange( ID3D11DepthStencilView * view )
	{
		if ( !view ) return {};
		Range result = whole( view );
		D3D11_DEPTH_STENCIL_VIEW_DESC d{};
		view->GetDesc( &d );
		result.wholeResource = false;
		switch ( d.ViewDimension )
		{
		case D3D11_DSV_DIMENSION_TEXTURE1D: result.firstMip = d.Texture1D.MipSlice; break;
		case D3D11_DSV_DIMENSION_TEXTURE1DARRAY: result.firstMip = d.Texture1DArray.MipSlice; result.firstSlice = d.Texture1DArray.FirstArraySlice; result.sliceCount = d.Texture1DArray.ArraySize; break;
		case D3D11_DSV_DIMENSION_TEXTURE2D: result.firstMip = d.Texture2D.MipSlice; break;
		case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: result.firstMip = d.Texture2DArray.MipSlice; result.firstSlice = d.Texture2DArray.FirstArraySlice; result.sliceCount = d.Texture2DArray.ArraySize; break;
		case D3D11_DSV_DIMENSION_TEXTURE2DMS: break;
		case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY: result.firstSlice = d.Texture2DMSArray.FirstArraySlice; result.sliceCount = d.Texture2DMSArray.ArraySize; break;
		default: result.wholeResource = true; break;
		}
		return result;
	}

	inline bool overlaps( Range const & a, Range const & b )
	{
		if ( !a.resource || a.resource != b.resource ) return false;
		if ( a.wholeResource || b.wholeResource ) return true;
		auto const aMipEnd = uint64_t( a.firstMip ) + a.mipCount;
		auto const bMipEnd = uint64_t( b.firstMip ) + b.mipCount;
		auto const aSliceEnd = uint64_t( a.firstSlice ) + a.sliceCount;
		auto const bSliceEnd = uint64_t( b.firstSlice ) + b.sliceCount;
		return a.firstMip < bMipEnd && b.firstMip < aMipEnd
			&& a.firstSlice < bSliceEnd && b.firstSlice < aSliceEnd;
	}

	struct WriteSlot
	{
		WriteKind kind{};
		UINT slot{};
		Range range{};
	};

	// v13.52: both switches are read once. They were asked through std::getenv on EVERY
	// texture bind (writesOverlapping() below runs for each SRV a descriptor set binds), and
	// the CRT getenv locks and scans the whole environment block: measured as most of the
	// 2-4 ms per frame that BindDescriptorSet cost on the laptop ("binds tex" in [RW-GPU-PROF]).
	inline bool enabled()
	{
		static bool const result = []()
			{
				auto value = std::getenv( "RW_HD5500_HAZARD_TRACKING" );
				return !( value && value[0] == '0' );
			}();
		return result;
	}

	inline bool diagnostics()
	{
		static bool const result = []()
			{
				auto value = std::getenv( "RW_HD5500_D3D_DIAGNOSTICS" );
				return value && value[0] != '0';
			}();
		return result;
	}

	class Tracker
	{
	public:
		void reset() noexcept
		{
			m_writes.clear();
			m_descriptorUavs.clear();
			++m_version;
		}

		// v13.72: changes whenever the set of written resources changes (targets, UAVs). While it is
		// unchanged, an SRV that is already bound in a slot cannot have gained a hazard.
		uint64_t version()const noexcept
		{
			return m_version;
		}

		void beginDescriptor( std::vector< ID3D11UnorderedAccessView * > const & uavs )
		{
			++m_version;
			m_descriptorUavs.clear();
			for ( auto * uav : uavs ) m_descriptorUavs.push_back( getRange( uav ) );
		}

		void setComputeUav( UINT slot, ID3D11UnorderedAccessView * view )
		{
			setWrite( WriteKind::computeUav, slot, getRange( view ) );
		}

		void setOutputUav( UINT slot, ID3D11UnorderedAccessView * view )
		{
			setWrite( WriteKind::outputMergerUav, slot, getRange( view ) );
		}

		void setTargets( std::vector< ID3D11RenderTargetView * > const & rtvs, ID3D11DepthStencilView * dsv )
		{
			++m_version;
			m_writes.erase( std::remove_if( m_writes.begin(), m_writes.end(), []( WriteSlot const & item )
				{ return item.kind == WriteKind::rtv || item.kind == WriteKind::dsv; } ), m_writes.end() );
			for ( UINT i = 0u; i < rtvs.size(); ++i ) setWrite( WriteKind::rtv, i, getRange( rtvs[i] ) );
			if ( dsv ) setWrite( WriteKind::dsv, 0u, getRange( dsv ) );
		}

		void clearTargets() noexcept
		{
			++m_version;
			m_writes.erase( std::remove_if( m_writes.begin(), m_writes.end(), []( WriteSlot const & item )
				{ return item.kind == WriteKind::rtv || item.kind == WriteKind::dsv; } ), m_writes.end() );
		}

		std::vector< WriteSlot > writesOverlapping( ID3D11ShaderResourceView * srv, bool & sameDescriptor )const
		{
			std::vector< WriteSlot > result;
			sameDescriptor = false;
			// v13.52: nothing is being written -> nothing can overlap; skip the COM queries.
			if ( !enabled() || !srv || ( m_writes.empty() && m_descriptorUavs.empty() ) ) return result;
			auto range = getRange( srv );
			for ( auto const & incoming : m_descriptorUavs )
			{
				if ( overlaps( range, incoming ) ) { sameDescriptor = true; break; }
			}
			for ( auto const & write : m_writes )
			{
				if ( overlaps( range, write.range ) ) result.push_back( write );
			}
			return result;
		}

		void erase( WriteKind kind, UINT slot )
		{
			++m_version;
			m_writes.erase( std::remove_if( m_writes.begin(), m_writes.end(), [kind, slot]( WriteSlot const & item )
				{ return item.kind == kind && item.slot == slot; } ), m_writes.end() );
		}

	private:
		void setWrite( WriteKind kind, UINT slot, Range range )
		{
			erase( kind, slot );   // bumps m_version
			if ( range.resource ) m_writes.push_back( { kind, slot, range } );
		}

		std::vector< WriteSlot > m_writes;
		std::vector< Range > m_descriptorUavs;
		uint64_t m_version{ 1u };
	};

	inline void diagnostic( char const * kind, char const * stage, UINT slot, UINT count )
	{
		if ( diagnostics() )
		{
			static uint32_t repeats{};
			if ( repeats++ < 32u ) std::fprintf( stderr, "[RW-GPU-HAZARD] kind=%s stage=%s slot=%u count=%u\n", kind, stage, slot, count );
		}
	}
}
