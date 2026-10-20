/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include "Command/D3D11CommandBuffer.hpp"
#include "Command/D3D11CommandStats.hpp"
#include "Command/D3D11CommandReaper.hpp"
#include "Miscellaneous/D3D11WatchedBytes.hpp"
#include "Miscellaneous/D3D11EnvFlags.hpp"
#include <typeinfo>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include "Command/Commands/D3D11DrawBlitCommand.hpp"

#include "Buffer/D3D11Buffer.hpp"
#include "Command/D3D11CommandPool.hpp"
#include "Core/D3D11Device.hpp"
#include "Descriptor/D3D11DescriptorSet.hpp"
#include "Image/D3D11Image.hpp"
#include "Image/D3D11ImageView.hpp"
#include "Miscellaneous/D3D11QueryPool.hpp"
#include "Pipeline/D3D11Pipeline.hpp"
#include "Pipeline/D3D11PipelineLayout.hpp"
#include "RenderPass/D3D11FrameBuffer.hpp"
#include "RenderPass/D3D11RenderPass.hpp"

#include "Command/Commands/D3D11BeginQueryCommand.hpp"
#include "Command/Commands/D3D11BeginRenderPassCommand.hpp"
#include "Command/Commands/D3D11BeginSubpassCommand.hpp"
#include "Command/Commands/D3D11BindComputePipelineCommand.hpp"
#include "Command/Commands/D3D11BindDescriptorSetCommand.hpp"
#include "Command/Commands/D3D11BindIndexBufferCommand.hpp"
#include "Command/Commands/D3D11BindVertexBuffersCommand.hpp"
#include "Command/Commands/D3D11BindPipelineCommand.hpp"
#include "Command/Commands/D3D11ClearAttachmentsCommand.hpp"
#include "Command/Commands/D3D11ClearColourCommand.hpp"
#include "Command/Commands/D3D11ClearDepthStencilCommand.hpp"
#include "Command/Commands/D3D11CopyBufferCommand.hpp"
#include "Command/Commands/D3D11CopyBufferToImageCommand.hpp"
#include "Command/Commands/D3D11CopyImageCommand.hpp"
#include "Command/Commands/D3D11CopyImageToBufferCommand.hpp"
#include "Command/Commands/D3D11CopyQueryPoolResultsCommand.hpp"
#include "Command/Commands/D3D11DispatchCommand.hpp"
#include "Command/Commands/D3D11DispatchIndirectCommand.hpp"
#include "Command/Commands/D3D11DownloadMemoryCommand.hpp"
#include "Command/Commands/D3D11DrawCommand.hpp"
#include "Command/Commands/D3D11DrawIndexedCommand.hpp"
#include "Command/Commands/D3D11DrawIndexedIndirectCommand.hpp"
#include "Command/Commands/D3D11DrawIndirectCommand.hpp"
#include "Command/Commands/D3D11EndQueryCommand.hpp"
#include "Command/Commands/D3D11EndRenderPassCommand.hpp"
#include "Command/Commands/D3D11EndSubpassCommand.hpp"
#include "Command/Commands/D3D11ExecuteActionsCommand.hpp"
#include "Command/Commands/D3D11ExecuteCommandsCommand.hpp"
#include "Command/Commands/D3D11FillBufferCommand.hpp"
#include "Command/Commands/D3D11GenerateMipsCommand.hpp"
#include "Command/Commands/D3D11MemoryBarrierCommand.hpp"
#include "Command/Commands/D3D11PushConstantsCommand.hpp"
#include "Command/Commands/D3D11ResetEventCommand.hpp"
#include "Command/Commands/D3D11ResetQueryPoolCommand.hpp"
#include "Command/Commands/D3D11ResolveImageCommand.hpp"
#include "Command/Commands/D3D11ScissorCommand.hpp"
#include "Command/Commands/D3D11SetDepthBiasCommand.hpp"
#include "Command/Commands/D3D11SetEventCommand.hpp"
#include "Command/Commands/D3D11SetLineWidthCommand.hpp"
#include "Command/Commands/D3D11UpdateBufferCommand.hpp"
#include "Command/Commands/D3D11UploadMemoryCommand.hpp"
#include "Miscellaneous/D3D11UniformRing.hpp"
#include "Command/Commands/D3D11ViewportCommand.hpp"
#include "Command/Commands/D3D11WaitEventsCommand.hpp"
#include "Command/Commands/D3D11WriteTimestampCommand.hpp"

#include "ashesd3d11_api.hpp"
#include "Miscellaneous/D3D11CommandTrace.hpp"
#include "Miscellaneous/D3D11FrameDiag.hpp"
#include <typeinfo>

namespace ashes::D3D11_NAMESPACE
{
	namespace cpuwatch
	{
		// v13.48: machine-wide CPU busy share and current clock while a command buffer runs,
		// to tell "the bridge does more work" from "the CPU got slower/contended".
		struct PROCESSOR_POWER_INFORMATION_ { ULONG Number; ULONG MaxMhz; ULONG CurrentMhz; ULONG MhzLimit; ULONG MaxIdleState; ULONG CurrentIdleState; };
		inline void currentMhz( ULONG & current, ULONG & maxMhz, ULONG & limit )
		{
			using Fn = LONG ( WINAPI * )( int, PVOID, ULONG, PVOID, ULONG );
			static Fn fn = []() -> Fn
				{
					auto module = LoadLibraryA( "powrprof.dll" );
					return module ? reinterpret_cast< Fn >( GetProcAddress( module, "CallNtPowerInformation" ) ) : nullptr;
				}();
			current = maxMhz = limit = 0u;
			if ( !fn ) return;
			PROCESSOR_POWER_INFORMATION_ info[64]{};
			SYSTEM_INFO si{};
			GetSystemInfo( &si );
			ULONG count = std::min< ULONG >( si.dwNumberOfProcessors, 64u );
			if ( fn( 11 /*ProcessorInformation*/, nullptr, 0u, info, ULONG( sizeof( info[0] ) * count ) ) != 0 ) return;
			auto cpu = GetCurrentProcessorNumber();
			if ( cpu >= count ) cpu = 0u;
			current = info[cpu].CurrentMhz;
			maxMhz = info[cpu].MaxMhz;
			limit = info[cpu].MhzLimit;
		}
		inline uint64_t ft( FILETIME const & f ) { return ( uint64_t( f.dwHighDateTime ) << 32 ) | f.dwLowDateTime; }
		// Fixed CPU work (pure integer arithmetic, no memory traffic) timed in microseconds.
		// ~250 us on an idle 2.2 GHz core; much more = this thread runs slower right now
		// (lower clock, or sharing its core with another busy thread).
		inline double probeUs()
		{
			LARGE_INTEGER f{}, a{}, b{};
			QueryPerformanceFrequency( &f );
			QueryPerformanceCounter( &a );
			volatile uint64_t sink = 0u;
			uint64_t x = 0x9E3779B97F4A7C15ull;
			for ( uint32_t i = 0u; i < 400000u; ++i ) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
			sink = x;
			(void)sink;
			QueryPerformanceCounter( &b );
			return f.QuadPart ? double( b.QuadPart - a.QuadPart ) * 1e6 / double( f.QuadPart ) : 0.0;
		}
	}
}

namespace ashes::D3D11_NAMESPACE
{
	static bool constexpr UseCommandsList = false;

	//*********************************************************************************************

	namespace cmdbuf
	{
		static uint64_t makeHash( VkFormat src
			, VkFormat dst )
		{
			return ( uint64_t( src ) << 32 )
				| ( uint64_t( dst ) << 0 );
		}
	}

	//*********************************************************************************************

	CommandBuffer::CommandBuffer( VkDevice device
		, VkCommandPool commandPool
		, bool primary )
		: m_device{ device }
		, m_commandPool{ commandPool }
	{
		get( commandPool )->registerCommands( get( this ) );

		if constexpr ( UseCommandsList )
		{
			if ( !primary )
			{
				get( m_device )->getDevice()->CreateDeferredContext( 0u, &m_deferredContext );
			}
		}
	}

	CommandBuffer::~CommandBuffer()noexcept
	{
		// Disconnect memory callbacks before releasing commands and their owned blit temporaries.
		m_mappedResources.clear();
		m_afterSubmitActions.clear();
		m_afterSubmitKeys.clear();
		m_afterSubmitRecordingOrder = true;
		m_commands.clear();
		if constexpr ( UseCommandsList )
		{
			safeRelease( m_commandList );
			safeRelease( m_deferredContext );
		}
	}

	void CommandBuffer::execute( Context & context )const
	{
		if ( m_recordingResult != VK_SUCCESS ) throw Exception{ m_recordingResult, "Cannot execute failed command recording" };
		get( m_device )->requireHealthy( "CommandBuffer execute" );
		// Contexts can be immediate or deferred/reused. Native setters outside the
		// descriptor binder are deliberately not trusted across an execution boundary.
		context.stateCache.invalidate();
		context.hazardTracker.reset();
		context.uploaded.clear();
		context.uploadsSkipped = context.uploadsDone = context.uploadBytes = 0u;
		context.directUploads = 0u;
		context.bindMsTextures = context.bindMsBuffers = context.bindMsDynamic = context.bindMsUavs = 0.0;
		++context.sideEffectEpoch;   // v13.63: no descriptor-set fast path across executions
		context.lastSetBindLayout = VK_NULL_HANDLE;
		context.indexMemory = nullptr;   // v13.74
		context.indexPending = false;
		// v13.69: the per-command fillContext() pass was removed (the only override was empty).
		if ( !m_ringViews.empty() ) doFillUniformRing( context );   // v13.74

			cmdtrace::Trace trace{ get( m_device )->getDevice(), *context.context };
			// v13.22 [RW-GPU-PROF]: time every command; when one command buffer takes long
			// enough to be felt as a hitch, report which command types used the time.
			LARGE_INTEGER freq{}, tStart{}, tB{};
			QueryPerformanceFrequency( &freq );
			QueryPerformanceCounter( &tStart );
			// v13.48: thread CPU time of this execution (user+kernel). Wall time much larger than
			// CPU time = the render thread was waiting (driver sync, preempted by game threads).
			FILETIME ctA{}, etA{}, ktA{}, utA{};
			GetThreadTimes( GetCurrentThread(), &ctA, &etA, &ktA, &utA );
			FILETIME sysIdleA{}, sysKernelA{}, sysUserA{};
			GetSystemTimes( &sysIdleA, &sysKernelA, &sysUserA );
			// v13.57: per-command bookkeeping made cheaper (a command buffer has 2,500-9,500 commands):
			//  * the command type is keyed by its type_info address; the readable name is only
			//    fetched when a report is printed (or when the command trace is on);
			//  * the device-removed check (GetDeviceRemovedReason, a call into the D3D11 runtime)
			//    ran twice per command; now once every 64 commands and after the last one.
			//    A removed device is still caught within the same command buffer.
			//    RW_GPU_HEALTH_EVERY_COMMAND=1 restores the old check around every command.
			// v13.63: per-command time from the CPU's time-stamp counter (one __rdtsc per command
			// boundary, ~10 ns) instead of two QueryPerformanceCounter calls per command (~50 ns
			// together; 0.5+ ms per 12,000-command buffer). Converted to ms once per buffer with
			// the QPC-measured total (Broadwell has an invariant TSC).
			struct Slot { std::type_info const * type; double ms; uint32_t count; uint64_t tsc; };
			Slot slots[16]{};
			uint32_t slotCount = 0u;
			static bool const healthEveryCommand = envflags::readOne( "RW_GPU_HEALTH_EVERY_COMMAND" );
			bool const tracing = cmdtrace::enabled();
			uint32_t sinceCheck = 0u;
			uint64_t const tscStart = __rdtsc();
			uint64_t tscPrev = tscStart;
			for ( auto const & command : m_commands )
			{
				auto const & type = typeid( *command );
				char const * name = tracing ? type.name() : nullptr;
				auto const traceId = tracing ? trace.before( name ) : 0;
				if ( healthEveryCommand || ++sinceCheck >= 64u )
				{
					sinceCheck = 0u;
					get( m_device )->requireHealthy( "Before command apply" );
				}
				if ( !command->isStateOnly() ) ++context.sideEffectEpoch;   // v13.63
				command->apply( context );
				uint64_t const tscNow = __rdtsc();
				if ( tracing ) trace.after( traceId, name );
				if ( healthEveryCommand ) get( m_device )->requireHealthy( "After command apply" );
				uint32_t i = 0u;
				while ( i < slotCount && slots[i].type != &type ) ++i;
				if ( i == slotCount && slotCount < 16u ) slots[slotCount++] = Slot{ &type, 0.0, 0u, 0u };
				if ( i < slotCount ) { slots[i].tsc += tscNow - tscPrev; ++slots[i].count; }
				tscPrev = tscNow;
			}
			QueryPerformanceCounter( &tB );
			{
				double const totalMs = freq.QuadPart ? double( tB.QuadPart - tStart.QuadPart ) * 1000.0 / double( freq.QuadPart ) : 0.0;
				uint64_t const tscTotal = tscPrev - tscStart;
				double const msPerTsc = tscTotal ? totalMs / double( tscTotal ) : 0.0;
				for ( uint32_t i = 0u; i < slotCount; ++i ) slots[i].ms = double( slots[i].tsc ) * msPerTsc;
				cmdstats::addSlots( slots, slotCount );   // v13.66: per-frame table
			}
			get( m_device )->requireHealthy( "After command buffer" );
			{
				double const total = freq.QuadPart ? double( tB.QuadPart - tStart.QuadPart ) * 1000.0 / double( freq.QuadPart ) : 0.0;
				g_bridgeExecuteUs += uint64_t( total * 1000.0 );   // v13.48: frame pacing summary
				FILETIME ctB{}, etB{}, ktB{}, utB{};
				GetThreadTimes( GetCurrentThread(), &ctB, &etB, &ktB, &utB );
				auto ft = []( FILETIME const & f ){ return ( uint64_t( f.dwHighDateTime ) << 32 ) | f.dwLowDateTime; };
				double const cpuMs = double( ( ft( ktB ) - ft( ktA ) ) + ( ft( utB ) - ft( utA ) ) ) / 10000.0;
				double const kernelMs = double( ft( ktB ) - ft( ktA ) ) / 10000.0;
				FILETIME sysIdleB{}, sysKernelB{}, sysUserB{};
				GetSystemTimes( &sysIdleB, &sysKernelB, &sysUserB );
				uint64_t const sysTotal = ( cpuwatch::ft( sysKernelB ) - cpuwatch::ft( sysKernelA ) ) + ( cpuwatch::ft( sysUserB ) - cpuwatch::ft( sysUserA ) );   // kernel includes idle
				uint64_t const sysIdle = cpuwatch::ft( sysIdleB ) - cpuwatch::ft( sysIdleA );
				double const sysBusy = sysTotal ? 100.0 * double( sysTotal - std::min( sysIdle, sysTotal ) ) / double( sysTotal ) : 0.0;
				g_bridgeExecuteCpuUs += uint64_t( cpuMs * 1000.0 );
				++g_bridgeExecutions;
				if ( total > 100.0 ) ++g_bridgeSlowExecutions;
				static std::atomic< uint32_t > slowReports{ 0u };
				// v13.48: also a sample of ordinary command buffers (every 500th with 1000+ commands),
				// so slow ones can be compared with normal ones of the same content.
				static std::atomic< uint32_t > sampleCounter{ 0u };
				bool const sample = total <= 150.0 && m_commands.size() >= 1000u && ( ++sampleCounter % 500u ) == 0u && sampleCounter < 20000u;
				// v13.57: "medium" = 30-150 ms. Camera moves and zooms gave frames with 40-50 ms of
				// bridge time that never reached the 150 ms report threshold, so their cause was
				// unknown. At most one such report per 2 seconds, 300 per session.
				static std::atomic< uint32_t > mediumReports{ 0u };
				static LONGLONG lastMedium = 0;
				bool medium = false;
				if ( !sample && total > 30.0 && total <= 150.0 && mediumReports < 300u
					&& ( lastMedium == 0 || tB.QuadPart - lastMedium > 2 * freq.QuadPart ) )
				{
					medium = true;
					lastMedium = tB.QuadPart;
					++mediumReports;
				}
				if ( ( sample || medium || total > 150.0 ) && !m_commands.empty() && ( sample || medium || slowReports++ < 200u ) )   // v13.48: 40 -> 200 (long play sessions)
				{
					std::sort( slots, slots + slotCount, []( Slot const & l, Slot const & r ){ return l.ms > r.ms; } );
					ULONG mhzNow{}, mhzMax{}, mhzLimit{};
					cpuwatch::currentMhz( mhzNow, mhzMax, mhzLimit );
					double const probe = cpuwatch::probeUs();
					std::string top;
					for ( uint32_t i = 0u; i < slotCount && i < 4u; ++i )
					{
						char part[160];
						char const * fullName = slots[i].type->name();
						char const * shortName = std::strrchr( fullName, ':' );
						std::snprintf( part, sizeof( part ), "%s%s x%u=%.0fms", i ? ", " : "", shortName ? shortName + 1 : fullName, slots[i].count, slots[i].ms );
						top += part;
					}
					std::fprintf( stderr, "[RW-GPU-PROF] %s command buffer %.0f ms (thread CPU %.0f ms, kernel %.0f ms; machine busy %.0f%%, speed probe %.0f us), %zu commands: %s; uploads done=%llu skipped=%llu bytes=%.1fMB directTex=%llu; binds tex=%.0fms buf=%.0fms dyn=%.0fms uav=%.0fms\n", sample ? "sample" : ( medium ? "medium" : "slow" ), total, cpuMs, kernelMs, sysBusy, probe, m_commands.size(), top.c_str()
						, static_cast< unsigned long long >( context.uploadsDone ), static_cast< unsigned long long >( context.uploadsSkipped ), double( context.uploadBytes ) / 1048576.0, static_cast< unsigned long long >( context.directUploads )
						, context.bindMsTextures, context.bindMsBuffers, context.bindMsDynamic, context.bindMsUavs );
				}
			}

			get( m_device )->requireHealthy( "Command cleanup actions" );   // v13.69: once, not per action
			for ( auto const & action : m_afterSubmitActions )
		{
			action( context );
		}
	}

	VkResult CommandBuffer::begin( VkCommandBufferBeginInfo info )const
	{
		m_recordingResult = get( m_device )->getStatus();
		m_mappedResources.clear();
		m_afterSubmitActions.clear();
		m_afterSubmitKeys.clear();
		m_afterSubmitRecordingOrder = true;
		reaper::retire( m_commands );   // v13.69: freed on a background thread
		m_recordedUploads.clear();   // v13.60
		m_uploadSpans.clear();   // v13.73
		m_hasBufferWrites = false;
		m_ringViews.clear();   // v13.74
		for ( auto & recent : m_recentUploads ) recent = RecordedUpload{};   // v13.72
		m_recentUploadsNext = 0u;
		m_state = State{};
		m_state.beginInfo = std::move( info );
		QueryPerformanceCounter( &m_recordStart );   // v13.62
		m_recordTscStart = __rdtsc();   // v13.64
		m_recordTscBridge = 0u;
		m_recordCalls = 0u;
		return m_recordingResult;
	}

	// v13.73: upload merging. Waylith writes the per-draw uniform/material data of a frame into
	// a few large host-visible buffers and binds a different small window (a few hundred bytes)
	// per draw; every window became its own upload at execution (~90 per frame, 6-12 us each,
	// mostly the driver's UpdateSubresource1 cost: ~0.6-1.1 ms per frame). All host writes are
	// finished before the command buffer is submitted, so the first upload of a buffer in a
	// recording may already carry the later windows: the windows of one buffer that lie close
	// together (gaps up to 4 KB, cluster up to 256 KB) are merged into the earliest upload of the
	// cluster, and the execution-time dedupe (v13.24) then skips the later, contained ones.
	// Only for buffers the GPU cannot write (no UAV/stream-out/render-target binding) and only
	// in recordings without transfer writes (copy/fill/update buffer, query results, secondary
	// command buffers); otherwise nothing changes. RW_GPU_UPLOAD_MERGE=0 = one upload per window.
	bool CommandBuffer::uploadMerge()
	{
		static bool const result = envflags::on( "RW_GPU_UPLOAD_MERGE", true ) && envflags::uploadDedupe();
		return result;
	}

	void CommandBuffer::doMergeUploads()const
	{
		if ( m_uploadSpans.size() < 2u || m_hasBufferWrites )
		{
			m_uploadSpans.clear();
			return;
		}
		std::sort( m_uploadSpans.begin(), m_uploadSpans.end(), []( UploadSpan const & l, UploadSpan const & r )
			{
				return l.memory != r.memory ? std::less< ObjectMemory const * >{}( l.memory, r.memory ) : l.begin < r.begin;
			} );
		size_t i = 0u;
		while ( i < m_uploadSpans.size() )
		{
			auto const * memory = m_uploadSpans[i].memory;
			size_t groupEnd = i;
			while ( groupEnd < m_uploadSpans.size() && m_uploadSpans[groupEnd].memory == memory ) ++groupEnd;
			bool eligible = groupEnd - i >= 2u && memory && memory->resource;
			if ( eligible )
			{
				D3D11_RESOURCE_DIMENSION dimension{};
				memory->resource->GetType( &dimension );
				eligible = dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
				if ( eligible )
				{
					D3D11_BUFFER_DESC desc{};
					static_cast< ID3D11Buffer * >( memory->resource )->GetDesc( &desc );
					eligible = ( desc.BindFlags & ( D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL ) ) == 0u;
				}
			}
			if ( eligible )
			{
				size_t c = i;
				while ( c < groupEnd )
				{
					VkDeviceSize clusterBegin = m_uploadSpans[c].begin, clusterEnd = m_uploadSpans[c].end;
					size_t first = c, n = c + 1u;
					while ( n < groupEnd && m_uploadSpans[n].begin <= clusterEnd + 4096u
						&& std::max( clusterEnd, m_uploadSpans[n].end ) - clusterBegin <= 262144u )
					{
						clusterEnd = std::max( clusterEnd, m_uploadSpans[n].end );
						if ( m_uploadSpans[n].seq < m_uploadSpans[first].seq ) first = n;
						++n;
					}
					if ( n - c >= 2u )
					{
						m_uploadSpans[first].command->widen( clusterBegin, clusterEnd );
						if ( g_uploadsMerged.fetch_add( uint64_t( n - c - 1u ), std::memory_order_relaxed ) == 0u )
							std::fprintf( stderr, "[RW-GPU-UPLOAD] merging small buffer uploads per recording (RW_GPU_UPLOAD_MERGE=0 switches it off)\n" );
					}
					c = n;
				}
			}
			i = groupEnd;
		}
		m_uploadSpans.clear();
	}

	void CommandBuffer::doMarkGpuWritten( VkBuffer buffer )const
	{
		if ( buffer ) get( buffer )->getObjectMemory().gpuWritten = true;
	}

	std::shared_ptr< ubring::View > CommandBuffer::doPrepareUniformRing( VkDescriptorSet descriptor
		, ArrayView< uint32_t const > const & dynamicOffsets
		, std::vector< uint8_t > & skip
		, std::vector< uint8_t > & skipStatic )const
	{
		if ( !ubring::enabled() ) return nullptr;
		auto device = get( m_device );
		if ( device->getFeatureLevel() < D3D_FEATURE_LEVEL_11_1 || !device->canMapNoOverwriteConstantBuffer() ) return nullptr;
		auto set = get( descriptor );
		if ( set->getDynamicBuffers().empty() && set->getUniformBuffers().empty() ) return nullptr;
		std::shared_ptr< ubring::View > view;
		auto ensureView = [&]()
		{
			if ( view ) return;
			view = std::make_shared< ubring::View >();
			view->elementToWindow.assign( dynamicOffsets.size(), -1 );
			skip.assign( dynamicOffsets.size(), 0u );
			size_t statics = 0u;
			for ( auto const * writes : set->getUniformBuffers() )
				for ( auto const & write : writes->writes ) statics += write.descriptorCount;
			view->staticToWindow.assign( statics, -1 );
			skipStatic.assign( statics, 0u );
		};
		// Returns the window for one uniform-buffer descriptor, or false when it must use the old path.
		auto makeWindow = [&]( VkDescriptorBufferInfo const & info, uint64_t dynamicOffset, bool dynamic, ubring::Window & window ) -> bool
		{
			if ( !info.buffer ) return false;
			auto buf = get( info.buffer );
			auto const & memory = buf->getObjectMemory();
			auto parent = get( buf->getMemory() );
			if ( !memory.resource || memory.gpuWritten || !parent->shadow()
				|| !checkFlag( parent->getMemoryPropertyFlags(), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) ) return false;
			D3D11_RESOURCE_DIMENSION dimension{};
			memory.resource->GetType( &dimension );
			if ( dimension != D3D11_RESOURCE_DIMENSION_BUFFER ) return false;
			D3D11_BUFFER_DESC desc{};
			static_cast< ID3D11Buffer * >( memory.resource )->GetDesc( &desc );
			if ( desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER ) return false;
			uint64_t const size = buf->getSize();
			uint64_t const offset = uint64_t( info.offset ) + dynamicOffset;
			if ( offset >= size || ( offset % 256u ) != 0u ) return false;
			uint64_t range = 0u;
			if ( dynamic )   // same range rules as getDynamicUniformBufferRange
			{
				range = info.range == VK_WHOLE_SIZE ? size - offset : uint64_t( info.range );
				if ( range == 0u || range > size - offset ) return false;
			}
			else             // same range rules as bindUniformBuffer
			{
				range = std::min< uint64_t >( uint64_t( info.range ), size );
				if ( range == 0u ) return false;
			}
			uint64_t const bindBytes = ( range + 255u ) / 256u * 256u;
			if ( bindBytes > 65536u ) return false;
			uint64_t const copyBytes = std::min< uint64_t >( bindBytes, size - offset );
			uint64_t const shadowOffset = uint64_t( memory.offset ) + offset;
			if ( shadowOffset + copyBytes > uint64_t( parent->debugShadowSize() ) ) return false;
			window.memory = &memory;
			window.parent = parent;
			window.lifetime = parent->getLifetimeToken();
			window.objectOffset = offset;
			window.shadowOffset = shadowOffset;
			window.copyBytes = UINT( copyBytes );
			window.bindBytes = UINT( bindBytes );
			return true;
		};
		size_t offsetIndex = 0u;
		for ( auto const * writes : set->getDynamicBuffers() )
		{
			if ( writes->binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC )
			{
				for ( auto const & write : writes->writes )
				{
					for ( uint32_t i = 0u; i < write.descriptorCount; ++i )
					{
						size_t const index = offsetIndex + write.dstArrayElement + i;
						if ( index >= dynamicOffsets.size() ) continue;
						ubring::Window window;
						if ( !makeWindow( write.pBufferInfo[i], uint64_t( dynamicOffsets[index] ), true, window ) ) continue;
						ensureView();
						view->elementToWindow[index] = int32_t( view->windows.size() );
						view->windows.push_back( std::move( window ) );
						skip[index] = 1u;
					}
				}
			}
			offsetIndex += writes->binding.descriptorCount;
		}
		// v13.75: plain uniform buffers - Waylith's per-draw uniforms (VulkanMeshDrawEncoder.TryUploadUniforms
		// writes a window of its 4 MB "DrawCallBuilder.Uniforms" buffer into a new descriptor set per draw)
		static bool const statics = envflags::on( "RW_GPU_UNIFORM_RING_STATIC", true );
		if ( statics )
		{
			size_t element = 0u;
			for ( auto const * writes : set->getUniformBuffers() )
			{
				for ( auto const & write : writes->writes )
				{
					for ( uint32_t i = 0u; i < write.descriptorCount; ++i, ++element )
					{
						ubring::Window window;
						if ( !makeWindow( write.pBufferInfo[i], 0u, false, window ) ) continue;
						ensureView();
						view->staticToWindow[element] = int32_t( view->windows.size() );
						view->windows.push_back( std::move( window ) );
						skipStatic[element] = 1u;
					}
				}
			}
		}
		return view;
	}

	void CommandBuffer::doFillUniformRing( Context & context )const
	{
		// One Map for all windows of this execution (see D3D11UniformRing.hpp). Any problem: every
		// window keeps ok=false and BindDescriptorSetCommand uses the v13.73 path for it.
		uint64_t total = 0u;
		for ( auto const & view : m_ringViews )
		{
			for ( auto & window : view->windows ) window.ok = false;
			view->ringBuffer = nullptr;
			if ( view->forceFallback ) continue;
			for ( auto const & window : view->windows ) total += window.bindBytes;
		}
		if ( !total ) return;
		auto * ring = ubring::ring( get( m_device )->getDevice() );
		if ( !ring || total > ring->size() ) return;
		std::lock_guard< std::mutex > lock{ ring->mutex() };
		UINT batchStart = 0u;
		uint8_t * dst = ring->begin( *context.context, UINT( total ), batchStart );
		if ( !dst ) return;
		UINT cursor = 0u;
		uint64_t count = 0u;
		for ( auto const & view : m_ringViews )
		{
			if ( view->forceFallback ) continue;
			view->ringBuffer = ring->buffer();
			for ( auto & window : view->windows )
			{
				if ( !window.lifetime.expired() && !window.memory->gpuWritten )
				{
					auto const * src = window.parent->shadow();
					if ( src )
					{
						std::memcpy( dst + cursor, src + window.shadowOffset, window.copyBytes );
						window.firstConstant = ( batchStart + cursor ) / 16u;
						window.ok = true;
						++count;
					}
				}
				cursor += window.bindBytes;
			}
		}
		ring->end( *context.context );
		if ( ubring::g_windows.fetch_add( count, std::memory_order_relaxed ) == 0u && count )
			std::fprintf( stderr, "[RW-GPU-UBRING] first batch: %llu uniform windows (%u bytes) copied with one Map (RW_GPU_UNIFORM_RING=0 switches it off)\n", (unsigned long long)count, unsigned( total ) );
	}

	VkResult CommandBuffer::end()const
	{
		if ( m_recordStart.QuadPart )   // v13.62: recording time
		{
			static LARGE_INTEGER const freq = []() { LARGE_INTEGER f{}; QueryPerformanceFrequency( &f ); return f; }();
			LARGE_INTEGER now{};
			QueryPerformanceCounter( &now );
			double const recordUs = freq.QuadPart ? double( now.QuadPart - m_recordStart.QuadPart ) * 1e6 / double( freq.QuadPart ) : 0.0;
			g_recordUs.fetch_add( uint64_t( recordUs ), std::memory_order_relaxed );
			// v13.64: the bridge's share of the recording time (rdtsc ratio, converted with the QPC wall time)
			uint64_t const tscTotal = __rdtsc() - m_recordTscStart;
			if ( tscTotal ) g_recordBridgeUs.fetch_add( uint64_t( recordUs * double( std::min( m_recordTscBridge, tscTotal ) ) / double( tscTotal ) ), std::memory_order_relaxed );
			g_recordedCommands.fetch_add( m_commands.size(), std::memory_order_relaxed );
			m_recordStart.QuadPart = 0;
		}
		if ( m_recordingResult != VK_SUCCESS )
		{
			// v13.10 watchdog: a failed recording is fatal for the game's render thread
			// (RimWorld's binding throws on vkEndCommandBuffer). Say exactly what was in
			// the buffer so the failing pattern can be reproduced in the harness.
			static std::atomic< uint32_t > reports{ 0u };
			if ( reports++ < 8u )
			{
				std::fprintf( stderr, "[RW-GPU-WATCHDOG] vkEndCommandBuffer failing with result=%d after %u commands; last commands:\n"
					, int( m_recordingResult ), unsigned( m_commands.size() ) );
				size_t const first = m_commands.size() > 48u ? m_commands.size() - 48u : 0u;
				for ( size_t i = first; i < m_commands.size(); ++i )
				{
					auto const & cmd = *m_commands[i];
					std::fprintf( stderr, "[RW-GPU-WATCHDOG]   #%u %s\n", unsigned( i ), typeid( cmd ).name() );
				}
				std::fflush( stderr );
			}
			return m_recordingResult;
		}
		if ( auto status = get( m_device )->getStatus(); status != VK_SUCCESS ) return status;
		m_state.pushConstantBuffers.clear();
		doMergeUploads();   // v13.73
		if ( m_hasBufferWrites )   // v13.74: transfer writes or secondary command buffers in this recording: old path for every window
			for ( auto & view : m_ringViews ) view->forceFallback = true;
		doFinishAfterSubmitActions();   // v13.51: recording order -> run order

		if constexpr ( UseCommandsList )
		{
			if ( m_deferredContext )
			{
				safeRelease( m_commandList );
				Context context{ get( m_device )->getFeatureLevel(), m_device, DeviceContextLock{ m_deferredContext } };
				m_deferredContext->AddRef();
				execute( context );
				m_deferredContext->FinishCommandList( TRUE, &m_commandList );
			}
		}

		return VK_SUCCESS;
	}

	VkResult CommandBuffer::reset()const
	{
		m_recordingResult = VK_SUCCESS;
		m_mappedResources.clear();
		m_afterSubmitActions.clear();
		m_afterSubmitKeys.clear();
		m_afterSubmitRecordingOrder = true;
		reaper::retire( m_commands );   // v13.69: freed on a background thread
		m_uploadSpans.clear();   // v13.73
		m_hasBufferWrites = false;
		m_ringViews.clear();   // v13.74
		m_state = State{};
		return VK_SUCCESS;
	}

	void CommandBuffer::beginRenderPass( VkRenderPassBeginInfo beginInfo )const
	{
		m_state.currentRenderPass = beginInfo.renderPass;
		m_state.currentFrameBuffer = beginInfo.framebuffer;
		m_state.currentSubpassIndex = 0u;
		m_state.currentSubpass = &get( m_state.currentRenderPass )->getSubpasses()[m_state.currentSubpassIndex];
		++m_state.currentSubpassIndex;
		// v13.33: vertex buffer bindings are command-buffer state in Vulkan; they are not
		// reset by render passes or pipeline binds (Waylith binds its per-instance default
		// attribute buffer once and keeps drawing with it through many pipelines).
		m_commands.emplace_back( std::make_unique< BeginRenderPassCommand >( m_device
			, m_state.currentRenderPass
			, m_state.currentFrameBuffer
			, VkClearValueArray{ beginInfo.pClearValues, beginInfo.pClearValues + beginInfo.clearValueCount }
			, beginInfo.renderArea ) );
		m_commands.emplace_back( std::make_unique< BeginSubpassCommand >( m_device
			, m_state.currentRenderPass
			, m_state.currentFrameBuffer
			, *m_state.currentSubpass ) );
	}

	void CommandBuffer::nextSubpass()const
	{
		m_commands.emplace_back( std::make_unique< EndSubpassCommand >( m_device
			, m_state.currentFrameBuffer
			, *m_state.currentSubpass ) );
		m_state.currentSubpass = &get( m_state.currentRenderPass )->getSubpasses()[m_state.currentSubpassIndex];
		++m_state.currentSubpassIndex;
		m_commands.emplace_back( std::make_unique< BeginSubpassCommand >( m_device
			, m_state.currentRenderPass
			, m_state.currentFrameBuffer
			, *m_state.currentSubpass ) );
	}

	void CommandBuffer::endRenderPass()const
	{
		m_commands.emplace_back( std::make_unique< EndSubpassCommand >( m_device
			, m_state.currentFrameBuffer
			, *m_state.currentSubpass ) );
		m_commands.emplace_back( std::make_unique< EndRenderPassCommand >( m_device
			, m_state.currentRenderPass
			, m_state.currentFrameBuffer ) );
	}

	void CommandBuffer::executeCommands( ArrayView< VkCommandBuffer const > commands )const
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		if constexpr ( UseCommandsList )
		{
			for ( auto & commandBuffer : commands )
			{
				m_commands.emplace_back( std::make_unique< ExecuteCommandsCommand >( m_device
					, commandBuffer ) );
			}
		}
		else
		{
			for ( auto & commandBuffer : commands )
			{
				auto & dxCommandBuffer = *get( commandBuffer );
				if ( dxCommandBuffer.getRecordingResult() != VK_SUCCESS ) { recordFailure( dxCommandBuffer.getRecordingResult() ); return; }

				for ( auto & command : dxCommandBuffer.getCommands() )
				{
					m_commands.emplace_back( command->clone() );
				}

				m_commands.emplace_back( std::make_unique< ExecuteActionsCommand >( m_device
					, dxCommandBuffer.m_afterSubmitActions ) );
			}
		}
	}

	void CommandBuffer::clearColorImage( VkImage image
		, VkClearColorValue colour
		, ArrayView< VkImageSubresourceRange const > ranges )const
	{
		if ( m_recordingResult != VK_SUCCESS ) return;
		try
		{
			m_commands.emplace_back( std::make_unique< ClearColourCommand >( m_device
				, image, ranges, colour ) );
		}
		catch ( Exception const & error )
		{
			m_recordingResult = error.getResult();
			std::cerr << "Clear image recording failed: " << error.what() << std::endl;
		}
		catch ( std::bad_alloc const & )
		{
			m_recordingResult = VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		catch ( std::exception const & error )
		{
			m_recordingResult = VK_ERROR_INITIALIZATION_FAILED;
			std::cerr << "Clear image recording failed: " << error.what() << std::endl;
		}
	}

	void CommandBuffer::clearDepthStencilImage( VkImage image
		, VkClearDepthStencilValue value
		, ArrayView< VkImageSubresourceRange const > ranges )const
	{
		m_commands.emplace_back( std::make_unique< ClearDepthStencilCommand >( m_device
			, image
			, ranges
			, value ) );
	}

	void CommandBuffer::clearAttachments( ArrayView< VkClearAttachment const > clearAttachments
		, ArrayView< VkClearRect const > clearRects )
	{
		m_commands.emplace_back( std::make_unique< ClearAttachmentsCommand >( m_device
			, *m_state.currentSubpass
			, m_state.currentFrameBuffer
			, clearAttachments
			, clearRects ) );
	}

	void CommandBuffer::bindPipeline( VkPipeline pipeline
		, VkPipelineBindPoint bindingPoint )const
	{
		if ( bindingPoint == VK_PIPELINE_BIND_POINT_GRAPHICS )
		{
			m_state.currentPipeline = pipeline;
			if ( !m_state.resolvedUavs || m_state.resolvedLayout != get( pipeline )->getLayout() ) m_state.resolvedUavs.reset();
			doApplyDynamicStates();
			m_commands.emplace_back( std::make_unique< BindPipelineCommand >( m_device
				, pipeline
				, bindingPoint ) );
			doAddAfterSubmitAction();

			for ( auto const & [layout, desc] : m_state.pushConstantBuffers )
			{
				m_commands.emplace_back( std::make_unique< PushConstantsCommand >( m_device
					, get( m_state.currentPipeline )->findPushConstantBuffer( desc ) ) );
				doAddAfterSubmitAction();
			}

			m_state.pushConstantBuffers.clear();
		}
		else
		{
			m_state.currentComputePipeline = pipeline;
			m_commands.emplace_back( std::make_unique< BindComputePipelineCommand >( m_device
				, pipeline
				, bindingPoint ) );
			doAddAfterSubmitAction();

			for ( auto const & [layout, desc] : m_state.pushConstantBuffers )
			{
				m_commands.emplace_back( std::make_unique< PushConstantsCommand >( m_device
					, get( m_state.currentPipeline )->findPushConstantBuffer( desc ) ) );
				doAddAfterSubmitAction();
			}

			m_state.pushConstantBuffers.clear();
		}

		auto it = m_state.boundDescriptors.end();

		for ( auto & layout : get( pipeline )->getDescriptorsLayouts() )
		{
			it = std::remove_if( m_state.boundDescriptors.begin()
				, it
				, [&layout]( VkDescriptorSet lookup )
				{
					return get( lookup )->getLayout() == layout;
				} );
		}

		if ( it != m_state.boundDescriptors.begin() )
		{
			m_state.boundDescriptors.erase( m_state.boundDescriptors.begin(), it );
		}
	}

	void CommandBuffer::bindVertexBuffers( uint32_t firstBinding
		, ArrayView< VkBuffer const > buffers
		, ArrayView< uint64_t const > offsets )const
	{
		assert( buffers.size() == offsets.size() );
		if ( m_state.vboSlots.size() < firstBinding + buffers.size() )
		{
			m_state.vboSlots.resize( firstBinding + buffers.size() );
		}

		++m_state.vboVersion;   // v13.60
		for ( auto i = 0u; i < buffers.size(); ++i )
		{
			auto & slot = m_state.vboSlots[ firstBinding + i ];
			slot.buffer = buffers[i];
			slot.d3dBuffer = buffers[i] ? get( buffers[i] )->getBuffer() : nullptr;
			slot.offset = UINT( offsets[i] );
		}
	}

	void CommandBuffer::bindIndexBuffer( VkBuffer buffer
		, uint64_t offset
		, VkIndexType indexType )const
	{
		m_commands.emplace_back( std::make_unique< BindIndexBufferCommand >( m_device
			, buffer
			, offset
			, indexType ) );
		doAddAfterSubmitAction();
		m_state.indexType = indexType;
		m_state.newlyBoundIbo = buffer;
		m_state.boundIbo = buffer;
		m_state.boundIboOffset = offset;
	}

	void CommandBuffer::bindDescriptorSets( VkPipelineBindPoint bindingPoint
		, VkPipelineLayout layout
		, uint32_t firstSet
		, ArrayView< VkDescriptorSet const > descriptorSets
		, ArrayView< uint32_t const > dynamicOffsets )const
	{
		try
		{
		if ( m_recordingResult != VK_SUCCESS ) return;

		auto const & layouts = get( layout )->getDescriptorsLayouts();

		if ( firstSet > layouts.size()
			|| descriptorSets.size() > layouts.size() - firstSet )
		{
			recordFailure( VK_ERROR_INITIALIZATION_FAILED );
			return;
		}

		size_t dynamicOffsetIndex{};

		// Validate before recording any command so malformed offsets cannot leave a
		// partially bound descriptor sequence behind.
		for ( auto descriptorSetIndex = 0u; descriptorSetIndex < descriptorSets.size(); ++descriptorSetIndex )
		{
			auto descriptorSet = descriptorSets[descriptorSetIndex];

			if ( !descriptorSet
				|| get( descriptorSet )->getLayout() != layouts[firstSet + descriptorSetIndex] )
			{
				recordFailure( VK_ERROR_INITIALIZATION_FAILED );
				return;
			}

			// v13.49: dynamic storage buffers are supported now (UAV views at descriptor offset +
			// dynamic offset, see Buffer::getUnorderedAccessView( offset, range )). They were
			// rejected here before (the recording failed with VK_ERROR_FEATURE_NOT_PRESENT).

			auto const dynamicOffsetCount = get( descriptorSet )->getDynamicOffsetCount();

			if ( dynamicOffsetIndex > dynamicOffsets.size()
				|| dynamicOffsetCount > dynamicOffsets.size() - dynamicOffsetIndex )
			{
				recordFailure( VK_ERROR_INITIALIZATION_FAILED );
				return;
			}

			dynamicOffsetIndex += dynamicOffsetCount;
		}

		if ( dynamicOffsetIndex != dynamicOffsets.size() )
		{
			recordFailure( VK_ERROR_INITIALIZATION_FAILED );
			return;
		}

		dynamicOffsetIndex = 0u;

		for ( auto descriptorSetIndex = 0u; descriptorSetIndex < descriptorSets.size(); ++descriptorSetIndex )
		{
			auto descriptorSet = descriptorSets[descriptorSetIndex];
			auto const dynamicOffsetCount = get( descriptorSet )->getDynamicOffsetCount();
			auto dynamicOffsetBegin = dynamicOffsets.begin() + dynamicOffsetIndex;
			auto dynamicOffsetEnd = dynamicOffsetBegin + dynamicOffsetCount;
			auto const setDynamicOffsets = ArrayView< uint32_t const >{ dynamicOffsetBegin, dynamicOffsetEnd };

			// v13.71: a newly bound set replaces an earlier one with the same layout (Waylith binds a new
			// set per draw, so the list grew between pipeline binds and was walked on every draw by
			// doProcessMappedBoundDescriptorsResourcesOut). The replaced set's resources were already
			// handled at the draws that used it. RW_GPU_BOUND_SET_DEDUPE=0 = old behaviour.
			static bool const dedupeBound = envflags::on( "RW_GPU_BOUND_SET_DEDUPE", true );
			if ( dedupeBound )
			{
				auto const layout = get( descriptorSet )->getLayout();
				auto & list = m_state.boundDescriptors;
				list.erase( std::remove_if( list.begin(), list.end(), [layout]( VkDescriptorSet s ) { return get( s )->getLayout() == layout; } ), list.end() );
			}
			m_state.boundDescriptors.push_back( descriptorSet );
			std::vector< uint8_t > ringSkip, ringSkipStatic;   // v13.74 / v13.75
			auto ringView = doPrepareUniformRing( descriptorSet, setDynamicOffsets, ringSkip, ringSkipStatic );
			doProcessMappedBoundDescriptorResourcesIn( descriptorSet, setDynamicOffsets, ringView ? &ringSkip : nullptr, ringView ? &ringSkipStatic : nullptr );
			if ( bindingPoint == VK_PIPELINE_BIND_POINT_GRAPHICS )
			{
				// v13.47: remember what is bound per set index for the draw-time UAV resolve.
				auto setIndex = firstSet + descriptorSetIndex;
				if ( m_state.graphicsSets.size() <= setIndex ) m_state.graphicsSets.resize( setIndex + 1u );
				auto & entry = m_state.graphicsSets[setIndex];   // v13.69: assign in place (no vector per bind)
				// v13.69: the resolved output-merger UAV list only depends on sets that carry storage
				// descriptors; replacing a set without any by another set without any changes nothing,
				// so the (allocating) re-resolve at the next draw is skipped. RW_GPU_UAV_RESOLVE_ALWAYS=1 = old.
				static bool const resolveAlways = envflags::readOne( "RW_GPU_UAV_RESOLVE_ALWAYS" );
				auto hasUav = []( VkDescriptorSet s )
					{
						if ( !s ) return false;
						auto d = get( s );
						if ( !d->getStorageBuffers().empty() || !d->getStorageTextures().empty() ) return true;
						for ( auto const * w : d->getDynamicBuffers() )
							if ( w->binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ) return true;
						return false;
					};
				bool const touchesUavs = resolveAlways || hasUav( descriptorSet ) || hasUav( entry.first );
				entry.first = descriptorSet;
				entry.second.assign( setDynamicOffsets.begin(), setDynamicOffsets.end() );
				if ( touchesUavs ) m_state.resolvedUavs.reset();
			}
			{
				auto command = std::make_unique< BindDescriptorSetCommand >( m_device
					, descriptorSet
					, layout
					, firstSet + descriptorSetIndex
					, setDynamicOffsets
					, bindingPoint );
				if ( ringView )
				{
					command->setUniformRing( ringView );
					m_ringViews.push_back( std::move( ringView ) );
				}
				m_commands.emplace_back( std::move( command ) );
			}
			doAddAfterSubmitAction();
			dynamicOffsetIndex += dynamicOffsetCount;
		}
		}
		catch ( Exception const & error ) { recordFailure( error.getResult() ); }
		catch ( std::bad_alloc const & ) { recordFailure( VK_ERROR_OUT_OF_HOST_MEMORY ); }
		catch ( std::exception const & error )
		{
			recordFailure( VK_ERROR_INITIALIZATION_FAILED );
			std::cerr << "Descriptor recording failed: " << error.what() << std::endl;
		}

	}

	void CommandBuffer::setViewport( uint32_t firstViewport
		, ArrayView< VkViewport const > viewports )const
	{
		m_commands.emplace_back( std::make_unique< ViewportCommand >( m_device, firstViewport, viewports ) );
	}

	void CommandBuffer::setScissor( uint32_t firstScissor
		, ArrayView< VkRect2D const > scissors )const
	{
		m_commands.emplace_back( std::make_unique< ScissorCommand >( m_device, firstScissor, scissors ) );
	}

	void CommandBuffer::doResolveOutputUavs()const
	{
		// v13.47: rebuild the full OM UAV array from the bound sets and the drawing pipeline's
		// own layout (the layout the shader registers were assigned from).
		if ( !ResolveOutputUavsCommand::enabled() || !m_state.currentPipeline ) return;
		auto layout = get( m_state.currentPipeline )->getLayout();
		if ( !m_state.resolvedUavs || m_state.resolvedLayout != layout )
		{
			m_state.resolvedUavs = ResolveOutputUavsCommand::resolve( layout, m_state.graphicsSets );
			m_state.resolvedLayout = layout;
		}
		uint32_t mask = 0u;
		for ( auto const & [stage, desc] : get( m_state.currentPipeline )->getProgramLayout() ) mask |= desc.uavRegisterMask;
		m_commands.emplace_back( std::make_unique< ResolveOutputUavsCommand >( m_device, m_state.resolvedUavs, mask, m_state.currentPipeline ) );
	}

	void CommandBuffer::draw( uint32_t vtxCount
		, uint32_t instCount
		, uint32_t firstVertex
		, uint32_t firstInstance )const
	{
		doBindDynamicStates();
		doFillVboStrides();

		if ( !get( m_state.currentPipeline )->hasVertexLayout() )
		{
			bindIndexBuffer( get( m_device )->getEmptyIndexedVaoIdx(), 0u, VK_INDEX_TYPE_UINT32 );
			doProcessMappedBoundIndexBufferIn();
			doProcessMappedBoundVaoBuffersIn();
			doResolveOutputUavs();
			m_commands.emplace_back( std::make_unique< DrawIndexedCommand >( m_device
				, vtxCount
				, instCount
				, 0u
				, firstVertex
				, firstInstance
				, get( m_state.currentPipeline )->getInputAssemblyState().topology
				, m_state.indexType
				, currentVbos() ) );
		}
		else
		{
			doProcessMappedBoundVaoBuffersIn();
			doResolveOutputUavs();
			m_commands.emplace_back( std::make_unique< DrawCommand >( m_device
				, vtxCount
				, instCount
				, firstVertex
				, firstInstance
				, get( m_state.currentPipeline )->getInputAssemblyState().topology
				, currentVbos() ) );
		}

		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentPipeline );
	}

	void CommandBuffer::drawIndexed( uint32_t indexCount
		, uint32_t instCount
		, uint32_t firstIndex
		, uint32_t vertexOffset
		, uint32_t firstInstance )const
	{
		if ( !get( m_state.currentPipeline )->hasVertexLayout()
			&& m_state.boundIbo == nullptr )
		{
			bindIndexBuffer( get( m_device )->getEmptyIndexedVaoIdx(), 0u, VK_INDEX_TYPE_UINT32 );
		}

		doBindDynamicStates();
		doFillVboStrides();
		doProcessMappedBoundVaoBuffersIn();
		doProcessMappedBoundIndexBufferIn();
		doResolveOutputUavs();
		m_commands.emplace_back( std::make_unique< DrawIndexedCommand >( m_device
			, indexCount
			, instCount
			, firstIndex
			, vertexOffset
			, firstInstance
			, get( m_state.currentPipeline )->getInputAssemblyState().topology
			, m_state.indexType
			, currentVbos()
			, m_state.boundIbo
			, m_state.boundIboOffset ) );
		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentPipeline );
		m_state.newlyBoundIbo = nullptr;
	}

	void CommandBuffer::drawIndirect( VkBuffer buffer
		, VkDeviceSize offset
		, uint32_t drawCount
		, uint32_t stride )const
	{
		doProcessMappedBoundResourceIn( buffer, offset, get( buffer )->getSize() - offset );
		doFillVboStrides();
		doProcessMappedBoundVaoBuffersIn();
		doBindDynamicStates();
		doResolveOutputUavs();
		m_commands.emplace_back( std::make_unique< DrawIndirectCommand >( m_device
			, buffer
			, offset
			, drawCount
			, stride
			, get( m_state.currentPipeline )->getInputAssemblyState().topology
			, currentVbos() ) );
		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentPipeline );
	}

	void CommandBuffer::drawIndexedIndirect( VkBuffer buffer
		, VkDeviceSize offset
		, uint32_t drawCount
		, uint32_t stride )const
	{
		doProcessMappedBoundResourceIn( buffer, offset, get( buffer )->getSize() - offset );
		if ( !get( m_state.currentPipeline )->hasVertexLayout()
			&& m_state.boundIbo == nullptr )
		{
			bindIndexBuffer( get( m_device )->getEmptyIndexedVaoIdx(), 0u, VK_INDEX_TYPE_UINT32 );
		}

		doFillVboStrides();
		doProcessMappedBoundVaoBuffersIn();
		doProcessMappedBoundIndexBufferIn();
		doBindDynamicStates();
		doResolveOutputUavs();
		m_commands.emplace_back( std::make_unique< DrawIndexedIndirectCommand >( m_device
			, buffer
			, offset
			, drawCount
			, stride
			, get( m_state.currentPipeline )->getInputAssemblyState().topology
			, m_state.indexType
			, currentVbos() ) );
		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentPipeline );
		m_state.newlyBoundIbo = nullptr;
	}

	void CommandBuffer::copyToImage( VkBuffer src
		, VkImage dst
		, ArrayView< VkBufferImageCopy const > copyInfos )const
	{
		if ( m_recordingResult != VK_SUCCESS ) return;
		try
		{
			doProcessMappedBoundResourceIn( src, 0u, get( src )->getSize() );
			get( m_device )->onCopyToImageCommand( get( this ), copyInfos, src, dst );
			m_commands.emplace_back( std::make_unique< CopyBufferToImageCommand >( m_device
				, std::move( copyInfos )
				, src
				, dst ) );
		}
		catch ( Exception const & error )
		{
			m_recordingResult = error.getResult();
			std::cerr << "Buffer to image copy recording failed: " << error.what() << std::endl;
		}
		catch ( std::bad_alloc const & )
		{
			m_recordingResult = VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		catch ( std::exception const & error )
		{
			m_recordingResult = VK_ERROR_INITIALIZATION_FAILED;
			std::cerr << "Buffer to image copy recording failed: " << error.what() << std::endl;
		}
	}

	void CommandBuffer::copyToBuffer( VkImage src
		, VkBuffer dst
		, ArrayView< VkBufferImageCopy const > copyInfos )const
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		doMarkGpuWritten( dst );   // v13.74
		if ( m_recordingResult != VK_SUCCESS ) return;
		try
		{
			m_commands.emplace_back( std::make_unique< CopyImageToBufferCommand >( m_device
				, std::move( copyInfos )
				, src
				, dst ) );
		}
		catch ( Exception const & error )
		{
			m_recordingResult = error.getResult();
			std::cerr << "Image to buffer copy recording failed: " << error.what() << std::endl;
		}
		catch ( std::bad_alloc const & )
		{
			m_recordingResult = VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		catch ( std::exception const & error )
		{
			m_recordingResult = VK_ERROR_INITIALIZATION_FAILED;
			std::cerr << "Image to buffer copy recording failed: " << error.what() << std::endl;
		}
	}

	void CommandBuffer::updateBuffer( VkBuffer dstBuffer
		, VkDeviceSize dstOffset
		, ArrayView< uint8_t const > data )
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		doMarkGpuWritten( dstBuffer );   // v13.74
		m_commands.emplace_back( std::make_unique< UpdateBufferCommand >( m_device
			, dstBuffer
			, dstOffset
			, data ) );
	}

	void CommandBuffer::fillBuffer( VkBuffer dstBuffer
		, [[maybe_unused]] VkDeviceSize dstOffset
		, VkDeviceSize size
		, uint32_t data )
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		doMarkGpuWritten( dstBuffer );   // v13.74
		m_commands.emplace_back( std::make_unique< FillBufferCommand >( m_device
			, dstBuffer
			, size
			, data ) );
	}

	void CommandBuffer::copyBuffer( VkBuffer src
		, VkBuffer dst
		, ArrayView< VkBufferCopy const > copyInfos )const
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		doMarkGpuWritten( dst );   // v13.74
		doProcessMappedBoundResourceIn( src, 0u, get( src )->getSize() );
		for ( auto const & copyInfo : copyInfos )
		{
			m_commands.emplace_back( std::make_unique< CopyBufferCommand >( m_device
				, copyInfo
				, src
				, dst ) );
		}
	}

	void CommandBuffer::copyImage( VkImage src
		, VkImage dst
		, ArrayView< VkImageCopy const > copyInfos )const
	{
		if ( m_recordingResult != VK_SUCCESS ) return;
		try
		{
			for ( auto & copyInfo : copyInfos )
			{
				m_commands.emplace_back( std::make_unique< CopyImageCommand >( m_device
					, copyInfo
					, src
					, dst ) );
			}
		}
		catch ( Exception const & error )
		{
			m_recordingResult = error.getResult();
			std::cerr << "Image copy recording failed: " << error.what() << std::endl;
		}
		catch ( std::bad_alloc const & )
		{
			m_recordingResult = VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		catch ( std::exception const & error )
		{
			m_recordingResult = VK_ERROR_INITIALIZATION_FAILED;
			std::cerr << "Image copy recording failed: " << error.what() << std::endl;
		}
	}

	void CommandBuffer::blitImage( VkImage srcImage
		, VkImage dstImage
		, ArrayView< VkImageBlit const > regions
		, VkFilter filter )const
	{
		if ( m_recordingResult != VK_SUCCESS ) return;
		try
		{
			get( m_device )->requireHealthy( "Blit recording" );
			if ( !srcImage || !dstImage ) throw Exception{ VK_ERROR_INITIALIZATION_FAILED, "Null blit image" };
			framediag::onBlitRecorded( get( srcImage )->getResource(), get( dstImage )->getResource(), uint32_t( regions.size() ), int( filter ) );
			// v13.12: draw-based blit by default (RW_GPU_BLIT_MODE=compute restores the
			// compute/UAV path for A/B testing). See D3D11DrawBlitCommand.hpp for why.
			static bool const useDraw = []()
			{
				char value[16]{};
				auto const n = ::GetEnvironmentVariableA( "RW_GPU_BLIT_MODE", value, DWORD( sizeof( value ) ) );
				bool const compute = n > 0u && n < sizeof( value ) && std::string_view{ value } == "compute";
				std::fprintf( stderr, "[RW-GPU-BLIT] mode=%s\n", compute ? "compute" : "draw" );
				return !compute;
			}();
			if ( useDraw && DrawBlitCommand::isSupported( m_device, srcImage, dstImage ) )
			{
				m_commands.emplace_back( std::make_unique< DrawBlitCommand >( m_device, srcImage, dstImage, regions, filter ) );
				return;
			}
			if ( useDraw )
			{
				static std::atomic< uint32_t > fallbackCount{ 0u };
				if ( fallbackCount++ < 16u )
				{
					std::fprintf( stderr, "[RW-GPU-BLIT] draw path unavailable for srcFormat=%d dstFormat=%d, using compute path\n"
						, int( get( srcImage )->getFormat() ), int( get( dstImage )->getFormat() ) );
				}
			}
			auto & pipeline = doGetBlitPipeline( get( srcImage )->getFormat(), get( dstImage )->getFormat() );
			m_commands.emplace_back( std::make_unique< BlitImageCommand >( m_commandPool, get( this ), m_device, pipeline, srcImage, dstImage, regions, filter ) );
		}
		catch ( Exception const & e ) { recordFailure( e.getResult() ); std::fprintf( stderr, "[RW-GPU-BLIT] recording failed result=%d message=%s\n", int( e.getResult() ), e.what() ); }
		catch ( std::bad_alloc const & ) { recordFailure( VK_ERROR_OUT_OF_HOST_MEMORY ); }
		catch ( std::exception const & e ) { recordFailure( VK_ERROR_INITIALIZATION_FAILED ); std::fprintf( stderr, "[RW-GPU-BLIT] recording exception=%s\n", e.what() ); }
		catch ( ... ) { recordFailure( VK_ERROR_INITIALIZATION_FAILED ); }

	}

	void CommandBuffer::resolveImage( VkImage srcImage
		, VkImage dstImage
		, ArrayView< VkImageResolve const > regions )const
	{
		m_commands.emplace_back( std::make_unique< ResolveImageCommand >( m_device
			, srcImage
			, dstImage
			, regions ) );
	}

	void CommandBuffer::resetQueryPool( [[maybe_unused]] VkQueryPool pool
		, [[maybe_unused]] uint32_t firstQuery
		, [[maybe_unused]] uint32_t queryCount )const
	{
		m_commands.emplace_back( std::make_unique< ResetQueryPoolCommand >( m_device ) );
	}

	void CommandBuffer::beginQuery( VkQueryPool pool
		, uint32_t query
		, [[maybe_unused]] VkQueryControlFlags flags )const
	{
		m_commands.emplace_back( std::make_unique< BeginQueryCommand >( m_device
			, pool
			, query ) );
	}

	void CommandBuffer::endQuery( VkQueryPool pool
		, uint32_t query )const
	{
		m_commands.emplace_back( std::make_unique< EndQueryCommand >( m_device
			, pool
			, query ) );
	}

	void CommandBuffer::writeTimestamp( [[maybe_unused]] VkPipelineStageFlagBits pipelineStage
		, VkQueryPool pool
		, uint32_t query )const
	{
		m_commands.emplace_back( std::make_unique< WriteTimestampCommand >( m_device
			, pool
			, query ) );
	}

	void CommandBuffer::copyQueryPoolResults( VkQueryPool queryPool
		, uint32_t firstQuery
		, uint32_t queryCount
		, VkBuffer dstBuffer
		, VkDeviceSize dstOffset
		, VkDeviceSize stride
		, VkQueryResultFlags flags )const
	{
		m_hasBufferWrites = true;   // v13.73: no upload merging in this recording
		doMarkGpuWritten( dstBuffer );   // v13.74
		m_commands.emplace_back( std::make_unique< CopyQueryPoolResultsCommand >( m_device
			, queryPool
			, firstQuery
			, queryCount
			, dstBuffer
			, dstOffset
			, stride
			, flags ) );
	}

	void CommandBuffer::pushConstants( VkPipelineLayout layout
		, VkShaderStageFlags stageFlags
		, uint32_t offset
		, uint32_t size
		, void const * data )const
	{
		PushConstantsDesc desc
		{
			stageFlags,
			offset,
			size,
			{ reinterpret_cast< uint8_t const * >( data ), reinterpret_cast< uint8_t const * >( data ) + size }
		};

		if ( m_state.currentPipeline )
		{
			m_commands.emplace_back( std::make_unique< PushConstantsCommand >( m_device
				, get( m_state.currentPipeline )->findPushConstantBuffer( std::move( desc ) ) ) );   // v13.71: no copies
			doAddAfterSubmitAction();
		}
		else if ( m_state.currentComputePipeline )
		{
			m_commands.emplace_back( std::make_unique< PushConstantsCommand >( m_device
				, get( m_state.currentComputePipeline )->findPushConstantBuffer( std::move( desc ) ) ) );
			doAddAfterSubmitAction();
		}
		else
		{
			m_state.pushConstantBuffers.emplace_back( layout, desc );
		}
	}

	void CommandBuffer::dispatch( uint32_t groupCountX
		, uint32_t groupCountY
		, uint32_t groupCountZ )const
	{
		m_commands.emplace_back( std::make_unique< DispatchCommand >( m_device
			, groupCountX
			, groupCountY
			, groupCountZ ) );
		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentComputePipeline );
	}

	void CommandBuffer::dispatchIndirect( VkBuffer buffer
		, VkDeviceSize offset )const
	{
		doProcessMappedBoundResourceIn( buffer, offset, get( buffer )->getSize() - offset );
		m_commands.emplace_back( std::make_unique< DispatchIndirectCommand >( m_device
			, buffer
			, offset ) );
		doProcessMappedBoundDescriptorsResourcesOut( m_state.currentComputePipeline );
	}

	void CommandBuffer::setLineWidth( float width )const
	{
		m_state.lineWidth = width;
		doApplyDynamicStates();
	}

	void CommandBuffer::setDepthBias( float constantFactor
		, float clamp
		, float slopeFactor )const
	{
		m_state.depthBias = DepthBias{ constantFactor, clamp, slopeFactor };
		doApplyDynamicStates();
	}

	void CommandBuffer::setBlendConstants( const float blendConstants[4] )
	{
		m_state.blendConstants = std::array< float, 4u >{};
		m_state.blendConstants.value()[0] = blendConstants[0];
		m_state.blendConstants.value()[1] = blendConstants[1];
		m_state.blendConstants.value()[2] = blendConstants[2];
		m_state.blendConstants.value()[3] = blendConstants[3];
		doApplyDynamicStates();
	}

	void CommandBuffer::setDepthBounds( float minDepthBounds
		, float maxDepthBounds )
	{
		m_state.depthBounds = DepthBounds{ minDepthBounds, maxDepthBounds };
		doApplyDynamicStates();
	}

	void CommandBuffer::setStencilCompareMask( VkStencilFaceFlags faceMask
		, uint32_t compareMask )
	{
		if ( checkFlag( faceMask, VK_STENCIL_FACE_BACK_BIT ) )
		{
			m_state.stencilCompareMask[0] = compareMask;
		}

		if ( checkFlag( faceMask, VK_STENCIL_FACE_FRONT_BIT ) )
		{
			m_state.stencilCompareMask[1] = compareMask;
		}

		doApplyDynamicStates();
	}

	void CommandBuffer::setStencilWriteMask( VkStencilFaceFlags faceMask
		, uint32_t writeMask )
	{
		if ( checkFlag( faceMask, VK_STENCIL_FACE_BACK_BIT ) )
		{
			m_state.stencilWriteMask[0] = writeMask;
		}

		if ( checkFlag( faceMask, VK_STENCIL_FACE_FRONT_BIT ) )
		{
			m_state.stencilWriteMask[1] = writeMask;
		}

		doApplyDynamicStates();
	}

	void CommandBuffer::setStencilReference( VkStencilFaceFlags faceMask
		, uint32_t reference )
	{
		if ( checkFlag( faceMask, VK_STENCIL_FACE_BACK_BIT ) )
		{
			m_state.stencilReference[0] = reference;
		}

		if ( checkFlag( faceMask, VK_STENCIL_FACE_FRONT_BIT ) )
		{
			m_state.stencilReference[1] = reference;
		}

		doApplyDynamicStates();
	}

	void CommandBuffer::setEvent( VkEvent event
		, [[maybe_unused]] VkPipelineStageFlags stageMask )const
	{
		m_commands.emplace_back( std::make_unique< SetEventCommand >( m_device
			, event ) );
	}

	void CommandBuffer::resetEvent( VkEvent event
		, [[maybe_unused]] VkPipelineStageFlags stageMask )const
	{
		m_commands.emplace_back( std::make_unique< ResetEventCommand >( m_device
			, event ) );
	}

	void CommandBuffer::waitEvents( ArrayView< VkEvent const > events
		, [[maybe_unused]] VkPipelineStageFlags srcStageMask
		, [[maybe_unused]] VkPipelineStageFlags dstStageMask
		, [[maybe_unused]] ArrayView< VkBufferMemoryBarrier const > bufferMemoryBarriers
		, [[maybe_unused]] ArrayView< VkImageMemoryBarrier const > imageMemoryBarriers )const
	{
		m_commands.emplace_back( std::make_unique< WaitEventsCommand >( m_device
			, events ) );
	}

	void CommandBuffer::pipelineBarrier( [[maybe_unused]] VkPipelineStageFlags after
		, [[maybe_unused]] VkPipelineStageFlags before
		, [[maybe_unused]] ArrayView< VkMemoryBarrier const > memoryBarriers
		, ArrayView< VkBufferMemoryBarrier const > bufferMemoryBarriers
		, [[maybe_unused]] ArrayView< VkImageMemoryBarrier const > imageMemoryBarriers )const
	{
		m_commands.emplace_back( std::make_unique< MemoryBarrierCommand >( m_device
			, bufferMemoryBarriers ) );
	}

	void CommandBuffer::generateMipmaps( VkImage texture )const
	{
		m_commands.emplace_back( std::make_unique< GenerateMipsCommand >( m_device
			, texture ) );
	}

#if VK_EXT_debug_utils

	void CommandBuffer::beginDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pLabelName,
		};
	}

	void CommandBuffer::endDebugUtilsLabel()const
	{
		m_label = ashes::nullopt;
	}

	void CommandBuffer::insertDebugUtilsLabel( VkDebugUtilsLabelEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pLabelName,
		};
	}

#endif
#if VK_EXT_debug_marker

	void CommandBuffer::debugMarkerBegin( VkDebugMarkerMarkerInfoEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pMarkerName,
		};
	}

	void CommandBuffer::debugMarkerEnd()const
	{
		m_label = ashes::nullopt;
	}

	void CommandBuffer::debugMarkerInsert( VkDebugMarkerMarkerInfoEXT const & labelInfo )const
	{
		m_label = DebugLabel
		{
			{ labelInfo.color[0], labelInfo.color[1], labelInfo.color[2], labelInfo.color[3] },
			labelInfo.pMarkerName,
		};
	}

#endif

	void CommandBuffer::doFillVboStrides()const
	{
		// v13.33: rebuild the D3D11 vertex buffer binding from the per-slot Vulkan state and
		// the current pipeline's strides (one IASetVertexBuffers call from slot 0 to the
		// highest bound slot; slots the pipeline does not describe get stride 0).
		auto & state = get( m_state.currentPipeline )->getVertexInputState();
		auto bindingsEnd = state.pVertexBindingDescriptions + state.vertexBindingDescriptionCount;
		// v13.60: RimWorld draws hundreds of times with the same vertex buffers; the binding
		// (4 small vectors) was rebuilt and then copied into every draw command.
		if ( m_state.vbosShared && m_state.vboBuiltVersion == m_state.vboVersion && m_state.vboBuiltState == &state )
		{
			return;
		}
		m_state.vboBuiltVersion = m_state.vboVersion;
		m_state.vboBuiltState = &state;
		m_state.vbos.clear();

		if ( m_state.vboSlots.empty() )
		{
			m_state.vbosShared = std::make_shared< VbosBindingArray const >();
			return;
		}

		VbosBinding binding;
		binding.startIndex = 0u;

		for ( uint32_t slotIndex = 0u; slotIndex < m_state.vboSlots.size(); ++slotIndex )
		{
			auto const & slot = m_state.vboSlots[ slotIndex ];
			auto it = std::find_if( state.pVertexBindingDescriptions
				, bindingsEnd
				, [slotIndex]( ::VkVertexInputBindingDescription const & desc )
				{
					return desc.binding == slotIndex;
				} );
			binding.buffers.push_back( slot.buffer );
			binding.d3dBuffers.push_back( slot.d3dBuffer );
			binding.offsets.push_back( slot.offset );
			binding.strides.push_back( it != bindingsEnd ? it->stride : 0u );
		}

		m_state.vbos.push_back( std::move( binding ) );
		m_state.vbosShared = std::make_shared< VbosBindingArray const >( m_state.vbos );
	}

	// v13.51: cleanup ("remove") actions after a command buffer ran.
	// Every pipeline/push-constant/index-buffer/descriptor-set bind queued a remove() that
	// writes "nothing bound" back into the D3D11 slots it used. A RimWorld frame records
	// hundreds of binds of the same few pipelines and sets, so the same slots were nulled
	// over and over: thousands of redundant D3D11 calls per frame after every submit, plus an
	// insert at the FRONT of the vector for each one (quadratic in the command count).
	// Now: a cleanup whose removeKey() was already queued is skipped (identical effect), and
	// actions are appended and reversed once in end(), which keeps the old run order (last
	// bind undone first). RW_GPU_CLEANUP_DEDUPE=0 restores the old behaviour.
	namespace cleanup
	{
		static bool dedupeEnabled()
		{
			return envflags::cleanupDedupe();
		}
	}

	void CommandBuffer::doAddAfterSubmitAction()const
	{
		auto const & command = *m_commands.back();
		if ( cleanup::dedupeEnabled() )
		{
			if ( auto key = command.removeKey(); key != 0u && !m_afterSubmitKeys.insert( key ).second )
			{
				g_cleanupSkipped.fetch_add( 1u, std::memory_order_relaxed );
				return;
			}
		}
		g_cleanupKept.fetch_add( 1u, std::memory_order_relaxed );
		auto action = [&command]( Context const & context )
			{
				command.remove( context );
			};
		if ( m_afterSubmitRecordingOrder )
		{
			m_afterSubmitActions.emplace_back( std::move( action ) );
		}
		else
		{
			// Recording continued after end() (should not happen in Vulkan); keep old order rule.
			m_afterSubmitActions.emplace( m_afterSubmitActions.begin(), std::move( action ) );
		}
	}

	void CommandBuffer::doFinishAfterSubmitActions()const
	{
		if ( m_afterSubmitRecordingOrder )
		{
			std::reverse( m_afterSubmitActions.begin(), m_afterSubmitActions.end() );
			m_afterSubmitRecordingOrder = false;
		}
		m_afterSubmitKeys.clear();
	}

	void CommandBuffer::doProcessMappedBoundDescriptorResourcesIn( VkDescriptorSet descriptor
		, ArrayView< uint32_t const > const & dynamicOffsets
		, std::vector< uint8_t > const * skipDynamic
		, std::vector< uint8_t > const * skipStatic )const
	{
		// v13.28: refresh the range the shader will actually read: descriptor offset PLUS the
		// dynamic offset of this bind. The dynamic offset was ignored, so every dynamic
		// uniform/storage buffer bind refreshed only the first element; per-draw material
		// data at other offsets (Waylith: one 2 MB material buffer, a different dynamic
		// offset per draw) stayed stale. Pawns read ColorTwo = 0 and were discarded.
		// Same offset order as BindDescriptorSetCommand (binding by binding, element order).
		size_t offsetIndex = 0u;
		for ( auto & writes : get( descriptor )->getDynamicBuffers() )
		{
			for ( auto const & write : writes->writes )
			{
				auto infos = makeArrayView( write.pBufferInfo, write.descriptorCount );
				for ( uint32_t i = 0u; i < write.descriptorCount; ++i )
				{
					auto const & info = infos[i];
					size_t const index = offsetIndex + write.dstArrayElement + i;
					VkDeviceSize const dyn = index < dynamicOffsets.size() ? dynamicOffsets[index] : 0u;
					if ( skipDynamic && index < skipDynamic->size() && ( *skipDynamic )[index] ) continue;   // v13.74: served from the uniform ring
					doProcessMappedBoundResourceIn( info.buffer, info.offset + dyn, info.range );
				}
			}
			offsetIndex += writes->binding.descriptorCount;
		}

		for ( auto & writes : get( descriptor )->getStorageBuffers() )
		{
			for ( auto const & write : writes->writes )
			{
				for ( auto & info : makeArrayView( write.pBufferInfo, write.descriptorCount ) )
				{
					doProcessMappedBoundResourceIn( info.buffer, info.offset, info.range );
				}
			}
		}
		
		for ( auto & writes : get( descriptor )->getStorageTextures() )
		{
			for ( auto const & write : writes->writes )
			{
				for ( auto & info : makeArrayView( write.pImageInfo, write.descriptorCount ) )
				{
					doProcessMappedBoundResourceIn( info.imageView );
				}
			}
		}

		size_t staticElement = 0u;   // v13.75: same order as BindDescriptorSetCommand's ring cursor
		for ( auto & writes : get( descriptor )->getUniformBuffers() )
		{
			for ( auto const & write : writes->writes )
			{
				for ( auto & info : makeArrayView( write.pBufferInfo, write.descriptorCount ) )
				{
					size_t const element = staticElement++;
					if ( skipStatic && element < skipStatic->size() && ( *skipStatic )[element] ) continue;   // served from the uniform ring
					doProcessMappedBoundResourceIn( info.buffer, info.offset, info.range );
				}
			}
		}
	}

	void CommandBuffer::doProcessMappedBoundDescriptorsResourcesOut( VkPipeline pipeline )const
	{
		// v13.23: download (GPU->CPU copy, a full stall per buffer) only storage resources
		// the current pipeline can actually write. Waylith's instanced draws bind their
		// per-instance data as read-only storage buffers; every one was downloaded after
		// every draw (~190 stalls, ~0.5 s per map frame on the HD 5500).
		static bool const downloadAll = []()
		{
			char text[4]{};
			auto length = GetEnvironmentVariableA( "RW_GPU_DOWNLOAD_ALL", text, DWORD( sizeof( text ) ) );
			return length == 1u && text[0] == '1';
		}();
		auto canWrite = [&]( uint32_t binding )
		{
			return downloadAll || !pipeline || get( pipeline )->writesStorageBinding( binding );
		};
		for ( auto descriptor : m_state.boundDescriptors )
		{
			for ( auto & writes : get( descriptor )->getDynamicStorageBuffers() )
			{
				for ( auto const & write : writes->writes )
				{
					if ( !canWrite( write.dstBinding ) ) continue;
					for ( auto & info : makeArrayView( write.pBufferInfo, write.descriptorCount ) )
					{
						doProcessMappedBoundResourceOut( info.buffer, info.offset, info.range );
					}
				}
			}

			for ( auto & writes : get( descriptor )->getStorageBuffers() )
			{
				for ( auto const & write : writes->writes )
				{
					if ( !canWrite( write.dstBinding ) ) continue;
					for ( auto & info : makeArrayView( write.pBufferInfo, write.descriptorCount ) )
					{
						doProcessMappedBoundResourceOut( info.buffer, info.offset, info.range );
					}
				}
			}

			for ( auto & writes : get( descriptor )->getStorageTextures() )
			{
				for ( auto const & write : writes->writes )
				{
					if ( !canWrite( write.dstBinding ) ) continue;
					for ( auto & info : makeArrayView( write.pImageInfo, write.descriptorCount ) )
					{
						doProcessMappedBoundResourceOut( info.imageView );
					}
				}
			}
		}
	}

	void CommandBuffer::doProcessMappedBoundVaoBuffersIn()const
	{
		for ( auto const & vbo : m_state.vbos )
		{
			for ( auto & buffer : vbo.buffers )
			{
				if ( !buffer ) continue;
				doProcessMappedBoundResourceIn( buffer
					, 0u
					, get( buffer )->getSize() );
			}
		}
	}

	void CommandBuffer::doProcessMappedBoundIndexBufferIn()const
	{
		if ( m_state.boundIbo )
			doProcessMappedBoundResourceIn( m_state.boundIbo, 0u, get( m_state.boundIbo )->getSize() );
	}
	void CommandBuffer::doProcessMappedBoundResourceIn( VkBuffer buffer
		, VkDeviceSize offset
		, VkDeviceSize range )const
	{
		auto buf = get( buffer );
		auto parent = get( buf->getMemory() );
		// Record the use even when mapping happens after recording.  At execution,
		// only a currently mapped host-visible child is refreshed; GPU-only data is untouched.
		if ( checkFlag( parent->getMemoryPropertyFlags(), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) )
		{
			// v13.60: the same buffer range is recorded for hundreds of draws per command buffer
			// (~3,400 upload commands, ~95% skipped at execution by the per-execution dedupe of
			// v13.24). An exact repeat in the same recording is now not recorded at all: the
			// execution-time dedupe would skip it anyway (same scope: one command buffer).
			// RW_GPU_UPLOAD_DEDUPE=0 switches both off.
			bool const dedupe = envflags::uploadDedupe();
			auto const * memory = &buf->getObjectMemory();
			if ( dedupe )
			{
				uint64_t key = uint64_t( reinterpret_cast< uintptr_t >( memory ) ) * 0x9E3779B97F4A7C15ull;
				key ^= ( uint64_t( offset ) + 0x632BE59BD9B4E019ull ) * 0xBF58476D1CE4E5B9ull;
				key ^= ( uint64_t( range ) + 0x94D049BB133111EBull ) * 0x94D049BB133111EBull;
				// v13.72: a 4-entry "recently recorded" check first - per draw the same 2-3 buffers (vertex,
				// instance, index) repeat, so most calls end here without hashing; then try_emplace,
				// which never allocates a node when the key already exists.
				for ( auto const & recent : m_recentUploads )
				{
					if ( recent.memory == memory && recent.offset == offset && recent.range == range )
					{
						++m_recordedUploadsSkipped;
						return;
					}
				}
				auto [it, inserted] = m_recordedUploads.try_emplace( key, RecordedUpload{ memory, offset, range } );
				m_recentUploads[m_recentUploadsNext++ & 3u] = RecordedUpload{ memory, offset, range };
				if ( !inserted && it->second.memory == memory && it->second.offset == offset && it->second.range == range )
				{
					++m_recordedUploadsSkipped;
					return;
				}
			}
			auto command = std::make_unique< UploadMemoryCommand >( m_device
				, memory, offset, range, 0u, true );
			if ( uploadMerge() && range != VK_WHOLE_SIZE && range < 65536u )
				m_uploadSpans.push_back( UploadSpan{ memory, offset, offset + range, uint32_t( m_uploadSpans.size() ), command.get() } );
			m_commands.emplace_back( std::move( command ) );
		}
	}

	void CommandBuffer::doProcessMappedBoundResourceIn( VkImageView image )const
	{
		auto img = get( get( image )->getImage() );
		auto & range = get( image )->getSubResourceRange();
		auto layerSize = getLevelsSize( img->getDimensions()
			, img->getFormat()
			, 0u
			, img->getMipmapLevels()
			, uint32_t( img->getMemoryAlignment() ) );
		auto size = getLevelsSize( img->getDimensions()
			, img->getFormat()
			, range.baseMipLevel
			, range.levelCount
			, uint32_t( img->getMemoryAlignment() ) );
		auto offset = img->getMemoryOffset();

		for ( auto layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer )
		{
			doAddMappedResource( &img->getObjectMemory()
				, offset
				, size
				, get( image )->getSubresource( layer )
				, true );
			offset += layerSize;
		}
	}

	void CommandBuffer::doProcessMappedBoundResourceOut( VkBuffer buffer
		, VkDeviceSize offset
		, VkDeviceSize range )const
	{
		auto buf = get( buffer );

		if ( get( buf->getMemory() )->isMapped() )
		{
			doAddMappedResource( &buf->getObjectMemory()
				, offset
				, range
				, 0u
				, false );
		}
	}

	void CommandBuffer::doProcessMappedBoundResourceOut( VkImageView image )const
	{
		auto img = get( get( image )->getImage() );
		auto & range = get( image )->getSubResourceRange();
		auto layerSize = getLevelsSize( img->getDimensions()
			, img->getFormat()
			, 0u
			, img->getMipmapLevels()
			, uint32_t( img->getMemoryAlignment() ) );
		auto size = getLevelsSize( img->getDimensions()
			, img->getFormat()
			, range.baseMipLevel
			, range.levelCount
			, uint32_t( img->getMemoryAlignment() ) );
		auto offset = img->getMemoryOffset();

		for ( auto layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer )
		{
			doAddMappedResource( &img->getObjectMemory()
				, offset
				, size
				, get( image )->getSubresource( layer )
				, false );
			offset += layerSize;
		}
	}

	CommandBuffer::ResourceIndex & CommandBuffer::doAddMappedResource( ObjectMemory const * memory
		, VkDeviceSize offset
		, VkDeviceSize range
		, UINT subresource
		, bool isInput )const
	{
		if ( isInput )
		{
			m_commands.emplace_back( std::make_unique< UploadMemoryCommand >( m_device
				, memory
				, offset
				, range
				, subresource ) );
		}
		else
		{
			m_commands.emplace_back( std::make_unique< DownloadMemoryCommand >( m_device
				, memory
				, offset
				, range
				, subresource ) );
		}

		return doAddMappedResource( memory );
	}

	CommandBuffer::ResourceIndex & CommandBuffer::doAddMappedResource( ObjectMemory const * memory )const
	{
		auto it = std::find_if( m_mappedResources.begin()
			, m_mappedResources.end()
			, [memory]( ResourceIndex const & lookup )
		{
			return lookup.memory == memory;
		} );

		ResourceIndex * result{ nullptr };

		if ( it == m_mappedResources.end() )
		{
			result = &m_mappedResources.emplace_back( memory
				, m_commands.size() - 1u
				, get( memory->deviceMemory )->onDestroy.connect( [this, memory]( VkDeviceMemory )
					{
						doRemoveMappedResource( memory );
					} ) );
		}
		else
		{
			result = &( *it );
		}

		return *result;
	}

	void CommandBuffer::doRemoveMappedResource( ObjectMemory const * memory )const
	{
		auto it = std::find_if( m_mappedResources.begin()
			, m_mappedResources.end()
			, [memory]( ResourceIndex const & lookup )
		{
			return lookup.memory == memory;
		} );

		if ( it != m_mappedResources.end() )
		{
			auto index = it->index;
			m_commands.erase( m_commands.begin() + ptrdiff_t( index ) );
			it = m_mappedResources.erase( it );

			while ( it != m_mappedResources.end() )
			{
				it->index--;
				++it;
			}
		}
	}

	BlitPipeline const & CommandBuffer::doGetBlitPipeline( VkFormat src, VkFormat dst )const
	{
		auto hash = cmdbuf::makeHash( src, dst );
		auto it = m_blitPipelines.find( hash );

		if ( it == m_blitPipelines.end() )
		{
			it = m_blitPipelines.emplace( hash
				, std::make_unique< BlitPipeline >( *get( m_device )
					, src
					, dst ) ).first;
		}

		return *it->second;
	}

	void CommandBuffer::doApplyDynamicStates()const
	{
		if ( m_state.currentPipeline )
		{
			get( m_state.currentPipeline )->getDynamicStates().setLineWidth( m_state.lineWidth );
			get( m_state.currentPipeline )->getDynamicStates().setDepthBias( m_state.depthBias );
			get( m_state.currentPipeline )->getDynamicStates().setBlendConstants( m_state.blendConstants );
			get( m_state.currentPipeline )->getDynamicStates().setDepthBounds( m_state.depthBounds );
			get( m_state.currentPipeline )->getDynamicStates().setStencilCompareMask( m_state.stencilCompareMask );
			get( m_state.currentPipeline )->getDynamicStates().setStencilWriteMask( m_state.stencilWriteMask );
			get( m_state.currentPipeline )->getDynamicStates().setStencilReference( m_state.stencilReference );
			get( m_state.currentPipeline )->update();
		}
	}

	void CommandBuffer::doBindDynamicStates()const
	{
		assert( m_state.currentPipeline );

		if ( get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK )
			|| get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_STENCIL_WRITE_MASK )
			|| get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_STENCIL_REFERENCE ) )
		{
			m_commands.emplace_back( std::make_unique< BindDepthStencilStateCommand >( m_device
				, m_state.currentPipeline ) );
		}

		if ( get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_DEPTH_BIAS ) )
		{
			assert( m_state.depthBias );
			m_commands.emplace_back( std::make_unique< BindRasterizerStateCommand >( m_device
				, m_state.currentPipeline ) );
		}

		if ( get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_BLEND_CONSTANTS ) )
		{
			assert( m_state.blendConstants );
			m_commands.emplace_back( std::make_unique< BindBlendStateCommand >( m_device
				, m_state.currentPipeline
				, m_state.blendConstants.value() ) );
		}

		if ( get( m_state.currentPipeline )->hasDynamicStateEnable( VK_DYNAMIC_STATE_DEPTH_BOUNDS ) )
		{
			assert( m_state.depthBounds );
			m_commands.emplace_back( std::make_unique< SetDepthBoundsCommand >( m_device
				, m_state.depthBounds.value() ) );
		}
	}

	//*********************************************************************************************
}
