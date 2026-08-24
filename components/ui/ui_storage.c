#include "ui_internal.h"

#include "storage_service.h"

#include "ui_storage_internal.h"


esp_err_t ui_storage_open(void)
{
    ESP_ERROR_CHECK(
        storage_service_set_event_callback(
            ui_storage_service_event,
            NULL
        )
    );

    ui_storage_state_reset();

    return
        ui_storage_draw_menu_internal();
}


esp_err_t ui_storage_draw(void)
{
    return
        ui_storage_draw_current_internal();
}


esp_err_t ui_storage_handle_key(
    keypad_key_t key
)
{
    return
        ui_storage_handle_input(
            key
        );
}
