/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba/gba.h"
#include "gba/input.h"
#include "gba/audio.h"
#include "gba/video.h"

#include "gba/renderers/video-software.h"
#include "util/circle-buffer.h"
#include "util/memory.h"
#include "util/threading.h"
#include "util/vfs.h"
#include "platform/psp2/sce-vfs.h"
#include "third-party/blip_buf/blip_buf.h"

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/moduleinfo.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include <vita2d.h>

PSP2_MODULE_INFO(0, 0, "mGBA");

#define PSP2_HORIZONTAL_PIXELS 960
#define PSP2_VERTICAL_PIXELS 544
#define PSP2_INPUT 0x50535032
#define PSP2_SAMPLES 64
#define PSP2_AUDIO_BUFFER_SIZE (PSP2_SAMPLES * 19)

struct GBAPSP2AudioContext {
	struct CircleBuffer buffer;
	Mutex mutex;
	Condition cond;
	bool running;
};

static void _mapVitaKey(struct GBAInputMap* map, int pspKey, enum GBAKey key) {
	GBAInputBindKey(map, PSP2_INPUT, __builtin_ctz(pspKey), key);
}

static THREAD_ENTRY _audioThread(void* context) {
	struct GBAPSP2AudioContext* audio = (struct GBAPSP2AudioContext*) context;
	struct GBAStereoSample buffer[PSP2_AUDIO_BUFFER_SIZE];
	int audioPort = sceAudioOutOpenPort(PSP2_AUDIO_OUT_PORT_TYPE_MAIN, PSP2_AUDIO_BUFFER_SIZE, 48000, PSP2_AUDIO_OUT_MODE_STEREO);
	while (audio->running) {
		MutexLock(&audio->mutex);
		int len = CircleBufferSize(&audio->buffer);
		len /= sizeof(buffer[0]);
		if (len > PSP2_AUDIO_BUFFER_SIZE) {
			len = PSP2_AUDIO_BUFFER_SIZE;
		}
		if (len > 0) {
			len &= ~(PSP2_AUDIO_MIN_LEN - 1);
			CircleBufferRead(&audio->buffer, buffer, len * sizeof(buffer[0]));
			MutexUnlock(&audio->mutex);
			sceAudioOutSetConfig(audioPort, len, -1, -1);
			sceAudioOutOutput(audioPort, buffer);
			MutexLock(&audio->mutex);
		}
		ConditionWait(&audio->cond, &audio->mutex);
		MutexUnlock(&audio->mutex);
	}
	sceAudioOutReleasePort(audioPort);
	return 0;
}

int main() {
	printf("%s initializing", projectName);
	bool running = true;
	bool fullscreen = false;
	bool fsToggle = false;

	struct GBAVideoSoftwareRenderer renderer;
	GBAVideoSoftwareRendererCreate(&renderer);

	struct GBA* gba = anonymousMemoryMap(sizeof(struct GBA));
	struct ARMCore* cpu = anonymousMemoryMap(sizeof(struct ARMCore));

	printf("GBA: %08X", gba);
	printf("CPU: %08X", cpu);
	int activeKeys = 0;

	struct GBAInputMap inputMap;
	GBAInputMapInit(&inputMap);
	_mapVitaKey(&inputMap, PSP2_CTRL_CROSS, GBA_KEY_A);
	_mapVitaKey(&inputMap, PSP2_CTRL_CIRCLE, GBA_KEY_B);
	_mapVitaKey(&inputMap, PSP2_CTRL_START, GBA_KEY_START);
	_mapVitaKey(&inputMap, PSP2_CTRL_SELECT, GBA_KEY_SELECT);
	_mapVitaKey(&inputMap, PSP2_CTRL_UP, GBA_KEY_UP);
	_mapVitaKey(&inputMap, PSP2_CTRL_DOWN, GBA_KEY_DOWN);
	_mapVitaKey(&inputMap, PSP2_CTRL_LEFT, GBA_KEY_LEFT);
	_mapVitaKey(&inputMap, PSP2_CTRL_RIGHT, GBA_KEY_RIGHT);
	_mapVitaKey(&inputMap, PSP2_CTRL_LTRIGGER, GBA_KEY_L);
	_mapVitaKey(&inputMap, PSP2_CTRL_RTRIGGER, GBA_KEY_R);

	struct GBAAxis desc = { GBA_KEY_DOWN, GBA_KEY_UP, 192, 64 };
	GBAInputBindAxis(&inputMap, PSP2_INPUT, 0, &desc);
	desc = (struct GBAAxis) { GBA_KEY_RIGHT, GBA_KEY_LEFT, 192, 64 };
	GBAInputBindAxis(&inputMap, PSP2_INPUT, 1, &desc);

	vita2d_init();
	vita2d_texture* tex = vita2d_create_empty_texture_format(256, 256, SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1BGR);

	renderer.outputBuffer = vita2d_texture_get_datap(tex);
	renderer.outputBufferStride = 256;

	struct VFile* rom = VFileOpenSce("cache0:/VitaDefilerClient/Documents/GBA/rom.gba", PSP2_O_RDONLY, 0666);
	struct VFile* save = VFileOpenSce("cache0:/VitaDefilerClient/Documents/GBA/rom.sav", PSP2_O_RDWR | PSP2_O_CREAT, 0666);

	printf("ROM: %08X", rom);
	printf("Save: %08X", save);

	GBACreate(gba);
	ARMSetComponents(cpu, &gba->d, 0, 0);
	ARMInit(cpu);
	printf("%s initialized.", "CPU");

	gba->keySource = &activeKeys;
	gba->sync = 0;

	GBAVideoAssociateRenderer(&gba->video, &renderer.d);

	GBALoadROM(gba, rom, save, 0);
	printf("%s loaded.", "ROM");

	ARMReset(cpu);
	double ratio = GBAAudioCalculateRatio(1, 60, 1);
	blip_set_rates(gba->audio.left, GBA_ARM7TDMI_FREQUENCY, 48000 * ratio);
	blip_set_rates(gba->audio.right, GBA_ARM7TDMI_FREQUENCY, 48000 * ratio);

	struct GBAPSP2AudioContext audioContext;
	CircleBufferInit(&audioContext.buffer, PSP2_AUDIO_BUFFER_SIZE * sizeof(struct GBAStereoSample));
	MutexInit(&audioContext.mutex);
	ConditionInit(&audioContext.cond);
	audioContext.running = true;
	Thread audioThread;
	ThreadCreate(&audioThread, _audioThread, &audioContext);

	printf("%s all set and ready to roll.", projectName);

	int frameCounter = 0;
	while (running) {
		ARMRunLoop(cpu);

		if (frameCounter != gba->video.frameCounter) {
			SceCtrlData pad;
			sceCtrlPeekBufferPositive(0, &pad, 1);
			if (pad.buttons & PSP2_CTRL_TRIANGLE) {
				running = false;
				break;
			}
			if (pad.buttons & PSP2_CTRL_SQUARE) {
				if (!fsToggle) {
					fullscreen = !fullscreen;
				}
				fsToggle = true;
			} else {
				fsToggle = false;
			}

			activeKeys = GBAInputMapKeyBits(&inputMap, PSP2_INPUT, pad.buttons, 0);
			enum GBAKey angles = GBAInputMapAxis(&inputMap, PSP2_INPUT, 0, pad.ly);
			if (angles != GBA_KEY_NONE) {
				activeKeys |= 1 << angles;
			}
			angles = GBAInputMapAxis(&inputMap, PSP2_INPUT, 1, pad.lx);
			if (angles != GBA_KEY_NONE) {
				activeKeys |= 1 << angles;
			}
			angles = GBAInputMapAxis(&inputMap, PSP2_INPUT, 2, pad.ry);
			if (angles != GBA_KEY_NONE) {
				activeKeys |= 1 << angles;
			}
			angles = GBAInputMapAxis(&inputMap, PSP2_INPUT, 3, pad.rx);
			if (angles != GBA_KEY_NONE) {
				activeKeys |= 1 << angles;
			}

			MutexLock(&audioContext.mutex);
			while (blip_samples_avail(gba->audio.left) >= PSP2_SAMPLES) {
				if (CircleBufferSize(&audioContext.buffer) + PSP2_SAMPLES * sizeof(struct GBAStereoSample) > CircleBufferCapacity(&audioContext.buffer)) {
					break;
				}
				struct GBAStereoSample samples[PSP2_SAMPLES];
				blip_read_samples(gba->audio.left, &samples[0].left, PSP2_SAMPLES, true);
				blip_read_samples(gba->audio.right, &samples[0].right, PSP2_SAMPLES, true);
				int i;
				for (i = 0; i < PSP2_SAMPLES; ++i) {
					CircleBufferWrite16(&audioContext.buffer, samples[i].left);
					CircleBufferWrite16(&audioContext.buffer, samples[i].right);
				}
			}
			ConditionWake(&audioContext.cond);
			MutexUnlock(&audioContext.mutex);

			vita2d_start_drawing();
			vita2d_clear_screen();
			if (fullscreen) {
				vita2d_draw_texture_scale(tex, 0, 0, 960.0f / 240.0f, 544.0f / 160.0f);
			} else {
				vita2d_draw_texture_scale(tex, 120, 32, 3.0f, 3.0f);
			}
			vita2d_end_drawing();
			vita2d_swap_buffers();

			frameCounter = gba->video.frameCounter;
		}
	}
	printf("%s shutting down...", projectName);

	ARMDeinit(cpu);
	GBADestroy(gba);

	rom->close(rom);
	save->close(save);

	GBAInputMapDeinit(&inputMap);

	MutexLock(&audioContext.mutex);
	audioContext.running = false;
	ConditionWake(&audioContext.cond);
	MutexUnlock(&audioContext.mutex);
	ThreadJoin(audioThread);
	CircleBufferDeinit(&audioContext.buffer);
	MutexDeinit(&audioContext.mutex);
	ConditionDeinit(&audioContext.cond);

	mappedMemoryFree(gba, 0);
	mappedMemoryFree(cpu, 0);

	vita2d_fini();
	vita2d_free_texture(tex);

	sceKernelExitProcess(0);
	return 0;
}
