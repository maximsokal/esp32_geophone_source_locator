#include "tca9548a_mux.h"

#include <stddef.h>

#define TCA9548A_CHANNEL_COUNT 8U
#define TCA9548A_CLOSED_MASK   0x00U

static void tca9548a_mux_set_fault(tca9548a_mux_t *mux)
{
    mux->state = TCA9548A_MUX_STATE_FAULT;
    mux->selected_channel = -1;
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
    return ESP_OK;
}

static void tca9548a_mux_best_effort_recover_closed(tca9548a_mux_t *mux)
{
    if (tca9548a_mux_verify_closed_once(mux) != ESP_OK) {
        tca9548a_mux_set_fault(mux);
    }
}

esp_err_t tca9548a_mux_init(tca9548a_mux_t *mux, const tca9548a_mux_io_t *io)
{
    if (mux == NULL || io == NULL || io->write_control == NULL || io->read_control == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mux->io = *io;
    tca9548a_mux_set_fault(mux);
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

    tca9548a_mux_best_effort_recover_closed(mux);
    return first_err;
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
        tca9548a_mux_best_effort_recover_closed(mux);
        return err;
    }

    uint8_t readback = 0U;
    err = mux->io.read_control(
        mux->io.context, mux->io.address, &readback, mask,
        TCA9548A_MUX_PHASE_VERIFY, (int)channel);
    if (err != ESP_OK) {
        tca9548a_mux_best_effort_recover_closed(mux);
        return err;
    }
    if (readback != mask) {
        tca9548a_mux_best_effort_recover_closed(mux);
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

int tca9548a_mux_get_selected_channel(const tca9548a_mux_t *mux)
{
    if (mux == NULL || mux->state != TCA9548A_MUX_STATE_SELECTED) {
        return -1;
    }
    return mux->selected_channel;
}
