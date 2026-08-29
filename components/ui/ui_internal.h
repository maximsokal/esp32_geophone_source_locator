#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lvgl.h"

#include "keypad.h"
#include "object_service.h"
#include "sd_card_info.h"
#include "seismic_acquisition.h"
#include "storage_service.h"


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM

} ui_screen_t;


typedef enum
{
    UI_OBJECTS_MENU = 0,
    UI_OBJECTS_SELECT_ACTIVE,
    UI_OBJECTS_SELECT_DELETE,
    UI_OBJECTS_DELETE_CONFIRM

} ui_objects_mode_t;


typedef struct
{
    bool port_ready;
    bool runtime_ready;

    lv_display_t *display;
    lv_obj_t *root;
    lv_timer_t *refresh_timer;

    lv_obj_t *header_source;
    lv_obj_t *header_wifi;

    ui_screen_t screen;
    size_t selected;

    lv_obj_t *home_rows[5];

    lv_obj_t *measurement_channel_labels[
        SEISMIC_ACQUISITION_MAX_CHANNELS
    ];
    lv_obj_t *measurement_summary;
    lv_obj_t *measurement_transport;
    lv_obj_t *measurement_health;

    ui_objects_mode_t objects_mode;
    size_t object_action;
    size_t object_index;
    size_t object_count;
    object_service_summary_t objects[
        OBJECT_SERVICE_MAX_OBJECTS
    ];

    size_t storage_action;
    bool storage_erase_confirm;
    bool storage_info_valid;
    sd_card_info_t storage_info;

    size_t wifi_action;

    char notice[80];

} ui_context_t;


ui_context_t *ui_context(void);


esp_err_t ui_lvgl_init(void);
void ui_lvgl_deinit(void);


void ui_render_boot_locked(
    const char *stage
);

void ui_render_current_locked(void);
void ui_refresh_dynamic_locked(void);
void ui_refresh_header_locked(void);


void ui_render_home_locked(void);
void ui_render_measurements_locked(void);
void ui_measurements_refresh_locked(void);
void ui_render_objects_locked(void);
void ui_render_storage_locked(void);
void ui_render_wifi_locked(void);
void ui_render_system_locked(void);


void ui_dispatch_key_locked(
    keypad_key_t key
);

void ui_dispatch_app_event_locked(
    int event_type
);


esp_err_t ui_event_task_start(void);
void ui_event_task_stop(void);


void ui_object_service_event(
    const object_service_event_t *event,
    void *user_context
);

void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
);


lv_obj_t *ui_create_screen_base_locked(
    const char *title,
    const char *footer
);

lv_obj_t *ui_create_card_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    int height
);

lv_obj_t *ui_create_row_locked(
    lv_obj_t *parent,
    int y,
    const char *text,
    bool selected
);

void ui_set_notice_locked(
    const char *text
);
