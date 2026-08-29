#include "ui_internal.h"


static void ui_screen_open_home(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->storage_erase_confirm = false;
    ui_set_notice_locked(NULL);
    ui_render_home_locked();
}


static void ui_screen_open_objects(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->objects_mode = UI_OBJECTS_MENU;
    context->object_action = 0;
    context->object_index = 0;
    ui_set_notice_locked(NULL);
    ui_render_objects_locked();
}


static void ui_screen_open_measurements(void)
{
    ui_context()->selected = 0;
    ui_set_notice_locked(NULL);
    ui_render_measurements_locked();
}


static void ui_screen_open_storage(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->storage_action = 0;
    context->storage_erase_confirm = false;
    ui_set_notice_locked(NULL);

    (void)storage_service_info_async();
    ui_render_storage_locked();
}


static void ui_screen_open_wifi(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->wifi_action = 0;
    ui_set_notice_locked(NULL);
    ui_render_wifi_locked();
}


static void ui_screen_open_system(void)
{
    ui_context()->selected = 0;
    ui_set_notice_locked(NULL);
    ui_render_system_locked();
}


static void ui_screen_handle_home_key(
    keypad_key_t key
)
{
    ui_context_t *context = ui_context();
    const size_t count = ui_screen_home_count();

    if (key == KEYPAD_KEY_UP) {
        ui_move_index(&context->selected, count, false);
        ui_render_home_locked();
        return;
    }

    if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(&context->selected, count, true);
        ui_render_home_locked();
        return;
    }

    if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        ui_screen_open_locked(
            ui_screen_home_at(context->selected)
        );
    }
}


static void ui_screen_handle_back_key(
    keypad_key_t key
)
{
    if (key == KEYPAD_KEY_LEFT) {
        ui_screen_open_locked(UI_SCREEN_HOME);
    }
}


static const ui_screen_driver_t s_screens[UI_SCREEN_COUNT] = {
    [UI_SCREEN_HOME] = {
        .name = "HOME",
        .home_label = NULL,
        .open = ui_screen_open_home,
        .render = ui_render_home_locked,
        .handle_key = ui_screen_handle_home_key,
        .refresh = NULL,
    },

    [UI_SCREEN_OBJECTS] = {
        .name = "OBJECTS",
        .home_label = "OBJECTS",
        .open = ui_screen_open_objects,
        .render = ui_render_objects_locked,
        .handle_key = ui_handle_objects_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_MEASUREMENTS] = {
        .name = "MEASUREMENTS",
        .home_label = "MEASUREMENTS / AD7606",
        .open = ui_screen_open_measurements,
        .render = ui_render_measurements_locked,
        .handle_key = ui_screen_handle_back_key,
        .refresh = ui_measurements_refresh_locked,
    },

    [UI_SCREEN_STORAGE] = {
        .name = "STORAGE",
        .home_label = "STORAGE / SD CARD",
        .open = ui_screen_open_storage,
        .render = ui_render_storage_locked,
        .handle_key = ui_handle_storage_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_WIFI] = {
        .name = "WI-FI / WEB",
        .home_label = "WI-FI / WEB",
        .open = ui_screen_open_wifi,
        .render = ui_render_wifi_locked,
        .handle_key = ui_handle_wifi_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_SYSTEM] = {
        .name = "SYSTEM",
        .home_label = "SYSTEM",
        .open = ui_screen_open_system,
        .render = ui_render_system_locked,
        .handle_key = ui_screen_handle_back_key,
        .refresh = NULL,
    },
};


const ui_screen_driver_t *ui_screen_driver(
    ui_screen_t screen
)
{
    if (
        screen < UI_SCREEN_HOME ||
        screen >= UI_SCREEN_COUNT
    ) {
        return NULL;
    }

    return &s_screens[screen];
}


size_t ui_screen_home_count(void)
{
    return UI_HOME_SCREEN_COUNT;
}


ui_screen_t ui_screen_home_at(
    size_t index
)
{
    if (index >= UI_HOME_SCREEN_COUNT) {
        return UI_SCREEN_HOME;
    }

    return (ui_screen_t)(index + 1U);
}


const char *ui_screen_home_label(
    size_t index
)
{
    const ui_screen_driver_t *driver =
        ui_screen_driver(ui_screen_home_at(index));

    if (
        driver == NULL ||
        driver->home_label == NULL
    ) {
        return "--";
    }

    return driver->home_label;
}


void ui_screen_open_locked(
    ui_screen_t screen
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver == NULL ||
        driver->open == NULL
    ) {
        return;
    }

    ui_context()->screen = screen;
    driver->open();
}


void ui_screen_render_locked(
    ui_screen_t screen
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->render != NULL
    ) {
        driver->render();
    }
}


void ui_screen_handle_key_locked(
    ui_screen_t screen,
    keypad_key_t key
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->handle_key != NULL
    ) {
        driver->handle_key(key);
    }
}


void ui_screen_refresh_locked(
    ui_screen_t screen
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->refresh != NULL
    ) {
        driver->refresh();
    }
}
