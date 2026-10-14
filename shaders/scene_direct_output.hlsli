/*
 * Direct output: the scene's shaders tonemap their own result and the
 * forward pass writes display colours straight into the (sRGB) output
 * image, instead of HDR into an intermediate that a full-screen pass
 * tonemaps afterwards (AeronScene_SetDirectOutput). Same operators and
 * parameters as scene_tonemap.frag.hlsl; no bloom. Blending then happens
 * in display space.
 */
#ifndef AERON_SCENE_DIRECT_OUTPUT_HLSLI
#define AERON_SCENE_DIRECT_OUTPUT_HLSLI

#include "srgb.hlsli"
#include "scene_tonemap_agx.hlsli"

cbuffer DirectOutputPS : register(b3, space3)
{
    /* x = exposure, y = operator (0 ACES fitted, 1 AgX), z = AgX EOTF
     * exponent, w = ACES pre-exposure. */
    float4 direct_params;
    /* x = AgX look, y = punchy power, z = punchy saturation. */
    float4 direct_agx;
    /* rgb = gain after the tonemap. */
    float4 direct_tint;
};

static const float3x3 DIRECT_ACES_INPUT_MATRIX = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f
);

static const float3x3 DIRECT_ACES_OUTPUT_MATRIX = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f
);

float3 direct_tonemap(float3 hdr)
{
    float3 rgb = max(hdr, 0.0f) * direct_params.x;
    if (direct_params.y < 0.5f) {
        float3 c = mul(DIRECT_ACES_INPUT_MATRIX, rgb * direct_params.w);
        float3 a = c * (c + 0.0245786f) - 0.000090537f;
        float3 b = c * (0.983729f * c + 0.4329510f) + 0.238081f;
        rgb = saturate(mul(DIRECT_ACES_OUTPUT_MATRIX, a / b));
    } else {
        rgb = AeronAgxToneMap(rgb, AERON_AGX_MAX_EV, direct_params.z, direct_agx.x, direct_agx.y, direct_agx.z);
    }
    return saturate(rgb) * direct_tint.rgb;
}

/* Interleaved gradient noise, as the tonemap pass dithers. */
float direct_dither(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

/* Straight-alpha result (alpha blending or opaque). */
float4 direct_output(float3 hdr, float alpha, float2 pixel)
{
    float3 rgb = direct_tonemap(hdr);
    float d = (direct_dither(pixel) - 0.5f) * (1.0f / 255.0f);
    rgb = AeronSrgbToLinear(saturate(AeronLinearToSrgb(rgb) + d));
    return float4(rgb, alpha);
}

/* Premultiplied result: the premultiplied colour is the light this
 * texel adds (HDR emission scaled into it), so it is tonemapped as is and
 * blended src + dst * (1 - a). Tonemapping the straight colour and
 * multiplying by alpha back would cap every texel at its alpha and dim
 * faint glows. */
float4 direct_output_pma(float4 c)
{
    return float4(direct_tonemap(c.rgb), c.a);
}

#endif
