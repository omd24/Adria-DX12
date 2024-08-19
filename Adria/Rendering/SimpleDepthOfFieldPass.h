#pragma once
#include "BokehPass.h"
#include "BlurPass.h"
#include "PostEffect.h"

namespace adria
{
	class GfxDevice;
	class GfxComputePipelineState;
	class RenderGraph;

	class SimpleDepthOfFieldPass : public PostEffect
	{
	public:
		SimpleDepthOfFieldPass(GfxDevice* gfx, uint32 w, uint32 h);

		virtual void AddPass(RenderGraph&, PostProcessor*) override;
		virtual void OnResize(uint32, uint32) override;
		virtual bool IsEnabled(PostProcessor const*) const override;
		virtual void OnSceneInitialized() override;
		virtual void GUI();

	private:
		GfxDevice* gfx;
		uint32 width, height;
		BokehPass bokeh_pass;
		BlurPass blur_pass;

		std::unique_ptr<GfxComputePipelineState> dof_pso;

	private:
		void CreatePSO();
	};

}