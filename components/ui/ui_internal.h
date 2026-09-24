#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_err.h"
#include "lvgl.h"

#include "keypad.h"
#include "measurement_service.h"
#include "object_service.h"
#include "sd_card_info.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "storage_service.h"
#include "ui_theme.h"


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM,
    UI_SCREEN_COUNT

} ui_screen_t;

#define UI_HOME_SCREEN_COUNT ((size_t)UI_SCREEN_COUNT - 1U)


typedef enum
{
    UI_OBJECTS_MENU = 0,
    UI_OBJECTS_SELECT_ACTIVE,
    UI_OBJECTS_SELECT_DELETE,
    UI_OBJECTS_DELETE_CONFIRM

} ui_objects_mode_t;


typedef enum
{
    UI_WIFI_CONFIRM_NONE = 0,
    UI_WIFI_CONFIRM_SETUP,
    UI_WIFI_CONFIRM_FORGET

} ui_wifi_confirm_t;


typedef struct
{
    const char *name;
    const char *home_label;

    void (*open)(void);
    void (*render)(void);
    void (*handle_key)(keypad_key_t key);
    void (*refresh)(void);

} ui_screen_driver_t;


typedef struct
{
    bool port_ready;
    atomic_bool runtime_ready;

    lv_display_t *display;
    lv_obj_t *root;
    lv_timer_t *refresh_timer;

    lv_obj_t *header_source;
    lv_obj_t *header_wifi;
    /* MAX-182: battery/USB badge, refreshed with the header. */
    lv_obj_t *header_power;

    ui_screen_t screen;
    size_t selected;

    lv_obj_t *home_rows[UI_HOME_SCREEN_COUNT];

    lv_obj_t *measurement_channel_labels[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_charts[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_screen;
    lv_obj_t *measurement_graphs;
    lv_chart_series_t *measurement_min_series[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_chart_series_t *measurement_max_series[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_summary;
    lv_obj_t *measurement_transport;
    lv_obj_t *measurement_health;
    size_t measurement_action;
    bool measurement_exit_after_stop;

    lv_obj_t *system_heap;
    lv_obj_t *system_psram;
    lv_obj_t *system_uptime;

    ui_objects_mode_t objects_mode;
    size_t object_action;
    size_t object_index;
    size_t object_count;
    bool objects_loaded;
    object_service_summary_t objects[
        OBJECT_SERVICE_MAX_OBJECTS
    ];

    size_t storage_action;
    bool storage_erase_confirm;
    bool storage_info_valid;
    sd_card_info_t storage_info;

    size_t wifi_action;
    ui_wifi_confirm_t wifi_confirm;
    bool wifi_setup_page_opened;
    bool wifi_credentials_saved;
    bool wifi_ca_downloaded;
    bool wifi_ca_install_confirmed;
    bool wifi_verify_ready;
    bool wifi_verify_result_received;
    uint32_t wifi_verify_nonce;
    uint32_t wifi_verify_result_mask;

    bool storage_recovery_active;
    bool storage_recovery_failed;

    char notice[80];

} ui_context_t;


ui_context_t *ui_context(void);


/* LVGL/esp_lcd bridge lifecycle. */
esp_err_t ui_lvgl_init(void);
void ui_lvgl_deinit(void);


/* Shared shell/layout primitives. Caller holds the LVGL port lock. */
void ui_render_boot_locked(
    const char *stage
);

void ui_render_boot_error_locked(
    const char *message
);

void ui_render_current_locked(void);
void ui_refresh_dynamic_locked(void);
void ui_refresh_header_locked(void);

lv_obj_t *ui_create_label_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
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
    bool selected,
    bool enabled
);

void ui_set_notice_locked(
    const char *text
);


/* Screen registry: the single source of truth for screen behavior. */
const ui_screen_driver_t *ui_screen_driver(
    ui_screen_t screen
);

size_t ui_screen_home_count(void);

ui_screen_t ui_screen_home_at(
    size_t index
);

const char *ui_screen_home_label(
    size_t index
);

void ui_screen_open_locked(
    ui_screen_t screen
);

void ui_screen_render_locked(
    ui_screen_t screen
);

void ui_screen_handle_key_locked(
    ui_screen_t screen,
    keypad_key_t key
);

void ui_screen_refresh_locked(
    ui_screen_t screen
);


/* Screen renderers. */
void ui_render_home_locked(void);
void ui_render_measurements_locked(void);
void ui_measurements_refresh_locked(void);
void ui_render_objects_locked(void);
void ui_render_storage_locked(void);
void ui_render_wifi_locked(void);
void ui_render_system_locked(void);
void ui_system_refresh_locked(void);


/* Navigation/input policy. */
void ui_open_home_locked(void);

void ui_move_index(
    size_t *value,
    size_t count,
    bool down
);

void ui_handle_measurements_key_locked(
    keypad_key_t key
);

void ui_handle_objects_key_locked(
    keypad_key_t key
);

void ui_handle_storage_key_locked(
    keypad_key_t key
);

void ui_handle_wifi_key_locked(
    keypad_key_t key
);

void ui_dispatch_key_locked(
    keypad_key_t key
);

void ui_dispatch_app_event_locked(
    int event_type,
    int32_t event_value
);


/* Application event subscriber. */
esp_err_t ui_event_dispatcher_start(void);
void ui_event_dispatcher_stop(void);
/* Caller is the LVGL timer task and already holds the LVGL port lock. */
void ui_event_dispatcher_dispatch_pending_locked(void);


/* Async service callbacks. */
void ui_object_service_event(
    const object_service_event_t *event,
    void *user_context
);

void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
);
