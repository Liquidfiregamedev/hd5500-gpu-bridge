/*
This file belongs to Ashes.
See LICENSE file in root folder
*/
#pragma once

#include "renderer/D3D11Renderer/Command/Commands/D3D11CommandBase.hpp"

namespace ashes::D3D11_NAMESPACE
{
	class UploadMemoryCommand
		: public CommandBase
	{
	public:
		UploadMemoryCommand( VkDevice device
			, ObjectMemory const * memory
			, VkDeviceSize offset
			, VkDeviceSize size
			, UINT subresource
			, bool mappedOnly = false );

		void apply( Context & context )const override;
		bool isStateOnly()const override { return true; }   // v13.63
		CommandPtr clone()const override;
		// v13.73: upload merging (CommandBuffer::doMergeUploads); offsets relative to the object
		void widen( VkDeviceSize begin, VkDeviceSize end ) { m_range = { begin, end - begin }; }
		ObjectMemory const * memory()const { return m_memory; }

	private:
		ObjectMemory const * m_memory;
		std::pair< VkDeviceSize, VkDeviceSize > m_range;
		UINT m_subresource;
		bool m_mappedOnly{};
		std::weak_ptr< uint8_t > m_parentLifetime;
	};
}
