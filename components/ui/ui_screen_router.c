#include "ui_screen_router.h"

#include "ui_internal.h"


const char *ui_screen_name(
    ui_screen_t screen
)
{
    switch (screen) {
        case UI_SCREEN_HOME:
            return "Home";

        case UI_SCREEN_OBJECTS:
            return "Objects";

        case UI_SCREEN_MEASUREMENTS:
            return "Measurements";

        case UI_SCREEN_STORAGE:
            return "Storage";

        case UI_SCREEN_WIFI:
            return "Wi-Fi";

        case UI_SCREEN_SYSTEM:
            return "System";

        case UI_SCREEN_COUNT:
        default:
            return "Unknown";
    }
}


esp_err_t ui_screen_open(
    ui_screen_t screen
)
{
    switch (screen) {
        case UI_SCREEN_HOME:
            return
                ui_home_controller_open();

        case UI_SCREEN_OBJECTS:
            return
                ui_objects_open();

        case UI_SCREEN_STORAGE:
            return
                ui_storage_open();

        case UI_SCREEN_WIFI:
            return
                ui_wifi_controller_open();

        case UI_SCREEN_MEASUREMENTS:
        case UI_SCREEN_SYSTEM:
            return
                ui_screen_draw(
                    screen
                );

        case UI_SCREEN_COUNT:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}


esp_err_t ui_screen_draw(
    ui_screen_t screen
)
{
    switch (screen) {
        case UI_SCREEN_HOME:
            return
                ui_home_controller_draw();

        case UI_SCREEN_OBJECTS:
            return
                ui_objects_draw();

        case UI_SCREEN_MEASUREMENTS:
            return
                ui_measurements_draw();

        case UI_SCREEN_STORAGE:
            return
                ui_storage_draw();

        case UI_SCREEN_WIFI:
            return
                ui_wifi_controller_draw();

        case UI_SCREEN_SYSTEM:
            return
                ui_system_draw();

        case UI_SCREEN_COUNT:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}


esp_err_t ui_screen_handle_key(
    ui_screen_t screen,
    keypad_key_t key
)
{
    switch (screen) {
        case UI_SCREEN_HOME:
            return
                ui_home_controller_handle_key(
                    key
                );

        case UI_SCREEN_OBJECTS:
            return
                ui_objects_handle_key(
                    key
                );

        case UI_SCREEN_STORAGE:
            return
                ui_storage_handle_key(
                    key
                );

        case UI_SCREEN_WIFI:
            return
                ui_wifi_controller_handle_key(
                    key
                );

        case UI_SCREEN_MEASUREMENTS:
        case UI_SCREEN_SYSTEM:
            return
                key == KEYPAD_KEY_LEFT
                ? ESP_ERR_NOT_SUPPORTED
                : ESP_OK;

        case UI_SCREEN_COUNT:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}
