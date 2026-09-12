#include "keypad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "app_events.h"
#include "board_config.h"
#include "button_interface.h"
#include "iot_button.h"

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG = "KEYPAD";

#define KEYPAD_BUTTON_COUNT 5U
#define KEYPAD_DEBOUNCE_MS 60U
#define KEYPAD_SAMPLE_COUNT 16U
#define KEYPAD_BUTTON_SAMPLE_COUNT 4U
#define KEYPAD_BUTTON_CACHE_WINDOW_US 1000LL

/* Preserve the raw thresholds from the original KS0466 implementation for
 * both the compatibility read API and the custom Button driver below. */
#define KEYPAD_SW1_MAX_RAW 275
#define KEYPAD_SW2_MAX_RAW 905
#define KEYPAD_SW3_MAX_RAW 1605
#define KEYPAD_SW4_MAX_RAW 2411
#define KEYPAD_SW5_MAX_RAW 3484

/* The official ADC Button driver is intentionally not used: it reads a
 * borrowed ADC1 handle directly from its esp_timer callback. */
static const keypad_key_t s_keys[KEYPAD_BUTTON_COUNT] = {
    KEYPAD_KEY_LEFT,
    KEYPAD_KEY_UP,
    KEYPAD_KEY_DOWN,
    KEYPAD_KEY_RIGHT,
    KEYPAD_KEY_OK,
};


typedef struct {
    button_driver_t base;
    keypad_key_t key;
} keypad_button_driver_t;


static button_handle_t s_buttons[KEYPAD_BUTTON_COUNT];
static keypad_button_driver_t s_button_drivers[KEYPAD_BUTTON_COUNT];
static adc_channel_t s_adc_channel;
static bool s_initialized;
static bool s_adc_cache_valid;
static int64_t s_adc_cache_time_us;
static bool s_adc_cache_attempt_valid;
static int64_t s_adc_cache_attempt_time_us;
static keypad_key_t s_adc_cache_key;


static keypad_key_t keypad_decode_raw(int raw)
{
    if (raw <= KEYPAD_SW1_MAX_RAW) {
        return KEYPAD_KEY_LEFT;
    }
    if (raw <= KEYPAD_SW2_MAX_RAW) {
        return KEYPAD_KEY_UP;
    }
    if (raw <= KEYPAD_SW3_MAX_RAW) {
        return KEYPAD_KEY_DOWN;
    }
    if (raw <= KEYPAD_SW4_MAX_RAW) {
        return KEYPAD_KEY_RIGHT;
    }
    if (raw <= KEYPAD_SW5_MAX_RAW) {
        return KEYPAD_KEY_OK;
    }
    return KEYPAD_KEY_NONE;
}


static keypad_button_driver_t *keypad_button_driver_from_base(
    button_driver_t *driver
)
{
    return (keypad_button_driver_t *)(
        (uint8_t *)driver - offsetof(keypad_button_driver_t, base)
    );
}


static uint8_t keypad_button_level_from_cache(
    const keypad_button_driver_t *button
)
{
    return s_adc_cache_valid && s_adc_cache_key == button->key
        ? BUTTON_ACTIVE
        : BUTTON_INACTIVE;
}


static uint8_t keypad_button_get_key_level(button_driver_t *driver)
{
    keypad_button_driver_t *button =
        keypad_button_driver_from_base(driver);
    const int64_t now_us = esp_timer_get_time();

    /* Button core polls all five handles in one timer callback. Share one
     * broker-owned sample across that callback so each handle sees the same
     * conversion without multiplying ADC traffic. The short window is below
     * the 10 ms button period and is never used to bypass adc_service. */
    if (
        !s_adc_cache_valid ||
        now_us - s_adc_cache_time_us > KEYPAD_BUTTON_CACHE_WINDOW_US
    ) {
        /* A failed non-blocking attempt must not be retried by the next
         * logical Button object in the same timer callback. Keep the last
         * state for this cache window and let the next window try again. */
        if (
            s_adc_cache_attempt_valid &&
            now_us - s_adc_cache_attempt_time_us <=
                KEYPAD_BUTTON_CACHE_WINDOW_US
        ) {
            return keypad_button_level_from_cache(button);
        }

        s_adc_cache_attempt_valid = true;
        s_adc_cache_attempt_time_us = now_us;

        int raw = 0;
        esp_err_t err = adc_service_try_read_average_raw(
            s_adc_channel,
            KEYPAD_BUTTON_SAMPLE_COUNT,
            &raw
        );
        if (err == ESP_ERR_TIMEOUT) {
            return keypad_button_level_from_cache(button);
        }
        if (err != ESP_OK) {
            s_adc_cache_valid = false;
            return keypad_button_level_from_cache(button);
        }

        s_adc_cache_key = keypad_decode_raw(raw);
        /* Timestamp the completed broker read, not the potentially stale
         * pre-read time. This keeps all five callbacks in this timer cycle on
         * the same sample even when a conversion itself takes time. */
        s_adc_cache_time_us = esp_timer_get_time();
        s_adc_cache_valid = true;
        s_adc_cache_attempt_valid = false;
    }

    return keypad_button_level_from_cache(button);
}


static esp_err_t keypad_button_driver_delete(button_driver_t *driver)
{
    /* Driver storage is static because iot_button_create() retains the
     * caller-owned driver pointer until iot_button_delete(). */
    (void)driver;
    return ESP_OK;
}


void keypad_button_adapter_stop(void);


static void keypad_button_callback(void *button_handle, void *user_data)
{
    (void)button_handle;

    /* No delay, storage, or application lock is allowed in this callback. */
    (void)app_events_post(
        APP_EVENT_KEYPAD,
        (int32_t)(keypad_key_t)(uintptr_t)user_data
    );
}


esp_err_t keypad_button_adapter_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = adc_service_configure_gpio(
        BOARD_KEYPAD_GPIO,
        ADC_ATTEN_DB_12,
        ADC_BITWIDTH_DEFAULT,
        &s_adc_channel
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure KS0466 ADC: %s", esp_err_to_name(err));
        return err;
    }

    s_adc_cache_valid = false;

    for (size_t i = 0; i < KEYPAD_BUTTON_COUNT; ++i) {
        const button_config_t button_config = {
            .short_press_time = KEYPAD_DEBOUNCE_MS,
            .long_press_time = 1500U,
        };
        s_button_drivers[i] = (keypad_button_driver_t) {
            .base = {
                .enable_power_save = false,
                .get_key_level = keypad_button_get_key_level,
                .enter_power_save = NULL,
                .exit_power_save = NULL,
                .get_gpio_num = NULL,
                .del = keypad_button_driver_delete,
            },
            .key = s_keys[i],
        };

        err = iot_button_create(
            &button_config,
            &s_button_drivers[i].base,
            &s_buttons[i]
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create %s button: %s",
                keypad_key_name(s_keys[i]), esp_err_to_name(err));
            keypad_button_adapter_stop();
            return err;
        }

        /* PRESS_DOWN fires once for every debounced press, including each
         * subsequent press after release: this is the old monitor's event and
         * repeat/press behavior without an app-owned polling task. */
        err = iot_button_register_cb(
            s_buttons[i],
            BUTTON_PRESS_DOWN,
            NULL,
            keypad_button_callback,
            (void *)(uintptr_t)s_keys[i]
        );
        if (err != ESP_OK) {
            keypad_button_adapter_stop();
            return err;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "KS0466 Button objects ready: GPIO%d ADC1 channel %d",
        BOARD_KEYPAD_GPIO, (int)s_adc_channel);
    return ESP_OK;
}


esp_err_t keypad_button_adapter_start(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Retain the existing startup API; Button callbacks post directly to the
     * application event abstraction so this adapter cannot duplicate events. */
    (void)callback;
    (void)user_context;
    return ESP_OK;
}


esp_err_t keypad_button_adapter_read_key(keypad_key_t *key)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t err = adc_service_read_average_raw(
        s_adc_channel,
        KEYPAD_SAMPLE_COUNT,
        &raw
    );
    if (err != ESP_OK) {
        return err;
    }

    *key = keypad_decode_raw(raw);
    return ESP_OK;
}


void keypad_button_adapter_stop(void)
{
    for (size_t i = 0; i < KEYPAD_BUTTON_COUNT; ++i) {
        if (s_buttons[i] != NULL) {
            (void)iot_button_delete(s_buttons[i]);
            s_buttons[i] = NULL;
        }
    }

    s_adc_cache_valid = false;
    s_adc_cache_attempt_valid = false;
    s_initialized = false;
}
