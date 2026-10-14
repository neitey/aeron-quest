/* scene_billboard3d.frag.hlsl, tonemapped in place for direct output.
 * BB_DIRECT_BLEND: 0 alpha (straight), 1 additive, 2 premultiplied. */
#include "scene_direct_output.hlsli"

Texture2D<float4> g_atlas : register(t0, space2);
SamplerState      s_atlas : register(s0, space2);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float4 main(VSOut i) : SV_Target
{
    float4 c = g_atlas.Sample(s_atlas, i.uv) * i.color;
    if (c.a < 0.01f)
        discard;
#if BB_DIRECT_BLEND == 2
    return direct_output_pma(c);
#else
    return float4(direct_tonemap(c.rgb), c.a);
#endif
}
