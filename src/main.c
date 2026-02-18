#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <lvgl.h>

#define APP_TICK_MS 5
#define UI_UPDATE_MS 50

#if DT_HAS_CHOSEN(zephyr_display)
static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#else
static const struct device *display_dev;
#endif

static lv_obj_t *bg_panel;
static lv_obj_t *counter_label;
static lv_obj_t *beat_box;
static uint32_t frame_count;

static void ui_step(void)
{
	char text[24];
	bool odd = (frame_count & 1U) != 0U;

	frame_count++;
	snprintk(text, sizeof(text), "FRAME %u", frame_count);
	lv_label_set_text(counter_label, text);
	if (odd) {
		lv_obj_align(counter_label, LV_ALIGN_TOP_LEFT, 8, 8);
	} else {
		lv_obj_align(counter_label, LV_ALIGN_TOP_RIGHT, -8, 8);
	}

	if (odd) {
		lv_obj_set_style_bg_color(bg_panel, lv_color_hex(0x000000), 0);
		lv_obj_set_style_text_color(counter_label, lv_color_hex(0xFFFFFF), 0);
		lv_obj_set_style_bg_color(beat_box, lv_color_hex(0xFFFFFF), 0);
		lv_obj_align(beat_box, LV_ALIGN_BOTTOM_LEFT, 8, -8);
	} else {
		lv_obj_set_style_bg_color(bg_panel, lv_color_hex(0xFFFFFF), 0);
		lv_obj_set_style_text_color(counter_label, lv_color_hex(0x000000), 0);
		lv_obj_set_style_bg_color(beat_box, lv_color_hex(0x000000), 0);
		lv_obj_align(beat_box, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
	}

	lv_obj_invalidate(lv_screen_active());
}

static void create_ui(void)
{
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
	lv_obj_set_style_pad_all(screen, 0, 0);

	bg_panel = lv_obj_create(screen);
	lv_obj_set_size(bg_panel, LV_PCT(100), LV_PCT(100));
	lv_obj_center(bg_panel);
	lv_obj_set_style_border_width(bg_panel, 0, 0);
	lv_obj_set_style_radius(bg_panel, 0, 0);
	lv_obj_set_style_pad_all(bg_panel, 0, 0);
	lv_obj_set_style_bg_opa(bg_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bg_panel, lv_color_hex(0x0A1F44), 0);
	lv_obj_clear_flag(bg_panel, LV_OBJ_FLAG_SCROLLABLE);

	counter_label = lv_label_create(bg_panel);
	lv_label_set_text(counter_label, "FRAME 0");
	lv_obj_set_style_text_color(counter_label, lv_color_hex(0xF7F9FF), 0);
	lv_obj_align(counter_label, LV_ALIGN_TOP_LEFT, 8, 8);

	beat_box = lv_obj_create(bg_panel);
	lv_obj_set_size(beat_box, 48, 48);
	lv_obj_set_style_border_width(beat_box, 0, 0);
	lv_obj_set_style_radius(beat_box, 0, 0);
	lv_obj_set_style_bg_opa(beat_box, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(beat_box, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(beat_box, LV_ALIGN_BOTTOM_LEFT, 8, -8);
}

int main(void)
{
#if !DT_HAS_CHOSEN(zephyr_display)
	printk("No zephyr,display chosen node in devicetree\\n");
	return 0;
#endif

	if (!device_is_ready(display_dev)) {
		printk("Display device not ready\\n");
		return 0;
	}

	create_ui();
	display_blanking_off(display_dev);

	int64_t next_update = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();
		if (now >= next_update) {
			ui_step();
			next_update = now + UI_UPDATE_MS;
		}

		lv_timer_handler();
		k_msleep(APP_TICK_MS);
	}

	return 0;
}
