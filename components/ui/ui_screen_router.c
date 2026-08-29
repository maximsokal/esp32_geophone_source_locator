#include "ui_screen_router.h"

#include <stddef.h>

#include "ui_internal.h"


typedef esp_err_t (*ui_screen_action_fn)(void);
typedef esp_err_t (*ui_screen_key_fn)(keypad_key_t key);


typedef struct
{
    const char *name;
    ui_screen_action_fn open;
    ui_screen_action_fn draw;
    ui_screen_key_fn handle_key;

} ui_screen_descriptor_t;


static esp_err_t ui_screen_read_only_handle_key(
    keypad_key_t key
)
{
    return
        key == KEYPAD_KEY_LEFT
        ? ESP_ERR_NOT_SUPPORTED
        : ESP_OK;
}


static const ui_screen_descriptor_t s_screens[
    UI_SCREEN_COUNT
] = {
    [UI_SCREEN_HOME] = {
        .name = "Home",
        .open = ui_home_controller_open,
        .draw = ui_home_controller_draw,
        .handle_key = ui_home_controller_handle_key,
    },

    [UI_SCREEN_OBJECTS] = {
        .name = "Objects",
        .open = ui_objects_open,
        .draw = ui_objects_draw,
        .handle_key = ui_objects_handle_key,
    },

    [UI_SCREEN_MEASUREMENTS] = {
        .name = "Measurements",
        .open = NULL,
        .draw = ui_measurements_draw,
        .handle_key = ui_screen_read_only_handle_key,
    },

    [UI_SCREEN_STORAGE] = {
        .name = "Storage",
        .open = ui_storage_open,
        .draw = ui_storage_draw,
        .handle_key = ui_storage_handle_key,
    },

    [UI_SCREEN_WIFI] = {
        .name = "Wi-Fi",
        .open = ui_wifi_controller_open,
        .draw = ui_wifi_controller_draw,
        .handle_key = ui_wifi_controller_handle_key,
    },

    [UI_SCREEN_SYSTEM] = {
        .name = "System",
        .open = NULL,
        .draw = ui_system_draw,
        .handle_key = ui_screen_read_only_handle_key,
    },
};


static const ui_screen_descriptor_t *ui_screen_descriptor(
    ui_screen_t screen
)
{
    if (
        (int)screen < 0 ||
        screen >= UI_SCREEN_COUNT
    ) {
        return NULL;
    }

    return
        &s_screens[screen];
}


const char *ui_screen_name(
    ui_screen_t screen
)
{
    const ui_screen_descriptor_t *descriptor =
        ui_screen_descriptor(
            screen
        );

    if (
        descriptor == NULL ||
        descriptor->name == NULL
    ) {
        return "Unknown";
    }

    return
        descriptor->name;
}


esp_err_t ui_screen_open(
    ui_screen_t screen
)
{
    const ui_screen_descriptor_t *descriptor =
        ui_screen_descriptor(
            screen
        );

    if (
        descriptor == NULL ||
        descriptor->draw == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Measurements and System intentionally have no dedicated
     * open hook. Preserve the legacy behavior by drawing them
     * directly when opened.
     */
    if (descriptor->open == NULL) {
        return
            descriptor->draw();
    }

    return
        descriptor->open();
}


esp_err_t ui_screen_draw(
    ui_screen_t screen
)
{
    const ui_screen_descriptor_t *descriptor =
        ui_screen_descriptor(
            screen
        );

    if (
        descriptor == NULL ||
        descriptor->draw == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    return
        descriptor->draw();
}


esp_err_t ui_screen_handle_key(
    ui_screen_t screen,
    keypad_key_t key
)
{
    const ui_screen_descriptor_t *descriptor =
        ui_screen_descriptor(
            screen
        );

    if (
        descriptor == NULL ||
        descriptor->handle_key == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    return
        descriptor->handle_key(
            key
        );
}
