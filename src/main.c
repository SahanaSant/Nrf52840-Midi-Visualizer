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
#define SCREEN_DIM_OPA LV_OPA_TRANSP
#define MODE_BUTTON_DEBOUNCE_MS 180

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

static lv_obj_t *eq_slot[EQ_BAR_COUNT];
static lv_obj_t *eq_fill[EQ_BAR_COUNT];
static lv_obj_t *title_glow_label;
static lv_obj_t *title_label;
static int32_t eq_level_fp[EQ_BAR_COUNT];
static int32_t eq_energy_fp[EQ_BAR_COUNT];
static lv_coord_t disp_w = 320;
static lv_coord_t disp_h = 240;
static lv_coord_t slot_h_px;
static bool status_led_ready;
static bool show_elapsed_time;
static volatile bool mode_toggle_pending;
static uint32_t prng_state = 0xA5A5F00DU;
static uint32_t midi_frame_cursor;
static uint32_t midi_frame_elapsed_ms;
static uint32_t midi_song_elapsed_ms;
static uint32_t last_displayed_second = UINT32_MAX;
static uint32_t playback_start_ms;
static int64_t mode_button_last_press_ms;
static char header_text_buf[24];

#if HAVE_MODE_BUTTON
static struct gpio_callback mode_button_cb_data;
#endif

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

static uint32_t palette_hex(uint8_t t)
{
	static const uint32_t stops[] = {
		0x002296U, /* deep blue */
		0x82008FU, /* violet */
		0xC0007AU, /* magenta */
		0xEA0C5FU, /* pink-red */
		0xFF5341U, /* warm red-orange */
		0xFF8820U, /* orange */
		0xF6BA00U  /* yellow */
	};
	uint16_t pos = (uint16_t)t * 6U;
	uint8_t idx = (uint8_t)(pos / 255U);
	uint8_t frac = (uint8_t)(pos % 255U);

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
	uint32_t elapsed_ms = k_uptime_get_32() - playback_start_ms;

	if (total_song_ms == 0U) {
		return elapsed_ms;
	}
	return elapsed_ms % total_song_ms;
#else
	return k_uptime_get_32() - playback_start_ms;
#endif
}

static void set_header_long_mode(bool elapsed_time_mode)
{
	if (title_glow_label == NULL || title_label == NULL) {
		return;
	}

	if (elapsed_time_mode) {
		lv_label_set_long_mode(title_glow_label, LV_LABEL_LONG_MODE_CLIP);
		lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_CLIP);
	} else {
		lv_label_set_long_mode(title_glow_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
		lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
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
			set_header_text(EQ_UI_TITLE);
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

static void excite_energy(uint32_t dt_ms)
{
	ARG_UNUSED(dt_ms);
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;

#if HAVE_MIDI_EQ_DATA
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
		if (lvl_fp > max_fp) {
			lvl_fp = max_fp;
		}
		if (lvl_fp < 0) {
			lvl_fp = 0;
		}
		eq_energy_fp[i] = lvl_fp;
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

	if (bar_w < 6) {
		bar_w = 6;
	}
	if (slot_h < 24) {
		slot_h = 24;
	}

	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x050312), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	create_background_layer(screen, screen_w, screen_h);

	title_glow_label = lv_label_create(screen);
	title_label = lv_label_create(screen);
	lv_obj_t *title_line = lv_obj_create(screen);
	if ((title_glow_label == NULL) || (title_label == NULL) || (title_line == NULL)) {
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

	lv_obj_set_size(title_line, screen_w - 30, 2);
	lv_obj_align(title_line, LV_ALIGN_TOP_MID, 0, 26);
	lv_obj_set_style_border_width(title_line, 0, 0);
	lv_obj_set_style_radius(title_line, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(title_line, 185, 0);
	lv_obj_set_style_bg_color(title_line, lv_color_hex(0x2DFDFF), 0);
	lv_obj_set_style_shadow_color(title_line, lv_color_hex(0x00D6FF), 0);
	lv_obj_set_style_shadow_width(title_line, 10, 0);
	lv_obj_set_style_shadow_opa(title_line, 180, 0);
	lv_obj_clear_flag(title_line, LV_OBJ_FLAG_SCROLLABLE);

	slot_h_px = slot_h;

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		lv_obj_t *slot = lv_obj_create(screen);
		lv_obj_t *fill;
		uint8_t t = (uint8_t)((i * 255U) / (EQ_BAR_COUNT - 1U));
		uint32_t base_hex = palette_hex(t);
		uint32_t top_hex = blend_hex(base_hex, 0xFFFFFFU, 32U);
		uint32_t bot_hex = darken_hex(base_hex, 98U);
		uint32_t slot_hex = darken_hex(base_hex, 235U);
		uint32_t edge_hex = darken_hex(base_hex, 165U);

		if (slot == NULL) {
			return -1;
		}

		fill = lv_obj_create(slot);
		if (fill == NULL) {
			return -1;
		}

		eq_slot[i] = slot;
		eq_fill[i] = fill;
		eq_level_fp[i] = EQ_BASE_LEVEL * FP_ONE;
		eq_energy_fp[i] = EQ_BASE_LEVEL * FP_ONE;

		lv_obj_set_size(slot, bar_w, slot_h);
		lv_obj_set_pos(slot, side_pad + (i * (bar_w + gap)), top_pad);
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
		lv_obj_set_style_shadow_width(fill, 14, 0);
		lv_obj_set_style_shadow_spread(fill, 2, 0);
		lv_obj_set_style_shadow_opa(fill, 230, 0);
		lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
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

	set_header_long_mode(false);
	refresh_header_text(true);

	return 0;
}

static void update_eq_frame(uint32_t dt_ms)
{
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;

	excite_energy(dt_ms);

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		int32_t delta = eq_energy_fp[i] - eq_level_fp[i];
		int32_t step;

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

		int32_t level = (int32_t)(eq_level_fp[i] >> FP_SHIFT);
		lv_coord_t fill_h;

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

		fill_h = (lv_coord_t)((level * slot_h_px) / EQ_MAX_LEVEL);
		if (fill_h < 2) {
			fill_h = 2;
		}
		if (fill_h > slot_h_px) {
			fill_h = slot_h_px;
		}

		lv_obj_set_height(eq_fill[i], fill_h);
		lv_obj_align(eq_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
	}
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

			update_eq_frame(dt_ms);
			refresh_header_text(false);
			last_render = now;
			next_render = now + EQ_RENDER_MS;
		}

		lv_timer_handler();
		k_msleep(APP_TICK_MS);
	}

	return 0;
}
