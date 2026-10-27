#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "board_config.h"
#include "sensor_bus.h"
#include "sensor_bus_timeout.h"
#include "sensor_bus_lines.h"
#include "sensor_bus_lines_fake.h"
#include "tca9548a_mux.h"
#include "tca9548a_mux_i2c_bus.h"

/*
 * Regression guard for sensor_bus.c's shared s_bus/s_ina3221/s_tca9548a
 * state: it is written from at least two independent FreeRTOS tasks in
 * production (the INA3221 monitor worker and the nRF topology service
 * worker), both of which call sensor_bus_init()/get_handles()/
 * get_tca9548a() on their own schedule. Before the sensor_bus_lock_init()
 * fix, that state had zero synchronization.
 *
 * This test spawns two real OS threads driving sensor_bus concurrently,
 * including periodic sensor_bus_deinit() calls to force repeated
 * re-initialization races, and asserts that no two threads are ever inside
 * the fake I2C layer at the same time. The fake layer also sleeps briefly
 * on every call to deliberately widen the race window.
 */

#ifdef _WIN32
typedef HANDLE test_thread_t;
typedef DWORD (WINAPI *test_thread_fn_t)(LPVOID);

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
typedef pthread_t test_thread_t;
typedef void *(*test_thread_fn_t)(void *);

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


/* Real cross-task mutex backing sensor_bus's own lock. */
struct test_semaphore
{
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
};

static struct test_semaphore s_lock;
static bool s_lock_initialized;
static int s_recursive_mutex_create_calls;
static atomic_int s_priority_boosts;
static atomic_bool s_force_next_wait_timeout;
static atomic_uint s_fake_next_wait_us;
#ifdef _WIN32
__declspec(thread) static int s_current_priority;
#else
static _Thread_local int s_current_priority;
#endif
static atomic_int s_owner_priority;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (!s_lock_initialized) {
#ifdef _WIN32
        InitializeCriticalSection(&s_lock.mutex);
#else
        assert(pthread_mutex_init(&s_lock.mutex, NULL) == 0);
#endif
        s_lock_initialized = true;
    }
    return &s_lock;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    if (!s_lock_initialized) {
#ifdef _WIN32
        InitializeCriticalSection(&s_lock.mutex);
#else
        pthread_mutexattr_t attributes;
        assert(pthread_mutexattr_init(&attributes) == 0);
        assert(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) == 0);
        assert(pthread_mutex_init(&s_lock.mutex, &attributes) == 0);
        assert(pthread_mutexattr_destroy(&attributes) == 0);
#endif
        s_lock_initialized = true;
    }
    ++s_recursive_mutex_create_calls;
    return &s_lock;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    assert(semaphore == &s_lock);
    assert(ticks_to_wait == portMAX_DELAY);
#ifdef _WIN32
    EnterCriticalSection(&semaphore->mutex);
#else
    assert(pthread_mutex_lock(&semaphore->mutex) == 0);
#endif
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_lock);
#ifdef _WIN32
    LeaveCriticalSection(&semaphore->mutex);
#else
    assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
#endif
    return pdTRUE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    assert(semaphore == &s_lock);
    if (ticks_to_wait != 0U) {
        sensor_bus_lines_delay_us(atomic_exchange(&s_fake_next_wait_us, 0U));
    }
    if (ticks_to_wait == 0U) {
#ifdef _WIN32
        if (!TryEnterCriticalSection(&semaphore->mutex)) return pdFALSE;
        s_owner_priority = s_current_priority;
        return pdTRUE;
#else
        const int result = pthread_mutex_trylock(&semaphore->mutex);
        if (result == EBUSY) return pdFALSE;
        assert(result == 0);
        s_owner_priority = s_current_priority;
        return pdTRUE;
#endif
    }
    assert(ticks_to_wait == pdMS_TO_TICKS(SENSOR_BUS_MUTEX_WAIT_MS));
    if (atomic_exchange(&s_force_next_wait_timeout, false)) {
#ifdef _WIN32
        const bool unexpectedly_acquired = TryEnterCriticalSection(&semaphore->mutex) != 0;
        if (unexpectedly_acquired) {
            LeaveCriticalSection(&semaphore->mutex);
        }
        assert(!unexpectedly_acquired);
#else
        assert(pthread_mutex_trylock(&semaphore->mutex) == EBUSY);
#endif
        sensor_bus_lines_delay_us(SENSOR_BUS_MUTEX_WAIT_MS * 1000U);
        atomic_fetch_add(&s_priority_boosts, 1);
        return pdFALSE;
    }
#ifdef _WIN32
    const ULONGLONG deadline = GetTickCount64() + SENSOR_BUS_MUTEX_WAIT_MS;
    for (;;) {
        if (TryEnterCriticalSection(&semaphore->mutex)) {
            s_owner_priority = s_current_priority;
            return pdTRUE;
        }
        if (s_current_priority > atomic_load(&s_owner_priority)) {
            atomic_store(&s_owner_priority, s_current_priority);
            atomic_fetch_add(&s_priority_boosts, 1);
        }
        if (GetTickCount64() >= deadline) {
            return pdFALSE;
        }
        Sleep(1U);
    }
#else
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += SENSOR_BUS_MUTEX_WAIT_MS / 1000U;
    deadline.tv_nsec += (long)(SENSOR_BUS_MUTEX_WAIT_MS % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;) {
        const int result = pthread_mutex_trylock(&semaphore->mutex);
        if (result == 0) {
            s_owner_priority = s_current_priority;
            return pdTRUE;
        }
        assert(result == EBUSY);
        if (s_current_priority > atomic_load(&s_owner_priority)) {
            atomic_store(&s_owner_priority, s_current_priority);
            atomic_fetch_add(&s_priority_boosts, 1);
        }
        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            return pdFALSE;
        }
        test_sleep_ms(1U);
    }
#endif
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    return xSemaphoreGive(semaphore);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
#ifdef _WIN32
    return (TaskHandle_t)(uintptr_t)GetCurrentThreadId();
#else
    static _Thread_local int task_token;
    return &task_token;
#endif
}

const char *pcTaskGetName(TaskHandle_t task)
{
    (void)task;
    return "i2c-test";
}


/* Fake I2C layer with a reentrancy canary: exactly one caller may be
 * "inside" it at a time. This must hold as long as sensor_bus.c correctly
 * serializes every entry point with its own lock. */
static atomic_int s_active;
static int s_bus_token;
static int s_master_bus_token;
static int s_ina3221_token;
static int s_candidate_tokens[8]; /* one per 0x70..0x77 candidate address */
static uint8_t s_tca_control;
typedef struct {
    uint8_t address;
    bool active;
} fake_master_device_t;
static fake_master_device_t s_master_devices[16];
static atomic_bool s_channel_open;
static atomic_int s_channel_interleavings;
static atomic_bool s_owner_hold_started;
static atomic_uint s_fake_transaction_ms;
static atomic_bool s_hang_next_receive;
static atomic_uint s_receive_resume_delay_ms;
static atomic_int s_last_transaction_timeout_ms;
static atomic_uint s_priority_during_transfer;
static atomic_uint s_master_transfer_calls;
#define TCA_RESPONDING_ADDRESS 0x70U

static void canary_enter(void)
{
    const int now = atomic_fetch_add(&s_active, 1) + 1;
    assert(now == 1);
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER started;
    LARGE_INTEGER current;
    assert(QueryPerformanceFrequency(&frequency));
    assert(QueryPerformanceCounter(&started));
    do {
        SwitchToThread();
        assert(QueryPerformanceCounter(&current));
    } while ((current.QuadPart - started.QuadPart) * 1000LL < frequency.QuadPart);
#else
    test_sleep_ms(1U);
#endif
}

static void canary_exit(void)
{
    atomic_fetch_sub(&s_active, 1);
}

static fake_master_device_t *master_device_of(i2c_master_dev_handle_t device)
{
    fake_master_device_t *fake = device;
    assert(fake != NULL && fake->active);
    return fake;
}

static void master_canary_enter(fake_master_device_t *device)
{
    if (atomic_load(&s_channel_open) && device->address != TCA_RESPONDING_ADDRESS) {
        atomic_fetch_add(&s_channel_interleavings, 1);
    }
    canary_enter();
}

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    (void)port;
    (void)config;
    canary_enter();
    canary_exit();
    return &s_bus_token;
}

i2c_master_bus_handle_t i2c_bus_get_internal_bus_handle(void *bus)
{
    assert(bus == &s_bus_token);
    return &s_master_bus_token;
}

esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device)
{
    assert(bus == &s_master_bus_token);
    assert(config != NULL && device != NULL);
    canary_enter();
    for (size_t i = 0U; i < sizeof(s_master_devices) / sizeof(s_master_devices[0]); ++i) {
        if (!s_master_devices[i].active) {
            s_master_devices[i].address = (uint8_t)config->device_address;
            s_master_devices[i].active = true;
            *device = &s_master_devices[i];
            canary_exit();
            return ESP_OK;
        }
    }
    canary_exit();
    return ESP_ERR_NO_MEM;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    fake_master_device_t *fake = master_device_of(device);
    canary_enter();
    fake->active = false;
    canary_exit();
    return ESP_OK;
}

esp_err_t i2c_master_receive(
    i2c_master_dev_handle_t device, uint8_t *data, size_t data_len, int timeout_ms)
{
    fake_master_device_t *fake = master_device_of(device);
    assert(data != NULL && data_len > 0U);
    atomic_store(&s_last_transaction_timeout_ms, timeout_ms);
    atomic_store(&s_priority_during_transfer, atomic_load(&g_fake_lines.task_priority));
    atomic_fetch_add(&s_master_transfer_calls, 1U);
    master_canary_enter(fake);
    memset(data, fake->address == TCA_RESPONDING_ADDRESS ? s_tca_control : 0U, data_len);
    if (atomic_exchange(&s_hang_next_receive, false)) {
        sensor_bus_lines_delay_us((uint32_t)timeout_ms * 1000U);
        sensor_bus_lines_delay_us(atomic_load(&s_receive_resume_delay_ms) * 1000U);
        canary_exit();
        return ESP_ERR_TIMEOUT;
    }
    const unsigned transaction_ms = atomic_load(&s_fake_transaction_ms);
    sensor_bus_lines_delay_us(transaction_ms * 1000U);
    canary_exit();
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms)
{
    (void)write_buffer;
    (void)write_size;
    return i2c_master_receive(device, read_buffer, read_size, timeout_ms);
}

esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device,
    const uint8_t *data,
    size_t data_len,
    int timeout_ms)
{
    fake_master_device_t *fake = master_device_of(device);
    assert(data != NULL && data_len > 0U);
    atomic_store(&s_last_transaction_timeout_ms, timeout_ms);
    atomic_store(&s_priority_during_transfer, atomic_load(&g_fake_lines.task_priority));
    atomic_fetch_add(&s_master_transfer_calls, 1U);
    master_canary_enter(fake);
    if (fake->address == TCA_RESPONDING_ADDRESS) {
        s_tca_control = data[0];
    }
    const unsigned transaction_ms = atomic_load(&s_fake_transaction_ms);
    sensor_bus_lines_delay_us(transaction_ms * 1000U);
    canary_exit();
    return ESP_OK;
}

esp_err_t i2c_bus_delete(i2c_bus_handle_t *bus)
{
    canary_enter();
    assert(bus != NULL && *bus == &s_bus_token);
    *bus = NULL;
    canary_exit();
    return ESP_OK;
}

i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t speed_hz)
{
    (void)speed_hz;
    canary_enter();
    assert(bus == &s_bus_token);
    i2c_bus_device_handle_t result;
    if (address == BOARD_INA3221_I2C_ADDRESS) {
        result = &s_ina3221_token;
    } else {
        assert(address >= TCA_RESPONDING_ADDRESS && address < TCA_RESPONDING_ADDRESS + 8U);
        result = &s_candidate_tokens[address - TCA_RESPONDING_ADDRESS];
    }
    canary_exit();
    return result;
}

esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device)
{
    canary_enter();
    assert(device != NULL && *device != NULL);
    *device = NULL;
    canary_exit();
    return ESP_OK;
}

esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data)
{
    (void)mem_address;
    canary_enter();
    const bool responds = device == &s_candidate_tokens[0]; /* only 0x70 */
    if (data != NULL) {
        *data = responds ? s_tca_control : 0U;
    }
    canary_exit();
    /* Only the first discovery candidate (0x70) responds, so discovery
     * always resolves deterministically to one device. */
    return responds ? ESP_OK : ESP_FAIL;
}

esp_err_t i2c_bus_write_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t data)
{
    (void)mem_address;
    canary_enter();
    assert(device == &s_candidate_tokens[0]);
    s_tca_control = data;
    canary_exit();
    return ESP_OK;
}


#define ITERATIONS 150U

/* This fixture has a responding TCA at 0x70 and immediate address-only NACKs
 * at the other seven candidates. Discovery therefore holds the state mutex
 * only for bounded handle setup plus six control-register transfers; deinit
 * only removes owned handles. The 1 ms reentrancy canary is high-resolution
 * on Windows so Sleep(1)'s ~15 ms scheduler rounding cannot inflate that
 * short scope. Any MUTEX_BUSY in these 150 overlapping API calls is a test
 * failure; the separate forced waiter below checks the real 200 ms bound. */
static atomic_int s_deinit_countdown = 25;

#ifdef _WIN32
static DWORD WINAPI ina3221_like_worker_fn(LPVOID arg)
#else
static void *ina3221_like_worker_fn(void *arg)
#endif
{
    (void)arg;
    unsigned successful_reads = 0U;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        sensor_bus_handles_t handles = {0};
        const esp_err_t err = sensor_bus_get_handles(&handles);
        /* Require zero busy returns in all 150 calls while discovery and
         * periodic teardown overlap; see the bounded fake-device model above. */
        assert(err == ESP_OK);
        assert(handles.bus == &s_bus_token);
        assert(handles.ina3221 != NULL);
        ++successful_reads;
    }
    assert(successful_reads > 0U);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI topology_like_worker_fn(LPVOID arg)
#else
static void *topology_like_worker_fn(void *arg)
#endif
{
    (void)arg;
    unsigned successful_queries = 0U;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        i2c_bus_device_handle_t device = NULL;
        uint8_t address = 0U;
        const esp_err_t err = sensor_bus_get_tca9548a(&device, &address);
        assert(err == ESP_OK);
        assert(device != NULL);
        assert(address == 0x70U);
        ++successful_queries;

        if (atomic_fetch_sub(&s_deinit_countdown, 1) == 1) {
            atomic_store(&s_deinit_countdown, 25);
            (void)sensor_bus_deinit();
        }
    }
    assert(successful_queries > 0U);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

typedef struct {
    i2c_bus_device_handle_t tca;
    i2c_bus_device_handle_t ina;
    atomic_bool channel_open;
    esp_err_t waiter_result;
    sensor_bus_owner_stats_t waiter_stats;
    uint64_t waiter_wait_us;
    char waiter_log[512];
} owner_test_context_t;

#ifdef _WIN32
static DWORD WINAPI tca_sequence_owner_fn(LPVOID arg)
#else
static void *tca_sequence_owner_fn(void *arg)
#endif
{
    owner_test_context_t *context = arg;
    s_current_priority = 2;
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(sensor_bus_write_byte(context->tca, NULL_I2C_MEM_ADDR, 0x01U) == ESP_OK);
    atomic_store(&s_channel_open, true);
    atomic_store(&context->channel_open, true);
    test_sleep_ms(30U);
    assert(sensor_bus_write_byte(context->tca, NULL_I2C_MEM_ADDR, 0U) == ESP_OK);
    atomic_store(&s_channel_open, false);
    sensor_bus_owner_release();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI ina_waiter_fn(LPVOID arg)
#else
static void *ina_waiter_fn(void *arg)
#endif
{
    owner_test_context_t *context = arg;
    s_current_priority = 3;
    while (!atomic_load(&context->channel_open)) {
        test_sleep_ms(1U);
    }
    uint8_t bytes[2] = {0U, 0U};
    assert(sensor_bus_read_bytes(context->ina, 0U, sizeof(bytes), bytes) == ESP_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI long_owner_fn(LPVOID arg)
#else
static void *long_owner_fn(void *arg)
#endif
{
    (void)arg;
    s_current_priority = 1;
    assert(sensor_bus_owner_acquire() == ESP_OK);
    atomic_store(&s_owner_hold_started, true);
    test_sleep_ms(260U);
    sensor_bus_owner_release();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI bounded_waiter_fn(LPVOID arg)
#else
static void *bounded_waiter_fn(void *arg)
#endif
{
    owner_test_context_t *context = arg;
    s_current_priority = 5;
    while (!atomic_load(&context->channel_open)) {
        test_sleep_ms(1U);
    }
    context->waiter_result = sensor_bus_owner_acquire();
    assert(context->waiter_result == SENSOR_BUS_ERR_MUTEX_BUSY);
    (void)strncpy(context->waiter_log, g_fake_lines.last_log,
                  sizeof(context->waiter_log) - 1U);
    context->waiter_log[sizeof(context->waiter_log) - 1U] = '\0';
    assert(sensor_bus_get_owner_stats(&context->waiter_stats) == ESP_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI try_waiter_fn(LPVOID arg)
#else
static void *try_waiter_fn(void *arg)
#endif
{
    owner_test_context_t *context = arg;
    s_current_priority = 1;
    const int64_t started_us = sensor_bus_lines_now_us();
    context->waiter_result = sensor_bus_owner_try_acquire();
    const int64_t waited_us = sensor_bus_lines_now_us() - started_us;
    context->waiter_wait_us = waited_us > 0 ? (uint64_t)waited_us : 0U;
    assert(sensor_bus_get_owner_stats(&context->waiter_stats) == ESP_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_owner_try_acquire_does_not_wait(void)
{
    owner_test_context_t context = {0};
    atomic_store(&s_owner_hold_started, false);
    test_thread_t owner = test_thread_start(long_owner_fn, NULL);
    while (!atomic_load(&s_owner_hold_started)) test_sleep_ms(1U);
    test_thread_t waiter = test_thread_start(try_waiter_fn, &context);
    test_thread_join(waiter);
    test_thread_join(owner);
    assert(context.waiter_result == SENSOR_BUS_ERR_MUTEX_BUSY);
    assert(context.waiter_wait_us < 5000U);
    assert(context.waiter_stats.last_waited_for_mutex_us < 5000U);
    assert(context.waiter_stats.last_waiter_task[0] != '\0');
    assert(strcmp(context.waiter_stats.holder_task, "i2c-test") == 0);
}

static void test_ina_transfer_cannot_interleave_inside_open_tca_channel(void)
{
    owner_test_context_t context = {0};
    sensor_bus_handles_t handles = {0};
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    context.ina = handles.ina3221;
    assert(sensor_bus_get_tca9548a(&context.tca, NULL) == ESP_OK);
    atomic_store(&s_channel_open, false);
    atomic_store(&s_channel_interleavings, 0);

    test_thread_t owner = test_thread_start(tca_sequence_owner_fn, &context);
    while (!atomic_load(&context.channel_open)) {
        test_sleep_ms(1U);
    }
    test_thread_t waiter = test_thread_start(ina_waiter_fn, &context);
    test_thread_join(owner);
    test_thread_join(waiter);
    assert(atomic_load(&s_channel_interleavings) == 0);
}

static void test_bounded_wait_reports_owner_and_priority_inheritance(void)
{
    owner_test_context_t context = {0};
    const unsigned transfer_calls_before = atomic_load(&s_master_transfer_calls);
    atomic_store(&s_owner_hold_started, false);
    assert(s_recursive_mutex_create_calls == 1);

    test_thread_t owner = test_thread_start(long_owner_fn, &context);
    while (!atomic_load(&s_owner_hold_started)) {
        test_sleep_ms(1U);
    }
    atomic_store(&context.channel_open, true);
    atomic_store(&s_force_next_wait_timeout, true);
    test_thread_t waiter = test_thread_start(bounded_waiter_fn, &context);
    test_thread_join(waiter);
    test_thread_join(owner);

    assert(context.waiter_stats.busy_count > 0U);
    assert(strcmp(context.waiter_stats.holder_task, "i2c-test") == 0);
    assert(atomic_load(&s_priority_boosts) > 0);
    assert(atomic_load(&s_master_transfer_calls) == transfer_calls_before);
    assert(strstr(context.waiter_log, "err=mutex_busy") != NULL);
    assert(strstr(context.waiter_log, "holder_task=i2c-test") != NULL);
    const unsigned long long waited_us =
        strtoull(strstr(context.waiter_log, "waited_for_mutex_us=") +
                 strlen("waited_for_mutex_us="), NULL, 10);
    if (waited_us < 190000U || waited_us > 210000U) {
        (void)fprintf(stderr, "B2 fake mutex wait out of range: %llu us (%s)\n",
                      waited_us, context.waiter_log);
    }
    assert(waited_us >= 190000U && waited_us <= 210000U);
}

static esp_err_t s_timeout_log_peer_result;
static bool s_timeout_log_peer_checked;

#ifdef _WIN32
static DWORD WINAPI timeout_log_peer(void *context)
#else
static void *timeout_log_peer(void *context)
#endif
{
    (void)context;
    sensor_bus_handles_t handles = {0};
    s_timeout_log_peer_result = sensor_bus_get_handles(&handles);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void observe_timeout_log_unlocked(const char *line)
{
    if (strstr(line, "I2C_TIMEOUT_DIAG src=ina3221") == NULL ||
        (strstr(line, "transfer_us=") == NULL &&
         strstr(line, "err=ESP_ERR_INVALID_STATE") == NULL)) return;
    g_fake_lines.log_observer = NULL;
    test_thread_join(test_thread_start(timeout_log_peer, NULL));
    assert(s_timeout_log_peer_result == ESP_OK);
    s_timeout_log_peer_checked = true;
}

static void test_transaction_timeout_is_20ms_and_recovery_is_unlocked(void)
{
    sensor_bus_handles_t handles = {0};
    uint8_t bytes[2] = {0U, 0U};
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    g_fake_lines.log_observer = observe_timeout_log_unlocked;
    const int64_t started_us = sensor_bus_lines_now_us();
    atomic_store(&s_hang_next_receive, true);
    assert(sensor_bus_read_bytes(
        handles.ina3221, NULL_I2C_MEM_ADDR, sizeof(bytes), bytes) == ESP_ERR_TIMEOUT);
    assert(s_timeout_log_peer_checked);
    const int64_t held_us = sensor_bus_lines_now_us() - started_us;
    assert(atomic_load(&s_last_transaction_timeout_ms) == SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
    assert(held_us >= 15000 && held_us <= 25000);

    /* sensor_bus_recover() acquires the state lock itself; success proves the
     * timed-out INA read released it before recovery starts. One 20 ms read
     * is below B3's two-transfer hold ceiling. */
    i2c_line_result_t recovery = I2C_LINES_IDLE;
    assert(held_us <= 2 * (int64_t)SENSOR_BUS_TRANSACTION_TIMEOUT_MS * 1000);
    /* Earlier worker restarts exhausted the recovery backoff window. */
    sensor_bus_lines_delay_us(60000000U);
    assert(sensor_bus_recover(&recovery) == ESP_OK);
    assert(recovery == I2C_LINES_IDLE);

    const bool probe_configured = g_fake_lines.probe_configured;
    const int probe_result = g_fake_lines.probe_result[0x40U];
    g_fake_lines.probe_configured = true;
    g_fake_lines.probe_result[0x40U] = ESP_OK;
    s_timeout_log_peer_checked = false;
    g_fake_lines.log_observer = observe_timeout_log_unlocked;
    assert(sensor_bus_classify_transfer_error_with_context(
        0x40U, ESP_ERR_INVALID_STATE, "ina3221", -1) == ESP_ERR_INVALID_STATE);
    assert(s_timeout_log_peer_checked);
    g_fake_lines.probe_configured = probe_configured;
    g_fake_lines.probe_result[0x40U] = probe_result;

    assert(sensor_bus_read_bytes_with_timeout(
        handles.ina3221, NULL_I2C_MEM_ADDR, sizeof(bytes), bytes, 100U) == ESP_OK);
    assert(atomic_load(&s_last_transaction_timeout_ms) == 100);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(sensor_bus_read_bytes_with_timeout(
        handles.ina3221, NULL_I2C_MEM_ADDR, sizeof(bytes), bytes, 120U) ==
        SENSOR_BUS_ERR_OWNER_HOLD_BUDGET);
    assert(atomic_load(&s_last_transaction_timeout_ms) == 100);
    sensor_bus_owner_release();
}

static void prepare_mux_fixture(
    tca9548a_mux_t *mux,
    i2c_bus_device_handle_t *remote,
    sensor_bus_handles_t *handles)
{
    i2c_bus_device_handle_t tca = NULL;
    assert(sensor_bus_get_handles(handles) == ESP_OK);
    assert(sensor_bus_get_tca9548a(&tca, NULL) == ESP_OK);
    assert(sensor_bus_device_create(
        handles->bus, 0x32U, SENSOR_BUS_I2C_SPEED_HZ, remote) == ESP_OK);
    assert(tca9548a_mux_bind_i2c_bus(mux, tca, TCA_RESPONDING_ADDRESS) == ESP_OK);
}

static void test_hold_budget_aborts_closes_mux_without_fault(void)
{
    sensor_bus_handles_t handles = {0};
    i2c_bus_device_handle_t remote = NULL;
    tca9548a_mux_t mux = {0};
    uint8_t value = 0U;
    prepare_mux_fixture(&mux, &remote, &handles);
    atomic_store(&s_fake_transaction_ms, SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(tca9548a_mux_select(&mux, 3U) == ESP_OK);
    assert(sensor_bus_read_byte(remote, NULL_I2C_MEM_ADDR, &value) == ESP_OK);

    /* A second 20 ms downstream transfer cannot fit with the 40 ms close
     * reserve under the 150 ms channel-sequence budget. */
    assert(sensor_bus_read_byte(remote, NULL_I2C_MEM_ADDR, &value) ==
           SENSOR_BUS_ERR_OWNER_HOLD_BUDGET);
    assert(sensor_bus_owner_hold_budget_exceeded());
    assert(tca9548a_mux_close(&mux) == ESP_OK);
    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_NONE);
    assert(s_tca_control == 0U);
    sensor_bus_owner_release();

    sensor_bus_owner_stats_t stats = {0};
    assert(sensor_bus_get_owner_stats(&stats) == ESP_OK);
    assert(stats.last_hold_us <= (uint64_t)SENSOR_BUS_OWNER_MAX_HOLD_MS * 1000U);
    assert(g_fake_lines.hold_budget_exceeded_logged);
    assert(sensor_bus_deinit() == ESP_OK);
    atomic_store(&s_fake_transaction_ms, 0U);
}

static void test_hold_budget_preserves_driver_timeout(void)
{
    sensor_bus_handles_t handles = {0};
    i2c_bus_device_handle_t remote = NULL;
    tca9548a_mux_t mux = {0};
    uint8_t value = 0U;
    prepare_mux_fixture(&mux, &remote, &handles);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(tca9548a_mux_select(&mux, 3U) == ESP_OK);
    atomic_store(&s_receive_resume_delay_ms, 100U);
    atomic_store(&s_hang_next_receive, true);
    assert(sensor_bus_read_byte(remote, NULL_I2C_MEM_ADDR, &value) == ESP_ERR_TIMEOUT);
    assert(sensor_bus_owner_hold_budget_exceeded());
    atomic_store(&s_receive_resume_delay_ms, 0U);
    assert(tca9548a_mux_close(&mux) == ESP_OK);
    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);
    assert(s_tca_control == 0U);
    sensor_bus_owner_release();
    assert(sensor_bus_deinit() == ESP_OK);
}

typedef struct {
    tca9548a_mux_t *mux;
    i2c_bus_device_handle_t remote;
    atomic_bool first_channel_started;
    atomic_bool waiter_started;
    atomic_bool waiter_acquired;
    atomic_uint completed_channels;
    uint64_t waiter_wait_us;
    bool waiter_got_bus_between_channels;
    i2c_bus_device_handle_t main;
    i2c_bus_device_handle_t ina;
    uint64_t waiter_wall_wait_ms;
} scan_waiter_context_t;

#ifdef _WIN32
static DWORD WINAPI eight_channel_scan_fn(LPVOID arg)
#else
static void *eight_channel_scan_fn(void *arg)
#endif
{
    scan_waiter_context_t *context = arg;
    uint8_t value = 0U;
    for (uint8_t channel = 0U; channel < 8U; ++channel) {
        assert(sensor_bus_owner_acquire() == ESP_OK);
        if (channel == 0U) {
            atomic_store(&context->first_channel_started, true);
            while (!atomic_load(&context->waiter_started)) {
                test_sleep_ms(1U);
            }
            /* Hold the first channel for the fake transaction budget in both
             * fake time and wall time, so the peer is definitely contending. */
            sensor_bus_lines_delay_us((SENSOR_BUS_TRANSACTION_TIMEOUT_MS + 5U) * 1000U);
            test_sleep_ms(SENSOR_BUS_TRANSACTION_TIMEOUT_MS + 5U);
        }
        assert(tca9548a_mux_select(context->mux, channel) == ESP_OK);
        assert(sensor_bus_read_byte(context->remote, NULL_I2C_MEM_ADDR, &value) == ESP_OK);
        assert(tca9548a_mux_close(context->mux) == ESP_OK);
        atomic_store(&context->completed_channels, (unsigned)channel + 1U);
        sensor_bus_owner_release();
        if (channel == 0U) {
            while (!atomic_load(&context->waiter_acquired)) {
                test_sleep_ms(1U);
            }
        }
        /* Give a waiting peer a full scheduling window after each bounded
         * channel sequence before starting the next one. */
        test_sleep_ms(50U);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI scan_bus_waiter_fn(LPVOID arg)
#else
static void *scan_bus_waiter_fn(void *arg)
#endif
{
    scan_waiter_context_t *context = arg;
    while (!atomic_load(&context->first_channel_started)) {
        test_sleep_ms(1U);
    }
    atomic_store(&context->waiter_started, true);
    s_current_priority = 1;
#ifdef _WIN32
    const uint64_t wall_started_ms = GetTickCount64();
#else
    struct timespec wall_started;
    assert(clock_gettime(CLOCK_MONOTONIC, &wall_started) == 0);
    const uint64_t wall_started_ms = (uint64_t)wall_started.tv_sec * 1000U + (uint64_t)wall_started.tv_nsec / 1000000U;
#endif
    const int64_t started_us = sensor_bus_lines_now_us();
    assert(sensor_bus_owner_acquire() == ESP_OK);
    const int64_t waited_us = sensor_bus_lines_now_us() - started_us;
#ifdef _WIN32
    context->waiter_wall_wait_ms = GetTickCount64() - wall_started_ms;
#else
    struct timespec wall_finished;
    assert(clock_gettime(CLOCK_MONOTONIC, &wall_finished) == 0);
    context->waiter_wall_wait_ms = (uint64_t)wall_finished.tv_sec * 1000U +
        (uint64_t)wall_finished.tv_nsec / 1000000U - wall_started_ms;
#endif
    if (context->ina != NULL) {
        uint8_t registers[2];
        assert(s_tca_control == 0U);
        assert(sensor_bus_read_bytes(context->ina, NULL_I2C_MEM_ADDR, sizeof(registers), registers) == ESP_OK);
    }
    context->waiter_wait_us = waited_us > 0 ? (uint64_t)waited_us : 0U;
    const unsigned completed = atomic_load(&context->completed_channels);
    context->waiter_got_bus_between_channels = completed == 1U;
    sensor_bus_owner_release();
    atomic_store(&context->waiter_acquired, true);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_topology_waiter_gets_bus_between_channels(void)
{
    sensor_bus_handles_t handles = {0};
    i2c_bus_device_handle_t remote = NULL;
    tca9548a_mux_t mux = {0};
    scan_waiter_context_t context = { .mux = &mux };
    prepare_mux_fixture(&mux, &remote, &handles);
    context.remote = remote;
    atomic_store(&s_fake_transaction_ms, 0U);
    test_thread_t scan = test_thread_start(eight_channel_scan_fn, &context);
    while (!atomic_load(&context.first_channel_started)) {
        test_sleep_ms(1U);
    }
    test_thread_t waiter = test_thread_start(scan_bus_waiter_fn, &context);
    test_thread_join(waiter);
    test_thread_join(scan);
    assert(context.waiter_got_bus_between_channels);
    assert(context.waiter_wait_us <=
           (uint64_t)(SENSOR_BUS_OWNER_MAX_HOLD_MS + 20U) * 1000U);
    assert(s_tca_control == 0U);
    assert(sensor_bus_deinit() == ESP_OK);
    atomic_store(&s_fake_transaction_ms, 0U);
}

#ifdef _WIN32
static DWORD WINAPI three_node_log_pairs_fn(LPVOID arg)
#else
static void *three_node_log_pairs_fn(void *arg)
#endif
{
    scan_waiter_context_t *context = arg;
    const uint8_t channels[] = {UINT8_MAX, 0U, 3U};
    uint8_t frame[136] = {0};
    s_current_priority = 2;
    for (unsigned node = 0U; node < 3U; ++node) {
        assert(sensor_bus_owner_acquire() == ESP_OK);
        if (node == 0U) {
            assert(tca9548a_mux_close(context->mux) == ESP_OK);
            atomic_store(&context->first_channel_started, true);
            while (!atomic_load(&context->waiter_started)) test_sleep_ms(1U);
        } else {
            assert(tca9548a_mux_select(context->mux, channels[node]) == ESP_OK);
        }
        const i2c_bus_device_handle_t device = node == 0U ? context->main : context->remote;
        assert(sensor_bus_write_bytes(device, NULL_I2C_MEM_ADDR, 10U, frame) == ESP_OK);
        assert(sensor_bus_read_bytes(device, NULL_I2C_MEM_ADDR, sizeof(frame), frame) == ESP_OK);
        assert(tca9548a_mux_close(context->mux) == ESP_OK);
        atomic_store(&context->completed_channels, node + 1U);
        sensor_bus_owner_release();
        test_sleep_ms(10U); /* The production logger blocks for one 100 Hz tick. */
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_waiting_ina_runs_between_main_ch0_ch3_log_pairs(void)
{
    sensor_bus_handles_t handles = {0};
    tca9548a_mux_t mux = {0};
    scan_waiter_context_t context = {.mux = &mux};
    prepare_mux_fixture(&mux, &context.remote, &handles);
    assert(sensor_bus_device_create(handles.bus, 0x31U, SENSOR_BUS_I2C_SPEED_HZ, &context.main) == ESP_OK);
    context.ina = handles.ina3221;
    atomic_store(&s_fake_transaction_ms, 1U);
    test_thread_t logger = test_thread_start(three_node_log_pairs_fn, &context);
    test_thread_t ina = test_thread_start(scan_bus_waiter_fn, &context);
    test_thread_join(ina);
    test_thread_join(logger);
    assert(context.waiter_got_bus_between_channels);
    assert(atomic_load(&context.completed_channels) == 3U);
    assert(context.waiter_wait_us > 0U && context.waiter_wait_us < 50000U);
    assert(context.waiter_wall_wait_ms < 250U); /* Host scheduler guard, not an RTOS P99 claim. */
    assert(s_tca_control == 0U);
    atomic_store(&s_fake_transaction_ms, 0U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static void test_owner_deadline_bounds_driver_and_clears_on_release(void)
{
    sensor_bus_handles_t handles = {0};
    i2c_bus_device_handle_t remote = NULL;
    tca9548a_mux_t mux = {0};
    uint8_t value = 0U;
    prepare_mux_fixture(&mux, &remote, &handles);
    assert(sensor_bus_owner_set_deadline_us(sensor_bus_lines_now_us() + 15000) == ESP_ERR_INVALID_STATE);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(tca9548a_mux_select(&mux, 0U) == ESP_OK);
    assert(sensor_bus_owner_set_deadline_us(0) == ESP_ERR_INVALID_ARG);
    assert(sensor_bus_owner_set_deadline_us(sensor_bus_lines_now_us() + 15000) == ESP_OK);
    assert(sensor_bus_write_bytes(remote, NULL_I2C_MEM_ADDR, 1U, &value) == ESP_OK);
    assert(atomic_load(&s_last_transaction_timeout_ms) == 15);
    sensor_bus_lines_delay_us(7000U);
    const unsigned transfers = atomic_load(&s_master_transfer_calls);
    assert(sensor_bus_read_byte(remote, NULL_I2C_MEM_ADDR, &value) == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET);
    assert(atomic_load(&s_master_transfer_calls) == transfers);
    assert(sensor_bus_owner_set_deadline_us(sensor_bus_lines_now_us() + 40000) == ESP_OK);
    assert(tca9548a_mux_close(&mux) == ESP_OK && s_tca_control == 0U);
    sensor_bus_owner_release();
    assert(sensor_bus_read_byte(handles.ina3221, NULL_I2C_MEM_ADDR, &value) == ESP_OK);
    assert(atomic_load(&s_last_transaction_timeout_ms) == SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
    assert(sensor_bus_deinit() == ESP_OK);
}

/*
 * Measured on the board: 136-byte nRF reads take 12.5-14.3 ms at 100 kHz and
 * 40 of 50 were aborted by the fixed 20 ms timeout, which a 100 Hz tick only
 * guarantees for 10 ms. Every default transfer must wait at least twice its
 * wire time, whatever its length and whether or not it addresses a register.
 */
static void test_default_transfer_timeout_covers_the_wire_time(void)
{
    static const size_t lengths[] = {1U, 2U, 28U, 38U, 39U, 56U, 76U, 96U, 116U, 136U, 200U};
    sensor_bus_handles_t handles = {0};
    uint8_t buffer[256] = {0};

    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned variant = 0U; variant < 3U; ++variant) {
            /* 0: plain read, 1: register read (write + read), 2: plain write. */
            size_t wire_bytes = length;
            if (variant == 0U) {
                assert(sensor_bus_read_bytes(
                    handles.ina3221, NULL_I2C_MEM_ADDR, length, buffer) == ESP_OK);
            } else if (variant == 1U) {
                assert(sensor_bus_read_bytes(handles.ina3221, 0x10U, length, buffer) == ESP_OK);
                wire_bytes = length + 2U;
            } else {
                assert(sensor_bus_write_bytes(
                    handles.ina3221, NULL_I2C_MEM_ADDR, length, buffer) == ESP_OK);
            }
            const uint32_t timeout_ms = (uint32_t)atomic_load(&s_last_transaction_timeout_ms);
            const uint64_t required_ms =
                ((uint64_t)sensor_bus_wire_time_us(wire_bytes, SENSOR_BUS_I2C_SPEED_HZ) *
                     SENSOR_BUS_TIMEOUT_WIRE_MARGIN_PERCENT + 99999U) / 100000U;
            assert(timeout_ms >= SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
            assert(sensor_bus_wait_floor_ms(timeout_ms, configTICK_RATE_HZ) >= required_ms);
            if (length <= 38U) {
                assert(timeout_ms == SENSOR_BUS_TRANSACTION_TIMEOUT_MS);
            }
            if (length == 136U && variant == 0U) {
                /* The longest transfer that succeeded on the board lasted 14.273 ms. */
                assert(sensor_bus_wait_floor_ms(timeout_ms, configTICK_RATE_HZ) * 1000U >= 14273U);
            }
        }
    }

    /* An explicit caller timeout stays exactly as requested. */
    assert(sensor_bus_read_bytes_with_timeout(
        handles.ina3221, NULL_I2C_MEM_ADDR, 136U, buffer, 25U) == ESP_OK);
    assert(atomic_load(&s_last_transaction_timeout_ms) == 25);
    assert(sensor_bus_deinit() == ESP_OK);
}

/*
 * The IDF master driver executes transfers of 31 bytes or more in FIFO-sized
 * segments and, in blocking mode, the calling task must run at every segment
 * boundary. The topology task is priority 2 on the saturated service core, so
 * the caller is raised for the duration of such a transfer only.
 */
static void test_segmented_transfers_raise_the_caller_priority_only_while_they_run(void)
{
    static const size_t lengths[] = {1U, 28U, 30U, 31U, 32U, 38U, 56U, 136U, 200U};
    sensor_bus_handles_t handles = {0};
    uint8_t buffer[256] = {0};

    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    assert(atomic_load(&g_fake_lines.task_priority) == 2U);
    for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        const size_t length = lengths[i];
        for (unsigned variant = 0U; variant < 3U; ++variant) {
            /* 0: plain read, 1: register read, 2: write. */
            const unsigned boosts_before = atomic_load(&g_fake_lines.priority_boosts);
            const unsigned restores_before = atomic_load(&g_fake_lines.priority_restores);
            size_t wire_bytes = length;
            if (variant == 0U) {
                assert(sensor_bus_read_bytes(
                    handles.ina3221, NULL_I2C_MEM_ADDR, length, buffer) == ESP_OK);
            } else if (variant == 1U) {
                assert(sensor_bus_read_bytes(handles.ina3221, 0x10U, length, buffer) == ESP_OK);
                wire_bytes = length + 2U;
            } else {
                assert(sensor_bus_write_bytes(
                    handles.ina3221, NULL_I2C_MEM_ADDR, length, buffer) == ESP_OK);
            }
            const bool segmented = wire_bytes >= SENSOR_BUS_SEGMENTED_TRANSFER_BYTES;
            assert(atomic_load(&s_priority_during_transfer) ==
                   (segmented ? SENSOR_BUS_SEGMENTED_TRANSFER_PRIORITY : 2U));
            assert(atomic_load(&g_fake_lines.task_priority) == 2U);
            assert(atomic_load(&g_fake_lines.priority_boosts) == boosts_before + (segmented ? 1U : 0U));
            assert(atomic_load(&g_fake_lines.priority_restores) == restores_before + (segmented ? 1U : 0U));
        }
    }

    /* A failed or timed-out transfer gives the priority back as well. */
    atomic_store(&s_hang_next_receive, true);
    assert(sensor_bus_read_bytes(handles.ina3221, NULL_I2C_MEM_ADDR, 136U, buffer) == ESP_ERR_TIMEOUT);
    assert(atomic_load(&s_priority_during_transfer) == SENSOR_BUS_SEGMENTED_TRANSFER_PRIORITY);
    assert(atomic_load(&g_fake_lines.task_priority) == 2U);

    /* A caller that already runs above the boost is left alone. */
    atomic_store(&g_fake_lines.task_priority, SENSOR_BUS_SEGMENTED_TRANSFER_PRIORITY + 1U);
    assert(sensor_bus_read_bytes(handles.ina3221, NULL_I2C_MEM_ADDR, 136U, buffer) == ESP_OK);
    assert(atomic_load(&s_priority_during_transfer) == SENSOR_BUS_SEGMENTED_TRANSFER_PRIORITY + 1U);
    assert(atomic_load(&g_fake_lines.task_priority) == SENSOR_BUS_SEGMENTED_TRANSFER_PRIORITY + 1U);
    atomic_store(&g_fake_lines.task_priority, 2U);
    assert(sensor_bus_deinit() == ESP_OK);
}

/*
 * Logs from the board (2026-10-26 20:02) show 1-byte TCA9548A transfers timing out
 * with no interrupt pending, an idle controller and commands still unissued: the
 * priority-2 topology task did not get the core (cpu0 97.5 %) for the whole
 * timeout. A bus-owner session therefore runs at SENSOR_BUS_SESSION_PRIORITY from
 * the outermost acquire to the outermost release, whatever the transfer sizes.
 */
static void test_owner_session_runs_at_the_boosted_priority(void)
{
    sensor_bus_handles_t handles = {0};
    uint8_t buffer[4] = {0};
    uint8_t large[136] = {0};

    atomic_store(&g_fake_lines.task_priority, 2U);
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    const unsigned boosts_before = atomic_load(&g_fake_lines.priority_boosts);
    const unsigned restores_before = atomic_load(&g_fake_lines.priority_restores);

    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(atomic_load(&g_fake_lines.task_priority) == SENSOR_BUS_SESSION_PRIORITY);
    assert(sensor_bus_read_bytes(handles.ina3221, NULL_I2C_MEM_ADDR, 2U, buffer) == ESP_OK);
    assert(atomic_load(&s_priority_during_transfer) == SENSOR_BUS_SESSION_PRIORITY);

    /* A nested session neither boosts again nor gives the priority back early. */
    assert(sensor_bus_owner_acquire() == ESP_OK);
    assert(atomic_load(&g_fake_lines.priority_boosts) == boosts_before + 1U);
    sensor_bus_owner_release();
    assert(atomic_load(&g_fake_lines.task_priority) == SENSOR_BUS_SESSION_PRIORITY);
    assert(atomic_load(&g_fake_lines.priority_restores) == restores_before);

    /* A long transfer inside the session does not restore the session priority either. */
    assert(sensor_bus_read_bytes(handles.ina3221, NULL_I2C_MEM_ADDR, sizeof(large), large) == ESP_OK);
    assert(atomic_load(&g_fake_lines.task_priority) == SENSOR_BUS_SESSION_PRIORITY);

    sensor_bus_owner_release();
    assert(atomic_load(&g_fake_lines.task_priority) == 2U);
    assert(atomic_load(&g_fake_lines.priority_boosts) == boosts_before + 1U);
    assert(atomic_load(&g_fake_lines.priority_restores) == restores_before + 1U);

    /* try_acquire is a session as well. */
    assert(sensor_bus_owner_try_acquire() == ESP_OK);
    assert(atomic_load(&g_fake_lines.task_priority) == SENSOR_BUS_SESSION_PRIORITY);
    sensor_bus_owner_release();
    assert(atomic_load(&g_fake_lines.task_priority) == 2U);
    assert(sensor_bus_deinit() == ESP_OK);
}

static unsigned long long parse_log_ms(const char *line, const char *field)
{
    const char *value = strstr(line, field);
    assert(value != NULL);
    return strtoull(value + strlen(field), NULL, 10);
}

static void test_owner_logs_match_fake_clock(void)
{
    const int64_t started_us = sensor_bus_lines_now_us();
    assert(sensor_bus_owner_acquire() == ESP_OK);
    const uint64_t waited_us = (uint64_t)(sensor_bus_lines_now_us() - started_us);
    sensor_bus_lines_delay_us(37000U);
    sensor_bus_owner_release();
    assert(strstr(g_fake_lines.last_log, "I2C_BUS_OWNER owner=") != NULL);
    const unsigned long long wait_ms = parse_log_ms(g_fake_lines.last_log, "wait_ms=");
    const unsigned long long hold_ms = parse_log_ms(g_fake_lines.last_log, "hold_ms=");
    const uint64_t logged_wait_us = wait_ms * 1000U;
    assert(waited_us <= 1000U);
    assert(logged_wait_us <= 1U);
    assert(hold_ms >= 36U && hold_ms <= 38U);
}

/* A healthy session must not touch the UART: the line is per window read on the topology task. */
static void test_owner_release_is_quiet_unless_anomalous(void)
{
    /* Prime: the first release after other activity may report a busy_count change. */
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();

    const unsigned info_before = g_fake_lines.log_calls;
    const unsigned debug_before = g_fake_lines.debug_log_calls;
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_lines_delay_us(3000U);
    sensor_bus_owner_release();
    assert(g_fake_lines.log_calls == info_before);
    assert(g_fake_lines.debug_log_calls == debug_before + 1U);
    assert(strstr(g_fake_lines.last_debug_log, "I2C_BUS_OWNER owner=") != NULL);
    assert(parse_log_ms(g_fake_lines.last_debug_log, "hold_ms=") <= 4U);

    /* A hold at the threshold is an anomaly and stays at INFO. */
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_lines_delay_us(26000U);
    sensor_bus_owner_release();
    assert(g_fake_lines.log_calls == info_before + 1U);
    assert(g_fake_lines.debug_log_calls == debug_before + 1U);
    assert(strstr(g_fake_lines.last_log, "I2C_BUS_OWNER owner=") != NULL);
}

static unsigned s_owner_summary_lines;
static char s_owner_summary_line[256];

static void observe_owner_summary(const char *line)
{
    if (strstr(line, "I2C_BUS_OWNER_SUMMARY ") != NULL) {
        ++s_owner_summary_lines;
        (void)snprintf(s_owner_summary_line, sizeof(s_owner_summary_line), "%s", line);
    }
}

static void test_owner_summary_aggregates_sessions_once_per_second(void)
{
    g_fake_lines.log_observer = observe_owner_summary;
    sensor_bus_lines_delay_us(1000000U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    assert(s_owner_summary_lines > 0U);
    const unsigned before = s_owner_summary_lines;
    for (unsigned i = 0U; i < 3U; ++i) {
        assert(sensor_bus_owner_acquire() == ESP_OK);
        sensor_bus_owner_release();
    }
    sensor_bus_lines_delay_us(999999U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    assert(s_owner_summary_lines == before);
    sensor_bus_lines_delay_us(1U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    assert(s_owner_summary_lines == before + 1U);
    assert(strstr(s_owner_summary_line, "sessions=5") != NULL);
    assert(strstr(s_owner_summary_line, "busy=0") != NULL);
    unsigned info = g_fake_lines.log_calls;
    unsigned debug = g_fake_lines.debug_log_calls;
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_lines_delay_us(24999U);
    sensor_bus_owner_release();
    ++debug;
    assert(g_fake_lines.log_calls == info && g_fake_lines.debug_log_calls == debug);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_lines_delay_us(25000U);
    sensor_bus_owner_release();
    ++info;
    assert(g_fake_lines.log_calls == info && g_fake_lines.debug_log_calls == debug);
    atomic_store(&s_fake_next_wait_us, 4999U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    ++debug;
    assert(g_fake_lines.log_calls == info && g_fake_lines.debug_log_calls == debug);
    atomic_store(&s_fake_next_wait_us, 5000U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    ++info;
    assert(g_fake_lines.log_calls == info && g_fake_lines.debug_log_calls == debug);
    const unsigned before_busy_summary = s_owner_summary_lines;
    test_owner_try_acquire_does_not_wait();
    sensor_bus_lines_delay_us(1000000U);
    assert(sensor_bus_owner_acquire() == ESP_OK);
    sensor_bus_owner_release();
    assert(s_owner_summary_lines == before_busy_summary + 1U);
    assert(strstr(s_owner_summary_line, "busy=1") != NULL);
    g_fake_lines.log_observer = NULL;
}

/* MAX-434 T1: mutex wait, transfer timeout, and configuration diagnostics use separate ms budgets. */
static void test_mutex_wait_and_transfer_timeouts_are_distinct_ms(void)
{
    sensor_bus_handles_t handles = {0};
    assert(SENSOR_BUS_MUTEX_WAIT_MS == 200U);
    assert(SENSOR_BUS_TRANSACTION_TIMEOUT_MS == 20U);
    g_fake_lines.timeout_config_log[0] = '\0';
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    assert(strstr(g_fake_lines.timeout_config_log, "mutex_wait_ms=200") != NULL);
    assert(strstr(g_fake_lines.timeout_config_log, "transaction_timeout_ms=20") != NULL);

    test_owner_try_acquire_does_not_wait();
    test_bounded_wait_reports_owner_and_priority_inheritance();
    test_transaction_timeout_is_20ms_and_recovery_is_unlocked();
    test_owner_logs_match_fake_clock();
    test_owner_release_is_quiet_unless_anomalous();
    test_owner_summary_aggregates_sessions_once_per_second();
}

int main(void)
{
    fake_lines_reset();
    assert(sensor_bus_lock_init() == ESP_OK);
    g_fake_lines.probe_configured = true;
    for (size_t address = 0U; address < 128U; ++address) {
        g_fake_lines.probe_result[address] = ESP_ERR_NOT_FOUND;
    }
    g_fake_lines.probe_result[TCA_RESPONDING_ADDRESS] = ESP_OK;

    test_thread_t ina3221_thread = test_thread_start(ina3221_like_worker_fn, NULL);
    test_thread_t topology_thread = test_thread_start(topology_like_worker_fn, NULL);

    test_thread_join(ina3221_thread);
    test_thread_join(topology_thread);

    test_ina_transfer_cannot_interleave_inside_open_tca_channel();
    assert(sensor_bus_deinit() == ESP_OK);
    test_mutex_wait_and_transfer_timeouts_are_distinct_ms();
    test_hold_budget_aborts_closes_mux_without_fault();
    test_hold_budget_preserves_driver_timeout();
    test_topology_waiter_gets_bus_between_channels();
    test_waiting_ina_runs_between_main_ch0_ch3_log_pairs();
    test_owner_deadline_bounds_driver_and_clears_on_release();
    test_default_transfer_timeout_covers_the_wire_time();
    test_segmented_transfers_raise_the_caller_priority_only_while_they_run();
    test_owner_session_runs_at_the_boosted_priority();
    assert(sensor_bus_deinit() == ESP_OK);

    return 0;
}
