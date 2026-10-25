#include "cover.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include "src/system/db/sqlite3.h"

#include "src/system/library/albumart.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/image/art_shrink.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/core/utils.h"

// The backdrop is blurred at half resolution and stretched back up. Blurring
// the small copy (rather than relying on the stretch alone) is what keeps it
// smooth: a plain upscale of a heavily shrunk image shows the interpolation
// facets as visible blocks.
#define BACKDROP_SCALE_DIVISOR 2

// Box blur radius, in pixels of the half-size image. Repeated passes of a box
// blur approximate a gaussian closely enough that nothing looks boxy.
#define BACKDROP_BLUR_RADIUS 9
#define BACKDROP_BLUR_PASSES 3

// How far the backdrop is pushed towards black (dark theme) or white (light
// theme) so the text on top of it stays readable. 100 = untouched.
#define BACKDROP_BRIGHTNESS_PCT 42

static bool backdrop_light;
static bool backdrop_upright;

void cover_set_backdrop_light(bool light) { backdrop_light = light; }

void cover_set_backdrop_upright(bool upright) { backdrop_upright = upright; }

// ---------------------------------------------------------------------------
// raw (decoded, RGB888) images
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *pixels; // RGB888, w*h*3 bytes
	int w;
	int h;
} raw_image_t;

// A rectangle of a source image, in source pixels.
typedef struct {
	int x;
	int y;
	int w;
	int h;
} crop_t;

static inline uint16_t rgb_to_rgb565(uint32_t r, uint32_t g, uint32_t b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The same, rounded to the nearest 5- and 6-bit level instead of cut down to
// the one below: round(v * 31 / 255) and round(v * 63 / 255), exact for every
// input byte. Half the worst-case error of the cut, which on a flat grey is
// the difference between grey and a faint tint.
static inline uint16_t rgb_to_rgb565_rounded(uint32_t r, uint32_t g, uint32_t b) {
	uint32_t r5 = (r * 249 + 1014) >> 11;
	uint32_t g6 = (g * 253 + 505) >> 10;
	uint32_t b5 = (b * 249 + 1014) >> 11;
	return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void raw_free(raw_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	img->pixels = NULL;
}

// How much memory a single decode may take.
//
// On the device the budget is small and re-read every time: a quarter of what is
// free now, capped at 8 MB. It must not be derived from MemAvailable, which is
// mostly page cache -- mapped fonts, the player's own code, the audio decoder's
// read-ahead. The kernel cannot kill the player (it runs at oom_score_adj -500),
// so a large allocation is served by evicting all of that cache, and every glyph
// draw and instruction fetch then becomes a page fault against slow flash.
//
// A refused cover is a glyph; a thrashing kernel is a frozen device. The cap is
// what keeps the second from happening, and a larger one buys no artwork: the
// JPEG path decodes at half, a quarter or an eighth (see jpeg_planes.h), so even
// a poster-sized progressive cover fits in two megabytes. The host build keeps a
// large budget so a PC music library loses no artwork.
#define DECODE_BUDGET_MIN_BYTES (2 * 1024 * 1024)
#ifdef HOST_BUILD
#define DECODE_BUDGET_MAX_BYTES (64 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 1
#define DECODE_BUDGET_RESERVE_BYTES (3 * 1024 * 1024)
#else
#define DECODE_BUDGET_MAX_BYTES (8 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 4
#define DECODE_BUDGET_RESERVE_BYTES 0
#endif

static size_t read_mem_available(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return 0;
	}

	char line[256];
	size_t kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1) {
			break;
		}
	}

	fclose(f);
	return kb * 1024;
}

static size_t decode_budget_bytes(void) {
	// Recomputed on every call: what was free at boot says nothing about what is
	// free after an hour of playing. /proc/meminfo costs a few microseconds and
	// this only runs on the worker thread.
	size_t available = read_mem_available();
	size_t budget = available / DECODE_BUDGET_DIVISOR;

	if (budget > DECODE_BUDGET_RESERVE_BYTES) {
		budget -= DECODE_BUDGET_RESERVE_BYTES;
	}

	if (budget < DECODE_BUDGET_MIN_BYTES)
		budget = DECODE_BUDGET_MIN_BYTES;
	if (budget > DECODE_BUDGET_MAX_BYTES)
		budget = DECODE_BUDGET_MAX_BYTES;

	return budget;
}

// Peak bytes per source pixel while stb_image decodes a PNG: four channels out,
// plus the filtered scanline buffer. JPEGs need no estimate: the plane decoder
// works their peak out from the frame header and refuses on its own.
#define PNG_DECODE_BYTES_PER_PIXEL 8

// Decodes compressed JPEG/PNG bytes to RGB888. Alpha is dropped rather than
// composited: covers are opaque in practice, and three channels keep the
// intermediate buffer smaller.
//
// max_box is the largest output anybody wants from this decode. Baseline JPEGs,
// which is what album art overwhelmingly is, go through the vendored TJpgDec
// with its 1/2, 1/4, 1/8 output scaling: a 3000x3000 cover comes out as 750x750
// in under two megabytes instead of a 45 MB full-resolution pass this device
// cannot afford. That is how the stock player shows poster-sized covers on the
// same 64 MB. Progressive JPEGs and PNGs fall back to stb_image under the
// memory budget.
static bool decode_raw(const uint8_t *data, size_t size, int max_box, raw_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!data || size == 0)
		return false;

	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8 && max_box > 0) {
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint8_t *pixels = jpeg_scaled_decode(data, size, max_box, &w, &h, &src_w, &src_h);
		if (pixels) {
			// Both sizes, because the output on its own says nothing about what
			// happened: TJpgDec only ever halves, and a picture already smaller
			// than the box comes out untouched, so one number alone would make a
			// 360x360 cover look like something that had been reduced.
			fprintf(stderr, "cover: decoded jpeg %dx%d -> %dx%d\n", src_w, src_h, w, h);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}

		// Not readable by TJpgDec, which in practice means progressive. Those go
		// through stb, but not through its ordinary loader: that one interleaves
		// a full-resolution RGB buffer next to the planes it was built from,
		// which for a 1200x1200 cover peaks around 11 MB -- refused by any sane
		// budget on a 64 MB device.
		//
		// The plane decoder averages the components down to the size actually
		// wanted, and, when even that does not fit, decodes at half, a quarter or
		// an eighth by keeping only the low-frequency corner of each block. It
		// picks the best scale the budget allows and says which in the log. The
		// 2x margin is the same one the TJpgDec path leaves for the resampler
		// below.
		size_t peak = 0;
		pixels = jpeg_planes_decode(data, size, max_box * 2, decode_budget_bytes(), &w, &h, &peak);
		if (pixels) {
			fprintf(stderr, "cover: decoded progressive jpeg to %dx%d (planes, peak about %zu KB)\n", w, h,
					peak / 1024);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		if (peak > 0) {
			fprintf(stderr, "cover: skipping jpeg artwork, decoding it would need about %zu KB (budget %zu KB)\n",
					peak / 1024, decode_budget_bytes() / 1024);
		}
		return false;
	}

	if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && max_box > 0) {
		// The streaming decoder, not stb: stb inflates the whole image first,
		// which for a 3000x3000 file is far more than a 64 MB device can spare.
		// This one works scanline by scanline and downsamples as it goes, so the
		// source size stops mattering. The 2x margin gives the final resampler
		// enough to average over, as on the JPEG path.
		int w = 0, h = 0;
		uint8_t *pixels = png_scaled_decode(data, size, max_box * 2, &w, &h);
		if (pixels) {
			fprintf(stderr, "cover: decoded png to %dx%d (scaled)\n", w, h);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		// Interlaced or 16-bit: falls through to stb, under the memory budget.
	}

	int w = 0, h = 0, comp = 0;
	if (!stbi_info_from_memory(data, (int)size, &w, &h, &comp))
		return false;
	if (w <= 0 || h <= 0)
		return false;
	int bpp = PNG_DECODE_BYTES_PER_PIXEL;
	size_t needed = (size_t)w * (size_t)h * (size_t)bpp;
	size_t budget = decode_budget_bytes();
	if (needed > budget) {
		// Better no artwork than an allocation that wedges the whole player
		// halfway down a folder.
		fprintf(stderr, "cover: skipping %dx%d artwork, decoding it would need about %zu KB (budget %zu KB)\n", w, h,
				needed / 1024, budget / 1024);
		return false;
	}

	uint8_t *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 3);
	if (!pixels) {
		fprintf(stderr, "cover: decode of %dx%d artwork failed\n", w, h);
		return false;
	}

	// One line per decode: this is the most expensive thing the browser does, and
	// the log shows how many decodes a folder actually triggers.
	fprintf(stderr, "cover: decoded %dx%d (%d bytes/px)\n", w, h, bpp);

	out->pixels = pixels;
	out->w = w;
	out->h = h;
	return true;
}

// The largest centred rectangle of the source with the target's aspect ratio.
// This turns a square cover into an edge-to-edge banner: the sides stay put and
// the overflow at top and bottom is simply never sampled.
static crop_t crop_to_aspect(int src_w, int src_h, int dst_w, int dst_h) {
	crop_t c;

	if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
		c.h = src_h;
		c.w = (int)((int64_t)src_h * dst_w / dst_h);
	} else {
		c.w = src_w;
		c.h = (int)((int64_t)src_w * dst_h / dst_w);
	}
	if (c.w < 1)
		c.w = 1;
	if (c.h < 1)
		c.h = 1;

	c.x = (src_w - c.w) / 2;
	c.y = (src_h - c.h) / 2;
	return c;
}

// ---------------------------------------------------------------------------
// Shrinking by area
//
// Each output pixel is the average of the source area it covers, with the
// pixels cut by its edges counted for the part that falls inside. The plain box
// average below counts whole pixels only, so at a ratio that is not a whole
// number its footprint alternates between one and two source pixels: 600 to 480
// takes one column in four twice. Thin lines, circles and lettering then come
// out uneven, a stroke thicker in one place and thinner a few pixels on.
//
// Weights are in 1/AREA_ONE of an output pixel along each axis, and add up to
// exactly AREA_ONE, so a flat colour stays that colour. Two axes of 12 bits and
// 8-bit samples fill 32 bits and no more: 4096 * 4096 * 255 plus the rounding
// half is below 2^32.
// ---------------------------------------------------------------------------

#define AREA_SHIFT 12
#define AREA_ONE (1 << AREA_SHIFT)

// The source pixels under one output pixel: `count` of them from `first`, with
// their weights from `weight_at` in the axis' weight table.
typedef struct {
	int first;
	int count;
	int weight_at;
} area_span_t;

// Fills the spans and weights of one axis, src_len source pixels onto dst_len
// output pixels, dst_len <= src_len. `weights` holds src_len + dst_len entries,
// which is enough: every source pixel belongs to one output pixel, except the
// ones an edge cuts, which belong to two. False when a ratio is so large that the
// weights cannot be made to add up; the caller then shrinks the old way.
static bool area_axis(int src_len, int dst_len, area_span_t *spans, uint16_t *weights) {
	int at = 0;
	for (int d = 0; d < dst_len; d++) {
		// Positions in 1/dst_len of a source pixel: output pixel d covers
		// [lo, hi), source pixel s covers [s * dst_len, (s + 1) * dst_len).
		int64_t lo = (int64_t)d * src_len;
		int64_t hi = lo + src_len;
		int first = (int)(lo / dst_len);
		int last = (int)((hi - 1) / dst_len);
		if (last >= src_len)
			last = src_len - 1;

		spans[d].first = first;
		spans[d].count = last - first + 1;
		spans[d].weight_at = at;

		int sum = 0, biggest = at;
		for (int s = first; s <= last; s++) {
			int64_t s_lo = (int64_t)s * dst_len;
			int64_t s_hi = s_lo + dst_len;
			int64_t overlap = (hi < s_hi ? hi : s_hi) - (lo > s_lo ? lo : s_lo);
			int w = (int)((overlap * AREA_ONE + src_len / 2) / src_len);
			weights[at] = (uint16_t)w;
			if (w > weights[biggest])
				biggest = at;
			sum += w;
			at++;
		}

		// Rounding leaves the sum a little off AREA_ONE; the largest weight
		// takes the difference.
		int fixed = weights[biggest] + (AREA_ONE - sum);
		if (fixed < 0 || fixed > AREA_ONE)
			return false;
		weights[biggest] = (uint16_t)fixed;
	}
	return true;
}

// resample_rgb() for a shrink, by area. NULL when a table cannot be had.
static uint8_t *resample_area(const raw_image_t *src, const crop_t *crop, int dst_w, int dst_h, bool flip_v) {
	if (dst_w > crop->w || dst_h > crop->h)
		return NULL;

	size_t row_len = (size_t)dst_w * 3;
	area_span_t *xs = malloc((size_t)dst_w * sizeof(*xs));
	area_span_t *ys = malloc((size_t)dst_h * sizeof(*ys));
	uint16_t *xw = malloc((size_t)(crop->w + dst_w) * sizeof(*xw));
	uint16_t *yw = malloc((size_t)(crop->h + dst_h) * sizeof(*yw));
	uint32_t *acc = malloc(row_len * sizeof(*acc));
	uint32_t *line = malloc(row_len * sizeof(*line));
	uint8_t *dst = malloc((size_t)dst_h * row_len);

	bool ok = xs && ys && xw && yw && acc && line && dst && area_axis(crop->w, dst_w, xs, xw) &&
			  area_axis(crop->h, dst_h, ys, yw);
	if (ok) {
		int cached = -1; // the source row `line` holds, shrunk across

		for (int y = 0; y < dst_h; y++) {
			memset(acc, 0, row_len * sizeof(*acc));

			for (int k = 0; k < ys[y].count; k++) {
				int s = ys[y].first + k;
				uint32_t wy = yw[ys[y].weight_at + k];

				// One source row across: at most AREA_ONE * 255 per sample.
				// Consecutive output rows share the row an edge cuts, so the
				// last one is kept.
				if (s != cached) {
					const uint8_t *in = src->pixels + ((size_t)(crop->y + s) * src->w + crop->x) * 3;
					uint32_t *o = line;
					for (int x = 0; x < dst_w; x++) {
						const uint8_t *px = in + (size_t)xs[x].first * 3;
						const uint16_t *w = xw + xs[x].weight_at;
						uint32_t r = 0, g = 0, b = 0;
						for (int i = 0; i < xs[x].count; i++) {
							r += w[i] * (uint32_t)px[0];
							g += w[i] * (uint32_t)px[1];
							b += w[i] * (uint32_t)px[2];
							px += 3;
						}
						o[0] = r;
						o[1] = g;
						o[2] = b;
						o += 3;
					}
					cached = s;
				}

				for (size_t i = 0; i < row_len; i++)
					acc[i] += wy * line[i];
			}

			int out_row = flip_v ? (dst_h - 1 - y) : y;
			uint8_t *out = dst + (size_t)out_row * row_len;
			for (size_t i = 0; i < row_len; i++)
				out[i] = (uint8_t)((acc[i] + (1u << (2 * AREA_SHIFT - 1))) >> (2 * AREA_SHIFT));
		}
	}

	free(xs);
	free(ys);
	free(xw);
	free(yw);
	free(acc);
	free(line);
	if (!ok) {
		free(dst);
		return NULL;
	}
	return dst;
}

// Resamples a crop of the source into a dst_w x dst_h RGB888 buffer, optionally
// upside down. It averages over the source footprint when shrinking, which
// keeps thumbnails from turning into noise, and interpolates when growing, so
// small covers stretched edge to edge do not come out blocky.
static uint8_t *resample_rgb(const raw_image_t *src, const crop_t *crop, int dst_w, int dst_h, bool flip_v) {
	if (dst_w < 1 || dst_h < 1)
		return NULL;

	bool magnify = (dst_w >= crop->w);

	// A shrink goes by area (above). The loop below stays for the case where
	// that cannot get its tables, and for growing.
	if (!magnify) {
		uint8_t *by_area = resample_area(src, crop, dst_w, dst_h, flip_v);
		if (by_area)
			return by_area;
	}

	uint8_t *dst = malloc((size_t)dst_w * dst_h * 3);
	if (!dst)
		return NULL;

	for (int y = 0; y < dst_h; y++) {
		int out_row = flip_v ? (dst_h - 1 - y) : y;
		uint8_t *dst_row = dst + (size_t)out_row * dst_w * 3;

		for (int x = 0; x < dst_w; x++) {
			uint32_t r, g, b;

			if (magnify) {
				// Bilinear: sample the source at the centre of this output pixel.
				float fx = ((float)x + 0.5f) * crop->w / dst_w - 0.5f;
				float fy = ((float)y + 0.5f) * crop->h / dst_h - 0.5f;
				if (fx < 0)
					fx = 0;
				if (fy < 0)
					fy = 0;

				int x0 = (int)fx, y0 = (int)fy;
				int x1 = x0 + 1 < crop->w ? x0 + 1 : x0;
				int y1 = y0 + 1 < crop->h ? y0 + 1 : y0;
				float tx = fx - x0, ty = fy - y0;

				const uint8_t *p00 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x0) * 3;
				const uint8_t *p10 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x1) * 3;
				const uint8_t *p01 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x0) * 3;
				const uint8_t *p11 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x1) * 3;

				float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
				float w01 = (1 - tx) * ty, w11 = tx * ty;

				r = (uint32_t)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11);
				g = (uint32_t)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11);
				b = (uint32_t)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11);
			} else {
				// Box average over every source pixel this output pixel covers.
				int sy0 = crop->y + (int)((int64_t)y * crop->h / dst_h);
				int sy1 = crop->y + (int)((int64_t)(y + 1) * crop->h / dst_h);
				int sx0 = crop->x + (int)((int64_t)x * crop->w / dst_w);
				int sx1 = crop->x + (int)((int64_t)(x + 1) * crop->w / dst_w);
				if (sy1 <= sy0)
					sy1 = sy0 + 1;
				if (sx1 <= sx0)
					sx1 = sx0 + 1;

				uint32_t sr = 0, sg = 0, sb = 0, n = 0;
				for (int sy = sy0; sy < sy1; sy++) {
					const uint8_t *row = src->pixels + (size_t)sy * src->w * 3;
					for (int sx = sx0; sx < sx1; sx++) {
						const uint8_t *px = row + (size_t)sx * 3;
						sr += px[0];
						sg += px[1];
						sb += px[2];
						n++;
					}
				}
				r = sr / n;
				g = sg / n;
				b = sb / n;
			}

			uint8_t *out_px = dst_row + (size_t)x * 3;
			out_px[0] = (uint8_t)r;
			out_px[1] = (uint8_t)g;
			out_px[2] = (uint8_t)b;
		}
	}

	return dst;
}

// One separable box-blur pass over an RGB888 buffer, using a running sum so the
// cost does not grow with the radius. scratch must hold w*h*3 bytes.
static void box_blur_pass(uint8_t *img, uint8_t *scratch, int w, int h, int radius) {
	if (radius < 1)
		return;

	// horizontal: img -> scratch
	for (int y = 0; y < h; y++) {
		const uint8_t *row = img + (size_t)y * w * 3;
		uint8_t *out = scratch + (size_t)y * w * 3;

		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int x = 0; x <= radius && x < w; x++) {
			sum[0] += row[x * 3];
			sum[1] += row[x * 3 + 1];
			sum[2] += row[x * 3 + 2];
			count++;
		}

		for (int x = 0; x < w; x++) {
			out[x * 3] = (uint8_t)(sum[0] / count);
			out[x * 3 + 1] = (uint8_t)(sum[1] / count);
			out[x * 3 + 2] = (uint8_t)(sum[2] / count);

			int add = x + radius + 1;
			int drop = x - radius;
			if (add < w) {
				sum[0] += row[add * 3];
				sum[1] += row[add * 3 + 1];
				sum[2] += row[add * 3 + 2];
				count++;
			}
			if (drop >= 0) {
				sum[0] -= row[drop * 3];
				sum[1] -= row[drop * 3 + 1];
				sum[2] -= row[drop * 3 + 2];
				count--;
			}
		}
	}

	// vertical: scratch -> img
	for (int x = 0; x < w; x++) {
		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int y = 0; y <= radius && y < h; y++) {
			const uint8_t *px = scratch + ((size_t)y * w + x) * 3;
			sum[0] += px[0];
			sum[1] += px[1];
			sum[2] += px[2];
			count++;
		}

		for (int y = 0; y < h; y++) {
			uint8_t *out = img + ((size_t)y * w + x) * 3;
			out[0] = (uint8_t)(sum[0] / count);
			out[1] = (uint8_t)(sum[1] / count);
			out[2] = (uint8_t)(sum[2] / count);

			int add = y + radius + 1;
			int drop = y - radius;
			if (add < h) {
				const uint8_t *px = scratch + ((size_t)add * w + x) * 3;
				sum[0] += px[0];
				sum[1] += px[1];
				sum[2] += px[2];
				count++;
			}
			if (drop >= 0) {
				const uint8_t *px = scratch + ((size_t)drop * w + x) * 3;
				sum[0] -= px[0];
				sum[1] -= px[1];
				sum[2] -= px[2];
				count--;
			}
		}
	}
}

// Ordered-dither offsets, one per position of a 4x4 tile. Added before the 565
// truncation they spread the quantisation error around, turning the backdrop's
// banded gradients into a smooth ramp. Range is +-half of the 5-bit step.
static const int8_t BAYER4[4][4] = {
	{-4, 2, -3, 3},
	{4, -2, 5, -1},
	{-3, 3, -4, 2},
	{5, -1, 4, -2},
};

static inline uint8_t dither_channel(int value, int offset) {
	value += offset;
	if (value < 0)
		value = 0;
	if (value > 255)
		value = 255;
	return (uint8_t)value;
}

// pack_rgb565 with the ordered dither above. Used for the blurred backdrops,
// whose soft gradients are exactly where 16-bit banding shows.
static bool pack_rgb565_dithered(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *px = rgb + ((size_t)y * w + x) * 3;
			int offset = BAYER4[y & 3][x & 3];
			buf[(size_t)y * w + x] = rgb_to_rgb565(dither_channel(px[0], offset), dither_channel(px[1], offset),
												   dither_channel(px[2], offset));
		}
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// Packs an RGB888 buffer into the cover_image_t LVGL draws from. Takes
// ownership of nothing: the source buffer is still the caller's to free.
static bool pack_rgb565(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int i = 0; i < w * h; i++) {
		buf[i] = rgb_to_rgb565_rounded(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// Builds the visible picture: COVER_FIT_COVER crops to the box's aspect ratio,
// anything else fits the whole image inside the box.
static bool make_scaled(const raw_image_t *src, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop;
	int dst_w, dst_h;

	if (fit == COVER_FIT_COVER) {
		crop = crop_to_aspect(src->w, src->h, box_w, box_h);
		dst_w = box_w;
		dst_h = box_h;
	} else {
		// Whole picture, scaled down to fit the box, never blown up: a small
		// cover looks better small than stretched.
		crop = (crop_t){0, 0, src->w, src->h};
		dst_w = src->w;
		dst_h = src->h;

		if (dst_w > box_w) {
			dst_h = (int)((int64_t)dst_h * box_w / dst_w);
			dst_w = box_w;
		}
		if (dst_h > box_h) {
			dst_w = (int)((int64_t)dst_w * box_h / dst_h);
			dst_h = box_h;
		}
		if (dst_w < 1)
			dst_w = 1;
		if (dst_h < 1)
			dst_h = 1;
	}

	uint8_t *rgb = resample_rgb(src, &crop, dst_w, dst_h, false);
	if (!rgb)
		return false;

	bool ok = pack_rgb565(rgb, dst_w, dst_h, out);
	free(rgb);
	return ok;
}

// Builds the upside-down, blurred, dimmed copy that sits behind the controls.
// The blur is a shrink, box-blur passes, then an interpolated stretch: visually
// a soft gaussian-like smear for a few hundred kilobytes of work.
static bool make_backdrop(const raw_image_t *src, int box_w, int box_h, bool from_bottom, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop = crop_to_aspect(src->w, src->h, box_w, box_h);
	if (from_bottom) {
		crop.y = src->h - crop.h; // the band the block will be drawn over
		if (crop.y < 0)
			crop.y = 0;
	}

	int small_w = box_w / BACKDROP_SCALE_DIVISOR;
	int small_h = box_h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2)
		small_w = 2;
	if (small_h < 2)
		small_h = 2;

	// Flip while shrinking, so the stretch back up only has to interpolate and
	// no separate flip pass is needed. Not for a bottom band: that one stands
	// in for the pixels underneath it and has to face the same way. Nor when
	// the backdrop is the whole screen with the sleeve shown over it, where the
	// two are plainly the same picture and one of them being upside down is the
	// only thing anybody sees.
	bool flip = !from_bottom && !backdrop_upright;
	uint8_t *small = resample_rgb(src, &crop, small_w, small_h, flip);
	if (!small)
		return false;

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		for (int i = 0; i < BACKDROP_BLUR_PASSES; i++) {
			box_blur_pass(small, scratch, small_w, small_h, BACKDROP_BLUR_RADIUS);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t whole = {0, 0, small_w, small_h};

	uint8_t *rgb = resample_rgb(&tiny, &whole, box_w, box_h, false);
	free(small);
	if (!rgb)
		return false;

	// Push the backdrop away from the text colour: whatever the cover happens to
	// be, the title has to stay readable on top of it.
	for (int i = 0; i < box_w * box_h * 3; i++) {
		if (backdrop_light) {
			rgb[i] = (uint8_t)(255 - ((255 - rgb[i]) * BACKDROP_BRIGHTNESS_PCT) / 100);
		} else {
			rgb[i] = (uint8_t)((rgb[i] * BACKDROP_BRIGHTNESS_PCT) / 100);
		}
	}

	bool ok = pack_rgb565_dithered(rgb, box_w, box_h, out);
	free(rgb);
	return ok;
}

// A blurred, dimmed copy of a picture that has already been scaled and packed.
//
// Not make_backdrop() with different numbers: that one starts from the decoded
// source and is built while the track is being loaded, because it is wanted on
// every track. This is asked for from the sleeve already on screen, at the
// moment the VU meters are opened, and by then the decoded source has long been
// freed.
//
// Same treatment as the backdrop, so the sleeve behind the needles and the
// block behind the controls read as one surface: shrink, box-blur, stretch,
// and the same push away from the text colour.
// The one colour that stands for a sleeve: the pool of light under the record
// in Cover Flow, and what the player's alternative layout tints itself with.
//
// A plain average of a cover comes out mud: every sleeve has a lot of near-grey
// in it and the greys drown the one colour a person would name. So each pixel
// is weighted by how far it is from grey, and the near-greys count for almost
// nothing. What comes back is then pushed towards full saturation, because a
// glow is light and not paint: a washed-out tint reads as a smudge on the
// screen rather than as something lit.
//
// Every fourth pixel each way, which is a sixteenth of the work for a number
// that does not change in the last decimal.
//
// Two things here keep a black-and-white sleeve from coming out green.
//
// The first is how a 565 pixel is opened out. Shifting the five- and six-bit
// fields up by three and two leaves green a step ahead of red and blue on every
// grey there is: white arrives as 248,252,248 and mid grey as 96,100,96 -- four
// units of pure green, which the stretch below takes to full strength.
// Replicating the top bits down into the gap instead makes white 255,255,255
// and leaves the rest off by a unit or two either way, with no colour
// preferred.
//
// The second is the floor. Two units of rounding noise in a grey average is
// still a hue, and stretching it produces a confident colour out of nothing. So
// a sleeve whose average is that close to grey is taken at its word and lights
// the screen white.
#define TONE_CHROMA_FLOOR 14
#define TONE_MONO 0xF2EEE8 // barely warm white: light, not paint

// The sleeve's colours averaged with each pixel weighted by how far it is from
// grey (see above): what the eye would call the colour of the sleeve. False
// when there is nothing to look at.
static bool tone_weighted_average(const cover_image_t *cover, unsigned *r_out, unsigned *g_out, unsigned *b_out) {
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	int stride = (int)cover->dsc.header.stride / 2;
	if (!cover->pixels || w <= 0 || h <= 0) {
		return false;
	}

	uint64_t ar = 0, ag = 0, ab = 0, weight = 0;
	for (int y = 0; y < h; y += 4) {
		const uint16_t *row = (const uint16_t *)cover->pixels + (size_t)y * stride;
		for (int x = 0; x < w; x += 4) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
			unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
			unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
			// Distance from grey, and a little for being bright at all: a sleeve
			// that really is black and white still has to light something.
			unsigned k = (hi - lo) * 4 + hi / 8 + 1;
			ar += (uint64_t)r * k;
			ag += (uint64_t)g * k;
			ab += (uint64_t)b * k;
			weight += k;
		}
	}
	if (!weight) {
		return false;
	}
	*r_out = (unsigned)(ar / weight);
	*g_out = (unsigned)(ag / weight);
	*b_out = (unsigned)(ab / weight);
	return true;
}

// Pulls a colour away from grey: the gap between the strongest channel and
// the weakest is stretched, and the middle one moves with it, so the hue stays
// where it was and only the strength changes.
static uint32_t tone_stretch(unsigned r, unsigned g, unsigned b) {
	unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (hi - lo < TONE_CHROMA_FLOOR) {
		return TONE_MONO; // no colour worth finding: this sleeve is grey
	}
	if (hi > lo) {
		unsigned reach = hi > 200 ? 200 : hi; // a dark average is not stretched to full
		r = lo + (r - lo) * 255 / (hi - lo) * reach / 200;
		g = lo + (g - lo) * 255 / (hi - lo) * reach / 200;
		b = lo + (b - lo) * 255 / (hi - lo) * reach / 200;
	}
	if (r > 255) {
		r = 255;
	}
	if (g > 255) {
		g = 255;
	}
	if (b > 255) {
		b = 255;
	}

	// And lift the whole thing if it is still dark, so a black sleeve throws a
	// faint light of its own colour instead of a shadow. Taken after the
	// stretch above, which has usually already done most of it.
	hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (hi > 0 && hi < 180) {
		r = r * 180 / hi;
		g = g * 180 / hi;
		b = b * 180 / hi;
	}
	return (r << 16) | (g << 8) | b;
}

uint32_t cover_dominant_tone(const cover_image_t *cover) {
	unsigned r, g, b;
	if (!tone_weighted_average(cover, &r, &g, &b)) {
		return 0;
	}
	return tone_stretch(r, g, b);
}

uint32_t cover_tone_stretched(uint32_t tone) {
	return tone_stretch((tone >> 16) & 0xFF, (tone >> 8) & 0xFF, tone & 0xFF);
}

int cover_mean_luma(const cover_image_t *cover) {
	if (!cover || !cover->pixels) {
		return -1;
	}
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	int stride = (int)cover->dsc.header.stride / 2;
	if (w <= 0 || h <= 0) {
		return -1;
	}

	// Every fourth pixel each way, as cover_dominant_tone() samples it.
	uint64_t sum = 0, count = 0;
	for (int y = 0; y < h; y += 4) {
		const uint16_t *row = (const uint16_t *)cover->pixels + (size_t)y * stride;
		for (int x = 0; x < w; x += 4) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
			sum += (r * 299 + g * 587 + b * 114) / 1000;
			count++;
		}
	}
	return count ? (int)(sum / count) : -1;
}

// A page painted in a sleeve's colour cannot take the plain average of
// tone_weighted_average(): a yellow and blue sleeve averages to a grey-olive
// that is neither, and a yellow darkened for a dark page is olive whatever is
// done to it. So the sleeve's colours are sorted into families of hue, and the
// page takes one family rather than a mix of all of them.
//
// A family is three neighbouring bins of 15 degrees, scored by how much of the
// sleeve is that colour: each pixel votes with its saturation, its distance
// from grey measured against its own brightness. Measured against full scale
// instead, a deep navy (16,32,48) is as far from grey as a mid grey's rounding
// noise and gets no vote at all, and a dark blue sleeve with a gold crest on it
// is called gold. A pixel too dark to have a hue, or too close to grey, does
// not vote.
//
// The strongest family is always a candidate, and so is any other family that
// does not overlap it and scores at least half as much. A dark page takes the
// darkest candidate and a light page the lightest, so the page is a colour
// that looks right at that brightness. The family's colour is its pixels
// averaged as tone_weighted_average() weighs them, the vivid ones counting
// most, so a yellow is the yellow of the sleeve and not of its shaded edges.
// A sleeve with no colour worth sorting falls back to the plain average, which
// is near grey anyway.
#define TONE_BINS 24		 // 15 degrees of hue each
#define TONE_FAMILY_SAT 64	 // saturation out of 256 below which a pixel has no hue to vote for
#define TONE_FAMILY_DARK 24	 // and a brightest channel below which it is black
#define TONE_FAMILY_SPREAD 10 // and a channel spread below which its hue is noise
#define TONE_FAMILIES (TONE_BINS / 3)
// A yellow family that covers this much of the sleeve, in tenths, and is this
// bright makes a light page even on a darker sleeve: darkened, it would only
// be olive.
#define TONE_YELLOW_SHARE 3
#define TONE_YELLOW_LUMA 150
#define TONE_YELLOW_FROM 40 // degrees
#define TONE_YELLOW_TO 75

typedef struct {
	uint64_t r, g, b, weight; // the colours, weighted as tone_weighted_average() weighs them
	uint64_t score;			  // the saturation voted, for choosing between bins
	unsigned count;			  // how many samples
} tone_bin_t;

// The hue of a colour in degrees, 0..359, or -1 for a grey.
static int tone_hue(unsigned r, unsigned g, unsigned b) {
	unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	int d = (int)(hi - lo);
	if (d == 0) {
		return -1;
	}
	int h;
	if (hi == r) {
		h = ((int)g - (int)b) * 60 / d;
	} else if (hi == g) {
		h = 120 + ((int)b - (int)r) * 60 / d;
	} else {
		h = 240 + ((int)r - (int)g) * 60 / d;
	}
	return (h % 360 + 360) % 360;
}

static int tone_luma(unsigned r, unsigned g, unsigned b) { return (int)((r * 299 + g * 587 + b * 114) / 1000); }

// How far apart two bins are around the circle.
static int bin_distance(int a, int b) {
	int d = a > b ? a - b : b - a;
	return d < TONE_BINS - d ? d : TONE_BINS - d;
}

bool cover_page_tone(const cover_image_t *cover, int light_from, uint32_t *tone, int *luma, bool *light) {
	int mean = cover_mean_luma(cover);
	if (mean < 0) {
		return false;
	}
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	int stride = (int)cover->dsc.header.stride / 2;

	tone_bin_t bins[TONE_BINS];
	memset(bins, 0, sizeof(bins));
	unsigned samples = 0;
	uint64_t colour = 0;
	// Every fourth pixel each way, as tone_weighted_average() samples it.
	for (int y = 0; y < h; y += 4) {
		const uint16_t *row = (const uint16_t *)cover->pixels + (size_t)y * stride;
		for (int x = 0; x < w; x += 4) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
			unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
			unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
			samples++;
			if (hi < TONE_FAMILY_DARK || hi - lo < TONE_FAMILY_SPREAD) {
				continue;
			}
			unsigned sat = (hi - lo) * 256 / hi;
			if (sat < TONE_FAMILY_SAT) {
				continue;
			}
			tone_bin_t *bin = &bins[tone_hue(r, g, b) * TONE_BINS / 360];
			unsigned k = (hi - lo) * 4 + hi / 8 + 1;
			bin->r += (uint64_t)r * k;
			bin->g += (uint64_t)g * k;
			bin->b += (uint64_t)b * k;
			bin->weight += k;
			bin->score += sat;
			bin->count++;
			colour += sat;
		}
	}

	bool is_light = mean >= light_from;
	unsigned r = 0, g = 0, b = 0;
	// On average less than an eighth of the floor's worth of saturation per
	// sample: a grey sleeve with a few coloured specks, which the plain average
	// describes better than any one family.
	if (colour * 8 < (uint64_t)samples * TONE_FAMILY_SAT) {
		if (!tone_weighted_average(cover, &r, &g, &b)) {
			return false;
		}
	} else {
		// Each family's score, centred on each bin.
		uint64_t score[TONE_BINS];
		int best = 0;
		for (int i = 0; i < TONE_BINS; i++) {
			score[i] = bins[(i + TONE_BINS - 1) % TONE_BINS].score + bins[i].score + bins[(i + 1) % TONE_BINS].score;
			if (score[i] > score[best]) {
				best = i;
			}
		}

		// The candidates, strongest first, none overlapping another.
		int picks[TONE_FAMILIES];
		int picked = 0;
		picks[picked++] = best;
		for (;;) {
			int next = -1;
			for (int i = 0; i < TONE_BINS; i++) {
				if (score[i] * 2 < score[best]) {
					continue;
				}
				bool apart = true;
				for (int j = 0; j < picked; j++) {
					if (bin_distance(i, picks[j]) < 3) {
						apart = false;
						break;
					}
				}
				if (apart && (next < 0 || score[i] > score[next])) {
					next = i;
				}
			}
			if (next < 0 || picked == TONE_FAMILIES) {
				break;
			}
			picks[picked++] = next;
		}

		// The darkest candidate for a dark page, the lightest for a light one.
		int chosen_luma = -1;
		unsigned chosen_count = 0;
		for (int j = 0; j < picked; j++) {
			uint64_t ar = 0, ag = 0, ab = 0, weight = 0;
			unsigned count = 0;
			for (int d = -1; d <= 1; d++) {
				const tone_bin_t *bin = &bins[(picks[j] + d + TONE_BINS) % TONE_BINS];
				ar += bin->r;
				ag += bin->g;
				ab += bin->b;
				weight += bin->weight;
				count += bin->count;
			}
			if (!weight) {
				continue;
			}
			unsigned fr = (unsigned)(ar / weight), fg = (unsigned)(ag / weight), fb = (unsigned)(ab / weight);
			int fl = tone_luma(fr, fg, fb);
			if (chosen_luma < 0 || (is_light ? fl > chosen_luma : fl < chosen_luma)) {
				r = fr;
				g = fg;
				b = fb;
				chosen_luma = fl;
				chosen_count = count;
			}
		}
		if (chosen_luma < 0) {
			return false;
		}

		int hue = tone_hue(r, g, b);
		if (!is_light && hue >= TONE_YELLOW_FROM && hue <= TONE_YELLOW_TO && chosen_luma >= TONE_YELLOW_LUMA &&
			chosen_count * 10 >= samples * TONE_YELLOW_SHARE) {
			is_light = true;
		}
	}

	// Never 0, which says "no sleeve": a black one comes back one step above.
	uint32_t packed = (r << 16) | (g << 8) | b;
	*tone = packed ? packed : 0x010101;
	*luma = mean;
	*light = is_light;
	return true;
}

// A packed cover opened back out to RGB888, the form the resampler and the
// blur work in. The top bits are replicated into the low ones, so white comes
// back as 255.
static bool raw_from_cover(const cover_image_t *src, raw_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (!src || !src->pixels) {
		return false;
	}
	int w = (int)src->dsc.header.w;
	int h = (int)src->dsc.header.h;
	int stride = (int)src->dsc.header.stride / 2;
	if (w < 2 || h < 2 || stride < w) {
		return false;
	}
	uint8_t *rgb = malloc((size_t)w * h * 3);
	if (!rgb) {
		return false;
	}
	for (int y = 0; y < h; y++) {
		const uint16_t *row = (const uint16_t *)src->pixels + (size_t)y * stride;
		uint8_t *drow = rgb + (size_t)y * w * 3;
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			drow[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
			drow[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
			drow[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
		}
	}
	out->pixels = rgb;
	out->w = w;
	out->h = h;
	return true;
}

bool cover_blur_copy(const cover_image_t *src, cover_image_t *out) {
	if (!src) {
		if (out) {
			memset(out, 0, sizeof(*out));
		}
		return false;
	}
	return cover_blur_fill(src, (int)src->dsc.header.w, (int)src->dsc.header.h, out);
}

bool cover_blur_fill(const cover_image_t *src, int out_w, int out_h, cover_image_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	// Back out to RGB888: the blur and the resampler both work there, and five
	// and six bit channels have nowhere to put the intermediate values.
	raw_image_t opened;
	if (!raw_from_cover(src, &opened)) {
		return false;
	}
	uint8_t *rgb = opened.pixels;
	int w = opened.w;
	int h = opened.h;

	if (out_w < 2 || out_h < 2) {
		free(rgb);
		return false;
	}
	raw_image_t whole_img = {.pixels = rgb, .w = w, .h = h};
	crop_t whole = crop_to_aspect(w, h, out_w, out_h);
	int small_w = out_w / BACKDROP_SCALE_DIVISOR;
	int small_h = out_h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2) {
		small_w = 2;
	}
	if (small_h < 2) {
		small_h = 2;
	}
	uint8_t *small = resample_rgb(&whole_img, &whole, small_w, small_h, false);
	free(rgb);
	if (!small) {
		return false;
	}

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		for (int i = 0; i < BACKDROP_BLUR_PASSES; i++) {
			box_blur_pass(small, scratch, small_w, small_h, BACKDROP_BLUR_RADIUS);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t tiny_whole = {0, 0, small_w, small_h};
	uint8_t *big = resample_rgb(&tiny, &tiny_whole, out_w, out_h, false);
	free(small);
	if (!big) {
		return false;
	}

	for (int i = 0; i < out_w * out_h * 3; i++) {
		if (backdrop_light) {
			big[i] = (uint8_t)(255 - ((255 - big[i]) * BACKDROP_BRIGHTNESS_PCT) / 100);
		} else {
			big[i] = (uint8_t)((big[i] * BACKDROP_BRIGHTNESS_PCT) / 100);
		}
	}

	bool ok = pack_rgb565_dithered(big, out_w, out_h, out);
	free(big);
	return ok;
}

// ---------------------------------------------------------------------------
// building the finished images
//
// Decoding is the one operation here that can ask for more memory than the
// device has. Everything downstream works on the scaled result and costs a few
// hundred kilobytes at most, so all the risk sits in a single call, which is
// what makes it worth isolating.
// ---------------------------------------------------------------------------

typedef struct {
	int w;
	int h;
	cover_fit_t fit;
	bool backdrop; // build the blurred, flipped copy instead of a plain scale
	// The backdrop comes from the BOTTOM band of the source instead of a
	// centred crop of it, and is not flipped. What the screensaver's
	// full-screen picture wants: there the blurred block sits over the picture
	// itself, so it has to be a blurred continuation of the part it covers --
	// a mirrored centre crop would read as a different photograph.
	bool from_bottom;
} image_request_t;

// Decodes once and produces every requested image from that one decode. The
// expensive part is the JPEG, not the scaling, so a track change asking for
// both the cover and its backdrop still only decodes once.
static bool build_images(const uint8_t *data, size_t size, const image_request_t *reqs, int n, cover_image_t *outs) {
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	// Nobody asked for anything. Without this the box below comes out zero, both
	// scaled decoders are skipped (they need a box to aim at) and the picture
	// goes through the unbounded path at full resolution -- the one thing this
	// file exists to avoid.
	if (n < 1) {
		return false;
	}

	// The largest edge anybody asked for decides how much resolution the decode
	// must keep; everything smaller is resampled from that.
	int max_box = 0;
	for (int i = 0; i < n; i++) {
		if (reqs[i].w > max_box)
			max_box = reqs[i].w;
		if (reqs[i].h > max_box)
			max_box = reqs[i].h;
	}

	raw_image_t raw;
	if (!decode_raw(data, size, max_box, &raw)) {
		return false;
	}

	bool first_ok = false;
	for (int i = 0; i < n; i++) {
		bool ok;
		if (reqs[i].backdrop) {
			ok = make_backdrop(&raw, reqs[i].w, reqs[i].h, reqs[i].from_bottom, &outs[i]);
		} else {
			ok = make_scaled(&raw, reqs[i].w, reqs[i].h, reqs[i].fit, &outs[i]);
		}
		if (i == 0) {
			first_ok = ok;
		}
	}

	raw_free(&raw);
	return first_ok;
}

// ---------------------------------------------------------------------------
// bounded decoding
//
// No helper processes: the decode stays on the worker thread and is refused
// when it does not comfortably fit the memory actually free. The refusal is
// cheap (stbi_info reads only the header), the negative result is cached on disk
// like any other, and a folder whose cover is genuinely too big shows a glyph
// instead of gambling the whole player on an optimistic malloc.
//
// Isolating the decode in a forked child is not an option here. fork() on a
// single 1.2 GHz core copies the page tables of a ~25 MB process while holding
// mmap_sem, stalling every other thread's page faults, and fork() from a
// multithreaded process can catch another thread holding the malloc lock,
// leaving the child deadlocked in its first allocation.
// ---------------------------------------------------------------------------

static bool build_images_safely(const uint8_t *data, size_t size, const image_request_t *reqs, int n,
								cover_image_t *outs) {
	// The budget guard lives inside decode_raw and applies to the stb path only:
	// baseline JPEGs go through the scaled decoder, whose memory use is set by
	// the requested output rather than the source, so a poster-sized cover needs
	// no refusing at all.
	return build_images(data, size, reqs, n, outs);
}

// ---------------------------------------------------------------------------
// Walking the sources
//
// Finding artwork and decoding it are two different ways to fail, and the
// difference matters twice over.
//
// A track can carry a picture the decoder will not take -- a progressive JPEG
// well past the memory budget, an APIC truncated by a bad tagger. Committing to
// the first bytes found would leave that track showing nothing even with a
// perfectly good cover.jpg beside it, so every source is tried in turn until
// one decodes.
//
// And a decode that was refused is not the same answer as "there is no artwork
// here". The first depends on how much memory happened to be free; the second
// is a property of the files. Only the second is worth remembering on disk (see
// the thumbnail cache), which is why the outcome is reported as three states
// rather than a bool.
// ---------------------------------------------------------------------------

typedef enum {
	COVER_BUILD_OK,		// an image was produced
	COVER_BUILD_NONE,	// no source carried a picture
	COVER_BUILD_FAILED, // a picture was found, none of them would decode
} cover_build_t;

// `used`, when given, is set to the candidate index the images came from.
static cover_build_t build_from_sources_at(const char *path, bool is_dir, const image_request_t *reqs, int n,
										   cover_image_t *outs, int *used) {
	bool had_bytes = false;

	// Noted for the crash handler and cleared on the way out, so a log that
	// still names a file says the fault landed inside the decode rather than
	// merely at the same moment as one -- which is as far as the log line
	// printed below can settle it, since that one comes from this thread while
	// the interface runs on another.
	crumb_set_artwork(path);

	// Zeroed here rather than only inside build_images: a path with no artwork
	// at all never reaches that function, and the caller still reads these.
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		bool loaded = is_dir ? albumart_load_dir_candidate(path, i, &art) : albumart_load_candidate(path, i, &art);
		if (!loaded) {
			continue;
		}

		had_bytes = true;
		// A card pulled out under the decode: an image made of zeros, not kept.
		bool ok = build_images_safely(art.data, art.size, reqs, n, outs) && !albumart_faulted(&art);
		albumart_free(&art);
		if (ok) {
			crumb_set_artwork(NULL);
			if (used) {
				*used = i;
			}
			return COVER_BUILD_OK;
		}

		// A build reports on its first image only, so a failure can still have
		// left a backdrop behind. Clear every slot before the next candidate
		// overwrites the pointers.
		for (int j = 0; j < n; j++) {
			cover_free(&outs[j]);
		}
	}

	crumb_set_artwork(NULL);
	return had_bytes ? COVER_BUILD_FAILED : COVER_BUILD_NONE;
}

static cover_build_t build_from_sources(const char *path, bool is_dir, const image_request_t *reqs, int n,
										cover_image_t *outs) {
	return build_from_sources_at(path, is_dir, reqs, n, outs, NULL);
}

// ---------------------------------------------------------------------------
// public loaders
// ---------------------------------------------------------------------------

bool cover_load_for_file(const char *filepath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(filepath, false, &req, 1, out) == COVER_BUILD_OK;
}

bool cover_load_for_dir(const char *dirpath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(dirpath, true, &req, 1, out) == COVER_BUILD_OK;
}

// A picture that is simply a file, with no track and no folder behind it.
// AirPlay is the caller: shairport writes the artwork the phone sent to
// /tmp/cover-<md5>.jpg and reports the path, so there is nothing to look up.
bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!path || !path[0])
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}

	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (got != (size_t)size) {
		free(data);
		return false;
	}

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	bool ok = build_images_safely(data, got, &req, 1, out);
	free(data);
	return ok;
}

// A picture that is already in memory. The EPUB shelf is the caller: a book's
// cover is a member of its ZIP, so there is no file to open -- and writing one
// out just to read it back would put a temporary file on the card for every
// book on the shelf.
bool cover_load_image_memory(const void *data, size_t size, int box_w, int box_h, cover_fit_t fit,
							 cover_image_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!data || !size || size > ALBUMART_MAX_BYTES) {
		return false;
	}

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_images_safely((const uint8_t *)data, size, &req, 1, out);
}

bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *image_out, int strip_w,
								  int strip_h, cover_image_t *strip_out) {
	if (image_out)
		memset(image_out, 0, sizeof(*image_out));
	if (strip_out)
		memset(strip_out, 0, sizeof(*strip_out));
	if (!path || !path[0] || !image_out || !strip_out)
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}
	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(data);
		return false;
	}

	// One decode, two pictures, as on the player screen -- except that the
	// blurred one comes from the bottom of the same photograph rather than from
	// the middle of it.
	image_request_t reqs[2] = {
		{.w = w, .h = h, .fit = COVER_FIT_COVER, .backdrop = false, .from_bottom = false},
		{.w = strip_w, .h = strip_h, .fit = COVER_FIT_COVER, .backdrop = true, .from_bottom = true},
	};
	cover_image_t results[2];
	bool ok = build_images_safely(data, got, reqs, 2, results);
	free(data);

	if (!ok) {
		cover_free(&results[0]);
		cover_free(&results[1]);
		return false;
	}
	*image_out = results[0];
	*strip_out = results[1];
	return true;
}

// The size, with four kilobytes from each end folded in. Not the whole picture:
// hashing four megabytes on this processor would cost more than the question is
// worth, and two different covers that agree on their length and on both ends
// do not happen outside a deliberate attempt.
static uint64_t bytes_id(const uint8_t *data, size_t size) {
	uint64_t h = 1469598103934665603ULL;
	h ^= (uint64_t)size;
	h *= 1099511628211ULL;

	size_t edge = size < 4096 ? size : 4096;
	for (size_t i = 0; i < edge; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	for (size_t i = size - edge; i < size; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	// Never zero: that is the answer for "no artwork", and a picture that
	// hashed to it would read as one that is not there.
	return h ? h : 1;
}

// `index` is set to the candidate the bytes came from (see cover_source_id()).
static uint64_t source_id_uncached(const char *filepath, int *index) {
	*index = -1;
	if (!filepath || !filepath[0]) {
		return 0;
	}
	// The first source that has bytes, which is the one the decode will start
	// from. It may yet fail to decode and send the build on to the next
	// candidate -- but that outcome follows from these same bytes, so two
	// tracks that agree here agree about the picture that ends up on screen.
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		if (!albumart_load_candidate(filepath, i, &art)) {
			continue;
		}
		uint64_t id = art.size ? bytes_id(art.data, art.size) : 0;
		if (albumart_faulted(&art)) {
			id = 0;
		}
		albumart_free(&art);
		if (id) {
			*index = i;
			return id;
		}
	}
	return 0;
}

static bool playerart_key(char *out, size_t out_size, uint64_t id, int cw, int ch, int bw, int bh);
static bool playerart_load(const char *key, cover_image_t *cover, cover_image_t *backdrop);
static void playerart_store(const char *key, const cover_image_t *cover, const cover_image_t *backdrop);
static bool playerart_has(const char *key);

// The player's cover cache, further down with the other tables.
static uint64_t player_source_id(const char *filepath, int *index);
static bool player_cover_load(uint64_t id, int w, int h, cover_image_t *out);
static bool player_cover_kept(uint64_t id, int w, int h);
static void player_cover_store(uint64_t id, int w, int h, const cover_image_t *img);
static void player_cover_store_soon(uint64_t id, int w, int h, const cover_image_t *img);
static void cover_tone_attach(uint64_t id, cover_image_t *img);

// The backdrop made from a cover already scaled and packed, for a cover that
// came out of the cache with no source decoded.
static bool backdrop_from_cover(const cover_image_t *cover, int box_w, int box_h, cover_image_t *out) {
	raw_image_t raw;
	if (!raw_from_cover(cover, &raw)) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	bool ok = make_backdrop(&raw, box_w, box_h, false, out);
	raw_free(&raw);
	return ok;
}

bool cover_load_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out) {
	if (cover_out)
		memset(cover_out, 0, sizeof(*cover_out));
	if (backdrop_out)
		memset(backdrop_out, 0, sizeof(*backdrop_out));

	int source_index = -1;
	uint64_t id = cover_out ? player_source_id(filepath, &source_index) : 0;

	// Both pictures, the only request the player makes: the card's own copy of
	// the pair first, which spares the backdrop's blur as well as the decode.
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	char art_key[96];
	bool cacheable = cover_out && backdrop_out &&
					 playerart_key(art_key, sizeof(art_key), id, cover_w, cover_h, backdrop_w, backdrop_h);
	if (cacheable && playerart_load(art_key, cover_out, backdrop_out)) {
		cover_tone_attach(id, cover_out);
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("cover: player artwork from the cache in %ld ms\n",
			   (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
		return true;
	}

	// The cover from the thumbnail database, the backdrop made from it: no
	// source decoded.
	if (id && player_cover_load(id, cover_w, cover_h, cover_out)) {
		cover_tone_attach(id, cover_out);
		if (backdrop_out) {
			backdrop_from_cover(cover_out, backdrop_w, backdrop_h, backdrop_out);
		}
		return true;
	}

	// One decode feeds both images: the expensive part is the JPEG, not the
	// scaling, so a track change decodes once rather than twice.
	image_request_t reqs[2] = {0}; // a caller may want neither image
	cover_image_t results[2];
	int n = 0;
	int cover_index = -1, backdrop_index = -1;

	if (cover_out) {
		cover_index = n;
		reqs[n++] = (image_request_t){.w = cover_w, .h = cover_h, .fit = COVER_FIT_COVER, .backdrop = false};
	}
	if (backdrop_out) {
		backdrop_index = n;
		reqs[n++] = (image_request_t){.w = backdrop_w, .h = backdrop_h, .fit = COVER_FIT_COVER, .backdrop = true};
	}

	int used = -1;
	cover_build_t built = build_from_sources_at(filepath, false, reqs, n, results, &used);
	if (built == COVER_BUILD_NONE) {
		// A podcast episode with no cover file beside it: fetch it now, on the
		// loader thread, which can afford to wait on a network request, then
		// try again. This covers episodes started from the queue, whose tags
		// carry the cover URL but whose jpg was never downloaded.
		if (!podcastcache_ensure_cover(filepath)) {
			return false;
		}
		built = build_from_sources_at(filepath, false, reqs, n, results, &used);
	}
	bool ok = (built == COVER_BUILD_OK);
	// Under the id only when the picture came from the source the id was taken
	// from, as for the cover below.
	if (ok && cacheable && used == source_index) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("cover: player artwork decoded in %ld ms, cached\n",
			   (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
		playerart_store(art_key, &results[cover_index], &results[backdrop_index]);
	}

	// Kept only when the picture came from the source the id was taken from:
	// a cover that fell back to another one would be stored under a key that
	// does not describe it.
	if (ok && id && cover_index >= 0 && used == source_index) {
		player_cover_store_soon(id, cover_w, cover_h, &results[cover_index]);
	}
	// The colour goes with it, kept under the same id on the same condition.
	if (ok && cover_index >= 0) {
		cover_tone_attach(used == source_index ? id : 0, &results[cover_index]);
	}

	// A missing backdrop is survivable; the controls just stay plain.
	if (cover_index >= 0)
		*cover_out = results[cover_index];
	if (backdrop_index >= 0)
		*backdrop_out = results[backdrop_index];

	return ok;
}

bool cover_warm_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out) {
	memset(cover_out, 0, sizeof(*cover_out));
	memset(backdrop_out, 0, sizeof(*backdrop_out));

	char art_key[96];
	if (!playerart_key(art_key, sizeof(art_key), cover_source_id(filepath), cover_w, cover_h, backdrop_w,
					   backdrop_h) ||
		playerart_has(art_key)) {
		return false; // no artwork, or already on the card
	}
	// The ordinary load decodes and stores it. The pictures themselves are not
	// wanted now: the track that needs them comes later and reads them back.
	cover_image_t cover, backdrop;
	cover_load_player_images(filepath, cover_w, cover_h, &cover, backdrop_w, backdrop_h, &backdrop);
	cover_free(&cover);
	cover_free(&backdrop);
	return false;
}

void cover_prefetch_player(const char *filepath, int cover_w, int cover_h) {
	if (!filepath || !filepath[0] || cover_w <= 0 || cover_h <= 0) {
		return;
	}
	// Nothing to make: no artwork, or the cover is already kept at this size.
	int source_index = -1;
	uint64_t id = player_source_id(filepath, &source_index);
	if (!id || player_cover_kept(id, cover_w, cover_h)) {
		return;
	}
	cover_image_t cover;
	cover_load_player_images(filepath, cover_w, cover_h, &cover, 0, 0, NULL);
	cover_free(&cover);
}

bool cover_load_shared(const char *filepath, int player_w, int player_h, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (!filepath || player_w <= 0 || player_h <= 0 || size <= 0) {
		return false;
	}

	// The player's picture of this file: from its table, or decoded at the
	// player's size and kept there for the player too.
	int source_index = -1;
	uint64_t id = player_source_id(filepath, &source_index);
	cover_image_t big;
	bool have = id && player_cover_load(id, player_w, player_h, &big);
	bool keyed = have; // whether `id` describes the picture in `big`
	if (!have) {
		image_request_t req = {.w = player_w, .h = player_h, .fit = COVER_FIT_COVER, .backdrop = false};
		int used = -1;
		if (build_from_sources_at(filepath, false, &req, 1, &big, &used) != COVER_BUILD_OK) {
			return false;
		}
		keyed = id && used == source_index;
		if (keyed) {
			player_cover_store_soon(id, player_w, player_h, &big);
		}
	}

	// Its colour, from the player's picture, so the player and the album page
	// share one row of it.
	cover_tone_attach(keyed ? id : 0, &big);

	// Then down to the size asked for.
	if (player_w == size && player_h == size) {
		*out = big;
		return true;
	}
	raw_image_t raw;
	bool ok = raw_from_cover(&big, &raw);
	cover_image_t tones = big;
	cover_free(&big);
	if (!ok) {
		return false;
	}
	ok = make_scaled(&raw, size, size, COVER_FIT_COVER, out);
	raw_free(&raw);
	if (ok) {
		out->has_tone = tones.has_tone;
		out->tone = tones.tone;
		out->tone_luma = tones.tone_luma;
		out->tone_light = tones.tone_light;
	}
	return ok;
}

bool cover_scaled_copy(const cover_image_t *src, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	raw_image_t raw;
	if (size <= 0 || !raw_from_cover(src, &raw)) {
		return false;
	}
	bool ok = make_scaled(&raw, size, size, COVER_FIT_COVER, out);
	raw_free(&raw);
	if (ok) {
		out->has_tone = src->has_tone;
		out->tone = src->tone;
		out->tone_luma = src->tone_luma;
		out->tone_light = src->tone_light;
	}
	return ok;
}

void cover_free(cover_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

// ---------------------------------------------------------------------------
// on-disk thumbnail cache
//
// The same mechanism the stock player uses: resized artwork is kept on the card
// so browsing a library never decodes the original twice. What is stored is the
// already-scaled RGB565 thumbnail: a 44x44 entry is under 4 KB and loading one
// is a single read, with no decoder involved. The first pass over a big folder
// decodes, every pass after it reads back a few kilobytes. "No artwork here" is
// stored too, so empty folders stop being rescanned as well.
//
// A database, not a directory of one file per entry: thousands of small files in
// one directory is the worst thing to ask of a FAT filesystem, where listing and
// cleaning up are both slow and every entry costs a directory entry. It is a
// single SQLite file named thumbnails under .local, alongside library.db. The
// key is a hash of path + size + mtime + source size, so replacing a cover
// invalidates its entry by itself; the value is the pixels, or an empty row
// meaning nothing was found there.
// ---------------------------------------------------------------------------

// The largest thumbnail kept in the thumbs table; the player's own cover has a
// table of its own (see the player's cover below).
//
// The album carousel sets the ceiling: it decodes at 210, and a carousel that
// re-decoded every cover on every visit would be the one page that never got
// cheaper the second time. An entry at that size is worth keeping for the few
// hundred albums a card holds, but not for anything larger.
#define THUMB_DISK_MAX_SIZE 216
#define THUMB_WAV_EPOCH "|w2"

// A cached "no artwork here" is a row with no width, and its height carries
// the epoch of the readers that gave that answer. A row from another epoch is
// a miss: raising the epoch has every file without artwork asked again, while
// every cached picture stays as it is.
#define THUMB_NONE_EPOCH 3

static sqlite3 *thumb_db;
// The thumbnail worker writes while the GUI thread may be reading, and a card
// change closes and reopens the database under both: one connection, guarded by
// this lock.
static pthread_mutex_t thumb_db_lock = PTHREAD_MUTEX_INITIALIZER;

// Where the open cache came from, and which file it was, so that a cache
// deleted from under it is noticed (see cover_cache_reopen_if_replaced).
static char thumb_db_base[512];
static char thumb_db_path[512];
static file_identity_t thumb_db_identity;

// Everything the player keeps beside the user's music lives under one hidden
// directory, so a card ends up with a single .local folder rather than one dot
// entry per feature.
#define THUMB_DB_SUBPATH ".local/thumbnails"

// The player's own artwork cache lives beside it (see further down) and opens,
// closes and reopens with it.
static bool playerart_ready;
static char playerart_path[512];
static file_identity_t playerart_identity;
static void playerart_open(const char *base);
static void playerart_close(void);

// An earlier release kept the cache as a directory at the path the database now
// wants, so on first open it is emptied and removed. Everything inside it was
// written by this player (16 hex-digit names), and the content is rebuilt in the
// database on the first pass over the lists.
static void thumb_wipe_old_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	char victim[600];
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		if (snprintf(victim, sizeof(victim), "%s/%s", dir, e->d_name) < (int)sizeof(victim)) {
			unlink(victim);
		}
	}
	closedir(d);
	rmdir(dir);
	printf("cover: old folder cache removed (%s)\n", dir);
}

// Schema plus pragma. synchronous=OFF because this is only a cache: an entry
// lost to a card pulled mid-write recomputes itself, and every fsync saved is
// smoother scrolling.
//
// journal_mode=TRUNCATE, as every other database in the player has. The
// default (DELETE) creates thumbnails-journal on the card for every write and
// unlinks it after: two exFAT directory-entry updates per transaction, and
// this database is written on every track change (the picture's id, its
// colour, the player cover moved to the front). Truncating the same file
// leaves the directory alone and keeps the journal, so a player killed
// mid-write still rolls back as before.
static bool thumb_db_prepare_schema(sqlite3 *db) {
	char *err = NULL;
	if (sqlite3_exec(db,
					 "PRAGMA synchronous=OFF;"
					 "PRAGMA journal_mode=TRUNCATE;"
					 // SQLite's own default is 2 MB of page cache per
					 // connection, which browsing one big folder fills and
					 // keeps filled for the rest of the session. Every other
					 // database in the player (library, audiobooks,
					 // subscriptions, radio) asks for 64 to 256 kB.
					 "PRAGMA cache_size=-256;"
					 "CREATE TABLE IF NOT EXISTS thumbs("
					 " key TEXT PRIMARY KEY,"
					 " w INTEGER NOT NULL,"
					 " h INTEGER NOT NULL,"
					 " pixels BLOB);"
					 // The EPUB shelf's covers, with the title read from the
					 // same opening of the book (see cover_book_cached).
					 "CREATE TABLE IF NOT EXISTS books("
					 " key TEXT PRIMARY KEY,"
					 " title TEXT,"
					 " w INTEGER NOT NULL,"
					 " h INTEGER NOT NULL,"
					 " pixels BLOB);"
					 // The player's cover, one row per picture and size, and
					 // the picture of each track that carries its own (see
					 // the player's cover below).
					 "CREATE TABLE IF NOT EXISTS player_covers("
					 " key TEXT PRIMARY KEY,"
					 " w INTEGER NOT NULL,"
					 " h INTEGER NOT NULL,"
					 " used INTEGER NOT NULL,"
					 " pixels BLOB NOT NULL);"
					 "CREATE INDEX IF NOT EXISTS player_covers_used ON player_covers(used);"
					 "CREATE TABLE IF NOT EXISTS player_cover_ids("
					 " key TEXT PRIMARY KEY,"
					 " id TEXT NOT NULL);"
					 // The colour of each of those pictures, so the pages
					 // that wear it do not work it out again (see the
					 // picture's colour below).
					 "CREATE TABLE IF NOT EXISTS cover_colours("
					 " key TEXT PRIMARY KEY,"
					 " tone INTEGER NOT NULL,"
					 " luma INTEGER NOT NULL,"
					 " light INTEGER NOT NULL)",
					 NULL, NULL, &err) == SQLITE_OK) {
		return true;
	}
	fprintf(stderr, "cover: thumbnails db refused: %s\n", err ? err : "?");
	sqlite3_free(err);
	return false;
}

// Opens or creates <base>/.local/thumbnails and adopts it. Call with the lock
// already held.
static bool try_cache_db(const char *base) {
	char parent[512];
	char path[512];

	// A base path long enough to overflow these would leave a truncated name,
	// which is worse than simply having no cache.
	if (snprintf(parent, sizeof(parent), "%s/.local", base) >= (int)sizeof(parent)) {
		return false;
	}
	mkdir(parent, 0755);

	if (snprintf(path, sizeof(path), "%s/%s", base, THUMB_DB_SUBPATH) >= (int)sizeof(path)) {
		return false;
	}

	// The old cache directory occupies the same name as the database file, so
	// clear it out of the way first.
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		thumb_wipe_old_dir(path);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			return false; // it would not go away, so no cache here
		}
	}

	sqlite3 *db = NULL;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "cover: cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return false;
	}

	if (!thumb_db_prepare_schema(db)) {
		// A corrupt database, typically a card pulled mid-write, does not heal:
		// delete the file and start clean, since it is only a cache.
		sqlite3_close(db);
		unlink(path);
		db = NULL;
		if (sqlite3_open(path, &db) != SQLITE_OK || !thumb_db_prepare_schema(db)) {
			sqlite3_close(db);
			return false;
		}
	}

	thumb_db = db;
	snprintf(thumb_db_base, sizeof(thumb_db_base), "%s", base);
	snprintf(thumb_db_path, sizeof(thumb_db_path), "%s", path);
	file_identity_read(path, &thumb_db_identity);
	printf("cover: thumbnail cache in %s\n", path);
	return true;
}

void cover_cache_reopen_if_replaced(void) {
	pthread_mutex_lock(&thumb_db_lock);
	if (thumb_db && thumb_db_path[0] && file_identity_changed(thumb_db_path, &thumb_db_identity)) {
		printf("cover: %s was deleted or replaced; opening it again\n", thumb_db_path);
		sqlite3_close(thumb_db);
		thumb_db = NULL;
		char base[sizeof(thumb_db_base)];
		snprintf(base, sizeof(base), "%s", thumb_db_base);
		if (!try_cache_db(base) && !try_cache_db("/tmp")) {
			printf("cover: thumbnail cache disabled\n");
		}
	}
	if (playerart_ready && playerart_path[0] && file_identity_changed(playerart_path, &playerart_identity)) {
		char base[sizeof(thumb_db_base)];
		snprintf(base, sizeof(base), "%s", thumb_db_base);
		playerart_open(base);
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

void cover_set_cache_dir(const char *sd_root) {
	pthread_mutex_lock(&thumb_db_lock);

	if (thumb_db) {
		sqlite3_close(thumb_db);
		thumb_db = NULL;
	}
	playerart_close();

	bool ok = sd_root && sd_root[0] && try_cache_db(sd_root);
	playerart_open(ok ? sd_root : NULL);
	if (!ok) {
		// The card may be missing or read-only; a tmpfs cache still saves the
		// rescan cost within a single run.
		ok = try_cache_db("/tmp");
	}

	pthread_mutex_unlock(&thumb_db_lock);

	if (!ok) {
		printf("cover: thumbnail cache disabled\n");
	}
	fprintf(stderr, "cover: %zu KB free, decode budget %zu KB\n", read_mem_available() / 1024,
			decode_budget_bytes() / 1024);
}

// FNV-1a. The cache key has to change when the source does, so the file's size
// and timestamp go into it: replacing a cover.jpg invalidates the thumbnail
// without anybody having to clear the cache.
static uint64_t thumb_hash(const char *path, int box, bool is_dir) {
	struct stat st;
	uint64_t h = 1469598103934665603ULL;

	// THUMB_WAV_EPOCH goes into the key of every folder and every .wav: both
	// were once answered "no artwork" for a WAV that has it in its ID3 chunk,
	// and that answer is on disk under keys nothing else would change.
	const char *epoch = (is_dir || has_extension(path, ".wav")) ? THUMB_WAV_EPOCH : "";

	char material[700];
	if (stat(path, &st) == 0) {
		snprintf(material, sizeof(material), "%s|%d|%c|%lld|%lld%s", path, box, is_dir ? 'd' : 'f',
				 (long long)st.st_mtime, (long long)st.st_size, epoch);
	} else {
		snprintf(material, sizeof(material), "%s|%d|%c|?%s", path, box, is_dir ? 'd' : 'f', epoch);
	}

	for (const char *p = material; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

// Builds the cache key. False means this size is not worth caching. Whether the
// database is open at all is decided under the lock, in load and store.
static bool thumb_key(char *out, size_t out_size, const char *path, int box, bool is_dir) {
	if (box > THUMB_DISK_MAX_SIZE) {
		return false;
	}
	snprintf(out, out_size, "%016llx", (unsigned long long)thumb_hash(path, box, is_dir));
	return true;
}

// The RGB565 pixels in column `col` of the current row as an image of `w` x `h`.
// False for a blob whose size does not match its dimensions -- an interrupted
// write, which counts as a miss: the picture is decoded again and the row
// overwritten.
static bool image_from_column(sqlite3_stmt *stmt, int col, int w, int h, cover_image_t *out) {
	if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF) {
		return false;
	}
	size_t bytes = (size_t)w * h * 2;
	const void *blob = sqlite3_column_blob(stmt, col);
	if (!blob || (size_t)sqlite3_column_bytes(stmt, col) != bytes) {
		return false;
	}
	uint8_t *pixels = malloc(bytes);
	if (!pixels) {
		return false;
	}
	memcpy(pixels, blob, bytes);
	memset(out, 0, sizeof(*out));
	out->pixels = pixels;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)bytes;
	out->dsc.data = pixels;
	return true;
}

// Reads a cached thumbnail. Returns false on a miss; on a hit, has_image tells
// whether there was artwork at all.
static bool thumb_db_load(const char *key, cover_image_t *out, bool *has_image) {
	bool hit = false;

	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return false;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "SELECT w, h, pixels FROM thumbs WHERE key = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			int w = sqlite3_column_int(stmt, 0);
			int h = sqlite3_column_int(stmt, 1);

			if (w == 0) {
				if (h == THUMB_NONE_EPOCH) {
					*has_image = false; // cached "no artwork here"
					hit = true;
				}
			} else if (image_from_column(stmt, 2, w, h, out)) {
				*has_image = true;
				hit = true;
			}
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

// A single write, made atomic by SQLite: no more .tmp file and rename, since
// the database journal does the same job.
static void thumb_db_store(const char *key, const cover_image_t *img, bool has_image) {
	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "INSERT OR REPLACE INTO thumbs(key, w, h, pixels) VALUES(?1, ?2, ?3, ?4)", -1,
						   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (has_image && img->pixels) {
			int w = (int)img->dsc.header.w;
			int h = (int)img->dsc.header.h;
			sqlite3_bind_int(stmt, 2, w);
			sqlite3_bind_int(stmt, 3, h);
			sqlite3_bind_blob(stmt, 4, img->pixels, (int)((size_t)w * h * 2), SQLITE_STATIC);
		} else {
			sqlite3_bind_int(stmt, 2, 0);
			sqlite3_bind_int(stmt, 3, THUMB_NONE_EPOCH);
			sqlite3_bind_null(stmt, 4);
		}
		sqlite3_step(stmt); // a failure here only costs a future re-decode
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
}

// ---------------------------------------------------------------------------
// on-disk player artwork cache
//
// The player's sleeve and its blurred backdrop are the most expensive pictures
// in the interface: a full decode of the original, a resample to the width of
// the screen, three blur passes and a dithered pack, on every album the player
// is asked to show. Kept on the card, coming back to an album costs one read of
// about a megabyte instead.
//
// Not more rows in the thumbnails database: SonixLink copies that one to the
// phone, and a few hundred screen-sized pictures in it would turn every sync
// into a long download. Not a database of its own either, which it once was:
// SQLite reads a megabyte blob as a chain of overflow pages, and the
// least-recently-shown bookkeeping rewrote the whole row -- the megabyte of
// pixels with it -- on every hit. On the Blaze's card a hit took 2.3 seconds,
// longer than decoding the cover. So one plain file per entry, read in one go,
// in .local/playerart.d; "least recently shown" is the file's mtime, set on
// every hit, and the oldest go once there are more than PLAYERART_MAX_ROWS.
//
// The key is the artwork's own bytes (cover_source_id), not the track, so every
// track of an album that shares a cover.jpg or the same embedded picture shares
// one entry; the sizes and the backdrop's light/upright treatment go in too,
// since each of them changes the pixels.
// ---------------------------------------------------------------------------

#define PLAYERART_SUBPATH ".local/playerart.d"
#define PLAYERART_OLD_DB ".local/playerart" // the database it replaces, deleted on sight
#define PLAYERART_MAX_ROWS 150 // about a megabyte each at the Blaze's sizes
#define PLAYERART_MAGIC 0x41505853u // "SXPA"

typedef struct {
	uint32_t magic;
	uint16_t cw, ch, bw, bh;
} playerart_header_t;

// Call with thumb_db_lock held, like everything else that touches the caches.
static void playerart_close(void) { playerart_ready = false; }

// Only on the card: a tmpfs copy of pictures this size would be memory the
// player needs for everything else.
static void playerart_open(const char *base) {
	playerart_close();
	if (!base || !base[0] || strcmp(base, "/tmp") == 0) {
		return;
	}
	char old[600];
	snprintf(old, sizeof(old), "%s/%s", base, PLAYERART_OLD_DB);
	struct stat st;
	if (stat(old, &st) == 0 && S_ISREG(st.st_mode)) {
		unlink(old);
	}
	if (snprintf(playerart_path, sizeof(playerart_path), "%s/%s", base, PLAYERART_SUBPATH) >=
		(int)sizeof(playerart_path)) {
		playerart_path[0] = '\0';
		return;
	}
	mkdir(playerart_path, 0755);
	if (stat(playerart_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		playerart_path[0] = '\0';
		return;
	}
	playerart_ready = true;
	file_identity_read(playerart_path, &playerart_identity);
	printf("cover: player artwork cache in %s\n", playerart_path);
}

static bool playerart_key(char *out, size_t out_size, uint64_t id, int cw, int ch, int bw, int bh) {
	if (!id) {
		return false;
	}
	snprintf(out, out_size, "%016llx|%d|%d|%d|%d|%d%d", (unsigned long long)id, cw, ch, bw, bh, backdrop_light ? 1 : 0,
			 backdrop_upright ? 1 : 0);
	return true;
}

// The entry's file, or false with no cache. Takes and drops the lock: the
// reading and writing happen outside it, so a megabyte going to or from the
// card does not hold up the thumbnails.
static bool playerart_file(const char *key, char *out, size_t out_size) {
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = key; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	pthread_mutex_lock(&thumb_db_lock);
	bool ok = playerart_ready &&
			  snprintf(out, out_size, "%s/%016llx.px", playerart_path, (unsigned long long)h) < (int)out_size;
	pthread_mutex_unlock(&thumb_db_lock);
	return ok;
}

static bool playerart_read_image(FILE *f, int w, int h, cover_image_t *out) {
	if (w <= 0 || h <= 0) {
		return false;
	}
	size_t bytes = (size_t)w * h * 2;
	uint8_t *pixels = malloc(bytes);
	if (!pixels || fread(pixels, 1, bytes, f) != bytes) {
		free(pixels);
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->pixels = pixels;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)bytes;
	out->dsc.data = pixels;
	return true;
}

static bool playerart_load(const char *key, cover_image_t *cover, cover_image_t *backdrop) {
	char path[600];
	if (!playerart_file(key, path, sizeof(path))) {
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	playerart_header_t hdr;
	bool hit = fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == PLAYERART_MAGIC &&
			   playerart_read_image(f, hdr.cw, hdr.ch, cover);
	if (hit && !playerart_read_image(f, hdr.bw, hdr.bh, backdrop)) {
		cover_free(cover);
		hit = false;
	}
	fclose(f);
	if (hit) {
		utime(path, NULL); // shown now: the last to go
	} else {
		unlink(path); // short or foreign: decode again and write it anew
	}
	return hit;
}

static bool playerart_has(const char *key) {
	char path[600];
	if (!playerart_file(key, path, sizeof(path))) {
		return true; // no cache to warm: nothing to do
	}
	return access(path, F_OK) == 0;
}

// Drops the least recently shown entries beyond PLAYERART_MAX_ROWS.
static void playerart_trim(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	for (;;) {
		int count = 0;
		time_t oldest_time = 0;
		char oldest[300] = "";
		struct dirent *de;
		rewinddir(d);
		while ((de = readdir(d)) != NULL) {
			size_t len = strlen(de->d_name);
			if (len < 4 || strcmp(de->d_name + len - 3, ".px") != 0) {
				continue;
			}
			char path[900];
			struct stat st;
			snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			if (stat(path, &st) != 0) {
				continue;
			}
			count++;
			if (!oldest[0] || st.st_mtime < oldest_time) {
				oldest_time = st.st_mtime;
				snprintf(oldest, sizeof(oldest), "%s", de->d_name);
			}
		}
		if (count <= PLAYERART_MAX_ROWS || !oldest[0]) {
			break;
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", dir, oldest);
		unlink(path);
	}
	closedir(d);
}

static void playerart_store(const char *key, const cover_image_t *cover, const cover_image_t *backdrop) {
	if (!cover->pixels || !backdrop->pixels) {
		return;
	}
	char path[600];
	if (!playerart_file(key, path, sizeof(path))) {
		return;
	}
	playerart_header_t hdr = {.magic = PLAYERART_MAGIC,
							  .cw = (uint16_t)cover->dsc.header.w,
							  .ch = (uint16_t)cover->dsc.header.h,
							  .bw = (uint16_t)backdrop->dsc.header.w,
							  .bh = (uint16_t)backdrop->dsc.header.h};
	// The loader and the warm-up thread can be storing the same picture at
	// once: each writes a part file of its own, and the rename settles it.
	char tmp[640];
	snprintf(tmp, sizeof(tmp), "%s.%lx.part", path, (unsigned long)pthread_self());
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return;
	}
	bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
			  fwrite(cover->pixels, 1, (size_t)hdr.cw * hdr.ch * 2, f) == (size_t)hdr.cw * hdr.ch * 2 &&
			  fwrite(backdrop->pixels, 1, (size_t)hdr.bw * hdr.bh * 2, f) == (size_t)hdr.bw * hdr.bh * 2;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp); // a failure only costs a future decode
		return;
	}
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		playerart_trim(path);
	}
}

// ---------------------------------------------------------------------------
// browser thumbnails
// ---------------------------------------------------------------------------

static cover_build_t thumb_load_uncached(const char *path, bool is_dir, int size, cover_image_t *out) {
	image_request_t req = {.w = size, .h = size, .fit = COVER_FIT_CONTAIN, .backdrop = false};
	return build_from_sources(path, is_dir, &req, 1, out);
}

bool cover_thumb_cached(const char *path, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	char key[32];
	if (!thumb_key(key, sizeof(key), path, size, false)) {
		return false;
	}

	bool has_image = false;
	if (thumb_db_load(key, out, &has_image)) {
		return has_image;
	}
	return false;
}

bool cover_thumb_load(const char *path, bool is_dir, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	// A track answers for itself: its embedded art, or a cover file next to it.
	// Asking the folder first -- the "an album shares one picture" shortcut --
	// breaks as soon as a folder holds tracks from different albums, since every
	// row below the first would show the first track's cover. The per-file disk
	// cache keeps the cost at one decode per file, ever.

	char key[32];
	bool cacheable = thumb_key(key, sizeof(key), path, size, is_dir);

	// Disk first: a hit costs one small read instead of a decode, which is what
	// makes scrolling back over a folder cost nothing.
	if (cacheable) {
		bool has_image = false;
		if (thumb_db_load(key, out, &has_image)) {
			return has_image;
		}
	}

	// A track's picture is very often its album's: the same embedded image in
	// every file, or the one cover.jpg beside them. Keyed by the file alone,
	// the songs list decoded that picture once per track -- fifteen times for
	// an album that the albums list decodes once -- and that was the whole
	// difference between scrolling the two. So a file whose thumbnail is not
	// cached yet asks by its picture too (cover_source_id: the picture's length
	// and its two ends, read from the mapped file), and the answer is stored
	// under both keys.
	char pic_key[40] = "";
	if (cacheable && !is_dir) {
		uint64_t id = cover_source_id(path);
		if (id) {
			snprintf(pic_key, sizeof(pic_key), "p%016llx-%d", (unsigned long long)id, size);
			bool has_image = false;
			if (thumb_db_load(pic_key, out, &has_image) && has_image) {
				thumb_db_store(key, out, true); // and by the file, for next time
				return true;
			}
		}
	}

	cover_build_t built = thumb_load_uncached(path, is_dir, size, out);
	if (pic_key[0] && built == COVER_BUILD_OK) {
		thumb_db_store(pic_key, out, true);
	}

	// "There is no artwork here" is worth storing too: a folder without any is
	// otherwise rescanned from scratch every time it scrolls past.
	//
	// A refused or failed decode is not stored. The decode budget is a quarter
	// of the memory free at that instant, so the same file can be refused now
	// and decode fine a minute later -- and the key is built from the file's own
	// size and mtime, so nothing would ever invalidate the entry. Storing a
	// failure would leave that track without a cover for good.
	if (cacheable && built != COVER_BUILD_FAILED) {
		thumb_db_store(key, out, built == COVER_BUILD_OK);
	}

	return built == COVER_BUILD_OK;
}

// ---------------------------------------------------------------------------
// book covers
// ---------------------------------------------------------------------------

// The largest box a book cover is kept for: the shelf's tiles on either panel
// are well inside it, and a page asking for something bigger decodes it.
#define BOOK_DISK_MAX_W 512
#define BOOK_DISK_MAX_H 768

// A book without a cover is a row with no width, its height this epoch; one
// from another epoch is a miss.
#define BOOK_NONE_EPOCH 1

// FNV-1a over the book's path, size and modification time and the box: a book
// replaced or rewritten is a new key, and the old row is simply never asked for.
static bool book_key(char *out, size_t out_size, const char *path, int box_w, int box_h) {
	if (!path || box_w <= 0 || box_h <= 0 || box_w > BOOK_DISK_MAX_W || box_h > BOOK_DISK_MAX_H) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	char material[700];
	snprintf(material, sizeof(material), "book|%s|%d|%d|%lld|%lld", path, box_w, box_h, (long long)st.st_mtime,
			 (long long)st.st_size);
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = material; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	snprintf(out, out_size, "%016llx", (unsigned long long)h);
	return true;
}

bool cover_book_cached(const char *path, int box_w, int box_h, cover_image_t *out, bool *has_cover, char *title,
					   size_t title_size) {
	memset(out, 0, sizeof(*out));
	*has_cover = false;
	if (title && title_size) {
		title[0] = '\0';
	}
	char key[32];
	if (!book_key(key, sizeof(key), path, box_w, box_h)) {
		return false;
	}

	bool hit = false;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db, "SELECT title, w, h, pixels FROM books WHERE key = ?1", -1, &stmt,
									   NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			int w = sqlite3_column_int(stmt, 1);
			int h = sqlite3_column_int(stmt, 2);
			if (w == 0) {
				hit = h == BOOK_NONE_EPOCH;
			} else {
				hit = image_from_column(stmt, 3, w, h, out);
				*has_cover = hit;
			}
			const char *text = (const char *)sqlite3_column_text(stmt, 0);
			if (hit && title && title_size && text) {
				snprintf(title, title_size, "%s", text);
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

void cover_book_store(const char *path, int box_w, int box_h, const cover_image_t *cover, const char *title) {
	char key[32];
	if (!book_key(key, sizeof(key), path, box_w, box_h)) {
		return;
	}
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db,
									   "INSERT OR REPLACE INTO books(key, title, w, h, pixels) VALUES(?1, ?2, ?3, ?4, ?5)",
									   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_STATIC);
		if (cover && cover->pixels) {
			int w = (int)cover->dsc.header.w;
			int h = (int)cover->dsc.header.h;
			sqlite3_bind_int(stmt, 3, w);
			sqlite3_bind_int(stmt, 4, h);
			sqlite3_bind_blob(stmt, 5, cover->pixels, (int)((size_t)w * h * 2), SQLITE_STATIC);
		} else {
			sqlite3_bind_int(stmt, 3, 0);
			sqlite3_bind_int(stmt, 4, BOOK_NONE_EPOCH);
			sqlite3_bind_null(stmt, 5);
		}
		sqlite3_step(stmt); // a failure here only costs opening the book again
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// ---------------------------------------------------------------------------
// the player's cover
//
// The finished cover is kept in player_covers under cover_source_id() and its
// size. The id is a hash of the picture's bytes, so the tracks of a record
// that carry the same picture share one row, and tracks that carry different
// pictures get one row each. player_cover_ids keeps the id of each track whose
// picture is embedded, keyed by the file's path, size and mtime, so a track
// already seen is matched without reading its picture. A picture from a cover
// file beside the track is hashed every time: replacing that file does not
// change the track.
//
// The backdrop is not kept; it is made from the cover (backdrop_from_cover()).
//
// A row holds the cover as a JPEG at quality PLAYER_COVER_QUALITY: some 60 to
// 100 KB at 480x480 against 450 KB as pixels, which is what lets the table
// keep five times as many in the same room. Above 90 the encoder keeps the
// colour at full resolution, so lettering on a sleeve stays clean. Reading one
// back is a small read and a decode at 1:1 -- far less than the poster-sized
// picture in the file it came from. A row written as pixels by an older build
// is still read, and written again as a JPEG.
// ---------------------------------------------------------------------------

// The most recently shown covers are kept, the rest deleted: about 90 MB at
// 480x480.
#define PLAYER_COVER_MAX_ROWS 1000
#define PLAYER_COVER_QUALITY 92
#define PLAYER_ID_MAX_ROWS 20000
#define PLAYER_ID_PRUNE_EVERY 256
// Part of every cover key: raising it makes every stored cover a miss.
#define PLAYER_COVER_EPOCH 1

static uint64_t fnv1a(const char *text) {
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = text; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

static bool player_id_key(const char *path, char *out, size_t out_size) {
	struct stat st;
	if (!path || !path[0] || stat(path, &st) != 0) {
		return false;
	}
	char material[700];
	snprintf(material, sizeof(material), "pid|%s|%lld|%lld", path, (long long)st.st_mtime, (long long)st.st_size);
	snprintf(out, out_size, "%016llx", (unsigned long long)fnv1a(material));
	return true;
}

static bool player_id_lookup(const char *key, uint64_t *id) {
	bool hit = false;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db &&
		sqlite3_prepare_v2(thumb_db, "SELECT id FROM player_cover_ids WHERE key = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *text = (const char *)sqlite3_column_text(stmt, 0);
			char *end = NULL;
			unsigned long long value = text ? strtoull(text, &end, 16) : 0;
			if (value && end && *end == '\0') {
				*id = (uint64_t)value;
				hit = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

// Every PLAYER_ID_PRUNE_EVERY stores the table is cut back to its newest
// PLAYER_ID_MAX_ROWS rows. Called with the lock held.
static void player_id_prune(void) {
	static unsigned stores;
	if (++stores % PLAYER_ID_PRUNE_EVERY) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db,
						   "DELETE FROM player_cover_ids WHERE rowid IN"
						   " (SELECT rowid FROM player_cover_ids ORDER BY rowid DESC LIMIT -1 OFFSET ?1)",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, PLAYER_ID_MAX_ROWS);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

static void player_id_store(const char *key, uint64_t id) {
	char text[24];
	snprintf(text, sizeof(text), "%016llx", (unsigned long long)id);
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db, "INSERT OR REPLACE INTO player_cover_ids(key, id) VALUES(?1, ?2)", -1,
									   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, text, -1, SQLITE_STATIC);
		sqlite3_step(stmt); // a failure here only costs reading the picture again
		sqlite3_finalize(stmt);
		player_id_prune();
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// The id, and the candidate it came from (0 when it was found in the table,
// which only holds embedded pictures).
static uint64_t player_source_id(const char *filepath, int *index) {
	char key[32];
	bool keyed = player_id_key(filepath, key, sizeof(key));
	uint64_t id = 0;
	if (keyed && player_id_lookup(key, &id)) {
		*index = 0;
		return id;
	}
	id = source_id_uncached(filepath, index);
	if (keyed && id && *index == 0) {
		player_id_store(key, id);
	}
	return id;
}

uint64_t cover_source_id(const char *filepath) {
	int index;
	return player_source_id(filepath, &index);
}

bool cover_known_id(const char *filepath, uint64_t *id) {
	char key[32];
	return player_id_key(filepath, key, sizeof(key)) && player_id_lookup(key, id);
}

static bool cover_tone_load(uint64_t id, cover_image_t *img);

bool cover_tone_known(const char *filepath, uint32_t *tone, int *luma, bool *light) {
	uint64_t id = 0;
	cover_image_t img = {0};
	if (!cover_known_id(filepath, &id) || !cover_tone_load(id, &img)) {
		return false;
	}
	*tone = img.tone;
	*luma = img.tone_luma;
	*light = img.tone_light;
	return true;
}

bool cover_picture_known(const char *filepath) {
	uint64_t id = 0;
	if (!cover_known_id(filepath, &id)) {
		return false;
	}
	// Any size of it: the rows of one picture share the id and then '|', and
	// '}' is the character after '|', so the range is exactly those rows and
	// is answered from the key's index.
	char from[24], to[24];
	snprintf(from, sizeof(from), "%016llx|", (unsigned long long)id);
	snprintf(to, sizeof(to), "%016llx}", (unsigned long long)id);
	bool kept = false;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db, "SELECT 1 FROM player_covers WHERE key >= ?1 AND key < ?2 LIMIT 1", -1,
									   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, from, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, to, -1, SQLITE_STATIC);
		kept = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return kept;
}

static void player_cover_key(char *out, size_t out_size, uint64_t id, int w, int h) {
	snprintf(out, out_size, "%016llx|%dx%d|%d", (unsigned long long)id, w, h, PLAYER_COVER_EPOCH);
}

// A row's JPEG back as the cover it was made from. Its size has to be the one
// the key says, or the row is not the picture asked for.
static bool image_from_jpeg(const uint8_t *data, size_t size, int w, int h, cover_image_t *out) {
	int dw = 0, dh = 0;
	// The short side is under twice `w`, so TJpgDec decodes at 1:1.
	uint8_t *rgb = jpeg_scaled_decode(data, size, w > h ? w : h, &dw, &dh, NULL, NULL);
	if (!rgb) {
		int comp = 0;
		rgb = stbi_load_from_memory(data, (int)size, &dw, &dh, &comp, 3);
	}
	if (!rgb) {
		return false;
	}
	bool ok = dw == w && dh == h && pack_rgb565(rgb, w, h, out);
	free(rgb);
	return ok;
}

static bool player_cover_load(uint64_t id, int w, int h, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF) {
		return false;
	}
	char key[64];
	player_cover_key(key, sizeof(key), id, w, h);

	// The row is copied out under the lock and decoded after it, so a list
	// asking for its thumbnails is not held up behind a decode.
	uint8_t *blob = NULL;
	size_t blob_len = 0;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db, "SELECT w, h, pixels FROM player_covers WHERE key = ?1", -1, &stmt,
									   NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == w && sqlite3_column_int(stmt, 1) == h) {
			const void *data = sqlite3_column_blob(stmt, 2);
			int len = sqlite3_column_bytes(stmt, 2);
			if (data && len > 0 && (blob = malloc((size_t)len)) != NULL) {
				memcpy(blob, data, (size_t)len);
				blob_len = (size_t)len;
			}
		}
		sqlite3_finalize(stmt);
	}
	if (blob && sqlite3_prepare_v2(thumb_db,
								   "UPDATE player_covers SET used = (SELECT IFNULL(MAX(used), 0) + 1 FROM player_covers)"
								   " WHERE key = ?1 AND used < (SELECT MAX(used) FROM player_covers)",
								   -1, &stmt, NULL) == SQLITE_OK) {
		// Moved to the front of the queue the pruning deletes from -- unless it
		// is at the front already, which is every track of an album after the
		// first: an UPDATE that matches no row touches no page, so the card is
		// not written (journal, page, truncate) just to restate the order.
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	if (!blob) {
		return false;
	}

	bool hit;
	bool legacy = blob_len == (size_t)w * h * 2;
	if (legacy) {
		// Pixels, as an older build wrote them: used as they are, and the row
		// rewritten as a JPEG.
		hit = true;
		memset(out, 0, sizeof(*out));
		out->pixels = blob;
		out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		out->dsc.header.w = (uint32_t)w;
		out->dsc.header.h = (uint32_t)h;
		out->dsc.header.stride = (uint32_t)w * 2;
		out->dsc.data_size = (uint32_t)blob_len;
		out->dsc.data = blob;
		player_cover_store_soon(id, w, h, out);
	} else {
		hit = blob_len > 2 && blob[0] == 0xFF && blob[1] == 0xD8 && image_from_jpeg(blob, blob_len, w, h, out);
		free(blob);
	}
	return hit;
}

// Whether the cover of picture `id` is kept at w x h, without reading it.
static bool player_cover_kept(uint64_t id, int w, int h) {
	char key[64];
	player_cover_key(key, sizeof(key), id, w, h);
	bool kept = false;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db &&
		sqlite3_prepare_v2(thumb_db, "SELECT 1 FROM player_covers WHERE key = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		kept = sqlite3_step(stmt) == SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return kept;
}

// A cover kept for cover_store_deferred(): a copy, since the original goes to
// the screen and may be freed before the store runs.
static __thread bool store_deferred;
static __thread struct {
	uint64_t id;
	int w, h;
	cover_image_t img;
} deferred_store;

void cover_defer_player_store(bool on) { store_deferred = on; }

void cover_store_deferred(void) {
	if (deferred_store.img.pixels) {
		player_cover_store(deferred_store.id, deferred_store.w, deferred_store.h, &deferred_store.img);
		cover_free(&deferred_store.img);
	}
}

static void player_cover_store_soon(uint64_t id, int w, int h, const cover_image_t *img) {
	if (!store_deferred) {
		player_cover_store(id, w, h, img);
		return;
	}
	cover_free(&deferred_store.img);
	if (!img || !img->pixels || !(deferred_store.img.pixels = malloc(img->dsc.data_size))) {
		return;
	}
	memcpy(deferred_store.img.pixels, img->pixels, img->dsc.data_size);
	deferred_store.img.dsc = img->dsc;
	deferred_store.img.dsc.data = deferred_store.img.pixels;
	deferred_store.id = id;
	deferred_store.w = w;
	deferred_store.h = h;
}

static void player_cover_store(uint64_t id, int w, int h, const cover_image_t *img) {
	if (!img || !img->pixels) {
		return;
	}
	char key[64];
	player_cover_key(key, sizeof(key), id, w, h);
	int iw = (int)img->dsc.header.w;
	int ih = (int)img->dsc.header.h;
	if (iw != w || ih != h) {
		return; // the key would not describe it
	}

	// Encoded before the lock is taken: it is the slow part.
	raw_image_t raw;
	if (!raw_from_cover(img, &raw)) {
		return;
	}
	size_t jpeg_len = 0;
	uint8_t *jpeg = art_encode_jpeg(raw.pixels, raw.w, raw.h, PLAYER_COVER_QUALITY, &jpeg_len);
	raw_free(&raw);
	if (!jpeg) {
		return;
	}

	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db,
									   "INSERT OR REPLACE INTO player_covers(key, w, h, used, pixels) VALUES(?1, ?2, ?3,"
									   " (SELECT IFNULL(MAX(used), 0) + 1 FROM player_covers), ?4)",
									   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, iw);
		sqlite3_bind_int(stmt, 3, ih);
		sqlite3_bind_blob(stmt, 4, jpeg, (int)jpeg_len, SQLITE_STATIC);
		sqlite3_step(stmt); // a failure here only costs a future decode
		sqlite3_finalize(stmt);

		// Everything older than the newest PLAYER_COVER_MAX_ROWS goes.
		if (sqlite3_prepare_v2(thumb_db,
							   "DELETE FROM player_covers WHERE used <="
							   " (SELECT used FROM player_covers ORDER BY used DESC LIMIT 1 OFFSET ?1)",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, PLAYER_COVER_MAX_ROWS);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&thumb_db_lock);
	free(jpeg);
}

// ---------------------------------------------------------------------------
// the picture's colour
//
// What the pages that wear a sleeve's colour need from it: the colour an
// album's page is painted in, which the player is tinted with as well
// (cover_page_tone). Working it out is a few passes over the picture, small
// next to decoding it, but not something to repeat on the interface thread
// every time a page opens. It is kept in cover_colours under the picture's id,
// like the picture itself, so it is worked out once per picture, on the loader
// thread, and read back with the picture after that.
//
// The epoch is part of the key: raising it when the way the colour is worked
// out changes makes every stored row a miss.
// ---------------------------------------------------------------------------

#define COVER_TONE_EPOCH 1
#define COVER_TONE_MAX_ROWS 20000
#define COVER_TONE_PRUNE_EVERY 256

static void cover_tone_key(char *out, size_t out_size, uint64_t id) {
	snprintf(out, out_size, "%016llx|%d|%d", (unsigned long long)id, COVER_PAGE_LIGHT_FROM, COVER_TONE_EPOCH);
}

static bool cover_tone_load(uint64_t id, cover_image_t *img) {
	char key[64];
	cover_tone_key(key, sizeof(key), id);
	bool hit = false;
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db, "SELECT tone, luma, light FROM cover_colours WHERE key = ?1", -1,
									   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			img->tone = (uint32_t)sqlite3_column_int64(stmt, 0);
			img->tone_luma = (int16_t)sqlite3_column_int(stmt, 1);
			img->tone_light = sqlite3_column_int(stmt, 2) != 0;
			img->has_tone = true;
			hit = true;
		}
		sqlite3_finalize(stmt);
	}
	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

static void cover_tone_store(uint64_t id, const cover_image_t *img) {
	static unsigned stores;
	char key[64];
	cover_tone_key(key, sizeof(key), id);
	pthread_mutex_lock(&thumb_db_lock);
	sqlite3_stmt *stmt = NULL;
	if (thumb_db && sqlite3_prepare_v2(thumb_db,
									   "INSERT OR REPLACE INTO cover_colours(key, tone, luma, light) VALUES(?1, ?2, ?3, ?4)",
									   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 2, img->tone);
		sqlite3_bind_int(stmt, 3, img->tone_luma);
		sqlite3_bind_int(stmt, 4, img->tone_light ? 1 : 0);
		sqlite3_step(stmt); // a failure here only costs working it out again
		sqlite3_finalize(stmt);

		// A row is a few dozen bytes, so the table keeps many more pictures
		// than player_covers does; past that the oldest rows go.
		if (++stores % COVER_TONE_PRUNE_EVERY == 0 &&
			sqlite3_prepare_v2(thumb_db,
							   "DELETE FROM cover_colours WHERE rowid IN"
							   " (SELECT rowid FROM cover_colours ORDER BY rowid DESC LIMIT -1 OFFSET ?1)",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, COVER_TONE_MAX_ROWS);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&thumb_db_lock);
}

// Fills in the colour of `img`: read back under `id`, or worked out from the
// picture and written down. An id of 0 is a picture the id does not describe,
// whose colour is worked out and not kept.
static void cover_tone_attach(uint64_t id, cover_image_t *img) {
	img->has_tone = false;
	if (!img->pixels) {
		return;
	}
	if (id && cover_tone_load(id, img)) {
		return;
	}
	uint32_t tone = 0;
	int luma = 0;
	bool light = false;
	if (!cover_page_tone(img, COVER_PAGE_LIGHT_FROM, &tone, &luma, &light)) {
		return;
	}
	img->tone = tone;
	img->tone_luma = (int16_t)luma;
	img->tone_light = light;
	img->has_tone = true;
	if (id) {
		cover_tone_store(id, img);
	}
}
