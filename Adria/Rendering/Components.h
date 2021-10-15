#pragma once
#include <memory>
#include "Enums.h"
#include "../Core/Definitions.h"
#include <DirectXCollision.h>
#include "../Graphics/VertexTypes.h"
#include "../Graphics/VertexBuffer.h"
#include "../Graphics/IndexBuffer.h"
#include "../Graphics/TextureManager.h"
#include <unordered_map>


#define COMPONENT 

namespace adria
{
	struct COMPONENT Transform
	{
		DirectX::XMMATRIX starting_transform = DirectX::XMMatrixIdentity();
		DirectX::XMMATRIX current_transform = DirectX::XMMatrixIdentity();
	};

	struct COMPONENT Mesh
	{
		std::shared_ptr<VertexBuffer>	vertex_buffer = nullptr;
		std::shared_ptr<IndexBuffer>	index_buffer = nullptr;
		std::shared_ptr<VertexBuffer>   instance_buffer = nullptr;
		//only vb
		u32 vertex_count = 0;
		u32 start_vertex_location = 0; //Index of the first vertex

		//vb/ib
		u32 indices_count = 0;
		u32 start_index_location = 0; //The location of the first index read by the GPU from the index buffer
		i32 base_vertex_location = 0;  //A value added to each index before reading a vertex from the vertex buffer

		//instancing
		u32 instance_count = 1;
		u32 start_instance_location = 0; //A value added to each index before reading per-instance data from a vertex buffer

		D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		void Draw(ID3D12GraphicsCommandList* cmd_list) const
		{
			cmd_list->IASetPrimitiveTopology(topology);

			vertex_buffer->Bind(cmd_list, 0);

			if (index_buffer)
			{
				index_buffer->Bind(cmd_list);
				cmd_list->DrawIndexedInstanced(indices_count, instance_count, start_index_location, base_vertex_location, start_instance_location);
			}
			else cmd_list->DrawInstanced(vertex_count, instance_count, start_vertex_location, start_instance_location);
		}

		void Draw(ID3D12GraphicsCommandList* cmd_list, D3D12_PRIMITIVE_TOPOLOGY override_topology) const
		{
			cmd_list->IASetPrimitiveTopology(override_topology);

			vertex_buffer->Bind(cmd_list, 0);

			if (index_buffer)
			{
				index_buffer->Bind(cmd_list);
				cmd_list->DrawIndexedInstanced(indices_count, instance_count, start_index_location, base_vertex_location, start_instance_location);
			}
			else cmd_list->DrawInstanced(vertex_count, instance_count, start_vertex_location, start_instance_location);
		}
	};

	struct COMPONENT Material
	{
		TEXTURE_HANDLE normal_texture			  = INVALID_TEXTURE_HANDLE;
		TEXTURE_HANDLE albedo_texture		      = INVALID_TEXTURE_HANDLE;
		TEXTURE_HANDLE metallic_roughness_texture = INVALID_TEXTURE_HANDLE;
		TEXTURE_HANDLE emissive_texture			  = INVALID_TEXTURE_HANDLE;

		f32 albedo_factor		= 1.0f;
		f32 metallic_factor		= 1.0f;
		f32 roughness_factor	= 1.0f;
		f32 emissive_factor		= 1.0f;

		DirectX::XMFLOAT3 diffuse = DirectX::XMFLOAT3(1, 1, 1);
		EPipelineStateObject pso = EPipelineStateObject::Unknown;
	};

	struct COMPONENT Light
	{
		DirectX::XMVECTOR position	= DirectX::XMVectorSet(0, 0, 0, 1);
		DirectX::XMVECTOR direction	= DirectX::XMVectorSet(0, -1, 0, 0);
		DirectX::XMVECTOR color		= DirectX::XMVectorSet(1, 1, 1, 1);
		f32 range = 100.0f;
		f32 energy = 1.0f;
		ELightType type = ELightType::Directional;
		f32 outer_cosine;
		f32 inner_cosine;
		bool casts_shadows = false;
		bool use_cascades = false;
		bool active = true;
		f32 volumetric_strength = 1.0f;
		bool volumetric = false;
		bool lens_flare = false;
		bool god_rays = false;
		f32 godrays_decay = 0.825f;
		f32 godrays_weight = 0.25f;
		f32 godrays_density = 0.975f;
		f32 godrays_exposure = 2.0f;
		bool screenspace_shadows = false;
	};

	struct COMPONENT Visibility
	{
		DirectX::BoundingBox aabb;
		bool camera_visible = true;
		bool light_visible = true;
	};

	struct COMPONENT Skybox
	{
		TEXTURE_HANDLE cubemap_texture;
		bool active;
	};
	
	struct COMPONENT Ocean {};

	struct COMPONENT Deferred {};

	struct COMPONENT Forward
	{
		bool transparent;
	};

	struct COMPONENT Tag
	{
		std::string name = "default";
	};

	struct COMPONENT RayTracing
	{

	};
}