// Three places where LVGL 9.6 spends the time of building a page on work that
// changes nothing, taken out at the linker (--wrap, LVGL_FAST_WRAP in the
// Makefile) so that no LVGL source is touched.
//
// Measured on the Blaze with the sampling profiler over five starts: building
// the interface is 1.9 s of CPU, and of the samples on the interface thread
// 35% were in lv_event_send() and lv_array_at(), 9% in lv_obj_get_display() and
// 9% in lv_ap_get_char_index(). All three are per object or per character,
// and the pages are built whether or not they are ever shown.

#include "lvgl/lvgl.h"
#include "src/system/device/power.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The refresh request nobody needs
//
// lv_obj_mark_layout_as_dirty() ends by sending LV_EVENT_REFR_REQUEST to the
// display, and it runs on every style set and every child created. Sending an
// event walks the display's whole handler list twice -- and that list also
// holds one entry for every label waiting for its text to be laid out, a
// hundred and more while a page is being built.
//
// The event has two listeners and both only look after the refresh timer:
// LVGL's own resumes it, and power.c pauses it again while the screen is off.
// With the timer already running and the screen on, neither would do anything.
// That is the case every time but the first after a pause, so then the event
// is not sent. Anything else -- the timer paused, the screen off -- goes
// through as before.
// ---------------------------------------------------------------------------

lv_result_t __real_lv_display_send_event(lv_display_t *disp, lv_event_code_t code, void *param);

lv_result_t __wrap_lv_display_send_event(lv_display_t *disp, lv_event_code_t code, void *param) {
	if (code == LV_EVENT_REFR_REQUEST && disp && param == NULL) {
		lv_timer_t *refr = lv_display_get_refr_timer(disp);
		if (refr && !lv_timer_get_paused(refr) && power_screen_is_on()) {
			return LV_RESULT_OK;
		}
	}
	return __real_lv_display_send_event(disp, code, param);
}

// ---------------------------------------------------------------------------
// Which display an object is on
//
// LVGL answers by climbing to the object's screen and then looking for that
// screen in every display's list of screens: some seventy comparisons here,
// where every page is a screen of its own. There is one display, so that is
// the answer; with more than one, or none yet, LVGL's own search runs.
// ---------------------------------------------------------------------------

lv_display_t *__real_lv_obj_get_display(const lv_obj_t *obj);

lv_display_t *__wrap_lv_obj_get_display(const lv_obj_t *obj) {
	lv_display_t *only = lv_display_get_next(NULL);
	if (obj && only && lv_display_get_next(only) == NULL) {
		return only;
	}
	return __real_lv_obj_get_display(obj);
}

// ---------------------------------------------------------------------------
// Arabic and Persian shaping, for text that has none
//
// Every text a label is given goes through lv_text_ap_strlen() and
// lv_text_ap_proc(), which look each character up in the table of Arabic
// letters -- forty-odd entries, compared one by one -- to pick its joined form.
// A text with no character from U+0600 up comes out byte for byte as it went
// in, so it is copied instead. Decided on the bytes: ASCII, and two-byte
// sequences below U+0600 (Latin, Greek, Cyrillic) that are well formed.
// Everything else, CJK and malformed text included, takes LVGL's path.
// ---------------------------------------------------------------------------

size_t __real_lv_text_ap_strlen(const char *txt);
void __real_lv_text_ap_proc(const char *txt, char *txt_out);

// The length in bytes when `txt` is certain to pass through unchanged, or -1.
static long plain_length(const char *txt) {
	const unsigned char *p = (const unsigned char *)txt;
	for (;;) {
		unsigned char c = *p;
		if (c == 0) {
			return (long)(p - (const unsigned char *)txt);
		}
		if (c < 0x80) {
			p++;
		} else if (c >= 0xC2 && c <= 0xD7 && (p[1] & 0xC0) == 0x80) {
			p += 2; // U+0080 .. U+05FF
		} else {
			return -1;
		}
	}
}

size_t __wrap_lv_text_ap_strlen(const char *txt) {
	long n = txt ? plain_length(txt) : -1;
	return n >= 0 ? (size_t)n : __real_lv_text_ap_strlen(txt);
}

void __wrap_lv_text_ap_proc(const char *txt, char *txt_out) {
	long n = txt && txt_out ? plain_length(txt) : -1;
	if (n < 0) {
		__real_lv_text_ap_proc(txt, txt_out);
		return;
	}
	// The label hands its own buffer in as both: memmove, not memcpy.
	memmove(txt_out, txt, (size_t)n + 1);
}
