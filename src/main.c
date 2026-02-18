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
#define EQ_UPDATE_MS 40
#define EQ_BAR_COUNT 12
#define EQ_MAX_LEVEL 100

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
static uint8_t eq_level[EQ_BAR_COUNT];
static bool status_led_ready;
static uint32_t prng_state = 0xA5A5F00DU;

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

static uint8_t next_target(uint8_t idx)
{
	uint8_t v = (uint8_t)(prng_next() % (EQ_MAX_LEVEL + 1U));

	/* Keep the center bands slightly hotter for a natural fake EQ curve. */
	if (idx > 2U && idx < (EQ_BAR_COUNT - 3U)) {
		v = (uint8_t)MIN(EQ_MAX_LEVEL, v + 12U);
	}

	return v;
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
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1119), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	lv_obj_t *title = lv_label_create(screen);
	if (title == NULL) {
		return -1;
	}

	lv_label_set_text(title, "MIDI EQ DEMO");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);
	lv_obj_set_style_text_color(title, lv_color_hex(0xD6E2F0), 0);

	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		lv_obj_t *bar = lv_bar_create(screen);
		if (bar == NULL) {
			return -1;
		}

		eq_bar[i] = bar;
		eq_level[i] = 0U;

		lv_obj_set_size(bar, bar_w, slot_h);
		lv_obj_set_pos(bar, side_pad + (i * (bar_w + gap)), top_pad);
		lv_bar_set_range(bar, 0, EQ_MAX_LEVEL);
		lv_bar_set_value(bar, 0, LV_ANIM_OFF);

		lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
		lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(bar, LV_OPA_70, LV_PART_MAIN);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0x1C2A35), LV_PART_MAIN);

		lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0x25D34F), LV_PART_INDICATOR);
		lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xB3F75F), LV_PART_INDICATOR);
		lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, LV_PART_INDICATOR);
	}

	return 0;
}

static void update_eq_frame(void)
{
	for (uint8_t i = 0; i < EQ_BAR_COUNT; i++) {
		uint8_t target = next_target(i);
		uint8_t cur = eq_level[i];

		if (target > cur) {
			uint8_t up = (uint8_t)(((target - cur) / 2U) + 6U);
			cur = (uint8_t)MIN(EQ_MAX_LEVEL, cur + up);
		} else {
			uint8_t down = (uint8_t)(((cur - target) / 6U) + 2U);
			cur = (uint8_t)((cur > down) ? (cur - down) : 0U);
		}

		eq_level[i] = cur;
		lv_bar_set_value(eq_bar[i], cur, LV_ANIM_OFF);
	}

	status_led_toggle_safe();

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

	int64_t next_eq = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		if (now >= next_eq) {
			update_eq_frame();
			next_eq = now + EQ_UPDATE_MS;
		}

		lv_timer_handler();
		k_msleep(APP_TICK_MS);
	}

	return 0;
}
