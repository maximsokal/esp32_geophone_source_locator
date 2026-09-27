#pragma once

typedef struct lv_display_t lv_display_t;
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_timer_t lv_timer_t;
typedef struct lv_chart_series_t lv_chart_series_t;

#define LVGL_VERSION_MAJOR 9
#define LVGL_VERSION_MINOR 6
#define LVGL_VERSION_PATCH 0

const char *lv_label_get_text(const lv_obj_t *label);
void lv_label_set_text(lv_obj_t *label, const char *text);
