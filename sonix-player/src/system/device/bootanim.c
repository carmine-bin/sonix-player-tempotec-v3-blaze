#include "bootanim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "src/system/image/stb_image_decl.h"

#define FRAME_DIR "/usr/resource/sonix/boot_animation"
#define FRAME_MAX 125
#define FRAME_NS (1000000000L / 25)

// Where the last frame's mark lands on the mark in /etc/logo.jpeg (320x480,
// mark at x 72..265, y 220..260; in the frame x 32..225, y 10..50).
#define ORIGIN_X 40
#define ORIGIN_Y 210

static unsigned char *read_file(const char *path, long *size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	unsigned char *buf = *size > 0 ? malloc((size_t)*size) : NULL;
	if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

static volatile sig_atomic_t stop_asked;

void bootanim_stop(void) { stop_asked = 1; }

int bootanim_play(uint16_t *fb, int panel_w, int panel_h) {
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	int i;
	for (i = 0; i < FRAME_MAX && !stop_asked; i++) {
		char path[96];
		snprintf(path, sizeof(path), FRAME_DIR "/%d.png", i);
		long size;
		unsigned char *file = read_file(path, &size);
		if (!file) {
			break;
		}
		int w, h, n;
		// 4 channels: the first frame is a palette PNG, the rest plain RGB.
		unsigned char *px = stbi_load_from_memory(file, (int)size, &w, &h, &n, 4);
		free(file);
		if (!px) {
			break;
		}
		if (ORIGIN_X + w <= panel_w && ORIGIN_Y + h <= panel_h) {
			for (int y = 0; y < h; y++) {
				uint16_t *row = fb + (size_t)(ORIGIN_Y + y) * panel_w + ORIGIN_X;
				const unsigned char *p = px + (size_t)y * w * 4;
				for (int x = 0; x < w; x++, p += 4) {
					// Over black, as the stock player draws it.
					unsigned r = p[0] * p[3] / 255, g = p[1] * p[3] / 255, b = p[2] * p[3] / 255;
					row[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
				}
			}
		}
		stbi_image_free(px);

		next.tv_nsec += FRAME_NS;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return i;
}
