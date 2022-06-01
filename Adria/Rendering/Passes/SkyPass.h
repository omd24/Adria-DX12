#pragma once
#include "../../Core/Definitions.h"
#include "../../RenderGraph/RenderGraphResourceRef.h"

namespace adria
{
	class RenderGraph;
	class TextureManager;
	namespace tecs
	{
		class registry;
	}
	enum class ESkyType : uint8;

	struct SkyPassData
	{
		RGTextureRef	_1;
		RGTextureSRVRef _2;
	};

	class SkyPass
	{
	public:
		SkyPass(tecs::registry& reg, TextureManager& texture_manager, uint32 w, uint32 h);
		SkyPassData const& AddPass(RenderGraph& rg, ESkyType sky_type);

		void OnSceneInitialized(GraphicsDevice* gfx);
		void OnResize(uint32 w, uint32 h);
	private:
		tecs::registry& reg;
		TextureManager& texture_manager;
		uint32 width, height;
		std::unique_ptr<Buffer>	cube_vb = nullptr;
		std::unique_ptr<Buffer>	cube_ib = nullptr;

	private:
		void CreateCubeBuffers(GraphicsDevice* gfx);
	};
}