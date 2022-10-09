#pragma once
#include <memory>
#include <d3d12.h>
#include <wrl.h>
#include "Enums.h"
#include "RendererSettings.h"
#include "BlurPass.h"
#include "CopyToTexturePass.h"
#include "AddTexturesPass.h"
#include "GenerateMipsPass.h"
#include "AutomaticExposurePass.h"
#include "LensFlarePass.h"
#include "VolumetricCloudsPass.h"
#include "SSRPass.h"
#include "FogPass.h"
#include "DepthOfFieldPass.h"
#include "BloomPass.h"
#include "VelocityBufferPass.h"
#include "MotionBlurPass.h"
#include "TAAPass.h"
#include "GodRaysPass.h"
#include "../Core/Definitions.h"
#include "../RenderGraph/RenderGraphResourceId.h"
#include "entt/entity/entity.hpp"


namespace adria
{
	namespace tecs
	{
		class registry;
	}
	class RenderGraph;
	class GraphicsDevice;
	class GPUProfiler;
	class TextureManager;
	class Texture;
	class Buffer;
	struct Light;

	class Postprocessor
	{
	public:
		Postprocessor(entt::registry& reg, TextureManager& texture_manager, uint32 width, uint32 height);
		void AddPasses(RenderGraph& rg, PostprocessSettings const& settings);

		void OnResize(GraphicsDevice* gfx, uint32 w, uint32 h);
		void OnSceneInitialized(GraphicsDevice* gfx);
		RGResourceName GetFinalResource() const;

		CloudParameters const& GetCloudParams() const { return clouds_pass.GetParams(); }
		SSRParameters   const& GetSSRParams() const { return ssr_pass.GetParams(); }
		FogParameters   const& GetFogParams() const { return fog_pass.GetParams(); }
		DoFParameters   const& GetDoFParams() const { return dof_pass.GetParams(); }
		BloomParameters const& GetBloomParams() const { return bloom_pass.GetParams(); }
		VelocityBufferParams const& GetVelocityBufferParams() const { return velocity_buffer_pass.GetParams(); }

	private:
		entt::registry& reg;
		TextureManager& texture_manager;
		uint32 width, height;
		PostprocessSettings settings;

		std::vector<size_t> cloud_textures;
		size_t hex_bokeh_handle = -1;
		size_t oct_bokeh_handle = -1;
		size_t circle_bokeh_handle = -1;
		size_t cross_bokeh_handle = -1;

		RGResourceName final_resource;
		std::unique_ptr<Texture> history_buffer;
		std::unique_ptr<Buffer> counter_reset_buffer;
		std::unique_ptr<Buffer> bokeh_indirect_buffer;
		Microsoft::WRL::ComPtr<ID3D12CommandSignature> bokeh_command_signature;
		
		BlurPass blur_pass;
		CopyToTexturePass copy_to_texture_pass;
		AddTexturesPass add_textures_pass;
		GenerateMipsPass generate_mips_pass;
		AutomaticExposurePass automatic_exposure_pass;
		LensFlarePass lens_flare_pass;
		VolumetricCloudsPass clouds_pass;
		SSRPass ssr_pass;
		FogPass fog_pass;
		DepthOfFieldPass dof_pass;
		BloomPass bloom_pass;
		VelocityBufferPass velocity_buffer_pass;
		MotionBlurPass motion_blur_pass;
		TAAPass taa_pass;
		GodRaysPass god_rays_pass;
	private:
		RGResourceName AddHDRCopyPass(RenderGraph& rg);
		void AddHistoryCopyPass(RenderGraph& rg);
		void AddSunPass(RenderGraph& rg, entt::entity sun);

		void AddGenerateBokehPass(RenderGraph& rg);
		void AddDrawBokehPass(RenderGraph& rg);
	};
}