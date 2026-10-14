/*
 * Lite fragment shader for the glTF (PBR) mesh path, for mobile VR.
 *
 * Same resources, inputs and output as scene_pbr_mesh_impl.hlsli (it is a
 * drop-in replacement in the same pipelines); cheaper per pixel:
 *
 *   - Atlas samples use SampleLevel with the LOD computed from the UV
 *     derivatives instead of SampleGrad (explicit-gradient sampling runs
 *     at a reduced rate on mobile GPUs). No anisotropy.
 *   - The Cook-Torrance specular (and the derivative-based face normal it
 *     needs) only runs for materials that can show a highlight
 *     (roughness < 0.9). Runtime-converted OPT materials are fully rough
 *     (roughness 1): their specular is a faint, broad sheen of a few
 *     percent, dropped.
 *   - Colour math in min16float (fp16 on the GPU); positions stay fp32.
 *
 * Shadows, SSAO, point lights, emissive, alpha modes and the XvT flat
 * look behave as in the full shader. Debug views are not available.
 */

#define AERON_DIRECTIONAL_SHADOW_UNIFORM_REGISTER b0
#define AERON_PBR_LIGHT_UNIFORM_REGISTER b1
#define AERON_PBR_MATERIAL_REGISTER t8
#define AERON_PBR_VARIANT_REGISTER t9
#define AERON_PBR_POINT_LIGHT_REGISTER t10
#define AERON_CLUSTER_HEADER_REGISTER t11
#define AERON_CLUSTER_INDEX_REGISTER t12
#include "scene_pbr_lighting.hlsli"
#include "scene_pbr_atlas_sample.hlsli"
#include "scene_pbr_vsout.hlsli"
#include "srgb.hlsli"
#include "scene_pbr_material_alpha.hlsli"

Texture2D    g_normal       : register(t1, space2);
SamplerState g_normal_sampler : register(s1, space2);
Texture2D    g_mr           : register(t2, space2);
SamplerState g_mr_sampler   : register(s2, space2);
Texture2D    g_emissive     : register(t3, space2);
SamplerState g_em_sampler   : register(s3, space2);
Texture2D<float2> g_ao      : register(t4, space2);
SamplerState g_ao_sampler   : register(s4, space2);
TextureCube<float4> g_environment : register(t7, space2);
SamplerState g_environment_sampler : register(s7, space2);

#ifndef AERON_PBR_DIRECT_OUTPUT
#define AERON_PBR_DIRECT_OUTPUT 0
#endif
#if AERON_PBR_DIRECT_OUTPUT
#include "scene_direct_output.hlsli"
#endif

/* atlas_sample() with an explicit LOD (see the header). */
float4 atlas_sample_lod(Texture2D tex, SamplerState s, float2 uv, float4 sub_rect)
{
    float2 wrapped  = frac(uv);
    float2 atlas_uv = sub_rect.xy + wrapped * sub_rect.zw;
    float tw, th;
    tex.GetDimensions(tw, th);
    float2 scale = sub_rect.zw * float2(tw, th);
    float2 dx = ddx(uv) * scale;
    float2 dy = ddy(uv) * scale;
    float lod = 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1.0e-8f));
    return tex.SampleLevel(s, atlas_uv, lod);
}

float3 detailed_environment(float3 world_direction)
{
	if (environment_params.x <= 0.0f) return 0.0f;
	float3 local_direction = float3(dot(world_direction, environment_right.xyz),
									dot(world_direction, environment_up.xyz),
									dot(world_direction, environment_forward.xyz));
	return max(g_environment.SampleLevel(g_environment_sampler, normalize(local_direction), 0.0f).rgb,
			   0.0f) * environment_params.x;
}

struct FSOut
{
    float4 color : SV_Target0;
};

FSOut main(PbrForwardVSOut i, bool is_front : SV_IsFrontFace)
{
    FSOut _out;

    GltfMaterial m = pbr_resolve_material(
        i.prim_id, i.variant_row_base, i.variant_group_count,
        i.material_count);

    /* ===== Base color ============================================ */
    float4 albedo_tex = (m.base_rect.z > 0.0f && m.base_rect.w > 0.0f)
        ? atlas_sample_lod(g_base_color, g_base_sampler, i.uv, m.base_rect)
        : float4(1, 1, 1, 1);
#if AERON_PBR_ALPHA_MASK
    pbr_apply_alpha_mask(albedo_tex.a * m.base_color_factor.a,
                         m.metal_rough.z);
#endif
    min16float3 albedo = (min16float3)(albedo_tex.rgb * m.base_color_factor.rgb);

    /* ===== Normal ================================================ */
    float  side_sign = is_front ? 1.0f : -1.0f;
    float3 N_geom = normalize(i.world_normal) * side_sign;
    float3 N = N_geom;
    if ((m.flags & GLTF_MATERIAL_HAS_NORMAL) != 0u) {
        float3 ntex = atlas_sample_lod(g_normal, g_normal_sampler,
                                       i.uv, m.normal_rect).rgb * 2.0f - 1.0f;
        float  nz   = sqrt(saturate(1.0f - dot(ntex.xy, ntex.xy)));
        float3 nt   = float3(ntex.xy, max(ntex.z, nz));
        float3 T    = normalize(i.world_tangent);
        float3 B    = normalize(cross(N_geom, T)) * i.tangent_sign;
        N = normalize(T * nt.x + B * nt.y + N_geom * nt.z);
    }

    /* ===== Diffuse =============================================== */
    float3 L = fs_directional_dir;
    min16float ndotl = (min16float)saturate(dot(N, L));
    min16float wrap_n = saturate((ndotl + (min16float)light_wrap) / ((min16float)1.0f + (min16float)light_wrap));
    wrap_n = lerp(wrap_n, wrap_n * wrap_n, (min16float)light_wrap);
    min16float lambert_term = saturate(wrap_n * (min16float)light_intensity);
    min16float3 ambient_rgb = (min16float3)(world_ambient(N) + detailed_environment(N));

    min16float ao = 1.0f;
    min16float screen_shadow_visibility = 1.0f;
    if (ssao_intensity > 0.0f) {
        float2 ao_uv = i.position.xy / float2(ssao_rt_w, ssao_rt_h);
        float2 visibility = g_ao.Sample(g_ao_sampler, ao_uv);
        screen_shadow_visibility = (min16float)visibility.y;
        ao = (min16float)lerp(1.0f, pow(visibility.x, ssao_power), ssao_intensity);
    }

    /* The face normal (screen-space derivatives) only where it is used. */
    float3 world_pos_dx = 0.0f;
    float3 world_pos_dy = 0.0f;
    float3 N_face = N_geom;
    const bool spec_on = m.metal_rough.y < 0.9f || (m.flags & GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS) != 0u;
    if (spec_on || (i.receive_shadow != 0u && i.screen_shadow == 0u)) {
        world_pos_dx = ddx(i.world_pos);
        world_pos_dy = ddy(i.world_pos);
        float3 face_cross = cross(world_pos_dx, world_pos_dy);
        float face_length_sq = dot(face_cross, face_cross);
        if (face_length_sq > 1.0e-12f) {
            N_face = face_cross * rsqrt(face_length_sq);
            N_face = (dot(N_face, N_geom) < 0.0f) ? -N_face : N_face;
        }
    }

    min16float shadow_visibility = 1.0f;
    if (i.receive_shadow != 0u) {
        if (i.screen_shadow != 0u) {
            shadow_visibility = screen_shadow_visibility;
        } else {
            uint shadow_cascade = 4u;
            float shadow_cascade_blend = 0.0f;
            float shadow_coverage = 0.0f;
            shadow_visibility = (min16float)directional_shadow_visibility(
                i.world_pos, N_face, 1.0f,
                world_pos_dx, world_pos_dy, i.position.xy, shadow_cascade,
                shadow_cascade_blend, shadow_coverage);
        }
    }

    min16float ao_direct = lerp((min16float)1.0f, ao, (min16float)ssao_direct);
    min16float3 extra_rgb = 0.0f;
    [unroll] for (int ed = 0; ed < 3; ed++) {
        extra_rgb += (min16float)saturate(dot(N, fs_extra_dir[ed].xyz)) * (min16float3)fs_extra_col[ed].rgb;
    }
    min16float3 base_rgb = albedo *
        (((min16float3)sun_color * lambert_term * shadow_visibility + extra_rgb) * ao_direct + ambient_rgb * ao +
         (min16float3)i.local_rgb);

    /* ===== Specular (only where a highlight can show) ============= */
    float3 V = normalize(fs_camera_pos_world - i.world_pos);
    PbrMaterialParams mp;
    mp.spec_intensity = 1.0f;
    mp.roughness      = m.metal_rough.y;
    mp.F0             = float3(0.04f, 0.04f, 0.04f);
    mp.albedo         = (float3)albedo;
    float metallic = m.metal_rough.x;
    float3 spec_rgb = 0.0f;
    float3 N_spec = N;
    if (spec_on) {
        if ((m.flags & GLTF_MATERIAL_HAS_METALLIC_ROUGHNESS) != 0u) {
            float3 mr = atlas_sample_lod(g_mr, g_mr_sampler, i.uv, m.mr_rect).rgb;
            mp.roughness = mr.g * m.metal_rough.y;
            metallic     = mr.b * m.metal_rough.x;
        }
        mp.roughness = max(mp.roughness, 0.045f);
        mp.F0 = lerp(float3(0.04f, 0.04f, 0.04f), (float3)albedo, metallic);
        N_spec = normalize(lerp(N_face, N, smoothstep(0.0f, 0.2f, dot(N, V))));
        float G_term;
        spec_rgb = cook_torrance_spec(N_spec, V, L, mp, G_term) *
                   (sun_color * mp.spec_intensity * global_spec_mul) * (float)shadow_visibility;
    } else {
        mp.F0 = lerp(float3(0.04f, 0.04f, 0.04f), (float3)albedo, metallic);
    }
    base_rgb *= (min16float)(1.0f - metallic);

    float3 lit = (float3)base_rgb;
    if (fs_cluster_point_count > 0u) {
        float3 point_diff = accumulate_point_lights(N, N_spec, V, i.world_pos,
                                                    i.position.xy, mp,
                                                    spec_on ? global_spec_mul : 0.0f, spec_rgb);
        lit += (float3)albedo * (1.0f - metallic) * point_diff;
    }
    lit += spec_rgb;

    if (xvt_flat != 0.0f) {
        const float xvt_ambient = 0.08f;
        float xndotl = saturate(dot(N, L)) * light_intensity * (float)shadow_visibility;
        float shade = xvt_ambient + (1.0f - xvt_ambient) * xndotl;
        shade = round(shade * 15.0f) / 15.0f;
        lit = (float3)albedo * (sun_color * shade + i.local_rgb);
    }

    /* ===== Emissive (as the full shader) ========================== */
    if (xvt_flat == 0.0f && i.base_color_emissive_strength > 0.0f) {
        lit = (float3)albedo * i.base_color_emissive_strength;
    } else if (xvt_flat == 0.0f) {
        float3 emissive = m.emissive_packed.rgb;
        float emissive_coverage = 0.0f;
        if ((m.flags & GLTF_MATERIAL_HAS_EMISSIVE) != 0u) {
            float4 emissive_tex = atlas_sample_lod(g_emissive, g_em_sampler,
                                                   i.uv, m.emissive_rect);
            emissive_coverage = emissive_tex.a;
            float3 emissive_rgb = emissive_tex.rgb;
            if ((m.flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
                if (emissive_coverage > 1.0e-5f) {
                    float3 glow_linear = saturate(emissive_rgb / emissive_coverage);
                    float3 glow_srgb = AeronLinearToSrgb(glow_linear);
                    float coverage = saturate(emissive_coverage);
                    emissive_rgb = AeronSrgbToLinear(glow_srgb * coverage * coverage);
                } else {
                    emissive_rgb = 0.0f;
                }
            }
            emissive *= emissive_rgb;
        }
        float emissive_mul = i.emissive_mul;
        emissive *= m.emissive_packed.a * emissive_mul;
        if ((m.flags & GLTF_MATERIAL_LEGACY_EMISSIVE) != 0u) {
            lit = emissive + lit * (1.0f - saturate(emissive_coverage * emissive_mul));
        } else {
            lit += emissive;
        }
    }

    float alpha = ((m.flags & GLTF_MATERIAL_ALPHA_BLEND) != 0u)
                      ? saturate(albedo_tex.a * m.base_color_factor.a)
                      : 1.0f;
#if AERON_PBR_DIRECT_OUTPUT
    _out.color = direct_output(lit, alpha, i.position.xy);
#else
    _out.color = float4(lit, alpha);
#endif
    return _out;
}
