#include "powermenu.h"
#include "src/gui/library/browser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/shell/gui.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/device/led.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/radio.h"

// ---------------------------------------------------------------------------
// The power menu: iOS-style slide-to-confirm pills, a Sleep Timer card with
// quick preset buttons (10, 30, 60, 120, Album), slider bar and +/- buttons,
// and a round cancel button at the bottom.
// ---------------------------------------------------------------------------

#define SLIDE_WIDTH bp_pick(216, 380)
#define SLIDE_HEIGHT bp_pick(48, 70)
#define SLIDE_KNOB bp_pick(42, 60)
// How far along (as a fraction of the track) the knob has to be on release.
#define SLIDE_COMMIT_PCT 88

#define SLIDE_INSET (SLIDE_KNOB / 2 + 6)

#define CANCEL_SIZE bp_pick(48, 68)

static lv_obj_t *panel;

typedef struct {
	lv_obj_t *pill; // the visible track
	lv_obj_t *slider;
	lv_obj_t *knob_icon;
	int knob_icon_w; // real bitmap width: the two action icons are 56 px
	lv_obj_t *label;
	void (*action)(void);
} slide_t;

static slide_t slide_off;
static slide_t slide_reboot;

static lv_obj_t *sleep_card;
static lv_obj_t *sleep_status_label;
static lv_obj_t *sleep_slider;
static lv_obj_t *preset_btns[5];
static lv_obj_t *preset_labels[5];
static lv_timer_t *sleep_poll_timer;

static const int SLEEP_PRESETS[4] = {10, 30, 60, 120};

static void update_preset_highlights(int current_minutes, bool is_album) {
	for (int i = 0; i < 4; i++) {
		bool match = !is_album && (current_minutes == SLEEP_PRESETS[i]);
		lv_obj_set_style_bg_color(preset_btns[i], lv_color_white(), 0);
		lv_obj_set_style_bg_opa(preset_btns[i], match ? LV_OPA_COVER : LV_OPA_20, 0);
		lv_obj_set_style_text_color(preset_labels[i], match ? lv_color_black() : lv_color_white(), 0);
	}
	lv_obj_set_style_bg_color(preset_btns[4], lv_color_white(), 0);
	lv_obj_set_style_bg_opa(preset_btns[4], is_album ? LV_OPA_COVER : LV_OPA_20, 0);
	lv_obj_set_style_text_color(preset_labels[4], is_album ? lv_color_black() : lv_color_white(), 0);
}

static void sleep_card_refresh(void) {
	if (!panel || !sleep_status_label || !sleep_slider) {
		return;
	}
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	bool is_album = sleeptimer_stop_at_album_end();
	bool running = sleeptimer_running(kind);
	bool enabled = sleeptimer_enabled(kind);
	int mins = sleeptimer_minutes(kind);
	int rem_sec = sleeptimer_remaining(kind);

	if (is_album) {
		lv_label_set_text(sleep_status_label, "Sleep: End of album");
		lv_slider_set_value(sleep_slider, 0, LV_ANIM_OFF);
		update_preset_highlights(0, true);
	} else if (running && rem_sec > 0) {
		int rem_min = (rem_sec + 59) / 60;
		char buf[48];
		snprintf(buf, sizeof(buf), "Sleep: %d min left", rem_min);
		lv_label_set_text(sleep_status_label, buf);
		lv_slider_set_value(sleep_slider, rem_min > 120 ? 120 : rem_min, LV_ANIM_OFF);
		update_preset_highlights(mins, false);
	} else if (enabled && mins > 0) {
		char buf[48];
		snprintf(buf, sizeof(buf), "Sleep: %d min", mins);
		lv_label_set_text(sleep_status_label, buf);
		lv_slider_set_value(sleep_slider, mins > 120 ? 120 : mins, LV_ANIM_OFF);
		update_preset_highlights(mins, false);
	} else {
		lv_label_set_text(sleep_status_label, "Sleep: Off");
		lv_slider_set_value(sleep_slider, 0, LV_ANIM_OFF);
		update_preset_highlights(0, false);
	}
}

static void sleep_poll_cb(lv_timer_t *t) {
	(void)t;
	if (powermenu_is_open()) {
		sleeptimer_kind_t kind = sleeptimer_current_kind();
		if (sleeptimer_running(kind) && !sleeptimer_stop_at_album_end()) {
			sleep_card_refresh();
		}
	}
}

static void off_click_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	sleeptimer_set_stop_at_album_end(false);
	sleeptimer_set_enabled(kind, false);
	sleeptimer_set_minutes(kind, 0);
	sleep_card_refresh();
}

static void preset_click_cb(lv_event_t *e) {
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	if (idx >= 0 && idx < 4) {
		int target = SLEEP_PRESETS[idx];
		if (!sleeptimer_stop_at_album_end() && sleeptimer_enabled(kind) && sleeptimer_minutes(kind) == target) {
			// Toggle off if already matching
			sleeptimer_set_enabled(kind, false);
			sleeptimer_set_minutes(kind, 0);
		} else {
			sleeptimer_set_stop_at_album_end(false);
			sleeptimer_set_minutes(kind, target);
			sleeptimer_set_enabled(kind, true);
		}
	} else if (idx == 4) {
		// Album mode
		if (sleeptimer_stop_at_album_end()) {
			sleeptimer_set_stop_at_album_end(false);
		} else {
			sleeptimer_set_enabled(kind, false);
			sleeptimer_set_stop_at_album_end(true);
		}
	}
	sleep_card_refresh();
}

static void step_minus_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	if (sleeptimer_stop_at_album_end()) {
		sleeptimer_set_stop_at_album_end(false);
		sleep_card_refresh();
		return;
	}
	int mins = sleeptimer_enabled(kind) ? sleeptimer_minutes(kind) : 0;
	if (mins <= 5) {
		mins = 0;
	} else if (mins <= 30) {
		mins -= 5;
	} else {
		mins -= 10;
	}
	if (mins <= 0) {
		sleeptimer_set_enabled(kind, false);
		sleeptimer_set_minutes(kind, 0);
	} else {
		sleeptimer_set_minutes(kind, mins);
		sleeptimer_set_enabled(kind, true);
	}
	sleep_card_refresh();
}

static void step_plus_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	sleeptimer_set_stop_at_album_end(false);
	int mins = sleeptimer_enabled(kind) ? sleeptimer_minutes(kind) : 0;
	if (mins <= 0) {
		mins = 10;
	} else if (mins < 30) {
		mins += 5;
	} else {
		mins += 10;
	}
	if (mins > 120) {
		mins = 120;
	}
	sleeptimer_set_minutes(kind, mins);
	sleeptimer_set_enabled(kind, true);
	sleep_card_refresh();
}

static void slider_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	sleeptimer_kind_t kind = sleeptimer_current_kind();
	int val = lv_slider_get_value(sleep_slider);

	if (code == LV_EVENT_VALUE_CHANGED) {
		if (val > 0) {
			char buf[32];
			snprintf(buf, sizeof(buf), "Sleep: %d min", val);
			lv_label_set_text(sleep_status_label, buf);
		} else {
			lv_label_set_text(sleep_status_label, "Sleep: Off");
		}
		update_preset_highlights(val, false);
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		sleeptimer_set_stop_at_album_end(false);
		if (val > 0) {
			sleeptimer_set_minutes(kind, val);
			sleeptimer_set_enabled(kind, true);
		} else {
			sleeptimer_set_enabled(kind, false);
			sleeptimer_set_minutes(kind, 0);
		}
		sleep_card_refresh();
	}
}

void powermenu_refresh_labels(void) {
	if (slide_off.label) {
		lv_label_set_text(slide_off.label, tr("powermenu_slide_to_power_off"));
	}
	if (slide_reboot.label) {
		lv_label_set_text(slide_reboot.label, tr("powermenu_slide_to_restart"));
	}
	sleep_card_refresh();
}

bool powermenu_is_open(void) { return panel && !lv_obj_is_hidden(panel); }

// ---------------------------------------------------------------------------
// opening and closing
// ---------------------------------------------------------------------------

static void slide_reset(slide_t *s);

void powermenu_show(void) {
	if (!panel) {
		return;
	}

	slide_reset(&slide_off);
	slide_reset(&slide_reboot);
	sleep_card_refresh();
	if (sleep_poll_timer) {
		lv_timer_resume(sleep_poll_timer);
	}

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);
	power_hold_screen_on(true);
}

static void hide(void) {
	if (sleep_poll_timer) {
		lv_timer_pause(sleep_poll_timer);
	}
	if (panel) {
		lv_obj_set_hidden(panel, true);
	}
	power_hold_screen_on(false);
	power_notify_activity();
}

static void async_show_cb(void *unused) {
	(void)unused;
	powermenu_show();
}

void powermenu_request_show(void) { gui_post(async_show_cb, NULL); }

// Both actions flush first: the settings file and the thumbnail cache are
// written lazily, and a DAP that loses them on every shutdown would be worse
// than one that takes an extra moment to stop.
static void do_power_off(void) {
	printf("power: shutting down\n");
	power_shutdown();
}

static void do_reboot(void) {
	printf("power: rebooting\n");
	device_state_remember_flush();
	browser_state_flush();
	power_screen_off();
	// The status LED, faded out like the power-off path: the screen is already
	// dark, so this is the last thing the device says before it restarts.
	led_shutdown();
	if (!audio_prepare_poweroff()) {
		fprintf(stderr, "power: audio did not quiesce completely before reboot\n");
	}
	clock_shutdown();
	radio_store_close();
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();
	storage_release_for_shutdown();
	sync();

	// Through init, the same way down as power_shutdown(). `reboot(RB_AUTOBOOT)`
	// restarts the machine from inside this process: init's shutdown hooks never
	// run, so the daemons are not stopped and nothing is remounted read-only or
	// unmounted, apart from the card, which is released above either way.
	//
	// The syscall stays as the last resort, on the same eight-second guard, for
	// a firmware whose init never gets there.
	int rc = system("reboot");
	(void)rc;
	sleep(8);
	reboot(RB_AUTOBOOT);
	execl("/bin/sh", "sh", "-c", "reboot", (char *)NULL);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	hide();
}

// ---------------------------------------------------------------------------
// the slide-to-confirm pills
// ---------------------------------------------------------------------------

// Keeps the knob icon riding on the knob: the slider's knob is drawn by LVGL
// at a position derived from the value, so the icon just mirrors that maths.
static void slide_track_knob(slide_t *s) {
	int value = lv_slider_get_value(s->slider);

	// The slider is SLIDE_HEIGHT narrower than the pill and centred in it, so
	// the knob's centre -- which LVGL runs from slider edge to slider edge --
	// travels [SLIDE_HEIGHT/2, SLIDE_WIDTH - SLIDE_HEIGHT/2] in pill
	// coordinates: always fully inside. The icon mirrors that in the pill.
	int travel = SLIDE_WIDTH - SLIDE_HEIGHT;
	int knob_center = SLIDE_HEIGHT / 2 + (travel * value) / 100;
	lv_obj_align(s->knob_icon, LV_ALIGN_LEFT_MID, knob_center - s->knob_icon_w / 2, 0);

	// The action label fades as the knob approaches, exactly like the real thing.
	lv_obj_set_style_opa(s->label, (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER * value) / 100), 0);
}

static void slide_reset(slide_t *s) {
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	slide_track_knob(s);
}

static void slide_event_cb(lv_event_t *e) {
	slide_t *s = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_VALUE_CHANGED) {
		slide_track_knob(s);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (lv_slider_get_value(s->slider) >= SLIDE_COMMIT_PCT) {
			s->action();
		} else {
			slide_reset(s); // not far enough: snap home
		}
	}
}

// One pill: a rounded container carrying the action label, with a transparent
// slider inside it that is half a knob narrower on each side -- LVGL runs the
// knob's centre from slider edge to slider edge, so insetting the slider is
// what keeps the white disc inside the pill at both extremes.
static void make_slide(lv_obj_t *parent, slide_t *s, const lv_image_dsc_t *icon, lv_color_t icon_tint,
					   const char *text, void (*action)(void)) {
	s->action = action;

	// The visible pill: translucent white over the blur, like iOS.
	s->pill = lv_obj_create(parent);
	lv_obj_set_size(s->pill, SLIDE_WIDTH, SLIDE_HEIGHT);
	lv_obj_set_style_radius(s->pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(s->pill, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(s->pill, LV_OPA_20, 0);
	lv_obj_set_style_border_width(s->pill, 0, 0);
	lv_obj_set_style_pad_all(s->pill, 0, 0);
	lv_obj_set_scrollable(s->pill, false);

	// The localized action label, centred on the pill; it fades as the knob advances.
	s->label = lv_label_create(s->pill);
	lv_label_set_text(s->label, tr(text));
	lv_obj_set_style_text_font(s->label, &font_ui_24, 0);
	lv_obj_set_style_text_color(s->label, lv_color_white(), 0);
	lv_obj_center(s->label);

	// The working part: invisible track, white disc knob.
	s->slider = lv_slider_create(s->pill);
	lv_obj_set_size(s->slider, SLIDE_WIDTH - SLIDE_HEIGHT, SLIDE_HEIGHT);
	lv_obj_center(s->slider);
	lv_slider_set_range(s->slider, 0, 100);
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	// Zero padding, or the theme's default insets shift the knob's travel and
	// the icon riding it ends up off the disc's centre.
	lv_obj_set_style_pad_all(s->slider, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_INDICATOR);

	lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(s->slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(s->slider, (SLIDE_KNOB - SLIDE_HEIGHT) / 2, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(s->slider, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(s->slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(s->slider, lv_color_black(), LV_PART_KNOB);

	// Drag only, never tap. A plain LVGL slider jumps its value to wherever the
	// finger presses, so one tap near the far end of the track would pass the
	// commit threshold -- exactly the careless gesture the pill exists to
	// prevent. With ADV_HITTEST the slider only answers a press that begins on
	// the knob, so the value can only be dragged to the end. A tap on the rest
	// of the pill does nothing.
	lv_obj_set_adv_hittest(s->slider, true);
	// The knob stays easy to grab: the extended area applies to its hit-test,
	// not to the track.
	lv_obj_set_ext_click_area(s->slider, SLIDE_HEIGHT / 2);

	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_VALUE_CHANGED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_RELEASED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_PRESS_LOST, s);

	// The icon rides the knob, drawn in the pill's coordinate space (the knob
	// itself cannot hold children).
	s->knob_icon = lv_image_create(s->pill);
	lv_image_set_src(s->knob_icon, icon);
	lv_image_set_scale(s->knob_icon, (uint32_t)bp_pick(130, 190));
	s->knob_icon_w = (int)icon->header.w;
	lv_obj_set_style_image_recolor(s->knob_icon, icon_tint, 0);
	lv_obj_set_style_image_recolor_opa(s->knob_icon, LV_OPA_COVER, 0);
	lv_obj_set_ignore_layout(s->knob_icon, true);

	slide_track_knob(s);
}

void powermenu_init(gui_config_t *cfg) {
	panel = lv_obj_create(lv_layer_top());
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	// Nearly-opaque black: the page underneath is still just visible, but the
	// menu clearly owns the screen.
	lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_hidden(panel, true);

	// The two pills, upper section of the screen
	lv_obj_t *pills = lv_obj_create(panel);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(pills, LV_ALIGN_TOP_MID, 0, bp_pick(12, 20));
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, bp_pick(8, 14), 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	make_slide(pills, &slide_off, &icon_power_off, lv_color_make(224, 27, 36), "powermenu_slide_to_power_off", do_power_off);
	make_slide(pills, &slide_reboot, &icon_reboot, lv_color_make(0, 122, 255), "powermenu_slide_to_restart", do_reboot);

	// Sleep timer card
	sleep_card = lv_obj_create(panel);
	lv_obj_set_size(sleep_card, SLIDE_WIDTH, bp_pick(132, 180));
	lv_obj_align(sleep_card, LV_ALIGN_TOP_MID, 0, bp_pick(124, 180));
	lv_obj_set_style_radius(sleep_card, 16, 0);
	lv_obj_set_style_bg_color(sleep_card, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(sleep_card, LV_OPA_20, 0);
	lv_obj_set_style_border_width(sleep_card, 0, 0);
	lv_obj_set_style_pad_hor(sleep_card, bp_pick(8, 14), 0);
	lv_obj_set_style_pad_ver(sleep_card, bp_pick(8, 12), 0);
	lv_obj_set_scrollable(sleep_card, false);

	// Header row
	lv_obj_t *top_row = lv_obj_create(sleep_card);
	lv_obj_set_size(top_row, lv_pct(100), bp_pick(26, 34));
	lv_obj_set_style_bg_opa(top_row, 0, 0);
	lv_obj_set_style_border_width(top_row, 0, 0);
	lv_obj_set_style_pad_all(top_row, 0, 0);
	lv_obj_set_scrollable(top_row, false);
	lv_obj_align(top_row, LV_ALIGN_TOP_MID, 0, 0);

	int icon_size = bp_pick(18, 24);
	lv_obj_t *sleep_icon = lv_image_create(top_row);
	lv_image_set_src(sleep_icon, &icon_sleep_music_quick);
	lv_obj_set_size(sleep_icon, icon_size, icon_size);
	lv_image_set_inner_align(sleep_icon, LV_IMAGE_ALIGN_CENTER);
	lv_image_set_scale(sleep_icon, (uint32_t)(LV_SCALE_NONE * icon_size / 46));
	lv_obj_set_style_image_recolor(sleep_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(sleep_icon, LV_OPA_COVER, 0);
	lv_obj_align(sleep_icon, LV_ALIGN_LEFT_MID, 0, 0);

	sleep_status_label = lv_label_create(top_row);
	lv_label_set_text(sleep_status_label, "Sleep: Off");
	lv_obj_set_style_text_font(sleep_status_label, &font_ui_16, 0);
	lv_obj_set_style_text_color(sleep_status_label, lv_color_white(), 0);
	lv_obj_align(sleep_status_label, LV_ALIGN_LEFT_MID, bp_pick(26, 34), 0);

	lv_obj_t *off_btn = lv_btn_create(top_row);
	lv_obj_set_size(off_btn, bp_pick(40, 54), bp_pick(22, 28));
	lv_obj_align(off_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_radius(off_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(off_btn, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(off_btn, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(off_btn, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_set_style_pad_all(off_btn, 0, 0);
	lv_obj_add_event_cb(off_btn, off_click_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *off_lbl = lv_label_create(off_btn);
	lv_label_set_text(off_lbl, "Off");
	lv_obj_set_style_text_font(off_lbl, &font_ui_14, 0);
	lv_obj_set_style_text_color(off_lbl, lv_color_white(), 0);
	lv_obj_center(off_lbl);

	// Presets row
	lv_obj_t *presets_row = lv_obj_create(sleep_card);
	lv_obj_set_size(presets_row, lv_pct(100), bp_pick(28, 36));
	lv_obj_align(presets_row, LV_ALIGN_TOP_MID, 0, bp_pick(32, 42));
	lv_obj_set_style_bg_opa(presets_row, 0, 0);
	lv_obj_set_style_border_width(presets_row, 0, 0);
	lv_obj_set_style_pad_all(presets_row, 0, 0);
	lv_obj_set_flex_flow(presets_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(presets_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(presets_row, false);

	static const char *preset_texts[5] = {"10m", "30m", "60m", "120m", "Album"};
	int preset_widths[5] = {bp_pick(32, 56), bp_pick(32, 56), bp_pick(32, 56), bp_pick(40, 66), bp_pick(52, 80)};

	for (int i = 0; i < 5; i++) {
		preset_btns[i] = lv_btn_create(presets_row);
		lv_obj_set_size(preset_btns[i], preset_widths[i], bp_pick(26, 34));
		lv_obj_set_style_radius(preset_btns[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(preset_btns[i], lv_color_white(), 0);
		lv_obj_set_style_bg_opa(preset_btns[i], LV_OPA_20, 0);
		lv_obj_set_style_pad_all(preset_btns[i], 0, 0);
		lv_obj_add_event_cb(preset_btns[i], preset_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		preset_labels[i] = lv_label_create(preset_btns[i]);
		lv_label_set_text(preset_labels[i], preset_texts[i]);
		lv_obj_set_style_text_font(preset_labels[i], &font_ui_14, 0);
		lv_obj_set_style_text_color(preset_labels[i], lv_color_white(), 0);
		lv_obj_center(preset_labels[i]);
	}

	// Stepper & Slider row
	lv_obj_t *ctrl_row = lv_obj_create(sleep_card);
	lv_obj_set_size(ctrl_row, lv_pct(100), bp_pick(38, 50));
	lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_MID, 0, bp_pick(-2, -4));
	lv_obj_set_style_bg_opa(ctrl_row, 0, 0);
	lv_obj_set_style_border_width(ctrl_row, 0, 0);
	lv_obj_set_style_pad_all(ctrl_row, 0, 0);
	lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(ctrl_row, false);

	// Minus button
	lv_obj_t *btn_minus = lv_btn_create(ctrl_row);
	lv_obj_set_size(btn_minus, bp_pick(34, 46), bp_pick(34, 46));
	lv_obj_set_style_radius(btn_minus, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn_minus, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(btn_minus, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(btn_minus, LV_OPA_60, LV_STATE_PRESSED);
	lv_obj_set_style_pad_all(btn_minus, 0, 0);
	lv_obj_add_event_cb(btn_minus, step_minus_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl_minus = lv_label_create(btn_minus);
	lv_label_set_text(lbl_minus, "-");
	lv_obj_set_style_text_font(lbl_minus, &font_ui_20_bold, 0);
	lv_obj_set_style_text_color(lbl_minus, lv_color_white(), 0);
	lv_obj_center(lbl_minus);

	// Slider
	sleep_slider = lv_slider_create(ctrl_row);
	lv_obj_set_size(sleep_slider, bp_pick(112, 230), bp_pick(14, 20));
	lv_slider_set_range(sleep_slider, 0, 120);
	lv_slider_set_value(sleep_slider, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(sleep_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(sleep_slider, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sleep_slider, LV_OPA_20, LV_PART_MAIN);

	lv_obj_set_style_radius(sleep_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(sleep_slider, lv_color_white(), LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(sleep_slider, LV_OPA_70, LV_PART_INDICATOR);

	lv_obj_set_style_radius(sleep_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(sleep_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(sleep_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(sleep_slider, bp_pick(5, 7), LV_PART_KNOB);
	lv_obj_add_event_cb(sleep_slider, slider_event_cb, LV_EVENT_ALL, NULL);

	// Plus button
	lv_obj_t *btn_plus = lv_btn_create(ctrl_row);
	lv_obj_set_size(btn_plus, bp_pick(34, 46), bp_pick(34, 46));
	lv_obj_set_style_radius(btn_plus, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn_plus, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(btn_plus, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(btn_plus, LV_OPA_60, LV_STATE_PRESSED);
	lv_obj_set_style_pad_all(btn_plus, 0, 0);
	lv_obj_add_event_cb(btn_plus, step_plus_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *lbl_plus = lv_label_create(btn_plus);
	lv_label_set_text(lbl_plus, "+");
	lv_obj_set_style_text_font(lbl_plus, &font_ui_20_bold, 0);
	lv_obj_set_style_text_color(lbl_plus, lv_color_white(), 0);
	lv_obj_center(lbl_plus);

	// Cancel button at bottom
	lv_obj_t *cancel = lv_btn_create(panel);
	lv_obj_set_size(cancel, CANCEL_SIZE, CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, bp_pick(-8, -14));
	lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_30, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_make(224, 27, 36), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_image_set_scale(cancel_icon, (uint32_t)bp_pick(180, 256));
	lv_obj_set_style_image_recolor(cancel_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(cancel_icon, LV_OPA_COVER, 0);
	lv_obj_center(cancel_icon);

	sleep_poll_timer = lv_timer_create(sleep_poll_cb, 1000, NULL);
	lv_timer_pause(sleep_poll_timer);
}
