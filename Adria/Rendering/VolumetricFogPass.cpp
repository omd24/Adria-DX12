#include "VolumetricFogPass.h"
#include "BlackboardData.h"
#include "PSOCache.h" 
#include "Components.h"
#include "Graphics/GfxTexture.h"
#include "Graphics/GfxBuffer.h"
#include "RenderGraph/RenderGraph.h"
#include "Editor/GUICommand.h"
#include "Core/Paths.h"

namespace adria
{
	static constexpr uint32 VOXEL_TEXEL_SIZE_X = 8;
	static constexpr uint32 VOXEL_TEXEL_SIZE_Y = 8;
	static constexpr uint32 VOXEL_GRID_SIZE_Z  = 128;


	VolumetricFogPass::VolumetricFogPass(GfxDevice* gfx, entt::registry& reg, uint32 w, uint32 h) : gfx(gfx), reg(reg), width(w), height(h)
	{
		CreateLightInjectionHistoryTexture();
	}

	void VolumetricFogPass::OnSceneInitialized()
	{
		std::string blue_noise_base_path = paths::TexturesDir() + "BlueNoise/";
		for (uint32 i = 0; i < BLUE_NOISE_TEXTURE_COUNT; ++i)
		{
			std::string blue_noise_texture_path = blue_noise_base_path + "LDR_LLL1_" + std::to_string(i) + ".png";
			blue_noise_handles[i] = g_TextureManager.LoadTexture(blue_noise_texture_path);
		}

		BoundingBox scene_bounding_box;
		for (auto mesh_entity : reg.view<Mesh>())
		{
			Mesh& mesh = reg.get<Mesh>(mesh_entity);
			for (auto const& instance : mesh.instances)
			{
				SubMeshGPU& submesh = mesh.submeshes[instance.submesh_index];
				BoundingBox instance_bounding_box;
				submesh.bounding_box.Transform(instance_bounding_box, instance.world_transform);
				BoundingBox::CreateMerged(scene_bounding_box, scene_bounding_box, instance_bounding_box);
			}
		}

		FogVolume& fog_volume = fog_volumes.emplace_back();
		fog_volume.volume = scene_bounding_box;
		fog_volume.color = Color(1, 1, 1);
		fog_volume.density_base = 0.0f;
		fog_volume.density_change = 0.05f;

		CreateFogVolumeBuffer();
	}

	void VolumetricFogPass::AddPasses(RenderGraph& rg)
	{
		GfxDescriptor fog_volume_buffer_srv_gpu = gfx->AllocateDescriptorsGPU();
		gfx->CopyDescriptors(1, fog_volume_buffer_srv_gpu, fog_volume_buffer_srv);
		fog_volume_buffer_idx = fog_volume_buffer_srv_gpu.GetIndex();

		AddLightInjectionPass(rg);
		AddScatteringIntegrationPass(rg);
		AddCombineFogPass(rg);

		GUI_Command([&]()
			{
				if (fog_volumes.empty()) return;

				FogVolume& fog_volume = fog_volumes[0];
				if (ImGui::TreeNode("Volumetric Fog"))
				{
					ImGui::Checkbox("Temporal Lighting", &temporal_lighting);

					bool update_fog_volume_buffer = false;
					update_fog_volume_buffer |= ImGui::SliderFloat("Density Base", &fog_volume.density_base, 0.0f, 1.0f);
					update_fog_volume_buffer |= ImGui::SliderFloat("Density Change", &fog_volume.density_change, 0.0f, 1.0f);
					Vector3 fog_color = fog_volume.color.ToVector3();
					update_fog_volume_buffer |= ImGui::ColorEdit3("Fog Color", (float*)&fog_color);
					fog_volume.color = Color(fog_color);

					if (update_fog_volume_buffer)
					{
						CreateFogVolumeBuffer();
					}
					
					ImGui::TreePop();
				}
			}, GUICommandGroup_Renderer);
	}

	void VolumetricFogPass::AddLightInjectionPass(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		rg.ImportTexture(RG_RES_NAME(FogLightInjectionTargetHistory), light_injection_target_history.get());

		struct LightInjectionPassData
		{
			RGTextureReadWriteId light_injection_target;
			RGTextureReadOnlyId  light_injection_target_history;
			RGBufferReadOnlyId   fog_volume_buffer;
		};

		rg.AddPass<LightInjectionPassData>("Volumetric Fog Light Injection Pass",
			[=](LightInjectionPassData& data, RenderGraphBuilder& builder)
			{
				uint32 const voxel_grid_width = DivideAndRoundUp(width, VOXEL_TEXEL_SIZE_X);
				uint32 const voxel_grid_height = DivideAndRoundUp(height, VOXEL_TEXEL_SIZE_Y);

				RGTextureDesc light_injection_target_desc{};
				light_injection_target_desc.type = GfxTextureType_3D;
				light_injection_target_desc.width = voxel_grid_width;
				light_injection_target_desc.height = voxel_grid_height;
				light_injection_target_desc.depth = VOXEL_GRID_SIZE_Z;
				light_injection_target_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				builder.DeclareTexture(RG_RES_NAME(FogLightInjectionTarget), light_injection_target_desc);

				data.light_injection_target = builder.WriteTexture(RG_RES_NAME(FogLightInjectionTarget));
				data.light_injection_target_history = builder.ReadTexture(RG_RES_NAME(FogLightInjectionTargetHistory));
			},
			[=](LightInjectionPassData const& data, RenderGraphContext& ctx, GfxCommandList* cmd_list)
			{
				GfxDevice* gfx = cmd_list->GetDevice();
				
				uint32 i = gfx->AllocateDescriptorsGPU(2).GetIndex();
				gfx->CopyDescriptors(1, gfx->GetDescriptorGPU(i + 0), ctx.GetReadWriteTexture(data.light_injection_target));
				gfx->CopyDescriptors(1, gfx->GetDescriptorGPU(i + 1), ctx.GetReadOnlyTexture(data.light_injection_target_history));
				
				struct LightInjectionConstants
				{
					Vector3u voxel_grid_dimensions;
					uint32 fog_volumes_count;
					uint32 fog_volume_buffer_idx;
					uint32 light_injection_target_idx;
					uint32 light_injection_target_history_idx;
					uint32 blue_noise_idx;
				} constants =
				{
					.voxel_grid_dimensions = Vector3u(light_injection_target_history->GetWidth(), light_injection_target_history->GetHeight(), light_injection_target_history->GetDepth()),
					.fog_volumes_count = fog_volume_buffer->GetCount(),
					.fog_volume_buffer_idx = fog_volume_buffer_idx,
					.light_injection_target_idx = i,
					.light_injection_target_history_idx = i + 1,
					.blue_noise_idx = (uint32)blue_noise_handles[temporal_lighting ? gfx->GetFrameIndex() % BLUE_NOISE_TEXTURE_COUNT : 0]
				};
				
				cmd_list->SetPipelineState(PSOCache::Get(GfxPipelineStateID::VolumetricFog_LightInjection));
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(light_injection_target_history->GetWidth(), 8), 
								   DivideAndRoundUp(light_injection_target_history->GetHeight(), 8),
								   DivideAndRoundUp(light_injection_target_history->GetDepth(), 8));
			}, RGPassType::Compute, RGPassFlags::None);

		rg.ExportTexture(RG_RES_NAME(FogLightInjectionTarget), light_injection_target_history.get());
	}

	void VolumetricFogPass::AddScatteringIntegrationPass(RenderGraph& rg)
	{
		FrameBlackboardData const& frame_data = rg.GetBlackboard().Get<FrameBlackboardData>();

		struct ScatteringIntegrationPassData
		{
			RGTextureReadWriteId integrated_scattering;
			RGTextureReadOnlyId injected_light;
		};

		rg.AddPass<ScatteringIntegrationPassData>("Volumetric Fog Scattering Integration Pass",
			[=](ScatteringIntegrationPassData& data, RenderGraphBuilder& builder)
			{
				uint32 const voxel_grid_width = DivideAndRoundUp(width, VOXEL_TEXEL_SIZE_X);
				uint32 const voxel_grid_height = DivideAndRoundUp(height, VOXEL_TEXEL_SIZE_Y);

				RGTextureDesc voxel_desc{};
				voxel_desc.type = GfxTextureType_3D;
				voxel_desc.width = voxel_grid_width;
				voxel_desc.height = voxel_grid_height;
				voxel_desc.depth = VOXEL_GRID_SIZE_Z;
				voxel_desc.format = GfxFormat::R16G16B16A16_FLOAT;
				builder.DeclareTexture(RG_RES_NAME(FogFinal), voxel_desc);

				data.integrated_scattering = builder.WriteTexture(RG_RES_NAME(FogFinal));
				data.injected_light = builder.ReadTexture(RG_RES_NAME(FogLightInjectionTarget));
			},
			[=](ScatteringIntegrationPassData const& data, RenderGraphContext& ctx, GfxCommandList* cmd_list)
			{
				GfxDevice* gfx = cmd_list->GetDevice();

				uint32 i = gfx->AllocateDescriptorsGPU(2).GetIndex();
				gfx->CopyDescriptors(1, gfx->GetDescriptorGPU(i + 0), ctx.GetReadOnlyTexture(data.injected_light));
				gfx->CopyDescriptors(1, gfx->GetDescriptorGPU(i + 1), ctx.GetReadWriteTexture(data.integrated_scattering));

				struct ScatteringAccumulationConstants
				{
					Vector3u voxel_grid_dimensions;
					uint32   injected_light_idx;
					uint32   integrated_scattering_idx;
				} constants =
				{
					.voxel_grid_dimensions = Vector3u(light_injection_target_history->GetWidth(), light_injection_target_history->GetHeight(), light_injection_target_history->GetDepth()),
					.injected_light_idx = i,
					.integrated_scattering_idx = i + 1
				};

				cmd_list->SetPipelineState(PSOCache::Get(GfxPipelineStateID::VolumetricFog_ScatteringAccumulation));
				cmd_list->SetRootCBV(0, frame_data.frame_cbuffer_address);
				cmd_list->SetRootConstants(1, constants);
				cmd_list->Dispatch(DivideAndRoundUp(light_injection_target_history->GetWidth(), 8),
					DivideAndRoundUp(light_injection_target_history->GetHeight(), 8),
					DivideAndRoundUp(light_injection_target_history->GetDepth(), 8));

			}, RGPassType::Compute, RGPassFlags::ForceNoCull);
	}

	void VolumetricFogPass::AddCombineFogPass(RenderGraph& rg)
	{
		struct CombinePassData
		{
			RGTextureReadOnlyId fog;
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<CombinePassData>("Volumetric Fog Combine Pass",
			[=](CombinePassData& data, RenderGraphBuilder& builder)
			{
				builder.WriteRenderTarget(RG_RES_NAME(HDR_RenderTarget), RGLoadStoreAccessOp::Preserve_Preserve);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				data.fog = builder.ReadTexture(RG_RES_NAME(FogFinal), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](CombinePassData const& data, RenderGraphContext& context, GfxCommandList* cmd_list)
			{
				GfxDevice* gfx = cmd_list->GetDevice();
				cmd_list->SetPipelineState(PSOCache::Get(GfxPipelineStateID::VolumetricFog_CombineFog));

				GfxDescriptor src_descriptors[] = { context.GetReadOnlyTexture(data.fog), context.GetReadOnlyTexture(data.depth) };
				GfxDescriptor dst_descriptor = gfx->AllocateDescriptorsGPU(ARRAYSIZE(src_descriptors));
				gfx->CopyDescriptors(dst_descriptor, src_descriptors);
				uint32 const i = dst_descriptor.GetIndex();

				cmd_list->SetRootConstant(1, i, 0);
				cmd_list->SetRootConstant(1, i + 1, 1);
				cmd_list->SetTopology(GfxPrimitiveTopology::TriangleList);
				cmd_list->Draw(3);
			}, RGPassType::Graphics, RGPassFlags::None);
	}

	void VolumetricFogPass::CreateLightInjectionHistoryTexture()
	{
		uint32 const voxel_grid_width = DivideAndRoundUp(width, VOXEL_TEXEL_SIZE_X);
		uint32 const voxel_grid_height = DivideAndRoundUp(height, VOXEL_TEXEL_SIZE_Y);
		if (light_injection_target_history && light_injection_target_history->GetWidth() == voxel_grid_width &&
			light_injection_target_history->GetHeight() == voxel_grid_height)
		{
			return;
		}

		GfxTextureDesc light_injection_target_desc{};
		light_injection_target_desc.type = GfxTextureType_3D;
		light_injection_target_desc.width = voxel_grid_width;
		light_injection_target_desc.height = voxel_grid_height;
		light_injection_target_desc.depth = VOXEL_GRID_SIZE_Z;
		light_injection_target_desc.format = GfxFormat::R16G16B16A16_FLOAT;
		light_injection_target_desc.bind_flags = GfxBindFlag::ShaderResource;
		light_injection_target_desc.initial_state = GfxResourceState::CopyDst;
		light_injection_target_history = gfx->CreateTexture(light_injection_target_desc);
		light_injection_target_history->SetName("Light Injection Target History");
		light_injection_target_history_srv = gfx->CreateTextureSRV(light_injection_target_history.get());
	}

	void VolumetricFogPass::CreateFogVolumeBuffer()
	{
		if (!fog_volume_buffer || fog_volume_buffer->GetCount() < fog_volumes.size())
		{
			fog_volume_buffer = gfx->CreateBuffer(StructuredBufferDesc<FogVolumeGPU>(fog_volumes.size(), false, true));
			fog_volume_buffer_srv = gfx->CreateBufferSRV(fog_volume_buffer.get());
		}

		std::vector<FogVolumeGPU> gpu_fog_volumes;
		for (auto const& fog_volume : fog_volumes)
		{
			FogVolumeGPU fog_volume_gpu =
			{
				.center = fog_volume.volume.Center,
				.extents = fog_volume.volume.Extents,
				.color = fog_volume.color.ToVector3(),
				.density_base = fog_volume.density_base,
				.density_change = fog_volume.density_change
			};
			gpu_fog_volumes.push_back(std::move(fog_volume_gpu));
		}
		fog_volume_buffer->Update(gpu_fog_volumes.data(), gpu_fog_volumes.size() * sizeof(FogVolumeGPU));
	}

}

