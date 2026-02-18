#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#define APP_TICK_MS 5
#define EQ_RENDER_MS 10
#define EQ_BAR_COUNT 12
#define EQ_MAX_LEVEL 100
#define EQ_BASE_LEVEL (EQ_MAX_LEVEL / 3)
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

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

#if DT_HAS_CHOSEN(zephyr_display) && DT_NODE_HAS_PROP(DT_CHOSEN(zephyr_display), width)
#define DISP_W DT_PROP(DT_CHOSEN(zephyr_display), width)
#else
#define DISP_W 320
#endif

#if DT_HAS_CHOSEN(zephyr_display) && DT_NODE_HAS_PROP(DT_CHOSEN(zephyr_display), height)
#define DISP_H DT_PROP(DT_CHOSEN(zephyr_display), height)
#else
#define DISP_H 240
#endif

static lv_obj_t *eq_bar[EQ_BAR_COUNT];
static int32_t eq_level_fp[EQ_BAR_COUNT];
static int32_t eq_energy_fp[EQ_BAR_COUNT];
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
		/* Pull energy toward the baseline (2/3 screen height). */
		eq_energy_fp[i] += ((base_fp - eq_energy_fp[i]) * 30) >> 8;

		uint32_t r = prng_next();
		int32_t kick = (int32_t)((r & 0x3FU) - 31U); /* -31..32 */

		/* Occasional stronger transients. */
		if ((r & 0x3C0U) >= 0x300U) {
			kick += (r & 0x400U) ? 26 : -26;
		}

		/* Keep middle bands slightly more active. */
		if (i > 2U && i < (EQ_BAR_COUNT - 3U)) {
			kick = (kick * 11) / 10;
		}

		eq_energy_fp[i] += kick * (FP_ONE / 5);
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
	lv_coord_t screen_w = DISP_W;
	lv_coord_t screen_h = DISP_H;
	lv_coord_t top_pad = 18;
	lv_coord_t side_pad = 8;
	lv_coord_t gap = 3;
	lv_coord_t slot_h = screen_h - top_pad - 6;
	lv_coord_t usable_w = screen_w - (2 * side_pad);
	lv_coord_t bar_w = (lv_coord_t)((usable_w - ((EQ_BAR_COUNT - 1) * gap)) / EQ_BAR_COUNT);

	if (bar_w < 6) {
		bar_w = 6;
	}
	if (slot_h < 24) {
		slot_h = 24;
	}

	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x060A2A), 0);
	lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x2A063B), 0);
	lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	lv_obj_t *title = lv_label_create(screen);
	if (title == NULL) {
		return -1;
	}

	lv_label_set_text(title, "MIDI EQ DEMO");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);
	lv_obj_set_style_text_color(title, lv_color_hex(0xFFD66A), 0);

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		lv_obj_t *bar = lv_bar_create(screen);
		uint8_t t = (uint8_t)((i * 255U) / (EQ_BAR_COUNT - 1U));
		uint32_t base_hex = palette_hex(t);
		uint32_t top_hex = blend_hex(base_hex, 0xFFFFFFU, 40U);
		uint32_t bot_hex = darken_hex(base_hex, 58U);
		uint32_t slot_hex = darken_hex(base_hex, 175U);

		if (bar == NULL) {
			return -1;
		}

		eq_bar[i] = bar;
		eq_level_fp[i] = EQ_BASE_LEVEL * FP_ONE;
		eq_energy_fp[i] = EQ_BASE_LEVEL * FP_ONE;

		lv_obj_set_size(bar, bar_w, slot_h);
		lv_obj_set_pos(bar, side_pad + (i * (bar_w + gap)), top_pad);
		lv_bar_set_range(bar, 0, EQ_MAX_LEVEL);
		lv_bar_set_value(bar, EQ_BASE_LEVEL, LV_ANIM_OFF);

		lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
		lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(bar, LV_OPA_70, LV_PART_MAIN);
		lv_obj_set_style_bg_color(bar, lv_color_hex(slot_hex), LV_PART_MAIN);

		lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(bar, lv_color_hex(top_hex), LV_PART_INDICATOR);
		lv_obj_set_style_bg_grad_color(bar, lv_color_hex(bot_hex), LV_PART_INDICATOR);
		lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
	}

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
			step = (delta * 176) >> 8;
			if (step < (FP_ONE / 3)) {
				step = FP_ONE / 3;
			}
			if (step > (FP_ONE * 3)) {
				step = FP_ONE * 3;
			}
			eq_level_fp[i] += step;
			if (eq_level_fp[i] > eq_energy_fp[i]) {
				eq_level_fp[i] = eq_energy_fp[i];
			}
		} else {
			step = ((-delta) * 46) >> 8;
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

		lv_bar_set_value(eq_bar[i], (int32_t)(eq_level_fp[i] >> FP_SHIFT), LV_ANIM_OFF);
	}

	lv_obj_invalidate(lv_screen_active());
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
