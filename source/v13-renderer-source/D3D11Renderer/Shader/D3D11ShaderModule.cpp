/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#include <algorithm>
#include "Shader/D3D11ShaderModule.hpp"
#include "Shader/D3D11ShaderPrewarm.hpp"

#include "Core/D3D11Device.hpp"
#include "Shader/D3D11ShaderBytecodeCache.hpp"

#if D3D11Renderer_USE_SPIRV_CROSS
#	include "spirv_cpp.hpp"
#	include "spirv_cross_util.hpp"
#	include "spirv_hlsl.hpp"
#endif

#include "ashesd3d11_api.hpp"

#include <atomic>
#include <cstdio>

#pragma warning( push )
#pragma warning( disable: 4191 )
#pragma warning( disable: 4365 )
#pragma warning( disable: 5204 )
#pragma warning( disable: 5262 )
#if defined( _UNICODE )
#	define CreateEvent CreateEventW
#else
#	define CreateEvent CreateEventA
#endif
#include <wrl/client.h>
#undef CreateEvent

#include <iostream>
#include <locale>
#include <regex>
#include <sstream>
#pragma warning( pop )

#include <d3dcompiler.h>

namespace ashes::D3D11_NAMESPACE
{
	//*************************************************************************

	namespace shader
	{
		inline uint32_t constexpr OpCodeSPIRV = 0x07230203;

		struct BlockLocale
			: public NonCopyable
		{
			BlockLocale()
			{
				m_prvLoc = std::locale( "" );

				if ( m_prvLoc.name() != "C" )
				{
					std::locale::global( std::locale{ "C" } );
				}
			}

			~BlockLocale()noexcept
			{
				try
				{
					if ( m_prvLoc.name() != "C" )
					{
						std::locale::global( m_prvLoc );
					}
				}
				catch ( ... )
				{
					// Nothing to do here
				}
			}

		private:
			std::locale m_prvLoc;
		};

#if D3D11Renderer_USE_SPIRV_CROSS

		static spv::ExecutionModel getExecutionModel( VkShaderStageFlagBits stage )
		{
			spv::ExecutionModel result{};

			switch ( stage )
			{
			case VK_SHADER_STAGE_VERTEX_BIT:
				result = spv::ExecutionModelVertex;
				break;
			case VK_SHADER_STAGE_GEOMETRY_BIT:
				result = spv::ExecutionModelGeometry;
				break;
			case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
				result = spv::ExecutionModelTessellationControl;
				break;
			case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
				result = spv::ExecutionModelTessellationEvaluation;
				break;
			case VK_SHADER_STAGE_FRAGMENT_BIT:
				result = spv::ExecutionModelFragment;
				break;
			case VK_SHADER_STAGE_COMPUTE_BIT:
				result = spv::ExecutionModelGLCompute;
				break;
			default:
				assert( false && "Unsupported shader stage flag" );
				break;
			}

			return result;
		}

		static void doFillConstant( VkSpecializationInfo const & specialisationInfo
			, VkSpecializationMapEntry const & entry
			, spirv_cross::SPIRType const & type
			, spirv_cross::SPIRConstant & constant )
		{
			auto offset = entry.offset;
			auto entrySize = uint32_t( entry.size / ( size_t( type.columns ) * type.vecsize ) );

			for ( auto col = 0u; col < type.columns; ++col )
			{
				for ( auto vec = 0u; vec < type.vecsize; ++vec )
				{
					std::memcpy( &constant.m.c[col].r[vec]
						, reinterpret_cast< uint8_t const * >( specialisationInfo.pData ) + offset
						, entrySize );
					offset += entrySize;
				}
			}
		}

		static void doProcessSpecializationConstants( VkPipelineShaderStageCreateInfo const & state
			, spirv_cross::CompilerHLSL & compiler )
		{
			if ( state.pSpecializationInfo )
			{
				auto constants = compiler.get_specialization_constants();

				for ( auto & spec : makeArrayView( state.pSpecializationInfo->pMapEntries, state.pSpecializationInfo->mapEntryCount ) )
				{
					auto it = std::find_if( constants.begin()
						, constants.end()
						, [&spec]( spirv_cross::SpecializationConstant const & lookup )
						{
							return lookup.constant_id == spec.constantID;
						} );

					if ( it != constants.end() )
					{
						auto & constant = compiler.get_constant( it->id );
						auto & type = compiler.get_type( constant.constant_type );
						doFillConstant( *state.pSpecializationInfo
							, spec
							, type
							, constant );
					}
				}
			}
		}

		static void doSetEntryPoint( VkShaderStageFlagBits stage
			, spirv_cross::CompilerHLSL & compiler )
		{
			auto model = getExecutionModel( stage );
			std::string entryPoint;

			for ( auto const & e : compiler.get_entry_points_and_stages() )
			{
				if ( entryPoint.empty() && e.execution_model == model )
				{
					entryPoint = e.name;
				}
			}

			if ( entryPoint.empty() )
			{
				throw ashes::BaseException{ "Could not find an entry point." };
			}

			compiler.set_entry_point( entryPoint, model );
		}

		static void doSetupOptions( spirv_cross::CompilerHLSL & compiler )
		{
			spirv_cross::CompilerGLSL::Options options = compiler.get_common_options();
			options.es = false;
			options.separate_shader_objects = true;
			options.enable_420pack_extension = true;
			options.vertex.fixup_clipspace = false;
			options.vertex.flip_vert_y = true;
			options.vertex.support_nonzero_base_instance = true;
			compiler.set_common_options( options );
		}

		static void doSetupHlslOptions( spirv_cross::CompilerHLSL & compiler )
		{
			spirv_cross::CompilerHLSL::Options hlslOptions = compiler.get_hlsl_options();
			hlslOptions.shader_model = 50;
			hlslOptions.point_coord_compat = true;
			hlslOptions.point_size_compat = true;
			// v13.15: read-only SSBOs would otherwise become ByteAddressBuffer SRVs (t#),
			// which the binding side never sets; the bridge binds every storage buffer
			// as a UAV, so make the shader agree.
			hlslOptions.force_storage_buffer_as_uav = true;
			compiler.set_hlsl_options( hlslOptions );
		}

		static void reportMissingBinding( VkShaderModule shaderModule
			, std::string const & typeName
			, uint32_t binding
			, uint32_t set )
		{
			assert( false );
			std::stringstream stream;
			stream.imbue( std::locale{ "C" } );
			stream << typeName << ", binding=" << binding << ", set=" << set;
			reportError( shaderModule
				, VK_ERROR_VALIDATION_FAILED_EXT
				, "Missing binding"
				, stream.str() );
		}

		static void doReworkBindings( VkShaderModule shaderModule
			, std::string const & typeName
			, spirv_cross::CompilerGLSL & compiler
			, spirv_cross::SmallVector< spirv_cross::Resource > const & resources
			, ShaderBindingMap const & bindings
			, bool failOnError
			, ShaderBindingMap const * fallback = nullptr )
		{
			for ( auto const & obj : resources )
			{
				auto binding = compiler.get_decoration( obj.id, spv::DecorationBinding );
				auto set = compiler.get_decoration( obj.id, spv::DecorationDescriptorSet );
				compiler.unset_decoration( obj.id, spv::DecorationDescriptorSet );
				auto it = bindings.find( makeShaderBindingKey( set, binding ) );

				if ( it != bindings.end() )
				{
					compiler.set_decoration( obj.id, spv::DecorationBinding, it->second );
				}
				else if ( fallback )
				{
					it = fallback->find( makeShaderBindingKey( set, binding ) );

					if ( it != fallback->end() )
					{
						compiler.set_decoration( obj.id, spv::DecorationBinding, it->second );
					}
					else if ( failOnError )
					{
						reportMissingBinding( shaderModule, typeName, binding, set );
					}
				}
				else if ( failOnError )
				{
					reportMissingBinding( shaderModule, typeName, binding, set );
				}
			}
		}

		static void doReworkUavBindings( VkShaderModule shaderModule
			, std::string const & typeName
			, spirv_cross::CompilerGLSL & compiler
			, spirv_cross::SmallVector< spirv_cross::Resource > const & resources
			, ShaderBindingMap const & bindings
			, uint32_t uavStart
			, bool failOnError
			, ShaderBindingMap const * fallback = nullptr )
		{
			for ( auto const & obj : resources )
			{
				auto binding = compiler.get_decoration( obj.id, spv::DecorationBinding );
				auto set = compiler.get_decoration( obj.id, spv::DecorationDescriptorSet );
				compiler.unset_decoration( obj.id, spv::DecorationDescriptorSet );
				auto it = bindings.find( makeShaderBindingKey( set, binding ) );

				if ( it != bindings.end() )
				{
					// UAV and fragment outputs share the same namespace,
					// hence we add UAV start offset (which is just the outputs count).
					compiler.set_decoration( obj.id, spv::DecorationBinding, uavStart + it->second );
				}
				else if ( fallback )
				{
					it = fallback->find( makeShaderBindingKey( set, binding ) );

					if ( it != fallback->end() )
					{
						// Fallback is used only for storage texel buffers,
						// which are handled the same way as uniform texel buffers,
						// hence we don't add the UAV start offset.
						compiler.set_decoration( obj.id, spv::DecorationBinding, it->second );
					}
					else if ( failOnError )
					{
						reportMissingBinding( shaderModule, typeName, binding, set );
					}
				}
				else if ( failOnError )
				{
					reportMissingBinding( shaderModule, typeName, binding, set );
				}
			}
		}

		static void doReworkOutputs( spirv_cross::CompilerGLSL const & compiler
			, spirv_cross::SmallVector< spirv_cross::Resource > const & resources
			, uint32_t & index )
		{
			for ( auto const & obj : resources )
			{
				auto location = compiler.get_decoration( obj.id, spv::DecorationLocation );
				index = std::max( index, location );
			}
		}

		// v13.15: D3D11 UAV slots are shared with the render targets of the whole
		// graphics pipeline (OMSetRenderTargetsAndUnorderedAccessViews), so a shader's
		// u-registers must start after the subpass' colour attachments, in every stage.
		// Pipeline sets this before compiling its stages (0 for compute).
		thread_local uint32_t g_uavRegisterStart = 0u;
		thread_local uint64_t g_writtenStorageMask = 0u;
		thread_local bool g_writtenStorageUnknown = false;
		thread_local std::vector< uint32_t > const * g_fragmentInputs = nullptr;   // v13.35

		// v13.35: mask vertex outputs the fragment shader does not read (see the header).
		static void doMaskUnreadVertexOutputs( VkShaderStageFlagBits stage
			, spirv_cross::CompilerGLSL & compiler )
		{
			if ( stage != VK_SHADER_STAGE_VERTEX_BIT || !g_fragmentInputs ) return;
			static bool const enabled = []()
			{
				auto value = std::getenv( "RW_GPU_VARYING_MASK" );
				return !( value && value[0] == '0' );
			}();
			if ( !enabled ) return;
			auto isRead = [&]( uint32_t loc )
			{
				return std::find( g_fragmentInputs->begin(), g_fragmentInputs->end(), loc ) != g_fragmentInputs->end();
			};
			auto resources = compiler.get_shader_resources();
			for ( auto const & out : resources.stage_outputs )
			{
				if ( !compiler.has_decoration( out.id, spv::DecorationLocation ) ) continue;
				auto const location = compiler.get_decoration( out.id, spv::DecorationLocation );
				auto const & type = compiler.get_type( out.base_type_id );
				if ( type.basetype == spirv_cross::SPIRType::Struct )
				{
					// v13.37: a struct output (Slang/ImGui `struct { Color; UV } Out` at location 0)
					// covers one location per member. Mark it as an interface Block so SPIRV-Cross
					// emits the members separately (each with its own semantic, sorted by location),
					// then move the unread members behind the read ones exactly like loose outputs.
					compiler.set_decoration( type.self, spv::DecorationBlock );
					uint32_t next = location;
					for ( uint32_t m = 0u; m < uint32_t( type.member_types.size() ); ++m )
					{
						uint32_t memberLoc = compiler.has_member_decoration( type.self, m, spv::DecorationLocation )
							? compiler.get_member_decoration( type.self, m, spv::DecorationLocation )
							: next;
						auto const & mt = compiler.get_type( type.member_types[ m ] );
						uint32_t span = 1u;
						for ( auto dim : mt.array ) span *= std::max( 1u, dim );
						if ( mt.columns > 1u ) span *= mt.columns;
						next = memberLoc + span;
						bool read = false;
						for ( uint32_t k = 0u; k < span; ++k ) read = read || isRead( memberLoc + k );
						compiler.set_member_decoration( type.self, m, spv::DecorationLocation, read ? memberLoc : 32u + memberLoc );
					}
					continue;
				}
				uint32_t span = 1u;
				for ( auto dim : type.array ) span *= std::max( 1u, dim );
				if ( type.columns > 1u ) span *= type.columns;
				bool read = false;
				for ( uint32_t k = 0u; k < span; ++k ) read = read || isRead( location + k );
				if ( !read )
				{
					// SPIRV-Cross's HLSL backend ignores mask_stage_output_by_location, so move the
					// unread output behind all read ones instead: HLSL interface structs are sorted by
					// location and fxc assigns registers in declaration order, so the read outputs now
					// occupy the same registers as the pixel shader's inputs. Extra trailing outputs
					// are legal in D3D11 linkage.
					compiler.set_decoration( out.id, spv::DecorationLocation, 32u + location );
				}
			}
		}


		static void doCollectWrittenStorage( spirv_cross::CompilerGLSL & compiler )
		{
			auto resources = compiler.get_shader_resources();
			auto note = [&compiler]( spirv_cross::Resource const & res, bool writable )
			{
				if ( !writable ) return;
				auto binding = compiler.get_decoration( res.id, spv::DecorationBinding );
				if ( binding < 64u ) g_writtenStorageMask |= ( 1ull << binding );
				else g_writtenStorageUnknown = true;
			};
			for ( auto const & res : resources.storage_buffers )
			{
				auto flags = compiler.get_buffer_block_flags( res.id );
				note( res, !flags.get( spv::DecorationNonWritable ) );
			}
			for ( auto const & res : resources.storage_images )
			{
				note( res, !compiler.has_decoration( res.id, spv::DecorationNonWritable ) );
			}
		}

		static void doReworkBindings( VkPipelineLayout pipelineLayout
			, VkPipelineCreateFlags createFlags
			, VkShaderModule shaderModule
			, VkShaderStageFlagBits stage
			, spirv_cross::CompilerGLSL & compiler )
		{
			spirv_cross::ShaderResources resources = compiler.get_shader_resources();
			auto & bindings = get( pipelineLayout )->getShaderBindings();
			auto failOnError = !( checkFlag( createFlags, VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT )
				|| checkFlag( createFlags, VK_PIPELINE_CREATE_DERIVATIVE_BIT ) );
			doReworkBindings( shaderModule, "UniformBuffer", compiler, resources.uniform_buffers, bindings.ubo, failOnError );
			doReworkBindings( shaderModule, "CombinedSamplerImage", compiler, resources.sampled_images, bindings.tex, failOnError, &bindings.tbo );
			doReworkBindings( shaderModule, "SampledImage", compiler, resources.separate_images, bindings.tex, failOnError );
			doReworkBindings( shaderModule, "Sampler", compiler, resources.separate_samplers, bindings.tex, failOnError );
			doReworkBindings( shaderModule, "SubpassInput", compiler, resources.subpass_inputs, bindings.tex, failOnError );

			// v13.15: was "max fragment output location" (0 for a single output, and 0 in
			// vertex shaders), which made u0 collide with render target 0, so vertex-shader
			// storage buffers (RimWorld's instanced pawns/flecks) read nothing.
			uint32_t uav = g_uavRegisterStart;
			static_cast< void >( stage );

			// v13.17: push-constant blocks get a fixed register, b13 (the last D3D11
			// constant-buffer slot), in every stage. Without a register the HLSL compiler
			// picked the first free slot (e.g. b1 in a VS that declares only b0), which
			// can also be the slot a descriptor-set uniform buffer of the same pipeline
			// layout is bound to for that stage; whichever was bound last won. Waylith's
			// mesh shaders take the model matrix from push constants, so pawns, animals
			// and trees collapsed. CompiledShaderModule reflects the bind point, so the
			// push-constant buffer follows automatically.
			for ( auto const & obj : resources.push_constant_buffers )
			{
				compiler.set_decoration( obj.id, spv::DecorationDescriptorSet, 0u );
				compiler.set_decoration( obj.id, spv::DecorationBinding, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1u );
			}
			for ( auto const & ubo : bindings.ubo )
			{
				if ( ubo.second >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1u )
				{
					static std::atomic< uint32_t > warned{ 0u };
					if ( warned++ < 8u )
					{
						std::fprintf( stderr, "[RW-GPU-PUSHCONST] uniform buffer slot %u collides with the push-constant slot b%u\n"
							, unsigned( ubo.second ), unsigned( D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1u ) );
					}
				}
			}

			doReworkUavBindings( shaderModule, "StorageBuffer", compiler, resources.storage_buffers, bindings.uav, uav, failOnError );
			doReworkUavBindings( shaderModule, "StorageImage", compiler, resources.storage_images, bindings.uav, uav, failOnError, &bindings.ibo );
		}

#endif

		static std::string compileSpvToHlsl( VkPipelineLayout pipelineLayout
			, VkPipelineCreateFlags createFlags
			, VkShaderModule shaderModule
			, UInt32Array const & shader
			, VkShaderStageFlagBits stage
			, VkPipelineShaderStageCreateInfo const & state )
		{
			if ( shader[0] == OpCodeSPIRV )
			{
#if D3D11Renderer_USE_SPIRV_CROSS

				try
				{
					BlockLocale guard;
					spirv_cross::CompilerHLSL compiler{ shader };
					doProcessSpecializationConstants( state, compiler );
					doSetEntryPoint( stage, compiler );
					doSetupOptions( compiler );
					doSetupHlslOptions( compiler );
					doCollectWrittenStorage( compiler );   // v13.23, before bindings are renumbered
					doMaskUnreadVertexOutputs( stage, compiler );   // v13.35
					doReworkBindings( pipelineLayout, createFlags, shaderModule, stage, compiler );
					return compiler.compile();
				}
				catch ( std::exception & exc )
				{
					std::cerr << "SPIR-V to HLSL compilation failed: " << exc.what() << std::endl;
					throw;
				}

#else

				throw ashes::BaseException{ "Can't parse SPIR-V shaders, pull submodule SpirvCross" };

#endif
			}

			std::vector< char > hlslCode( shader.size() * sizeof( uint32_t ) );
			std::memcpy( hlslCode.data(), shader.data(), hlslCode.size() );
			return std::string( hlslCode.data(), hlslCode.data() + strnlen( hlslCode.data(), hlslCode.size() ) );
		}

		static uint32_t extractLocation( InputElementDesc const & desc )
		{
			uint32_t result{ 0u };

			if ( desc.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA )
			{
				std::string name = desc.SemanticName;
				name = name.substr( 0u, name.size() - 1u );
				auto index = name.find_last_not_of( "0123456789" );
				name = name.substr( index + 1 );
				result = uint32_t( std::stoi( name ) );
			}

			return result;
		}

		static void normalise( InputElementDesc & desc )
		{
			if ( desc.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA )
			{
				desc.BaseSemanticIndex = desc.SemanticIndex + extractLocation( desc );
			}
		}
	}

	//*************************************************************************

	CompiledShaderModule::CompiledShaderModule( CompiledShaderModule && rhs )noexcept
		: m_shader{ std::move( rhs.m_shader ) }
		, m_stage{ std::move( rhs.m_stage ) }
		, m_source{ std::move( rhs.m_source ) }
		, m_compiled{ std::move( rhs.m_compiled ) }
		, m_layout{ std::move( rhs.m_layout ) }
	{
		m_layout.shaderModule = this;
		rhs.m_shader.compute = nullptr;
		rhs.m_compiled = nullptr;
		rhs.m_layout.shaderModule = nullptr;
	}

	CompiledShaderModule & CompiledShaderModule::operator=( CompiledShaderModule && rhs )noexcept
	{
		if ( this != &rhs )
		{
			m_shader = std::move( rhs.m_shader );
			m_stage = std::move( rhs.m_stage );
			m_source = std::move( rhs.m_source );
			m_compiled = std::move( rhs.m_compiled );
			m_layout = std::move( rhs.m_layout );
			m_layout.shaderModule = this;
			rhs.m_shader.compute = nullptr;
			rhs.m_compiled = nullptr;
			rhs.m_layout.shaderModule = nullptr;
		}

		return *this;
	}

	CompiledShaderModule::CompiledShaderModule( VkDevice device
		, VkPipelineLayout pipelineLayout
		, VkPipelineCreateFlags createFlags
		, VkShaderModule shaderModule
		, UInt32Array const & spv
		, VkPipelineShaderStageCreateInfo const & state )
		: m_stage{ state.stage }
	{
		static std::map< D3D_FEATURE_LEVEL, std::map< VkShaderStageFlagBits, std::string > > Profiles
		{
			{ 
				D3D_FEATURE_LEVEL_11_1,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_5_0" },
					{ VK_SHADER_STAGE_GEOMETRY_BIT, "gs_5_0" },
					{ VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "hs_5_0" },
					{ VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "ds_5_0" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_5_0" },
					{ VK_SHADER_STAGE_COMPUTE_BIT, "cs_5_0" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_11_0,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_5_0" },
					{ VK_SHADER_STAGE_GEOMETRY_BIT, "gs_5_0" },
					{ VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "hs_5_0" },
					{ VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "ds_5_0" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_5_0" },
					{ VK_SHADER_STAGE_COMPUTE_BIT, "cs_5_0" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_10_1,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_4_1" },
					{ VK_SHADER_STAGE_GEOMETRY_BIT, "gs_4_1" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_4_1" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_10_0,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_4_0" },
					{ VK_SHADER_STAGE_GEOMETRY_BIT, "gs_4_0" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_4_0" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_9_3,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_3_0" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_3_0" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_9_2,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_2_0" },
					{ VK_SHADER_STAGE_FRAGMENT_BIT, "ps_2_0" },
				}
			},
			{ 
				D3D_FEATURE_LEVEL_9_1,
				{
					{ VK_SHADER_STAGE_VERTEX_BIT, "vs_1_1" },
				}
			}
		};

		// v13.64: time the three parts of making a shader (translation, bytecode, driver object).
		static LARGE_INTEGER const timeFreq = []() { LARGE_INTEGER f{}; QueryPerformanceFrequency( &f ); return f; }();
		auto msSince = []( LARGE_INTEGER const & from ) { LARGE_INTEGER now{}; QueryPerformanceCounter( &now ); return timeFreq.QuadPart ? double( now.QuadPart - from.QuadPart ) * 1000.0 / double( timeFreq.QuadPart ) : 0.0; };
		LARGE_INTEGER timeStart{};
		QueryPerformanceCounter( &timeStart );
		m_source = shader::compileSpvToHlsl( pipelineLayout
			, createFlags
			, shaderModule
			, spv
			, m_stage
			, state );
		// v13.15 diagnostics: RW_GPU_DUMP_HLSL=<dir> writes every generated HLSL program
		// (and the D3DCompile errors) as <dir>\\shader-<n>-<stage>.hlsl.
		int dumpIndex = -1;
		{
			static char dumpDir[512] = {};
			static bool const dumpEnabled = ::GetEnvironmentVariableA( "RW_GPU_DUMP_HLSL", dumpDir, DWORD( sizeof( dumpDir ) ) ) > 0u;
			if ( dumpEnabled )
			{
				static std::atomic< uint32_t > counter{ 0u };
				char const * stageName = m_stage == VK_SHADER_STAGE_VERTEX_BIT ? "vs" : m_stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "ps" : m_stage == VK_SHADER_STAGE_COMPUTE_BIT ? "cs" : "xs";
				char path[640];
				dumpIndex = int( counter++ );
				std::snprintf( path, sizeof( path ), "%s\\shader-%04u-%s.hlsl", dumpDir, unsigned( dumpIndex ), stageName );
				if ( auto f = std::fopen( path, "wb" ) ) { std::fwrite( m_source.data(), 1u, m_source.size(), f ); std::fclose( f ); }
			}
		}
		std::string profile = Profiles[get( device )->getFeatureLevel()][m_stage];
		assert( !profile.empty() && "Unsupported shader stage for currently supported feature level" );
		Microsoft::WRL::ComPtr< ID3DBlob > errors;
		UINT flags = 0;

#if !defined( NDEBUG )
		flags |= D3DCOMPILE_DEBUG;
#endif

		// v12: identical compiler input yields identical bytecode; reuse it.
		auto cacheKey = shadercache::enabled()
			? shadercache::makeKey( m_source, state.pName, profile.c_str(), flags )
			: std::string{};
		double const translateMs = msSince( timeStart );
		LARGE_INTEGER timeBytecode{};
		QueryPerformanceCounter( &timeBytecode );
		char const * bytecodeSource = "compile";
		m_cacheKey = cacheKey;
		HRESULT hr = S_OK;
		if ( auto cached = cacheKey.empty() ? nullptr : shadercache::find( cacheKey ) )
		{
			m_compiled = cached;
			bytecodeSource = "memory";
		}
		else if ( auto onDisk = cacheKey.empty() ? nullptr : shadercache::diskFind( cacheKey ) )
		{
			// v13.22: compiled in an earlier run; no D3DCompile on this launch.
			m_compiled = onDisk;
			bytecodeSource = "disk";
			shadercache::insert( std::move( cacheKey ), m_compiled );
		}
		else
		{
			LARGE_INTEGER t0{}, t1{}, freq{};
			QueryPerformanceFrequency( &freq );
			QueryPerformanceCounter( &t0 );
			hr = D3DCompile( m_source.c_str()
				, UINT( m_source.size() )
				, nullptr
				, nullptr
				, nullptr
				, state.pName
				, profile.c_str()
				, flags
				, 0
				, &m_compiled
				, errors.GetAddressOf() );
			QueryPerformanceCounter( &t1 );
			double const ms = freq.QuadPart ? double( t1.QuadPart - t0.QuadPart ) * 1000.0 / double( freq.QuadPart ) : 0.0;
			shadercache::noteCompile( ms );
			if ( ms > 250.0 )
			{
				std::fprintf( stderr, "[RW-GPU-PROF] slow shader compile %.0f ms (%zu bytes HLSL, %s)\n", ms, m_source.size(), profile.c_str() );
			}
			if ( hr == S_OK && m_compiled && !cacheKey.empty() )
			{
				shadercache::diskStore( cacheKey, m_compiled );
				shadercache::insert( std::move( cacheKey ), m_compiled );
			}
		}

		double const bytecodeMs = msSince( timeBytecode );
		if ( hr == S_OK )
		{
			LARGE_INTEGER timeCreate{};
			QueryPerformanceCounter( &timeCreate );
			bool const prewarmed = doRetrieveShader( device );
			double const createMs = msSince( timeCreate );
			shadercache::noteModule( translateMs, bytecodeMs, createMs, prewarmed );
			static std::atomic< uint32_t > timeLines{ 0u };
			if ( timeLines++ < 96u )
			{
				std::fprintf( stderr, "[RW-GPU-SHADER-TIME] %s translate=%.1fms bytecode=%.1fms (%s) create=%.1fms%s\n"
					, profile.c_str(), translateMs, bytecodeMs, bytecodeSource, createMs, prewarmed ? " (prewarmed object)" : "" );
			}
			if ( dumpIndex >= 0 )
			{
				// v13.23: link the D3D shader object to its dump file, so the frame trace's
				// vs=/ps= pointers can be matched with hlsl\shader-NNNN-*.hlsl.
				std::fprintf( stderr, "[RW-GPU-HLSL] shader-%04d ptr=%p\n", dumpIndex, static_cast< void * >( m_shader.vertex ) );
			}
			m_layout = doRetrieveShaderDesc( device );

			if ( errors )
			{
				std::stringstream stream;
				stream << reinterpret_cast< char * >( errors->GetBufferPointer() ) << std::endl;
				stream << m_source << std::endl;
				reportInfo( shaderModule
					, "Shader compilation succeeded with messages"
					, stream.str() );
			}
		}
		else if ( errors )
		{
			std::stringstream stream;
			stream << reinterpret_cast< char * >( errors->GetBufferPointer() ) << std::endl;
			stream << m_source << std::endl;
			reportError( shaderModule
				, VK_ERROR_VALIDATION_FAILED_EXT
				, "Shader compilation failed"
				, stream.str() );
		}
	}

	CompiledShaderModule::~CompiledShaderModule()noexcept
	{
		safeRelease( m_compiled );

		switch ( m_stage )
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
			safeRelease( m_shader.vertex );
			break;
		case VK_SHADER_STAGE_GEOMETRY_BIT:
			safeRelease( m_shader.geometry );
			break;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			safeRelease( m_shader.hull );
			break;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			safeRelease( m_shader.domain );
			break;
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			safeRelease( m_shader.pixel );
			break;
		case VK_SHADER_STAGE_COMPUTE_BIT:
			safeRelease( m_shader.compute );
			break;
		default:
			break;
		}
	}

	bool CompiledShaderModule::doRetrieveShader( VkDevice device )
	{
		auto dxDevice = get( device )->getDevice();
		HRESULT hr{};
		// v13.64: shader object created in the background from the disk cache?
		if ( auto ready = shaderprewarm::take( dxDevice, m_cacheKey ) )
		{
			void ** target = nullptr;
			IID iid{};
			switch ( m_stage )
			{
			case VK_SHADER_STAGE_VERTEX_BIT: target = reinterpret_cast< void ** >( &m_shader.vertex ); iid = __uuidof( ID3D11VertexShader ); break;
			case VK_SHADER_STAGE_GEOMETRY_BIT: target = reinterpret_cast< void ** >( &m_shader.geometry ); iid = __uuidof( ID3D11GeometryShader ); break;
			case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: target = reinterpret_cast< void ** >( &m_shader.hull ); iid = __uuidof( ID3D11HullShader ); break;
			case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: target = reinterpret_cast< void ** >( &m_shader.domain ); iid = __uuidof( ID3D11DomainShader ); break;
			case VK_SHADER_STAGE_FRAGMENT_BIT: target = reinterpret_cast< void ** >( &m_shader.pixel ); iid = __uuidof( ID3D11PixelShader ); break;
			case VK_SHADER_STAGE_COMPUTE_BIT: target = reinterpret_cast< void ** >( &m_shader.compute ); iid = __uuidof( ID3D11ComputeShader ); break;
			default: break;
			}
			bool const ok = target && SUCCEEDED( ready->QueryInterface( iid, target ) ) && *target;
			ready->Release();
			if ( ok ) return true;
		}

		switch ( m_stage )
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
			hr = dxDevice->CreateVertexShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.vertex );
			dxDebugName( m_shader.vertex, VertexShader );
			break;

		case VK_SHADER_STAGE_GEOMETRY_BIT:
			hr = dxDevice->CreateGeometryShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.geometry );
			dxDebugName( m_shader.geometry, GeometryShader );
			break;

		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			hr = dxDevice->CreateHullShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.hull );
			dxDebugName( m_shader.hull, HullShader );
			break;

		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			hr = dxDevice->CreateDomainShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.domain );
			dxDebugName( m_shader.domain, DomainShader );
			break;

		case VK_SHADER_STAGE_FRAGMENT_BIT:
			hr = dxDevice->CreatePixelShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.pixel );
			dxDebugName( m_shader.pixel, PixelShader );
			break;

		case VK_SHADER_STAGE_COMPUTE_BIT:
			hr = dxDevice->CreateComputeShader( reinterpret_cast< DWORD * >( m_compiled->GetBufferPointer() )
				, m_compiled->GetBufferSize()
				, nullptr
				, &m_shader.compute );
			dxDebugName( m_shader.compute, ComputeShader );
			break;

		default:
			assert( false && "Unsupported shader stage" );
			break;
		}

		checkError( device, hr, "RetrieveShader" );
		return false;
	}

	ShaderDesc CompiledShaderModule::doRetrieveShaderDesc( VkDevice device )
	{
		// Reflect shader info
		Microsoft::WRL::ComPtr< ID3D11ShaderReflection > shaderReflection;
		auto hr = D3DReflect( m_compiled->GetBufferPointer()
			, m_compiled->GetBufferSize()
			, __uuidof( ID3D11ShaderReflection )
			, reinterpret_cast< void ** >( shaderReflection.GetAddressOf() ) );
		checkError( device, hr, "D3DReflect" );

		// Get shader info
		D3D11_SHADER_DESC shaderDesc{};
		shaderReflection->GetDesc( &shaderDesc );
		ShaderDesc result{ this };
		result.inputLayout = doRetrieveInputLayout( shaderReflection.Get(), shaderDesc.InputParameters );
		result.interfaceBlockLayout = doRetrieveInterfaceBlockLayout( shaderReflection.Get(), shaderDesc.ConstantBuffers );
		for ( UINT r = 0u; r < shaderDesc.BoundResources; ++r )
		{
			D3D11_SHADER_INPUT_BIND_DESC bind{};
			if ( FAILED( shaderReflection->GetResourceBindingDesc( r, &bind ) ) ) continue;
			switch ( bind.Type )
			{
			case D3D_SIT_UAV_RWTYPED: case D3D_SIT_UAV_RWSTRUCTURED: case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED: case D3D_SIT_UAV_CONSUME_STRUCTURED: case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				for ( UINT k = 0u; k < std::max( bind.BindCount, 1u ); ++k )
					if ( bind.BindPoint + k < 32u ) result.uavRegisterMask |= 1u << ( bind.BindPoint + k );
				break;
			default:
				break;
			}
		}
		return result;
	}

	InputLayout CompiledShaderModule::doRetrieveInputLayout( ID3D11ShaderReflection * reflection
		, UINT inputParameters )const
	{
		InputLayout result;

		for ( uint32_t i = 0; i < inputParameters; i++ )
		{
			D3D11_SIGNATURE_PARAMETER_DESC paramDesc{};
			reflection->GetInputParameterDesc( i, &paramDesc );

			// fill out input element desc
			InputElementDesc elementDesc{};
			elementDesc.SemanticName = paramDesc.SemanticName;
			elementDesc.SemanticIndex = paramDesc.SemanticIndex;
			elementDesc.BaseSemanticIndex = paramDesc.SemanticIndex;
			elementDesc.InputSlot = paramDesc.Stream;
			elementDesc.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
			elementDesc.InputSlotClass = elementDesc.SemanticName.back() == '_'
				? D3D11_INPUT_PER_INSTANCE_DATA
				: D3D11_INPUT_PER_VERTEX_DATA;
			elementDesc.InstanceDataStepRate = 0;

			// determine DXGI format
			if ( paramDesc.Mask == 1 )
			{
				if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ) elementDesc.Format = DXGI_FORMAT_R32_UINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ) elementDesc.Format = DXGI_FORMAT_R32_SINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ) elementDesc.Format = DXGI_FORMAT_R32_FLOAT;
			}
			else if ( paramDesc.Mask <= 3 )
			{
				if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32_UINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32_SINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ) elementDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
			}
			else if ( paramDesc.Mask <= 7 )
			{
				if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32_UINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32_SINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			}
			else if ( paramDesc.Mask <= 15 )
			{
				if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_SINT;
				else if ( paramDesc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ) elementDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			}

			//save element desc
			shader::normalise( elementDesc );
			result.push_back( elementDesc );
		}

		return result;
	}

	InterfaceBlockLayout CompiledShaderModule::doRetrieveInterfaceBlockLayout( ID3D11ShaderReflection * reflection
		, UINT constantBuffers )const
	{
		InterfaceBlockLayout result;

		for ( UINT i = 0; i < constantBuffers; i++ )
		{
			auto constantBuffer = reflection->GetConstantBufferByIndex( i );
			D3D11_SHADER_BUFFER_DESC sbDesc{ "" };
			constantBuffer->GetDesc( &sbDesc );

			if ( sbDesc.Type == D3D_CT_CBUFFER )
			{
				// v13.17: push_constant blocks are pinned to the last constant-buffer slot
				// (see doReworkBindings); identify them by bind point. The old text search
				// for "cbuffer <name>\n" (declared without register) is kept as a fallback.
				D3D11_SHADER_INPUT_BIND_DESC pushBind{};
				bool const isPushSlot = SUCCEEDED( reflection->GetResourceBindingDescByName( sbDesc.Name, &pushBind ) )
					&& pushBind.BindPoint == D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1u;
				auto it = m_source.find( std::string{ "cbuffer " } + sbDesc.Name + "\n" );

				if ( isPushSlot || it != std::string::npos )
				{
					ConstantBufferDesc constantBufferDesc{};
					constantBufferDesc.name = sbDesc.Name;
					constantBufferDesc.size = sbDesc.Size;

					for ( UINT j = 0; j < sbDesc.Variables; ++j )
					{
						auto variable = constantBuffer->GetVariableByIndex( j );
						D3D11_SHADER_VARIABLE_DESC svDesc{};
						variable->GetDesc( &svDesc );
						ConstantDesc constantDesc{};
						constantDesc.name = svDesc.Name;
						constantDesc.offset = svDesc.StartOffset;
						constantDesc.size = svDesc.Size;
						constantBufferDesc.constants.push_back( constantDesc );
					}

					D3D11_SHADER_INPUT_BIND_DESC sibDesc{};
					reflection->GetResourceBindingDescByName( sbDesc.Name, &sibDesc );
					constantBufferDesc.binding = sibDesc.BindPoint;
					result.push_back( constantBufferDesc );
				}
			}
		}

		return result;
	}

	//*************************************************************************

	ShaderModule::ShaderModule( VkDevice device
		, VkShaderModuleCreateInfo createInfo )
		: m_device{ device }
		, m_createInfo{ std::move( createInfo ) }
		, m_code{ UInt32Array( m_createInfo.pCode, m_createInfo.pCode + ( m_createInfo.codeSize / sizeof( uint32_t ) ) ) }
	{
		m_createInfo.pCode = m_code.data();
	}

	void setUavRegisterStart( uint32_t value )noexcept
	{
#if D3D11Renderer_USE_SPIRV_CROSS
		shader::g_uavRegisterStart = value;
#else
		static_cast< void >( value );
#endif
	}

	void setFragmentInputLocations( std::vector< uint32_t > const * locations )noexcept
	{
#if D3D11Renderer_USE_SPIRV_CROSS
		shader::g_fragmentInputs = locations;
#else
		static_cast< void >( locations );
#endif
	}

	bool collectFragmentInputLocations( VkShaderModule module, std::vector< uint32_t > & locations )
	{
		locations.clear();
#if D3D11Renderer_USE_SPIRV_CROSS
		try
		{
			auto const & code = get( module )->getCode();
			if ( code.empty() || code[0] != shader::OpCodeSPIRV ) return false;
			spirv_cross::Compiler compiler{ code };
			bool found = false;
			for ( auto const & e : compiler.get_entry_points_and_stages() )
			{
				if ( e.execution_model == spv::ExecutionModelFragment )
				{
					compiler.set_entry_point( e.name, e.execution_model );
					found = true;
					break;
				}
			}
			if ( !found ) return false;
			auto resources = compiler.get_shader_resources();
			for ( auto const & in : resources.stage_inputs )
			{
				if ( !compiler.has_decoration( in.id, spv::DecorationLocation ) ) return false;   // unknown layout: mask nothing
				auto const base = compiler.get_decoration( in.id, spv::DecorationLocation );
				auto const & type = compiler.get_type( in.base_type_id );
				auto spanOf = [&]( spirv_cross::SPIRType const & t )
				{
					uint32_t span = 1u;
					for ( auto dim : t.array ) span *= std::max( 1u, dim );
					if ( t.columns > 1u ) span *= t.columns;
					return span;
				};
				if ( type.basetype == spirv_cross::SPIRType::Struct )
				{
					uint32_t next = base;
					for ( uint32_t m = 0u; m < uint32_t( type.member_types.size() ); ++m )
					{
						uint32_t const memberLoc = compiler.has_member_decoration( type.self, m, spv::DecorationLocation )
							? compiler.get_member_decoration( type.self, m, spv::DecorationLocation ) : next;
						auto const span = spanOf( compiler.get_type( type.member_types[ m ] ) );
						for ( uint32_t k = 0u; k < span; ++k ) locations.push_back( memberLoc + k );
						next = memberLoc + span;
					}
				}
				else
				{
					for ( uint32_t k = 0u; k < spanOf( type ); ++k ) locations.push_back( base + k );
				}
			}
			return true;
		}
		catch ( std::exception & )
		{
			return false;
		}
#else
		static_cast< void >( module );
		return false;
#endif
	}

	void resetWrittenStorage()noexcept
	{
#if D3D11Renderer_USE_SPIRV_CROSS
		shader::g_writtenStorageMask = 0u;
		shader::g_writtenStorageUnknown = false;
#endif
	}

	void takeWrittenStorage( uint64_t & mask, bool & unknown )noexcept
	{
#if D3D11Renderer_USE_SPIRV_CROSS
		mask = shader::g_writtenStorageMask;
		unknown = shader::g_writtenStorageUnknown;
#else
		mask = ~0ull;
		unknown = true;
#endif
		resetWrittenStorage();
	}

	CompiledShaderModule ShaderModule::compile( VkPipelineShaderStageCreateInfo const & state
		, VkPipelineLayout pipelineLayout
		, VkPipelineCreateFlags createFlags )const
	{
		return CompiledShaderModule{ m_device, pipelineLayout, createFlags, get( this ), m_code, state };
	}

	//*************************************************************************
}
