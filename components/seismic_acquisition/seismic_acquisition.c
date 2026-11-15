#include "seismic_acquisition.h"

#include <stdatomic.h>
#include <string.h>

#include "device_status.h"
#include "seismic_buffer.h"
#include "seismic_source_driver.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG = "SEISMIC_ACQ";

static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static atomic_bool s_running = false;
static atomic_int s_last_error = ESP_OK;
/* Protected by s_lifecycle_mutex; no consumer reference counts. */
static bool s_tft_preview;
static int64_t s_web_preview_until_us;
static bool s_shutdown;
static bool s_probe_waiting_for_power;


static const seismic_source_driver_t *seismic_driver(void)
{
    return seismic_source_registry_selected();
}


static uint32_t seismic_driver_rate(const seismic_source_driver_t *driver)
{
    return driver->get_sample_rate != NULL ? driver->get_sample_rate() : driver->sample_rate_hz;
}


static bool seismic_driver_healthy(void)
{
    const seismic_source_driver_t *driver = seismic_driver();

    if (!atomic_load_explicit(&s_running, memory_order_acquire)) {
        return false;
    }

    return driver->is_healthy == NULL || driver->is_healthy();
}


static esp_err_t seismic_acquisition_publish_status(
    bool running
)
{
    const seismic_source_driver_t *driver = seismic_driver();

    return device_status_set_acquisition(
        running,
        running ? seismic_driver_rate(driver) : 0,
        0,
        0
    );
}


static esp_err_t seismic_acquisition_lifecycle_lock(void)
{
    if (s_lifecycle_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xSemaphoreTake(
        s_lifecycle_mutex,
        portMAX_DELAY
    ) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}


static void seismic_acquisition_lifecycle_unlock(void)
{
    if (s_lifecycle_mutex != NULL) {
        (void)xSemaphoreGive(
            s_lifecycle_mutex
        );
    }
}


static void seismic_acquisition_reset_failed_start_locked(void)
{
    const seismic_source_driver_t *driver = seismic_driver();

    atomic_store_explicit(
        &s_running,
        false,
        memory_order_release
    );

    /* Backends own partial cleanup; stop() must remain idempotent. */
    driver->stop();

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    (void)seismic_acquisition_publish_status(false);
}


esp_err_t seismic_acquisition_init(void)
{
    if (s_lifecycle_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex =
        xSemaphoreCreateMutex();

    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_lifecycle_mutex =
        mutex;

    atomic_store_explicit(
        &s_running,
        false,
        memory_order_release
    );

    atomic_store_explicit(
        &s_last_error,
        ESP_OK,
        memory_order_release
    );

    return ESP_OK;
}


static esp_err_t seismic_acquisition_start_locked(void)
{
    if (s_shutdown) {
        return ESP_ERR_INVALID_STATE;
    }

    if (atomic_load_explicit(&s_running, memory_order_acquire)) {
        /* A stale physical-health observation must not restart this task. */
        return ESP_OK;
    }

    const seismic_source_driver_t *driver = seismic_driver();

    ESP_LOGI(TAG, "Source: %s", driver->name);

    esp_err_t err = driver->start();
    if (err != ESP_OK) {
        atomic_store_explicit(
            &s_last_error,
            err,
            memory_order_release
        );

        seismic_acquisition_reset_failed_start_locked();

        ESP_LOGE(
            TAG,
            "Source %s unavailable: %s",
            driver->name,
            esp_err_to_name(err)
        );

        return err;
    }

    err = seismic_acquisition_publish_status(true);
    if (err != ESP_OK) {
        atomic_store_explicit(
            &s_last_error,
            err,
            memory_order_release
        );

        ESP_LOGE(
            TAG,
            "Unable to publish running acquisition status: %s",
            esp_err_to_name(err)
        );

        seismic_acquisition_reset_failed_start_locked();
        return err;
    }

    atomic_store_explicit(
        &s_last_error,
        ESP_OK,
        memory_order_release
    );

    atomic_store_explicit(
        &s_running,
        true,
        memory_order_release
    );
    s_probe_waiting_for_power = false;

    return ESP_OK;
}


esp_err_t seismic_acquisition_start(void)
{
    esp_err_t err = seismic_acquisition_lifecycle_lock();
    if (err != ESP_OK) return err;
    err = seismic_acquisition_start_locked();
    seismic_acquisition_lifecycle_unlock();
    return err;
}


static esp_err_t seismic_acquisition_probe_locked(void)
{
    if (s_shutdown) return ESP_ERR_INVALID_STATE;
    if (atomic_load_explicit(&s_running, memory_order_acquire)) return ESP_OK;
    const seismic_source_driver_t *driver = seismic_driver();
    const esp_err_t err = driver->probe != NULL ? driver->probe() : ESP_OK;
    s_probe_waiting_for_power = err == ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&s_last_error, err, memory_order_release);
    return err;
}


esp_err_t seismic_acquisition_probe(void)
{
    esp_err_t err = seismic_acquisition_lifecycle_lock();
    if (err != ESP_OK) return err;
    err = seismic_acquisition_probe_locked();
    seismic_acquisition_lifecycle_unlock();
    return err;
}


static void seismic_acquisition_stop_locked(bool preserve_buffer)
{
    const seismic_source_driver_t *driver = seismic_driver();

    const bool was_running =
        atomic_exchange_explicit(
            &s_running,
            false,
            memory_order_acq_rel
        );

    if (was_running) {
        driver->stop();
    }

    if (!preserve_buffer) {
        (void)seismic_buffer_configure(SEISMIC_SOURCE_NONE, 0);
    }

    esp_err_t err = seismic_acquisition_publish_status(false);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to publish stopped acquisition status: %s",
            esp_err_to_name(err)
        );
    }
}


void seismic_acquisition_stop(void)
{
    if (seismic_acquisition_lifecycle_lock() != ESP_OK) return;
    s_tft_preview = false;
    s_web_preview_until_us = 0;
    seismic_acquisition_stop_locked(false);
    seismic_acquisition_lifecycle_unlock();
}


uint32_t seismic_acquisition_get_sample_rate(void)
{
    return seismic_driver_rate(seismic_driver());
}


bool seismic_acquisition_sample_rate_supported(uint32_t sample_rate_hz)
{
    const seismic_source_driver_t *driver = seismic_driver();
    if (driver->sample_rate_supported != NULL) return driver->sample_rate_supported(sample_rate_hz);
    return sample_rate_hz == driver->sample_rate_hz;
}


esp_err_t seismic_acquisition_set_sample_rate(uint32_t sample_rate_hz)
{
    const seismic_source_driver_t *driver = seismic_driver();
    if (driver->set_sample_rate == NULL) {
        return sample_rate_hz == driver->sample_rate_hz ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = seismic_acquisition_lifecycle_lock();
    if (err != ESP_OK) return err;
    if (atomic_load_explicit(&s_running, memory_order_acquire)) {
        err = sample_rate_hz == seismic_driver_rate(driver) ? ESP_OK : ESP_ERR_INVALID_STATE;
    } else {
        err = driver->set_sample_rate(sample_rate_hz);
    }
    seismic_acquisition_lifecycle_unlock();
    return err;
}


esp_err_t seismic_acquisition_set_preview(seismic_preview_t preview, bool active)
{
    if (preview != SEISMIC_PREVIEW_TFT && preview != SEISMIC_PREVIEW_WEB) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = seismic_acquisition_lifecycle_lock();
    if (err != ESP_OK) return err;
    if (active) err = seismic_acquisition_start_locked();
    if (!active || err == ESP_OK) {
        if (preview == SEISMIC_PREVIEW_TFT) s_tft_preview = active;
        else s_web_preview_until_us = active
            ? esp_timer_get_time() + (int64_t)SEISMIC_WEB_PREVIEW_LEASE_MS * 1000
            : 0;
    }
    /* The measurement worker releases the producer after checking recorder ownership. */
    seismic_acquisition_lifecycle_unlock();
    return err;
}


void seismic_acquisition_poll(bool measurement_owns_source)
{
    if (seismic_acquisition_lifecycle_lock() != ESP_OK) return;
    bool report_rate = false;
    if (!s_shutdown) {
        if (s_probe_waiting_for_power && !atomic_load(&s_running)) {
            (void)seismic_acquisition_probe_locked();
        }
        const bool web_preview = esp_timer_get_time() < s_web_preview_until_us;
        if (!measurement_owns_source && !s_tft_preview && !web_preview &&
            atomic_load(&s_running)) {
            seismic_acquisition_stop_locked(false);
        }
        report_rate = atomic_load(&s_running);
    }
    seismic_acquisition_lifecycle_unlock();
    /* Printing to the UART blocks for milliseconds: do it here on the service core, never in the worker. */
    if (report_rate) {
        const seismic_source_driver_t *driver = seismic_driver();
        if (driver != NULL && driver->periodic_report != NULL) driver->periodic_report();
    }
}


void seismic_acquisition_prepare_shutdown(void)
{
    if (seismic_acquisition_lifecycle_lock() != ESP_OK) return;
    s_shutdown = true;
    s_tft_preview = false;
    s_web_preview_until_us = 0;
    seismic_acquisition_stop_locked(true);
    seismic_acquisition_lifecycle_unlock();
}


bool seismic_acquisition_is_running(void)
{
    return atomic_load_explicit(
        &s_running,
        memory_order_acquire
    );
}


bool seismic_acquisition_source_healthy(void)
{
    return seismic_driver_healthy();
}


esp_err_t seismic_acquisition_last_error(void)
{
    return (esp_err_t)atomic_load_explicit(
        &s_last_error,
        memory_order_acquire
    );
}


const char *seismic_acquisition_source_name(void)
{
    return seismic_driver()->name;
}


esp_err_t seismic_acquisition_get_snapshot(
    seismic_acquisition_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const seismic_source_driver_t *driver = seismic_driver();

    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->running =
        atomic_load_explicit(
            &s_running,
            memory_order_acquire
        );

    snapshot->source_name = driver->name;
    snapshot->transport_name = driver->transport_name;
    snapshot->sample_rate_hz = seismic_driver_rate(driver);
    snapshot->channel_count = driver->channel_count;
    snapshot->core_id = driver->core_id;

    if (driver->populate_snapshot != NULL) {
        esp_err_t err = driver->populate_snapshot(snapshot);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Keep task state, physical presence, and serial quality independent. */
    snapshot->running =
        snapshot->running &&
        atomic_load_explicit(
            &s_running,
            memory_order_acquire
        );

    snapshot->source_healthy = seismic_driver_healthy();

    return ESP_OK;
}
