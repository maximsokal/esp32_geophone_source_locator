#pragma once

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    unsigned task_priority;
    unsigned task_stack;
    int task_affinity;
    unsigned task_max_sleep_ms;
    int timer_period_ms;
} lvgl_port_cfg_t;

#define ESP_LVGL_PORT_INIT_CONFIG() ((lvgl_port_cfg_t){0})

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    unsigned buffer_size;
    bool double_buffer;
    unsigned trans_size;
    unsigned hres;
    unsigned vres;
    bool monochrome;
    lv_color_format_t color_format;
    struct { bool swap_xy; bool mirror_x; bool mirror_y; } rotation;
    struct {
        bool buff_dma; bool buff_spiram; bool sw_rotate;
        bool swap_bytes; bool full_refresh; bool direct_mode;
    } flags;
} lvgl_port_display_cfg_t;

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *config);
esp_err_t lvgl_port_deinit(void);
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *config);
esp_err_t lvgl_port_remove_disp(lv_display_t *display);
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
