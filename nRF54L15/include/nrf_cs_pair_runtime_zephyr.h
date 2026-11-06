#ifndef NRF_CS_PAIR_RUNTIME_ZEPHYR_H
#define NRF_CS_PAIR_RUNTIME_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_CS_PROVIDER_QUALITY_REALTIME_RAS (1U << 0)
#define NRF_CS_PROVIDER_QUALITY_TONE_OK      (1U << 1)
#define NRF_CS_PROVIDER_QUALITY_PBR_VALID    (1U << 2)
#define NRF_CS_PROVIDER_QUALITY_RTT_VALID    (1U << 3)

typedef void (*nrf_cs_pair_runtime_complete_cb_t)(
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement,
    void *context);

int nrf_cs_pair_runtime_zephyr_start(void);
int nrf_cs_pair_runtime_zephyr_set_node_id(nrf_cs_node_id_t node_id);
/*
 * Allows or forbids radio activity (advertising, scanning, connecting). With
 * CONFIG_APP_NRF_RADIO_ON_DEMAND the radio starts forbidden; the autonomous cycle opens it only while
 * it runs. Call from the main loop, not from the TWIS callbacks.
 */
int nrf_cs_pair_runtime_zephyr_set_radio_enabled(bool enabled);
/* Nonblocking, may be called under the cycle lock; cancellation runs on the CS queue. */
void nrf_cs_pair_runtime_zephyr_cancel(const nrf_cs_step_t *step);
int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context);

/*
 * Relay hooks of the autonomous cycle (nrf_cs_cycle_runtime.h, NRF_CS_CYCLE_RELEASE_REASON).
 *  final_hint: asked when an attempt ends and the link is about to be dropped; true when that attempt also ends the
 *              execution, and the link is then dropped with the release reason, which tells the node on the other end
 *              that its turn has come.
 *  released:   the node on the other end of an incoming link dropped it with the release reason.
 * Either may be NULL. Called from the Bluetooth work queues; they must be short and must not block.
 */
typedef bool (*nrf_cs_pair_runtime_final_hint_cb_t)(const nrf_cs_measurement_t *measurement);
typedef void (*nrf_cs_pair_runtime_released_cb_t)(void);
void nrf_cs_pair_runtime_zephyr_set_relay_hooks(
    nrf_cs_pair_runtime_final_hint_cb_t final_hint, nrf_cs_pair_runtime_released_cb_t released);
bool nrf_cs_pair_runtime_zephyr_is_busy(void);
/*
 * How long the incoming (reflector) link has been down, counted from its last activity. False while a link is up, while
 * its disconnect is still being handled, or when none was ever accepted. The cycle runtime uses it to infer the end of
 * an execution whose initiator could not send the release reason (nrf_cs_cycle_runtime_peer_silent).
 */
bool nrf_cs_pair_runtime_zephyr_incoming_idle_ms(uint32_t *idle_ms);
bool nrf_cs_pair_runtime_zephyr_is_progress_required(void);

#ifdef __cplusplus
}
#endif

#endif
