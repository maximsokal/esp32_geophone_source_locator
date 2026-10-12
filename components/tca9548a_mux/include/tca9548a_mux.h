#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TCA9548A_MUX_STATE_CLOSED = 0,
    TCA9548A_MUX_STATE_SELECTED,
    TCA9548A_MUX_STATE_FAULT,
} tca9548a_mux_state_t;

typedef enum {
    TCA9548A_MUX_PHASE_CLOSE = 0,
    TCA9548A_MUX_PHASE_SELECT,
    TCA9548A_MUX_PHASE_VERIFY,
} tca9548a_mux_phase_t;

typedef enum {
    TCA9548A_MUX_FAULT_NONE = 0,
    TCA9548A_MUX_FAULT_VERIFY_TIMEOUT,
    TCA9548A_MUX_FAULT_READBACK_MISMATCH,
    TCA9548A_MUX_FAULT_CLOSE_ERROR,
    TCA9548A_MUX_FAULT_BUS_STUCK,
} tca9548a_mux_fault_reason_t;

typedef esp_err_t (*tca9548a_mux_write_fn)(
    void *context,
    uint8_t address,
    uint8_t value,
    tca9548a_mux_phase_t phase,
    int channel);
typedef esp_err_t (*tca9548a_mux_read_fn)(
    void *context,
    uint8_t address,
    uint8_t *value,
    uint8_t expected,
    tca9548a_mux_phase_t phase,
    int channel);

typedef struct {
    tca9548a_mux_write_fn write_control;
    tca9548a_mux_read_fn read_control;
    void *context;
    uint8_t address;
} tca9548a_mux_io_t;

typedef struct {
    tca9548a_mux_io_t io;
    tca9548a_mux_state_t state;
    int8_t selected_channel;
    tca9548a_mux_fault_reason_t fault_reason;
} tca9548a_mux_t;

esp_err_t tca9548a_mux_init(tca9548a_mux_t *mux, const tca9548a_mux_io_t *io);
esp_err_t tca9548a_mux_close(tca9548a_mux_t *mux);
esp_err_t tca9548a_mux_select(tca9548a_mux_t *mux, uint8_t channel);
tca9548a_mux_state_t tca9548a_mux_get_state(const tca9548a_mux_t *mux);
tca9548a_mux_fault_reason_t tca9548a_mux_get_fault_reason(const tca9548a_mux_t *mux);
const char *tca9548a_mux_fault_reason_name(tca9548a_mux_fault_reason_t reason);
void tca9548a_mux_latch_bus_stuck(tca9548a_mux_t *mux);
int tca9548a_mux_get_selected_channel(const tca9548a_mux_t *mux);
