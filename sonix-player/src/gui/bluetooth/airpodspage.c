#include "airpodspage.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/airpods.h"
#include "src/system/core/lang.h"

lv_obj_t *airpodspage_screen;

#define POLL_MS 700

// A column is a picture, a ring, and a number. The picture's box is a fixed
// height so a short case and a tall earbud put their rings on the same line.
#define ART_HEIGHT 72
#define RING_SIZE 46
#define RING_WIDTH 4
#define COLUMN_GAP 10

// Where the ring and the bolt in it turn from green to red. The same pair of
// colours the status bar's battery uses, so a level means the same thing
// wherever it is drawn.
#define LOW_PERCENT 25
#define COLOR_GOOD lv_color_make(60, 190, 90)
#define COLOR_LOW lv_color_make(220, 60, 50)

// Three of them across 480 px, with the card's own padding either side.
#define TAB_HEIGHT 60
#define TAB_WIDTH 124

static lv_obj_t *card;
static lv_obj_t *columns[3];
static lv_obj_t *column_art[3];
static lv_obj_t *column_ring[3];
static lv_obj_t *column_bolt[3];
static lv_obj_t *column_value[3];
static lv_obj_t *waiting_label;

// The three modes that do something. There is no "off" tab: the headphones
// answer the off command with nothing at all -- on a pair whose owner has not
// enabled Off in the noise-control cycle, Apple's own firmware refuses it -- so
// a tab for it would be a button that does nothing.
#define TAB_COUNT 3
static lv_obj_t *tabbar;
static lv_obj_t *tabs[TAB_COUNT];
static lv_obj_t *tab_icons[TAB_COUNT];
static const airpods_noise_t TAB_MODES[TAB_COUNT] = {AIRPODS_NOISE_CANCELLATION, AIRPODS_NOISE_TRANSPARENCY,
													 AIRPODS_NOISE_ADAPTIVE};

// The press-and-hold page: which noise modes the long press steps through.
// Apple's own screen is the same shape -- a list of the modes with a switch
// each, and at least two of them have to stay on.
//
// The three modes the headphones themselves offer, and nothing else. There is
// no "off" row: these headphones do not put the off mode in that cycle. And
// because two of the three cannot be arranged in more than one way, the whole
// page belongs only to the models that have all three -- see press_hold in
// airpods.c.
#define HOLD_COUNT 3
static lv_obj_t *hold_screen;
static lv_obj_t *hold_row;	// the row on this page that opens it
static lv_obj_t *hold_cards[HOLD_COUNT];
static lv_obj_t *hold_switches[HOLD_COUNT];
static const uint8_t HOLD_BITS[HOLD_COUNT] = {AIRPODS_CYCLE_CANCELLATION, AIRPODS_CYCLE_TRANSPARENCY,
											  AIRPODS_CYCLE_ADAPTIVE};

static lv_timer_t *poll_timer;
static uint32_t drawn_serial = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Which picture
// ---------------------------------------------------------------------------

const lv_image_dsc_t *airpodspage_model_icon(airpods_model_t model) {
	switch (model) {
	case AIRPODS_MODEL_GEN3:
		return &icon_airpods_gen3_hero;
	case AIRPODS_MODEL_GEN4:
		return &icon_airpods_gen4_hero;
	case AIRPODS_MODEL_PRO:
		return &icon_airpods_pro_hero;
	case AIRPODS_MODEL_MAX:
		return &icon_airpods_max_hero;
	case AIRPODS_MODEL_GEN1:
	case AIRPODS_MODEL_GEN2:
	case AIRPODS_MODEL_UNKNOWN:
	default:
		// The first two generations look the same from the outside, and an
		// unknown model gets the plain pair rather than nothing at all.
		return &icon_airpods_hero;
	}
}

static const lv_image_dsc_t *bud_icon(airpods_model_t model, bool left) {
	switch (model) {
	case AIRPODS_MODEL_GEN3:
		return left ? &icon_airpod_gen3_left : &icon_airpod_gen3_right;
	case AIRPODS_MODEL_GEN4:
		return left ? &icon_airpod_gen4_left : &icon_airpod_gen4_right;
	case AIRPODS_MODEL_PRO:
		return left ? &icon_airpod_pro_left : &icon_airpod_pro_right;
	default:
		return left ? &icon_airpod_left : &icon_airpod_right;
	}
}

// The first two generations were sold with two different cases and nothing the
// headphones say tells the two apart, so the plain one is drawn for the first
// generation -- which never had another -- and the wireless one for the
// second, which is the case it is remembered by.
static const lv_image_dsc_t *case_icon(airpods_model_t model) {
	switch (model) {
	case AIRPODS_MODEL_GEN3:
		return &icon_airpods_gen3_case;
	case AIRPODS_MODEL_GEN4:
		return &icon_airpods_gen4_case;
	case AIRPODS_MODEL_PRO:
		return &icon_airpods_pro_case;
	case AIRPODS_MODEL_GEN2:
		return &icon_airpods_case_wireless;
	default:
		return &icon_airpods_case;
	}
}

// ---------------------------------------------------------------------------
// Drawing a column
// ---------------------------------------------------------------------------

static void set_column(int index, const lv_image_dsc_t *art, const airpods_level_t *level) {
	lv_obj_remove_flag(columns[index], LV_OBJ_FLAG_HIDDEN);
	lv_image_set_src(column_art[index], art);

	// A part that is not reporting -- an earbud sitting in the case, a case
	// that has not been opened -- is drawn faint with a dash instead of a
	// number, rather than left out: the space it occupies is what says the
	// headphones have three parts and this is the one with nothing to say.
	bool reporting = level->known && level->charge != AIRPODS_CHARGE_ABSENT;

	lv_obj_set_style_image_opa(column_art[index], reporting ? LV_OPA_COVER : LV_OPA_30, 0);

	if (!reporting) {
		lv_arc_set_value(column_ring[index], 0);
		lv_obj_set_style_arc_opa(column_ring[index], LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_image_opa(column_bolt[index], LV_OPA_30, 0);
		lv_obj_set_style_image_recolor(column_bolt[index], theme()->text_secondary, 0);
		lv_label_set_text(column_value[index], "--");
		lv_obj_set_style_text_color(column_value[index], theme()->text_secondary, 0);
		return;
	}

	lv_color_t colour = level->percent <= LOW_PERCENT ? COLOR_LOW : COLOR_GOOD;

	lv_arc_set_value(column_ring[index], level->percent);
	lv_obj_set_style_arc_opa(column_ring[index], LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(column_ring[index], colour, LV_PART_INDICATOR);
	lv_obj_set_style_image_opa(column_bolt[index], LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(column_bolt[index], colour, 0);

	char text[16];
	snprintf(text, sizeof(text), "%d%%", level->percent);
	lv_label_set_text(column_value[index], text);

	// On the charger the number goes to the accent colour. The ring keeps
	// saying how full it is; this says which way it is going.
	lv_obj_set_style_text_color(column_value[index],
								level->charge == AIRPODS_CHARGE_CHARGING ? theme()->accent : theme()->text_primary, 0);
}

static void hide_column(int index) { lv_obj_add_flag(columns[index], LV_OBJ_FLAG_HIDDEN); }

// ---------------------------------------------------------------------------
// The noise control
// ---------------------------------------------------------------------------

static void tab_clicked_cb(lv_event_t *e) {
	airpods_set_noise((airpods_noise_t)(intptr_t)lv_event_get_user_data(e));
}

static void set_tab_active(int index, bool active) {
	lv_obj_set_style_bg_color(tabs[index], active ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_image_recolor(tab_icons[index], active ? lv_color_white() : theme()->text_primary, 0);
}

static void redraw_tabbar(const airpods_state_t *state) {
	if (!state->has_noise_control) {
		lv_obj_add_flag(tabbar, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_remove_flag(tabbar, LV_OBJ_FLAG_HIDDEN);

	for (int i = 0; i < TAB_COUNT; i++) {
		// The adaptive mode is the one not every model with noise control has,
		// so its tab comes and goes while the other two are always there.
		bool offered = TAB_MODES[i] != AIRPODS_NOISE_ADAPTIVE || state->has_adaptive;
		if (offered) {
			lv_obj_remove_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
		}
		set_tab_active(i, offered && state->noise == TAB_MODES[i]);
	}
}

// ---------------------------------------------------------------------------
// The press-and-hold page
// ---------------------------------------------------------------------------

static void redraw_hold(const airpods_state_t *state) {
	for (int i = 0; i < HOLD_COUNT; i++) {
		if (state->noise_cycle & HOLD_BITS[i]) {
			lv_obj_add_state(hold_switches[i], LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(hold_switches[i], LV_STATE_CHECKED);
		}
	}
}

static void hold_toggled_cb(lv_event_t *e) {
	lv_obj_t *target = lv_event_get_target(e);

	airpods_state_t state;
	airpods_get(&state);

	uint8_t wanted = state.noise_cycle;
	for (int i = 0; i < HOLD_COUNT; i++) {
		if (hold_switches[i] != target) {
			continue;
		}
		if (lv_obj_has_state(target, LV_STATE_CHECKED)) {
			wanted |= HOLD_BITS[i];
		} else {
			wanted &= (uint8_t)~HOLD_BITS[i];
		}
		break;
	}

	// Refused rather than sent when it would leave fewer than two: the
	// headphones would keep what they had and the switch would be lying. The
	// redraw below puts it back where the state says it is.
	airpods_set_noise_cycle(wanted);
	airpods_get(&state);
	redraw_hold(&state);
	drawn_serial = state.serial;
}

static void open_hold_cb(lv_event_t *e) {
	(void)e;
	switch_screen(hold_screen);
}

// ---------------------------------------------------------------------------

static void redraw(const airpods_state_t *state) {
	lv_label_set_text(settingsrow_page_title(airpodspage_screen), state->name[0] ? state->name : tr("airpods"));

	redraw_tabbar(state);

	// The row into the press-and-hold page, on the models with three modes to
	// arrange. On the first Pro and the Max there are two and their order is
	// the only one there is; on the plain AirPods the long press is Siri.
	if (state->has_press_hold) {
		lv_obj_remove_flag(hold_row, LV_OBJ_FLAG_HIDDEN);
		redraw_hold(state);
	} else {
		lv_obj_add_flag(hold_row, LV_OBJ_FLAG_HIDDEN);
	}

	if (!state->have_battery) {
		for (int i = 0; i < 3; i++) {
			hide_column(i);
		}
		lv_obj_remove_flag(waiting_label, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_add_flag(waiting_label, LV_OBJ_FLAG_HIDDEN);

	// The Max are one headset with one battery and no case, so they get one
	// column in the middle instead of three. Reporting a single battery at all
	// is what says so -- a pair never reports one.
	if (state->single.known) {
		hide_column(0);
		set_column(1, &icon_airpods_max_big, &state->single);
		hide_column(2);
		return;
	}

	set_column(0, bud_icon(state->model, true), &state->left);
	set_column(1, bud_icon(state->model, false), &state->right);
	set_column(2, case_icon(state->model), &state->charging_case);
}

static void refresh(bool force) {
	airpods_state_t state;
	airpods_get(&state);

	if (!force && state.serial == drawn_serial) {
		return;
	}
	drawn_serial = state.serial;
	redraw(&state);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh(false);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh(true);
	lv_timer_resume(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
}

// ---------------------------------------------------------------------------
// Building it
// ---------------------------------------------------------------------------

// A container with no chrome of its own, for the boxes that only exist to hold
// something in the middle of a space.
static lv_obj_t *plain_box(lv_obj_t *parent) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_shadow_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
	return box;
}

static void build_column(int index) {
	columns[index] = plain_box(card);
	lv_obj_set_size(columns[index], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(columns[index], COLUMN_GAP, 0);
	lv_obj_set_flex_flow(columns[index], LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(columns[index], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *art_box = plain_box(columns[index]);
	lv_obj_set_size(art_box, LV_SIZE_CONTENT, ART_HEIGHT);
	column_art[index] = lv_image_create(art_box);
	lv_obj_add_style(column_art[index], &theme_style_icon, 0);
	lv_obj_center(column_art[index]);

	// The ring. A whole circle of background with the level drawn over it from
	// the top, no knob and nothing to drag: it is a reading, not a control.
	column_ring[index] = lv_arc_create(columns[index]);
	lv_obj_set_size(column_ring[index], RING_SIZE, RING_SIZE);
	lv_arc_set_rotation(column_ring[index], 270);
	lv_arc_set_bg_angles(column_ring[index], 0, 360);
	lv_arc_set_range(column_ring[index], 0, 100);
	lv_arc_set_value(column_ring[index], 0);
	lv_obj_remove_style(column_ring[index], NULL, LV_PART_KNOB);
	lv_obj_remove_flag(column_ring[index], LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(column_ring[index], LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_style_arc_width(column_ring[index], RING_WIDTH, 0);
	lv_obj_set_style_arc_width(column_ring[index], RING_WIDTH, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(column_ring[index], theme()->surface_pressed, 0);
	lv_obj_set_style_arc_opa(column_ring[index], LV_OPA_COVER, 0);
	lv_obj_set_style_bg_opa(column_ring[index], LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_all(column_ring[index], 0, 0);

	column_bolt[index] = lv_image_create(column_ring[index]);
	lv_image_set_src(column_bolt[index], &icon_airpods_charge);
	// Recoloured by hand rather than through the icon style, because it follows
	// the ring's green or red instead of the theme's text colour -- but the
	// recolouring still has to be switched on for the colour to apply at all.
	lv_obj_set_style_image_recolor_opa(column_bolt[index], LV_OPA_COVER, 0);
	lv_obj_center(column_bolt[index]);

	column_value[index] = lv_label_create(columns[index]);
	lv_obj_add_style(column_value[index], &theme_style_text, 0);
	lv_obj_set_style_text_font(column_value[index], &font_ui_18, 0);
	lv_label_set_text(column_value[index], "--");
}

static void build_tabbar(lv_obj_t *container) {
	static const lv_image_dsc_t *const TAB_ICONS[TAB_COUNT] = {&icon_airpods_anc, &icon_airpods_transparency,
															   &icon_airpods_adaptive};

	tabbar = lv_obj_create(container);
	lv_obj_set_width(tabbar, lv_pct(100));
	lv_obj_set_height(tabbar, LV_SIZE_CONTENT);
	lv_obj_add_style(tabbar, &theme_style_card, 0);
	lv_obj_set_style_radius(tabbar, 12, 0);
	lv_obj_set_style_border_width(tabbar, 0, 0);
	lv_obj_set_style_shadow_width(tabbar, 0, 0);
	lv_obj_set_style_pad_all(tabbar, 10, 0);
	lv_obj_remove_flag(tabbar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(tabbar, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(tabbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tabbar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < TAB_COUNT; i++) {
		tabs[i] = lv_btn_create(tabbar);
		lv_obj_set_size(tabs[i], TAB_WIDTH, TAB_HEIGHT);
		lv_obj_set_style_radius(tabs[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(tabs[i], 0, 0);
		lv_obj_set_style_border_width(tabs[i], 0, 0);
		lv_obj_add_event_cb(tabs[i], tab_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)TAB_MODES[i]);

		tab_icons[i] = lv_image_create(tabs[i]);
		lv_image_set_src(tab_icons[i], TAB_ICONS[i]);
		lv_obj_center(tab_icons[i]);

		set_tab_active(i, false);
	}
}

static void build_hold_page(gui_config_t *cfg) {
	hold_screen = lv_obj_create(NULL);
	lv_obj_add_style(hold_screen, &theme_style_screen, 0);

	lv_obj_t *container = settingsrow_page(hold_screen, cfg, "airpods_press_and_hold");

	lv_obj_t *hint = lv_label_create(container);
	lv_label_set_text(hint, tr("airpods_stem_note"));
	lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(hint, lv_pct(100));
	lv_obj_add_style(hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hint, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(hint, 4, 0);
	lv_obj_set_style_margin_bottom(hint, 6, 0);

	// Written out rather than looped over a table of names: the tool that keeps
	// the language files honest follows literals into the function that
	// translates them, and a name reaching it through an array is a name that
	// silently never gets a line in any of them. Same order as HOLD_BITS.
	hold_cards[0] = settingsrow_toggle(container, "airpods_noise_cancellation", &hold_switches[0], hold_toggled_cb);
	hold_cards[1] = settingsrow_toggle(container, "airpods_transparency", &hold_switches[1], hold_toggled_cb);
	hold_cards[2] = settingsrow_toggle(container, "airpods_adaptive", &hold_switches[2], hold_toggled_cb);

	switcher_attach_back_gesture(hold_screen);
}

void airpodspage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(airpodspage_screen, cfg, "airpods");

	card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_ver(card, 22, 0);
	lv_obj_set_style_pad_hor(card, 10, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < 3; i++) {
		build_column(i);
	}

	// Between the session opening and the first notification there is a second
	// or so with nothing to show, and an empty card in its place would read as
	// a page that does not work.
	waiting_label = lv_label_create(card);
	lv_label_set_text(waiting_label, tr("airpods_battery_reading"));
	lv_obj_add_style(waiting_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(waiting_label, &font_ui_22, 0);
	lv_obj_add_flag(waiting_label, LV_OBJ_FLAG_HIDDEN);

	build_tabbar(container);
	lv_obj_add_flag(tabbar, LV_OBJ_FLAG_HIDDEN);

	build_hold_page(cfg);
	hold_row = settingsrow_add(container, "airpods_press_and_hold", NULL, open_hold_cb, NULL);
	lv_obj_add_flag(hold_row, LV_OBJ_FLAG_HIDDEN);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(airpodspage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(airpodspage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	// The press-and-hold page follows the same session, so it keeps the same
	// tick: entering it from here would otherwise leave it drawing whatever the
	// state was at the moment it was built.
	lv_obj_add_event_cb(hold_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(hold_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(airpodspage_screen);
}
