/* Headless check of Aeron's OpenXR stereo protocol: clears the left eye red,
 * the right eye green, draws a translucent HUD box, and commits each frame.
 * Run with AERON_XR=1 and AERON_XR_CAPTURE to dump the eye images. */
#include "aeron/aeron.h"
#include "aeron/render.h"
#include "aeron/xr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void clear_target(AeronCommandBuffer* cmd, AeronRenderTarget* target, float r, float g, float b,
						 float a) {
	AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target     = target,
		.clear_color      = 1,
		.clear_color_rgba = { r, g, b, a },
		.command_buffer   = cmd,
		.debug_label      = "xr_stereo_test clear",
	});
	if (pass) {
		Aeron_EndRenderPass(pass);
	}
}

int main(int argc, char** argv) {
	int         frames = argc > 1 ? atoi(argv[1]) : 120;
	AeronConfig config;
	memset(&config, 0, sizeof config);
	config.org_name          = "Aeron";
	config.app_name          = "XrStereoTest";
	config.shader_path       = XR_STEREO_TEST_SHADER_RELATIVE_DIR;
	config.window_title      = "Aeron XR stereo test";
	config.logical_width     = 1280;
	config.logical_height    = 720;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;
	config.xr_enabled        = 1;
	if (!Aeron_Init(&config)) {
		fprintf(stderr, "Aeron_Init failed\n");
		return 1;
	}
	int stereo_frames = 0;
	for (int i = 0; i < frames && !Aeron_QuitRequested() && !Aeron_FatalErrorRequested(); ++i) {
		Aeron_BeginFrame();
		AeronXrStereoFrame frame;
		if (Aeron_XrStereoBegin(&frame)) {
			AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
			clear_target(cmd, frame.eye_targets[0], 0.8f, 0.05f, 0.05f, 1.0f);
			clear_target(cmd, frame.eye_targets[1], 0.05f, 0.8f, 0.05f, 1.0f);
			clear_target(cmd, frame.hud_target, 0.0f, 0.4f, 0.0f, 0.4f);
			Aeron_SubmitCommandBuffer(cmd);
			AeronXrHudQuad hud = { 1, { 0.0f, 0.0f, -2.0f }, { 1.0f, 0.5f } };
			Aeron_XrStereoCommit(&hud);
			if (stereo_frames++ == 0) {
				printf("stereo %dx%d L fov %.2f %.2f %.2f %.2f pos %.3f %.3f %.3f\n", frame.width,
					   frame.height, frame.views[0].fov[0], frame.views[0].fov[1], frame.views[0].fov[2],
					   frame.views[0].fov[3], frame.views[0].position[0], frame.views[0].position[1],
					   frame.views[0].position[2]);
			}
		}
		if (!Aeron_Present()) {
			fprintf(stderr, "present failed: %s\n", Aeron_RenderLastError());
			break;
		}
	}
	printf("stereo frames: %d\n", stereo_frames);
	Aeron_Shutdown();
	return stereo_frames > 0 ? 0 : 1;
}
