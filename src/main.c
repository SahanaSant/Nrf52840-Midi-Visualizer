#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/printk.h>
#include <lvgl.h>

#include "BTN.h"
#include "LED.h"
#include <lv_data_obj.h>

#define SLEEP_MS 1

#if DT_HAS_CHOSEN(zephyr_display)
static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#else
static const struct device *display_dev;
#endif
static lv_obj_t *screen = NULL; 

void lv_button_callback(lv_event_t *event){
  lv_obj_t *data_obj = (lv_obj_t *)lv_event_get_user_data(event); 
  led_id led = *(led_id *)lv_data_obj_get_data_ptr(data_obj); 

  LED_toggle(led); 
}

int main(void) {
#if !DT_HAS_CHOSEN(zephyr_display)
  printk("No zephyr,display chosen node in devicetree\n");
  return 0;
#endif

  if(!device_is_ready(display_dev)) {
    return 0;
  }
  screen = lv_screen_active(); 
  if(screen == NULL){
    return 0; 
  }

  if (0 > BTN_init()) {
    return 0;
  }
  if (0 > LED_init()) {
    return 0;
  }

    for (uint8_t i = 0; i < NUM_LEDS; i++){
    lv_obj_t *ui_btn = lv_button_create(screen); 
    //placing on a 2x2 grid in center, matching orientation of the LEDS
    lv_obj_align(ui_btn, LV_ALIGN_CENTER, 50 * (i % 2 ? 1 : -1), 20 * (i < 2 ? -1 : 1)); 
    lv_obj_t *button_label = lv_label_create(ui_btn); 
    char label_text[10]; 
    snprintf(label_text, 10, "LED %d", i);
    lv_label_set_text(button_label, label_text);
    lv_obj_align(button_label, LV_ALIGN_CENTER, 0, 0);  

    led_id led = (led_id)i; 
    lv_obj_t *data_obj = lv_data_obj_create_alloc_assign(ui_btn, &led, sizeof(led_id)); 
    lv_obj_add_event_cb(ui_btn, lv_button_callback, LV_EVENT_CLICKED, data_obj); 
  }

/*
  // For writing Hello World

  lv_obj_t *label = lv_label_create(screen);
  lv_label_set_text(label, "Hello World!"); 
*/


  display_blanking_off(display_dev);
  while(1) {
    for (uint8_t i = 0; i < NUM_BTNS && i < NUM_LEDS; i++) {
      if (BTN_check_clear_pressed((btn_id)i)) {
        LED_toggle((led_id)i);
      }
    }

    lv_timer_handler(); 
    k_msleep(SLEEP_MS); 

  }
  	return 0;
}
