#pragma once
#include "RenderGraph/RenderGraphResourceName.h"

namespace adria
{
	class GfxDevice;
	class ComputePipelineState;
	class RenderGraph;

	class MotionBlurPass
	{
	public:
		MotionBlurPass(GfxDevice* gfx, uint32 w, uint32 h);

		RGResourceName AddPass(RenderGraph& rendergraph, RGResourceName input);
		void OnResize(uint32 w, uint32 h);

	private:
		GfxDevice* gfx;
		uint32 width, height;
		std::unique_ptr<ComputePipelineState> motion_blur_pso;

	private:
		void CreatePSO();
	};

}