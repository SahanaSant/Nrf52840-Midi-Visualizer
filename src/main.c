#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#if __has_include("bg_image.h")
#include "bg_image.h"
#define HAVE_BG_IMAGE 1
#else
#define HAVE_BG_IMAGE 0
#endif

#define APP_TICK_MS 5
#define EQ_RENDER_MS 10
#define EQ_BAR_COUNT 12
#define EQ_MAX_LEVEL 100
#define EQ_BASE_LEVEL (EQ_MAX_LEVEL / 3)
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define TITLE_TEXT "my midi eq"
#define BG_IMAGE_OPA 190
#define BG_VEIL_OPA 90
#define SCREEN_DIM_OPA LV_OPA_TRANSP

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

static lv_obj_t *eq_slot[EQ_BAR_COUNT];
static lv_obj_t *eq_fill[EQ_BAR_COUNT];
static int32_t eq_level_fp[EQ_BAR_COUNT];
static int32_t eq_energy_fp[EQ_BAR_COUNT];
static lv_coord_t disp_w = 320;
static lv_coord_t disp_h = 240;
static lv_coord_t slot_h_px;
static bool status_led_ready;
static uint32_t prng_state = 0xA5A5F00DU;

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

static void excite_energy(void)
{
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;
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

	lv_obj_t *title_glow = lv_label_create(screen);
	lv_obj_t *title = lv_label_create(screen);
	lv_obj_t *title_line = lv_obj_create(screen);
	if ((title_glow == NULL) || (title == NULL) || (title_line == NULL)) {
		return -1;
	}

	lv_label_set_text(title_glow, TITLE_TEXT);
	lv_obj_set_width(title_glow, screen_w - 10);
	lv_obj_set_style_text_font(title_glow, &lv_font_unscii_16, 0);
	lv_obj_set_style_text_letter_space(title_glow, 1, 0);
	lv_obj_set_style_text_align(title_glow, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(title_glow, lv_color_hex(0x00FFF5), 0);
	lv_obj_set_style_text_opa(title_glow, 180, 0);
	lv_obj_align(title_glow, LV_ALIGN_TOP_MID, 1, 4);

	lv_label_set_text(title, TITLE_TEXT);
	lv_obj_set_width(title, screen_w - 10);
	lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
	lv_obj_set_style_text_letter_space(title, 1, 0);
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 3);

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

	return 0;
}

static void update_eq_frame(void)
{
	int32_t max_fp = EQ_MAX_LEVEL * FP_ONE;

	excite_energy();

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		int32_t delta = eq_energy_fp[i] - eq_level_fp[i];
		int32_t step;

		if (delta >= 0) {
			step = (delta * 212) >> 8;
			if (step < (FP_ONE / 3)) {
				step = FP_ONE / 3;
			}
			if (step > (FP_ONE * 5)) {
				step = FP_ONE * 5;
			}
			eq_level_fp[i] += step;
			if (eq_level_fp[i] > eq_energy_fp[i]) {
				eq_level_fp[i] = eq_energy_fp[i];
			}
		} else {
			step = ((-delta) * 62) >> 8;
			if (step < (FP_ONE / 8)) {
				step = FP_ONE / 8;
			}
			if (step > FP_ONE) {
				step = FP_ONE;
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
			level = EQ_BASE_LEVEL + (((level - EQ_BASE_LEVEL) * 150) / 100);
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

	struct display_capabilities caps;
	display_get_capabilities(display_dev, &caps);
	disp_w = caps.x_resolution;
	disp_h = caps.y_resolution;

	if (create_eq_ui() != 0) {
		printk("EQ UI creation failed\\n");
		fail_blink_forever(500);
	}

	display_blanking_off(display_dev);

	int64_t next_render = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		if (now >= next_render) {
			update_eq_frame();
			next_render = now + EQ_RENDER_MS;
		}

		lv_timer_handler();
		k_msleep(APP_TICK_MS);
	}

	return 0;
}
