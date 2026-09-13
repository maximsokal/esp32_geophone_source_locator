#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef int esp_err_t;

#define ESP_OK 0


typedef enum
{
    KEYPAD_KEY_NONE = 0,
    KEYPAD_KEY_LEFT,
    KEYPAD_KEY_UP,
    KEYPAD_KEY_DOWN,
    KEYPAD_KEY_RIGHT,
    KEYPAD_KEY_OK

} keypad_key_t;


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
    ui_screen_t screen;
    size_t selected;
    bool storage_erase_confirm;
    ui_wifi_confirm_t wifi_confirm;
    ui_objects_mode_t objects_mode;
    size_t object_action;
    size_t object_index;
    size_t object_count;
    bool objects_loaded;
    size_t measurement_action;
    size_t storage_action;
    size_t wifi_action;
    bool wifi_setup_page_opened;
    bool wifi_credentials_saved;
    bool wifi_ca_downloaded;
    bool wifi_ca_install_confirmed;
    bool wifi_verify_ready;
    bool wifi_verify_result_received;
    uint32_t wifi_verify_nonce;
    uint32_t wifi_verify_result_mask;

} ui_context_t;


ui_context_t *ui_context(void);
void ui_set_notice_locked(const char *text);
void ui_refresh_header_locked(void);
void ui_render_home_locked(void);
void ui_render_objects_locked(void);
void ui_render_measurements_locked(void);
void ui_render_storage_locked(void);
void ui_render_wifi_locked(void);
void ui_render_system_locked(void);
void ui_measurements_refresh_locked(void);
void ui_system_refresh_locked(void);
void ui_handle_objects_key_locked(keypad_key_t key);
void ui_handle_measurements_key_locked(keypad_key_t key);
void ui_handle_storage_key_locked(keypad_key_t key);
void ui_handle_wifi_key_locked(keypad_key_t key);
esp_err_t object_service_list_async(void);
esp_err_t storage_service_info_async(void);

const ui_screen_driver_t *ui_screen_driver(ui_screen_t screen);
size_t ui_screen_home_count(void);
ui_screen_t ui_screen_home_at(size_t index);
const char *ui_screen_home_label(size_t index);
void ui_screen_open_locked(ui_screen_t screen);
void ui_screen_render_locked(ui_screen_t screen);
void ui_screen_handle_key_locked(ui_screen_t screen, keypad_key_t key);
void ui_screen_refresh_locked(ui_screen_t screen);
void ui_move_index(size_t *value, size_t count, bool down);
void ui_dispatch_key_locked(keypad_key_t key);
void ui_dispatch_app_event_locked(int event_type, int32_t event_value);
