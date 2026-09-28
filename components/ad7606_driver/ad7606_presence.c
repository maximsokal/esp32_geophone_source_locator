#include "ad7606_driver.h"

#include <stdbool.h>
#include <stdint.h>

#include "ad7606_presence_policy.h"
#include "board_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AD7606_PRESENCE_START_TIMEOUT_US 75000LL
#define AD7606_PRESENCE_STALE_MS 250U

static const char *TAG = "AD7606_PRESENCE";

static portMUX_TYPE s_presence_lock = portMUX_INITIALIZER_UNLOCKED;
static ad7606_presence_observer_t s_observer;
static bool s_monitor_active = false;
static bool s_isr_service_ready = false;
static bool s_external_busy_isr = false;
static TickType_t s_last_complete_pulse_tick = 0;

static void ad7606_presence_isr(void *argument)
{
    (void)argument;

    const int level = gpio_get_level(BOARD_AD7606_BUSY_GPIO);

    portENTER_CRITICAL_ISR(&s_presence_lock);
    const uint32_t before = s_observer.completed_pulses;
    ad7606_presence_observer_observe(&s_observer, level);
    if (s_observer.completed_pulses != before) {
        s_last_complete_pulse_tick = xTaskGetTickCountFromISR();
    }
    portEXIT_CRITICAL_ISR(&s_presence_lock);
}

static bool ad7606_presence_ready_snapshot(void)
{
    bool ready;

    portENTER_CRITICAL(&s_presence_lock);
    ready = s_monitor_active && ad7606_presence_observer_ready(&s_observer);
    portEXIT_CRITICAL(&s_presence_lock);

    return ready;
}

esp_err_t ad7606_driver_presence_monitor_start(void)
{
    ad7606_driver_presence_monitor_stop();

    portENTER_CRITICAL(&s_presence_lock);
    const bool external_busy_isr = s_external_busy_isr;
    portEXIT_CRITICAL(&s_presence_lock);

    /*
     * A disconnected BUSY input must not float into a false-positive. The
     * AD7606 BUSY output is push-pull, so this weak internal pull-down is only
     * a deterministic fallback when no ADC is physically driving the line.
     */
    esp_err_t err = gpio_set_pull_mode(
        BOARD_AD7606_BUSY_GPIO,
        GPIO_PULLDOWN_ONLY
    );
    if (err != ESP_OK) {
        return err;
    }

    /*
     * gpio_uninstall_isr_service() is deliberately not used: the service is a
     * process-wide singleton and may be shared by other drivers. Install it at
     * most once from this component. A pre-existing service is accepted on the
     * first call; subsequent AD7606 retries no longer emit the driver's
     * "GPIO isr service already installed" error before returning
     * ESP_ERR_INVALID_STATE.
     */
    if (!external_busy_isr && !s_isr_service_ready) {
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(err));
            return err;
        }
        s_isr_service_ready = true;
    }

    if (!external_busy_isr) {
        err = gpio_set_intr_type(
            BOARD_AD7606_BUSY_GPIO,
            GPIO_INTR_ANYEDGE
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    portENTER_CRITICAL(&s_presence_lock);
    ad7606_presence_observer_reset(
        &s_observer,
        gpio_get_level(BOARD_AD7606_BUSY_GPIO)
    );
    s_last_complete_pulse_tick = 0;
    s_monitor_active = true;
    portEXIT_CRITICAL(&s_presence_lock);

    if (!external_busy_isr) {
        err = gpio_isr_handler_add(
            BOARD_AD7606_BUSY_GPIO,
            ad7606_presence_isr,
            NULL
        );
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_presence_lock);
            s_monitor_active = false;
            portEXIT_CRITICAL(&s_presence_lock);
            ESP_LOGE(TAG, "BUSY ISR registration failed: %s", esp_err_to_name(err));
            return err;
        }

        err = gpio_intr_enable(BOARD_AD7606_BUSY_GPIO);
        if (err != ESP_OK) {
            (void)gpio_isr_handler_remove(BOARD_AD7606_BUSY_GPIO);
            portENTER_CRITICAL(&s_presence_lock);
            s_monitor_active = false;
            portEXIT_CRITICAL(&s_presence_lock);
            return err;
        }
    }

    const int64_t deadline_us =
        esp_timer_get_time() + AD7606_PRESENCE_START_TIMEOUT_US;

    while (esp_timer_get_time() < deadline_us) {
        if (ad7606_presence_ready_snapshot()) {
            uint32_t pulses;
            portENTER_CRITICAL(&s_presence_lock);
            pulses = s_observer.completed_pulses;
            portEXIT_CRITICAL(&s_presence_lock);

            ESP_LOGI(
                TAG,
                "Physical ADC detected: BUSY pulses=%u",
                (unsigned)pulses
            );
            return ESP_OK;
        }

        vTaskDelay(1);
    }

    ESP_LOGE(
        TAG,
        "AD7606 not detected: no valid BUSY LOW-HIGH-LOW pulse train within %lld ms",
        AD7606_PRESENCE_START_TIMEOUT_US / 1000LL
    );

    ad7606_driver_presence_monitor_stop();
    return ESP_ERR_NOT_FOUND;
}

void ad7606_driver_presence_monitor_stop(void)
{
    bool was_active;

    portENTER_CRITICAL(&s_presence_lock);
    was_active = s_monitor_active;
    s_monitor_active = false;
    s_last_complete_pulse_tick = 0;
    ad7606_presence_observer_reset(&s_observer, 0);
    portEXIT_CRITICAL(&s_presence_lock);

    if (!was_active) {
        return;
    }

    portENTER_CRITICAL(&s_presence_lock);
    const bool external_busy_isr = s_external_busy_isr;
    portEXIT_CRITICAL(&s_presence_lock);
    if (!external_busy_isr) {
        (void)gpio_intr_disable(BOARD_AD7606_BUSY_GPIO);
        (void)gpio_isr_handler_remove(BOARD_AD7606_BUSY_GPIO);
    }
}

void ad7606_driver_presence_use_external_busy_isr(bool enabled)
{
    portENTER_CRITICAL(&s_presence_lock);
    s_external_busy_isr = enabled;
    portEXIT_CRITICAL(&s_presence_lock);
}

void ad7606_driver_presence_note_busy_pulse(void)
{
    portENTER_CRITICAL(&s_presence_lock);
    if (s_external_busy_isr && s_monitor_active) {
        ad7606_presence_observer_observe(&s_observer, 1);
        ad7606_presence_observer_observe(&s_observer, 0);
        s_last_complete_pulse_tick = xTaskGetTickCount();
    }
    portEXIT_CRITICAL(&s_presence_lock);
}

bool ad7606_driver_presence_is_healthy(void)
{
    bool active;
    bool ready;
    TickType_t last_tick;

    portENTER_CRITICAL(&s_presence_lock);
    active = s_monitor_active;
    ready = ad7606_presence_observer_ready(&s_observer);
    last_tick = s_last_complete_pulse_tick;
    portEXIT_CRITICAL(&s_presence_lock);

    if (!active || !ready || last_tick == 0) {
        return false;
    }

    const TickType_t age_ticks = xTaskGetTickCount() - last_tick;
    return age_ticks <= pdMS_TO_TICKS(AD7606_PRESENCE_STALE_MS);
}
