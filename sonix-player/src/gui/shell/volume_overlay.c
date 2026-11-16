#include "volume_overlay.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/shell/theme.h"
#include "src/system/audio/alsa-controls.h"
#include "src/gui/shell/uiscale.h"

// GNOME's OSD look: a rounded pill, icon on the left, a slim scale, the number
// on the right. The scale is a real slider, so the level can be dragged with a
// finger while the pill is up.
//
// The pill follows the theme rather than being dark in both: it sits over the
// controls block as well as over artwork, and a fixed dark pill would be the
// only black thing on a light screen.
#define OVERLAY_WIDTH ui_px(340)
#define OVERLAY_HEIGHT ui_px(72)
#define OVERLAY_TOP_Y ui_px(96) // clear of the artwork
#define OVERLAY_RADIUS (OVERLAY_HEIGHT / 2)
#define OVERLAY_HIDE_MS 1600

// Where the icon changes: silent, quiet, loud.
#define VOLUME_LOW_PERCENT 1
#define VOLUME_HIGH_PERCENT 55

static lv_obj_t *overlay;
// Invisible and full-screen, and it takes no input: a tap or a drag anywhere
// but the pill goes through to the page underneath.
static lv_obj_t *veil;
static lv_obj_t *overlay_icon;
static lv_obj_t *overlay_slider;
static lv_obj_t *overlay_label;
static lv_timer_t *hide_timer;

// What the pill is currently showing, so a theme switch can recolour the
// number without having to be told the level again.
static int shown_percent;

// The number: the ordinary text colour, or the warning red past the point where
// every further step is loud. The colour is always set, never cleared: clearing
// it drops the label back to LVGL's default black as soon as the level falls
// below the warning, which is unreadable on a dark pill.
static void apply_label_color(int percent) {
	lv_color_t colour = percent > VOLUME_WARN_PERCENT ? VOLUME_WARN_COLOR : theme()->text_primary;
	lv_obj_set_style_text_color(overlay_label, colour, 0);
}

// Everything on the pill whose colour comes from the palette. Called when it is
// built, every time it is shown, and after a theme switch.
static void apply_theme(void) {
	if (!overlay) {
		return;
	}

	lv_obj_set_style_bg_color(overlay, theme()->panel, 0);
	lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);

	lv_obj_set_style_image_recolor(overlay_icon, theme()->text_primary, 0);
	lv_obj_set_style_image_recolor_opa(overlay_icon, LV_OPA_COVER, 0);

	apply_label_color(shown_percent);

	// The unfilled part of the scale: the text colour at low opacity reads as
	// a quiet groove against either background, where a fixed white would read
	// as nothing at all on the light one.
	lv_obj_set_style_bg_color(overlay_slider, theme()->text_primary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_20, LV_PART_MAIN);

	lv_obj_set_style_bg_color(overlay_slider, theme()->accent, LV_PART_INDICATOR);
	theme_apply_slider_knob(overlay_slider);
}

static void hide_cb(lv_timer_t *timer) {
	(void)timer;
	// Not while a finger is on the slider: the pill must never vanish
	// mid-drag.
	if (overlay_slider && lv_obj_has_state(overlay_slider, LV_STATE_PRESSED)) {
		lv_timer_reset(hide_timer);
		return;
	}
	lv_obj_set_hidden(veil, true);
	lv_timer_pause(hide_timer);
}

static void set_icon_for(int percent) {
	const lv_image_dsc_t *icon = &icon_volume_high;
	if (volume_lock_enabled()) {
		icon = &icon_volume_locked;
	} else if (percent < VOLUME_LOW_PERCENT) {
		icon = &icon_volume_mute;
	} else if (percent < VOLUME_HIGH_PERCENT) {
		icon = &icon_volume_low;
	}
	lv_image_set_src(overlay_icon, icon);
}

// With the volume locked the scale is a gauge and not a control: it takes no
// finger and loses its knob, which is what says so. Checked every time the pill
// comes up, since the lock is switched from the control centre.
static void apply_lock(void) {
	bool locked = volume_lock_enabled();
	if (locked == lv_obj_has_state(overlay_slider, LV_STATE_DISABLED)) {
		return;
	}
	if (locked) {
		lv_obj_add_state(overlay_slider, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(overlay_slider, LV_STATE_DISABLED);
	}
	lv_obj_set_style_opa(overlay_slider, locked ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
}

// The finger moved the slider: apply it and keep the pill up.
static void slider_changed_cb(lv_event_t *e) {
	(void)e;
	int percent = (int)lv_slider_get_value(overlay_slider);

	if (!volume_lock_enabled()) {
		set_volume_percent(percent);
	}

	// What the output is really at, which is not always what was asked for:
	// line out holds a fixed level, and a pill showing a number the hardware
	// ignored would be the one thing on screen saying otherwise.
	percent = get_volume_percent();
	lv_slider_set_value(overlay_slider, percent, LV_ANIM_OFF);
	set_icon_for(percent);
	shown_percent = percent;
	lv_label_set_text_fmt(overlay_label, "%d", percent);
	apply_label_color(percent);

	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

// See volume_overlay.h.
static bool allowed_outside_player;

void volume_overlay_allow_outside_player(bool allow) { allowed_outside_player = allow; }

void volume_overlay_show(int percent) {
	// The screensaver is the third place with no status bar, and the one where
	// the level is most likely to be moved: the screen has just lit up, the
	// music is playing, and the keys are the only thing being touched.
	bool over_saver = screensaver_is_visible();
	// Nor in line out, where the level is fixed (the status bar hides it too).
	if (!overlay || lineout_is_active() || !(player_sheet_is_open() || allowed_outside_player || over_saver)) {
		return;
	}

	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}

	set_icon_for(percent);
	shown_percent = percent;
	apply_lock();

	// Re-read the palette each time, so a theme or accent change shows up
	// without a restart.
	apply_theme();

	// Never yank the knob out from under a finger.
	if (!lv_obj_has_state(overlay_slider, LV_STATE_PRESSED)) {
		lv_slider_set_value(overlay_slider, percent, LV_ANIM_OFF);
	}
	lv_label_set_text_fmt(overlay_label, "%d", percent);
	apply_label_color(percent);

	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);

	// Every key press pushes the dismissal back, so holding the key keeps the
	// bar up for as long as the level is moving.
	lv_timer_reset(hide_timer);
	lv_timer_resume(hide_timer);
}

void volume_overlay_init(gui_config_t *cfg) {
	(void)cfg;

	// The veil under the pill. A parent that takes no input does not stop its
	// children from taking theirs: the pill and its slider still get the finger.
	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_clickable(veil, false);
	lv_obj_set_hidden(veil, true);

	overlay = lv_obj_create(veil);
	lv_obj_set_size(overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT);
	lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, OVERLAY_TOP_Y);
	lv_obj_set_style_radius(overlay, OVERLAY_RADIUS, 0);
	lv_obj_set_style_border_width(overlay, 0, 0);
	lv_obj_set_style_shadow_width(overlay, 0, 0);
	lv_obj_set_style_pad_hor(overlay, ui_px(22), 0);
	lv_obj_set_style_pad_ver(overlay, 0, 0);
	lv_obj_set_style_pad_gap(overlay, ui_px(16), 0);
	lv_obj_set_scrollable(overlay, false);
	lv_obj_set_event_bubble(overlay, false);

	lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	overlay_icon = lv_image_create(overlay);
	lv_image_set_src(overlay_icon, &icon_volume_high);

	// The number rides right beside the glyph; the scale takes the rest.
	overlay_label = lv_label_create(overlay);
	lv_label_set_text(overlay_label, "0");
	lv_obj_set_style_text_font(overlay_label, &font_ui_24, 0);
	lv_obj_set_width(overlay_label, ui_px(42));
	lv_obj_set_style_text_align(overlay_label, LV_TEXT_ALIGN_CENTER, 0);

	// The scale: an Adwaita slider (slim trough, accent fill, white knob) that
	// takes the finger directly.
	overlay_slider = lv_slider_create(overlay);
	lv_obj_set_height(overlay_slider, ui_px(10));
	lv_obj_set_flex_grow(overlay_slider, 1);
	lv_slider_set_range(overlay_slider, 0, 100);

	lv_obj_set_style_radius(overlay_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(overlay_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_COVER, LV_PART_INDICATOR);

	lv_obj_set_style_radius(overlay_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(overlay_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(overlay_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(overlay_slider, ui_px(7), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(overlay_slider, ui_px(6), LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(overlay_slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(overlay_slider, lv_color_black(), LV_PART_KNOB);

	// A slim trough is a hard touch target; give the finger the pill's whole
	// height to land on.
	lv_obj_set_ext_click_area(overlay_slider, (OVERLAY_HEIGHT - ui_px(6)) / 2);
	lv_obj_add_event_cb(overlay_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	hide_timer = lv_timer_create(hide_cb, OVERLAY_HIDE_MS, NULL);
	lv_timer_pause(hide_timer);

	apply_theme();
	theme_register_refresh(apply_theme);
}
