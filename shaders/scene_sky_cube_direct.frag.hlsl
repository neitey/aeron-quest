/* scene_sky_cube.frag.hlsl, tonemapped in place for direct output. */
#include "scene_direct_output.hlsli"

cbuffer SkyCubePS : register(b0, space3)
{
    float4 params;
};

TextureCube<float4> g_sky     : register(t0, space2);
SamplerState        g_sampler : register(s0, space2);

float4 main(float4 pos : SV_Position,
            float3 dir : TEXCOORD0) : SV_Target
{
    float3 d = normalize(dir);
    float4 c = g_sky.Sample(g_sampler, d);
    return direct_output(c.rgb * params.x, 1.0f, pos.xy);
}
