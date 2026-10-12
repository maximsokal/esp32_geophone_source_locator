#include "tca9548a_mux.h"
#include "sensor_bus.h"

#include <stddef.h>

#define TCA9548A_CHANNEL_COUNT 8U
#define TCA9548A_CLOSED_MASK   0x00U

static bool tca9548a_mux_is_yield_error(esp_err_t err)
{
    return err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
}

static void tca9548a_mux_set_fault(
    tca9548a_mux_t *mux,
    tca9548a_mux_fault_reason_t reason)
{
    mux->state = TCA9548A_MUX_STATE_FAULT;
    mux->selected_channel = -1;
    mux->fault_reason = reason;
}

static tca9548a_mux_fault_reason_t tca9548a_mux_reason_for_error(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT) {
        return TCA9548A_MUX_FAULT_VERIFY_TIMEOUT;
    }
    if (err == ESP_ERR_INVALID_RESPONSE) {
        return TCA9548A_MUX_FAULT_READBACK_MISMATCH;
    }
    return TCA9548A_MUX_FAULT_CLOSE_ERROR;
}

static esp_err_t tca9548a_mux_verify_closed_once(tca9548a_mux_t *mux)
{
    esp_err_t err = mux->io.write_control(
        mux->io.context, mux->io.address, TCA9548A_CLOSED_MASK,
        TCA9548A_MUX_PHASE_CLOSE, -1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0xFFU;
    err = mux->io.read_control(
        mux->io.context, mux->io.address, &value, TCA9548A_CLOSED_MASK,
        TCA9548A_MUX_PHASE_VERIFY, -1);
    if (err != ESP_OK) {
        return err;
    }
    if (value != TCA9548A_CLOSED_MASK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    mux->state = TCA9548A_MUX_STATE_CLOSED;
    mux->selected_channel = -1;
    mux->fault_reason = TCA9548A_MUX_FAULT_NONE;
    return ESP_OK;
}

static esp_err_t tca9548a_mux_best_effort_recover_closed(tca9548a_mux_t *mux)
{
    const esp_err_t err = tca9548a_mux_verify_closed_once(mux);
    if (err != ESP_OK && !tca9548a_mux_is_yield_error(err)) {
        tca9548a_mux_set_fault(mux, tca9548a_mux_reason_for_error(err));
    }
    return err;
}

esp_err_t tca9548a_mux_init(tca9548a_mux_t *mux, const tca9548a_mux_io_t *io)
{
    if (mux == NULL || io == NULL || io->write_control == NULL || io->read_control == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mux->io = *io;
    tca9548a_mux_set_fault(mux, TCA9548A_MUX_FAULT_NONE);
    return ESP_OK;
}

esp_err_t tca9548a_mux_close(tca9548a_mux_t *mux)
{
    if (mux == NULL || mux->io.write_control == NULL || mux->io.read_control == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t first_err = tca9548a_mux_verify_closed_once(mux);
    if (first_err == ESP_OK) {
        return ESP_OK;
    }
    if (tca9548a_mux_is_yield_error(first_err)) {
        return first_err;
    }

    /* A failed first readback is transient when the next close+readback
     * confirms the closed state. Only the final unsuccessful attempt faults. */
    const esp_err_t retry_err = tca9548a_mux_best_effort_recover_closed(mux);
    if (retry_err != ESP_OK && !tca9548a_mux_is_yield_error(retry_err)) {
        tca9548a_mux_set_fault(mux, tca9548a_mux_reason_for_error(retry_err));
    }
    return retry_err;
}

esp_err_t tca9548a_mux_select(tca9548a_mux_t *mux, uint8_t channel)
{
    if (mux == NULL || mux->io.write_control == NULL || mux->io.read_control == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= TCA9548A_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tca9548a_mux_close(mux);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t mask = (uint8_t)(1U << channel);
    err = mux->io.write_control(
        mux->io.context, mux->io.address, mask,
        TCA9548A_MUX_PHASE_SELECT, (int)channel);
    if (err != ESP_OK) {
        (void)tca9548a_mux_best_effort_recover_closed(mux);
        return err;
    }

    uint8_t readback = 0U;
    err = mux->io.read_control(
        mux->io.context, mux->io.address, &readback, mask,
        TCA9548A_MUX_PHASE_VERIFY, (int)channel);
    if (err != ESP_OK) {
        (void)tca9548a_mux_best_effort_recover_closed(mux);
        return err;
    }
    if (readback != mask) {
        (void)tca9548a_mux_best_effort_recover_closed(mux);
        return ESP_ERR_INVALID_RESPONSE;
    }

    mux->state = TCA9548A_MUX_STATE_SELECTED;
    mux->selected_channel = (int8_t)channel;
    return ESP_OK;
}

tca9548a_mux_state_t tca9548a_mux_get_state(const tca9548a_mux_t *mux)
{
    return mux == NULL ? TCA9548A_MUX_STATE_FAULT : mux->state;
}

tca9548a_mux_fault_reason_t tca9548a_mux_get_fault_reason(const tca9548a_mux_t *mux)
{
    return mux == NULL ? TCA9548A_MUX_FAULT_CLOSE_ERROR : mux->fault_reason;
}

const char *tca9548a_mux_fault_reason_name(tca9548a_mux_fault_reason_t reason)
{
    switch (reason) {
    case TCA9548A_MUX_FAULT_VERIFY_TIMEOUT: return "verify_timeout";
    case TCA9548A_MUX_FAULT_READBACK_MISMATCH: return "readback_mismatch";
    case TCA9548A_MUX_FAULT_BUS_STUCK: return "TCA_BUS_STUCK";
    case TCA9548A_MUX_FAULT_NONE: return "none";
    case TCA9548A_MUX_FAULT_CLOSE_ERROR:
    default: return "close_error";
    }
}

void tca9548a_mux_latch_bus_stuck(tca9548a_mux_t *mux)
{
    if (mux != NULL) {
        tca9548a_mux_set_fault(mux, TCA9548A_MUX_FAULT_BUS_STUCK);
    }
}

int tca9548a_mux_get_selected_channel(const tca9548a_mux_t *mux)
{
    if (mux == NULL || mux->state != TCA9548A_MUX_STATE_SELECTED) {
        return -1;
    }
    return mux->selected_channel;
}
