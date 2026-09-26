#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board_config.h"
#include "sensor_bus.h"

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


/* Fake I2C layer with a reentrancy canary: exactly one caller may be
 * "inside" it at a time. This must hold as long as sensor_bus.c correctly
 * serializes every entry point with its own lock. */
static atomic_int s_active;
static int s_bus_token;
static int s_ina3221_token;
static int s_candidate_tokens[8]; /* one per 0x70..0x77 candidate address */
static uint8_t s_tca_control;
#define TCA_RESPONDING_ADDRESS 0x70U

static void canary_enter(void)
{
    const int now = atomic_fetch_add(&s_active, 1) + 1;
    assert(now == 1);
    test_sleep_ms(1U);
}

static void canary_exit(void)
{
    atomic_fetch_sub(&s_active, 1);
}

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config)
{
    (void)port;
    (void)config;
    canary_enter();
    canary_exit();
    return &s_bus_token;
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

static atomic_int s_deinit_countdown = 25;

#ifdef _WIN32
static DWORD WINAPI ina3221_like_worker_fn(LPVOID arg)
#else
static void *ina3221_like_worker_fn(void *arg)
#endif
{
    (void)arg;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        sensor_bus_handles_t handles = {0};
        const esp_err_t err = sensor_bus_get_handles(&handles);
        assert(err == ESP_OK);
        assert(handles.bus == &s_bus_token);
        assert(handles.ina3221 == &s_ina3221_token);
    }
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
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        i2c_bus_device_handle_t device = NULL;
        uint8_t address = 0U;
        const esp_err_t err = sensor_bus_get_tca9548a(&device, &address);
        assert(err == ESP_OK);
        assert(device == &s_candidate_tokens[0]);
        assert(address == 0x70U);

        if (atomic_fetch_sub(&s_deinit_countdown, 1) == 1) {
            atomic_store(&s_deinit_countdown, 25);
            (void)sensor_bus_deinit();
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(void)
{
    assert(sensor_bus_lock_init() == ESP_OK);

    test_thread_t ina3221_thread = test_thread_start(ina3221_like_worker_fn, NULL);
    test_thread_t topology_thread = test_thread_start(topology_like_worker_fn, NULL);

    test_thread_join(ina3221_thread);
    test_thread_join(topology_thread);

    return 0;
}
