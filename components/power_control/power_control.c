#include "power_control.h"

#include "board_config.h"

#include "esp_log.h"

#include "power_output.h"


typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);

esp_err_t power_button_adapter_init(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
);
esp_err_t power_button_adapter_arm_after_startup(void);
esp_err_t power_button_adapter_set_notification_hook(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
);
void power_button_adapter_wait_for_shutdown_request(void);
bool power_button_adapter_is_shutdown_requested(void);
void power_button_adapter_stop(void);


static const char *TAG =
    "POWER_CONTROL";

static power_control_shutdown_notification_hook_t s_notification_hook;
static void *s_notification_context;


esp_err_t power_control_init(void)
{
    esp_err_t err =
        power_output_init();

    if (err != ESP_OK) {
        return err;
    }

    err = power_button_adapter_init(
        s_notification_hook,
        s_notification_context
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize power button: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "Initialized: OFF=GPIO%d BUTTON=GPIO%d shutdown=DISARMED_UNTIL_SAFE_BOUNDARY",
        BOARD_POWER_OFF_GPIO,
        BOARD_POWER_BUTTON_GPIO
    );

    return ESP_OK;
}


esp_err_t power_control_arm_shutdown_button(void)
{
    const esp_err_t err =
        power_button_adapter_arm_after_startup();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to arm shutdown button at safe boundary: %s",
            esp_err_to_name(err)
        );
    }

    return err;
}


esp_err_t power_control_set_shutdown_notification_hook(
    power_control_shutdown_notification_hook_t hook,
    void *user_context
)
{
    s_notification_hook = hook;
    s_notification_context = user_context;
    return power_button_adapter_set_notification_hook(hook, user_context);
}


void power_control_wait_for_shutdown_request(void)
{
    power_button_adapter_wait_for_shutdown_request();
}


bool power_control_is_shutdown_requested(void)
{
    return
        power_button_adapter_is_shutdown_requested();
}


void power_control_shutdown(void)
{
    power_button_adapter_stop();

    ESP_LOGW(
        TAG,
        "Sending OFF pulse to Pololu"
    );

    power_output_pulse_off();

    ESP_LOGW(
        TAG,
        "ESP32 still powered - probably USB power is connected"
    );
}
