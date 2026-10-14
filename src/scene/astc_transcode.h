#ifndef AERON_SCENE_ASTC_TRANSCODE_H
#define AERON_SCENE_ASTC_TRANSCODE_H

/*
 * RGBA8 -> ASTC 4x4 compression for mobile GPUs (which sample ASTC but not
 * the BCn formats desktop assets use), with an optional on-disk cache so a
 * texture is only ever compressed once per device.
 *
 *   AERON_ASTC_CACHE_DIR  directory for the cache (created; unset = none)
 *   AERON_ASTC_QUALITY    fastest (default), fast, medium
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the encoder is compiled in. */
int AeronAstc_Available(void);

/* Compresses a tightly packed w x h RGBA8 image (sRGB-encoded colour when
 * `srgb`) into ASTC 4x4 blocks: ((w+3)/4) * ((h+3)/4) * 16 bytes, returned
 * as a malloc'd buffer (free() it) with its size in *out_size. Looks in /
 * fills the cache. NULL on failure. */
uint8_t* AeronAstc_Encode4x4(const uint8_t* rgba, int w, int h, int srgb, size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
