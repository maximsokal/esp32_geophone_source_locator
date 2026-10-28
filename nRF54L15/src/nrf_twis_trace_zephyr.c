#include <errno.h>
#include <string.h>

#include <nrfx_twis.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"

/* The NCS buffer shim drops completion/error events; observe them without
 * changing its handling or printing while the controller waits for the IRQ. */
static nrfx_twis_t *s_upstream;
static nrfx_twis_event_handler_t s_driver_handler;
static nrf_boot_twis_record_t s_selected;
static uint32_t s_read_started_cycles;
static uint32_t s_write_started_cycles;
static bool s_write_in_flight;
static bool s_rx_dma_prepared;
static int s_rx_prepare_result;

int __real_nrfx_twis_init(nrfx_twis_t *instance, const nrfx_twis_config_t *config,
                         nrfx_twis_event_handler_t handler);
int __real_nrfx_twis_tx_prepare(nrfx_twis_t *instance, const void *buffer, size_t length);
int __real_nrfx_twis_rx_prepare(nrfx_twis_t *instance, void *buffer, size_t length);

static void trace_event(const nrfx_twis_event_t *event)
{
    if (event->type == NRFX_TWIS_EVT_READ_REQ) {
        /* A read begins a new transfer; do not reuse an orphaned RX arm. */
        s_write_in_flight = false;
        s_rx_dma_prepared = false;
        memset(&s_selected, 0, sizeof(s_selected));
        s_selected.direction = NRF_BOOT_TWIS_DIRECTION_TX_PREPARED;
        s_selected.prepare_result = -ENOSYS;
        s_selected.result = -ENOSYS;
        s_read_started_cycles = k_cycle_get_32();
        s_driver_handler(event);
        nrf_bootstrap_runtime_zephyr_record_twis_hardware(&s_selected);
        return;
    }
    if (event->type == NRFX_TWIS_EVT_WRITE_REQ) {
        s_write_in_flight = true;
        s_rx_dma_prepared = false;
        s_rx_prepare_result = -ENOSYS;
        s_write_started_cycles = k_cycle_get_32();
        s_driver_handler(event);
        return;
    }
    if (event->type == NRFX_TWIS_EVT_WRITE_DONE || event->type == NRFX_TWIS_EVT_WRITE_ERROR) {
        /* Sample the RX buffer before the shim hands it to the app: this shows whether the
         * bytes the master wrote were in RAM when the TWIS finished, or were already
         * replaced. Address-only writes (nothing received) are not recorded. */
        nrf_boot_twis_record_t received = {0};
        const bool failed = event->type == NRFX_TWIS_EVT_WRITE_ERROR;
        received.direction = failed
            ? NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR : NRF_BOOT_TWIS_DIRECTION_WRITE_DONE;
        received.rx_amount = (uint32_t)nrfx_twis_rx_amount(s_upstream);
        if (!s_write_in_flight) {
            /* The nrfx driver cannot legitimately finish a transfer that
             * never requested an RX buffer. Its cached DMA amount may still
             * be non-zero: do not forward that stale frame to the app. */
            received.direction = NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR;
            received.result = -EPROTO;
            received.prepare_result = -EPROTO;
            nrf_bootstrap_runtime_zephyr_record_twis_hardware(&received);
            return;
        }
        s_write_in_flight = false;
        if (!s_rx_dma_prepared) {
            /* WRITE_REQ alone cannot validate the EasyDMA buffer. The shim
             * may have called nrfx_twis_rx_prepare(), received an error,
             * and still receive a completion with stale RXD.AMOUNT. */
            received.direction = NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR;
            received.result = s_rx_prepare_result;
            received.prepare_result = s_rx_prepare_result;
            nrf_bootstrap_runtime_zephyr_record_twis_hardware(&received);
            return;
        }
        s_rx_dma_prepared = false;
        received.event_dt_us = k_cyc_to_us_floor32(k_cycle_get_32() - s_write_started_cycles);
        received.hw_error = failed ? event->data.error : 0U;
        received.result = failed ? -EIO : 0;
        const uint8_t *rx = nrf_twis_rx_buffer_get(s_upstream->p_reg);
        if (rx != NULL && received.rx_amount != 0U) {
            received.prefix_length = (uint8_t)(received.rx_amount < sizeof(received.prefix)
                ? received.rx_amount : sizeof(received.prefix));
            memcpy(received.prefix, rx, received.prefix_length);
        }
        s_driver_handler(event);
        if (failed || received.rx_amount != 0U) {
            nrf_bootstrap_runtime_zephyr_record_twis_hardware(&received);
        }
        return;
    }
    if (event->type == NRFX_TWIS_EVT_READ_DONE || event->type == NRFX_TWIS_EVT_READ_ERROR) {
        nrf_boot_twis_record_t completed = s_selected;
        completed.direction = event->type == NRFX_TWIS_EVT_READ_DONE
            ? NRF_BOOT_TWIS_DIRECTION_READ_DONE : NRF_BOOT_TWIS_DIRECTION_READ_ERROR;
        completed.tx_amount = (uint32_t)nrfx_twis_tx_amount(s_upstream);
        completed.hw_error = event->type == NRFX_TWIS_EVT_READ_ERROR ? event->data.error : 0U;
        completed.result = event->type == NRFX_TWIS_EVT_READ_ERROR ? -EIO : 0;
        nrf_bootstrap_runtime_zephyr_record_twis_hardware(&completed);
    } else if (event->type == NRFX_TWIS_EVT_GENERAL_ERROR) {
        s_write_in_flight = false;
        s_rx_dma_prepared = false;
        nrf_boot_twis_record_t error = {0};
        error.direction = NRF_BOOT_TWIS_DIRECTION_GENERAL_ERROR;
        error.prepare_result = -ENOSYS;
        error.hw_error = event->data.error;
        error.result = -EIO;
        nrf_bootstrap_runtime_zephyr_record_twis_hardware(&error);
    }
    s_driver_handler(event);
}

int __wrap_nrfx_twis_init(nrfx_twis_t *instance, const nrfx_twis_config_t *config,
                         nrfx_twis_event_handler_t handler)
{
    if (instance->p_reg != (NRF_TWIS_Type *)DT_REG_ADDR(DT_NODELABEL(i2c22)) || handler == NULL) {
        return __real_nrfx_twis_init(instance, config, handler);
    }
    nrfx_twis_t *previous_instance = s_upstream;
    nrfx_twis_event_handler_t previous_handler = s_driver_handler;
    const bool previous_write_in_flight = s_write_in_flight;
    const bool previous_rx_dma_prepared = s_rx_dma_prepared;
    const int previous_rx_prepare_result = s_rx_prepare_result;
    s_upstream = instance;
    s_write_in_flight = false;
    s_rx_dma_prepared = false;
    s_rx_prepare_result = -ENOSYS;
    s_driver_handler = handler;
    const int result = __real_nrfx_twis_init(instance, config, trace_event);
    if (result != 0) {
        s_upstream = previous_instance;
        s_driver_handler = previous_handler;
        s_write_in_flight = previous_write_in_flight;
        s_rx_dma_prepared = previous_rx_dma_prepared;
        s_rx_prepare_result = previous_rx_prepare_result;
    }
    return result;
}

int __wrap_nrfx_twis_rx_prepare(nrfx_twis_t *instance, void *buffer, size_t length)
{
    const int result = __real_nrfx_twis_rx_prepare(instance, buffer, length);
    if (instance == s_upstream && s_write_in_flight) {
        s_rx_prepare_result = result;
        s_rx_dma_prepared = result == 0;
    }
    return result;
}

int __wrap_nrfx_twis_tx_prepare(nrfx_twis_t *instance, const void *buffer, size_t length)
{
    const int result = __real_nrfx_twis_tx_prepare(instance, buffer, length);
    if (instance == s_upstream) {
        s_selected.length = (uint32_t)length;
        s_selected.prefix_length = buffer == NULL ? 0U :
            (uint8_t)(length < sizeof(s_selected.prefix) ? length : sizeof(s_selected.prefix));
        if (s_selected.prefix_length != 0U) {
            memcpy(s_selected.prefix, buffer, s_selected.prefix_length);
        }
        s_selected.prepare_result = result;
        s_selected.result = result;
        s_selected.tx_prepared = result == 0;
        s_selected.prepare_us = k_cyc_to_us_floor32(k_cycle_get_32() - s_read_started_cycles);
    }
    return result;
}
