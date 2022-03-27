#include "../Globals/GlobalsRT.hlsli"

RaytracingAccelerationStructure rt_scene : register(t0);
Texture2D depth_tx : register(t1);
RWTexture2D<float> shadow_rt_output : register(u0);

struct ShadowRayData
{
    bool hit;
};


[shader("raygeneration")]
void RTS_RayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint2 launchDim = DispatchRaysDimensions().xy;

    float depth = depth_tx.Load(int3(launchIndex.xy, 0)).r;
    float2 tex_coords = launchDim / frame_cbuf.screen_resolution;

    float3 posView = GetPositionVS(tex_coords, depth);
    float4 posWorld = mul(float4(posView, 1.0f), frame_cbuf.inverse_view);
    posWorld /= posWorld.w;

    Light light = light_cbuf.current_light;
    float3 direction;
    float maxT;

    switch (light.type)
    {
    case POINT_LIGHT:
        direction = light.position.xyz - posWorld.xyz;
        maxT = length(direction);
        break;
    case DIRECTIONAL_LIGHT:
        direction = -light.direction.xyz;
        maxT = 10000.0f;
        break;
    case SPOT_LIGHT:
        direction = -light.direction.xyz;
        maxT = length(light.position.xyz - posWorld.xyz);
        break;
    }

    RayDesc ray;
    ray.Origin = posWorld.xyz;
    ray.Direction = normalize(direction);
    ray.TMin = 0.01;
    ray.TMax = max(0.005, maxT);

    ShadowRayData payload;
    payload.hit = true;
    TraceRay(rt_scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 0, 1, 0, ray, payload);
    shadow_rt_output[launchIndex.xy] = payload.hit ? 0.0f : 1.0f;
}

[shader("miss")]
void RTS_Miss(inout ShadowRayData hitData)
{
    hitData.hit = false;
}


[shader("anyhit")]
void RTS_Anyhit(inout ShadowRayData hitData, in BuiltInTriangleIntersectionAttributes attribs)
{
    hitData.hit = true;
}








