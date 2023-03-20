#pragma once
#include <memory>
#include "GfxFormat.h"
#include "GfxDefines.h"
#include "../Utilities/AutoRefCountPtr.h"

namespace adria
{
	class GfxDevice;
	class GfxCommandList;
	class GfxTexture;

	struct GfxSwapchainDesc
	{
		uint32 width = 0;
		uint32 height = 0;
		GfxFormat backbuffer_format = GfxFormat::R8G8B8A8_UNORM_SRGB;
		bool fullscreen_windowed = false;
	};

	class GfxSwapchain
	{
	public:
		GfxSwapchain(GfxDevice* gfx, GfxSwapchainDesc const& desc);
		~GfxSwapchain();

		void SetAsRenderTarget(GfxCommandList* cmd_list);
		void ClearBackbuffer(GfxCommandList* cmd_list);
		void Present(bool vsync);
		void OnResize(uint32 w, uint32 h);

		IDXGISwapChain4* GetNative() const { return swapchain.Get(); }
		uint32 GetBackbufferIndex() const { return backbuffer_index; }
		GfxTexture* GetBackbuffer() const { return back_buffers[backbuffer_index].get(); }

	private:
		GfxDevice* gfx = nullptr;
		ArcPtr<IDXGISwapChain4>				swapchain = nullptr;
		std::unique_ptr<GfxTexture>			back_buffers[GFX_BACKBUFFER_COUNT] = { nullptr };

		uint32		 width;
		uint32		 height;
		uint32		 backbuffer_index;

	private:
		void CreateBackbuffers();
		size_t GetBackbufferRTV() const;
	};
}