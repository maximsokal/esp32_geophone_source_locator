#include "seismic_acquisition.h"

#include <stdatomic.h>
#include <string.h>

#include "device_status.h"
#include "seismic_buffer.h"
#include "seismic_source_driver.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG = "SEISMIC_ACQ";

static SemaphoreHandle_t s_lifecycle_mutex = NULL;
static atomic_bool s_running = ATOMIC_VAR_INIT(false);
static atomic_int s_last_error = ATOMIC_VAR_INIT(ESP_OK);


static const seismic_source_driver_t *seismic_driver(void)
{
    return seismic_source_registry_selected();
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
        running ? driver->sample_rate_hz : 0,
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


esp_err_t seismic_acquisition_start(void)
{
    esp_err_t err =
        seismic_acquisition_lifecycle_lock();

    if (err != ESP_OK) {
        return err;
    }

    if (atomic_load_explicit(&s_running, memory_order_acquire)) {
        /* A stale physical-health observation must not restart this task. */
        seismic_acquisition_lifecycle_unlock();
        return ESP_OK;
    }

    const seismic_source_driver_t *driver = seismic_driver();

    ESP_LOGI(TAG, "Source: %s", driver->name);

    err = driver->start();
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

        seismic_acquisition_lifecycle_unlock();
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
        seismic_acquisition_lifecycle_unlock();
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

    seismic_acquisition_lifecycle_unlock();
    return ESP_OK;
}


void seismic_acquisition_stop(void)
{
    if (seismic_acquisition_lifecycle_lock() != ESP_OK) {
        atomic_store_explicit(
            &s_running,
            false,
            memory_order_release
        );
        return;
    }

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

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    esp_err_t err = seismic_acquisition_publish_status(false);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to publish stopped acquisition status: %s",
            esp_err_to_name(err)
        );
    }

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
    snapshot->sample_rate_hz = driver->sample_rate_hz;
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
