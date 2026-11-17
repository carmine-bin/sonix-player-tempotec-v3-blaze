#include "meterview.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/uiscale.h"
#include "src/system/audio/levelmeter.h"
#include "src/system/core/config.h"
#include "src/system/device/power.h"

#include <math.h>
#include <string.h>

#define FRAME_MS 40	 // 25 a second: the panel runs at 45, a meter needs no more
#define IDLE_MS 500	 // the check while it is not on screen
#define CLIP_HOLD_MS 2000
#define PEAK_HOLD_MS 1500

#define PEAK_FLOOR_DB (-48.0f)
#define PEAK_FALL_DB_S (24.0f / 2.8f) // the IEC/BBC-style return

#define VU_REF_DBFS (-18.0f) // 0 VU, the usual digital alignment
#define VU_MIN (-20.0f)
#define VU_MAX (3.0f)
#define VU_TAU_S 0.065f // 99% of a step in 300 ms

#define BANDS 20
#define SPEC_FLOOR_DB (-66.0f)
#define SPEC_FALL_DB_S 20.0f
#define SPEC_LOW_HZ 60.0f
#define SPEC_HIGH_HZ 16000.0f
#define FFT_N LEVELMETER_SPECTRUM_SAMPLES

static lv_obj_t *band;
static lv_timer_t *timer;
static bool (*where_cb)(lv_area_t *rel);
static meter_mode_t mode;
static bool mode_loaded;

// What is drawn, in dB, after the ballistics.
static float shown_db[2] = {PEAK_FLOOR_DB, PEAK_FLOOR_DB};
static float hold_db[2] = {PEAK_FLOOR_DB, PEAK_FLOOR_DB};
static uint32_t hold_at[2];
static float vu_level[2]; // linear rms, integrated
// What the VU integrates towards: the last window's rms, kept between reads.
// A block is a period of the output (90 ms at 44.1 kHz) and this runs every
// 40 ms, so most frames find no new block; integrating towards silence on
// those read a steady tone ten decibels low.
static float vu_target[2];
static uint32_t got_at;
#define SIGNAL_GONE_MS 250
static float spec_db[BANDS];
static uint32_t clip_at;
static bool clip_seen;
static uint32_t last_tick;
static lv_area_t placed;

meter_mode_t meterview_mode(void) {
	if (!mode_loaded) {
		long v = config_get_int("screen", "meter", METER_OFF);
		mode = (v >= METER_OFF && v <= METER_SPECTRUM) ? (meter_mode_t)v : METER_OFF;
		mode_loaded = true;
	}
	return mode;
}

static void reset_ballistics(void) {
	for (int c = 0; c < 2; c++) {
		shown_db[c] = hold_db[c] = PEAK_FLOOR_DB;
		vu_level[c] = vu_target[c] = 0.0f;
	}
	for (int i = 0; i < BANDS; i++) {
		spec_db[i] = SPEC_FLOOR_DB;
	}
	clip_seen = false;
}

void meterview_set_mode(meter_mode_t m) {
	meterview_mode();
	mode = m;
	config_set_int("screen", "meter", (long)m);
	config_save();
	reset_ballistics();
	if (timer) {
		lv_timer_ready(timer);
	}
}

static float to_db(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }

// ---------------------------------------------------------------------------
// The spectrum
// ---------------------------------------------------------------------------

static float fft_re[FFT_N], fft_im[FFT_N], window[FFT_N];
static bool window_ready;

static void fft(float *re, float *im, int n) {
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wr = cosf(ang), wi = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int a = i + k, b = i + k + len / 2;
				float tr = re[b] * cr - im[b] * ci;
				float ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

// Each band's level now, in dB of full scale; false when nothing is playing.
static bool spectrum_now(float *db) {
	int rate = 0;
	if (!levelmeter_read_samples(fft_re, FFT_N, &rate)) {
		return false;
	}
	if (!window_ready) {
		for (int i = 0; i < FFT_N; i++) {
			window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(FFT_N - 1));
		}
		window_ready = true;
	}
	for (int i = 0; i < FFT_N; i++) {
		fft_re[i] *= window[i];
		fft_im[i] = 0.0f;
	}
	fft(fft_re, fft_im, FFT_N);
	float top = fminf(SPEC_HIGH_HZ, (float)rate * 0.45f);
	float bin_hz = (float)rate / (float)FFT_N;
	float ratio = powf(top / SPEC_LOW_HZ, 1.0f / BANDS);
	float lo = SPEC_LOW_HZ;
	for (int b = 0; b < BANDS; b++) {
		float hi = lo * ratio;
		int k0 = (int)(lo / bin_hz + 0.5f), k1 = (int)(hi / bin_hz + 0.5f);
		if (k0 < 1) {
			k0 = 1;
		}
		if (k1 < k0) {
			k1 = k0;
		}
		if (k1 >= FFT_N / 2) {
			k1 = FFT_N / 2 - 1;
		}
		float m = 0.0f;
		for (int k = k0; k <= k1; k++) {
			float v = fft_re[k] * fft_re[k] + fft_im[k] * fft_im[k];
			if (v > m) {
				m = v;
			}
		}
		// A full-scale sine through a Hann window peaks at N/4 in its bin.
		db[b] = to_db(sqrtf(m) * 4.0f / (float)FFT_N);
		lo = hi;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The ballistics, once per frame
// ---------------------------------------------------------------------------

static void step(uint32_t now) {
	float dt = last_tick ? (float)(now - last_tick) / 1000.0f : FRAME_MS / 1000.0f;
	if (dt > 0.5f) {
		dt = 0.5f;
	}
	last_tick = now;

	levelmeter_reading_t r;
	bool got = levelmeter_read(&r);
	if (got && r.clipped) {
		clip_at = now;
		clip_seen = true;
	}
	if (got) {
		vu_target[0] = r.rms[0];
		vu_target[1] = r.rms[1];
		got_at = now;
	} else if (now - got_at > SIGNAL_GONE_MS) {
		vu_target[0] = vu_target[1] = 0.0f;
	}

	if (mode == METER_SPECTRUM) {
		float db[BANDS];
		bool live = spectrum_now(db);
		for (int b = 0; b < BANDS; b++) {
			float fallen = spec_db[b] - SPEC_FALL_DB_S * dt;
			float target = live ? db[b] : SPEC_FLOOR_DB;
			spec_db[b] = fmaxf(fmaxf(target, fallen), SPEC_FLOOR_DB);
		}
		return;
	}

	for (int c = 0; c < 2; c++) {
		if (mode == METER_PEAK) {
			float fallen = shown_db[c] - PEAK_FALL_DB_S * dt;
			float db = got ? to_db(r.peak[c]) : PEAK_FLOOR_DB;
			shown_db[c] = fmaxf(fmaxf(db, fallen), PEAK_FLOOR_DB);
			if (shown_db[c] >= hold_db[c] || now - hold_at[c] > PEAK_HOLD_MS) {
				hold_db[c] = shown_db[c];
				hold_at[c] = now;
			}
		} else {
			vu_level[c] += (vu_target[c] - vu_level[c]) * (1.0f - expf(-dt / VU_TAU_S));
			shown_db[c] = to_db(vu_level[c]) - VU_REF_DBFS; // in VU
		}
	}
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t color, lv_opa_t opa,
				 int32_t radius) {
	if (x2 < x1 || y2 < y1) {
		return;
	}
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.bg_opa = opa;
	dsc.radius = radius;
	lv_area_t a = {x1, y1, x2, y2};
	lv_draw_rect(layer, &dsc, &a);
}

static void text(lv_layer_t *layer, const char *s, int32_t x1, int32_t y1, int32_t x2, lv_text_align_t align,
				 lv_color_t color) {
	lv_draw_label_dsc_t dsc;
	lv_draw_label_dsc_init(&dsc);
	dsc.text = s;
	dsc.font = &font_ui_14;
	dsc.color = color;
	dsc.opa = LV_OPA_80;
	dsc.align = align;
	lv_area_t a = {x1, y1, x2, y1 + font_ui_14.line_height};
	lv_draw_label(layer, &dsc, &a);
}

static const lv_color_t CLIP_RED = LV_COLOR_MAKE(224, 27, 36);
static const lv_color_t HOT_AMBER = LV_COLOR_MAKE(245, 180, 40);

// One horizontal bar from `lo` to `hi` (dB or VU), filled to `value`, with the
// part past `hot` in amber.
static void hbar(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, float lo, float hi, float hot,
				 float value, float hold) {
	int32_t w = x2 - x1 + 1;
	rect(layer, x1, y1, x2, y2, lv_color_white(), LV_OPA_20, ui_px(3));
	float f = fminf(fmaxf((value - lo) / (hi - lo), 0.0f), 1.0f);
	float fh = fminf(fmaxf((hot - lo) / (hi - lo), 0.0f), 1.0f);
	int32_t end = x1 + (int32_t)(f * (float)w) - 1;
	int32_t hot_x = x1 + (int32_t)(fh * (float)w);
	rect(layer, x1, y1, end < hot_x - 1 ? end : hot_x - 1, y2, theme()->accent, LV_OPA_COVER, ui_px(3));
	if (end >= hot_x) {
		rect(layer, hot_x, y1, end, y2, HOT_AMBER, LV_OPA_COVER, ui_px(3));
	}
	if (hold > lo) {
		int32_t hx = x1 + (int32_t)(fminf((hold - lo) / (hi - lo), 1.0f) * (float)w) - ui_px(2);
		rect(layer, hx, y1, hx + ui_px(2), y2, lv_color_white(), LV_OPA_80, 0);
	}
}

static void draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer || mode == METER_OFF) {
		return;
	}
	lv_area_t a;
	lv_obj_get_coords(band, &a);
	int32_t pad = ui_px(14);
	int32_t clip_w = ui_px(10);
	int32_t x1 = a.x1 + pad, x2 = a.x2 - pad - clip_w - ui_px(8);
	int32_t top = a.y1 + ui_px(10), bottom = a.y2 - ui_px(8);
	lv_color_t ink = lv_color_white();
	bool clip_lit = clip_seen && lv_tick_elaps(clip_at) < CLIP_HOLD_MS;

	// The clip mark, always in the same place, dark until the output clips.
	rect(layer, a.x2 - pad - clip_w + 1, top, a.x2 - pad, bottom - font_ui_14.line_height - ui_px(4),
		 clip_lit ? CLIP_RED : lv_color_white(), clip_lit ? LV_OPA_COVER : LV_OPA_20, ui_px(3));

	if (mode == METER_SPECTRUM) {
		int32_t base = bottom - font_ui_14.line_height - ui_px(4);
		int32_t span = base - top;
		int32_t gap = ui_px(3);
		int32_t bw = ((x2 - x1 + 1) - gap * (BANDS - 1)) / BANDS;
		for (int b = 0; b < BANDS; b++) {
			int32_t bx = x1 + b * (bw + gap);
			float f = (spec_db[b] - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB;
			int32_t h = (int32_t)(fminf(fmaxf(f, 0.0f), 1.0f) * (float)span);
			rect(layer, bx, top, bx + bw - 1, base, lv_color_white(), LV_OPA_10, ui_px(2));
			rect(layer, bx, base - h, bx + bw - 1, base, theme()->accent, LV_OPA_COVER, ui_px(2));
		}
		text(layer, "60", x1, bottom - font_ui_14.line_height, x1 + ui_px(60), LV_TEXT_ALIGN_LEFT, ink);
		text(layer, "1k", (x1 + x2) / 2 - ui_px(30), bottom - font_ui_14.line_height, (x1 + x2) / 2 + ui_px(30),
			 LV_TEXT_ALIGN_CENTER, ink);
		text(layer, "16k", x2 - ui_px(60), bottom - font_ui_14.line_height, x2, LV_TEXT_ALIGN_RIGHT, ink);
		return;
	}

	bool vu = mode == METER_VU;
	float lo = vu ? VU_MIN : PEAK_FLOOR_DB, hi = vu ? VU_MAX : 0.0f, hot = vu ? 0.0f : -6.0f;
	int32_t label_w = ui_px(18);
	int32_t bx1 = x1 + label_w;
	int32_t scale_h = font_ui_14.line_height;
	int32_t bars_bottom = bottom - scale_h - ui_px(4);
	int32_t bar_h = (bars_bottom - top - ui_px(6)) / 2;
	static const char *const CH[2] = {"L", "R"};
	for (int c = 0; c < 2; c++) {
		int32_t y1 = top + c * (bar_h + ui_px(6));
		text(layer, CH[c], x1, y1 + (bar_h - font_ui_14.line_height) / 2, bx1 - ui_px(4), LV_TEXT_ALIGN_LEFT, ink);
		hbar(layer, bx1, y1, x2, y1 + bar_h - 1, lo, hi, hot, shown_db[c], vu ? lo : hold_db[c]);
	}
	static const float PEAK_TICKS[] = {-40, -20, -10, -6, -3, 0};
	static const char *const PEAK_NAMES[] = {"-40", "-20", "-10", "-6", "-3", "0"};
	static const float VU_TICKS[] = {-20, -10, -5, -3, 0, 3};
	static const char *const VU_NAMES[] = {"-20", "-10", "-5", "-3", "0", "+3"};
	const float *ticks = vu ? VU_TICKS : PEAK_TICKS;
	const char *const *names = vu ? VU_NAMES : PEAK_NAMES;
	int32_t w = x2 - bx1 + 1;
	for (int i = 0; i < 6; i++) {
		int32_t tx = bx1 + (int32_t)((ticks[i] - lo) / (hi - lo) * (float)w);
		lv_text_align_t al = i == 5 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
		int32_t tx1 = i == 5 ? tx - ui_px(40) : tx - ui_px(20), tx2 = i == 5 ? tx : tx + ui_px(20);
		text(layer, names[i], tx1, bottom - scale_h, tx2, al, ink);
	}
	text(layer, vu ? "VU" : "dBFS", a.x2 - pad - ui_px(60), bottom - scale_h, a.x2 - pad, LV_TEXT_ALIGN_RIGHT, ink);
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	lv_area_t rel;
	bool show = meterview_mode() != METER_OFF && power_screen_is_on() && where_cb && where_cb(&rel);
	if (!show) {
		if (!lv_obj_is_hidden(band)) {
			lv_obj_set_hidden(band, true);
			reset_ballistics();
		}
		levelmeter_set_wanted(false);
		lv_timer_set_period(t, IDLE_MS);
		last_tick = 0;
		return;
	}
	lv_timer_set_period(t, FRAME_MS);
	levelmeter_set_wanted(true);
	if (memcmp(&rel, &placed, sizeof(rel)) != 0) {
		placed = rel;
		lv_obj_set_pos(band, rel.x1, rel.y1);
		lv_obj_set_size(band, lv_area_get_width(&rel), lv_area_get_height(&rel));
	}
	if (lv_obj_is_hidden(band)) {
		lv_obj_set_hidden(band, false);
		lv_obj_move_foreground(band);
	}
	step(lv_tick_get());
	lv_obj_invalidate(band);
}

void meterview_create(lv_obj_t *panel, bool (*where)(lv_area_t *rel)) {
	where_cb = where;
	meterview_mode();
	reset_ballistics();

	band = lv_obj_create(panel);
	lv_obj_remove_style_all(band);
	lv_obj_set_style_bg_color(band, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(band, LV_OPA_60, 0);
	// Takes no press: the artwork under it is what the player is dragged by.
	lv_obj_set_clickable(band, false);
	lv_obj_set_scrollable(band, false);
	lv_obj_add_event_cb(band, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
	lv_obj_set_hidden(band, true);

	timer = lv_timer_create(tick_cb, IDLE_MS, NULL);
	power_pause_in_standby(timer);
}
