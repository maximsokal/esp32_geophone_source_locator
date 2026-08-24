#include "keypad.h"

#include <stdbool.h>

#include "board_config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "KEYPAD";


#define KEYPAD_SAMPLE_COUNT        16
#define KEYPAD_POLL_PERIOD_MS      20
#define KEYPAD_DEBOUNCE_COUNT      3
#define KEYPAD_TASK_STACK_SIZE     8192
#define KEYPAD_TASK_PRIORITY       4


/*
 * KS0466 calibration measured on this device at 3.3 V:
 *
 * Released : RAW = 4095, voltage = 3131 mV
 * SW1      : RAW =    2, voltage =    1 mV
 * SW2      : RAW =  550, voltage =  476 mV
 * SW3      : RAW = 1261, voltage = 1059 mV
 * SW4      : RAW = 1949, voltage = 1624 mV
 * SW5      : RAW = 2873, voltage = 2372 mV
 *
 * Physical button mapping:
 *
 *              SW2
 *               UP
 *
 *     SW1                 SW4
 *     LEFT               RIGHT
 *
 *              SW3
 *              DOWN
 *
 *                         SW5
 *                          OK
 *
 * Thresholds are placed approximately halfway between
 * adjacent measured RAW values.
 */
#define KEYPAD_SW1_MAX_RAW         275
#define KEYPAD_SW2_MAX_RAW         905
#define KEYPAD_SW3_MAX_RAW         1605
#define KEYPAD_SW4_MAX_RAW         2411
#define KEYPAD_SW5_MAX_RAW         3484


static adc_oneshot_unit_handle_t s_adc_handle = NULL;

static adc_unit_t s_adc_unit;
static adc_channel_t s_adc_channel;


static TaskHandle_t s_event_task = NULL;

static keypad_event_callback_t s_event_callback = NULL;
static void *s_event_user_context = NULL;

static volatile bool s_event_task_running = false;
static bool s_initialized = false;


/*
 * Read and average several ADC samples.
 *
 * Averaging reduces small fluctuations caused by ADC noise
 * and the resistor ladder inside the KS0466 module.
 */
static esp_err_t keypad_read_raw(int *raw_value)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (raw_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_sum = 0;

    for (int i = 0; i < KEYPAD_SAMPLE_COUNT; i++) {

        int raw = 0;

        esp_err_t err = adc_oneshot_read(
            s_adc_handle,
            s_adc_channel,
            &raw
        );

        if (err != ESP_OK) {
            return err;
        }

        raw_sum += raw;
    }

    *raw_value =
        raw_sum / KEYPAD_SAMPLE_COUNT;

    return ESP_OK;
}


/*
 * Convert a calibrated RAW ADC value into a logical key.
 */
static keypad_key_t keypad_raw_to_key(int raw)
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


esp_err_t keypad_init(void)
{
    if (s_initialized) {
        ESP_LOGW(
            TAG,
            "Keypad already initialized"
        );

        return ESP_OK;
    }

    /*
     * Convert the physical GPIO number into the matching
     * ESP32-S3 ADC unit and ADC channel.
     */
    esp_err_t err = adc_oneshot_io_to_channel(
        BOARD_KEYPAD_GPIO,
        &s_adc_unit,
        &s_adc_channel
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "GPIO%d is not a valid ADC GPIO: %s",
            BOARD_KEYPAD_GPIO,
            esp_err_to_name(err)
        );

        return err;
    }


    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };


    err = adc_oneshot_new_unit(
        &unit_config,
        &s_adc_handle
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to create ADC unit: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    const adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };


    err = adc_oneshot_config_channel(
        s_adc_handle,
        s_adc_channel,
        &channel_config
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to configure ADC channel: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    s_initialized = true;


    ESP_LOGI(
        TAG,
        "KS0466 initialized: GPIO%d, ADC%d channel %d",
        BOARD_KEYPAD_GPIO,
        (int)s_adc_unit + 1,
        (int)s_adc_channel
    );


    return ESP_OK;
}


esp_err_t keypad_read_key(keypad_key_t *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;

    esp_err_t err =
        keypad_read_raw(&raw);

    if (err != ESP_OK) {
        return err;
    }

    *key =
        keypad_raw_to_key(raw);

    return ESP_OK;
}


const char *keypad_key_name(keypad_key_t key)
{
    switch (key) {

        case KEYPAD_KEY_LEFT:
            return "LEFT";

        case KEYPAD_KEY_UP:
            return "UP";

        case KEYPAD_KEY_DOWN:
            return "DOWN";

        case KEYPAD_KEY_RIGHT:
            return "RIGHT";

        case KEYPAD_KEY_OK:
            return "OK";

        case KEYPAD_KEY_NONE:
        default:
            return "NONE";
    }
}


/*
 * FreeRTOS task responsible for:
 *
 * 1. Reading the analog keypad.
 * 2. Debouncing the detected key.
 * 3. Generating exactly one callback per physical key press.
 *
 * Holding a key does not repeatedly generate callbacks.
 * Another event can only occur after the key is released
 * and pressed again.
 */
static void keypad_event_task(void *arg)
{
    (void)arg;


    keypad_key_t stable_key =
        KEYPAD_KEY_NONE;

    keypad_key_t candidate_key =
        KEYPAD_KEY_NONE;

    int candidate_count = 0;


    ESP_LOGI(
        TAG,
        "Key event monitor started"
    );


    while (s_event_task_running) {

        keypad_key_t current_key =
            KEYPAD_KEY_NONE;


        esp_err_t err =
            keypad_read_key(&current_key);


        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "Failed to read keypad: %s",
                esp_err_to_name(err)
            );

            vTaskDelay(
                pdMS_TO_TICKS(
                    KEYPAD_POLL_PERIOD_MS
                )
            );

            continue;
        }


        /*
         * The same candidate state must be detected several
         * consecutive times before it becomes stable.
         */
        if (current_key == candidate_key) {

            candidate_count++;
        }
        else {

            candidate_key =
                current_key;

            candidate_count = 1;
        }


        /*
         * Process the state only after debounce is complete
         * and the stable state actually changed.
         */
        if (
            candidate_count >= KEYPAD_DEBOUNCE_COUNT &&
            candidate_key != stable_key
        ) {

            stable_key =
                candidate_key;

            candidate_count = 0;


            if (stable_key != KEYPAD_KEY_NONE) {

                ESP_LOGI(
                    TAG,
                    "Key pressed: %s",
                    keypad_key_name(
                        stable_key
                    )
                );


                /*
                 * Deliver exactly one application event
                 * for this physical key press.
                 */
                if (s_event_callback != NULL) {

                    s_event_callback(
                        stable_key,
                        s_event_user_context
                    );
                }
            }
            else {

                ESP_LOGI(
                    TAG,
                    "Key released"
                );
            }
        }


        vTaskDelay(
            pdMS_TO_TICKS(
                KEYPAD_POLL_PERIOD_MS
            )
        );
    }


    ESP_LOGI(
        TAG,
        "Key event monitor stopped"
    );


    /*
     * Mark the task as stopped before deleting it.
     * keypad_stop_event_monitor() waits for this state.
     */
    s_event_task = NULL;


    vTaskDelete(NULL);
}


esp_err_t keypad_start_event_monitor(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    if (s_event_task != NULL) {

        ESP_LOGW(
            TAG,
            "Key event monitor already running"
        );

        return ESP_OK;
    }


    s_event_callback =
        callback;

    s_event_user_context =
        user_context;

    s_event_task_running =
        true;


    BaseType_t result = xTaskCreate(
        keypad_event_task,
        "keypad_events",
        KEYPAD_TASK_STACK_SIZE,
        NULL,
        KEYPAD_TASK_PRIORITY,
        &s_event_task
    );


    if (result != pdPASS) {

        s_event_task = NULL;

        s_event_task_running =
            false;

        s_event_callback =
            NULL;

        s_event_user_context =
            NULL;


        ESP_LOGE(
            TAG,
            "Failed to create keypad event task"
        );


        return ESP_ERR_NO_MEM;
    }


    return ESP_OK;
}


void keypad_stop_event_monitor(void)
{
    if (s_event_task == NULL) {
        return;
    }


    /*
     * Ask the keypad task to terminate itself cleanly.
     */
    s_event_task_running =
        false;


    /*
     * Wait until the keypad task has fully stopped.
     *
     * This is important during safe shutdown because the
     * keypad callback may currently be drawing to the TFT.
     * The display must not be cleared or deinitialized until
     * that operation has finished.
     */
    while (s_event_task != NULL) {

        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }


    s_event_callback =
        NULL;

    s_event_user_context =
        NULL;
}