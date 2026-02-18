#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>

int main(void)
{
    const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        return 0;
    }

    display_blanking_off(display_dev);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "MIDI Visualizer");
    lv_obj_center(label);

    while (1) {
        lv_task_handler();
        k_msleep(10);
    }
}
