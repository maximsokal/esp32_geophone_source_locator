#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_source_driver.h"


#ifdef _WIN32
static CRITICAL_SECTION sync_lock;
static CONDITION_VARIABLE sync_cond;
typedef HANDLE test_thread_t;
typedef DWORD (WINAPI *test_thread_fn_t)(LPVOID);

static void sync_init(void)
{
    InitializeCriticalSection(&sync_lock);
    InitializeConditionVariable(&sync_cond);
}

static void sync_destroy(void)
{
    DeleteCriticalSection(&sync_lock);
}

static void sync_enter(void)
{
    EnterCriticalSection(&sync_lock);
}

static void sync_leave(void)
{
    LeaveCriticalSection(&sync_lock);
}

static void sync_wait(void)
{
    assert(SleepConditionVariableCS(&sync_cond, &sync_lock, INFINITE));
}

static void sync_broadcast(void)
{
    WakeAllConditionVariable(&sync_cond);
}

static test_thread_t test_thread_start(test_thread_fn_t fn, void *arg)
{
    HANDLE thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    assert(thread != NULL);
    return thread;
}

static void test_thread_join(test_thread_t thread)
{
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
}

static void test_sleep_ms(unsigned ms)
{
    Sleep(ms);
}
#else
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
typedef pthread_t test_thread_t;
typedef void *(*test_thread_fn_t)(void *);

static void sync_init(void)
{
}

static void sync_destroy(void)
{
    assert(pthread_cond_destroy(&sync_cond) == 0);
    assert(pthread_mutex_destroy(&sync_lock) == 0);
}

static void sync_enter(void)
{
    assert(pthread_mutex_lock(&sync_lock) == 0);
}

static void sync_leave(void)
{
    assert(pthread_mutex_unlock(&sync_lock) == 0);
}

static void sync_wait(void)
{
    assert(pthread_cond_wait(&sync_cond, &sync_lock) == 0);
}

static void sync_broadcast(void)
{
    assert(pthread_cond_broadcast(&sync_cond) == 0);
}

static test_thread_t test_thread_start(test_thread_fn_t fn, void *arg)
{
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fn, arg) == 0);
    return thread;
}

static void test_thread_join(test_thread_t thread)
{
    assert(pthread_join(thread, NULL) == 0);
}

static void test_sleep_ms(unsigned ms)
{
    struct timespec duration = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    (void)nanosleep(&duration, NULL);
}
#endif


struct test_semaphore
{
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
};

static struct test_semaphore lifecycle_semaphore;
static bool lifecycle_semaphore_initialized;


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (!lifecycle_semaphore_initialized) {
#ifdef _WIN32
        InitializeCriticalSection(&lifecycle_semaphore.mutex);
#else
        assert(pthread_mutex_init(&lifecycle_semaphore.mutex, NULL) == 0);
#endif
        lifecycle_semaphore_initialized = true;
    }

    return &lifecycle_semaphore;
}


BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t ticks_to_wait
)
{
    assert(semaphore == &lifecycle_semaphore);
    assert(ticks_to_wait == portMAX_DELAY);

#ifdef _WIN32
    EnterCriticalSection(&semaphore->mutex);
#else
    assert(pthread_mutex_lock(&semaphore->mutex) == 0);
#endif

    return pdTRUE;
}


BaseType_t xSemaphoreGive(
    SemaphoreHandle_t semaphore
)
{
    assert(semaphore == &lifecycle_semaphore);

#ifdef _WIN32
    LeaveCriticalSection(&semaphore->mutex);
#else
    assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
#endif

    return pdTRUE;
}


static unsigned backend_start_calls;
static unsigned backend_stop_calls;
static bool backend_running;
static esp_err_t backend_start_result;
static bool backend_start_blocked;
static bool backend_start_entered;
static bool backend_start_release;
static unsigned publish_calls;
static bool last_published_running;
static uint32_t last_published_rate;
static unsigned buffer_configure_calls;
static int last_buffer_source;
static uint32_t last_buffer_rate;


static esp_err_t fake_backend_start(void)
{
    sync_enter();

    ++backend_start_calls;
    backend_start_entered = true;
    sync_broadcast();

    while (backend_start_blocked && !backend_start_release) {
        sync_wait();
    }

    const esp_err_t result =
        backend_start_result;

    if (result == ESP_OK) {
        backend_running = true;
    }

    sync_leave();
    return result;
}


static void fake_backend_stop(void)
{
    sync_enter();
    ++backend_stop_calls;
    backend_running = false;
    sync_broadcast();
    sync_leave();
}


static bool fake_backend_healthy(void)
{
    sync_enter();
    const bool healthy = backend_running;
    sync_leave();
    return healthy;
}


static esp_err_t fake_backend_snapshot(
    seismic_acquisition_snapshot_t *snapshot
)
{
    assert(snapshot != NULL);

    sync_enter();
    snapshot->running = backend_running;
    snapshot->latest_valid = backend_running;
    sync_leave();

    return ESP_OK;
}


static const seismic_source_driver_t fake_driver = {
    .name = "FAKE",
    .transport_name = "HOST",
    .sample_rate_hz = 500U,
    .channel_count = 4U,
    .core_id = 1,
    .start = fake_backend_start,
    .stop = fake_backend_stop,
    .is_healthy = fake_backend_healthy,
    .populate_snapshot = fake_backend_snapshot,
};


const seismic_source_driver_t *seismic_source_registry_selected(void)
{
    return &fake_driver;
}


esp_err_t device_status_set_acquisition(
    bool running,
    uint32_t sample_rate_hz,
    uint64_t samples_recorded,
    uint64_t recording_ms
)
{
    assert(samples_recorded == 0U);
    assert(recording_ms == 0U);

    ++publish_calls;
    last_published_running = running;
    last_published_rate = sample_rate_hz;

    return ESP_OK;
}


esp_err_t seismic_buffer_configure(
    int source,
    uint32_t sample_rate_hz
)
{
    ++buffer_configure_calls;
    last_buffer_source = source;
    last_buffer_rate = sample_rate_hz;
    return ESP_OK;
}


static void release_backend_start(void)
{
    sync_enter();
    backend_start_release = true;
    sync_broadcast();
    sync_leave();
}


static void wait_backend_start_entered(void)
{
    sync_enter();

    while (!backend_start_entered) {
        sync_wait();
    }

    sync_leave();
}


static void reset_observation(void)
{
    seismic_acquisition_stop();

    sync_enter();
    backend_start_calls = 0U;
    backend_stop_calls = 0U;
    backend_running = false;
    backend_start_result = ESP_OK;
    backend_start_blocked = false;
    backend_start_entered = false;
    backend_start_release = false;
    sync_leave();

    publish_calls = 0U;
    last_published_running = false;
    last_published_rate = 0U;
    buffer_configure_calls = 0U;
    last_buffer_source = -1;
    last_buffer_rate = UINT32_MAX;
}


typedef struct
{
    esp_err_t result;
    bool entered;
    bool returned;

} start_thread_context_t;

static bool stop_thread_entered;
static bool stop_thread_returned;


#ifdef _WIN32
static DWORD WINAPI start_thread_main(LPVOID argument)
{
    start_thread_context_t *context = argument;

    sync_enter();
    context->entered = true;
    sync_broadcast();
    sync_leave();

    context->result = seismic_acquisition_start();

    sync_enter();
    context->returned = true;
    sync_broadcast();
    sync_leave();

    return 0;
}


static DWORD WINAPI stop_thread_main(LPVOID argument)
{
    (void)argument;

    sync_enter();
    stop_thread_entered = true;
    sync_broadcast();
    sync_leave();

    seismic_acquisition_stop();

    sync_enter();
    stop_thread_returned = true;
    sync_broadcast();
    sync_leave();

    return 0;
}
#else
static void *start_thread_main(void *argument)
{
    start_thread_context_t *context = argument;

    sync_enter();
    context->entered = true;
    sync_broadcast();
    sync_leave();

    context->result = seismic_acquisition_start();

    sync_enter();
    context->returned = true;
    sync_broadcast();
    sync_leave();

    return NULL;
}


static void *stop_thread_main(void *argument)
{
    (void)argument;

    sync_enter();
    stop_thread_entered = true;
    sync_broadcast();
    sync_leave();

    seismic_acquisition_stop();

    sync_enter();
    stop_thread_returned = true;
    sync_broadcast();
    sync_leave();

    return NULL;
}
#endif


static void wait_thread_entered(
    const start_thread_context_t *context
)
{
    sync_enter();

    while (!context->entered) {
        sync_wait();
    }

    sync_leave();
}


static void test_stop_waits_for_start_and_wins_call_order(void)
{
    reset_observation();

    sync_enter();
    backend_start_blocked = true;
    sync_leave();

    start_thread_context_t start_context = {0};
    test_thread_t start_thread =
        test_thread_start(
            start_thread_main,
            &start_context
        );

    wait_backend_start_entered();

    /* Read-side APIs remain nonblocking while START owns the lifecycle mutex. */
    seismic_acquisition_snapshot_t snapshot;
    assert(!seismic_acquisition_is_running());
    assert(!seismic_acquisition_source_healthy());
    assert(seismic_acquisition_get_snapshot(&snapshot) == ESP_OK);
    assert(!snapshot.running);
    assert(!snapshot.source_healthy);

    sync_enter();
    stop_thread_entered = false;
    stop_thread_returned = false;
    sync_leave();

    test_thread_t stop_thread =
        test_thread_start(
            stop_thread_main,
            NULL
        );

    sync_enter();
    while (!stop_thread_entered) {
        sync_wait();
    }
    sync_leave();

    test_sleep_ms(10U);

    sync_enter();
    assert(!stop_thread_returned);
    assert(backend_start_calls == 1U);
    assert(backend_stop_calls == 0U);
    sync_leave();

    release_backend_start();

    test_thread_join(start_thread);
    test_thread_join(stop_thread);

    assert(start_context.result == ESP_OK);
    assert(start_context.returned);
    assert(!seismic_acquisition_is_running());
    assert(!seismic_acquisition_source_healthy());

    sync_enter();
    assert(backend_start_calls == 1U);
    assert(backend_stop_calls == 1U);
    assert(!backend_running);
    sync_leave();

    assert(!last_published_running);
    assert(last_published_rate == 0U);
    assert(last_buffer_source == SEISMIC_SOURCE_NONE);
    assert(last_buffer_rate == 0U);
}


static void test_concurrent_starts_share_one_backend_transition(void)
{
    reset_observation();

    sync_enter();
    backend_start_blocked = true;
    sync_leave();

    start_thread_context_t first = {0};
    start_thread_context_t second = {0};

    test_thread_t first_thread =
        test_thread_start(
            start_thread_main,
            &first
        );

    wait_backend_start_entered();

    test_thread_t second_thread =
        test_thread_start(
            start_thread_main,
            &second
        );

    wait_thread_entered(&second);
    test_sleep_ms(10U);

    sync_enter();
    assert(backend_start_calls == 1U);
    assert(!second.returned);
    sync_leave();

    release_backend_start();

    test_thread_join(first_thread);
    test_thread_join(second_thread);

    assert(first.result == ESP_OK);
    assert(second.result == ESP_OK);

    sync_enter();
    assert(backend_start_calls == 1U);
    assert(backend_stop_calls == 0U);
    assert(backend_running);
    sync_leave();

    assert(seismic_acquisition_is_running());

    seismic_acquisition_stop();
}


static void test_idempotent_running_start_and_stopped_stop(void)
{
    reset_observation();

    assert(seismic_acquisition_start() == ESP_OK);
    assert(seismic_acquisition_start() == ESP_OK);

    sync_enter();
    assert(backend_start_calls == 1U);
    sync_leave();

    seismic_acquisition_stop();
    seismic_acquisition_stop();

    sync_enter();
    assert(backend_stop_calls == 1U);
    sync_leave();

    assert(!seismic_acquisition_is_running());
}


static void test_failed_start_cleans_once_and_records_error(void)
{
    reset_observation();

    sync_enter();
    backend_start_result = ESP_FAIL;
    sync_leave();

    assert(seismic_acquisition_start() == ESP_FAIL);
    assert(!seismic_acquisition_is_running());
    assert(seismic_acquisition_last_error() == ESP_FAIL);

    sync_enter();
    assert(backend_start_calls == 1U);
    assert(backend_stop_calls == 1U);
    assert(!backend_running);
    sync_leave();

    assert(publish_calls == 1U);
    assert(!last_published_running);
    assert(last_published_rate == 0U);
    assert(buffer_configure_calls == 1U);
}


int main(void)
{
    sync_init();

    assert(seismic_acquisition_init() == ESP_OK);
    assert(seismic_acquisition_init() == ESP_OK);
    assert(!seismic_acquisition_is_running());
    assert(seismic_acquisition_last_error() == ESP_OK);

    test_stop_waits_for_start_and_wins_call_order();
    test_concurrent_starts_share_one_backend_transition();
    test_idempotent_running_start_and_stopped_stop();
    test_failed_start_cleans_once_and_records_error();

    reset_observation();
    sync_destroy();

    puts("seismic acquisition lifecycle tests passed");
    return 0;
}
