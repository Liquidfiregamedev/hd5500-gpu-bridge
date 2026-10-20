/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/Commands/D3D11BindDescriptorSetCommand.hpp"
#include "Miscellaneous/D3D11WatchedBytes.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Buffer/D3D11BufferView.hpp"
#include "Descriptor/D3D11DescriptorSet.hpp"
#include "Pipeline/D3D11PipelineLayout.hpp"
#include "Image/D3D11Sampler.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11UavAlias.hpp"
#include "Miscellaneous/D3D11UniformRing.hpp"
#include "Miscellaneous/D3D11DeviceMemory.hpp"

#include "ashesd3d11_api.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

namespace ashes::D3D11_NAMESPACE
{
	namespace binddesc
	{
		static ID3D11ShaderResourceView * getView( Context & context, VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			auto view = get( write.pImageInfo[index].imageView );
			if ( !view->hasSwizzle() ) return view->getShaderView();
			// v13.14: swizzle emulation may draw into a shadow texture; the bridge's
			// state cache must not assume its last-set state survived that.
			// v13.51: invalidate only when a redraw really happened (the shadow is cached per
			// image content version, so most binds are a plain lookup). Before, every bind of a
			// swizzled view threw the whole cache away: ~2.6 million invalidations per map test.
			bool rendered = false;
			auto result = view->getSampledView( context.context.operator->(), &rendered );
			if ( rendered ) context.stateCache.invalidate();
			return result;
		}
		
		static ID3D11ShaderResourceView * getBufferView( VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			return get( write.pTexelBufferView[index] )->getView();
		}
		
		static ID3D11UnorderedAccessView * getImageUAV( VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			return get( write.pImageInfo[index].imageView )->getUnorderedAccessView();
		}

		static ID3D11SamplerState * getSampler( VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			return get( write.pImageInfo[index].sampler )->getSampler();
		}

		static ID3D11Buffer * getBuffer( VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			return get( write.pBufferInfo[index].buffer )->getBuffer();
		}

		static ID3D11UnorderedAccessView * getBufferUAV( VkWriteDescriptorSet const & write, uint32_t index )
		{
			assert( index < write.descriptorCount );
			// v13.47b: honour the descriptor's offset/range (ring-buffer sub-allocations).
			return get( write.pBufferInfo[index].buffer )->getUnorderedAccessView( write.pBufferInfo[index].offset
				, write.pBufferInfo[index].range );
		}

		static char const * getStageName( statecache::Stage stage )
		{
			switch ( stage )
			{
			case statecache::Stage::compute: return "CS";
			case statecache::Stage::vertex: return "VS";
			case statecache::Stage::geometry: return "GS";
			case statecache::Stage::hull: return "HS";
			case statecache::Stage::domain: return "DS";
			case statecache::Stage::pixel: return "PS";
			}
			return "?";
		}

		static void setSamplerNative( Context & context, statecache::Stage stage, UINT slot, ID3D11SamplerState * value )
		{
			switch ( stage )
			{
			case statecache::Stage::compute: context.context->CSSetSamplers( slot, 1u, &value ); break;
			case statecache::Stage::vertex: context.context->VSSetSamplers( slot, 1u, &value ); break;
			case statecache::Stage::geometry: context.context->GSSetSamplers( slot, 1u, &value ); break;
			case statecache::Stage::hull: context.context->HSSetSamplers( slot, 1u, &value ); break;
			case statecache::Stage::domain: context.context->DSSetSamplers( slot, 1u, &value ); break;
			case statecache::Stage::pixel: context.context->PSSetSamplers( slot, 1u, &value ); break;
			}
		}

		static void setSrvNative( Context & context, statecache::Stage stage, UINT slot, ID3D11ShaderResourceView * value )
		{
			switch ( stage )
			{
			case statecache::Stage::compute: context.context->CSSetShaderResources( slot, 1u, &value ); break;
			case statecache::Stage::vertex: context.context->VSSetShaderResources( slot, 1u, &value ); break;
			case statecache::Stage::geometry: context.context->GSSetShaderResources( slot, 1u, &value ); break;
			case statecache::Stage::hull: context.context->HSSetShaderResources( slot, 1u, &value ); break;
			case statecache::Stage::domain: context.context->DSSetShaderResources( slot, 1u, &value ); break;
			case statecache::Stage::pixel: context.context->PSSetShaderResources( slot, 1u, &value ); break;
			}
		}

		static void setConstantBufferNative( Context & context, statecache::Stage stage, UINT slot, ID3D11Buffer * value, UINT first, UINT count, bool extended )
		{
			static bool const trace = std::getenv( "RW_GPU_CB_TRACE" ) != nullptr;
			if ( trace ) std::fprintf( stderr, "[RW-GPU-CB] stage=%d slot=%u buf=%p first=%u count=%u ext=%d\n", int( stage ), slot, static_cast< void * >( value ), first, count, int( extended ) );
			if ( extended )
			{
				switch ( stage )
				{
				case statecache::Stage::compute: context.context1->CSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				case statecache::Stage::vertex: context.context1->VSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				case statecache::Stage::geometry: context.context1->GSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				case statecache::Stage::hull: context.context1->HSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				case statecache::Stage::domain: context.context1->DSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				case statecache::Stage::pixel: context.context1->PSSetConstantBuffers1( slot, 1u, &value, &first, &count ); break;
				}
			}
			else
			{
				switch ( stage )
				{
				case statecache::Stage::compute: context.context->CSSetConstantBuffers( slot, 1u, &value ); break;
				case statecache::Stage::vertex: context.context->VSSetConstantBuffers( slot, 1u, &value ); break;
				case statecache::Stage::geometry: context.context->GSSetConstantBuffers( slot, 1u, &value ); break;
				case statecache::Stage::hull: context.context->HSSetConstantBuffers( slot, 1u, &value ); break;
				case statecache::Stage::domain: context.context->DSSetConstantBuffers( slot, 1u, &value ); break;
				case statecache::Stage::pixel: context.context->PSSetConstantBuffers( slot, 1u, &value ); break;
				}
			}
		}

		static void flushOutputUavs( Context & context )
		{
			context.rawUavs.clear();
			if ( context.uavs.empty() )
			{
				context.context1->OMSetRenderTargetsAndUnorderedAccessViews( D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, context.uavStart, 0u, nullptr, nullptr );
				return;
			}
			// v13.15: keys are binding indices relative to uavStart.
			auto last = context.uavs.rbegin()->first;
			context.rawUavs.resize( size_t( last ) + 1u, nullptr );
			for ( auto const & entry : context.uavs )
			{
				context.rawUavs[entry.first] = entry.second;
			}
			// v13.53: views sharing a buffer would make D3D11 drop the whole bind (see D3D11UavAlias.hpp).
			context.uavAliased = uavalias::bindOutput( get( context.device )->getDevice(), *context.context, context.uavStart, UINT( context.rawUavs.size() ), context.rawUavs.data() );
			for ( auto const & entry : context.uavs ) context.hazardTracker.setOutputUav( entry.first, entry.second );
			context.uavAbsEnd = std::max( context.uavAbsEnd, UINT( context.uavStart + context.rawUavs.size() ) );
		}

		static void resolveSrvHazards( Context & context, statecache::Stage stage, UINT slot, ID3D11ShaderResourceView * view )
		{
			bool sameDescriptor{};
			auto writes = context.hazardTracker.writesOverlapping( view, sameDescriptor );
			if ( writes.empty() ) return;
			bool feedback = sameDescriptor || std::any_of( writes.begin(), writes.end(), []( hazards::WriteSlot const & item )
				{ return item.kind == hazards::WriteKind::rtv || item.kind == hazards::WriteKind::dsv; } );
			if ( feedback )
			{
				hazards::diagnostic( sameDescriptor ? "same-descriptor-srv-uav" : "renderpass-feedback", getStageName( stage ), slot, UINT( writes.size() ) );
				return;
			}
			bool outputDirty{};
			for ( auto const & write : writes )
			{
				if ( write.kind == hazards::WriteKind::computeUav )
				{
					ID3D11UnorderedAccessView * nullView{};
					context.context->CSSetUnorderedAccessViews( write.slot, 1u, &nullView, nullptr );
					context.hazardTracker.erase( write.kind, write.slot );
				}
				else if ( write.kind == hazards::WriteKind::outputMergerUav )
				{
					context.uavs.erase( write.slot );
					context.hazardTracker.erase( write.kind, write.slot );
					outputDirty = true;
				}
			}
			if ( outputDirty ) flushOutputUavs( context );
			context.stateCache.invalidateSrvs();   // v13.44
		}

		static void setSampler( Context & context, statecache::Stage stage, UINT slot, ID3D11SamplerState * value )
		{
			if ( context.stateCache.needsSet( stage, statecache::Kind::sampler, slot, value ) ) setSamplerNative( context, stage, slot, value );
		}

		static void setSrv( Context & context, statecache::Stage stage, UINT slot, ID3D11ShaderResourceView * value )
		{
			// v13.72: state-cache check first. The hazard check costs 3 COM calls per SRV during a
			// render pass (GetResource/Release/GetDesc) and ran even when the same view was already
			// bound in that slot. Safe because the SRV cache entries are dropped whenever the hazard
			// tracker's set of written resources changes (version). RW_GPU_SRV_CACHE_FIRST=0 = old order.
			static bool const cacheFirst = envflags::on( "RW_GPU_SRV_CACHE_FIRST", true );
			if ( !cacheFirst )
			{
				resolveSrvHazards( context, stage, slot, value );
				if ( context.stateCache.needsSet( stage, statecache::Kind::srv, slot, value ) ) setSrvNative( context, stage, slot, value );
				return;
			}
			auto const version = context.hazardTracker.version();
			if ( version != context.srvHazardVersion )
			{
				context.stateCache.invalidateSrvs();
				context.srvHazardVersion = version;
			}
			if ( !context.stateCache.needsSet( stage, statecache::Kind::srv, slot, value ) ) return;
			resolveSrvHazards( context, stage, slot, value );
			if ( context.hazardTracker.version() != version )
			{
				// the resolve unbound a UAV (and invalidated the SRV cache); remember what is bound now
				context.srvHazardVersion = context.hazardTracker.version();
				context.stateCache.needsSet( stage, statecache::Kind::srv, slot, value );
			}
			setSrvNative( context, stage, slot, value );
		}

		// v13.28: feature level 11_0 (e.g. Ivy Bridge HD 4000, and Wine's WineD3D) has no
		// *SetConstantBuffers1, so a uniform-buffer window at a non-zero offset (dynamic
		// uniform buffers, sub-allocated uniform buffers) cannot be bound directly. The old
		// code bound the buffer from offset 0: every draw read the first element. Copy the
		// window into a small scratch constant buffer from a ring and bind that instead.
		// GPU commands execute in order, so reusing a ring slot is always correct.
		struct CbWindowRing
		{
			ID3D11Device * device{};
			ID3D11Buffer * buffers[ 128 ]{};
			uint32_t next{};
			~CbWindowRing()noexcept { for ( auto & b : buffers ) safeRelease( b ); }
		};

		static ID3D11Buffer * copyConstantWindow( Context & context, ID3D11Buffer * source, UINT firstConstant, UINT numConstants )
		{
			static std::mutex mutex;
			static std::map< ID3D11Device *, std::unique_ptr< CbWindowRing > > rings;
			ID3D11Device * d3d{};
			context.context->GetDevice( &d3d );
			if ( !d3d ) return nullptr;
			std::lock_guard< std::mutex > lock{ mutex };
			auto & ring = rings[ d3d ];
			if ( !ring ) { ring = std::make_unique< CbWindowRing >(); ring->device = d3d; }
			d3d->Release();
			D3D11_BUFFER_DESC sd{};
			source->GetDesc( &sd );
			UINT const begin = firstConstant * 16u;
			if ( begin >= sd.ByteWidth ) return nullptr;
			UINT const count = numConstants ? numConstants : 4096u;
			UINT const end = std::min( sd.ByteWidth, begin + std::min( count, 4096u ) * 16u );
			auto & slot = ring->buffers[ ring->next++ % 128u ];
			if ( !slot )
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = 65536u;
				bd.Usage = D3D11_USAGE_DEFAULT;
				bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				if ( FAILED( ring->device->CreateBuffer( &bd, nullptr, &slot ) ) ) { slot = nullptr; return nullptr; }
			}
			D3D11_BOX box{ begin, 0u, 0u, end, 1u, 1u };
			context.context->CopySubresourceRegion( slot, 0u, 0u, 0u, 0u, source, 0u, &box );
			static std::atomic< uint32_t > logged{ 0u };
			if ( logged++ == 0u ) std::fprintf( stderr, "[RW-GPU-CBWINDOW] feature level 11_0: uniform-buffer windows at non-zero offsets are copied to scratch buffers\n" );
			return slot;
		}

		template< bool Extended >
		static void setConstantBuffer( Context & context, statecache::Stage stage, UINT slot, ID3D11Buffer * value, UINT first, UINT count )
		{
			// RW_GPU_CB_EMULATE=1 forces the copy path on 11_1 too (tests it in the emulator,
			// whose WineD3D ignores constant-buffer offsets, and A/B tests on real drivers).
			static bool const forceCopy = []()
			{
				auto value = std::getenv( "RW_GPU_CB_EMULATE" );
				return value && value[0] == '1';
			}();
			if ( ( !Extended || forceCopy ) && value && first != 0u )
			{
				if ( auto scratch = copyConstantWindow( context, value, first, count ) )
				{
					if ( context.stateCache.needsSet( stage, statecache::Kind::constantBuffer, slot, scratch, 0u, 0u, false ) )
						setConstantBufferNative( context, stage, slot, scratch, 0u, 0u, false );
					return;
				}
			}
			if ( context.stateCache.needsSet( stage, statecache::Kind::constantBuffer, slot, value, first, count, Extended ) ) setConstantBufferNative( context, stage, slot, value, first, count, Extended );
		}

		static void setComputeUav( Context & context, UINT slot, ID3D11UnorderedAccessView * value )
		{
			context.context->CSSetUnorderedAccessViews( slot, 1u, &value, nullptr );
			context.hazardTracker.setComputeUav( slot, value );
			context.stateCache.invalidateSrvs();   // v13.44
		}

		template< VkShaderStageFlagBits Flag, bool Supports11_1 >
		struct Binder;

		template<>
		struct Binder< VK_SHADER_STAGE_COMPUTE_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::compute, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::compute, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::compute, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void bind( Context & context, UINT bindingIndex, ID3D11UnorderedAccessView * view )
			{
				setComputeUav( context, bindingIndex, view );
			}
			// Storage buffer/image
			static void gather( UINT, ID3D11UnorderedAccessView const *, std::map< UINT, ID3D11UnorderedAccessView * > const * )
			{
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_VERTEX_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::vertex, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::vertex, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::vertex, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_GEOMETRY_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::geometry, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::geometry, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::geometry, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::hull, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::hull, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::hull, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::domain, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::domain, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::domain, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_FRAGMENT_BIT, true >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::pixel, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::pixel, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< true >( context, statecache::Stage::pixel, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		static void checkOffset( uint32_t offset )
		{
#ifndef NDEBUG
			if ( offset != 0u )
			{
				std::cerr << "Binding buffer range not supported by this driver" << std::endl;
			}
#endif
		}

		template<>
		struct Binder< VK_SHADER_STAGE_COMPUTE_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::compute, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::compute, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::compute, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void bind( Context & context, UINT bindingIndex, ID3D11UnorderedAccessView * view )
			{
				setComputeUav( context, bindingIndex, view );
			}
			// Storage buffer/image
			static void gather( UINT, ID3D11UnorderedAccessView const *, std::map< UINT, ID3D11UnorderedAccessView * > const * )
			{
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_VERTEX_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::vertex, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::vertex, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::vertex, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_GEOMETRY_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::geometry, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::geometry, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::geometry, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::hull, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::hull, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::hull, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::domain, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::domain, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::domain, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template<>
		struct Binder< VK_SHADER_STAGE_FRAGMENT_BIT, false >
		{
			// Combined Image + Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image, ID3D11SamplerState * sampler )
			{
				bind( context, bindingIndex, image );
				bind( context, bindingIndex, sampler );
			}
			// Sampler
			static void bind( Context & context, UINT bindingIndex, ID3D11SamplerState * sampler )
			{
				setSampler( context, statecache::Stage::pixel, bindingIndex, sampler );
			}
			// Sampled Image
			static void bind( Context & context, UINT bindingIndex, ID3D11ShaderResourceView * image )
			{
				setSrv( context, statecache::Stage::pixel, bindingIndex, image );
			}
			// Uniform Buffer
			static void bind( Context & context, UINT bindingIndex, ID3D11Buffer * buffer, UINT offset, UINT range )
			{
				setConstantBuffer< false >( context, statecache::Stage::pixel, bindingIndex, buffer, offset, range );
			}
			// Storage buffer/image
			static void gather( UINT bindingIndex, ID3D11UnorderedAccessView * view, std::map< UINT, ID3D11UnorderedAccessView * > * uavs )
			{
				( *uavs )[bindingIndex] = view;
			}
		};

		template< VkShaderStageFlagBits Flag, bool Supports11_1, typename ... Params >
		static void tryBindOne( Context & context
			, UINT bindingIndex
			, VkShaderStageFlags const & flags
			, Params ... params )
		{
			if ( checkFlag( flags, Flag ) )
			{
				Binder< Flag, Supports11_1 >::bind( context
					, bindingIndex
					, params... );
			}
		}

		template< bool Supports11_1, typename ... Params >
		static void tryBind( Context & context
			, UINT bindingIndex
			, VkShaderStageFlags const & flags
			, Params ... params )
		{
			tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
			tryBindOne< VK_SHADER_STAGE_VERTEX_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
			tryBindOne< VK_SHADER_STAGE_GEOMETRY_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
			tryBindOne< VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
			tryBindOne< VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
			tryBindOne< VK_SHADER_STAGE_FRAGMENT_BIT, Supports11_1 >( context
				, bindingIndex
				, flags
				, params... );
		}

		template< bool Supports11_1 >
		static void bindCombinedSampler( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getView( context, write, i );
					auto sampler = getSampler( write, i );
					tryBind< Supports11_1 >( context, bindingIndex, flags, view, sampler );
				}
			}
		}
		
		template< bool Supports11_1 >
		static void bindInputAttachment( Context & context
			, VkSampler sampler
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getView( context, write, i );
					tryBind< Supports11_1 >( context, bindingIndex, flags, view, get( sampler )->getSampler() );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindSampler( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto sampler = getSampler( write, i );
					tryBind< Supports11_1 >( context, bindingIndex, flags, sampler );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindSampledTexture( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getView( context, write, i );
					tryBind< Supports11_1 >( context, bindingIndex, flags, view );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindUniformBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, size_t * ringCursor = nullptr )   // v13.75: element counter for the uniform ring (plain uniform buffers only)
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() )   // v13.47: binding not in this layout, skip (was UB)
				{
					if ( ringCursor ) *ringCursor += write.descriptorCount;
					continue;
				}
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					// v13.75: window copied into the uniform ring at the start of the execution
					if ( ringCursor )
					{
						size_t const element = ( *ringCursor )++;
						if ( Supports11_1 && context.ubRing )
						{
							auto const & view = *context.ubRing;
							int32_t const w = element < view.staticToWindow.size() ? view.staticToWindow[element] : -1;
							if ( w >= 0 )
							{
								auto const & window = view.windows[size_t( w )];
								if ( window.ok )
								{
									tryBind< Supports11_1 >( context, bindingIndex, flags, view.ringBuffer, window.firstConstant, window.bindBytes / 16u );
									continue;
								}
								if ( !window.lifetime.expired() && window.memory && window.memory->resource )
									window.parent->updateUpload( context.context, *window.memory, window.objectOffset, window.copyBytes, 0u );
								ubring::g_fallbacks.fetch_add( 1u, std::memory_order_relaxed );
							}
						}
					}
					auto buffer = getBuffer( write, i );
					auto range = std::min( write.pBufferInfo[i].range
						, get( write.pBufferInfo[i].buffer )->getSize() );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, buffer
						, UINT( write.pBufferInfo[i].offset ) / 16
						, UINT( ashes::getAlignedSize( range, 256ULL ) / 16ULL ) );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindUniformTexelBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getBufferView( write, i );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, view );
				}
			}
		}

		static void getDynamicUniformBufferRange( VkDescriptorBufferInfo const & info
			, uint32_t dynamicOffset
			, UINT & firstConstant
			, UINT & constantCount )
		{
			auto const bufferSize = get( info.buffer )->getSize();

			if ( info.offset > bufferSize
				|| VkDeviceSize( dynamicOffset ) > bufferSize - info.offset )
			{
				throw ashes::BaseException{ "Dynamic uniform buffer offset exceeds the buffer" };
			}

			auto const offset = info.offset + VkDeviceSize( dynamicOffset );
			auto const available = bufferSize - offset;
			auto const range = info.range == VK_WHOLE_SIZE
				? available
				: info.range;

			if ( range == 0u
				|| range > available
				|| ( offset % 256u ) != 0u )
			{
				throw ashes::BaseException{ "Invalid dynamic uniform buffer range" };
			}

			auto const alignedRange = ashes::getAlignedSize( range, 256ULL );

			if ( offset / 16u > std::numeric_limits< UINT >::max()
				|| alignedRange / 16u > std::numeric_limits< UINT >::max() )
			{
				throw ashes::BaseException{ "Dynamic uniform buffer range exceeds D3D11 limits" };
			}

			firstConstant = UINT( offset / 16u );
			constantCount = UINT( alignedRange / 16u );
		}

		template< bool Supports11_1 >
		static void bindDynamicUniformBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, UInt32Array const & offsets
			, size_t offsetIndex )
		{
			assert( offsetIndex + writeBinding.binding.descriptorCount <= offsets.size() );

			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					// v13.74: window copied into the uniform ring at the start of the execution
					if ( Supports11_1 && context.ubRing )
					{
						size_t const element = offsetIndex + write.dstArrayElement + i;
						auto const & view = *context.ubRing;
						int32_t const w = element < view.elementToWindow.size() ? view.elementToWindow[element] : -1;
						if ( w >= 0 )
						{
							auto const & window = view.windows[size_t( w )];
							if ( window.ok )
							{
								tryBind< Supports11_1 >( context, bindingIndex, flags, view.ringBuffer, window.firstConstant, window.bindBytes / 16u );
								continue;
							}
							// fallback: the upload this window's recording skipped, then the old bind
							if ( !window.lifetime.expired() && window.memory && window.memory->resource )
							{
								window.parent->updateUpload( context.context, *window.memory, window.objectOffset, window.copyBytes, 0u );
							}
							ubring::g_fallbacks.fetch_add( 1u, std::memory_order_relaxed );
						}
					}
					UINT firstConstant{};
					UINT constantCount{};
					getDynamicUniformBufferRange( write.pBufferInfo[i]
						, offsets[offsetIndex + write.dstArrayElement + i]
						, firstConstant
						, constantCount );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, getBuffer( write, i )
						, firstConstant
						, constantCount );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindStorageImage( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getImageUAV( write, i );
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, view );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindStorageTexelBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getBufferView( write, i );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, view );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindStorageBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getBufferUAV( write, i );
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, view );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindDynamicStorageBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, UInt32Array const & offsets
			, size_t offsetIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					size_t const dynIndex = offsetIndex + write.dstArrayElement + i;   // v13.49
					VkDeviceSize const dyn = dynIndex < offsets.size() ? offsets[dynIndex] : 0u;
					auto const & info = write.pBufferInfo[i];
					auto view = get( info.buffer )->getUnorderedAccessView( info.offset + dyn, info.range );
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, view );
				}
			}
		}

		template< bool Supports11_1 >
		static void bindDynamicBuffers( Context & context
			, LayoutBindingWritesArray const & writes
			, ShaderBindings const & bindings
			, uint32_t setIndex
			, UInt32Array const & offsets )
		{
			size_t offsetIndex{};

			for ( auto const * write : writes )
			{
				auto const & writeBinding = *write;
				assert( offsetIndex + writeBinding.binding.descriptorCount <= offsets.size() );

				switch ( writeBinding.binding.descriptorType )
				{
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
					bindDynamicUniformBuffer< Supports11_1 >( context, writeBinding, bindings.ubo, setIndex, offsets, offsetIndex );
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
					bindDynamicStorageBuffer< Supports11_1 >( context, writeBinding, bindings.uav, setIndex, offsets, offsetIndex );   // v13.49: uav numbering = shader register
					break;

				default:
					assert( false && "Unsupported dynamic descriptor type" );
					throw ashes::BaseException{ "Unsupported dynamic descriptor type" };
				}

				offsetIndex += writeBinding.binding.descriptorCount;
			}

			assert( offsetIndex == offsets.size() );
		}

		template< VkShaderStageFlagBits Flag, typename ... Params >
		static void tryGatherOne( UINT bindingIndex
			, VkShaderStageFlags const & flags
			, Params ... params )
		{
			if ( checkFlag( flags, Flag ) )
			{
				Binder< Flag, true >::gather( bindingIndex
					, params... );
			}
		}

		template< typename ... Params >
		static void tryGather( UINT bindingIndex
			, VkShaderStageFlags const & flags
			, Params ... params )
		{
			tryGatherOne< VK_SHADER_STAGE_COMPUTE_BIT >( bindingIndex
				, flags
				, params... );
			tryGatherOne< VK_SHADER_STAGE_VERTEX_BIT >( bindingIndex
				, flags
				, params... );
			tryGatherOne< VK_SHADER_STAGE_GEOMETRY_BIT >( bindingIndex
				, flags
				, params... );
			tryGatherOne< VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT >( bindingIndex
				, flags
				, params... );
			tryGatherOne< VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT >( bindingIndex
				, flags
				, params... );
			tryGatherOne< VK_SHADER_STAGE_FRAGMENT_BIT >( bindingIndex
				, flags
				, params... );
		}

		static void gatherStorageImage( LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, std::map< UINT, ID3D11UnorderedAccessView * > & uavs )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getImageUAV( write, i );
					tryGather( bindingIndex
						, flags
						, view
						, &uavs );
				}
			}
		}

		static void gatherStorageBuffer( LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, std::map< UINT, ID3D11UnorderedAccessView * > & uavs )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto view = getBufferUAV( write, i );
					tryGather( bindingIndex
						, flags
						, view
						, &uavs );
				}
			}
		}

		static void gatherDynamicStorageBuffer( LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, std::map< UINT, ID3D11UnorderedAccessView * > & uavs
			, UInt32Array const & offsets
			, size_t offsetIndex )
		{
			// v13.49: dynamic storage buffers: the shader register comes from the UAV index (the
			// caller passed the sbo map before, a different numbering when storage images exist),
			// and the view starts at descriptor offset + this bind's dynamic offset (was ignored).
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					size_t const dynIndex = offsetIndex + write.dstArrayElement + i;
					VkDeviceSize const dyn = dynIndex < offsets.size() ? offsets[dynIndex] : 0u;
					auto const & info = write.pBufferInfo[i];
					auto view = get( info.buffer )->getUnorderedAccessView( info.offset + dyn, info.range );
					tryGather( bindingIndex
						, flags
						, view
						, &uavs );
				}
			}
		}

		static void gatherDynamicBuffers( LayoutBindingWritesArray const & writes
			, ShaderBindings const & bindings
			, uint32_t setIndex
			, std::map< UINT, ID3D11UnorderedAccessView * > & uavs
			, UInt32Array const & offsets )
		{
			size_t offsetIndex{};

			for ( auto const * write : writes )
			{
				auto const & writeBinding = *write;
				assert( offsetIndex + writeBinding.binding.descriptorCount <= offsets.size() );

				switch ( writeBinding.binding.descriptorType )
				{
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
					// Uniform buffers do not contribute an output UAV.
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
					gatherDynamicStorageBuffer( writeBinding
						, bindings.uav
						, setIndex
						, uavs
						, offsets
						, offsetIndex );
					break;

				default:
					assert( false && "Unsupported dynamic descriptor type" );
					throw ashes::BaseException{ "Unsupported dynamic descriptor type" };
				}

				offsetIndex += writeBinding.binding.descriptorCount;
			}

			assert( offsetIndex == offsets.size() );
		}

		template< bool Supports11_1 >
		static void unbindCombinedSampler( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11ShaderResourceView * )nullptr
						, ( ID3D11SamplerState * )nullptr );
				}
			}
		}
		
		template< bool Supports11_1 >
		static void unbindInputAttachment( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11ShaderResourceView * )nullptr
						, ( ID3D11SamplerState * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindSampler( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11SamplerState * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindSampledTexture( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11ShaderResourceView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindUniformBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					auto range = std::min( write.pBufferInfo[i].range
						, get( write.pBufferInfo[i].buffer )->getSize() );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11Buffer * )nullptr
						, UINT( write.pBufferInfo[i].offset / 16 )
						, UINT( ashes::getAlignedSize( range, 256ULL ) / 16ULL ) );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindUniformTexelBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11ShaderResourceView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindDynamicUniformBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, UInt32Array const & offsets
			, size_t offsetIndex )
		{
			assert( offsetIndex + writeBinding.binding.descriptorCount <= offsets.size() );

			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					UINT firstConstant{};
					UINT constantCount{};
					getDynamicUniformBufferRange( write.pBufferInfo[i]
						, offsets[offsetIndex + write.dstArrayElement + i]
						, firstConstant
						, constantCount );
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11Buffer * )nullptr
						, firstConstant
						, constantCount );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindStorageImage( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11UnorderedAccessView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindStorageTexelBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBind< Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11ShaderResourceView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindStorageBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11UnorderedAccessView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindDynamicStorageBuffer( Context & context
			, LayoutBindingWrites const & writeBinding
			, ShaderBindingMap const & bindings
			, uint32_t setIndex
			, UInt32Array const & /*offsets*/
			, size_t /*offsetIndex*/ )
		{
			for ( auto & write : writeBinding.writes )
			{
				auto flags = writeBinding.binding.stageFlags;
				auto it = bindings.find( makeShaderBindingKey( setIndex, write.dstBinding ) );
				if ( it == bindings.end() ) continue;   // v13.47: binding not in this layout, skip (was UB)
				auto dstBinding = it->second;

				for ( auto i = 0u; i < write.descriptorCount; ++i )
				{
					uint32_t bindingIndex = dstBinding + write.dstArrayElement + i;
					tryBindOne< VK_SHADER_STAGE_COMPUTE_BIT, Supports11_1 >( context
						, bindingIndex
						, flags
						, ( ID3D11UnorderedAccessView * )nullptr );
				}
			}
		}

		template< bool Supports11_1 >
		static void unbindDynamicBuffers( Context & context
			, LayoutBindingWritesArray const & writes
			, ShaderBindings const & bindings
			, uint32_t setIndex
			, UInt32Array const & offsets )
		{
			size_t offsetIndex{};

			for ( auto const * write : writes )
			{
				auto const & writeBinding = *write;
				assert( offsetIndex + writeBinding.binding.descriptorCount <= offsets.size() );

				switch ( writeBinding.binding.descriptorType )
				{
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
					unbindDynamicUniformBuffer< Supports11_1 >( context, writeBinding, bindings.ubo, setIndex, offsets, offsetIndex );
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
					unbindDynamicStorageBuffer< Supports11_1 >( context, writeBinding, bindings.uav, setIndex, offsets, offsetIndex );   // v13.49: uav numbering = shader register
					break;

				default:
					assert( false && "Unsupported dynamic descriptor type" );
					throw ashes::BaseException{ "Unsupported dynamic descriptor type" };
				}

				offsetIndex += writeBinding.binding.descriptorCount;
			}

			assert( offsetIndex == offsets.size() );
		}

		template< bool Supports11_1 >
		static void bindAll( Context & context
			, VkPipelineLayout pipelineLayout
			, VkDescriptorSet descriptorSet
			, uint32_t setIndex
			, UInt32Array const & dynamicOffsets )
		{
			auto d3dDescriptorSet = get( descriptorSet );
			auto & bindings = get( pipelineLayout )->getShaderBindings();
			// v13.69: time-stamp counter instead of four QueryPerformanceCounter calls per bind
			uint64_t p0{}, p1{}, p2{}, p3{};
			p0 = __rdtsc();
			auto msBetween = []( uint64_t a, uint64_t b ) { return double( b - a ) * msPerTsc(); };

			for ( auto & write : d3dDescriptorSet->getInputAttachments() )
			{
				bindInputAttachment< Supports11_1 >( context, get( context.device )->getSampler(), *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getCombinedTextureSamplers() )
			{
				bindCombinedSampler< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getSamplers() )
			{
				bindSampler< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getSampledTextures() )
			{
				bindSampledTexture< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			p1 = __rdtsc();
			size_t ringCursor = 0u;   // v13.75
			for ( auto & write : d3dDescriptorSet->getUniformBuffers() )
			{
				bindUniformBuffer< Supports11_1 >( context, *write, bindings.ubo, setIndex, &ringCursor );
			}

			for ( auto & write : d3dDescriptorSet->getInlineUniforms() )
			{
				bindUniformBuffer< Supports11_1 >( context, *write, bindings.ubo, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getUniformTexelBuffers() )
			{
				bindUniformTexelBuffer< Supports11_1 >( context, *write, bindings.tbo, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageTexelBuffers() )
			{
				unbindStorageTexelBuffer< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageBuffers() )
			{
				bindStorageBuffer< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageTextures() )
			{
				bindStorageImage< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			p2 = __rdtsc();
			bindDynamicBuffers< Supports11_1 >( context, d3dDescriptorSet->getDynamicBuffers(), bindings, setIndex, dynamicOffsets );
			p3 = __rdtsc();
			context.bindMsTextures += msBetween( p0, p1 );
			context.bindMsBuffers += msBetween( p1, p2 );
			context.bindMsDynamic += msBetween( p2, p3 );
		}

		template< bool Supports11_1 >
		static void unbindAll( Context & context
			, VkPipelineLayout pipelineLayout
			, VkDescriptorSet descriptorSet
			, uint32_t setIndex
			, UInt32Array const & dynamicOffsets )
		{
			auto d3dDescriptorSet = get( descriptorSet );
			auto & bindings = get( pipelineLayout )->getShaderBindings();

			for ( auto & write : d3dDescriptorSet->getInputAttachments() )
			{
				unbindInputAttachment< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getCombinedTextureSamplers() )
			{
				unbindCombinedSampler< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getSamplers() )
			{
				unbindSampler< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getSampledTextures() )
			{
				unbindSampledTexture< Supports11_1 >( context, *write, bindings.tex, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getUniformBuffers() )
			{
				unbindUniformBuffer< Supports11_1 >( context, *write, bindings.ubo, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getInlineUniforms() )
			{
				unbindUniformBuffer< Supports11_1 >( context, *write, bindings.ubo, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getUniformTexelBuffers() )
			{
				unbindUniformTexelBuffer< Supports11_1 >( context, *write, bindings.tbo, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageTexelBuffers() )
			{
				unbindStorageTexelBuffer< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageBuffers() )
			{
				unbindStorageBuffer< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			for ( auto & write : d3dDescriptorSet->getStorageTextures() )
			{
				unbindStorageImage< Supports11_1 >( context, *write, bindings.uav, setIndex );
			}

			unbindDynamicBuffers< Supports11_1 >( context, d3dDescriptorSet->getDynamicBuffers(), bindings, setIndex, dynamicOffsets );
		}

		static std::map< UINT, ID3D11UnorderedAccessView * > gatherUavs( VkDescriptorSet descriptorSet
			, VkPipelineLayout pipelineLayout
			, uint32_t setIndex
			, UInt32Array const & dynamicOffsets )
		{
			auto d3dDescriptorSet = get( descriptorSet );
			auto & bindings = get( pipelineLayout )->getShaderBindings();
			std::map< UINT, ID3D11UnorderedAccessView * > result;

			for ( auto & write : d3dDescriptorSet->getStorageBuffers() )
			{
				gatherStorageBuffer( *write, bindings.uav, setIndex, result );
			}

			for ( auto & write : d3dDescriptorSet->getStorageTextures() )
			{
				gatherStorageImage( *write, bindings.uav, setIndex, result );
			}

			gatherDynamicBuffers( d3dDescriptorSet->getDynamicBuffers(), bindings, setIndex, result, dynamicOffsets );
			return result;
		}
	}

	namespace binddesc
	{
		// v13.73: content fast path. The v13.63 fast path (skip re-binding a set's textures,
		// samplers and fixed buffers when the same set is bound again) compared set handles,
		// but Waylith allocates a new VkDescriptorSet for every draw: map test 150140 had
		// fast=0 full=4.6 million, and every bind walked the whole set (~4.4-5.4 us, 400 binds
		// per frame, ~2 ms per frame). The set's content is captured here, at record time (the
		// same point gatherUavs reads the set): the exact list of descriptors it binds. A bind
		// whose list equals the previous bind of that set index (same layout, nothing changed
		// in between) takes the fast path even with a new handle. Exact comparison, no hash.
		// Only sets made of textures, samplers and uniform buffers qualify; storage images/
		// buffers, input attachments, inline uniforms and swizzle-emulated views keep the full
		// bind. RW_GPU_SET_CONTENT_FAST=0 = old handle-only fast path.
		static bool contentFastPath()
		{
			static bool const result = envflags::on( "RW_GPU_SET_CONTENT_FAST", true );
			return result;
		}

		static bool uboOffsetsVariable()
		{
			static bool const result = envflags::on( "RW_GPU_SET_CONTENT_UBO", true );
			return result;
		}

		static bool buildContent( VkDescriptorSet descriptorSet, std::vector< uint64_t > & out )
		{
			out.clear();
			if ( !contentFastPath() || descriptorSet == VK_NULL_HANDLE ) return false;
			auto set = get( descriptorSet );
			if ( !set->getInputAttachments().empty() || !set->getStorageTextures().empty()
				|| !set->getInlineUniforms().empty() || !set->getStorageBuffers().empty()
				|| !set->getStorageTexelBuffers().empty() )
				return false;
			auto u = []( auto handle ) { return uint64_t( reinterpret_cast< uintptr_t >( handle ) ); };
			uint64_t category = 0u;
			auto shape = [&out, &category]( LayoutBindingWrites const & lbw, VkWriteDescriptorSet const & write )
			{
				out.push_back( category << 56 | uint64_t( lbw.binding.binding ) << 32 | uint64_t( lbw.binding.stageFlags ) );
				out.push_back( uint64_t( write.dstBinding ) << 40 | uint64_t( write.dstArrayElement ) << 20 | uint64_t( write.descriptorCount ) );
			};
			auto images = [&]( LayoutBindingWritesArray const & list, bool withSampler, bool withView ) -> bool
			{
				++category;
				out.push_back( category << 56 | uint64_t( list.size() ) );
				for ( auto * lbw : list )
				{
					for ( auto & write : lbw->writes )
					{
						shape( *lbw, write );
						for ( uint32_t i = 0u; i < write.descriptorCount; ++i )
						{
							auto const & info = write.pImageInfo[i];
							if ( withView )
							{
								if ( info.imageView == VK_NULL_HANDLE || get( info.imageView )->hasSwizzle() ) return false;
								out.push_back( u( info.imageView ) );
							}
							if ( withSampler ) out.push_back( u( info.sampler ) );
						}
					}
				}
				return true;
			};
			if ( !images( set->getCombinedTextureSamplers(), true, true ) ) return false;
			if ( !images( set->getSamplers(), true, false ) ) return false;
			if ( !images( set->getSampledTextures(), false, true ) ) return false;
			++category;
			out.push_back( category << 56 | uint64_t( set->getUniformBuffers().size() ) );
			for ( auto * lbw : set->getUniformBuffers() )
			{
				for ( auto & write : lbw->writes )
				{
					shape( *lbw, write );
					for ( uint32_t i = 0u; i < write.descriptorCount; ++i )
					{
						out.push_back( u( write.pBufferInfo[i].buffer ) );
						// v13.75: offset left out (re-bound on the fast path); RW_GPU_SET_CONTENT_UBO=0 = keep it in the key
						if ( !uboOffsetsVariable() ) out.push_back( uint64_t( write.pBufferInfo[i].offset ) );
						out.push_back( uint64_t( write.pBufferInfo[i].range ) );
					}
				}
			}
			++category;
			out.push_back( category << 56 | uint64_t( set->getUniformTexelBuffers().size() ) );
			for ( auto * lbw : set->getUniformTexelBuffers() )
			{
				for ( auto & write : lbw->writes )
				{
					shape( *lbw, write );
					for ( uint32_t i = 0u; i < write.descriptorCount; ++i ) out.push_back( u( write.pTexelBufferView[i] ) );
				}
			}
			// dynamic buffers: re-bound on the fast path from this set, so only their shape counts
			++category;
			out.push_back( category << 56 | uint64_t( set->getDynamicBuffers().size() ) );
			for ( auto * lbw : set->getDynamicBuffers() )
			{
				if ( lbw->binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ) return false;
				for ( auto & write : lbw->writes ) shape( *lbw, write );
			}
			return true;
		}
	}

	BindDescriptorSetCommand::BindDescriptorSetCommand( VkDevice context
		, VkDescriptorSet descriptorSet
		, VkPipelineLayout layout
		, uint32_t setIndex
		, ArrayView< uint32_t const > const & dynamicOffsets
		, VkPipelineBindPoint bindingPoint )
		: CommandBase{ context }
		, m_descriptorSet{ descriptorSet }
		, m_layout{ layout }
		, m_setIndex{ setIndex }
		, m_bindingPoint{ bindingPoint }
		, m_dynamicOffsets{ dynamicOffsets.begin(), dynamicOffsets.end() }
		, m_uavs{ binddesc::gatherUavs( m_descriptorSet, m_layout, m_setIndex, m_dynamicOffsets ) }
	{
		for ( auto const & [id, uav] : m_uavs )
		{
			if ( uav ) { uav->AddRef(); m_keep.emplace_back( uav, []( ID3D11UnorderedAccessView * p ){ p->Release(); } ); }
		}
		assert( get( m_descriptorSet )->getDynamicOffsetCount() == m_dynamicOffsets.size()
			&& "Dynamic descriptor array elements and dynamic offsets sizes must match." );
		m_contentValid = binddesc::buildContent( m_descriptorSet, m_content );
		m_contentUboRebind = m_contentValid && binddesc::uboOffsetsVariable() && !get( m_descriptorSet )->getUniformBuffers().empty();   // v13.75
	}

	namespace binddesc
	{
		// v13.63: RW_GPU_SET_FASTPATH=0 always re-applies every descriptor of a set.
		static bool setFastPath()
		{
			static bool const result = envflags::on( "RW_GPU_SET_FASTPATH", true );
			return result;
		}
	}

	void BindDescriptorSetCommand::apply( Context & context )const
	{
		// v13.74: dynamic uniform windows come from the uniform ring when this command has some
		struct RingScope
		{
			Context & context;
			RingScope( Context & c, ubring::View const * view ) : context{ c } { context.ubRing = view; }
			~RingScope() { context.ubRing = nullptr; }
		} ringScope{ context, m_ring.get() };
		// v13.63: Waylith re-binds the same descriptor set with new dynamic offsets for most draws.
		// When the previous bind of this set index used the same pipeline layout and set, and no
		// command since then could have changed bindings (render pass, clear, copy, blit,
		// dispatch, output-merger rebind; see CommandBase::isStateOnly) and the state cache was
		// not invalidated, the textures, samplers and fixed buffers are still bound exactly as
		// that bind left them - only the dynamic uniform/storage buffers move.
		if ( context.lastSetBindLayout != m_layout )
		{
			for ( auto & entry : context.lastSetBinds ) entry = Context::LastSetBind{};
			context.lastSetBindLayout = m_layout;
		}
		uint64_t const generation = context.stateCache.resourceGeneration();
		bool fast = false;
		if ( m_setIndex < 8u && binddesc::setFastPath() )
		{
			auto const & last = context.lastSetBinds[m_setIndex];
			fast = last.layout == m_layout && last.set == m_descriptorSet
				&& last.epoch == context.sideEffectEpoch && last.generation == generation;
			if ( !fast && m_contentValid && last.contentValid && last.layout == m_layout
				&& last.epoch == context.sideEffectEpoch && last.generation == generation
				&& context.lastSetContent[m_setIndex] == m_content )
			{
				fast = true;
				if ( g_setContentFast.fetch_add( 1u, std::memory_order_relaxed ) == 0u )
					std::fprintf( stderr, "[RW-GPU-SET] descriptor-set content fast path in use (RW_GPU_SET_CONTENT_FAST=0 switches it off)\n" );
			}
		}
		if ( fast )
		{
			g_setFast.fetch_add( 1u, std::memory_order_relaxed );
			auto & bindings = get( m_layout )->getShaderBindings();
			// v13.75: the content key leaves out plain uniform-buffer offsets (Waylith writes a new set per
			// draw only because its per-draw uniforms sit at a new offset), so re-bind those buffers too.
			if ( m_contentUboRebind )
			{
				size_t ringCursor = 0u;
				for ( auto & write : get( m_descriptorSet )->getUniformBuffers() )
				{
					if ( context.featureLevel >= D3D_FEATURE_LEVEL_11_1 )
						binddesc::bindUniformBuffer< true >( context, *write, bindings.ubo, m_setIndex, &ringCursor );
					else
						binddesc::bindUniformBuffer< false >( context, *write, bindings.ubo, m_setIndex, &ringCursor );
				}
			}
			if ( context.featureLevel >= D3D_FEATURE_LEVEL_11_1 )
				binddesc::bindDynamicBuffers< true >( context, get( m_descriptorSet )->getDynamicBuffers(), bindings, m_setIndex, m_dynamicOffsets );
			else
				binddesc::bindDynamicBuffers< false >( context, get( m_descriptorSet )->getDynamicBuffers(), bindings, m_setIndex, m_dynamicOffsets );
		}
		else
		{
			g_setFull.fetch_add( 1u, std::memory_order_relaxed );
			if ( context.featureLevel >= D3D_FEATURE_LEVEL_11_1 )
			{
				binddesc::bindAll< true >( context, m_layout, m_descriptorSet, m_setIndex, m_dynamicOffsets );
			}
			else
			{
				binddesc::bindAll< false >( context, m_layout, m_descriptorSet, m_setIndex, m_dynamicOffsets );
			}
		}
		if ( m_setIndex < 8u )
		{
			// generation read after binding: binds through the cache never change it, but a
			// bind path that invalidates (e.g. SRV hazards) does, which correctly disables the
			// fast path for the next bind.
			context.lastSetBinds[m_setIndex] = Context::LastSetBind{ m_layout, m_descriptorSet, context.sideEffectEpoch, context.stateCache.resourceGeneration(), m_contentValid };
			if ( m_contentValid ) context.lastSetContent[m_setIndex].assign( m_content.begin(), m_content.end() );   // v13.73 (no allocation once sized)
		}

		if ( m_uavs.empty() ) return;   // v13.63: most sets carry no storage buffers
		uint64_t const u0 = __rdtsc();   // v13.69
		// v13.63: compare in place instead of copying the whole slot map on every bind.
		bool dirtyUavs = false;
		for ( auto const & [id, uav] : m_uavs )
		{
			auto it = context.uavs.find( id );
			if ( it == context.uavs.end() || it->second != uav ) { dirtyUavs = true; break; }
		}

		if ( dirtyUavs )
		{
			++context.sideEffectEpoch;   // v13.63
			for ( auto const & [id, uav] : m_uavs ) context.uavs[id] = uav;
			context.rawUavs.clear();
			// v13.15: keys are binding indices; slot = uavStart + key. Keep gaps.
			if ( !context.uavs.empty() )
			{
				context.rawUavs.resize( size_t( context.uavs.rbegin()->first ) + 1u, nullptr );
				for ( auto const & [id, uav] : context.uavs ) context.rawUavs[id] = uav;
			}

			if ( !context.rawUavs.empty() )
			{
				// v13.53: through uavalias::bindOutput (one buffer in two slots -> private copies).
				context.uavAliased = uavalias::bindOutput( get( context.device )->getDevice(), *context.context, context.uavStart, UINT( context.rawUavs.size() ), context.rawUavs.data() );
				context.uavAbsEnd = std::max( context.uavAbsEnd, UINT( context.uavStart + context.rawUavs.size() ) );
			}
		}
		context.bindMsUavs += double( __rdtsc() - u0 ) * msPerTsc();
	}

	uint64_t BindDescriptorSetCommand::removeKey()const
	{
		// unbindAll() binds null to the slots given by (layout, set index) and, per write of the
		// set, (category, dstBinding, dstArrayElement, descriptorCount, stage flags); the bound
		// resources and dynamic offsets do not change which slots are nulled.
		// v13.69: key on exactly that shape instead of the set handle. Waylith uses a new
		// VkDescriptorSet for almost every draw, so the handle made ~every bind keep its own
		// cleanup (hundreds of unbindAll calls after every command buffer); sets of the same
		// shape now share one. RW_GPU_CLEANUP_SHAPE_KEY=0 = old key.
		static bool const shapeKey = envflags::on( "RW_GPU_CLEANUP_SHAPE_KEY", true );
		auto key = removeKeyMix( removeKeyStart( 5u ), uint64_t( reinterpret_cast< uintptr_t >( m_layout ) ) );
		key = removeKeyMix( key, m_setIndex );
		if ( !shapeKey )
		{
			key = removeKeyMix( key, uint64_t( reinterpret_cast< uintptr_t >( m_descriptorSet ) ) );
			for ( auto offset : m_dynamicOffsets ) key = removeKeyMix( key, offset );
			return key;
		}
		auto set = get( m_descriptorSet );
		uint64_t category = 0u;
		auto mixList = [&key, &category]( LayoutBindingWritesArray const & list )
		{
			++category;
			key = removeKeyMix( key, category << 32 | uint64_t( list.size() ) );
			for ( auto & lbw : list )
			{
				key = removeKeyMix( key, uint64_t( lbw->binding.binding ) << 32 | uint64_t( lbw->binding.stageFlags ) );
				key = removeKeyMix( key, uint64_t( lbw->binding.descriptorType ) << 32 | uint64_t( lbw->binding.descriptorCount ) );
				for ( auto & write : lbw->writes )
				{
					key = removeKeyMix( key, uint64_t( write.dstBinding ) << 40 | uint64_t( write.dstArrayElement ) << 20 | uint64_t( write.descriptorCount ) );
				}
			}
		};
		mixList( set->getInputAttachments() );
		mixList( set->getCombinedTextureSamplers() );
		mixList( set->getSamplers() );
		mixList( set->getSampledTextures() );
		mixList( set->getUniformBuffers() );
		mixList( set->getInlineUniforms() );
		mixList( set->getUniformTexelBuffers() );
		mixList( set->getStorageTexelBuffers() );
		mixList( set->getStorageBuffers() );
		mixList( set->getStorageTextures() );
		mixList( set->getDynamicBuffers() );
		return key;
	}

	void BindDescriptorSetCommand::remove( Context const & context )const
	{
		if ( context.featureLevel >= D3D_FEATURE_LEVEL_11_1 )
		{
			binddesc::unbindAll< true >( const_cast< Context & >( context ), m_layout, m_descriptorSet, m_setIndex, m_dynamicOffsets );
		}
		else
		{
			binddesc::unbindAll< false >( const_cast< Context & >( context ), m_layout, m_descriptorSet, m_setIndex, m_dynamicOffsets );
		}
	}

	void BindDescriptorSetCommand::fillContext( Context & context )const
	{
	}

	CommandPtr BindDescriptorSetCommand::clone()const
	{
		return std::make_unique< BindDescriptorSetCommand >( *this );
	}

	//*************************************************************************

	bool ResolveOutputUavsCommand::enabled()
	{
		static bool const result = []()
			{
				char const * env = std::getenv( "RW_GPU_UAV_RESOLVE" );
				return !( env && env[0] == '0' );
			}();
		return result;
	}

	static bool sameSetLayoutDefinition( VkDescriptorSetLayout lhs, VkDescriptorSetLayout rhs )
	{
		if ( lhs == rhs ) return true;
		if ( !lhs || !rhs ) return false;
		auto a = std::vector< VkDescriptorSetLayoutBinding >( get( lhs )->begin(), get( lhs )->end() );
		auto b = std::vector< VkDescriptorSetLayoutBinding >( get( rhs )->begin(), get( rhs )->end() );
		if ( a.size() != b.size() ) return false;
		auto byBinding = []( VkDescriptorSetLayoutBinding const & x, VkDescriptorSetLayoutBinding const & y ){ return x.binding < y.binding; };
		std::sort( a.begin(), a.end(), byBinding );
		std::sort( b.begin(), b.end(), byBinding );
		for ( size_t i = 0; i < a.size(); ++i )
		{
			if ( a[i].binding != b[i].binding || a[i].descriptorType != b[i].descriptorType || a[i].descriptorCount != b[i].descriptorCount ) return false;
		}
		return true;
	}

	std::shared_ptr< ResolvedUavs const > ResolveOutputUavsCommand::resolve( VkPipelineLayout layout
		, std::vector< std::pair< VkDescriptorSet, UInt32Array > > const & sets )
	{
		static std::atomic< uint64_t > serial{ 0u };
		auto result = std::make_shared< ResolvedUavs >();
		result->serial = ++serial;
		if ( !layout ) return result;
		auto setCount = uint32_t( get( layout )->getDescriptorsLayouts().size() );

		for ( uint32_t setIndex = 0u; setIndex < sets.size() && setIndex < setCount; ++setIndex )
		{
			auto const & [set, offsets] = sets[setIndex];
			if ( !set ) continue;
			// Vulkan: a set stays usable when its layout is *identically defined* to the pipeline's
			// set layout (handles may differ: Waylith creates equal layouts separately).
			// v13.49: native drivers address descriptors by (set, binding), so a set whose layout
			// differs from the pipeline's still works there as long as the bindings the shader uses
			// exist. Skipping such sets (v13.47b) left instanced draws without instance data
			// (geyser steam / dust puffs invisible). Now: gather by (set, binding) through the
			// pipeline layout; bindings the pipeline does not know are ignored. Logged with both
			// binding lists the first times. RW_GPU_UAV_STRICT_SETS=1 = v13.47b behaviour.
			if ( !sameSetLayoutDefinition( get( set )->getLayout(), get( layout )->getDescriptorsLayouts()[setIndex] ) )
			{
				static bool const strict = []()
					{
						char const * env = std::getenv( "RW_GPU_UAV_STRICT_SETS" );
						return env && env[0] == '1';
					}();
				static std::atomic< uint32_t > warned{ 0u };
				if ( warned++ < 8u )
				{
					auto describe = []( VkDescriptorSetLayout l )
						{
							std::string text;
							char buf[48];
							for ( auto it = get( l )->begin(); it != get( l )->end(); ++it )
							{
								std::snprintf( buf, sizeof( buf ), " b%u:t%u x%u", unsigned( it->binding ), unsigned( it->descriptorType ), unsigned( it->descriptorCount ) );
								text += buf;
							}
							return text;
						};
					std::fprintf( stderr, "[RW-GPU-UAV-RESOLVE] set %u layout differs (%s): bound{%s } pipeline{%s }\n", unsigned( setIndex )
						, strict ? "skipped" : "used by set+binding"
						, describe( get( set )->getLayout() ).c_str(), describe( get( layout )->getDescriptorsLayouts()[setIndex] ).c_str() );
				}
				if ( strict ) continue;
			}
			if ( get( set )->getDynamicOffsetCount() != offsets.size() ) continue;
			for ( auto const & [id, uav] : binddesc::gatherUavs( set, layout, setIndex, offsets ) )
			{
				result->uavs[id] = uav;
				if ( uav ) { uav->AddRef(); result->keep.emplace_back( uav, []( ID3D11UnorderedAccessView * p ){ p->Release(); } ); }
			}
		}

		return result;
	}

	ResolveOutputUavsCommand::ResolveOutputUavsCommand( VkDevice device
		, std::shared_ptr< ResolvedUavs const > resolved
		, uint32_t declaredUavMask
		, VkPipeline pipeline )
		: CommandBase{ device }
		, m_resolved{ std::move( resolved ) }
		, m_declaredUavMask{ declaredUavMask }
		, m_pipeline{ pipeline }
	{
	}

	static void checkDeclaredUavs( Context const & context, uint32_t mask, VkPipeline pipeline, std::map< UINT, ID3D11UnorderedAccessView * > const & wanted )
	{
		// v13.49: a shader that declares uN while slot N is empty reads zeros (instance data
		// lost: the draw silently disappears). Generic detector, logs the first 24 cases.
		if ( !mask ) return;
		static std::atomic< uint32_t > logged{ 0u };
		if ( logged >= 24u ) return;   // v13.63: diagnostic only; nothing left to log
		// v13.73: the result only depends on (pipeline, resolved UAV list, uavStart); the same
		// combination repeats for hundreds of draws per frame. RW_GPU_UAV_CHECK_CACHE=0 = check every draw.
		static bool const cache = envflags::on( "RW_GPU_UAV_CHECK_CACHE", true );
		thread_local VkPipeline lastPipeline{};
		thread_local void const * lastWanted{};
		thread_local UINT lastStart{ ~0u };
		if ( cache )
		{
			if ( lastPipeline == pipeline && lastWanted == &wanted && lastStart == context.uavStart ) return;
			lastPipeline = pipeline; lastWanted = &wanted; lastStart = context.uavStart;
		}
		static std::atomic< uint64_t > total{ 0u };
		for ( uint32_t slot = 0u; slot < 32u; ++slot )
		{
			if ( !( mask & ( 1u << slot ) ) ) continue;
			if ( slot < context.uavStart ) continue;   // overlaps render targets: compiled for another pass layout
			auto it = wanted.find( slot - context.uavStart );
			if ( it != wanted.end() && it->second ) continue;
			auto n = ++total;
			if ( logged < 24u && ( n <= 8u || ( n & ( n - 1u ) ) == 0u ) )
			{
				++logged;
				std::fprintf( stderr, "[RW-GPU-UAV-MISSING] pipeline %p declares u%u but the slot is empty (%llu so far); bound:", (void *)pipeline, unsigned( slot ), (unsigned long long)n );
				for ( auto const & [id, uav] : wanted ) std::fprintf( stderr, " [%u]=%p", unsigned( id + context.uavStart ), (void *)uav );
				std::fprintf( stderr, "\n" );
			}
		}
	}

	void ResolveOutputUavsCommand::apply( Context & context )const
	{
		checkDeclaredUavs( context, m_declaredUavMask, m_pipeline, m_resolved->uavs );
		auto const & wanted = m_resolved->uavs;
		UINT wantedEnd = wanted.empty() ? 0u : UINT( wanted.rbegin()->first + 1u );
		// v13.53: with private copies bound (aliased buffer) the copy must be refreshed per draw.
		bool same = !context.uavAliased
			&& context.uavs == wanted
			&& context.rawUavs.size() == wantedEnd
			&& context.uavAbsEnd <= context.uavStart + wantedEnd;
		if ( same ) return;

		static std::atomic< uint32_t > logged{ 0u };
		if ( !context.uavs.empty() && logged < 12u )
		{
			bool mismatch = false;
			for ( auto const & [id, uav] : context.uavs )
			{
				auto it = wanted.find( id );
				if ( it == wanted.end() || it->second != uav ) { mismatch = true; break; }
			}
			if ( mismatch )
			{
				++logged;
				std::string text;
				char buf[96];
				for ( auto const & [id, uav] : context.uavs ) { std::snprintf( buf, sizeof( buf ), " had[%u]=%p", unsigned( id ), (void *)uav ); text += buf; }
				for ( auto const & [id, uav] : wanted ) { std::snprintf( buf, sizeof( buf ), " want[%u]=%p", unsigned( id ), (void *)uav ); text += buf; }
				std::fprintf( stderr, "[RW-GPU-UAV-RESOLVE] stale/misplaced UAV corrected:%s\n", text.c_str() );
			}
		}

		++context.sideEffectEpoch;   // v13.63: output-merger rebind can unbind aliasing SRVs
		for ( auto const & [id, uav] : context.uavs ) context.hazardTracker.erase( hazards::WriteKind::outputMergerUav, id );
		context.uavs = wanted;
		context.rawUavs.assign( wantedEnd, nullptr );
		for ( auto const & [id, uav] : wanted ) context.rawUavs[id] = uav;

		// Clear every slot a previous draw left bound above the new range as well.
		UINT count = wantedEnd;
		if ( context.uavAbsEnd > context.uavStart + count ) count = context.uavAbsEnd - context.uavStart;
		ID3D11UnorderedAccessView * raw[64]{};   // v13.63: stack array (was a heap vector per rebind)
		if ( count > 64u ) count = 64u;
		for ( size_t k = 0u; k < context.rawUavs.size() && k < count; ++k ) raw[k] = context.rawUavs[k];
		// v13.53: D3D11 rejects the whole call when two slots share a buffer (Waylith keeps
		// _InstanceTransforms and _Color of a batch in one storage ring) -> private copies.
		context.uavAliased = uavalias::bindOutput( get( context.device )->getDevice(), *context.context, context.uavStart, count, raw );
		context.uavAbsEnd = context.uavStart + wantedEnd;
		for ( auto const & [id, uav] : wanted ) context.hazardTracker.setOutputUav( id, uav );
		++context.uavResolves;
	}

	CommandPtr ResolveOutputUavsCommand::clone()const
	{
		return std::make_unique< ResolveOutputUavsCommand >( *this );
	}
}
