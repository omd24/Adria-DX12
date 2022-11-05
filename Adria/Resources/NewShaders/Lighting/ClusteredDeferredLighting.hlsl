#include "LightRadiance.hlsli"
#include "../Packing.hlsli"

#define BLOCK_SIZE 16

struct LightGrid
{
	uint offset;
	uint lightCount;
};

struct ClusteredDeferredLightingConstants
{
	uint lightIndexListIdx;
	uint lightGridIdx;
	uint normalMetallicIdx;
	uint diffuseIdx;
	uint depthIdx;
	uint outputIdx;
};
ConstantBuffer<ClusteredDeferredLightingConstants> PassCB : register(b1);

struct CS_INPUT
{
	uint3 GroupId : SV_GroupID;
	uint3 GroupThreadId : SV_GroupThreadID;
	uint3 DispatchThreadId : SV_DispatchThreadID;
	uint  GroupIndex : SV_GroupIndex;
};

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void ClusteredDeferredLighting(CS_INPUT input)
{
	Texture2D               normalMetallicTx = ResourceDescriptorHeap[PassCB.normalMetallicIdx];
	Texture2D               diffuseTx = ResourceDescriptorHeap[PassCB.diffuseIdx];
	Texture2D<float>        depthTx = ResourceDescriptorHeap[PassCB.depthIdx];
	StructuredBuffer<Light> lights = ResourceDescriptorHeap[FrameCB.lightsIdx];
	StructuredBuffer<uint> lightIndexList = ResourceDescriptorHeap[PassCB.lightIndexListIdx];
	StructuredBuffer<LightGrid> lightGrid = ResourceDescriptorHeap[PassCB.lightGridIdx];

	float2 uv = ((float2) input.DispatchThreadId.xy + 0.5f) * 1.0f / (FrameCB.screenResolution);

	float4 normalMetallic = normalMetallicTx.Sample(LinearWrapSampler, uv);
	float3 normal = 2.0f * normalMetallic.rgb - 1.0f;
	float  metallic = normalMetallic.a;
	float  depth = depthTx.Sample(LinearWrapSampler, uv);

	float3 viewPosition = GetViewPosition(uv, depth);
	float4 albedoRoughness = diffuseTx.Sample(LinearWrapSampler, uv);
	float3 V = normalize(float3(0.0f, 0.0f, 0.0f) - viewPosition);
	float3 albedo = albedoRoughness.rgb;
	float  roughness = albedoRoughness.a;
	float linearDepth = LinearizeDepth(depth);


	uint zCluster = uint(max((log2(linearDepth) - log2(FrameCB.cameraNear)) * 16.0f / log2(FrameCB.cameraFar / FrameCB.cameraNear), 0.0f));
	uint2 clusterDim = ceil(FrameCB.screenResolution / float2(16, 16));
	uint3 tiles = uint3(uint2(((float2) input.DispatchThreadId.xy + 0.5f) / clusterDim), zCluster);

	uint tileIndex = tiles.x +
		16 * tiles.y +
		(256) * tiles.z;

	uint lightCount = lightGrid[tileIndex].lightCount;
	uint lightOffset = lightGrid[tileIndex].offset;

	float3 totalRadiance = float3(0.0f, 0.0f, 0.0f);
	for (uint i = 0; i < lightCount; i++)
	{
		uint lightIndex = lightIndexList[lightOffset + i];
		Light light = lights[lightIndex];
		if (!light.active) continue;
		totalRadiance += LightRadiance(light, viewPosition, normal, V, albedo, metallic, roughness);
	}

	RWTexture2D<float4> outputTx = ResourceDescriptorHeap[PassCB.outputIdx];
	outputTx[input.DispatchThreadId.xy] += float4(totalRadiance, 1.0f);
}