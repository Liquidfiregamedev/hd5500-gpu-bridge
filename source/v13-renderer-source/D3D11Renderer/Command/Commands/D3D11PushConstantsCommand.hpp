/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/Command/Commands/D3D11CommandBase.hpp"

#include "renderer/D3D11Renderer/Shader/D3D11ShaderDesc.hpp"

namespace ashes::D3D11_NAMESPACE
{
	/**
	*\brief
	*	Commande de d�marrage d'une requ�te.
	*/
	class PushConstantsCommand
		: public CommandBase
	{
	public:
		PushConstantsCommand( VkDevice device
			, PushConstantsBuffer pcb );   // v13.71: by value (moved in)

		void apply( Context & context )const override;
		bool isStateOnly()const override { return true; }   // v13.63
		void remove( Context const & context )const override;
		uint64_t removeKey()const override;   // v13.51
		CommandPtr clone()const override;

	private:
		PushConstantsBuffer m_pcb;
	};
}
