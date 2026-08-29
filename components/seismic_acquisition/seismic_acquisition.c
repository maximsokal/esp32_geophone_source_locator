#include "seismic_acquisition.h"

#include <string.h>

#include "device_status.h"
#include "seismic_buffer.h"
#include "seismic_source_driver.h"

#include "esp_log.h"


static const char *TAG = "SEISMIC_ACQ";

static bool s_running = false;


static const seismic_source_driver_t *seismic_driver(void)
{
    return seismic_source_registry_selected();
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


static void seismic_acquisition_reset_failed_start(void)
{
    const seismic_source_driver_t *driver = seismic_driver();

    /* Backends own partial cleanup; stop() must remain idempotent. */
    driver->stop();

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running = false;
    (void)seismic_acquisition_publish_status(false);
}


esp_err_t seismic_acquisition_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    const seismic_source_driver_t *driver = seismic_driver();

    ESP_LOGI(TAG, "Source: %s", driver->name);

    esp_err_t err = driver->start();
    if (err != ESP_OK) {
        seismic_acquisition_reset_failed_start();
        return err;
    }

    err = seismic_acquisition_publish_status(true);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to publish running acquisition status: %s",
            esp_err_to_name(err)
        );

        seismic_acquisition_reset_failed_start();
        return err;
    }

    s_running = true;
    return ESP_OK;
}


void seismic_acquisition_stop(void)
{
    const seismic_source_driver_t *driver = seismic_driver();

    if (s_running) {
        driver->stop();
    }

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running = false;

    esp_err_t err = seismic_acquisition_publish_status(false);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to publish stopped acquisition status: %s",
            esp_err_to_name(err)
        );
    }
}


bool seismic_acquisition_is_running(void)
{
    return s_running;
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

    snapshot->running = s_running;
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

    /* A backend cannot report RUN if the orchestrator is stopped. */
    snapshot->running = snapshot->running && s_running;

    return ESP_OK;
}
