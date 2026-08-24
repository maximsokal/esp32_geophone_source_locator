#include "display_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static SemaphoreHandle_t s_access_mutex =
    NULL;


static esp_lcd_panel_handle_t s_panel_handle =
    NULL;


esp_err_t display_transfer_init(void)
{
    if (s_access_mutex != NULL) {
        return ESP_OK;
    }

    s_access_mutex =
        xSemaphoreCreateRecursiveMutex();

    return
        s_access_mutex != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM;
}


void display_transfer_deinit(void)
{
    s_panel_handle =
        NULL;

    if (s_access_mutex != NULL) {
        vSemaphoreDelete(
            s_access_mutex
        );

        s_access_mutex =
            NULL;
    }
}


esp_err_t display_transfer_lock(void)
{
    if (s_access_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        xSemaphoreTakeRecursive(
            s_access_mutex,
            portMAX_DELAY
        ) == pdTRUE
            ? ESP_OK
            : ESP_FAIL;
}


void display_transfer_unlock(void)
{
    if (s_access_mutex == NULL) {
        return;
    }

    xSemaphoreGiveRecursive(
        s_access_mutex
    );
}


void display_transfer_set_panel(
    esp_lcd_panel_handle_t panel_handle
)
{
    s_panel_handle =
        panel_handle;
}


esp_lcd_panel_handle_t display_transfer_panel(void)
{
    return
        s_panel_handle;
}


bool display_transfer_has_panel(void)
{
    return
        s_panel_handle != NULL;
}
