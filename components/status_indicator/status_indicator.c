#include "status_indicator.h"

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "app_events.h"
#include "device_status.h"

#include "esp_event.h"
#include "esp_log.h"
#include "led_indicator.h"
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
#include "board_config.h"
#include "led_indicator_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hw_inventory.h"
#endif


static const char *TAG = "STATUS_INDICATOR";


static bool s_registered = false;
static bool s_warning_sticky = false;
static bool s_error_sticky = false;
static status_indicator_state_t s_state = STATUS_INDICATOR_NORMAL;
static atomic_bool s_backend_enabled;

#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
#define STATUS_LED_MASK ((1ULL << BOARD_STATUS_LED_RED_GPIO) | \
                         (1ULL << BOARD_STATUS_LED_YELLOW_GPIO) | \
                         (1ULL << BOARD_STATUS_LED_GREEN_GPIO))
_Static_assert(BOARD_STATUS_LED_RED_GPIO != BOARD_STATUS_LED_YELLOW_GPIO &&
               BOARD_STATUS_LED_RED_GPIO != BOARD_STATUS_LED_GREEN_GPIO &&
               BOARD_STATUS_LED_YELLOW_GPIO != BOARD_STATUS_LED_GREEN_GPIO,
               "Status LEDs must use distinct GPIOs");
_Static_assert((STATUS_LED_MASK & (
    (1ULL << 1) | /* Existing divider wiring. */
    (1ULL << BOARD_SPI_MOSI_GPIO) | (1ULL << BOARD_SPI_SCLK_GPIO) |
    (1ULL << BOARD_SPI_MISO_GPIO) | (1ULL << BOARD_TFT_CS_GPIO) |
    (1ULL << BOARD_TFT_DC_GPIO) | (1ULL << BOARD_TFT_RESET_GPIO) |
    (1ULL << BOARD_SD_CS_GPIO) | (1ULL << BOARD_SENSOR_I2C_SDA_GPIO) |
    (1ULL << BOARD_SENSOR_I2C_SCL_GPIO) | (1ULL << BOARD_AD7606_SPI_MISO_GPIO) |
    (1ULL << BOARD_AD7606_SPI_SCLK_GPIO) | (1ULL << BOARD_AD7606_CS_GPIO) |
    (1ULL << BOARD_AD7606_CONVST_GPIO) | (1ULL << BOARD_AD7606_BUSY_GPIO) |
    (1ULL << BOARD_AD7606_RESET_GPIO) | (1ULL << BOARD_POWER_OFF_GPIO) |
    (1ULL << BOARD_POWER_BUTTON_GPIO) | (1ULL << BOARD_KEYPAD_GPIO))) == 0,
    "Status LED GPIO conflicts with the board pin contract");
#undef STATUS_LED_MASK

static led_indicator_handle_t s_leds[3];
static TimerHandle_t s_health_timer;
static app_event_type_t s_last_health_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;

/*
 * Cause of the last health transition. The health timer runs on the FreeRTOS
 * "Tmr Svc" task (CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048), where a
 * formatted ESP_LOGW overflowed the stack and rebooted the device. The timer
 * therefore only records the cause and posts the event; the event handler logs
 * it on the event-loop task. The timer writes the record only while
 * s_health_log_pending is false, the handler clears the flag after copying, so
 * the two tasks never touch the record at the same time.
 */
typedef struct {
    app_event_type_t event;
    bool has_cause;
    int module_id;
    int state;
    int requirement;
    char reason[HW_INVENTORY_REASON_LEN];
} status_health_log_t;

static status_health_log_t s_health_log;
static atomic_bool s_health_log_pending;
static const int32_t s_led_pins[] = {
    BOARD_STATUS_LED_GREEN_GPIO,
    BOARD_STATUS_LED_YELLOW_GPIO,
    BOARD_STATUS_LED_RED_GPIO,
};

static void status_indicator_health_tick(TimerHandle_t timer)
{
    (void)timer;
    if (!s_backend_enabled) {
        return;
    }
    hw_module_info_t modules[HW_MODULE_COUNT];
    const size_t count = hw_inventory_snapshot(modules, HW_MODULE_COUNT);
    app_event_type_t event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
    size_t cause = count; /* module that decided the event; count = none */
    for (size_t index = 0; index < count; ++index) {
        if (modules[index].state == HW_STATE_FAULT ||
            (modules[index].requirement == HW_REQ_REQUIRED &&
             modules[index].state == HW_STATE_ABSENT)) {
            event = APP_EVENT_STATUS_ERROR;
            cause = index;
            break;
        }
        if (modules[index].state == HW_STATE_DEGRADED) {
            if (event != APP_EVENT_STATUS_WARNING) {
                cause = index;
            }
            event = APP_EVENT_STATUS_WARNING;
        }
    }
    /* Retry a full queue on the next tick; LED I/O and logging stay off this task. */
    if (event == s_last_health_event || atomic_load_explicit(&s_health_log_pending, memory_order_acquire)) {
        return;
    }
    s_health_log.event = event;
    s_health_log.has_cause = cause < count;
    if (s_health_log.has_cause) {
        s_health_log.module_id = (int)modules[cause].id;
        s_health_log.state = (int)modules[cause].state;
        s_health_log.requirement = (int)modules[cause].requirement;
        memcpy(s_health_log.reason, modules[cause].reason, sizeof(s_health_log.reason));
        s_health_log.reason[sizeof(s_health_log.reason) - 1U] = '\0';
    }
    atomic_store_explicit(&s_health_log_pending, true, memory_order_release);
    if (app_events_post_best_effort(event, 0) == ESP_OK) {
        s_last_health_event = event;
    } else {
        atomic_store_explicit(&s_health_log_pending, false, memory_order_release);
    }
}

/* Event-loop side of the health transition; see status_health_log_t. */
static void status_indicator_log_health_cause(app_event_type_t event)
{
    if (!atomic_load_explicit(&s_health_log_pending, memory_order_acquire) ||
        s_health_log.event != event) {
        return;
    }
    const status_health_log_t record = s_health_log;
    atomic_store_explicit(&s_health_log_pending, false, memory_order_release);
    if (record.has_cause) {
        ESP_LOGW(TAG, "STATUS_HEALTH event=%d module_id=%d state=%d requirement=%d reason=%s",
            (int)record.event, record.module_id, record.state, record.requirement, record.reason);
    } else {
        ESP_LOGI(TAG, "STATUS_HEALTH event=%d no faulting module", (int)record.event);
    }
}

static void status_indicator_backend_stop(void)
{
    s_backend_enabled = false;
    atomic_store_explicit(&s_health_log_pending, false, memory_order_release);
    if (s_health_timer != NULL) {
        if (xTimerDelete(s_health_timer, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "LED health timer release failed");
        }
        s_health_timer = NULL;
    }
    for (size_t index = 0; index < 3; ++index) {
        if (s_leds[index] != NULL) {
            (void)led_indicator_set_on_off(s_leds[index], false);
            const esp_err_t err = led_indicator_delete(s_leds[index]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LED GPIO%d release failed: %d", (int)s_led_pins[index], (int)err);
            }
            s_leds[index] = NULL;
        }
    }
}

static esp_err_t status_indicator_backend_apply(void)
{
    if (!s_backend_enabled) {
        return ESP_OK;
    }
    /* Turn inactive colors off first, so transitions never leave two LEDs on. */
    for (size_t index = 0; index < 3; ++index) {
        if (index != (size_t)s_state) {
            const esp_err_t err = led_indicator_set_on_off(s_leds[index], false);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return led_indicator_set_on_off(s_leds[s_state], true);
}

static esp_err_t status_indicator_backend_init(void)
{
    const led_indicator_config_t config = {0};
    for (size_t index = 0; index < 3; ++index) {
        const led_indicator_gpio_config_t gpio_config = {
            .gpio_num = s_led_pins[index],
            /* Assumed GPIO -> resistor -> LED -> GND; physical check pending. */
            .is_active_level_high = true,
        };
        const esp_err_t err = led_indicator_new_gpio_device(&config, &gpio_config, &s_leds[index]);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (size_t index = 0; index < 3; ++index) {
        esp_err_t err = led_indicator_set_on_off(s_leds[index], true);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(75));
        err = led_indicator_set_on_off(s_leds[index], false);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_backend_enabled = true;
    atomic_store_explicit(&s_health_log_pending, false, memory_order_release);
    s_last_health_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
    s_health_timer = xTimerCreate("status_health", pdMS_TO_TICKS(1000), pdTRUE,
        NULL, status_indicator_health_tick);
    if (s_health_timer == NULL || xTimerStart(s_health_timer, 0) != pdPASS) {
        return ESP_FAIL;
    }
    return status_indicator_backend_apply();
}
#endif


static status_indicator_state_t status_indicator_effective_state(void)
{
    if (s_error_sticky) {
        return STATUS_INDICATOR_ERROR;
    }

    if (s_warning_sticky) {
        return STATUS_INDICATOR_WARNING;
    }

    return STATUS_INDICATOR_NORMAL;
}


static void status_indicator_apply(app_status_event_t event)
{
    switch (event) {
        case APP_STATUS_WARNING:
            s_warning_sticky = true;
            break;

        case APP_STATUS_ERROR:
            s_error_sticky = true;
            break;

        case APP_STATUS_CLEAR_NON_STICKY:
        case APP_STATUS_NORMAL:
        default:
            /* Sticky states survive until the next power cycle. */
            break;
    }

    s_state = status_indicator_effective_state();
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    const esp_err_t err = status_indicator_backend_apply();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED state apply failed: %d", (int)err);
        status_indicator_backend_stop();
    }
#endif
}


static void status_indicator_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_base;
    (void)event_data;

#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    status_indicator_log_health_cause((app_event_type_t)event_id);
#endif

    switch ((app_event_type_t)event_id) {
        case APP_EVENT_STATUS_WARNING:
            status_indicator_apply(APP_STATUS_WARNING);
            break;

        case APP_EVENT_STATUS_ERROR:
            status_indicator_apply(APP_STATUS_ERROR);
            break;

        case APP_EVENT_STATUS_CLEAR_NON_STICKY:
            status_indicator_apply(APP_STATUS_CLEAR_NON_STICKY);
            break;

        default:
            return;
    }

    ESP_LOGI(TAG, "status=%d warning_sticky=%d error_sticky=%d",
        (int)s_state,
        s_warning_sticky ? 1 : 0,
        s_error_sticky ? 1 : 0);
}


esp_err_t status_indicator_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    esp_event_loop_handle_t loop = app_events_get_loop();
    if (loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_event_handler_register_with(
        loop,
        APP_EVENT,
        ESP_EVENT_ANY_ID,
        status_indicator_event_handler,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    s_state = status_indicator_effective_state();
    s_registered = true;

#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    err = status_indicator_backend_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED backend init failed: %d", (int)err);
        status_indicator_stop();
        return err;
    }
    ESP_LOGI(TAG, "LED backend ready red=%d yellow=%d green=%d active_high=1 lamp_test=complete",
        (int)BOARD_STATUS_LED_RED_GPIO, (int)BOARD_STATUS_LED_YELLOW_GPIO,
        (int)BOARD_STATUS_LED_GREEN_GPIO);
#else
    ESP_LOGI(TAG, "LED backend disabled by configuration");
#endif

    return ESP_OK;
}


void status_indicator_stop(void)
{
    if (!s_registered) {
        return;
    }

    esp_event_loop_handle_t loop = app_events_get_loop();
    if (loop != NULL) {
        (void)esp_event_handler_unregister_with(
            loop,
            APP_EVENT,
            ESP_EVENT_ANY_ID,
            status_indicator_event_handler
        );
    }

    s_registered = false;
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    status_indicator_backend_stop();
#endif
}


esp_err_t status_indicator_post(app_status_event_t event)
{
    app_event_type_t app_event;

    switch (event) {
        case APP_STATUS_WARNING:
            app_event = APP_EVENT_STATUS_WARNING;
            break;
        case APP_STATUS_ERROR:
            app_event = APP_EVENT_STATUS_ERROR;
            break;
        case APP_STATUS_CLEAR_NON_STICKY:
            app_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
            break;
        case APP_STATUS_NORMAL:
            /* Normal is an explicit non-sticky input; it never clears latches. */
            app_event = APP_EVENT_STATUS_CLEAR_NON_STICKY;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return app_events_post(app_event, 0);
}


status_indicator_state_t status_indicator_get_state(void)
{
    return s_state;
}


bool status_indicator_backend_enabled(void)
{
    return s_backend_enabled;
}
