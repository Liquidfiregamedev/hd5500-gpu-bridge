/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/Core/D3D11DeviceContextLock.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11HazardTracker.hpp"
#include "renderer/D3D11Renderer/Miscellaneous/D3D11StateCache.hpp"

#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	namespace ubring { struct View; }   // v13.74

	struct Context
		: public NonCopyable
	{
		Context( D3D_FEATURE_LEVEL pfeatureLevel
			, VkDevice pdevice )
			: Context{ pfeatureLevel, pdevice, getImmediateContext( pdevice ) }
		{
		}

		Context( D3D_FEATURE_LEVEL pfeatureLevel
			, VkDevice pdevice
			, DeviceContextLock pcontext )
			: device{ pdevice }
			, context{ std::move( pcontext ) }
			, featureLevel{ pfeatureLevel }
		{
			auto hr = context->QueryInterface( __uuidof( ID3D11DeviceContext1 )
				, reinterpret_cast< void ** >( &context1 ) );
			checkError( device, hr, "QueryInterface<ID3D11DeviceContext1>" );
		}

			~Context()noexcept
			{
				statecache::collect( stateCache );
				safeRelease( context1 );
			}

		VkDevice device{};
		DeviceContextLock context;
		ID3D11DeviceContext1 * context1{};
		D3D_FEATURE_LEVEL featureLevel{};
			std::map< UINT, ID3D11UnorderedAccessView * > uavs{};
			std::vector< ID3D11UnorderedAccessView * > rawUavs{};
			UINT uavStart{};
			UINT uavAbsEnd{};
			// v13.63: descriptor-set fast path (same layout + set as the previous bind of that set
			// index, nothing in between that could change bindings -> only the dynamic offsets
			// are re-applied).
			uint64_t sideEffectEpoch{ 1u };
			uint64_t srvHazardVersion{};
			ubring::View const * ubRing{};   // v13.74: uniform-ring windows of the BindDescriptorSetCommand being applied   // v13.72: hazard-tracker version the SRV cache entries were checked against
			// v13.66: index-buffer offset folding. The index buffer is bound at offset 0 and the
			// Vulkan offset (in indices) is added to firstIndex at draw time, so a new offset into
			// the same buffer needs no IASetIndexBuffer call (DXVK-style).
			UINT indexBase{};
			ID3D11Buffer * indexBuffer{};
			DXGI_FORMAT indexFormat{};
			// v13.74: index pool (Miscellaneous/D3D11IndexPool.hpp): vkCmdBindIndexBuffer only records
			// what is bound; the D3D11 binding is resolved at the next indexed draw (D3D11IndexBind.hpp).
			class ObjectMemory const * indexMemory{};
			ID3D11Buffer * indexObjectBuffer{};
			UINT indexVkOffset{};
			bool indexPending{};
			uint64_t indexResolvedVersion{};
			void * indexPool{};
			bool indexPoolChecked{};
			struct LastSetBind { VkPipelineLayout layout{}; VkDescriptorSet set{}; uint64_t epoch{}; uint64_t generation{}; bool contentValid{}; };
			LastSetBind lastSetBinds[8]{};
			std::vector< uint64_t > lastSetContent[8];   // v13.73: what the last bind of each set index bound (content fast path)
			VkPipelineLayout lastSetBindLayout{};        // v13.47: one past the highest OM UAV slot bound
			bool uavAliased{};      // v13.53: last OM UAV bind used private copies (re-copy every draw)
			uint64_t uavResolves{};  // v13.47
			statecache::Cache stateCache{};
			hazards::Tracker hazardTracker{};
			// v13.24: host->GPU refreshes already done in this command-buffer execution.
			// The host may not change memory the GPU is using during the execution, so a
			// second upload of the same (or a contained) range is redundant.
			struct UploadedRange { void const * memory; uint64_t begin; uint64_t end; UINT subresource; };
			std::vector< UploadedRange > uploaded{};
			uint64_t uploadsSkipped{};
			uint64_t directUploads{};   // v13.46: texture uploads done with one UpdateSubresource from the CPU shadow
			uint64_t uploadsDone{};
			uint64_t uploadBytes{};
			// v13.38 profiling: time inside descriptor binds, split by phase (ms).
			double bindMsTextures{}, bindMsBuffers{}, bindMsDynamic{}, bindMsUavs{};

	private:
		static DeviceContextLock getImmediateContext( VkDevice device );
	};

	// v13.51: small FNV-1a combiner for CommandBase::removeKey().
	inline uint64_t removeKeyMix( uint64_t hash, uint64_t value )noexcept
	{
		for ( int i = 0; i < 8; ++i )
		{
			hash ^= ( value >> ( i * 8 ) ) & 0xFFu;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	inline uint64_t removeKeyStart( uint32_t tag )noexcept
	{
		return removeKeyMix( 14695981039346656037ull, tag );
	}

	class CommandBase
	{
	public:
		CommandBase( CommandBase const & ) = default;
		CommandBase( CommandBase && )noexcept = default;
		CommandBase & operator=( CommandBase const & ) = default;
		CommandBase & operator=( CommandBase && )noexcept = default;

		explicit CommandBase( VkDevice device );
		virtual ~CommandBase()noexcept = default;

		virtual void apply( Context & context )const = 0;
		// v13.63: true for commands that only set pipeline state or draw/upload without changing
		// which resources are bound where (draws, binds, pushes, uploads, viewport/scissor).
		// Every other command (render passes, clears, copies, blits, dispatches, barriers) bumps
		// Context::sideEffectEpoch, which invalidates the descriptor-set fast path.
		virtual bool isStateOnly()const { return false; }
		virtual void remove( Context const & context )const
		{
		}

		// v13.51: identity of what remove() undoes. remove() only writes "nothing bound"
		// into D3D11 slots, so two commands with the same key undo exactly the same thing and
		// the second cleanup is redundant. 0 = no key (cleanup always kept).
		virtual uint64_t removeKey()const
		{
			return 0u;
		}

		virtual void fillContext( Context & context )const
		{
		}

		// v13.69: true when the destructor gives bridge objects back to the device; such commands
		// are not handed to the background reaper (D3D11CommandReaper.hpp).
		virtual bool destroyOnOwnerThread()const
		{
			return false;
		}

		virtual CommandPtr clone()const = 0;

		VkDevice getDevice()const noexcept
		{
			return m_device;
		}

	private:
		VkDevice m_device;
	};
}
