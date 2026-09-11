#include "web_status_cache_arbitration.h"

#include <string.h>


esp_err_t web_status_cache_arbitration_init(
    web_status_cache_arbitration_t *arbitration
)
{
    if (arbitration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (arbitration->mutex != NULL) {
        return ESP_OK;
    }

    memset(arbitration, 0, sizeof(*arbitration));
    arbitration->mutex = xSemaphoreCreateMutex();
    if (arbitration->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


void web_status_cache_arbitration_shutdown(
    web_status_cache_arbitration_t *arbitration
)
{
    if (arbitration == NULL) {
        return;
    }

    if (arbitration->mutex != NULL) {
        vSemaphoreDelete(arbitration->mutex);
    }

    memset(arbitration, 0, sizeof(*arbitration));
}


bool web_status_cache_arbitration_lock(
    web_status_cache_arbitration_t *arbitration
)
{
    return arbitration != NULL &&
        arbitration->mutex != NULL &&
        xSemaphoreTake(arbitration->mutex, portMAX_DELAY) == pdTRUE;
}


void web_status_cache_arbitration_unlock(
    web_status_cache_arbitration_t *arbitration
)
{
    if (arbitration != NULL && arbitration->mutex != NULL) {
        (void)xSemaphoreGive(arbitration->mutex);
    }
}


bool web_status_cache_arbitration_cache_ready(
    web_status_cache_arbitration_t *arbitration
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return false;
    }

    const bool ready = arbitration->cache_ready &&
        !arbitration->cache_refreshing;
    web_status_cache_arbitration_unlock(arbitration);
    return ready;
}


void web_status_cache_arbitration_invalidate(
    web_status_cache_arbitration_t *arbitration
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return;
    }

    arbitration->cache_ready = false;
    if (arbitration->cache_refreshing) {
        arbitration->invalidated_during_refresh = true;
    }

    web_status_cache_arbitration_unlock(arbitration);
}


bool web_status_cache_arbitration_try_reserve_refresh(
    web_status_cache_arbitration_t *arbitration,
    web_status_cache_storage_idle_callback_t storage_idle_callback,
    void *user_context
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return false;
    }

    bool allowed = false;
    if (!arbitration->cache_refreshing && !arbitration->start_reserved) {
        const bool storage_idle = storage_idle_callback == NULL ||
            storage_idle_callback(user_context);
        allowed = storage_idle;
    }
    if (allowed) {
        arbitration->cache_refreshing = true;
        arbitration->cache_ready = false;
        arbitration->invalidated_during_refresh = false;
    }

    web_status_cache_arbitration_unlock(arbitration);
    return allowed;
}


void web_status_cache_arbitration_finish_refresh(
    web_status_cache_arbitration_t *arbitration,
    bool success
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return;
    }

    arbitration->cache_refreshing = false;
    arbitration->cache_ready = success &&
        !arbitration->invalidated_during_refresh;
    arbitration->invalidated_during_refresh = false;
    web_status_cache_arbitration_unlock(arbitration);
}


bool web_status_cache_arbitration_try_reserve_start(
    web_status_cache_arbitration_t *arbitration
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return false;
    }

    /* Cache failure is not a device-recording failure. A missing/unready
     * cache may let START reserve the slot; an active refresh still wins the
     * arbitration, and the refresh task will retry after STARTING/RECORDING
     * has made storage non-idle. */
    const bool allowed = !arbitration->cache_refreshing &&
        !arbitration->start_reserved;
    if (allowed) {
        arbitration->start_reserved = true;
    }

    web_status_cache_arbitration_unlock(arbitration);
    return allowed;
}


void web_status_cache_arbitration_release_start(
    web_status_cache_arbitration_t *arbitration
)
{
    if (!web_status_cache_arbitration_lock(arbitration)) {
        return;
    }

    arbitration->start_reserved = false;
    web_status_cache_arbitration_unlock(arbitration);
}
