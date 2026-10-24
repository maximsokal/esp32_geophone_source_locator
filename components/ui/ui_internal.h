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
#include "ui_objects_delete_model.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "storage_service.h"
#include "ui_theme.h"
#include "ui_i18n.h"
#include "ui_measurements_nodes.h"
#include "ui_node_link_status.h"


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM,
    UI_SCREEN_SCAN_PREVIEW,
    UI_SCREEN_WORKFLOW,
    UI_SCREEN_COUNT

} ui_screen_t;


#define UI_HOME_SCREEN_COUNT 5U


typedef enum
{
    UI_OBJECTS_MENU = 0,
    UI_OBJECTS_SELECT_ACTIVE,
    UI_OBJECTS_SELECT_DELETE,
    UI_OBJECTS_SELECT_WORKFLOW,
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
    ui_i18n_key_t home_label;

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
    lv_obj_t *footer_label;

    ui_screen_t screen;
    size_t selected;
    bool home_required_missing;

    lv_obj_t *home_rows[UI_HOME_SCREEN_COUNT];

    lv_obj_t *measurement_channel_labels[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_link_labels[
        SEISMIC_CHANNEL_COUNT
    ];
    nrf_node_link_status_t measurement_link_status[
        SEISMIC_CHANNEL_COUNT
    ];
    bool measurement_link_status_valid[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_charts[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_screen;
    lv_obj_t *measurement_content;
    lv_obj_t *measurement_graphs;
    lv_chart_series_t *measurement_min_series[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_chart_series_t *measurement_max_series[
        SEISMIC_CHANNEL_COUNT
    ];
    lv_obj_t *measurement_summary;
    lv_obj_t *measurement_link_legend;
    lv_obj_t *measurement_transport;
    lv_obj_t *measurement_health;
    lv_obj_t *measurement_node_labels[UI_MEASUREMENTS_NODE_COUNT];
    size_t measurement_action;
    bool measurement_exit_after_stop;

    lv_obj_t *system_heap;
    lv_obj_t *system_psram;
    lv_obj_t *system_uptime;
    ui_locale_t system_language_selection;
    bool system_hardware_open;
    bool system_hardware_selected;

    ui_objects_mode_t objects_mode;
    size_t object_action;
    size_t object_index;
    size_t object_count;
    bool objects_loaded;
    bool workflow_memory_error;
    bool workflow_activation_pending;
    size_t workflow_object_index;
    bool objects_workflow_picker_on_open;
    ui_objects_delete_model_t object_delete_model;
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

    char notice[128];

} ui_context_t;


ui_context_t *ui_context(void);


/* LVGL/esp_lcd bridge lifecycle. */
esp_err_t ui_lvgl_init(void);
void ui_lvgl_deinit(void);
void ui_lvgl_set_measurement_load_active(bool load_active);


/* Shared shell/layout primitives. Caller holds the LVGL port lock. */
void ui_render_boot_locked(
    const char *stage
);

void ui_render_boot_error_locked(
    const char *message
);

void ui_render_boot_recovery_locked(
    const char *reason,
    const char *technical_code,
    const char *warning,
    const char *primary_hint,
    const char *retry_hint,
    const char *cancel_hint
);

void ui_render_boot_format_progress_locked(unsigned percent);

void ui_render_boot_recovery_result_locked(
    bool success,
    bool formatted,
    const char *error_name
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
void ui_home_refresh_locked(void);
void ui_render_measurements_locked(void);
void ui_measurements_refresh_locked(void);
void ui_render_objects_locked(void);
void ui_render_storage_locked(void);
void ui_render_wifi_locked(void);
void ui_render_system_locked(void);
void ui_system_refresh_locked(void);
void ui_system_handle_key_locked(keypad_key_t key);
void ui_render_hardware_locked(void);
void ui_system_handle_key_locked(keypad_key_t key);
void ui_workflow_screen_open_locked(void);
void ui_workflow_screen_close_locked(void);
void ui_workflow_screen_render_locked(void);
void ui_workflow_screen_refresh_locked(void);
void ui_workflow_screen_handle_key_locked(keypad_key_t key);
void ui_scan_preview_open_locked(const char *object_id, const char *session_id);
void ui_scan_preview_open_from_workflow_locked(const char *object_id, const char *session_id);
void ui_scan_preview_screen_open_locked(void);
void ui_scan_preview_close_locked(void);
void ui_scan_preview_release_locked(void);
void ui_scan_preview_render_locked(void);
void ui_scan_preview_refresh_locked(void);
void ui_scan_preview_handle_key_locked(keypad_key_t key);


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

void ui_storage_query_gate_open(void);
void ui_storage_query_gate_close_and_wait(void);
bool ui_storage_query_begin(void);
void ui_storage_query_end(void);

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
esp_err_t ui_event_dispatcher_queue_locale_save(ui_locale_t locale);


/* Async service callbacks. */
void ui_object_service_event(
    const object_service_event_t *event,
    void *user_context
);

void ui_storage_service_event(
    const storage_service_event_t *event,
    void *user_context
);
