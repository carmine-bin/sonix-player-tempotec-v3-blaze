#include "confirm.h"

#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/gui/shell/uiscale.h"

// The dialog is built once and reused: on a device with 64 MB and no
// allocator to speak of, a modal that builds and tears down its widgets on
// every question is a leak waiting to happen.

#define CARD_W ui_px(400)
#define BUTTON_H ui_px(64)

static lv_obj_t *veil;
static lv_obj_t *card;
static lv_obj_t *title_label;
static lv_obj_t *message_label;
static lv_obj_t *ok_label;
static lv_obj_t *ok_btn;
static lv_obj_t *cancel_label;
static lv_obj_t *cancel_btn;

static void (*pending_action)(void *user);
static void *pending_user;

static void refresh_theme(void);

void confirm_close(void) {
	if (veil) {
		lv_obj_set_hidden(veil, true);
	}
}

bool confirm_is_open(void) { return veil && !lv_obj_is_hidden(veil); }

// The action runs one tick after the card is down, so a callback that loads
// another screen never does it from under the dialog it was opened by.
static void run_pending_cb(void *unused) {
	(void)unused;
	void (*action)(void *) = pending_action;
	void *user = pending_user;
	pending_action = NULL;
	pending_user = NULL;
	if (action) {
		action(user);
	}
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	pending_action = NULL;
	pending_user = NULL;
	confirm_close();
}

static void ok_cb(lv_event_t *e) {
	(void)e;
	confirm_close();
	lv_async_call(run_pending_cb, NULL);
}

// Taps on the dark surround are swallowed, not treated as a "no": a scan or a
// deletion is worth an explicit answer, and a stray tap beside the card that
// dismissed the question would read as the dialog having done something.
static void veil_cb(lv_event_t *e) {
	(void)e;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, bool accent, lv_event_cb_t cb,
							 lv_obj_t **label_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_height(btn, BUTTON_H);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, ui_px(12), 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

	if (accent) {
		lv_obj_set_style_bg_color(btn, theme()->accent, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_style(btn, &theme_style_switch, 0); // the quiet neutral fill
	}

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, accent ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(label);

	if (label_out) {
		*label_out = label;
	}
	return btn;
}

void confirm_init(gui_config_t *cfg) {
	if (veil) {
		return;
	}

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	lv_obj_set_width(card, CARD_W < cfg->screen_width - ui_px(40) ? CARD_W : cfg->screen_width - ui_px(40));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ui_px(18), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, ui_px(22), 0);
	lv_obj_set_style_pad_gap(card, ui_px(14), 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	title_label = lv_label_create(card);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

	message_label = lv_label_create(card);
	lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(message_label, lv_pct(100));
	lv_obj_add_style(message_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(message_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, 0);

	lv_obj_t *buttons = lv_obj_create(card);
	lv_obj_set_width(buttons, lv_pct(100));
	lv_obj_set_height(buttons, BUTTON_H);
	lv_obj_set_style_bg_opa(buttons, 0, 0);
	lv_obj_set_style_border_width(buttons, 0, 0);
	lv_obj_set_style_pad_all(buttons, 0, 0);
	lv_obj_set_style_pad_gap(buttons, ui_px(12), 0);
	lv_obj_set_scrollable(buttons, false);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	cancel_btn = make_button(buttons, "cancel", false, cancel_cb, &cancel_label);
	ok_btn = make_button(buttons, "ok", true, ok_cb, &ok_label);

	theme_register_refresh(refresh_theme);
}

// The card is built once, under whichever theme was active at startup, and the
// accent fill and the cancel label carry local colours. A local style wins over
// the shared one, so they have to be repainted by hand on every theme change.
static void refresh_theme(void) {
	if (ok_btn) {
		lv_obj_set_style_bg_color(ok_btn, theme()->accent, 0);
	}
	if (cancel_label) {
		lv_obj_set_style_text_color(cancel_label, theme()->text_primary, 0);
	}
}

void confirm_show(const char *title, const char *message, const char *ok_text, void (*on_ok)(void *user),
				  void *user) {
	if (!veil) {
		return;
	}

	pending_action = on_ok;
	pending_user = user;

	lv_label_set_text(title_label, title ? tr(title) : "");
	if (message && message[0]) {
		lv_label_set_text(message_label, tr(message));
		lv_obj_set_hidden(message_label, false);
	} else {
		lv_obj_set_hidden(message_label, true);
	}
	lv_label_set_text(ok_label, tr(ok_text && ok_text[0] ? ok_text : "ok"));
	// Repaint on the way up too, in case the theme changed while the dialog was
	// hidden.
	refresh_theme();

	if (cancel_btn) {
		lv_obj_set_hidden(cancel_btn, false);
	}

	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
}

void confirm_notice(const char *title, const char *message) {
	confirm_show(title, message, "ok", NULL, NULL);
	// A notice has nothing to cancel: there is one way out and it is the same
	// as the other one. Two buttons that do the same thing ask a question that
	// was never put.
	if (cancel_btn) {
		lv_obj_set_hidden(cancel_btn, true);
	}
}
