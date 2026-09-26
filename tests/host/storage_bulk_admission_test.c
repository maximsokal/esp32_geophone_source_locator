#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "freertos/semphr.h"
#include "storage_bulk_admission.h"
#include "storage_manager_internal.h"

typedef struct
{
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif

} test_mutex_t;

typedef struct
{
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool signaled;
#endif

} test_event_t;

static SemaphoreHandle_t s_admission_mutex;
static atomic_uint s_admission_take_calls;
static atomic_bool s_barrier_active;
static atomic_bool s_chunk_callback_finished;
static atomic_bool s_enter_finished;
static test_event_t s_chunk_entered;
static test_event_t s_release_chunk;
static test_event_t s_enter_waiting;
static test_event_t s_enter_done;


static void test_mutex_init(test_mutex_t *mutex)
{
#ifdef _WIN32
    InitializeCriticalSection(&mutex->mutex);
#else
    assert(pthread_mutex_init(&mutex->mutex, NULL) == 0);
#endif
}


static void test_mutex_lock(test_mutex_t *mutex)
{
#ifdef _WIN32
    EnterCriticalSection(&mutex->mutex);
#else
    assert(pthread_mutex_lock(&mutex->mutex) == 0);
#endif
}


static void test_mutex_unlock(test_mutex_t *mutex)
{
#ifdef _WIN32
    LeaveCriticalSection(&mutex->mutex);
#else
    assert(pthread_mutex_unlock(&mutex->mutex) == 0);
#endif
}


static void test_mutex_destroy(test_mutex_t *mutex)
{
#ifdef _WIN32
    DeleteCriticalSection(&mutex->mutex);
#else
    assert(pthread_mutex_destroy(&mutex->mutex) == 0);
#endif
}


static void test_event_init(test_event_t *event)
{
#ifdef _WIN32
    event->handle = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(event->handle != NULL);
#else
    assert(pthread_mutex_init(&event->mutex, NULL) == 0);
    assert(pthread_cond_init(&event->condition, NULL) == 0);
    event->signaled = false;
#endif
}


static void test_event_set(test_event_t *event)
{
#ifdef _WIN32
    assert(SetEvent(event->handle) != 0);
#else
    assert(pthread_mutex_lock(&event->mutex) == 0);
    event->signaled = true;
    assert(pthread_cond_broadcast(&event->condition) == 0);
    assert(pthread_mutex_unlock(&event->mutex) == 0);
#endif
}


static void test_event_wait(test_event_t *event)
{
#ifdef _WIN32
    assert(WaitForSingleObject(event->handle, INFINITE) == WAIT_OBJECT_0);
#else
    assert(pthread_mutex_lock(&event->mutex) == 0);
    while (!event->signaled) {
        assert(pthread_cond_wait(&event->condition, &event->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&event->mutex) == 0);
#endif
}


static void test_event_reset(test_event_t *event)
{
#ifdef _WIN32
    assert(ResetEvent(event->handle) != 0);
#else
    assert(pthread_mutex_lock(&event->mutex) == 0);
    event->signaled = false;
    assert(pthread_mutex_unlock(&event->mutex) == 0);
#endif
}


static void test_event_destroy(test_event_t *event)
{
#ifdef _WIN32
    assert(CloseHandle(event->handle) != 0);
#else
    assert(pthread_cond_destroy(&event->condition) == 0);
    assert(pthread_mutex_destroy(&event->mutex) == 0);
#endif
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    test_mutex_t *mutex = malloc(sizeof(*mutex));
    assert(mutex != NULL);
    test_mutex_init(mutex);
    s_admission_mutex = mutex;
    return mutex;
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == s_admission_mutex);
    assert(timeout == portMAX_DELAY);
    const unsigned previous = atomic_fetch_add_explicit(
        &s_admission_take_calls,
        1U,
        memory_order_relaxed
    );
    if (previous == 1U && atomic_load_explicit(&s_barrier_active, memory_order_acquire)) {
        test_event_set(&s_enter_waiting);
    }
    test_mutex_lock((test_mutex_t *)semaphore);
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == s_admission_mutex);
    test_mutex_unlock((test_mutex_t *)semaphore);
    return pdTRUE;
}


static esp_err_t count_chunk(void *context)
{
    unsigned *calls = context;
    ++*calls;
    return ESP_OK;
}


static esp_err_t blocking_chunk(void *context)
{
    (void)context;
    test_event_set(&s_chunk_entered);
    test_event_wait(&s_release_chunk);
    atomic_store_explicit(&s_chunk_callback_finished, true, memory_order_release);
    return ESP_OK;
}


#ifdef _WIN32
static DWORD WINAPI run_blocking_chunk_thread(LPVOID context)
#else
static void *run_blocking_chunk_thread(void *context)
#endif
{
    (void)context;
    assert(storage_bulk_admission_run_chunk(blocking_chunk, NULL) == ESP_OK);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}


#ifdef _WIN32
static DWORD WINAPI enter_protected_thread(LPVOID context)
#else
static void *enter_protected_thread(void *context)
#endif
{
    (void)context;
    assert(storage_bulk_admission_enter_protected() == ESP_OK);
    atomic_store_explicit(&s_enter_finished, true, memory_order_release);
    test_event_set(&s_enter_done);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}


static void test_barrier_serializes_enter_after_chunk(void)
{
    test_event_init(&s_chunk_entered);
    test_event_init(&s_release_chunk);
    test_event_init(&s_enter_waiting);
    test_event_init(&s_enter_done);
    atomic_store_explicit(&s_barrier_active, true, memory_order_release);
    atomic_store_explicit(&s_admission_take_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_chunk_callback_finished, false, memory_order_relaxed);
    atomic_store_explicit(&s_enter_finished, false, memory_order_relaxed);

#ifdef _WIN32
    HANDLE chunk_thread = CreateThread(NULL, 0, run_blocking_chunk_thread, NULL, 0, NULL);
    assert(chunk_thread != NULL);
#else
    pthread_t chunk_thread;
    assert(pthread_create(&chunk_thread, NULL, run_blocking_chunk_thread, NULL) == 0);
#endif
    test_event_wait(&s_chunk_entered);

#ifdef _WIN32
    HANDLE enter_thread = CreateThread(NULL, 0, enter_protected_thread, NULL, 0, NULL);
    assert(enter_thread != NULL);
#else
    pthread_t enter_thread;
    assert(pthread_create(&enter_thread, NULL, enter_protected_thread, NULL) == 0);
#endif

    /* The second mutex take is enter_protected() blocked behind the chunk. */
    test_event_wait(&s_enter_waiting);
    assert(!atomic_load_explicit(&s_enter_finished, memory_order_acquire));
    test_event_set(&s_release_chunk);

#ifdef _WIN32
    assert(WaitForSingleObject(chunk_thread, INFINITE) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(enter_thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(chunk_thread);
    CloseHandle(enter_thread);
#else
    assert(pthread_join(chunk_thread, NULL) == 0);
    assert(pthread_join(enter_thread, NULL) == 0);
#endif

    test_event_wait(&s_enter_done);
    assert(atomic_load_explicit(&s_chunk_callback_finished, memory_order_acquire));
    assert(atomic_load_explicit(&s_enter_finished, memory_order_acquire));
    assert(storage_bulk_admission_is_protected());

    unsigned calls = 0U;
    assert(storage_bulk_admission_run_chunk(count_chunk, &calls) == ESP_ERR_INVALID_STATE);
    assert(calls == 0U);
    storage_bulk_admission_exit_protected();
    atomic_store_explicit(&s_barrier_active, false, memory_order_release);

    test_event_destroy(&s_enter_done);
    test_event_destroy(&s_enter_waiting);
    test_event_destroy(&s_release_chunk);
    test_event_destroy(&s_chunk_entered);
}


static void test_fixed_seed_randomized_admission_sequence(void)
{
    uint32_t state = UINT32_C(0x2705a17d);
    unsigned calls = 0U;

    for (unsigned index = 0U; index < 10000U; ++index) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        switch (state % 3U) {
            case 0U:
                assert(storage_bulk_admission_enter_protected() == ESP_OK);
                break;
            case 1U:
                storage_bulk_admission_exit_protected();
                break;
            default: {
                const bool protected_before = storage_bulk_admission_is_protected();
                const unsigned calls_before = calls;
                const esp_err_t err = storage_bulk_admission_run_chunk(count_chunk, &calls);
                if (protected_before) {
                    assert(err == ESP_ERR_INVALID_STATE);
                    assert(calls == calls_before);
                }
                else {
                    assert(err == ESP_OK);
                    assert(calls == calls_before + 1U);
                }
                break;
            }
        }
    }
}


int main(void)
{
    assert(storage_bulk_admission_initialize() == ESP_OK);

    unsigned calls = 0U;
    assert(storage_bulk_admission_run_chunk(count_chunk, &calls) == ESP_OK);
    assert(calls == 1U);

    assert(storage_bulk_admission_enter_protected() == ESP_OK);
    assert(storage_bulk_admission_enter_protected() == ESP_OK);
    assert(storage_bulk_admission_is_protected());
    assert(storage_bulk_admission_run_chunk(count_chunk, &calls) == ESP_ERR_INVALID_STATE);
    assert(calls == 1U);
    storage_bulk_admission_exit_protected();
    storage_bulk_admission_exit_protected();
    assert(!storage_bulk_admission_is_protected());

    test_barrier_serializes_enter_after_chunk();
    test_fixed_seed_randomized_admission_sequence();
    storage_bulk_admission_exit_protected();

    test_mutex_destroy((test_mutex_t *)s_admission_mutex);
    free(s_admission_mutex);
    return 0;
}
