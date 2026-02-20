#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#if __has_include("bg_image.h")
#include "bg_image.h"
#define HAVE_BG_IMAGE 1
#else
#define HAVE_BG_IMAGE 0
#endif

#if __has_include("midi_eq_data.h")
#include "midi_eq_data.h"
#define HAVE_MIDI_EQ_DATA 1
#else
#define HAVE_MIDI_EQ_DATA 0
#endif

#define APP_TICK_MS 2
#define EQ_RENDER_MS 5
#define EQ_BAR_COUNT 12
#define EQ_MAX_LEVEL 100
#define EQ_BASE_LEVEL (EQ_MAX_LEVEL / 3)
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define TITLE_TEXT "my midi eq"
#define BG_IMAGE_OPA 190
#define BG_VEIL_OPA 90
#define SCREEN_DIM_OPA 100
#define MODE_BUTTON_DEBOUNCE_MS 180
#define PROGRESS_LINE_H 6
#define PEAK_CAP_H 3
#define EQ_VISUAL_HEIGHT_PCT 72
#define EQ_FLASH_ATTACK_GAIN 390
#define EQ_FLASH_RELEASE_GAIN 240
#define EQ_FLASH_BOOST_PCT 132
#define EQ_CAP_FALL_PER_SEC 44
#define TITLE_SLIDE_RANGE_PX 18
#define TITLE_SLIDE_TIME_MS 1900
#define TITLE_MARQUEE_GAP "        "
#define PIANO_WHITE_KEY_COUNT 14
#define PIANO_BLACK_KEY_COUNT 10
#define PIANO_HIT_DECAY_PER_SEC 260U
#define PIANO_HIT_MIN 130U
#define PIANO_HIT_MAX 255U

#if HAVE_MIDI_EQ_DATA && (MIDI_EQ_BAR_COUNT != EQ_BAR_COUNT)
#error "MIDI_EQ_BAR_COUNT must match EQ_BAR_COUNT"
#endif

#if HAVE_MIDI_EQ_DATA
#define EQ_ATTACK_GAIN 320
#define EQ_RELEASE_GAIN 170
#define EQ_MIN_UP_STEP (FP_ONE / 2)
#define EQ_MAX_UP_STEP (FP_ONE * 8)
#define EQ_MIN_DOWN_STEP (FP_ONE / 3)
#define EQ_MAX_DOWN_STEP (FP_ONE * 3)
#define EQ_PEAK_BOOST_PCT 125
#else
#define EQ_ATTACK_GAIN 212
#define EQ_RELEASE_GAIN 62
#define EQ_MIN_UP_STEP (FP_ONE / 3)
#define EQ_MAX_UP_STEP (FP_ONE * 5)
#define EQ_MIN_DOWN_STEP (FP_ONE / 8)
#define EQ_MAX_DOWN_STEP FP_ONE
#define EQ_PEAK_BOOST_PCT 150
#endif

#if HAVE_MIDI_EQ_DATA && defined(MIDI_EQ_TITLE)
#define EQ_UI_TITLE MIDI_EQ_TITLE
#else
#define EQ_UI_TITLE TITLE_TEXT
#endif

#if DT_HAS_CHOSEN(zephyr_display)
static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#else
static const struct device *display_dev;
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_STATUS_LED 1
#else
#define HAVE_STATUS_LED 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
static const struct gpio_dt_spec mode_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#define HAVE_MODE_BUTTON 1
#else
#define HAVE_MODE_BUTTON 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
static const struct gpio_dt_spec scene_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
#define HAVE_SCENE_BUTTON 1
#else
#define HAVE_SCENE_BUTTON 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw2), okay)
static const struct gpio_dt_spec pause_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
#define HAVE_PAUSE_BUTTON 1
#else
#define HAVE_PAUSE_BUTTON 0
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw3), okay)
static const struct gpio_dt_spec piano_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);
#define HAVE_PIANO_BUTTON 1
#else
#define HAVE_PIANO_BUTTON 0
#endif

static lv_obj_t *eq_slot[EQ_BAR_COUNT];
static lv_obj_t *eq_fill[EQ_BAR_COUNT];
static lv_obj_t *eq_flash[EQ_BAR_COUNT];
static lv_obj_t *eq_cap[EQ_BAR_COUNT];
static lv_obj_t *title_glow_label;
static lv_obj_t *title_label;
static lv_obj_t *title_line_obj;
static lv_obj_t *beat_pulse_layer;
static lv_obj_t *progress_track_obj;
static lv_obj_t *progress_fill_obj;
static lv_obj_t *piano_panel_obj;
static lv_obj_t *piano_white_keys[PIANO_WHITE_KEY_COUNT];
static lv_obj_t *piano_black_keys[PIANO_BLACK_KEY_COUNT];
static int32_t eq_level_fp[EQ_BAR_COUNT];
static int32_t eq_energy_fp[EQ_BAR_COUNT];
static int32_t eq_flash_fp[EQ_BAR_COUNT];
static int32_t eq_cap_fp[EQ_BAR_COUNT];
static lv_coord_t disp_w = 320;
static lv_coord_t disp_h = 240;
static lv_coord_t slot_h_px;
static lv_coord_t progress_track_w;
static lv_coord_t piano_panel_y_shown;
static lv_coord_t piano_panel_y_hidden;
static bool status_led_ready;
static bool show_elapsed_time;
static bool piano_visible;
static bool piano_mode_active;
static uint8_t piano_white_hit[PIANO_WHITE_KEY_COUNT];
static uint8_t piano_black_hit[PIANO_BLACK_KEY_COUNT];
static uint8_t piano_black_used_count;
static volatile bool mode_toggle_pending;
static volatile bool scene_cycle_pending;
static volatile bool pause_toggle_pending;
static volatile bool piano_toggle_pending;
static uint32_t prng_state = 0xA5A5F00DU;
static uint32_t midi_frame_cursor;
static uint32_t midi_frame_elapsed_ms;
static uint32_t midi_song_elapsed_ms;
static uint32_t last_displayed_second = UINT32_MAX;
static uint32_t playback_start_ms;
static int64_t mode_button_last_press_ms;
static int64_t scene_button_last_press_ms;
static int64_t pause_button_last_press_ms;
static int64_t piano_button_last_press_ms;
static uint8_t active_scene_idx;
static bool playback_paused;
static uint32_t playback_pause_started_ms;
static uint32_t playback_pause_accum_ms;
static char header_text_buf[24];
static char marquee_text_buf[192];

static const uint8_t midi_bar_gain_pct[EQ_BAR_COUNT] = {
	142U, 130U, 116U, 102U, 84U, 70U, 70U, 84U, 102U, 116U, 130U, 142U
};

#if HAVE_MODE_BUTTON
static struct gpio_callback mode_button_cb_data;
#endif

#if HAVE_SCENE_BUTTON
static struct gpio_callback scene_button_cb_data;
#endif

#if HAVE_PAUSE_BUTTON
static struct gpio_callback pause_button_cb_data;
#endif

#if HAVE_PIANO_BUTTON
static struct gpio_callback piano_button_cb_data;
#endif

static uint32_t playback_elapsed_ms(void);
static void ground_all_bars(void);
static void piano_reset_state(void);
static uint32_t prng_next(void);

enum scene_id {
	SCENE_WHITE = 0,
	SCENE_NEON_NIGHT,
	SCENE_BOTANIC_POP,
	SCENE_SUNSET_HEAT,
	SCENE_AQUA_CRIMSON,
	SCENE_RAINBOW_STRIPE,
	SCENE_COUNT
};

struct scene_cfg {
	const char *name;
	uint16_t attack_gain;
	uint16_t release_gain;
	uint16_t peak_boost_pct;
	uint16_t flash_attack_gain;
	uint16_t flash_release_gain;
	uint16_t flash_boost_pct;
	uint16_t cap_fall_per_sec;
	uint8_t pulse_threshold;
	uint8_t pulse_gain;
	uint8_t pulse_max_opa;
	uint8_t pulse_rise_per_ms;
	uint8_t pulse_fall_per_ms;
	uint32_t pulse_color_hex;
};

static const struct scene_cfg scenes[SCENE_COUNT] = {
	[SCENE_WHITE] = {
		.name = "white",
		.attack_gain = 330,
		.release_gain = 175,
		.peak_boost_pct = 130,
		.flash_attack_gain = 380,
		.flash_release_gain = 235,
		.flash_boost_pct = 128,
		.cap_fall_per_sec = 42,
		.pulse_threshold = 20,
		.pulse_gain = 3,
		.pulse_max_opa = 84,
		.pulse_rise_per_ms = 4,
		.pulse_fall_per_ms = 1,
		.pulse_color_hex = 0xFFFFFFU
	},
	[SCENE_NEON_NIGHT] = {
		.name = "neon-night",
		.attack_gain = 360,
		.release_gain = 185,
		.peak_boost_pct = 140,
		.flash_attack_gain = 430,
		.flash_release_gain = 260,
		.flash_boost_pct = 145,
		.cap_fall_per_sec = 52,
		.pulse_threshold = 19,
		.pulse_gain = 4,
		.pulse_max_opa = 100,
		.pulse_rise_per_ms = 5,
		.pulse_fall_per_ms = 2,
		.pulse_color_hex = 0xFF1CC0U
	},
	[SCENE_BOTANIC_POP] = {
		.name = "botanic-pop",
		.attack_gain = 295,
		.release_gain = 150,
		.peak_boost_pct = 120,
		.flash_attack_gain = 320,
		.flash_release_gain = 210,
		.flash_boost_pct = 110,
		.cap_fall_per_sec = 34,
		.pulse_threshold = 22,
		.pulse_gain = 2,
		.pulse_max_opa = 52,
		.pulse_rise_per_ms = 3,
		.pulse_fall_per_ms = 1,
		.pulse_color_hex = 0x7FA834U
	},
	[SCENE_SUNSET_HEAT] = {
		.name = "sunset-heat",
		.attack_gain = 345,
		.release_gain = 178,
		.peak_boost_pct = 136,
		.flash_attack_gain = 390,
		.flash_release_gain = 250,
		.flash_boost_pct = 138,
		.cap_fall_per_sec = 46,
		.pulse_threshold = 18,
		.pulse_gain = 4,
		.pulse_max_opa = 94,
		.pulse_rise_per_ms = 5,
		.pulse_fall_per_ms = 2,
		.pulse_color_hex = 0xFF8C00U
	},
	[SCENE_AQUA_CRIMSON] = {
		.name = "aqua-crimson",
		.attack_gain = 338,
		.release_gain = 172,
		.peak_boost_pct = 134,
		.flash_attack_gain = 382,
		.flash_release_gain = 244,
		.flash_boost_pct = 136,
		.cap_fall_per_sec = 44,
		.pulse_threshold = 18,
		.pulse_gain = 4,
		.pulse_max_opa = 90,
		.pulse_rise_per_ms = 5,
		.pulse_fall_per_ms = 2,
		.pulse_color_hex = 0x00B8C7U
	},
	[SCENE_RAINBOW_STRIPE] = {
		.name = "rainbow-stripe",
		.attack_gain = 362,
		.release_gain = 186,
		.peak_boost_pct = 142,
		.flash_attack_gain = 410,
		.flash_release_gain = 258,
		.flash_boost_pct = 145,
		.cap_fall_per_sec = 50,
		.pulse_threshold = 17,
		.pulse_gain = 4,
		.pulse_max_opa = 96,
		.pulse_rise_per_ms = 5,
		.pulse_fall_per_ms = 2,
		.pulse_color_hex = 0xFF00B4U
	}
};

static uint32_t blend_hex(uint32_t a, uint32_t b, uint8_t t)
{
	uint32_t inv = 255U - t;
	uint32_t ar = (a >> 16) & 0xFFU;
	uint32_t ag = (a >> 8) & 0xFFU;
	uint32_t ab = a & 0xFFU;
	uint32_t br = (b >> 16) & 0xFFU;
	uint32_t bg = (b >> 8) & 0xFFU;
	uint32_t bb = b & 0xFFU;
	uint32_t rr = ((ar * inv) + (br * t)) / 255U;
	uint32_t rg = ((ag * inv) + (bg * t)) / 255U;
	uint32_t rb = ((ab * inv) + (bb * t)) / 255U;

	return (rr << 16) | (rg << 8) | rb;
}

static uint32_t darken_hex(uint32_t c, uint8_t amount)
{
	uint32_t r = (c >> 16) & 0xFFU;
	uint32_t g = (c >> 8) & 0xFFU;
	uint32_t b = c & 0xFFU;
	uint32_t k = 255U - amount;

	r = (r * k) / 255U;
	g = (g * k) / 255U;
	b = (b * k) / 255U;

	return (r << 16) | (g << 8) | b;
}

static const struct scene_cfg *active_scene(void)
{
	return &scenes[active_scene_idx % SCENE_COUNT];
}

static uint32_t palette_hex(uint8_t scene_idx, uint8_t t)
{
	static const uint32_t white[] = {
		0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU, 0xFFFFFFU
	};
	static const uint32_t neon_night[] = {
		0x1C3AFFU, 0x4B00FFU, 0x7E00FFU, 0xB131FAU, 0xE000FFU, 0xFF1CC0U, 0xFF4CD6U
	};
	static const uint32_t botanic_pop[] = {
		0xB8FF2EU, 0x6AFF1FU, 0x00D948U, 0x00A76AU, 0xAA00D4U, 0xE0009BU, 0xFF2BC2U
	};
	static const uint32_t sunset_heat[] = {
		0xFFF100U, 0xFFC000U, 0xFF8500U, 0xFF3A00U, 0xFF0049U, 0xA00056U, 0x5D003DU
	};
	static const uint32_t aqua_crimson[] = {
		0x64D6BCU, 0x00B8C7U, 0x17364FU, 0x0D1A2FU, 0x411E3AU, 0xBD0927U, 0x500A1FU
	};
	static const uint32_t rainbow_stripe[] = {
		0xFF00B4U, 0xFFB000U, 0xC8FF00U, 0x19FF00U, 0x1EE4D8U, 0x1DA4E6U, 0xA300FFU
	};
	const uint32_t *stops = white;
	uint16_t pos = (uint16_t)t * 6U;
	uint8_t idx = (uint8_t)(pos / 255U);
	uint8_t frac = (uint8_t)(pos % 255U);

	switch (scene_idx % SCENE_COUNT) {
	case SCENE_NEON_NIGHT:
		stops = neon_night;
		break;
	case SCENE_BOTANIC_POP:
		stops = botanic_pop;
		break;
	case SCENE_SUNSET_HEAT:
		stops = sunset_heat;
		break;
	case SCENE_AQUA_CRIMSON:
		stops = aqua_crimson;
		break;
	case SCENE_RAINBOW_STRIPE:
		stops = rainbow_stripe;
		break;
	case SCENE_WHITE:
	default:
		stops = white;
		break;
	}

	if (idx >= 6U) {
		return stops[6];
	}

	return blend_hex(stops[idx], stops[idx + 1U], frac);
}

static void create_background_layer(lv_obj_t *screen, lv_coord_t screen_w, lv_coord_t screen_h)
{
	lv_obj_t *bg = lv_obj_create(screen);
	lv_obj_set_size(bg, screen_w, screen_h);
	lv_obj_set_pos(bg, 0, 0);
	lv_obj_set_style_border_width(bg, 0, 0);
	lv_obj_set_style_radius(bg, 0, 0);
	lv_obj_set_style_pad_all(bg, 0, 0);
	lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bg, lv_color_hex(0x050312), 0);
	lv_obj_set_style_bg_grad_color(bg, lv_color_hex(0x140726), 0);
	lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, 0);
	lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

#if HAVE_BG_IMAGE
	lv_obj_t *img = lv_image_create(bg);
	lv_image_set_src(img, &bg_image);
	lv_image_set_pivot(img, bg_image.header.w / 2, bg_image.header.h / 2);
	lv_image_set_rotation(img, 900);
	lv_obj_center(img);
	lv_obj_set_style_opa(img, BG_IMAGE_OPA, 0);
#else
	const uint32_t colors[] = {
		0x002296U, 0x82008FU, 0xC0007AU, 0xEA0C5FU, 0xFF5341U, 0xFF8820U, 0xF6BA00U
	};
	const lv_align_t aligns[] = {
		LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_MID, LV_ALIGN_TOP_RIGHT,
		LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_MID, LV_ALIGN_BOTTOM_RIGHT, LV_ALIGN_CENTER
	};
	const int16_t offs_x[] = { -36, 0, 36, -24, 0, 24, 0 };
	const int16_t offs_y[] = { -20, -8, -12, 18, 20, 16, 0 };

	for (uint8_t i = 0; i < 7; i++) {
		lv_obj_t *blob = lv_obj_create(bg);
		lv_obj_set_size(blob, (lv_coord_t)(screen_w * 2 / 3), (lv_coord_t)(screen_h / 2));
		lv_obj_align(blob, aligns[i], offs_x[i], offs_y[i]);
		lv_obj_set_style_border_width(blob, 0, 0);
		lv_obj_set_style_radius(blob, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(blob, 28, 0);
		lv_obj_set_style_bg_color(blob, lv_color_hex(colors[i]), 0);
		lv_obj_clear_flag(blob, LV_OBJ_FLAG_SCROLLABLE);
	}
#endif

	/* Keep image/fallback very subtle so bars remain dominant. */
	lv_obj_t *veil = lv_obj_create(bg);
	lv_obj_set_size(veil, screen_w, screen_h);
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_bg_opa(veil, BG_VEIL_OPA, 0);
	lv_obj_set_style_bg_color(veil, lv_color_hex(0x05060E), 0);
	lv_obj_clear_flag(veil, LV_OBJ_FLAG_SCROLLABLE);
}

static void update_progress_line(void)
{
	if (progress_fill_obj == NULL || progress_track_obj == NULL) {
		return;
	}

#if HAVE_MIDI_EQ_DATA
	{
		const uint32_t total_song_ms = MIDI_EQ_FRAME_COUNT * MIDI_EQ_FRAME_MS;
		uint32_t elapsed_ms = playback_elapsed_ms();
		lv_coord_t width = 1;

		if ((total_song_ms > 0U) && (progress_track_w > 0)) {
			uint64_t scaled = (uint64_t)elapsed_ms * (uint64_t)progress_track_w;

			width = (lv_coord_t)(scaled / total_song_ms);
			if (width < 1) {
				width = 1;
			}
			if (width > progress_track_w) {
				width = progress_track_w;
			}
		}
		lv_obj_set_width(progress_fill_obj, width);
	}
#else
	lv_obj_set_width(progress_fill_obj, progress_track_w);
#endif
}

static void apply_scene_styles(void)
{
	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		uint8_t t = (uint8_t)((i * 255U) / (EQ_BAR_COUNT - 1U));
		uint32_t base_hex = palette_hex(active_scene_idx, t);
		uint32_t top_hex = blend_hex(base_hex, 0xFFFFFFU, 4U);
		uint32_t bot_hex = darken_hex(base_hex, 20U);
		uint32_t slot_hex = darken_hex(base_hex, 216U);
		uint32_t edge_hex = darken_hex(base_hex, 72U);
		uint32_t flash_hex = blend_hex(base_hex, 0xFFFFFFU, 72U);
		uint32_t cap_hex = blend_hex(base_hex, 0xFFFFFFU, 108U);

		if (eq_slot[i] != NULL) {
			lv_obj_set_style_bg_color(eq_slot[i], lv_color_hex(slot_hex), 0);
			lv_obj_set_style_border_color(eq_slot[i], lv_color_hex(edge_hex), 0);
			lv_obj_set_style_bg_opa(eq_slot[i], 210, 0);
			lv_obj_set_style_border_opa(eq_slot[i], 170, 0);
		}
		if (eq_fill[i] != NULL) {
			lv_obj_set_style_bg_color(eq_fill[i], lv_color_hex(top_hex), 0);
			lv_obj_set_style_bg_grad_color(eq_fill[i], lv_color_hex(bot_hex), 0);
			lv_obj_set_style_shadow_color(eq_fill[i], lv_color_hex(base_hex), 0);
			lv_obj_set_style_shadow_width(eq_fill[i], 10, 0);
			lv_obj_set_style_shadow_spread(eq_fill[i], 1, 0);
			lv_obj_set_style_shadow_opa(eq_fill[i], 210, 0);
		}
		if (eq_flash[i] != NULL) {
			lv_obj_set_style_bg_color(eq_flash[i], lv_color_hex(flash_hex), 0);
			lv_obj_set_style_bg_grad_color(
				eq_flash[i],
				lv_color_hex(blend_hex(flash_hex, base_hex, 88U)),
				0
			);
			lv_obj_set_style_shadow_color(eq_flash[i], lv_color_hex(flash_hex), 0);
			lv_obj_set_style_bg_opa(eq_flash[i], 220, 0);
			lv_obj_set_style_shadow_width(eq_flash[i], 8, 0);
			lv_obj_set_style_shadow_spread(eq_flash[i], 1, 0);
			lv_obj_set_style_shadow_opa(eq_flash[i], 170, 0);
		}
		if (eq_cap[i] != NULL) {
			lv_obj_set_style_bg_color(eq_cap[i], lv_color_hex(cap_hex), 0);
			lv_obj_set_style_shadow_color(eq_cap[i], lv_color_hex(flash_hex), 0);
			lv_obj_set_style_shadow_width(eq_cap[i], 6, 0);
			lv_obj_set_style_shadow_opa(eq_cap[i], 170, 0);
		}

		/* Avoid right-edge bright artifact from the final band. */
		if (i == (EQ_BAR_COUNT - 1U)) {
			if (eq_slot[i] != NULL) {
				lv_obj_set_style_border_width(eq_slot[i], 0, 0);
				lv_obj_set_style_border_opa(eq_slot[i], 0, 0);
			}
			if (eq_fill[i] != NULL) {
				lv_obj_set_style_shadow_width(eq_fill[i], 0, 0);
				lv_obj_set_style_shadow_opa(eq_fill[i], 0, 0);
			}
			if (eq_flash[i] != NULL) {
				lv_obj_set_style_shadow_width(eq_flash[i], 0, 0);
				lv_obj_set_style_shadow_opa(eq_flash[i], 0, 0);
			}
			if (eq_cap[i] != NULL) {
				lv_obj_set_style_shadow_width(eq_cap[i], 0, 0);
				lv_obj_set_style_shadow_opa(eq_cap[i], 0, 0);
			}
		}
	}

	if (progress_fill_obj != NULL) {
		lv_obj_set_style_bg_color(progress_fill_obj, lv_color_hex(palette_hex(active_scene_idx, 28U)), 0);
		lv_obj_set_style_bg_grad_color(
			progress_fill_obj,
			lv_color_hex(palette_hex(active_scene_idx, 235U)),
			0
		);
	}

}

static void set_eq_visible(bool show)
{
	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		if (eq_slot[i] == NULL) {
			continue;
		}
		if (show) {
			lv_obj_clear_flag(eq_slot[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(eq_slot[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (beat_pulse_layer != NULL) {
		if (show) {
			lv_obj_clear_flag(beat_pulse_layer, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(beat_pulse_layer, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void piano_slide_exec_cb(void *obj, int32_t value)
{
	lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)value);
}

static void set_piano_visible(bool show)
{
	if (piano_panel_obj == NULL) {
		piano_visible = false;
		return;
	}

	lv_anim_del(piano_panel_obj, piano_slide_exec_cb);
	lv_obj_set_y(piano_panel_obj, piano_panel_y_shown);
	if (show) {
		lv_obj_clear_flag(piano_panel_obj, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(piano_panel_obj, LV_OBJ_FLAG_HIDDEN);
	}

	piano_visible = show;
}

static void set_piano_mode(bool enabled)
{
	piano_mode_active = enabled;
	set_eq_visible(!enabled);
	if (enabled) {
		ground_all_bars();
	}
	piano_reset_state();
	set_piano_visible(enabled);
}

static void piano_apply_visuals(void)
{
	for (uint8_t i = 0; i < PIANO_WHITE_KEY_COUNT; i++) {
		lv_obj_t *key = piano_white_keys[i];
		uint8_t hit = piano_white_hit[i];
		uint8_t t = (uint8_t)((i * 255U) / MAX(1U, (PIANO_WHITE_KEY_COUNT - 1U)));
		uint8_t mix_t = (uint8_t)((hit * 180U) / 255U);
		uint8_t edge_t = (uint8_t)((hit * 110U) / 255U);
		uint8_t shadow_opa = (uint8_t)((hit * 170U) / 255U);
		lv_coord_t shadow_w = (lv_coord_t)(1 + ((hit * 8U) / 255U));
		uint32_t accent_hex = palette_hex(active_scene_idx, t);
		uint32_t hot_hex = blend_hex(accent_hex, 0xFFFFFFU, 48U);
		uint32_t base_hex = 0xF8FAFFU;
		uint32_t bg_hex = blend_hex(base_hex, hot_hex, mix_t);
		uint32_t edge_hex = blend_hex(0xAEB5C7U, accent_hex, edge_t);

		if (key == NULL) {
			continue;
		}
		lv_obj_set_style_bg_color(key, lv_color_hex(bg_hex), 0);
		lv_obj_set_style_border_color(key, lv_color_hex(edge_hex), 0);
		lv_obj_set_style_shadow_color(key, lv_color_hex(accent_hex), 0);
		lv_obj_set_style_shadow_width(key, shadow_w, 0);
		lv_obj_set_style_shadow_opa(key, shadow_opa, 0);
	}

	for (uint8_t i = 0; i < piano_black_used_count; i++) {
		lv_obj_t *key = piano_black_keys[i];
		uint8_t hit = piano_black_hit[i];
		uint8_t t = (uint8_t)((i * 255U) / MAX(1U, (piano_black_used_count - 1U)));
		uint8_t mix_t = (uint8_t)((hit * 210U) / 255U);
		uint8_t edge_t = (uint8_t)((hit * 160U) / 255U);
		uint8_t shadow_opa = (uint8_t)(60U + ((hit * 170U) / 255U));
		lv_coord_t shadow_w = (lv_coord_t)(4 + ((hit * 8U) / 255U));
		uint32_t accent_hex = palette_hex(active_scene_idx, t);
		uint32_t hot_hex = blend_hex(accent_hex, 0xFFFFFFU, 24U);
		uint32_t base_hex = 0x0B1028U;
		uint32_t bg_hex = blend_hex(base_hex, hot_hex, mix_t);
		uint32_t edge_hex = blend_hex(0x202746U, accent_hex, edge_t);

		if (key == NULL) {
			continue;
		}
		lv_obj_set_style_bg_color(key, lv_color_hex(bg_hex), 0);
		lv_obj_set_style_border_color(key, lv_color_hex(edge_hex), 0);
		lv_obj_set_style_shadow_color(key, lv_color_hex(accent_hex), 0);
		lv_obj_set_style_shadow_width(key, shadow_w, 0);
		lv_obj_set_style_shadow_opa(key, shadow_opa, 0);
	}
}

static void piano_reset_state(void)
{
	for (uint8_t i = 0; i < PIANO_WHITE_KEY_COUNT; i++) {
		piano_white_hit[i] = 0;
	}
	for (uint8_t i = 0; i < PIANO_BLACK_KEY_COUNT; i++) {
		piano_black_hit[i] = 0;
	}
	piano_apply_visuals();
}

static void update_piano_frame(uint32_t dt_ms)
{
	uint32_t decay = (PIANO_HIT_DECAY_PER_SEC * dt_ms) / 1000U;

	if (decay < 1U) {
		decay = 1U;
	}
	if (decay > 255U) {
		decay = 255U;
	}

	for (uint8_t i = 0; i < PIANO_WHITE_KEY_COUNT; i++) {
		if (piano_white_hit[i] <= decay) {
			piano_white_hit[i] = 0;
		} else {
			piano_white_hit[i] = (uint8_t)(piano_white_hit[i] - decay);
		}
	}
	for (uint8_t i = 0; i < piano_black_used_count; i++) {
		if (piano_black_hit[i] <= decay) {
			piano_black_hit[i] = 0;
		} else {
			piano_black_hit[i] = (uint8_t)(piano_black_hit[i] - decay);
		}
	}

	if (!playback_paused) {
		uint8_t hit_count = (uint8_t)(1U + (prng_next() & 0x01U));

		if ((prng_next() & 0x0FU) == 0U) {
			hit_count++;
		}

		for (uint8_t h = 0; h < hit_count; h++) {
			uint32_t r = prng_next();
			uint8_t amp = (uint8_t)(PIANO_HIT_MIN + (r % (PIANO_HIT_MAX - PIANO_HIT_MIN + 1U)));
			bool use_black = ((((r >> 8) & 0x03U) == 0U) && (piano_black_used_count > 0U));

			if (use_black) {
				uint8_t b = (uint8_t)(prng_next() % piano_black_used_count);
				uint8_t w = (uint8_t)((b * PIANO_WHITE_KEY_COUNT) / MAX(1U, piano_black_used_count));
				uint8_t w_amp = (amp > 46U) ? (uint8_t)(amp - 46U) : amp;

				if (amp > piano_black_hit[b]) {
					piano_black_hit[b] = amp;
				}
				if ((w < PIANO_WHITE_KEY_COUNT) && (w_amp > piano_white_hit[w])) {
					piano_white_hit[w] = w_amp;
				}
			} else {
				uint8_t w = (uint8_t)(prng_next() % PIANO_WHITE_KEY_COUNT);

				if (amp > piano_white_hit[w]) {
					piano_white_hit[w] = amp;
				}
				if ((w + 1U) < PIANO_WHITE_KEY_COUNT && ((r & 0x30U) == 0x30U)) {
					uint8_t chord_amp = (amp > 34U) ? (uint8_t)(amp - 34U) : amp;

					if (chord_amp > piano_white_hit[w + 1U]) {
						piano_white_hit[w + 1U] = chord_amp;
					}
				}
			}
		}
	}

	piano_apply_visuals();
	update_progress_line();
}

static int create_piano_overlay(lv_obj_t *screen, lv_coord_t screen_w, lv_coord_t screen_h)
{
	const uint8_t black_rel_idx[] = { 0U, 1U, 3U, 4U, 5U };
	const lv_coord_t panel_h = MAX(66, (screen_h * 33) / 100);
	const lv_coord_t panel_w = screen_w - 12;
	const lv_coord_t panel_x = 6;
	const lv_coord_t white_gap = 2;
	const lv_coord_t inner_pad = 6;
	lv_coord_t white_w;
	lv_coord_t white_h;
	lv_coord_t black_w;
	lv_coord_t black_h;
	lv_coord_t key_y;
	lv_coord_t start_x;
	uint8_t black_idx = 0U;

	piano_panel_obj = lv_obj_create(screen);
	if (piano_panel_obj == NULL) {
		return -1;
	}

	piano_panel_y_shown = screen_h - panel_h - 2;
	piano_panel_y_hidden = screen_h + 2;
	if (piano_panel_y_shown < 0) {
		piano_panel_y_shown = 0;
	}

	lv_obj_set_size(piano_panel_obj, panel_w, panel_h);
	lv_obj_set_pos(piano_panel_obj, panel_x, piano_panel_y_hidden);
	lv_obj_set_style_border_width(piano_panel_obj, 1, 0);
	lv_obj_set_style_border_color(piano_panel_obj, lv_color_hex(0x2A2E4AU), 0);
	lv_obj_set_style_border_opa(piano_panel_obj, 180, 0);
	lv_obj_set_style_radius(piano_panel_obj, 10, 0);
	lv_obj_set_style_pad_all(piano_panel_obj, 0, 0);
	lv_obj_set_style_bg_color(piano_panel_obj, lv_color_hex(0x070A14U), 0);
	lv_obj_set_style_bg_opa(piano_panel_obj, 220, 0);
	lv_obj_set_style_shadow_color(piano_panel_obj, lv_color_hex(0x000000U), 0);
	lv_obj_set_style_shadow_width(piano_panel_obj, 12, 0);
	lv_obj_set_style_shadow_opa(piano_panel_obj, 180, 0);
	lv_obj_clear_flag(piano_panel_obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(piano_panel_obj, LV_SCROLLBAR_MODE_OFF);

	white_w = (panel_w - (2 * inner_pad) - ((PIANO_WHITE_KEY_COUNT - 1) * white_gap)) / PIANO_WHITE_KEY_COUNT;
	if (white_w < 6) {
		white_w = 6;
	}

	white_h = panel_h - 14;
	if (white_h < 26) {
		white_h = 26;
	}
	key_y = panel_h - white_h - 5;
	start_x = (panel_w - ((white_w * PIANO_WHITE_KEY_COUNT) + ((PIANO_WHITE_KEY_COUNT - 1) * white_gap))) / 2;
	if (start_x < inner_pad) {
		start_x = inner_pad;
	}

	for (uint8_t i = 0U; i < PIANO_WHITE_KEY_COUNT; i++) {
		lv_obj_t *key = lv_obj_create(piano_panel_obj);

		if (key == NULL) {
			return -1;
		}
		lv_obj_set_size(key, white_w, white_h);
		lv_obj_set_pos(key, start_x + (i * (white_w + white_gap)), key_y);
		lv_obj_set_style_radius(key, 2, 0);
		lv_obj_set_style_border_width(key, 1, 0);
		lv_obj_set_style_border_color(key, lv_color_hex(0xB4B9C6U), 0);
		lv_obj_set_style_border_opa(key, 175, 0);
		lv_obj_set_style_bg_color(key, lv_color_hex(0xFFFFFFU), 0);
		lv_obj_set_style_bg_opa(key, 230, 0);
		lv_obj_set_style_shadow_width(key, 0, 0);
		lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);
		piano_white_keys[i] = key;
	}

	black_w = MAX(4, (white_w * 56) / 100);
	black_h = (white_h * 58) / 100;
	if (black_h < 14) {
		black_h = 14;
	}

	for (uint8_t oct = 0U; oct < 2U; oct++) {
		for (uint8_t j = 0U; j < ARRAY_SIZE(black_rel_idx); j++) {
			uint8_t left_idx = (oct * 7U) + black_rel_idx[j];
			lv_coord_t left_x;
			lv_coord_t right_x;
			lv_coord_t black_x;
			lv_obj_t *key;

			if ((left_idx + 1U) >= PIANO_WHITE_KEY_COUNT) {
				continue;
			}

			left_x = start_x + (left_idx * (white_w + white_gap));
			right_x = start_x + ((left_idx + 1U) * (white_w + white_gap));
			black_x = right_x - (black_w / 2);

			key = lv_obj_create(piano_panel_obj);
			if (key == NULL) {
				return -1;
			}
			lv_obj_set_size(key, black_w, black_h);
			lv_obj_set_pos(key, black_x, key_y);
			lv_obj_set_style_radius(key, 2, 0);
			lv_obj_set_style_border_width(key, 1, 0);
			lv_obj_set_style_border_color(key, lv_color_hex(0x1F2242U), 0);
			lv_obj_set_style_border_opa(key, 220, 0);
			lv_obj_set_style_bg_color(key, lv_color_hex(0x0B1028U), 0);
			lv_obj_set_style_bg_opa(key, 250, 0);
			lv_obj_set_style_shadow_color(key, lv_color_hex(0x5A6CFFU), 0);
			lv_obj_set_style_shadow_width(key, 6, 0);
			lv_obj_set_style_shadow_opa(key, 80, 0);
			lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);

			if (black_idx < PIANO_BLACK_KEY_COUNT) {
				piano_black_keys[black_idx] = key;
				black_idx++;
			}
		}
	}

	piano_black_used_count = black_idx;
	piano_visible = false;
	lv_obj_add_flag(piano_panel_obj, LV_OBJ_FLAG_HIDDEN);
	piano_reset_state();
	return 0;
}

static void status_led_toggle_safe(void)
{
#if HAVE_STATUS_LED
	if (status_led_ready) {
		(void)gpio_pin_toggle_dt(&status_led);
	}
#endif
}

static void fail_blink_forever(uint32_t period_ms)
{
	while (1) {
		status_led_toggle_safe();
		k_msleep(period_ms);
	}
}

static uint32_t prng_next(void)
{
	prng_state = (1664525U * prng_state) + 1013904223U;
	return prng_state;
}

static uint32_t playback_elapsed_ms(void)
{
#if HAVE_MIDI_EQ_DATA
	const uint32_t total_song_ms = MIDI_EQ_FRAME_COUNT * MIDI_EQ_FRAME_MS;
	uint32_t now_ms = playback_paused ? playback_pause_started_ms : k_uptime_get_32();
	uint32_t elapsed_ms = now_ms - playback_start_ms;

	if (elapsed_ms > playback_pause_accum_ms) {
		elapsed_ms -= playback_pause_accum_ms;
	} else {
		elapsed_ms = 0U;
	}

	if (total_song_ms == 0U) {
		return elapsed_ms;
	}
	return elapsed_ms % total_song_ms;
#else
	return k_uptime_get_32() - playback_start_ms;
#endif
}

static void title_slide_exec_cb(void *obj, int32_t value)
{
	lv_obj_set_style_translate_x((lv_obj_t *)obj, (lv_coord_t)value, 0);
}

static void set_title_slide(bool enabled)
{
	if ((title_glow_label == NULL) || (title_label == NULL)) {
		return;
	}

	lv_anim_del(title_glow_label, title_slide_exec_cb);
	lv_anim_del(title_label, title_slide_exec_cb);

	if (!enabled) {
		lv_obj_set_style_translate_x(title_glow_label, 0, 0);
		lv_obj_set_style_translate_x(title_label, 0, 0);
		return;
	}
}

static void set_header_long_mode(bool elapsed_time_mode)
{
	if (title_glow_label == NULL || title_label == NULL) {
		return;
	}

	if (elapsed_time_mode) {
		lv_label_set_long_mode(title_glow_label, LV_LABEL_LONG_MODE_CLIP);
		lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_CLIP);
		lv_obj_set_style_text_align(title_glow_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
		set_title_slide(false);
	} else {
		lv_label_set_long_mode(title_glow_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
		lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
		lv_obj_set_style_text_align(title_glow_label, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_style_anim_duration(title_glow_label, 5200, 0);
		lv_obj_set_style_anim_duration(title_label, 5200, 0);
		set_title_slide(true);
	}
}

static void set_header_text(const char *text)
{
	if (title_glow_label != NULL) {
		lv_label_set_text(title_glow_label, text);
		lv_obj_invalidate(title_glow_label);
	}
	if (title_label != NULL) {
		lv_label_set_text(title_label, text);
		lv_obj_invalidate(title_label);
	}
}

static void refresh_header_text(bool force)
{
	if ((title_glow_label == NULL) || (title_label == NULL)) {
		return;
	}

	if (!show_elapsed_time) {
		if (force) {
			(void)snprintf(
				marquee_text_buf,
				sizeof(marquee_text_buf),
				"%s%s%s%s%s%s%s",
				EQ_UI_TITLE,
				TITLE_MARQUEE_GAP,
				EQ_UI_TITLE,
				TITLE_MARQUEE_GAP,
				EQ_UI_TITLE,
				TITLE_MARQUEE_GAP,
				EQ_UI_TITLE
			);
			set_header_text(marquee_text_buf);
		}
		last_displayed_second = UINT32_MAX;
		return;
	}

	uint32_t elapsed_sec = playback_elapsed_ms() / 1000U;
	if (!force && (elapsed_sec == last_displayed_second)) {
		return;
	}
	last_displayed_second = elapsed_sec;

	(void)snprintf(
		header_text_buf,
		sizeof(header_text_buf),
		"%02u:%02u",
		(unsigned int)(elapsed_sec / 60U),
		(unsigned int)(elapsed_sec % 60U)
	);
	set_header_text(header_text_buf);
}

#if HAVE_MODE_BUTTON
static void mode_button_isr(
	const struct device *port,
	struct gpio_callback *cb,
	uint32_t pins
)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();
	if ((now - mode_button_last_press_ms) < MODE_BUTTON_DEBOUNCE_MS) {
		return;
	}
	mode_button_last_press_ms = now;
	mode_toggle_pending = true;
}
#endif

#if HAVE_SCENE_BUTTON
static void scene_button_isr(
	const struct device *port,
	struct gpio_callback *cb,
	uint32_t pins
)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();
	if ((now - scene_button_last_press_ms) < MODE_BUTTON_DEBOUNCE_MS) {
		return;
	}
	scene_button_last_press_ms = now;
	scene_cycle_pending = true;
}
#endif

#if HAVE_PAUSE_BUTTON
static void pause_button_isr(
	const struct device *port,
	struct gpio_callback *cb,
	uint32_t pins
)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();
	if ((now - pause_button_last_press_ms) < MODE_BUTTON_DEBOUNCE_MS) {
		return;
	}
	pause_button_last_press_ms = now;
	pause_toggle_pending = true;
}
#endif

#if HAVE_PIANO_BUTTON
static void piano_button_isr(
	const struct device *port,
	struct gpio_callback *cb,
	uint32_t pins
)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();
	if ((now - piano_button_last_press_ms) < MODE_BUTTON_DEBOUNCE_MS) {
		return;
	}
	piano_button_last_press_ms = now;
	piano_toggle_pending = true;
}
#endif

static void setup_mode_button(void)
{
#if HAVE_MODE_BUTTON
	if (!gpio_is_ready_dt(&mode_button)) {
		printk("Mode button not ready\\n");
		return;
	}
	if (gpio_pin_configure_dt(&mode_button, GPIO_INPUT) != 0) {
		printk("Mode button config failed\\n");
		return;
	}
	if (gpio_pin_interrupt_configure_dt(&mode_button, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		printk("Mode button interrupt setup failed\\n");
		return;
	}
	gpio_init_callback(&mode_button_cb_data, mode_button_isr, BIT(mode_button.pin));
	if (gpio_add_callback(mode_button.port, &mode_button_cb_data) != 0) {
		printk("Mode button callback add failed\\n");
	}
#endif
}

static void setup_scene_button(void)
{
#if HAVE_SCENE_BUTTON
	if (!gpio_is_ready_dt(&scene_button)) {
		printk("Scene button not ready\\n");
		return;
	}
	if (gpio_pin_configure_dt(&scene_button, GPIO_INPUT) != 0) {
		printk("Scene button config failed\\n");
		return;
	}
	if (gpio_pin_interrupt_configure_dt(&scene_button, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		printk("Scene button interrupt setup failed\\n");
		return;
	}
	gpio_init_callback(&scene_button_cb_data, scene_button_isr, BIT(scene_button.pin));
	if (gpio_add_callback(scene_button.port, &scene_button_cb_data) != 0) {
		printk("Scene button callback add failed\\n");
	}
#endif
}

static void setup_pause_button(void)
{
#if HAVE_PAUSE_BUTTON
	if (!gpio_is_ready_dt(&pause_button)) {
		printk("Pause button not ready\\n");
		return;
	}
	if (gpio_pin_configure_dt(&pause_button, GPIO_INPUT) != 0) {
		printk("Pause button config failed\\n");
		return;
	}
	if (gpio_pin_interrupt_configure_dt(&pause_button, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		printk("Pause button interrupt setup failed\\n");
		return;
	}
	gpio_init_callback(&pause_button_cb_data, pause_button_isr, BIT(pause_button.pin));
	if (gpio_add_callback(pause_button.port, &pause_button_cb_data) != 0) {
		printk("Pause button callback add failed\\n");
	}
#endif
}

static void setup_piano_button(void)
{
#if HAVE_PIANO_BUTTON
	if (!gpio_is_ready_dt(&piano_button)) {
		printk("Piano button not ready\\n");
		return;
	}
	if (gpio_pin_configure_dt(&piano_button, GPIO_INPUT) != 0) {
		printk("Piano button config failed\\n");
		return;
	}
	if (gpio_pin_interrupt_configure_dt(&piano_button, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
		printk("Piano button interrupt setup failed\\n");
		return;
	}
	gpio_init_callback(&piano_button_cb_data, piano_button_isr, BIT(piano_button.pin));
	if (gpio_add_callback(piano_button.port, &piano_button_cb_data) != 0) {
		printk("Piano button callback add failed\\n");
	}
#endif
}

static void ground_all_bars(void)
{
	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		eq_level_fp[i] = 0;
		eq_energy_fp[i] = 0;
		eq_flash_fp[i] = 0;
		eq_cap_fp[i] = 0;
		if (eq_fill[i] != NULL) {
			lv_obj_set_height(eq_fill[i], 1);
			lv_obj_align(eq_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
		}
		if (eq_flash[i] != NULL) {
			lv_obj_set_height(eq_flash[i], 1);
			lv_obj_align(eq_flash[i], LV_ALIGN_BOTTOM_MID, 0, 0);
		}
		if (eq_cap[i] != NULL) {
			lv_obj_set_y(eq_cap[i], slot_h_px - PEAK_CAP_H);
		}
	}

}

static void excite_energy(uint32_t dt_ms)
{
	ARG_UNUSED(dt_ms);
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;

#if HAVE_MIDI_EQ_DATA
	int32_t shaped_fp[EQ_BAR_COUNT];

	{
		uint32_t elapsed_ms = playback_elapsed_ms();
		uint32_t frame_cursor = elapsed_ms / MIDI_EQ_FRAME_MS;

		if (frame_cursor >= MIDI_EQ_FRAME_COUNT) {
			frame_cursor = MIDI_EQ_FRAME_COUNT - 1U;
		}

		midi_song_elapsed_ms = elapsed_ms;
		midi_frame_cursor = frame_cursor;
		midi_frame_elapsed_ms = elapsed_ms % MIDI_EQ_FRAME_MS;
	}

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		int32_t lvl_fp = (int32_t)midi_eq_frames[midi_frame_cursor][i] * FP_ONE;

		lvl_fp = (lvl_fp * midi_bar_gain_pct[i]) / 100;
		if (lvl_fp > max_fp) {
			lvl_fp = max_fp;
		}
		if (lvl_fp < 0) {
			lvl_fp = 0;
		}
		shaped_fp[i] = lvl_fp;
	}

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		int32_t acc = shaped_fp[i] * 84;
		int32_t w = 84;

		if (i > 0U) {
			acc += shaped_fp[i - 1U] * 8;
			w += 8;
		}
		if (i + 1U < EQ_BAR_COUNT) {
			acc += shaped_fp[i + 1U] * 8;
			w += 8;
		}
		eq_energy_fp[i] = (acc / w) * midi_bar_gain_pct[i] / 100;
		if (eq_energy_fp[i] > max_fp) {
			eq_energy_fp[i] = max_fp;
		}
	}
	return;
#endif

	int32_t base_fp = EQ_BASE_LEVEL * FP_ONE;

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		/* Pull energy toward the baseline (1/3 from the bottom). */
		eq_energy_fp[i] += ((base_fp - eq_energy_fp[i]) * 24) >> 8;

		uint32_t r = prng_next();
		int32_t kick = (int32_t)((r & 0x7FU) - 63U); /* -63..64 */

		/* Occasional stronger transients. */
		if ((r & 0x700U) == 0x700U) {
			kick += 70;
		} else if ((r & 0x3800U) == 0x0800U) {
			kick -= 36;
		}

		/* Keep middle bands slightly more active. */
		if (i > 2U && i < (EQ_BAR_COUNT - 3U)) {
			kick = (kick * 14) / 10;
		}

		eq_energy_fp[i] += kick * (FP_ONE / 6);
		if (eq_energy_fp[i] > max_fp) {
			eq_energy_fp[i] = max_fp;
		}
		if (eq_energy_fp[i] < 0) {
			eq_energy_fp[i] = 0;
		}
	}
}

static int create_eq_ui(void)
{
	lv_obj_t *screen = lv_screen_active();
	lv_coord_t screen_w = disp_w;
	lv_coord_t screen_h = disp_h;
	lv_coord_t top_pad = 36;
	lv_coord_t bottom_pad = 8;
	lv_coord_t side_pad = 8;
	lv_coord_t gap = 3;
	lv_coord_t slot_h = screen_h - top_pad - bottom_pad;
	lv_coord_t usable_w = screen_w - (2 * side_pad);
	lv_coord_t bar_w = (lv_coord_t)((usable_w - ((EQ_BAR_COUNT - 1) * gap)) / EQ_BAR_COUNT);
	lv_coord_t bars_total_w;
	lv_coord_t bars_x;

	if (bar_w < 6) {
		bar_w = 6;
	}
	if (slot_h < 24) {
		slot_h = 24;
	}
	bars_total_w = (bar_w * EQ_BAR_COUNT) + ((EQ_BAR_COUNT - 1) * gap);
	bars_x = (screen_w - bars_total_w) / 2;
	if (bars_x < 0) {
		bars_x = 0;
	}

	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x050312), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

	create_background_layer(screen, screen_w, screen_h);
	beat_pulse_layer = lv_obj_create(screen);
	if (beat_pulse_layer == NULL) {
		return -1;
	}
	lv_obj_set_size(beat_pulse_layer, screen_w, screen_h);
	lv_obj_set_pos(beat_pulse_layer, 0, 0);
	lv_obj_set_style_border_width(beat_pulse_layer, 0, 0);
	lv_obj_set_style_radius(beat_pulse_layer, 0, 0);
	lv_obj_set_style_bg_color(beat_pulse_layer, lv_color_hex(0xFF4D78), 0);
	lv_obj_set_style_bg_opa(beat_pulse_layer, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(beat_pulse_layer, LV_OBJ_FLAG_SCROLLABLE);

	title_glow_label = lv_label_create(screen);
	title_label = lv_label_create(screen);
	title_line_obj = lv_obj_create(screen);
	progress_track_obj = lv_obj_create(screen);
	if ((title_glow_label == NULL) || (title_label == NULL) || (title_line_obj == NULL) ||
	    (progress_track_obj == NULL)) {
		return -1;
	}

	lv_label_set_text(title_glow_label, EQ_UI_TITLE);
	lv_obj_set_width(title_glow_label, screen_w - 10);
	lv_obj_set_style_text_font(title_glow_label, &lv_font_unscii_16, 0);
	lv_obj_set_style_text_letter_space(title_glow_label, 1, 0);
	lv_obj_set_style_text_align(title_glow_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(title_glow_label, lv_color_hex(0x00FFF5), 0);
	lv_obj_set_style_text_opa(title_glow_label, 180, 0);
	lv_obj_align(title_glow_label, LV_ALIGN_TOP_MID, 1, 4);

	lv_label_set_text(title_label, EQ_UI_TITLE);
	lv_obj_set_width(title_label, screen_w - 10);
	lv_obj_set_style_text_font(title_label, &lv_font_unscii_16, 0);
	lv_obj_set_style_text_letter_space(title_label, 1, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 3);

	lv_obj_set_size(title_line_obj, screen_w - 30, 2);
	lv_obj_align(title_line_obj, LV_ALIGN_TOP_MID, 0, 26);
	lv_obj_set_style_border_width(title_line_obj, 0, 0);
	lv_obj_set_style_radius(title_line_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(title_line_obj, 185, 0);
	lv_obj_set_style_bg_color(title_line_obj, lv_color_hex(0x2DFDFF), 0);
	lv_obj_set_style_shadow_color(title_line_obj, lv_color_hex(0x00D6FF), 0);
	lv_obj_set_style_shadow_width(title_line_obj, 10, 0);
	lv_obj_set_style_shadow_opa(title_line_obj, 180, 0);
	lv_obj_clear_flag(title_line_obj, LV_OBJ_FLAG_SCROLLABLE);

	progress_track_w = screen_w - 30;
	lv_obj_set_size(progress_track_obj, progress_track_w, PROGRESS_LINE_H);
	lv_obj_align(progress_track_obj, LV_ALIGN_TOP_MID, 0, 30);
	lv_obj_set_style_border_width(progress_track_obj, 1, 0);
	lv_obj_set_style_border_opa(progress_track_obj, 120, 0);
	lv_obj_set_style_radius(progress_track_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_all(progress_track_obj, 0, 0);
	lv_obj_set_style_bg_opa(progress_track_obj, 180, 0);
	lv_obj_clear_flag(progress_track_obj, LV_OBJ_FLAG_SCROLLABLE);
	progress_fill_obj = lv_obj_create(progress_track_obj);
	if (progress_fill_obj == NULL) {
		return -1;
	}
	lv_obj_set_size(progress_fill_obj, 1, PROGRESS_LINE_H);
	lv_obj_align(progress_fill_obj, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_border_width(progress_fill_obj, 0, 0);
	lv_obj_set_style_radius(progress_fill_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(progress_fill_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_grad_dir(progress_fill_obj, LV_GRAD_DIR_HOR, 0);
	lv_obj_clear_flag(progress_fill_obj, LV_OBJ_FLAG_SCROLLABLE);

	slot_h_px = slot_h;

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		lv_obj_t *slot = lv_obj_create(screen);
		lv_obj_t *fill;
		lv_obj_t *flash;
		lv_obj_t *cap;
		uint8_t t = (uint8_t)((i * 255U) / (EQ_BAR_COUNT - 1U));
		uint32_t base_hex = palette_hex(active_scene_idx, t);
		uint32_t top_hex = blend_hex(base_hex, 0xFFFFFFU, 32U);
		uint32_t bot_hex = darken_hex(base_hex, 98U);
		uint32_t slot_hex = darken_hex(base_hex, 235U);
		uint32_t edge_hex = darken_hex(base_hex, 165U);

		if (slot == NULL) {
			return -1;
		}

		fill = lv_obj_create(slot);
		flash = lv_obj_create(slot);
		cap = lv_obj_create(slot);
		if ((fill == NULL) || (flash == NULL) || (cap == NULL)) {
			return -1;
		}

		eq_slot[i] = slot;
		eq_fill[i] = fill;
		eq_flash[i] = flash;
		eq_cap[i] = cap;
		eq_level_fp[i] = EQ_BASE_LEVEL * FP_ONE;
		eq_energy_fp[i] = EQ_BASE_LEVEL * FP_ONE;
		eq_flash_fp[i] = EQ_BASE_LEVEL * FP_ONE;
		eq_cap_fp[i] = EQ_BASE_LEVEL * FP_ONE;

		lv_obj_set_size(slot, bar_w, slot_h);
		lv_obj_set_pos(slot, bars_x + (i * (bar_w + gap)), top_pad);
		lv_obj_set_style_border_width(slot, 0, 0);
		lv_obj_set_style_pad_all(slot, 1, 0);
		lv_obj_set_style_radius(slot, 3, 0);
		lv_obj_set_style_bg_opa(slot, 185, 0);
		lv_obj_set_style_bg_color(slot, lv_color_hex(slot_hex), 0);
		lv_obj_set_style_border_width(slot, 1, 0);
		lv_obj_set_style_border_opa(slot, 120, 0);
		lv_obj_set_style_border_color(slot, lv_color_hex(edge_hex), 0);
		lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

		lv_obj_set_width(fill, bar_w - 2);
		lv_obj_set_height(fill, (slot_h * EQ_BASE_LEVEL) / EQ_MAX_LEVEL);
		lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_set_style_border_width(fill, 0, 0);
		lv_obj_set_style_radius(fill, 3, 0);
		lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(fill, lv_color_hex(top_hex), 0);
		lv_obj_set_style_bg_grad_color(fill, lv_color_hex(bot_hex), 0);
		lv_obj_set_style_bg_grad_dir(fill, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_shadow_color(fill, lv_color_hex(base_hex), 0);
		lv_obj_set_style_shadow_width(fill, 10, 0);
		lv_obj_set_style_shadow_spread(fill, 1, 0);
		lv_obj_set_style_shadow_opa(fill, 210, 0);
		lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);

		lv_obj_set_width(flash, MAX(1, bar_w - 4));
		lv_obj_set_height(flash, (slot_h * EQ_BASE_LEVEL) / EQ_MAX_LEVEL);
		lv_obj_align(flash, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_set_style_border_width(flash, 0, 0);
		lv_obj_set_style_radius(flash, 2, 0);
		lv_obj_set_style_bg_opa(flash, 168, 0);
		lv_obj_set_style_bg_color(flash, lv_color_hex(blend_hex(top_hex, 0xFFFFFFU, 100U)), 0);
		lv_obj_set_style_bg_grad_color(flash, lv_color_hex(base_hex), 0);
		lv_obj_set_style_bg_grad_dir(flash, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_shadow_color(flash, lv_color_hex(blend_hex(top_hex, 0xFFFFFFU, 120U)), 0);
		lv_obj_set_style_shadow_width(flash, 8, 0);
		lv_obj_set_style_shadow_spread(flash, 1, 0);
		lv_obj_set_style_shadow_opa(flash, 150, 0);
		lv_obj_clear_flag(flash, LV_OBJ_FLAG_SCROLLABLE);

		lv_obj_set_width(cap, MAX(2, bar_w - 2));
		lv_obj_set_height(cap, PEAK_CAP_H);
		lv_obj_set_pos(cap, 1, slot_h - ((slot_h * EQ_BASE_LEVEL) / EQ_MAX_LEVEL) - PEAK_CAP_H);
		lv_obj_set_style_border_width(cap, 0, 0);
		lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(cap, lv_color_hex(blend_hex(top_hex, 0xFFFFFFU, 150U)), 0);
		lv_obj_set_style_shadow_color(cap, lv_color_hex(top_hex), 0);
		lv_obj_set_style_shadow_width(cap, 6, 0);
		lv_obj_set_style_shadow_spread(cap, 1, 0);
		lv_obj_set_style_shadow_opa(cap, 150, 0);
		lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

		if (i == (EQ_BAR_COUNT - 1U)) {
			lv_obj_set_style_border_width(slot, 0, 0);
			lv_obj_set_style_border_opa(slot, 0, 0);
			lv_obj_set_style_shadow_width(fill, 0, 0);
			lv_obj_set_style_shadow_opa(fill, 0, 0);
			lv_obj_set_style_shadow_width(flash, 0, 0);
			lv_obj_set_style_shadow_opa(flash, 0, 0);
			lv_obj_set_style_shadow_width(cap, 0, 0);
			lv_obj_set_style_shadow_opa(cap, 0, 0);
		}
	}

	/* Subtle global dim so the screen isn't blinding. */
	lv_obj_t *dim = lv_obj_create(screen);
	if (dim == NULL) {
		return -1;
	}
	lv_obj_set_size(dim, screen_w, screen_h);
	lv_obj_set_pos(dim, 0, 0);
	lv_obj_set_style_border_width(dim, 0, 0);
	lv_obj_set_style_radius(dim, 0, 0);
	lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(dim, SCREEN_DIM_OPA, 0);
	lv_obj_clear_flag(dim, LV_OBJ_FLAG_SCROLLABLE);

	if (create_piano_overlay(screen, screen_w, screen_h) != 0) {
		return -1;
	}

	/* Guard strip to hide occasional right-edge draw artifacts. */
	lv_obj_t *edge_mask = lv_obj_create(screen);
	if (edge_mask == NULL) {
		return -1;
	}
	lv_obj_set_size(edge_mask, 2, screen_h);
	lv_obj_align(edge_mask, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_border_width(edge_mask, 0, 0);
	lv_obj_set_style_radius(edge_mask, 0, 0);
	lv_obj_set_style_bg_color(edge_mask, lv_color_hex(0x050312), 0);
	lv_obj_set_style_bg_opa(edge_mask, LV_OPA_COVER, 0);
	lv_obj_clear_flag(edge_mask, LV_OBJ_FLAG_SCROLLABLE);

	apply_scene_styles();
	update_progress_line();
	set_header_long_mode(false);
	refresh_header_text(true);

	return 0;
}

static void update_eq_frame(uint32_t dt_ms)
{
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;
	int32_t cap_drop_fp = (int32_t)(((uint32_t)EQ_CAP_FALL_PER_SEC * FP_ONE * dt_ms) / 1000U);

	if (cap_drop_fp < 1) {
		cap_drop_fp = 1;
	}

	if (playback_paused) {
		ground_all_bars();
		update_progress_line();
		return;
	}

	excite_energy(dt_ms);

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		int32_t delta = eq_energy_fp[i] - eq_level_fp[i];
		int32_t flash_target_fp;
		int32_t flash_delta;
		int32_t step;
		int32_t flash_step;
		int32_t cap_level_fp;

		if (delta >= 0) {
			step = (delta * EQ_ATTACK_GAIN) >> 8;
			if (step < EQ_MIN_UP_STEP) {
				step = EQ_MIN_UP_STEP;
			}
			if (step > EQ_MAX_UP_STEP) {
				step = EQ_MAX_UP_STEP;
			}
			eq_level_fp[i] += step;
			if (eq_level_fp[i] > eq_energy_fp[i]) {
				eq_level_fp[i] = eq_energy_fp[i];
			}
		} else {
			step = ((-delta) * EQ_RELEASE_GAIN) >> 8;
			if (step < EQ_MIN_DOWN_STEP) {
				step = EQ_MIN_DOWN_STEP;
			}
			if (step > EQ_MAX_DOWN_STEP) {
				step = EQ_MAX_DOWN_STEP;
			}
			eq_level_fp[i] -= step;
			if (eq_level_fp[i] < eq_energy_fp[i]) {
				eq_level_fp[i] = eq_energy_fp[i];
			}
		}

		if (eq_level_fp[i] < 0) {
			eq_level_fp[i] = 0;
		}
		if (eq_level_fp[i] > max_fp) {
			eq_level_fp[i] = max_fp;
		}

		flash_target_fp = eq_energy_fp[i] + ((eq_energy_fp[i] * EQ_FLASH_BOOST_PCT) / 100);
		if (flash_target_fp > max_fp) {
			flash_target_fp = max_fp;
		}
		flash_delta = flash_target_fp - eq_flash_fp[i];
		if (flash_delta >= 0) {
			flash_step = (flash_delta * EQ_FLASH_ATTACK_GAIN) >> 8;
			if (flash_step < EQ_MIN_UP_STEP) {
				flash_step = EQ_MIN_UP_STEP;
			}
			if (flash_step > (EQ_MAX_UP_STEP * 2)) {
				flash_step = EQ_MAX_UP_STEP * 2;
			}
			eq_flash_fp[i] += flash_step;
			if (eq_flash_fp[i] > flash_target_fp) {
				eq_flash_fp[i] = flash_target_fp;
			}
		} else {
			flash_step = ((-flash_delta) * EQ_FLASH_RELEASE_GAIN) >> 8;
			if (flash_step < EQ_MIN_DOWN_STEP) {
				flash_step = EQ_MIN_DOWN_STEP;
			}
			if (flash_step > (EQ_MAX_DOWN_STEP * 3)) {
				flash_step = EQ_MAX_DOWN_STEP * 3;
			}
			eq_flash_fp[i] -= flash_step;
			if (eq_flash_fp[i] < flash_target_fp) {
				eq_flash_fp[i] = flash_target_fp;
			}
		}
		if (eq_flash_fp[i] < 0) {
			eq_flash_fp[i] = 0;
		}
		if (eq_flash_fp[i] > max_fp) {
			eq_flash_fp[i] = max_fp;
		}

		if (eq_level_fp[i] >= eq_cap_fp[i]) {
			eq_cap_fp[i] = eq_level_fp[i];
		} else {
			eq_cap_fp[i] -= cap_drop_fp;
			if (eq_cap_fp[i] < eq_level_fp[i]) {
				eq_cap_fp[i] = eq_level_fp[i];
			}
		}
		if (eq_cap_fp[i] < 0) {
			eq_cap_fp[i] = 0;
		}
		if (eq_cap_fp[i] > max_fp) {
			eq_cap_fp[i] = max_fp;
		}

		int32_t level = (int32_t)(eq_level_fp[i] >> FP_SHIFT);
		int32_t flash_level = (int32_t)(eq_flash_fp[i] >> FP_SHIFT);
		cap_level_fp = eq_cap_fp[i];
		lv_coord_t fill_h;
		lv_coord_t flash_h;
		lv_coord_t cap_y;

		if (level < 0) {
			level = 0;
		}
		if (level > EQ_MAX_LEVEL) {
			level = EQ_MAX_LEVEL;
		}
		if (level > EQ_BASE_LEVEL) {
			level = EQ_BASE_LEVEL + (((level - EQ_BASE_LEVEL) * EQ_PEAK_BOOST_PCT) / 100);
			if (level > EQ_MAX_LEVEL) {
				level = EQ_MAX_LEVEL;
			}
		}
		if (flash_level < level) {
			flash_level = level;
		}
		if (flash_level > EQ_MAX_LEVEL) {
			flash_level = EQ_MAX_LEVEL;
		}

		fill_h = (lv_coord_t)((level * slot_h_px * EQ_VISUAL_HEIGHT_PCT) / (EQ_MAX_LEVEL * 100));
		flash_h = (lv_coord_t)((flash_level * slot_h_px * EQ_VISUAL_HEIGHT_PCT) / (EQ_MAX_LEVEL * 100));
		if (fill_h < 2) {
			fill_h = 2;
		}
		if (fill_h > slot_h_px) {
			fill_h = slot_h_px;
		}
		if (flash_h < 2) {
			flash_h = 2;
		}
		if (flash_h > slot_h_px) {
			flash_h = slot_h_px;
		}

		lv_obj_set_height(eq_fill[i], fill_h);
		lv_obj_align(eq_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
		if (eq_flash[i] != NULL) {
			lv_obj_set_height(eq_flash[i], flash_h);
			lv_obj_align(eq_flash[i], LV_ALIGN_BOTTOM_MID, 0, 0);
		}
		if (eq_cap[i] != NULL) {
			cap_y = (lv_coord_t)(
				slot_h_px -
				(lv_coord_t)(((cap_level_fp >> FP_SHIFT) * slot_h_px * EQ_VISUAL_HEIGHT_PCT) / (EQ_MAX_LEVEL * 100)) -
				PEAK_CAP_H
			);
			if (cap_y < 0) {
				cap_y = 0;
			}
			if (cap_y > (slot_h_px - PEAK_CAP_H)) {
				cap_y = slot_h_px - PEAK_CAP_H;
			}
			lv_obj_set_y(eq_cap[i], cap_y);
		}
	}

	update_progress_line();
}

int main(void)
{
#if HAVE_STATUS_LED
	if (gpio_is_ready_dt(&status_led) &&
	    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE) == 0) {
		status_led_ready = true;
	}
#endif

#if !DT_HAS_CHOSEN(zephyr_display)
	printk("No zephyr,display chosen node in devicetree\\n");
	fail_blink_forever(120);
#endif

	if (!device_is_ready(display_dev)) {
		printk("Display device not ready\\n");
		fail_blink_forever(250);
	}
	setup_mode_button();
	setup_scene_button();
	setup_pause_button();
	setup_piano_button();

	struct display_capabilities caps;
	display_get_capabilities(display_dev, &caps);
	disp_w = caps.x_resolution;
	disp_h = caps.y_resolution;

	if (create_eq_ui() != 0) {
		printk("EQ UI creation failed\\n");
		fail_blink_forever(500);
	}

	display_blanking_off(display_dev);
	playback_start_ms = k_uptime_get_32();

	int64_t next_render = k_uptime_get();
	int64_t last_render = next_render;

	while (1) {
		if (mode_toggle_pending) {
			mode_toggle_pending = false;
			show_elapsed_time = !show_elapsed_time;
			set_header_long_mode(show_elapsed_time);
			refresh_header_text(true);
		}
		if (scene_cycle_pending) {
			scene_cycle_pending = false;
			active_scene_idx = (active_scene_idx + 1U) % SCENE_COUNT;
			apply_scene_styles();
			printk("Scene: %s\\n", active_scene()->name);
		}
		if (pause_toggle_pending) {
			pause_toggle_pending = false;
			if (!playback_paused) {
				playback_paused = true;
				playback_pause_started_ms = k_uptime_get_32();
				ground_all_bars();
				printk("Playback paused\\n");
			} else {
				uint32_t now_ms = k_uptime_get_32();
				playback_pause_accum_ms += (now_ms - playback_pause_started_ms);
				playback_paused = false;
				printk("Playback resumed\\n");
			}
			refresh_header_text(true);
		}
		if (piano_toggle_pending) {
			piano_toggle_pending = false;
			set_piano_mode(!piano_mode_active);
			printk("Piano mode: %s\\n", piano_mode_active ? "on" : "off");
		}
		int64_t now = k_uptime_get();
		if (now >= next_render) {
			uint32_t dt_ms;

			if (now > last_render) {
				int64_t elapsed = now - last_render;

				if (elapsed > 200) {
					elapsed = 200;
				}
				dt_ms = (uint32_t)elapsed;
			} else {
				dt_ms = EQ_RENDER_MS;
			}

			if (piano_mode_active) {
				update_piano_frame(dt_ms);
			} else {
				update_eq_frame(dt_ms);
			}
			refresh_header_text(false);
			last_render = now;
			next_render = now + EQ_RENDER_MS;
		}

		lv_timer_handler();
		k_msleep(APP_TICK_MS);
	}

	return 0;
}
