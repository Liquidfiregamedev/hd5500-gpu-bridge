/**
*\file
*	Instance.h
*\author
*	Sylvain Doremus
*/
#pragma once

#include "renderer/D3D11Renderer/D3D11RendererPrerequisites.hpp"

#include <chrono>
#include <vector>

namespace ashes::D3D11_NAMESPACE
{
	class SwapchainKHR
		: public NonCopyable
	{
	public:
		SwapchainKHR( VkDevice device
			, VkSwapchainCreateInfoKHR createInfo );
		~SwapchainKHR()noexcept;

		uint32_t getImageCount()const;
		VkImageArray getImages()const;
		VkResult acquireNextImage( uint32_t & imageIndex )const;
		VkResult present( uint32_t imageIndex )const;

		inline IDXGISwapChain * getSwapChain()const
		{
			return m_swapChain;
		}

		VkDevice getDevice()const
		{
			return m_device;
		}

		// v13.3 WSI lifecycle diagnostics (read-only accessors).
		uint64_t getPresentCount()const
		{
			return m_presentCount;
		}

		uint64_t getAcquireCount()const
		{
			return m_acquireCount;
		}

		uint64_t getLifetimeMillis()const
		{
			return uint64_t( std::chrono::duration_cast< std::chrono::milliseconds >(
				std::chrono::steady_clock::now() - m_createdAt ).count() );
		}

		VkExtent2D getWindowExtent()const
		{
			return m_windowExtent;
		}

		VkExtent2D getSwapchainExtent()const
		{
			return m_swapchainExtent;
		}

		// v13.60: flip-model swapchains cannot share a window with another swapchain, so the
		// replaced (old) swapchain gives up its DXGI swapchain before the new one is created.
		// A retired swapchain answers present() with VK_ERROR_OUT_OF_DATE_KHR.
		void retireNative()noexcept;
		// v13.60c: hands the DXGI flip swapchain to the replacing swapchain (which resizes it
		// with ResizeBuffers) instead of destroying it; nullptr when this is not a flip swapchain.
		IDXGISwapChain * takeFlipSwapChain()noexcept;

	private:
		void doInitPresentParameters( bool flipModel );
		void doReleaseImages()noexcept;

	private:
		VkDevice m_device;
		VkSwapchainCreateInfoKHR m_createInfo;
#if defined( Ashes_D3D11_XBox )
		DXGI_SWAP_CHAIN_DESC1 m_presentDesc{};
#else
		DXGI_SWAP_CHAIN_DESC m_presentDesc;
#endif
		DXGI_MODE_DESC m_displayMode;
		uint32_t m_currentBuffer{};
		IDXGISwapChain * m_swapChain{ nullptr };
		VkImage m_swapChainImage{};
		bool m_flipModel{};   // v13.60
		bool m_tearing{};   // v13.70: DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING in use
		// v13.4: 1..3 Vulkan-visible images (RW_HD5500_SWAPCHAIN_IMAGES); index 0 is the v13.3 image.
		std::vector< VkImage > m_images;
		std::vector< VkDeviceMemory > m_deviceMemories;
		std::vector< VkImageView > m_views;
		mutable uint32_t m_nextImage{};
		VkExtent2D m_swapchainExtent;
		VkExtent2D m_windowExtent;
		// Opt-in only (RW_HD5500_FRAME_PROBE=1); mutable because present() is const.
		bool m_frameProbeEnabled{};
		mutable uint64_t m_presentCount{};
		mutable uint64_t m_nextFrameProbeTick{};
		mutable uint32_t m_timedFrameProbeCount{};
		mutable uint64_t m_acquireCount{};
		std::chrono::steady_clock::time_point m_createdAt{ std::chrono::steady_clock::now() };
	};
}
