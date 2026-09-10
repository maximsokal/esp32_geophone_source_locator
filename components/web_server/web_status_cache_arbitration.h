#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


typedef bool (*web_status_cache_storage_idle_callback_t)(void *user_context);


typedef struct
{
    SemaphoreHandle_t mutex;
    bool cache_ready;
    bool cache_refreshing;
    bool start_reserved;
    bool invalidated_during_refresh;

} web_status_cache_arbitration_t;


esp_err_t web_status_cache_arbitration_init(
    web_status_cache_arbitration_t *arbitration
);


void web_status_cache_arbitration_shutdown(
    web_status_cache_arbitration_t *arbitration
);


bool web_status_cache_arbitration_lock(
    web_status_cache_arbitration_t *arbitration
);


void web_status_cache_arbitration_unlock(
    web_status_cache_arbitration_t *arbitration
);


bool web_status_cache_arbitration_cache_ready(
    web_status_cache_arbitration_t *arbitration
);


void web_status_cache_arbitration_invalidate(
    web_status_cache_arbitration_t *arbitration
);


bool web_status_cache_arbitration_try_reserve_refresh(
    web_status_cache_arbitration_t *arbitration,
    web_status_cache_storage_idle_callback_t storage_idle_callback,
    void *user_context
);


void web_status_cache_arbitration_finish_refresh(
    web_status_cache_arbitration_t *arbitration,
    bool success
);


bool web_status_cache_arbitration_try_reserve_start(
    web_status_cache_arbitration_t *arbitration
);


void web_status_cache_arbitration_release_start(
    web_status_cache_arbitration_t *arbitration
);
