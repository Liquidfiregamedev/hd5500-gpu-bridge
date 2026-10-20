/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include <cstdlib>
#include <cstdio>
#include <map>

namespace ashes::D3D11_NAMESPACE::statecache
{
	enum class Stage : uint8_t
	{
		compute,
		vertex,
		geometry,
		hull,
		domain,
		pixel,
	};

	enum class Kind : uint8_t
	{
		sampler,
		srv,
		constantBuffer,
	};

	// v13.52: pipeline objects that BindPipelineCommand sets on every bind. They are only
	// written by BindPipelineCommand, by helper passes that save and restore them (swizzle,
	// rect clears, draw blits) or invalidate the cache, and by the post-submit cleanups (which
	// invalidate). Blend/depth/rasterizer states are NOT cached here: the dynamic-state
	// commands write them too.
	enum class Object : uint8_t
	{
		inputLayout,
		vertexShader,
		pixelShader,
		geometryShader,
		hullShader,
		domainShader,
		count,
	};

	struct Key
	{
		Stage stage{};
		Kind kind{};
		UINT slot{};

		bool operator<( Key const & rhs )const noexcept
		{
			return stage != rhs.stage ? stage < rhs.stage
				: kind != rhs.kind ? kind < rhs.kind
				: slot < rhs.slot;
		}
	};

	struct Value
	{
		void * pointer{};
		UINT firstConstant{};
		UINT numConstants{};
		bool extended{};

		bool operator==( Value const & rhs )const noexcept
		{
			return pointer == rhs.pointer
				&& firstConstant == rhs.firstConstant
				&& numConstants == rhs.numConstants
				&& extended == rhs.extended;
		}
	};

	struct Statistics
	{
		uint64_t hits{};
		uint64_t misses{};
		uint64_t invalidations{};
		uint64_t srvInvalidations{};
		uint64_t objectHits{};   // v13.52: shader/input-layout sets skipped
		uint64_t iaHits{};       // v13.52: topology/vertex-buffer sets skipped
	};

	inline bool getEnvFlag( char const * name, bool defaultValue )
	{
		if ( auto value = std::getenv( name ) )
		{
			return value[0] != '0';
		}
		return defaultValue;
	}

	// v13.39: on by default (RW_HD5500_STATE_CACHE=0 switches it off); cached, since this
	// is asked for every native state call.
	inline bool isEnabled()
	{
		static bool const enabled = getEnvFlag( "RW_HD5500_STATE_CACHE", true );
		return enabled;
	}

	inline bool diagnosticsEnabled()
	{
		return getEnvFlag( "RW_HD5500_D3D_DIAGNOSTICS", false );
	}

	// The cache owns no D3D objects; pointer identity is valid while the native context
	// retains its binding. Unknown slots are deliberately absent rather than represented
	// by a null pointer, because null can itself be a valid native binding.
	// v13.44: flat arrays with generation counters instead of a std::map (this is asked
	// ~20 million times per run), and per-kind invalidation: render-pass begin/end only
	// make SRVs stale (D3D11 unbinds SRVs that alias the new targets), not samplers or
	// constant buffers.
	class Cache
	{
	public:
		static constexpr UINT StageCount = 6u;
		static constexpr UINT KindCount = 3u;
		static constexpr UINT SlotCount = 128u;   // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT

		bool needsSet( Stage stage, Kind kind, UINT slot, void * pointer, UINT first = 0u, UINT count = 0u, bool extended = false )
		{
			if ( !isEnabled() || slot >= SlotCount )
			{
				++m_stats.misses;
				return true;
			}
			auto & entry = m_entries[UINT( stage )][UINT( kind )][slot];
			auto const generation = m_generation[UINT( kind )];
			Value value{ pointer, first, count, extended };
			if ( entry.generation == generation && entry.value == value )
			{
				++m_stats.hits;
				return false;
			}
			entry.value = value;
			entry.generation = generation;
			++m_stats.misses;
			return true;
		}

		// v13.52: true when `pointer` is not known to be bound in that pipeline slot already.
		bool needsObject( Object object, void * pointer )
		{
			if ( !isEnabled() )
			{
				++m_stats.misses;
				return true;
			}
			auto & entry = m_objects[UINT( object )];
			if ( entry.generation == m_objectGeneration && entry.pointer == pointer )
			{
				++m_stats.hits;
				++m_stats.objectHits;
				return false;
			}
			entry.pointer = pointer;
			entry.generation = m_objectGeneration;
			++m_stats.misses;
			return true;
		}

		// v13.52: input assembler. Every draw re-sent its primitive topology and vertex buffers
		// (IASetPrimitiveTopology + IASetVertexBuffers per draw, ~340 draws per RimWorld frame).
		// Compared exactly per slot (no hashing): skipped only when every slot already holds the
		// same buffer, stride and offset.
		bool needsTopology( UINT topology )
		{
			if ( !isEnabled() ) return true;
			if ( m_topologyGeneration == m_iaGeneration && m_topology == topology )
			{
				++m_stats.hits;
				++m_stats.iaHits;
				return false;
			}
			m_topology = topology;
			m_topologyGeneration = m_iaGeneration;
			++m_stats.misses;
			return true;
		}

		bool needsVertexBuffers( UINT start, UINT count, ID3D11Buffer * const * buffers, UINT const * strides, UINT const * offsets )
		{
			if ( !isEnabled() || start + count > VertexSlotCount ) return true;
			bool same = true;
			for ( UINT i = 0u; i < count && same; ++i )
			{
				auto const & slot = m_vertexSlots[start + i];
				same = slot.generation == m_iaGeneration && slot.buffer == buffers[i]
					&& slot.stride == strides[i] && slot.offset == offsets[i];
			}
			if ( same )
			{
				++m_stats.hits;
				++m_stats.iaHits;
				return false;
			}
			for ( UINT i = 0u; i < count; ++i )
			{
				m_vertexSlots[start + i] = VertexSlot{ buffers[i], strides[i], offsets[i], m_iaGeneration };
			}
			++m_stats.misses;
			return true;
		}

		// v13.58: index buffer. RimWorld binds an index buffer before nearly every draw
		// (~1,900 IASetIndexBuffer per 12,700-command buffer), mostly the same buffer again.
		bool needsIndexBuffer( ID3D11Buffer * buffer, UINT format, UINT offset )
		{
			if ( !isEnabled() ) return true;
			if ( m_indexGeneration == m_iaGeneration && m_indexBuffer == buffer && m_indexFormat == format && m_indexOffset == offset )
			{
				++m_stats.hits;
				++m_stats.iaHits;
				return false;
			}
			m_indexBuffer = buffer; m_indexFormat = format; m_indexOffset = offset;
			m_indexGeneration = m_iaGeneration;
			++m_stats.misses;
			return true;
		}

		// Something set vertex buffers or topology without going through the cache.
		void invalidateInputAssembly() noexcept
		{
			++m_iaGeneration;
		}

		void invalidate() noexcept
		{
			for ( auto & generation : m_generation ) ++generation;
			++m_objectGeneration;
			++m_iaGeneration;
			++m_stats.invalidations;
		}

		void invalidateSrvs() noexcept
		{
			++m_generation[UINT( Kind::srv )];
			++m_stats.srvInvalidations;
		}

		// v13.63: changes whenever any resource-binding kind is invalidated (full invalidate,
		// render-pass SRV invalidation). Used by the descriptor-set fast path.
		uint64_t resourceGeneration()const noexcept
		{
			return uint64_t( m_generation[0] ) + m_generation[1] + m_generation[2] + m_objectGeneration;
		}

		Statistics const & getStatistics()const noexcept
		{
			return m_stats;
		}

	private:
		struct Entry
		{
			Value value{};
			uint32_t generation{ 0u };   // valid when equal to m_generation[kind]
		};
		Entry m_entries[StageCount][KindCount][SlotCount]{};
		struct ObjectEntry
		{
			void * pointer{};
			uint32_t generation{ 0u };
		};
		ObjectEntry m_objects[UINT( Object::count )]{};
		uint32_t m_objectGeneration{ 1u };
		static constexpr UINT VertexSlotCount = 32u;   // D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
		struct VertexSlot
		{
			ID3D11Buffer * buffer{};
			UINT stride{};
			UINT offset{};
			uint32_t generation{ 0u };
		};
		VertexSlot m_vertexSlots[VertexSlotCount]{};
		ID3D11Buffer * m_indexBuffer{};
		UINT m_indexFormat{};
		UINT m_indexOffset{};
		uint32_t m_indexGeneration{ 0u };
		UINT m_topology{};
		uint32_t m_topologyGeneration{ 0u };
		uint32_t m_iaGeneration{ 1u };
		uint32_t m_generation[KindCount]{ 1u, 1u, 1u };
		Statistics m_stats;
	};

	// Aggregation is intentionally process-wide: CommandBuffer contexts may be immediate
	// or short-lived deferred contexts, while the counters describe every actual setter
	// decision made by this renderer DLL instance.
	struct Aggregate
	{
		uint64_t hits{};
		uint64_t misses{};
		uint64_t invalidations{};
		uint64_t srvInvalidations{};
		bool reported{};

		~Aggregate()
		{
			if ( diagnosticsEnabled() && !reported )
			{
				reported = true;
				std::fprintf( stderr, "[RW-GPU-STATECACHE] hits=%llu misses=%llu invalidations=%llu srvInvalidations=%llu\n"
					, static_cast< unsigned long long >( hits )
					, static_cast< unsigned long long >( misses )
					, static_cast< unsigned long long >( invalidations )
					, static_cast< unsigned long long >( srvInvalidations ) );
			}
		}
	};

	inline Aggregate & aggregate()
	{
		static Aggregate result;
		return result;
	}

	inline void collect( Cache const & cache ) noexcept
	{
		auto const & stats = cache.getStatistics();
		auto & all = aggregate();
		all.hits += stats.hits;
		all.misses += stats.misses;
		all.invalidations += stats.invalidations;
		all.srvInvalidations += stats.srvInvalidations;
	}
}
