#include "Postprocessor.h"
#include "ConstantBuffers.h"
#include "Components.h"
#include "GlobalBlackboardData.h"
#include "RootSigPSOManager.h"
#include "../RenderGraph/RenderGraph.h"
#include "../Graphics/GPUProfiler.h"
#include "../tecs/registry.h"
#include "../Logging/Logger.h"

using namespace DirectX;

namespace adria
{
	Postprocessor::Postprocessor(tecs::registry& reg, TextureManager& texture_manager, uint32 width, uint32 height)
		: reg(reg), texture_manager(texture_manager), width(width), height(height),
		  blur_pass(width, height), copy_to_texture_pass(width, height), generate_mips_pass(width, height),
		  add_textures_pass(width, height)
	{}

	void Postprocessor::AddPasses(RenderGraph& rg, PostprocessSettings const& _settings)
	{
		settings = _settings;
		auto lights = reg.view<Light>();
		AddVelocityBufferPass(rg);

		AddCopyHDRPass(rg);
		final_resource = RG_RES_NAME(PostprocessMain);
		for (tecs::entity light : lights)
		{
			auto const& light_data = lights.get(light);
			if (!light_data.active || !light_data.lens_flare) continue;
			AddLensFlarePass(rg, light_data);
		}

		if (settings.clouds)
		{
			AddVolumetricCloudsPass(rg);
			blur_pass.AddPass(rg, RG_RES_NAME(CloudsOutput), RG_RES_NAME(BlurredCloudsOutput), "Volumetric Clouds");
			copy_to_texture_pass.AddPass(rg, RG_RES_NAME(PostprocessMain), RG_RES_NAME(BlurredCloudsOutput), EBlendMode::AlphaBlend);
		}
		if (settings.reflections == EReflections::SSR)
		{
			AddSSRPass(rg);
			final_resource = RG_RES_NAME(SSR_Output);
		}
		if (settings.fog)
		{
			AddFogPass(rg);
			final_resource = RG_RES_NAME(FogOutput);
		}
		if (settings.dof)
		{
			blur_pass.AddPass(rg, final_resource, RG_RES_NAME(BlurredDofInput), "DoF");
			//if (settings.bokeh) PassGenerateBokeh(cmd_list);
			AddDepthOfFieldPass(rg);
			final_resource = RG_RES_NAME(DepthOfFieldOutput);
		}
		if (settings.motion_blur)
		{
			AddMotionBlurPass(rg);
			final_resource = RG_RES_NAME(MotionBlurOutput);
		}
		if (settings.bloom)
		{
			AddBloomPass(rg);
			final_resource = RG_RES_NAME(BloomOutput);
		}
		for (tecs::entity light_entity : lights)
		{
			auto const& light = lights.get(light_entity);
			if (!light.active) continue;

			if (light.type == ELightType::Directional)
			{
				AddSunPass(rg, light_entity);
				if (light.god_rays)
				{
					AddGodRaysPass(rg, light);
					copy_to_texture_pass.AddPass(rg, final_resource, RG_RES_NAME(GodRaysOutput), EBlendMode::AdditiveBlend);
				}
				else
				{
					copy_to_texture_pass.AddPass(rg, final_resource, RG_RES_NAME(SunOutput), EBlendMode::AdditiveBlend);
				}
				break;
			}
		}
		if (HasAnyFlag(settings.anti_aliasing, AntiAliasing_TAA))
		{
			AddTAAPass(rg);
			final_resource = RG_RES_NAME(TAAOutput);
			AddHistoryCopyPass(rg);
		}
	}

	void Postprocessor::OnResize(GraphicsDevice* gfx, uint32 w, uint32 h)
	{
		width = w, height = h;
		blur_pass.OnResize(width, height);
		copy_to_texture_pass.OnResize(width, height);
		generate_mips_pass.OnResize(w, h);

		TextureDesc render_target_desc{};
		render_target_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		render_target_desc.width = width;
		render_target_desc.height = height;
		render_target_desc.bind_flags = EBindFlag::ShaderResource;
		render_target_desc.initial_state = EResourceState::CopyDest;
		history_buffer = std::make_unique<Texture>(gfx, render_target_desc);
	}

	void Postprocessor::OnSceneInitialized(GraphicsDevice* gfx)
	{
		cloud_textures.push_back(texture_manager.LoadTexture(L"Resources\\Textures\\clouds\\weather.dds"));
		cloud_textures.push_back(texture_manager.LoadTexture(L"Resources\\Textures\\clouds\\cloud.dds"));
		cloud_textures.push_back(texture_manager.LoadTexture(L"Resources\\Textures\\clouds\\worley.dds"));

		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare0.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare1.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare2.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare3.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare4.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare5.jpg"));
		lens_flare_textures.push_back(texture_manager.LoadTexture(L"Resources/Textures/lensflare/flare6.jpg"));

		TextureDesc render_target_desc{};
		render_target_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		render_target_desc.width = width;
		render_target_desc.height = height;
		render_target_desc.bind_flags = EBindFlag::ShaderResource;
		render_target_desc.initial_state = EResourceState::CopyDest;

		history_buffer = std::make_unique<Texture>(gfx, render_target_desc);
	}

	RGResourceName Postprocessor::GetFinalResource() const
	{
		return final_resource;
	}

	void Postprocessor::AddCopyHDRPass(RenderGraph& rg)
	{
		struct CopyPassData
		{
			RGTextureCopySrcId copy_src;
			RGTextureCopyDstId copy_dst;
		};

		rg.AddPass<CopyPassData>("Copy HDR Pass",
			[=](CopyPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc postprocess_desc{};
				postprocess_desc.width = width;
				postprocess_desc.height = height;
				postprocess_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_RES_NAME(PostprocessMain), postprocess_desc);
				data.copy_dst = builder.WriteCopyDstTexture(RG_RES_NAME(PostprocessMain));
				data.copy_src = builder.ReadCopySrcTexture(RG_RES_NAME(HDR_RenderTarget));
			},
			[=](CopyPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				Texture const& src_texture = context.GetCopySrcTexture(data.copy_src);
				Texture const& dst_texture = context.GetCopyDstTexture(data.copy_dst);
				cmd_list->CopyResource(dst_texture.GetNative(), src_texture.GetNative());
			}, ERGPassType::Copy, ERGPassFlags::None);
	}

	void Postprocessor::AddVelocityBufferPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		struct VelocityBufferPassData
		{
			RGTextureReadOnlyId depth_srv;
		};
		rg.AddPass<VelocityBufferPassData>("Velocity Buffer Pass",
			[=](VelocityBufferPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc velocity_buffer_desc{};
				velocity_buffer_desc.width = width;
				velocity_buffer_desc.height = height;
				velocity_buffer_desc.format = DXGI_FORMAT_R16G16_FLOAT;

				builder.SetViewport(width, height);
				builder.DeclareTexture(RG_RES_NAME(VelocityBuffer), velocity_buffer_desc);
				builder.WriteRenderTarget(RG_RES_NAME(VelocityBuffer), ERGLoadStoreAccessOp::Discard_Preserve);
				data.depth_srv = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
			},
			[=](VelocityBufferPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::VelocityBuffer));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::VelocityBuffer));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(1);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index),
					context.GetReadOnlyTexture(data.depth_srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddVolumetricCloudsPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		struct VolumetricCloudsPassData
		{
			RGTextureReadOnlyId depth;
		};
		rg.AddPass<VolumetricCloudsPassData>("Volumetric Clouds Pass",
			[=](VolumetricCloudsPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc clouds_output_desc{};
				clouds_output_desc.clear_value = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				clouds_output_desc.width = width;
				clouds_output_desc.height = height;
				clouds_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_RES_NAME(CloudsOutput), clouds_output_desc);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.WriteRenderTarget(RG_RES_NAME(CloudsOutput), ERGLoadStoreAccessOp::Clear_Preserve);
				builder.SetViewport(width, height);
			},
			[=](VolumetricCloudsPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::Clouds));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::Clouds));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.weather_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(4);
				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { texture_manager.CpuDescriptorHandle(cloud_textures[0]),  texture_manager.CpuDescriptorHandle(cloud_textures[1]),
															 texture_manager.CpuDescriptorHandle(cloud_textures[2]), context.GetReadOnlyTexture(data.depth) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1, 1, 1 };
				uint32 dst_range_sizes[] = { 4 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddSSRPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;
		struct SSRPassData
		{
			RGTextureReadOnlyId normals;
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<SSRPassData>("SSR Pass",
			[=](SSRPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc ssr_output_desc{};
				ssr_output_desc.width = width;
				ssr_output_desc.height = height;
				ssr_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_RES_NAME(SSR_Output), ssr_output_desc);
				builder.WriteRenderTarget(RG_RES_NAME(SSR_Output), ERGLoadStoreAccessOp::Discard_Preserve);
				data.input = builder.ReadTexture(last_resource, ReadAccess_PixelShader);
				data.normals = builder.ReadTexture(RG_RES_NAME(GBufferNormal), ReadAccess_PixelShader);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](SSRPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::SSR));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::SSR));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);

				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { context.GetReadOnlyTexture(data.normals), context.GetReadOnlyTexture(data.input), context.GetReadOnlyTexture(data.depth) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1, 1 };
				uint32 dst_range_sizes[] = { 3 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);
			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddFogPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;
		struct FogPassData
		{
			RGTextureReadOnlyId depth;
			RGTextureReadOnlyId input;
		};

		rg.AddPass<FogPassData>("Fog Pass",
			[=](FogPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc fog_output_desc{};
				fog_output_desc.clear_value = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				fog_output_desc.width = width;
				fog_output_desc.height = height;
				fog_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_RES_NAME(FogOutput), fog_output_desc);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				data.input = builder.ReadTexture(last_resource, ReadAccess_PixelShader);
				builder.WriteRenderTarget(RG_RES_NAME(FogOutput), ERGLoadStoreAccessOp::Discard_Preserve);
				builder.SetViewport(width, height);
			},
			[=](FogPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::Fog));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::Fog));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);
				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { context.GetReadOnlyTexture(data.input),  context.GetReadOnlyTexture(data.depth) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1 };
				uint32 dst_range_sizes[] = { 2 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddBloomPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;
		struct BloomExtractPassData
		{
			RGTextureReadWriteId extract;
			RGTextureReadOnlyId input;
		};

		rg.AddPass<BloomExtractPassData>("BloomExtract Pass",
			[=](BloomExtractPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc bloom_extract_desc{};
				bloom_extract_desc.width = width;
				bloom_extract_desc.height = height;
				bloom_extract_desc.mip_levels = 5;
				bloom_extract_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.DeclareTexture(RG_RES_NAME(BloomExtract), bloom_extract_desc);
				data.extract = builder.WriteTexture(RG_RES_NAME(BloomExtract));
				data.input = builder.ReadTexture(last_resource, ReadAccess_NonPixelShader);
			},
			[=](BloomExtractPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetComputeRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::BloomExtract));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::BloomExtract));
				cmd_list->SetComputeRootConstantBufferView(0, global_data.compute_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(2);
				D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor = context.GetReadOnlyTexture(data.input);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetComputeRootDescriptorTable(1, descriptor_allocator->GetHandle(descriptor_index));

				++descriptor_index;
				cpu_descriptor = context.GetReadWriteTexture(data.extract);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetComputeRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));

				cmd_list->Dispatch((uint32)std::ceil(width / 32.0f),
					(uint32)std::ceil(height / 32.0f), 1);

			}, ERGPassType::Compute, ERGPassFlags::None);

		generate_mips_pass.AddPass(rg, RG_RES_NAME(BloomExtract));

		struct BloomCombinePassData
		{
			RGTextureReadWriteId output;
			RGTextureReadOnlyId  input;
			RGTextureReadOnlyId  extract;
		};
		rg.AddPass<BloomCombinePassData>("BloomCombine Pass",
			[=](BloomCombinePassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc bloom_output_desc{};
				bloom_output_desc.width = width;
				bloom_output_desc.height = height;
				bloom_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				
				builder.DeclareTexture(RG_RES_NAME(BloomOutput), bloom_output_desc);
				data.output = builder.WriteTexture(RG_RES_NAME(BloomOutput));
				data.extract = builder.ReadTexture(RG_RES_NAME(BloomExtract), ReadAccess_NonPixelShader);
				data.input  = builder.ReadTexture(final_resource, ReadAccess_NonPixelShader);
			},
			[=](BloomCombinePassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetComputeRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::BloomCombine));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::BloomCombine));

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);
				D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor = context.GetReadOnlyTexture(data.input);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cpu_descriptor = context.GetReadOnlyTexture(data.extract);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index + 1), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetComputeRootDescriptorTable(0, descriptor_allocator->GetHandle(descriptor_index));

				descriptor_index += 2;
				cpu_descriptor = context.GetReadWriteTexture(data.output);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetComputeRootDescriptorTable(1, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->Dispatch((uint32)std::ceil(width / 32.0f),(uint32)std::ceil(height / 32.0f), 1);
			}, ERGPassType::Compute, ERGPassFlags::None);

	}

	void Postprocessor::AddSunPass(RenderGraph& rg, tecs::entity sun)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;

		rg.AddPass<void>("Sun Pass",
			[=](RenderGraphBuilder& builder)
			{
				RGTextureDesc sun_output_desc{};
				sun_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				sun_output_desc.width = width;
				sun_output_desc.height = height;
				sun_output_desc.clear_value = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);

				builder.DeclareTexture(RG_RES_NAME(SunOutput), sun_output_desc);
				builder.ReadDepthStencil(RG_RES_NAME(DepthStencil), ERGLoadStoreAccessOp::Preserve_Preserve);
				builder.WriteRenderTarget(RG_RES_NAME(SunOutput), ERGLoadStoreAccessOp::Clear_Preserve);
				builder.SetViewport(width, height);
			},
			[=](RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				auto dynamic_allocator = gfx->GetDynamicAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::Forward));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::Sun));
				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				
				auto [transform, mesh, material] = reg.get<Transform, Mesh, Material>(sun);

				ObjectCBuffer object_cbuf_data{};
				object_cbuf_data.model = transform.current_transform;
				object_cbuf_data.inverse_transposed_model = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, object_cbuf_data.model));

				DynamicAllocation object_allocation = dynamic_allocator->Allocate(GetCBufferSize<ObjectCBuffer>(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				object_allocation.Update(object_cbuf_data);
				cmd_list->SetGraphicsRootConstantBufferView(1, object_allocation.gpu_address);

				MaterialCBuffer material_cbuf_data{};
				material_cbuf_data.diffuse = material.diffuse;
				DynamicAllocation material_allocation = dynamic_allocator->Allocate(GetCBufferSize<MaterialCBuffer>(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				material_allocation.Update(material_cbuf_data);
				cmd_list->SetGraphicsRootConstantBufferView(2, material_allocation.gpu_address);

				D3D12_CPU_DESCRIPTOR_HANDLE diffuse_handle = texture_manager.CpuDescriptorHandle(material.albedo_texture);
				uint32 src_range_size = 1;

				OffsetType descriptor_index = descriptor_allocator->Allocate();
				D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor = descriptor_allocator->GetHandle(descriptor_index);

				device->CopyDescriptorsSimple(1, dst_descriptor, diffuse_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetGraphicsRootDescriptorTable(3, descriptor_allocator->GetHandle(descriptor_index));
				mesh.Draw(cmd_list);

			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddGodRaysPass(RenderGraph& rg, Light const& light)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();

		struct GodRaysPassData
		{
			RGTextureReadOnlyId sun_output;
		};

		rg.AddPass<GodRaysPassData>("GodRays Pass",
			[=](GodRaysPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc god_rays_desc{};
				god_rays_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				god_rays_desc.width = width;
				god_rays_desc.height = height;
				god_rays_desc.clear_value = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
				
				builder.DeclareTexture(RG_RES_NAME(GodRaysOutput), god_rays_desc);
				builder.WriteRenderTarget(RG_RES_NAME(GodRaysOutput), ERGLoadStoreAccessOp::Clear_Preserve);
				data.sun_output = builder.ReadTexture(RG_RES_NAME(SunOutput), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](GodRaysPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				auto dynamic_allocator = gfx->GetDynamicAllocator();

				if (light.type != ELightType::Directional)
				{
					ADRIA_LOG(WARNING, "Using God Rays on a Non-Directional Light Source");
					return;
				}

				LightCBuffer light_cbuf_data{};
				{
					light_cbuf_data.godrays_decay = light.godrays_decay;
					light_cbuf_data.godrays_density = light.godrays_density;
					light_cbuf_data.godrays_exposure = light.godrays_exposure;
					light_cbuf_data.godrays_weight = light.godrays_weight;

					auto camera_position = global_data.camera_position;
					XMVECTOR light_position = light.type == ELightType::Directional ?
						XMVector4Transform(light.position, XMMatrixTranslation(XMVectorGetX(camera_position), 0.0f, XMVectorGetY(camera_position)))
						: light.position;

					DirectX::XMVECTOR light_pos_h = XMVector4Transform(light_position, global_data.camera_viewproj);
					DirectX::XMFLOAT4 light_pos{};
					DirectX::XMStoreFloat4(&light_pos, light_pos_h);
					light_cbuf_data.ss_position = XMVectorSet(0.5f * light_pos.x / light_pos.w + 0.5f, -0.5f * light_pos.y / light_pos.w + 0.5f, light_pos.z / light_pos.w, 1.0f);

					static const float32 f_max_sun_dist = 1.3f;
					float32 f_max_dist = (std::max)(abs(XMVectorGetX(light_cbuf_data.ss_position)), abs(XMVectorGetY(light_cbuf_data.ss_position)));
					if (f_max_dist >= 1.0f)
						light_cbuf_data.color = XMVector3Transform(light.color, XMMatrixScaling((f_max_sun_dist - f_max_dist), (f_max_sun_dist - f_max_dist), (f_max_sun_dist - f_max_dist)));
					else light_cbuf_data.color = light.color;
				}

				DynamicAllocation light_allocation = dynamic_allocator->Allocate(GetCBufferSize<LightCBuffer>(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				light_allocation.Update(light_cbuf_data);

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::GodRays));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::GodRays));

				cmd_list->SetGraphicsRootConstantBufferView(0, light_allocation.gpu_address);

				OffsetType descriptor_index = descriptor_allocator->Allocate();
				D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor = context.GetReadOnlyTexture(data.sun_output);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), cpu_descriptor,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetGraphicsRootDescriptorTable(1, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);
			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddLensFlarePass(RenderGraph& rg, Light const& light)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();

		struct LensFlarePassData
		{
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<LensFlarePassData>("LensFlare Pass",
			[=](LensFlarePassData& data, RenderGraphBuilder& builder)
			{
				builder.WriteRenderTarget(RG_RES_NAME(PostprocessMain), ERGLoadStoreAccessOp::Preserve_Preserve);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](LensFlarePassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				auto dynamic_allocator = gfx->GetDynamicAllocator();

				if (light.type != ELightType::Directional)
				{
					ADRIA_LOG(WARNING, "Using Lens Flare on a Non-Directional Light Source");
					return;
				}

				std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> lens_flare_descriptors{};
				for (size_t i = 0; i < lens_flare_textures.size(); ++i)
					lens_flare_descriptors.push_back(texture_manager.CpuDescriptorHandle(lens_flare_textures[i]));

				lens_flare_descriptors.push_back(context.GetReadOnlyTexture(data.depth));

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::LensFlare));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::LensFlare));
				DynamicAllocation light_allocation = dynamic_allocator->Allocate(GetCBufferSize<LightCBuffer>(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				LightCBuffer light_cbuf_data{};
				{
					auto camera_position = global_data.camera_position;
					XMVECTOR light_position = light.type == ELightType::Directional ?
						XMVector4Transform(light.position, XMMatrixTranslation(XMVectorGetX(camera_position), 0.0f, XMVectorGetY(camera_position)))
						: light.position;
					DirectX::XMVECTOR light_pos_h = XMVector4Transform(light_position, global_data.camera_viewproj);
					DirectX::XMFLOAT4 light_pos{};
					DirectX::XMStoreFloat4(&light_pos, light_pos_h);
					light_cbuf_data.ss_position = XMVectorSet(0.5f * light_pos.x / light_pos.w + 0.5f, -0.5f * light_pos.y / light_pos.w + 0.5f, light_pos.z / light_pos.w, 1.0f);
				}
				light_allocation.Update(light_cbuf_data);

				cmd_list->SetGraphicsRootConstantBufferView(0, light_allocation.gpu_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(8);
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1, 1, 1, 1, 1, 1, 1 };
				uint32 dst_range_sizes[] = { 8 };
				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_range_sizes), lens_flare_descriptors.data(), src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(1, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
				cmd_list->DrawInstanced(7, 1, 0, 0);
				
			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddDepthOfFieldPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;
		struct DepthOfFieldPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId blurred_input;
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<DepthOfFieldPassData>("DepthOfField Pass",
			[=](DepthOfFieldPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc dof_output_desc{};
				dof_output_desc.width = width;
				dof_output_desc.height = height;
				dof_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				
				builder.DeclareTexture(RG_RES_NAME(DepthOfFieldOutput), dof_output_desc);
				builder.WriteRenderTarget(RG_RES_NAME(DepthOfFieldOutput), ERGLoadStoreAccessOp::Discard_Preserve);
				data.input = builder.ReadTexture(last_resource, ReadAccess_PixelShader);
				data.blurred_input = builder.ReadTexture(RG_RES_NAME(BlurredDofInput), ReadAccess_PixelShader);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](DepthOfFieldPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::DOF));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::DOF));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);

				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { context.GetReadOnlyTexture(data.input), context.GetReadOnlyTexture(data.blurred_input), context.GetReadOnlyTexture(data.depth) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1, 1 };
				uint32 dst_range_sizes[] = { 3 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);
	}

	void Postprocessor::AddMotionBlurPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;
		struct MotionBlurPassData
		{
			RGTextureReadOnlyId input_srv;
			RGTextureReadOnlyId velocity_srv;
		};
		rg.AddPass<MotionBlurPassData>("Motion Blur Pass",
			[=](MotionBlurPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc motion_blur_desc{};
				motion_blur_desc.width = width;
				motion_blur_desc.height = height;
				motion_blur_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.SetViewport(width, height);
				builder.DeclareTexture(RG_RES_NAME(MotionBlurOutput), motion_blur_desc);
				builder.WriteRenderTarget(RG_RES_NAME(MotionBlurOutput), ERGLoadStoreAccessOp::Discard_Preserve);
				data.input_srv = builder.ReadTexture(last_resource, ReadAccess_PixelShader);
				data.velocity_srv = builder.ReadTexture(RG_RES_NAME(VelocityBuffer), ReadAccess_PixelShader);
			},
			[=](MotionBlurPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::MotionBlur));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::MotionBlur));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(2);

				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { context.GetReadOnlyTexture(data.input_srv), context.GetReadOnlyTexture(data.velocity_srv) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1 };
				uint32 dst_range_sizes[] = { 2 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0); 

			}, ERGPassType::Graphics, ERGPassFlags::None);

	}

	void Postprocessor::AddHistoryCopyPass(RenderGraph& rg)
	{
		struct CopyPassData
		{
			RGTextureCopySrcId copy_src;
			RGTextureCopyDstId copy_dst;
		};
		RGResourceName last_resource = final_resource;

		rg.AddPass<CopyPassData>("History Copy Pass",
			[=](CopyPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc history_desc{};
				history_desc.width = width;
				history_desc.height = height;
				history_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				data.copy_dst = builder.WriteCopyDstTexture(RG_RES_NAME(HistoryBuffer));
				data.copy_src = builder.ReadCopySrcTexture(last_resource);
			},
			[=](CopyPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				Texture const& src_texture = context.GetCopySrcTexture(data.copy_src);
				Texture const& dst_texture = context.GetCopyDstTexture(data.copy_dst);
				cmd_list->CopyResource(dst_texture.GetNative(), src_texture.GetNative());
			}, ERGPassType::Copy, ERGPassFlags::None);
	}

	void Postprocessor::AddTAAPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();
		RGResourceName last_resource = final_resource;

		struct TAAPassData
		{
			RGTextureReadOnlyId input_srv;
			RGTextureReadOnlyId history_srv;
			RGTextureReadOnlyId velocity_srv;
		};

		rg.ImportTexture(RG_RES_NAME(HistoryBuffer), history_buffer.get());
		rg.AddPass<TAAPassData>("TAA Pass",
			[=](TAAPassData& data, RenderGraphBuilder& builder)
			{
				RGTextureDesc taa_desc{};
				taa_desc.width = width;
				taa_desc.height = height;
				taa_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				builder.SetViewport(width, height);
				builder.DeclareTexture(RG_RES_NAME(TAAOutput), taa_desc);
				builder.WriteRenderTarget(RG_RES_NAME(TAAOutput), ERGLoadStoreAccessOp::Discard_Preserve);
				data.input_srv = builder.ReadTexture(last_resource, ReadAccess_PixelShader);
				data.history_srv = builder.ReadTexture(RG_RES_NAME(HistoryBuffer), ReadAccess_PixelShader);
				data.velocity_srv = builder.ReadTexture(RG_RES_NAME(VelocityBuffer), ReadAccess_PixelShader);
			},
			[=](TAAPassData const& data, RenderGraphContext& context, GraphicsDevice* gfx, CommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::TAA));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::TAA));

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), context.GetReadOnlyTexture(data.input_srv),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index + 1), context.GetReadOnlyTexture(data.history_srv),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index + 2), context.GetReadOnlyTexture(data.velocity_srv),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(0, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);

	}

	void Postprocessor::AddGenerateBokehPasses(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();

		struct GenerateBokehPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId depth;
		};
		/*
		rg.AddPass<GenerateBokehPassData>("GenerateBokeh Pass",
			[=](GenerateBokehPassData& data, RenderGraphBuilder& builder)
			{
				TextureDesc dof_output_desc{};
				dof_output_desc.width = width;
				dof_output_desc.height = height;
				dof_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				dof_output_desc.bind_flags = EBindFlag::UnorderedAccess | EBindFlag::RenderTarget | EBindFlag::ShaderResource;
				dof_output_desc.initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

				//builder.DeclareBuffer(RG_RES_NAME(DepthOfFieldOutput), dof_output_desc);
				data.input = builder.ReadTexture(final_resource, ReadAccess_PixelShader);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](GenerateBokehPassData const& data, RenderGraphResources& context, GraphicsDevice* gfx, RGCommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();

				D3D12_RESOURCE_BARRIER prereset_barrier = CD3DX12_RESOURCE_BARRIER::Transition(bokeh_counter.GetNative(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
				cmd_list->ResourceBarrier(1, &prereset_barrier);
				cmd_list->CopyBufferRegion(bokeh_counter.GetNative(), 0, counter_reset_buffer->GetNative(), 0, sizeof(uint32));
				D3D12_RESOURCE_BARRIER postreset_barrier = CD3DX12_RESOURCE_BARRIER::Transition(bokeh_counter.GetNative(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				cmd_list->ResourceBarrier(1, &postreset_barrier);

				cmd_list->SetComputeRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::BokehGenerate));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::BokehGenerate));
				cmd_list->SetComputeRootConstantBufferView(0, frame_cbuffer.View(backbuffer_index).BufferLocation);
				cmd_list->SetComputeRootConstantBufferView(1, postprocess_cbuffer.View(backbuffer_index).BufferLocation);
				cmd_list->SetComputeRootConstantBufferView(2, compute_cbuffer.View(backbuffer_index).BufferLocation);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(2);

				D3D12_RESOURCE_BARRIER dispatch_barriers[] =
				{
					CD3DX12_RESOURCE_BARRIER::Transition(depth_target->GetNative(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(bokeh->GetNative(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				};
				cmd_list->ResourceBarrier(ARRAYSIZE(dispatch_barriers), dispatch_barriers);

				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { postprocess_textures[!postprocess_index]->SRV(), depth_target->SRV() };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1 };
				uint32 dst_range_sizes[] = { 2 };
				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				cmd_list->SetComputeRootDescriptorTable(3, descriptor_allocator->GetHandle(descriptor_index));

				D3D12_CPU_DESCRIPTOR_HANDLE bokeh_uav = bokeh->UAV();
				descriptor_index = descriptor_allocator->Allocate();

				device->CopyDescriptorsSimple(1, descriptor_allocator->GetHandle(descriptor_index), bokeh_uav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetComputeRootDescriptorTable(4, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->Dispatch((uint32)std::ceil(width / 32.0f), (uint32)std::ceil(height / 32.0f), 1);

				CD3DX12_RESOURCE_BARRIER precopy_barriers[] = {
						CD3DX12_RESOURCE_BARRIER::Transition(bokeh->GetNative(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(depth_target->GetNative(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
						CD3DX12_RESOURCE_BARRIER::Transition(bokeh_indirect_draw_buffer->GetNative(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST),
						CD3DX12_RESOURCE_BARRIER::Transition(bokeh_counter.GetNative(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
				};
				cmd_list->ResourceBarrier(ARRAYSIZE(precopy_barriers), precopy_barriers);

				cmd_list->CopyBufferRegion(bokeh_indirect_draw_buffer->GetNative(), 0, bokeh_counter.GetNative(), 0, bokeh_counter.GetDesc().size);

				CD3DX12_RESOURCE_BARRIER postcopy_barriers[] =
				{
						CD3DX12_RESOURCE_BARRIER::Transition(bokeh_indirect_draw_buffer->GetNative(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
						CD3DX12_RESOURCE_BARRIER::Transition(bokeh_counter.GetNative(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
				};
				cmd_list->ResourceBarrier(ARRAYSIZE(postcopy_barriers), postcopy_barriers);

			}, ERGPassType::Compute, ERGPassFlags::None);
			*/
	}

	void Postprocessor::AddDrawBokehPass(RenderGraph& rg)
	{
		GlobalBlackboardData const& global_data = rg.GetBlackboard().GetChecked<GlobalBlackboardData>();

		/*
		struct DepthOfFieldPassData
		{
			RGTextureReadOnlyId input;
			RGTextureReadOnlyId blurred_input;
			RGTextureReadOnlyId depth;
		};

		rg.AddPass<DepthOfFieldPassData>("DrawBokeh Pass",
			[=](DepthOfFieldPassData& data, RenderGraphBuilder& builder)
			{
				TextureDesc dof_output_desc{};
				dof_output_desc.width = width;
				dof_output_desc.height = height;
				dof_output_desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				dof_output_desc.bind_flags = EBindFlag::UnorderedAccess | EBindFlag::RenderTarget | EBindFlag::ShaderResource;
				dof_output_desc.initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

				builder.DeclareTexture(RG_RES_NAME(DepthOfFieldOutput), dof_output_desc);
				builder.WriteRenderTarget(RG_RES_NAME(DepthOfFieldOutput), ERGLoadStoreAccessOp::Discard_Preserve);
				data.input = builder.ReadTexture(final_resource, ReadAccess_PixelShader);
				data.blurred_input = builder.ReadTexture(RG_RES_NAME(BlurredDofInput), ReadAccess_PixelShader);
				data.depth = builder.ReadTexture(RG_RES_NAME(DepthStencil), ReadAccess_PixelShader);
				builder.SetViewport(width, height);
			},
			[=](DepthOfFieldPassData const& data, RenderGraphResources& context, GraphicsDevice* gfx, RGCommandList* cmd_list)
			{
				ID3D12Device* device = gfx->GetDevice();
				auto descriptor_allocator = gfx->GetOnlineDescriptorAllocator();
				cmd_list->SetGraphicsRootSignature(RootSigPSOManager::GetRootSignature(ERootSignature::DOF));
				cmd_list->SetPipelineState(RootSigPSOManager::GetPipelineState(EPipelineStateObject::DOF));

				cmd_list->SetGraphicsRootConstantBufferView(0, global_data.frame_cbuffer_address);
				cmd_list->SetGraphicsRootConstantBufferView(1, global_data.postprocess_cbuffer_address);

				OffsetType descriptor_index = descriptor_allocator->AllocateRange(3);

				D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[] = { resources.GetDescriptor(data.input), resources.GetDescriptor(data.blurred_input), resources.GetDescriptor(data.depth) };
				D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[] = { descriptor_allocator->GetHandle(descriptor_index) };
				uint32 src_range_sizes[] = { 1, 1, 1 };
				uint32 dst_range_sizes[] = { 3 };

				device->CopyDescriptors(ARRAYSIZE(dst_ranges), dst_ranges, dst_range_sizes, ARRAYSIZE(src_ranges), src_ranges, src_range_sizes,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

				cmd_list->SetGraphicsRootDescriptorTable(2, descriptor_allocator->GetHandle(descriptor_index));
				cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				cmd_list->DrawInstanced(4, 1, 0, 0);

			}, ERGPassType::Graphics, ERGPassFlags::None);
			*/
	}

}

