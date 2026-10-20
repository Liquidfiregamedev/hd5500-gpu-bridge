/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

// v13.74: uniform ring ("bigger rebuild" 1).
//
// Waylith writes the per-draw uniform/material data of a frame into large host-visible uniform
// buffers and binds a different 256-byte-aligned window per draw (dynamic uniform buffers).
// Until v13.73 every window was copied from the CPU shadow into the buffer's own DEFAULT
// D3D11 buffer with UpdateSubresource1 (6-12 us each on the HD 5500, ~90 per frame) and then
// bound with *SetConstantBuffers1 at the window's offset.
//
// Now the windows of a command buffer are known at record time. When the command buffer is
// executed, all of them are copied from the CPU shadow into ONE DYNAMIC constant buffer (the
// ring) with ONE Map(WRITE_NO_OVERWRITE) - DISCARD only when the ring wraps - and every draw
// binds the ring at its window's offset. This is the pattern DXVK/NVIDIA/AMD recommend for
// per-draw constants ("one big buffer, NO_OVERWRITE appends, *SetConstantBuffers1 offsets");
// Dawn found that copying a ring back into a DEFAULT buffer stalls on Intel, so the ring is
// bound directly. NO_OVERWRITE never touches bytes a queued draw may still read (we only append
// after the last write since the last DISCARD), and DISCARD gives fresh memory, so no fences.
//
// Only windows that are safe qualify (decided at record time, CommandBuffer::bindDescriptorSets):
// dynamic UNIFORM buffers whose D3D11 buffer is a pure constant buffer, in host-visible memory
// with a CPU shadow, that the GPU never writes (never a copy/fill/update destination), with a
// window of at most 64 KB, on feature level 11_1 with NO_OVERWRITE support for dynamic constant
// buffers. Their upload commands are not recorded (that is the saving). If anything fails at
// execution (ring too small, Map failed, recording with transfer writes), each window falls back
// to the v13.73 path: upload the window, bind the original buffer.
// RW_GPU_UNIFORM_RING=0 = v13.73 behaviour. RW_GPU_UNIFORM_RING_MB = ring size (default 4, 1-32).

#include "renderer/D3D11Renderer/Miscellaneous/D3D11EnvFlags.hpp"

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	class ObjectMemory;
	class DeviceMemory;

	namespace ubring
	{
		inline std::atomic< uint64_t > g_windows{ 0u };     // windows bound from the ring
		inline std::atomic< uint64_t > g_fallbacks{ 0u };   // windows that used the old path
		inline std::atomic< uint64_t > g_maps{ 0u };        // Map calls on the ring
		inline std::atomic< uint64_t > g_discards{ 0u };    // ring wraps

		inline bool enabled()
		{
			static bool const result = envflags::on( "RW_GPU_UNIFORM_RING", true );
			return result;
		}

		struct Window
		{
			ObjectMemory const * memory{};
			DeviceMemory const * parent{};
			std::weak_ptr< uint8_t > lifetime;
			uint64_t objectOffset{};   // window start relative to the object (for the fallback upload)
			uint64_t shadowOffset{};   // window start in the parent's CPU shadow
			UINT copyBytes{};              // bytes to copy (window range, clipped to the buffer)
			UINT bindBytes{};              // bound bytes (range rounded up to 256)
			// written at execution:
			UINT firstConstant{};
			bool ok{};
		};

		// Windows of one BindDescriptorSetCommand, indexed by dynamic-offset element.
		struct View
		{
			std::vector< Window > windows;
			std::vector< int32_t > elementToWindow;   // dynamic uniform buffers, by dynamic-offset element; -1 = not from the ring
			std::vector< int32_t > staticToWindow;    // v13.75: plain uniform buffers, by element in getUniformBuffers() order; -1 = not from the ring
			bool forceFallback{};                     // set at end() for recordings with transfer writes
			ID3D11Buffer * ringBuffer{};              // written at execution
		};

		class Ring
		{
		public:
			explicit Ring( ID3D11Device * device )
			{
				char text[8]{};
				UINT mb = 4u;
				if ( envflags::text( "RW_GPU_UNIFORM_RING_MB", text, sizeof( text ) ) > 0u )
				{
					mb = UINT( std::strtoul( text, nullptr, 10 ) );
					if ( mb < 1u ) mb = 1u;
					if ( mb > 32u ) mb = 32u;
				}
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = mb << 20;
				// harness only: tiny rings force wraps (DISCARD) and overflow fallbacks
				char test[16]{};
				if ( envflags::text( "RW_GPU_UNIFORM_RING_TEST_BYTES", test, sizeof( test ) ) > 0u )
				{
					UINT const bytes = UINT( std::strtoul( test, nullptr, 10 ) ) / 256u * 256u;
					if ( bytes >= 256u ) desc.ByteWidth = bytes;
				}
				desc.Usage = D3D11_USAGE_DYNAMIC;
				desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				auto hr = device->CreateBuffer( &desc, nullptr, &m_buffer );
				m_size = SUCCEEDED( hr ) && m_buffer ? desc.ByteWidth : 0u;
				std::fprintf( stderr, "[RW-GPU-UBRING] uniform ring %u bytes: %s\n", desc.ByteWidth, m_size ? "created" : "creation failed (old upload path stays in use)" );
			}
			~Ring()noexcept
			{
				if ( m_buffer ) m_buffer->Release();
			}
			ID3D11Buffer * buffer()const { return m_buffer; }
			UINT size()const { return m_size; }

			// Reserves `bytes` (256-aligned) for one batch; maps the ring and returns the write
			// pointer at the batch start, or nullptr (then nothing is mapped).
			uint8_t * begin( ID3D11DeviceContext * context, UINT bytes, UINT & batchStart )
			{
				if ( !m_buffer || bytes == 0u || bytes > m_size ) return nullptr;
				D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
				if ( m_first || m_pos + bytes > m_size )
				{
					mode = D3D11_MAP_WRITE_DISCARD;
					m_pos = 0u;
					m_first = false;
					g_discards.fetch_add( 1u, std::memory_order_relaxed );
				}
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if ( FAILED( context->Map( m_buffer, 0u, mode, 0u, &mapped ) ) || !mapped.pData ) return nullptr;
				g_maps.fetch_add( 1u, std::memory_order_relaxed );
				batchStart = m_pos;
				m_pos += bytes;
				return static_cast< uint8_t * >( mapped.pData ) + batchStart;
			}
			void end( ID3D11DeviceContext * context )
			{
				context->Unmap( m_buffer, 0u );
			}
			std::mutex & mutex() { return m_mutex; }

		private:
			ID3D11Buffer * m_buffer{};
			UINT m_size{};
			UINT m_pos{};
			bool m_first{ true };
			std::mutex m_mutex;
		};

		inline Ring * ring( ID3D11Device * device )
		{
			static std::mutex mutex;
			static auto * rings = new std::map< ID3D11Device *, std::unique_ptr< Ring > >{};
			std::lock_guard< std::mutex > lock{ mutex };
			auto & entry = ( *rings )[device];
			if ( !entry ) entry = std::make_unique< Ring >( device );
			return entry->size() ? entry.get() : nullptr;
		}
	}
}
