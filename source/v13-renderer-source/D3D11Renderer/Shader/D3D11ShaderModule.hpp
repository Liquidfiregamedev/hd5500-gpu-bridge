/*
This file belongs to Ashes.
See LICENSE file in root folder.
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"
#include "renderer/D3D11Renderer/Shader/D3D11ShaderDesc.hpp"

#include <d3d11shader.h>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	// v13.15: first UAV register for shaders compiled after this call (colour attachment count).
	void setUavRegisterStart( uint32_t value )noexcept;
	// v13.23: storage buffers/images that the shaders compiled since the last reset can WRITE
	// (no NonWritable decoration). Bit N = descriptor binding N (any set); `unknown` when a
	// binding >= 64 is written. Read-only storage resources need no GPU->CPU download.
	void resetWrittenStorage()noexcept;
	void takeWrittenStorage( uint64_t & mask, bool & unknown )noexcept;
	// v13.35: D3D11 links vertex->pixel shader varyings by REGISTER, not by name. A pixel
	// shader that skips a vertex output (Waylith's CutoutPlant reads location 1 and 2 but
	// not 0) gets its registers shifted and reads the wrong data. Before compiling a
	// graphics pipeline, the pipeline passes the fragment shader's input locations here;
	// the vertex shader then drops the outputs nobody reads so both signatures line up.
	// Pass nullptr to switch it off again.
	void setFragmentInputLocations( std::vector< uint32_t > const * locations )noexcept;
	// Collects the Location decorations of the fragment entry point's stage inputs.
	bool collectFragmentInputLocations( VkShaderModule module, std::vector< uint32_t > & locations );

	class CompiledShaderModule
	{
	public:
		CompiledShaderModule( CompiledShaderModule const & ) = delete;
		CompiledShaderModule & operator=( CompiledShaderModule const & ) = delete;
		CompiledShaderModule( CompiledShaderModule && rhs )noexcept;
		CompiledShaderModule & operator=( CompiledShaderModule && rhs )noexcept;

		CompiledShaderModule( VkDevice device
			, VkPipelineLayout pipelineLayout
			, VkPipelineCreateFlags createFlags
			, VkShaderModule shaderModule
			, UInt32Array const & spv
			, VkPipelineShaderStageCreateInfo const & state );
		~CompiledShaderModule()noexcept;

		ID3D11ComputeShader * getCSShader()const
		{
			return m_shader.compute;
		}

		ID3D11VertexShader * getVSShader()const
		{
			return m_shader.vertex;
		}

		ID3D11GeometryShader * getGSShader()const
		{
			return m_shader.geometry;
		}

		ID3D11HullShader * getHSShader()const
		{
			return m_shader.hull;
		}

		ID3D11DomainShader * getDSShader()const
		{
			return m_shader.domain;
		}

		ID3D11PixelShader * getPSShader()const
		{
			return m_shader.pixel;
		}

		ID3DBlob * getCompiled()const
		{
			return m_compiled;
		}

		VkShaderStageFlagBits getStage()const
		{
			return m_stage;
		}

		ShaderDesc const & getLayout()const
		{
			return m_layout;
		}

	private:
		bool doRetrieveShader( VkDevice device );   // v13.64: true = prewarmed object used
		ShaderDesc doRetrieveShaderDesc( VkDevice device );
		InputLayout doRetrieveInputLayout( ID3D11ShaderReflection * reflection
			, UINT inputParameters )const;
		InterfaceBlockLayout doRetrieveInterfaceBlockLayout( ID3D11ShaderReflection * reflection
			, UINT constantBuffers )const;

	private:
		union
		{
			ID3D11ComputeShader * compute;
			ID3D11VertexShader * vertex;
			ID3D11GeometryShader * geometry;
			ID3D11HullShader * hull;
			ID3D11DomainShader * domain;
			ID3D11PixelShader * pixel;
		} m_shader{};
		VkShaderStageFlagBits m_stage;
		std::string m_source;
		ID3DBlob * m_compiled{ nullptr };
		std::string m_cacheKey;   // v13.64: for the prewarmed shader object lookup
		ShaderDesc m_layout;
	};

	class ShaderModule
	{
	public:
		ShaderModule( VkDevice device
			, VkShaderModuleCreateInfo createInfo );

		CompiledShaderModule compile( VkPipelineShaderStageCreateInfo const & state
			, VkPipelineLayout pipelineLayout
			, VkPipelineCreateFlags createFlags )const;

		VkDevice getDevice()const
		{
			return m_device;
		}

		UInt32Array const & getCode()const
		{
			return m_code;
		}

	private:
		VkDevice m_device;
		VkShaderModuleCreateInfo m_createInfo;
		UInt32Array m_code;
		mutable std::string m_source;
	};
}
