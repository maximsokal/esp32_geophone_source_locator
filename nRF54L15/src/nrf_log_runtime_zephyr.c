#include "nrf_log_runtime_zephyr.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "nrf_log_events.h"
#include "nrf_watchdog_zephyr.h"

static nrf_log_ring_t s_log_ring;
static struct k_spinlock s_log_ring_lock;
static struct k_spinlock s_metrics_lock;
static struct k_spinlock s_led_counters_lock;
static nrf_log_runtime_zephyr_led_counters_t s_led_counters;
static uint32_t s_max_block_build_cycles;
static bool s_initialized;

_Static_assert(
    sizeof(k_spinlock_key_t) <= sizeof(nrf_log_lock_key_t),
    "Zephyr spinlock key does not fit the frozen log lock interface");

static nrf_log_lock_key_t ring_lock(void *context)
{
    k_spinlock_key_t zephyr_key;
    nrf_log_lock_key_t encoded_key = 0U;

    (void)context;
    zephyr_key = k_spin_lock(&s_log_ring_lock);
    memcpy(&encoded_key, &zephyr_key, sizeof(zephyr_key));
    return encoded_key;
}

static void ring_unlock(void *context, nrf_log_lock_key_t key)
{
    k_spinlock_key_t zephyr_key = {0};

    (void)context;
    memcpy(&zephyr_key, &key, sizeof(zephyr_key));
    k_spin_unlock(&s_log_ring_lock, zephyr_key);
}

static uint32_t fallback_boot_id(void)
{
    uint8_t id_bytes[sizeof(uint32_t)] = {0U};
    uint32_t hardware_id;
    uint32_t boot_id;

    (void)hwinfo_get_device_id(id_bytes, sizeof(id_bytes));
    hardware_id = (uint32_t)id_bytes[0] |
        ((uint32_t)id_bytes[1] << 8U) |
        ((uint32_t)id_bytes[2] << 16U) |
        ((uint32_t)id_bytes[3] << 24U);
    boot_id = hardware_id ^ k_cycle_get_32();
    return boot_id != 0U ? boot_id : 1U;
}

static uint32_t generate_boot_id(uint8_t *fallback)
{
    uint32_t boot_id = 0U;

    *fallback = 0U;
    if (sys_csrand_get(&boot_id, sizeof(boot_id)) == 0 && boot_id != 0U) {
        return boot_id;
    }
    *fallback = 1U;
    return fallback_boot_id();
}

static uint32_t generate_next_boot_id(void *context)
{
    const uint32_t previous_boot_id = *(const uint32_t *)context;
    uint8_t fallback;
    uint32_t boot_id = generate_boot_id(&fallback);

    if (boot_id == previous_boot_id) {
        boot_id = fallback_boot_id();
        if (boot_id == previous_boot_id) {
            boot_id = previous_boot_id ^ 1U;
            if (boot_id == 0U) {
                boot_id = 1U;
            }
        }
    }
    return boot_id;
}

static void increment_saturating(uint32_t *counter)
{
    if (*counter < UINT32_MAX) {
        ++(*counter);
    }
}

static void track_led_event(uint16_t event_id)
{
    k_spinlock_key_t key = k_spin_lock(&s_led_counters_lock);

    switch (event_id) {
    case NRF_LOG_EVENT_TWIS_INVALID_FRAME:
    case NRF_LOG_EVENT_TWIS_UNKNOWN_TYPE:
    case NRF_LOG_EVENT_TWIS_BUSY:
        increment_saturating(&s_led_counters.twis_protocol_error_total);
        break;
    case NRF_LOG_EVENT_CS_EXECUTE_FAILED:
    case NRF_LOG_EVENT_CS_TIMEOUT:
        increment_saturating(&s_led_counters.cs_ranging_failure_total);
        break;
    case NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE:
        increment_saturating(&s_led_counters.watchdog_participant_stale_total);
        break;
    case NRF_LOG_EVENT_ROLE_LOST:
        increment_saturating(&s_led_counters.role_lost_total);
        break;
    default:
        break;
    }

    k_spin_unlock(&s_led_counters_lock, key);
}

int nrf_log_runtime_zephyr_init(void)
{
    const nrf_log_lock_interface_t lock = {
        .lock = ring_lock,
        .unlock = ring_unlock,
        .context = NULL,
    };
    uint8_t fallback;
    uint32_t boot_id;
    nrf_log_error_t result;

    if (s_initialized) {
        return 0;
    }
    boot_id = generate_boot_id(&fallback);
    result = nrf_log_ring_init(
        &s_log_ring,
        boot_id,
        fallback,
        &lock,
        generate_next_boot_id,
        &s_log_ring.boot_id);
    if (result != NRF_LOG_ERROR_OK) {
        return -EIO;
    }
    s_initialized = true;
    nrf_log_runtime_zephyr_append_event(
        NRF_LOG_EVENT_BOOT,
        nrf_watchdog_zephyr_boot_reset_cause(),
        fallback);
    return 0;
}

void nrf_log_runtime_zephyr_append_event(uint16_t event_id, uint32_t arg0, uint32_t arg1)
{
    const nrf_log_event_definition_t *event;

    if (!s_initialized) {
        return;
    }
    event = nrf_log_event_find(event_id);
    if (event == NULL) {
        return;
    }
    track_led_event(event_id);
    (void)nrf_log_ring_append(
        &s_log_ring,
        event->subsystem,
        event->default_severity,
        event_id,
        arg0,
        arg1,
        (uint32_t)k_uptime_get());
}

nrf_log_error_t nrf_log_runtime_zephyr_get_led_counters(
    nrf_log_runtime_zephyr_led_counters_t *counters)
{
    nrf_log_status_t log_status;
    k_spinlock_key_t key;
    nrf_log_error_t result;

    if (counters == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (!s_initialized) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }

    result = nrf_log_ring_status_snapshot(&s_log_ring, &log_status);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }

    key = k_spin_lock(&s_led_counters_lock);
    *counters = s_led_counters;
    k_spin_unlock(&s_led_counters_lock, key);
    counters->overwritten_total = log_status.overwritten_total;
    counters->lost_internal_total = log_status.lost_internal_total;
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_runtime_zephyr_build_status(
    uint32_t request_id,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length)
{
    if (!s_initialized) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    return nrf_log_ring_build_status_frame(
        &s_log_ring,
        request_id,
        frame,
        frame_capacity,
        frame_length);
}

nrf_log_error_t nrf_log_runtime_zephyr_build_block(
    const nrf_log_read_request_t *request,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length)
{
    uint32_t started;
    uint32_t elapsed;
    uint32_t max_cycles;
    k_spinlock_key_t key;
    nrf_log_error_t result;

    if (!s_initialized) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    if (request == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    started = k_cycle_get_32();
    result = nrf_log_ring_build_block_frame(
        &s_log_ring,
        request->request_id,
        request->expected_boot_id,
        request->cursor_seq,
        request->max_records,
        frame,
        frame_capacity,
        frame_length);
    elapsed = k_cycle_get_32() - started;

    key = k_spin_lock(&s_metrics_lock);
    max_cycles = s_max_block_build_cycles;
    if (elapsed > max_cycles) {
        s_max_block_build_cycles = elapsed;
    }
    k_spin_unlock(&s_metrics_lock, key);
    return result;
}

uint32_t nrf_log_runtime_zephyr_max_block_build_cycles(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_metrics_lock);
    uint32_t max_cycles = s_max_block_build_cycles;

    k_spin_unlock(&s_metrics_lock, key);
    return max_cycles;
}

#if defined(NRF_LOG_RING_TEST_HOOKS)
nrf_log_error_t nrf_log_runtime_zephyr_test_set_next_seq(uint32_t next_seq)
{
    const nrf_log_lock_interface_t lock = {
        .lock = ring_lock,
        .unlock = ring_unlock,
        .context = NULL,
    };
    const uint32_t boot_id = s_log_ring.boot_id;
    const uint8_t fallback = s_log_ring.boot_id_fallback;

    if (!s_initialized) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    /* The ring hook requires an empty ring; reset only this test fixture. */
    if (nrf_log_ring_init(
            &s_log_ring,
            boot_id,
            fallback,
            &lock,
            generate_next_boot_id,
            &s_log_ring.boot_id) != NRF_LOG_ERROR_OK) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    return nrf_log_ring_test_set_next_seq(&s_log_ring, next_seq);
}
#endif
