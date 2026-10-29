/* RGBA8 -> ASTC 4x4 (see astc_transcode.h). */
#include "astc_transcode.h"
#include <SDL3/SDL_stdinc.h>

#if defined(AERON_HAS_ASTCENC)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mutex>
#include <thread>
#include <vector>

#include "astcenc.h"

extern "C" {
#include "aeron/log.h"
}

namespace {

std::mutex g_lock;
astcenc_context* g_ctx[2]; /* [srgb] */
unsigned g_threads;
int g_failed;

float astc_quality() {
	const char* q = SDL_getenv("AERON_ASTC_QUALITY");
	if (q && strcmp(q, "fast") == 0) {
		return ASTCENC_PRE_FAST;
	}
	if (q && strcmp(q, "medium") == 0) {
		return ASTCENC_PRE_MEDIUM;
	}
	return ASTCENC_PRE_FASTEST;
}

astcenc_context* astc_context(int srgb) {
	if (g_ctx[srgb]) {
		return g_ctx[srgb];
	}
	if (g_failed) {
		return nullptr;
	}
	unsigned hw = std::thread::hardware_concurrency();
	g_threads = hw >= 6 ? 4u : (hw >= 2 ? hw - 1u : 1u);
	astcenc_config config;
	astcenc_error e = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, astc_quality(),
										  ASTCENC_FLG_USE_DECODE_UNORM8, &config);
	if (e == ASTCENC_SUCCESS) {
		e = astcenc_context_alloc(&config, g_threads, &g_ctx[srgb], nullptr);
	}
	if (e != ASTCENC_SUCCESS) {
		Aeron_LogError("aeron.astc", "encoder setup failed: %s", astcenc_get_error_string(e));
		g_failed = 1;
		return nullptr;
	}
	return g_ctx[srgb];
}

/* Cache: one file per encoded image, named by a hash of its pixels. */
uint64_t astc_hash(const uint8_t* rgba, size_t n, int w, int h, int srgb) {
	uint64_t x = 0xcbf29ce484222325ull ^ ((uint64_t)w << 32) ^ (uint64_t)h ^ ((uint64_t)srgb << 63) ^
				 (uint64_t)(astc_quality() * 1000.0f);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t v;
		memcpy(&v, rgba + i, 8);
		x = (x ^ v) * 0x100000001b3ull;
		x ^= x >> 29;
	}
	for (; i < n; ++i) {
		x = (x ^ rgba[i]) * 0x100000001b3ull;
	}
	return x;
}

bool astc_cache_path(uint64_t key, char* path, size_t size) {
	const char* dir = SDL_getenv("AERON_ASTC_CACHE_DIR");
	if (!dir || !dir[0]) {
		return false;
	}
	static bool made;
	if (!made) {
		mkdir(dir, 0775);
		made = true;
	}
	snprintf(path, size, "%s/%016llx.astc", dir, (unsigned long long)key);
	return true;
}

uint8_t* astc_cache_read(const char* path, size_t expected) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return nullptr;
	}
	uint8_t* data = (uint8_t*)malloc(expected);
	const bool ok = data && fread(data, 1, expected, f) == expected && fgetc(f) == EOF;
	fclose(f);
	if (!ok) {
		free(data);
		return nullptr;
	}
	return data;
}

void astc_cache_write(const char* path, const uint8_t* data, size_t size) {
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.new", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return;
	}
	const bool ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
	}
}

} // namespace

extern "C" int AeronAstc_Available(void) { return 1; }

extern "C" uint8_t* AeronAstc_Encode4x4(const uint8_t* rgba, int w, int h, int srgb, size_t* out_size) {
	if (!rgba || w <= 0 || h <= 0 || !out_size) {
		return nullptr;
	}
	srgb = srgb ? 1 : 0;
	const size_t blocks = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4);
	const size_t size = blocks * 16u;
	const uint64_t key = astc_hash(rgba, (size_t)w * (size_t)h * 4u, w, h, srgb);
	char path[1024];
	const bool cached = astc_cache_path(key, path, sizeof path);
	if (cached) {
		uint8_t* hit = astc_cache_read(path, size);
		if (hit) {
			*out_size = size;
			return hit;
		}
	}
	std::lock_guard<std::mutex> guard(g_lock);
	astcenc_context* ctx = astc_context(srgb);
	if (!ctx) {
		return nullptr;
	}
	uint8_t* out = (uint8_t*)malloc(size);
	if (!out) {
		return nullptr;
	}
	void* slices[1] = { (void*)rgba };
	astcenc_image image;
	image.dim_x = (unsigned)w;
	image.dim_y = (unsigned)h;
	image.dim_z = 1;
	image.data_type = ASTCENC_TYPE_U8;
	image.data = slices;
	const astcenc_swizzle swizzle = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
	/* Small images on one thread; large ones on the context's workers. */
	const unsigned threads = blocks >= 4096 ? g_threads : 1u;
	std::vector<astcenc_error> results(threads, ASTCENC_SUCCESS);
	if (threads == 1) {
		results[0] = astcenc_compress_image(ctx, &image, &swizzle, out, size, 0);
	} else {
		std::vector<std::thread> workers;
		for (unsigned t = 0; t < threads; ++t) {
			workers.emplace_back([&, t] { results[t] = astcenc_compress_image(ctx, &image, &swizzle, out, size, t); });
		}
		for (auto& worker : workers) {
			worker.join();
		}
	}
	astcenc_compress_reset(ctx);
	for (astcenc_error e : results) {
		if (e != ASTCENC_SUCCESS) {
			Aeron_LogError("aeron.astc", "%dx%d image: %s", w, h, astcenc_get_error_string(e));
			free(out);
			return nullptr;
		}
	}
	if (cached) {
		astc_cache_write(path, out, size);
	}
	*out_size = size;
	return out;
}

#else

extern "C" int AeronAstc_Available(void) { return 0; }

extern "C" uint8_t* AeronAstc_Encode4x4(const uint8_t* rgba, int w, int h, int srgb, size_t* out_size) {
	(void)rgba;
	(void)w;
	(void)h;
	(void)srgb;
	(void)out_size;
	return nullptr;
}

#endif
