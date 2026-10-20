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
	};

	inline bool getEnvFlag( char const * name, bool defaultValue )
	{
		if ( auto value = std::getenv( name ) )
		{
			return value[0] != '0';
		}
		return defaultValue;
	}

	inline bool isEnabled()
	{
		return getEnvFlag( "RW_HD5500_STATE_CACHE", false );
	}

	inline bool diagnosticsEnabled()
	{
		return getEnvFlag( "RW_HD5500_D3D_DIAGNOSTICS", false );
	}

	// The cache owns no D3D objects; pointer identity is valid while the native context
	// retains its binding. Unknown slots are deliberately absent rather than represented
	// by a null pointer, because null can itself be a valid native binding.
	class Cache
	{
	public:
		bool needsSet( Stage stage, Kind kind, UINT slot, void * pointer, UINT first = 0u, UINT count = 0u, bool extended = false )
		{
			if ( !isEnabled() )
			{
				++m_stats.misses;
				return true;
			}

			Key key{ stage, kind, slot };
			Value value{ pointer, first, count, extended };
			auto it = m_values.find( key );
			if ( it != m_values.end() && it->second == value )
			{
				++m_stats.hits;
				return false;
			}
			m_values[key] = value;
			++m_stats.misses;
			return true;
		}

		void invalidate() noexcept
		{
			m_values.clear();
			++m_stats.invalidations;
		}

		Statistics const & getStatistics()const noexcept
		{
			return m_stats;
		}

	private:
		std::map< Key, Value > m_values;
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
		bool reported{};

		~Aggregate()
		{
			if ( diagnosticsEnabled() && !reported )
			{
				reported = true;
				std::fprintf( stderr, "[RW-GPU-STATECACHE] hits=%llu misses=%llu invalidations=%llu\n"
					, static_cast< unsigned long long >( hits )
					, static_cast< unsigned long long >( misses )
					, static_cast< unsigned long long >( invalidations ) );
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
	}
}
