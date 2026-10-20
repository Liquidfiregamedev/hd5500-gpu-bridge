/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

// v13.74: index pool ("bigger rebuild" 2).
//
// Every RimWorld map section has its own small index buffer, so almost every draw switched the
// D3D11 index buffer: ~495 IASetIndexBuffer calls per frame at ~1.8 us each on the HD 5500
// (run 150140; none of them repeated the bound buffer). The v13.66 offset folding cannot help
// when the buffer itself changes.
//
// The pool is ONE large DEFAULT index buffer. The first time an index buffer is drawn from, its
// contents are copied into a slot of the pool on the GPU (CopySubresourceRegion); afterwards the
// draw binds the pool - which is usually already bound - and reaches its slot through
// StartIndexLocation (firstIndex), so no IASetIndexBuffer call is needed. A slot is re-copied
// whenever the bridge wrote the original buffer since (ObjectMemory::contentVersion: uploads,
// maps, buffer copies, update/fill, query results, image->buffer copies); GPU commands run in
// order, so a re-copy never changes what an earlier draw reads. The resolution happens at draw
// time (not at vkCmdBindIndexBuffer), after any upload recorded for that draw.
// Only plain index buffers qualify (no UAV/stream-out binding, at most 1 MB). When the pool is
// full it starts over (all slots re-copied on demand); if that happens more than 3 times in 10 s
// the working set does not fit and the pool switches itself off for the session.
// RW_GPU_INDEX_POOL=0 = v13.73 behaviour. RW_GPU_INDEX_POOL_MB = pool size (default 16, 2-64).

#include "renderer/D3D11Renderer/Miscellaneous/D3D11EnvFlags.hpp"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ashes::D3D11_NAMESPACE
{
	namespace idxpool
	{
		inline std::atomic< uint64_t > g_hits{ 0u };      // draws served from the pool without a copy
		inline std::atomic< uint64_t > g_copies{ 0u };    // slot (re)fills
		inline std::atomic< uint64_t > g_resets{ 0u };    // pool restarts
		inline std::atomic< bool > g_disabled{ false };   // switched off after thrashing

		inline bool enabled()
		{
			static bool const result = envflags::on( "RW_GPU_INDEX_POOL", true );
			return result && !g_disabled.load( std::memory_order_relaxed );
		}

		class Pool
		{
		public:
			explicit Pool( ID3D11Device * device )
			{
				char text[8]{};
				UINT mb = 16u;
				if ( envflags::text( "RW_GPU_INDEX_POOL_MB", text, sizeof( text ) ) > 0u )
				{
					mb = UINT( std::strtoul( text, nullptr, 10 ) );
					if ( mb < 2u ) mb = 2u;
					if ( mb > 64u ) mb = 64u;
				}
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = mb << 20;
				char test[16]{};   // harness only: tiny pools force restarts
				if ( envflags::text( "RW_GPU_INDEX_POOL_TEST_BYTES", test, sizeof( test ) ) > 0u )
				{
					UINT const bytes = UINT( std::strtoul( test, nullptr, 10 ) ) / 256u * 256u;
					if ( bytes >= 256u ) desc.ByteWidth = bytes;
				}
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
				auto hr = device->CreateBuffer( &desc, nullptr, &m_buffer );
				m_size = SUCCEEDED( hr ) && m_buffer ? desc.ByteWidth : 0u;
				std::fprintf( stderr, "[RW-GPU-IDXPOOL] index pool %u bytes: %s\n", desc.ByteWidth, m_size ? "created" : "creation failed (index buffers bound one by one)" );
			}
			~Pool()noexcept
			{
				if ( m_buffer ) m_buffer->Release();
			}
			ID3D11Buffer * buffer()const { return m_buffer; }
			UINT size()const { return m_size; }

			// Returns true with the slot's byte offset when `resource` (the object's D3D11 buffer)
			// is served from the pool; copies it there first when needed.
			bool resolve( ID3D11DeviceContext * context, uint64_t serial, uint64_t version, ID3D11Resource * resource, UINT & byteOffset )
			{
				std::lock_guard< std::mutex > lock{ m_mutex };
				if ( !m_buffer || !resource || g_disabled.load( std::memory_order_relaxed ) ) return false;
				auto it = m_entries.find( serial );
				if ( it != m_entries.end() && it->second.resource == resource )
				{
					auto & e = it->second;
					if ( !e.eligible ) return false;
					if ( e.generation == m_generation )
					{
						if ( e.version != version )
						{
							copy( context, resource, e.offset, e.bytes );
							e.version = version;
						}
						else
						{
							g_hits.fetch_add( 1u, std::memory_order_relaxed );
						}
						byteOffset = e.offset;
						return true;
					}
				}
				// new (or pool restarted since): check once, then allocate a slot
				Entry e{};
				e.resource = resource;
				e.eligible = isEligible( resource, e.bytes );
				if ( !e.eligible )
				{
					m_entries[serial] = e;
					return false;
				}
				UINT const slotBytes = ( e.bytes + 255u ) / 256u * 256u;
				if ( slotBytes > m_size ) { e.eligible = false; m_entries[serial] = e; return false; }
				if ( m_pos + slotBytes > m_size )
				{
					restart();
					if ( g_disabled.load( std::memory_order_relaxed ) ) return false;
				}
				e.offset = m_pos;
				e.generation = m_generation;
				e.version = version;
				m_pos += slotBytes;
				copy( context, resource, e.offset, e.bytes );
				m_entries[serial] = e;
				byteOffset = e.offset;
				return true;
			}

		private:
			struct Entry
			{
				ID3D11Resource * resource{};
				uint64_t version{};
				uint64_t generation{};
				UINT offset{};
				UINT bytes{};
				bool eligible{};
			};

			static bool isEligible( ID3D11Resource * resource, UINT & bytes )
			{
				D3D11_RESOURCE_DIMENSION dimension{};
				resource->GetType( &dimension );
				if ( dimension != D3D11_RESOURCE_DIMENSION_BUFFER ) return false;
				D3D11_BUFFER_DESC desc{};
				static_cast< ID3D11Buffer * >( resource )->GetDesc( &desc );
				bytes = desc.ByteWidth;
				return ( desc.BindFlags & D3D11_BIND_INDEX_BUFFER ) != 0u
					&& ( desc.BindFlags & ( D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT ) ) == 0u
					&& desc.Usage != D3D11_USAGE_STAGING
					&& desc.ByteWidth > 0u && desc.ByteWidth <= ( 1u << 20 );
			}

			void copy( ID3D11DeviceContext * context, ID3D11Resource * resource, UINT offset, UINT bytes )
			{
				D3D11_BOX box{ 0u, 0u, 0u, bytes, 1u, 1u };
				context->CopySubresourceRegion( m_buffer, 0u, offset, 0u, 0u, resource, 0u, &box );
				if ( g_copies.fetch_add( 1u, std::memory_order_relaxed ) == 0u )
					std::fprintf( stderr, "[RW-GPU-IDXPOOL] first index buffer copied into the pool (%u bytes; RW_GPU_INDEX_POOL=0 switches the pool off)\n", bytes );
			}

			void restart()
			{
				++m_generation;
				m_pos = 0u;
				m_entries.clear();
				g_resets.fetch_add( 1u, std::memory_order_relaxed );
				ULONGLONG const now = GetTickCount64();
				m_resetTimes[m_resetIndex++ % 4u] = now;
				ULONGLONG const oldest = m_resetTimes[m_resetIndex % 4u];
				if ( oldest && now - oldest < 10000u )
				{
					g_disabled.store( true, std::memory_order_relaxed );
					std::fprintf( stderr, "[RW-GPU-IDXPOOL] the index buffers in use do not fit in the pool (4 restarts within 10 s): pool switched off for this session (try RW_GPU_INDEX_POOL_MB=32)\n" );
				}
			}

			ID3D11Buffer * m_buffer{};
			UINT m_size{};
			UINT m_pos{};
			uint64_t m_generation{ 1u };
			std::unordered_map< uint64_t, Entry > m_entries;
			ULONGLONG m_resetTimes[4]{};
			uint32_t m_resetIndex{};
			std::mutex m_mutex;
		};

		inline Pool * pool( ID3D11Device * device )
		{
			static std::mutex mutex;
			static auto * pools = new std::map< ID3D11Device *, std::unique_ptr< Pool > >{};
			std::lock_guard< std::mutex > lock{ mutex };
			auto & entry = ( *pools )[device];
			if ( !entry ) entry = std::make_unique< Pool >( device );
			return entry->size() ? entry.get() : nullptr;
		}
	}
}
