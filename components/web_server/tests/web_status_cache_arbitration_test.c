#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "web_status_cache_arbitration.h"


typedef struct
{
    unsigned count;

} fake_semaphore_t;


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    fake_semaphore_t *semaphore = calloc(1U, sizeof(*semaphore));
    if (semaphore != NULL) semaphore->count = 1U;
    return (SemaphoreHandle_t)semaphore;
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    (void)timeout;
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL || semaphore->count == 0U) return pdFALSE;
    --semaphore->count;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    fake_semaphore_t *semaphore = handle;
    if (semaphore == NULL) return pdFALSE;
    ++semaphore->count;
    return pdTRUE;
}


void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    free(handle);
}


typedef struct
{
    bool idle;
    unsigned idle_checks;

} fake_storage_state_t;


static bool fake_storage_is_idle(void *user_context)
{
    fake_storage_state_t *state = user_context;
    assert(state != NULL);
    ++state->idle_checks;
    return state->idle;
}


static void test_cache_refresh_and_start_reservations_cannot_overlap(void)
{
    web_status_cache_arbitration_t arbitration = {0};
    fake_storage_state_t storage = {
        .idle = true,
        .idle_checks = 0U,
    };

    assert(web_status_cache_arbitration_init(&arbitration) == ESP_OK);

    /* Deterministic interleaving: refresh wins the reservation first, then
     * START attempts exactly in the old check-to-SD race window. */
    assert(web_status_cache_arbitration_try_reserve_refresh(
        &arbitration,
        fake_storage_is_idle,
        &storage
    ));
    assert(storage.idle_checks == 1U);
    assert(!web_status_cache_arbitration_try_reserve_start(&arbitration));

    web_status_cache_arbitration_finish_refresh(&arbitration, true);
    assert(web_status_cache_arbitration_cache_ready(&arbitration));

    /* START can win after refresh has published. A second refresh is rejected
     * before its filesystem-idle callback is even consulted. */
    assert(web_status_cache_arbitration_try_reserve_start(&arbitration));
    assert(!web_status_cache_arbitration_try_reserve_refresh(
        &arbitration,
        fake_storage_is_idle,
        &storage
    ));
    assert(storage.idle_checks == 1U);

    web_status_cache_arbitration_release_start(&arbitration);
    assert(web_status_cache_arbitration_try_reserve_refresh(
        &arbitration,
        fake_storage_is_idle,
        &storage
    ));
    web_status_cache_arbitration_finish_refresh(&arbitration, true);

    /* A busy recorder still blocks a refresh even when START is not pending. */
    storage.idle = false;
    assert(!web_status_cache_arbitration_try_reserve_refresh(
        &arbitration,
        fake_storage_is_idle,
        &storage
    ));
    assert(storage.idle_checks == 3U);

    web_status_cache_arbitration_shutdown(&arbitration);
}


int main(void)
{
    test_cache_refresh_and_start_reservations_cannot_overlap();
    return 0;
}
