#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_cs_pair_runtime_zephyr_bridge.h"
#include "nrf_cs_measurement_quality.h"
#include "nrf_cs_distance_method.h"
#include "nrf_cs_tone_stats.h"
#include "nrf_cs_iq_gap_fill.h"
#include "nrf_cs_ifft_quality.h"
#include "nrf_cs_rf_diagnostics.h"
#include "nrf_cs_trace_zephyr.h"

#include <errno.h>

#if defined(CONFIG_BT_RAS) && defined(CONFIG_BT_RAS_RREQ) && \
    defined(CONFIG_BT_RAS_RRSP) && defined(CONFIG_BT_CS_DE)

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bluetooth/cs_de.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/ras.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "nrf_cs_pair_adapter.h"
#include "nrf_cs_cycle_runtime.h"
#include "nrf_cs_event_overflow.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_watchdog_zephyr.h"

static nrf_cs_pair_runtime_final_hint_cb_t s_relay_final_hint_cb;
static nrf_cs_pair_runtime_released_cb_t s_relay_released_cb;

void nrf_cs_pair_runtime_zephyr_set_relay_hooks(
    nrf_cs_pair_runtime_final_hint_cb_t final_hint, nrf_cs_pair_runtime_released_cb_t released)
{
    s_relay_final_hint_cb = final_hint;
    s_relay_released_cb = released;
}

#include "nrf_cs_pair_runtime_zephyr_impl_1.inc"
#include "nrf_cs_pair_runtime_zephyr_impl_2.inc"
#include "nrf_cs_pair_runtime_zephyr_impl_3.inc"

#else

int nrf_cs_pair_runtime_zephyr_start(void)
{
    return 0;
}

int nrf_cs_pair_runtime_zephyr_set_node_id(nrf_cs_node_id_t node_id)
{
    return node_id == 0xFFU ? -EINVAL : 0;
}

int nrf_cs_pair_runtime_zephyr_set_radio_enabled(bool enabled)
{
    (void)enabled;
    return 0;
}

int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context)
{
    (void)step;
    (void)complete_cb;
    (void)context;
    return -ENOTSUP;
}

void nrf_cs_pair_runtime_zephyr_set_relay_hooks(
    nrf_cs_pair_runtime_final_hint_cb_t final_hint, nrf_cs_pair_runtime_released_cb_t released)
{
    (void)final_hint;
    (void)released;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return false;
}

bool nrf_cs_pair_runtime_zephyr_is_progress_required(void)
{
    return false;
}

#endif
