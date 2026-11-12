#include "dacpage.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/audio/usbdac.h"
#include "src/system/device/usb.h"

lv_obj_t *dacpage_screen;

static lv_obj_t *mode_switch;
static lv_obj_t *big_icon;
static lv_obj_t *format_label;
static lv_obj_t *status_label;
static lv_obj_t *charge_btn;
static lv_obj_t *charge_glyph;
static lv_timer_t *poll_timer;
static unsigned last_serial = (unsigned)-1;
static lv_color_t status_normal_color; // the theme's colour, captured at build time

static lv_obj_t *buffer_card;
static lv_obj_t *buffer_slider;
static lv_obj_t *buffer_value;
static lv_point_t drag_start_pt;
static int drag_start_val_idx;
static bool is_dragging_value;

static const int BUFFER_STEPS_MS[] = {20, 40, 60, 80, 100, 150, 200, 300, 500};
#define BUFFER_STEP_COUNT (sizeof(BUFFER_STEPS_MS) / sizeof(BUFFER_STEPS_MS[0]))

static int buffer_ms_to_index(int ms) {
	int best_idx = 3; // 80 ms default
	int best_diff = 99999;
	for (int i = 0; i < (int)BUFFER_STEP_COUNT; i++) {
		int diff = abs(BUFFER_STEPS_MS[i] - ms);
		if (diff < best_diff) {
			best_diff = diff;
			best_idx = i;
		}
	}
	return best_idx;
}

static void format_buffer_label(char *out, size_t out_size, int ms) {
	if (ms == 80) {
		snprintf(out, out_size, "%d ms (Def)", ms);
	} else {
		snprintf(out, out_size, "%d ms", ms);
	}
}

static void apply_buffer_index(int idx) {
	if (idx < 0) idx = 0;
	if (idx >= (int)BUFFER_STEP_COUNT) idx = (int)BUFFER_STEP_COUNT - 1;
	int ms = BUFFER_STEPS_MS[idx];
	usbdac_set_output_ms(ms);
	if (buffer_value) {
		char text[32];
		format_buffer_label(text, sizeof(text), ms);
		lv_label_set_text(buffer_value, text);
	}
	if (buffer_slider && lv_slider_get_value(buffer_slider) != idx) {
		lv_slider_set_value(buffer_slider, idx, LV_ANIM_OFF);
	}
}

static void buffer_slider_cb(lv_event_t *e) {
	(void)e;
	int idx = lv_slider_get_value(buffer_slider);
	apply_buffer_index(idx);
}

static void buffer_value_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) return;

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &drag_start_pt);
		drag_start_val_idx = lv_slider_get_value(buffer_slider);
		is_dragging_value = false;
	} else if (code == LV_EVENT_PRESSING) {
		lv_point_t cur_pt;
		lv_indev_get_point(indev, &cur_pt);
		int dx = cur_pt.x - drag_start_pt.x;
		int dy = drag_start_pt.y - cur_pt.y;
		int delta = (abs(dx) > abs(dy)) ? dx : dy;

		// Move 1 step every 16 pixels
		int step_diff = delta / 16;
		if (abs(delta) > 4) {
			is_dragging_value = true;
		}
		int new_idx = drag_start_val_idx + step_diff;
		if (new_idx < 0) new_idx = 0;
		if (new_idx >= (int)BUFFER_STEP_COUNT) new_idx = (int)BUFFER_STEP_COUNT - 1;

		if (new_idx != lv_slider_get_value(buffer_slider)) {
			apply_buffer_index(new_idx);
		}
	} else if (code == LV_EVENT_LONG_PRESSED_REPEAT) {
		if (!is_dragging_value) {
			int idx = (lv_slider_get_value(buffer_slider) + 1) % BUFFER_STEP_COUNT;
			apply_buffer_index(idx);
		}
	} else if (code == LV_EVENT_CLICKED) {
		if (!is_dragging_value) {
			int idx = (lv_slider_get_value(buffer_slider) + 1) % BUFFER_STEP_COUNT;
			apply_buffer_index(idx);
		}
	}
}

static void refresh_charge_glyph(void) {
	if (!charge_glyph) {
		return;
	}
	// A lightning bolt when the cable is allowed to charge, the struck-through
	// one when it is not.
	lv_image_set_src(charge_glyph, usbdac_charging_enabled() ? &icon_zap : &icon_zap_off);
}

static void refresh(void) {
	bool storage_on = usb_storage_active();

	// Switch state: ON when USB Storage is active, OFF when in default DAC mode
	if (storage_on) {
		lv_obj_add_state(mode_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(mode_switch, LV_STATE_CHECKED);
	}

	if (buffer_card) {
		lv_obj_set_style_opa(buffer_card, storage_on ? LV_OPA_40 : LV_OPA_COVER, 0);
	}

	usbdac_state_t st;
	usbdac_get_state(&st);

	// Dimmed when both modes are off, but never LV_STATE_DISABLED
	lv_obj_set_style_opa(charge_glyph, (st.active || storage_on) ? LV_OPA_COVER : LV_OPA_40, 0);
	refresh_charge_glyph();

	lv_obj_set_style_text_color(status_label, status_normal_color, 0);

	if (storage_on) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_storage_on"));
		return;
	}

	if (st.error[0]) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, st.error);
		lv_obj_set_style_text_color(status_label, lv_color_make(224, 27, 36), 0);
		return;
	}

	if (st.starting) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_turning_on_2"));
		return;
	}

	if (!st.streaming) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_waiting"));
		return;
	}

	char text[64];
	if (st.dsd) {
		snprintf(text, sizeof(text), "DSD  %d Hz", st.sample_rate);
	} else if (st.sample_rate % 1000 == 0) {
		snprintf(text, sizeof(text), "%d kHz", st.sample_rate / 1000);
	} else {
		snprintf(text, sizeof(text), "%.1f kHz", st.sample_rate / 1000.0);
	}
	lv_label_set_text(format_label, text);
	lv_label_set_text(status_label, tr("dac_playing"));
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != dacpage_screen) {
		return;
	}
	unsigned now = usbdac_serial();
	if (now == last_serial) {
		return;
	}
	last_serial = now;
	refresh();
}

// Applies whatever the switch currently says.
static void apply_mode_from_switch(void) {
	if (lv_obj_has_state(mode_switch, LV_STATE_CHECKED)) {
		// Switch ON: stop DAC and export Mass Storage (Drive D:)
		usbdac_stop();
		if (!usb_storage_export_manual()) {
			lv_obj_remove_state(mode_switch, LV_STATE_CHECKED);
			usbdac_start();
		}
	} else {
		// Switch OFF: restore storage and return to default DAC mode
		usb_storage_restore_manual();
		usbdac_start();
	}
	last_serial = (unsigned)-1;
	refresh();
}

// The whole row toggles, not just the switch: a 68x36 switch at the far edge of
// the screen is a small target, and the row gives one the width of the page
// whatever else may sit over that corner.
static void mode_row_cb(lv_event_t *e) {
	(void)e;
	if (lv_obj_has_state(mode_switch, LV_STATE_CHECKED)) {
		lv_obj_remove_state(mode_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_add_state(mode_switch, LV_STATE_CHECKED);
	}
	apply_mode_from_switch();
}

static void mode_cb(lv_event_t *e) {
	(void)e;
	// The stored switch rather than the event target, matching every other
	// toggle in the settings pages: the target of a VALUE_CHANGED can be the
	// row rather than the switch depending on how the row was built.
	apply_mode_from_switch();
}

static bool leaving;

// Leaves for real; the guard below lets the chevron through once `leaving` is
// set. Deferred by one turn of the event loop because really_leave() runs from
// inside the confirmation dialog's own handler, and navigating out from under a
// dialog that is still closing leaves the screen change undone.
static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

static void really_leave(void *user) {
	(void)user;
	usbdac_stop();
	last_serial = (unsigned)-1;
	refresh();
	leaving = true;
	lv_async_call(leave_async, NULL);
}

// Allow returning freely to home/menus without cutting off DAC mode
static bool back_guard(void) {
	return false;
}

static void charge_cb(lv_event_t *e) {
	(void)e;
	if (!usbdac_is_active()) {
		gui_notify_popup("dac_turn_dac_mode_on_first");
		return;
	}
	// The choice is written to the config, so a player set never to charge in
	// DAC mode stays that way across reboots.
	bool now_on = !usbdac_charging_enabled();
	usbdac_set_charging(now_on);
	refresh_charge_glyph();

	// The two glyphs differ by one thin diagonal stroke, which at 34 px is not
	// enough to tell a state change from a button that did nothing. Say it in
	// words as well.
	gui_notify_popup(now_on ? "dac_usb_charging_on" : "dac_usb_charging_off");
}

bool dacpage_is_holding(void) {
	return usbdac_is_active() && lv_screen_active() == dacpage_screen;
}

// Same shape as the corner buttons on the music page.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_obj_t **glyph_out) {
	lv_obj_t *button = lv_btn_create(dacpage_screen);
	settingsrow_place_corner_button(button, cfg, slot);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
	lv_obj_center(icon);
	if (glyph_out) {
		*glyph_out = icon;
	}
	return button;
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	last_serial = (unsigned)-1;
	refresh();
}

void dacpage_init(gui_config_t *cfg) {
	const bool compact = cfg->screen_width < 320;
	lv_obj_t *container = settingsrow_page(dacpage_screen, cfg, "dac");

	// The mode is a switch, not a side effect of arriving: the page can be
	// looked at, and the charging button read, without the cable being taken
	// over the moment it opens.
	mode_switch = NULL;
	lv_obj_t *mode_row = settingsrow_toggle(container, "dac_mode", &mode_switch, mode_cb);
	lv_obj_set_clickable(mode_row, true);
	lv_obj_add_event_cb(mode_row, mode_row_cb, LV_EVENT_CLICKED, NULL);

	buffer_slider = NULL;
	buffer_value = NULL;
	buffer_card = settingsrow_slider(container, "dac_buffer", (int)BUFFER_STEP_COUNT, &buffer_value, &buffer_slider,
									 buffer_slider_cb);
	if (buffer_value) {
		lv_obj_add_flag(buffer_value, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(buffer_value, buffer_value_event_cb, LV_EVENT_ALL, NULL);
	}
	int cur_ms = usbdac_get_output_ms();
	int cur_idx = buffer_ms_to_index(cur_ms);
	if (buffer_slider) {
		lv_slider_set_value(buffer_slider, cur_idx, LV_ANIM_OFF);
	}
	if (buffer_value) {
		char btext[32];
		format_buffer_label(btext, sizeof(btext), BUFFER_STEPS_MS[cur_idx]);
		lv_label_set_text(buffer_value, btext);
	}

	// Straight into the flex flow, with no alignment of its own: the container
	// settingsrow_page() builds already centres its children on the cross axis,
	// and an object positioned by alignment leaves the flow and is measured as
	// taking no space, so it would overlap the row above and steal its presses.
	// The original 200 px mark nearly fills the V1's 240 px panel and pushes the
	// status copy below the fold. Give the image a compact layout box as well as
	// a smaller draw scale so it takes only the space it visibly occupies.
	big_icon = lv_image_create(container);
	lv_image_set_src(big_icon, &icon_dac_page);
	int icon_size = compact ? 80 : 160;
	int source_side = LV_MAX((int)icon_dac_page.header.w, (int)icon_dac_page.header.h);
	lv_obj_set_size(big_icon, icon_size, icon_size);
	lv_image_set_inner_align(big_icon, LV_IMAGE_ALIGN_CENTER);
	if (source_side > 0) {
		lv_image_set_scale(big_icon, (uint32_t)(LV_SCALE_NONE * icon_size / source_side));
	}
	lv_obj_set_style_margin_top(big_icon, compact ? 4 : 20, 0);

	format_label = lv_label_create(container);
	lv_obj_set_width(format_label, lv_pct(100));
	lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
	// The theme's own text style, not the inherited default, which is black and
	// so invisible on the dark theme.
	lv_obj_add_style(format_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(format_label, compact ? &font_ui_20_bold : &font_ui_24_bold, 0);
	lv_obj_set_style_margin_top(format_label, compact ? 4 : 15, 0);
	lv_label_set_text(format_label, "");

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, compact ? &font_ui_14 : &font_ui_22, 0);
	lv_obj_set_style_margin_top(status_label, compact ? 2 : 10, 0);
	lv_label_set_text(status_label, "");
	status_normal_color = lv_obj_get_style_text_color(status_label, LV_PART_MAIN);

	charge_btn = corner_button(cfg, 0, &icon_zap, &charge_glyph);
	lv_obj_add_event_cb(charge_btn, charge_cb, LV_EVENT_CLICKED, NULL);
	// settingsrow_title reserves CORNER_BTN_SIZE on the right, but the settings
	// container spans the whole screen and is built after this button, so the
	// button must come to the front to stay reachable.
	lv_obj_move_foreground(charge_btn);
	lv_obj_set_style_opa(charge_glyph, LV_OPA_40, 0);
	refresh_charge_glyph();

	lv_obj_add_event_cb(dacpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_set_back_guard(dacpage_screen, back_guard);

	poll_timer = lv_timer_create(poll_cb, 500, NULL);
	(void)poll_timer;
}
