#include "keypad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "app_events.h"
#include "board_config.h"
#include "button_adc.h"
#include "iot_button.h"

#include "esp_log.h"


static const char *TAG = "KEYPAD";

#define KEYPAD_BUTTON_COUNT 5U
#define KEYPAD_DEBOUNCE_MS 60U
#define KEYPAD_SAMPLE_COUNT 16U

/* Preserve the raw thresholds from the original KS0466 implementation for
 * the compatibility read API. Button objects use the same measured points in
 * calibrated millivolts below. */
#define KEYPAD_SW1_MAX_RAW 275
#define KEYPAD_SW2_MAX_RAW 905
#define KEYPAD_SW3_MAX_RAW 1605
#define KEYPAD_SW4_MAX_RAW 2411
#define KEYPAD_SW5_MAX_RAW 3484

/* Existing KS0466 measurements, converted to the voltage ranges expected by
 * espressif/button. The boundaries are the midpoints of the measured adjacent
 * button voltages; no new electrical ranges are invented here. */
#define KEYPAD_LEFT_MIN_MV 0U
#define KEYPAD_LEFT_MAX_MV 238U
#define KEYPAD_UP_MIN_MV 239U
#define KEYPAD_UP_MAX_MV 767U
#define KEYPAD_DOWN_MIN_MV 768U
#define KEYPAD_DOWN_MAX_MV 1341U
#define KEYPAD_RIGHT_MIN_MV 1342U
#define KEYPAD_RIGHT_MAX_MV 1998U
#define KEYPAD_OK_MIN_MV 1999U
#define KEYPAD_OK_MAX_MV 2751U


typedef struct {
    keypad_key_t key;
    uint16_t min_mv;
    uint16_t max_mv;
} keypad_button_range_t;


static const keypad_button_range_t s_ranges[KEYPAD_BUTTON_COUNT] = {
    { KEYPAD_KEY_LEFT, KEYPAD_LEFT_MIN_MV, KEYPAD_LEFT_MAX_MV },
    { KEYPAD_KEY_UP, KEYPAD_UP_MIN_MV, KEYPAD_UP_MAX_MV },
    { KEYPAD_KEY_DOWN, KEYPAD_DOWN_MIN_MV, KEYPAD_DOWN_MAX_MV },
    { KEYPAD_KEY_RIGHT, KEYPAD_RIGHT_MIN_MV, KEYPAD_RIGHT_MAX_MV },
    { KEYPAD_KEY_OK, KEYPAD_OK_MIN_MV, KEYPAD_OK_MAX_MV },
};


static button_handle_t s_buttons[KEYPAD_BUTTON_COUNT];
static adc_channel_t s_adc_channel;
static bool s_initialized;


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

    for (size_t i = 0; i < KEYPAD_BUTTON_COUNT; ++i) {
        const button_config_t button_config = {
            .short_press_time = KEYPAD_DEBOUNCE_MS,
            .long_press_time = 1500U,
        };
        const button_adc_config_t adc_config = {
            .adc_handle = adc_service_handle(),
            .unit_id = adc_service_unit(),
            .adc_channel = s_adc_channel,
            .button_index = (uint8_t)i,
            .min = s_ranges[i].min_mv,
            .max = s_ranges[i].max_mv,
        };

        err = iot_button_new_adc_device(
            &button_config,
            &adc_config,
            &s_buttons[i]
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create %s button: %s",
                keypad_key_name(s_ranges[i].key), esp_err_to_name(err));
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
            (void *)(uintptr_t)s_ranges[i].key
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

    s_initialized = false;
}
