/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

// v13.66: where the bridge's execution time goes, for every frame (not only slow ones).
// The execute loop already measures each command with the time-stamp counter (v13.63); until now
// the per-type totals were only printed for slow/medium/sampled command buffers. They are now
// added to a small session table and printed every 30 s next to the FRAMES line:
//   [RW-GPU-CMDTIME] per frame (last window): DrawIndexed 2.10ms (560x, 3.8us) BindDescriptorSet ...
// One mutex lock per executed command buffer (~1-3 per frame), nothing per command.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <typeinfo>

namespace ashes::D3D11_NAMESPACE::cmdstats
{
	struct Entry
	{
		std::type_info const * type{};
		double ms{};
		uint64_t count{};
	};

	struct Table
	{
		std::mutex mutex;
		Entry entries[32]{};
		uint32_t used{};
	};

	inline Table & table()
	{
		static Table * const result = new Table{};
		return *result;
	}

	inline void add( std::type_info const * type, double ms, uint64_t count )
	{
		auto & t = table();
		uint32_t i = 0u;
		while ( i < t.used && t.entries[i].type != type ) ++i;
		if ( i == t.used )
		{
			if ( t.used >= 32u ) return;
			t.entries[t.used++] = Entry{ type, 0.0, 0u };
		}
		t.entries[i].ms += ms;
		t.entries[i].count += count;
	}

	// Called with the lock held by the caller pattern below.
	template< typename SlotT >
	inline void addSlots( SlotT const * slots, uint32_t slotCount )
	{
		std::lock_guard< std::mutex > lock{ table().mutex };
		for ( uint32_t i = 0u; i < slotCount; ++i ) add( slots[i].type, slots[i].ms, slots[i].count );
	}

	// Prints the window since the last call and resets it.
	inline void printAndReset( size_t frames )
	{
		if ( !frames ) return;
		Entry copy[32]{};
		uint32_t used = 0u;
		{
			auto & t = table();
			std::lock_guard< std::mutex > lock{ t.mutex };
			used = t.used;
			std::copy( t.entries, t.entries + used, copy );
			for ( uint32_t i = 0u; i < t.used; ++i ) { t.entries[i].ms = 0.0; t.entries[i].count = 0u; }
		}
		std::sort( copy, copy + used, []( Entry const & l, Entry const & r ) { return l.ms > r.ms; } );
		std::string text;
		double total = 0.0;
		for ( uint32_t i = 0u; i < used; ++i ) total += copy[i].ms;
		for ( uint32_t i = 0u; i < used && i < 8u; ++i )
		{
			if ( copy[i].count == 0u ) continue;
			char const * full = copy[i].type->name();
			char const * shortName = std::strrchr( full, ':' );
			shortName = shortName ? shortName + 1 : full;
			char name[64];
			std::snprintf( name, sizeof( name ), "%s", shortName );
			if ( auto suffix = std::strstr( name, "Command" ) ) *suffix = '\0';
			char part[160];
			std::snprintf( part, sizeof( part ), " %s %.2fms (%.0fx, %.1fus)", name
				, copy[i].ms / double( frames ), double( copy[i].count ) / double( frames )
				, copy[i].count ? copy[i].ms * 1000.0 / double( copy[i].count ) : 0.0 );
			text += part;
		}
		std::fprintf( stderr, "[RW-GPU-CMDTIME] per frame (last window, %zu frames): total %.2fms;%s\n", frames, total / double( frames ), text.c_str() );
	}
}
