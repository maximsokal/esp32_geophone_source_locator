/*
 * TCA9548A control-register state machine against a model of the real chip.
 * Every combination of bus outcomes (ACK, NACK, timeout, generic failure,
 * owner-lock miss, hold-budget miss, wrong readback) over the first calls of
 * select/close, from every starting state and for all 8 channels, must keep
 * the software state truthful about the hardware register.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensor_bus_errors.h"
#include "tca9548a_mux.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

enum { OUTCOME_OK = 0, OUTCOME_FAIL, OUTCOME_TIMEOUT, OUTCOME_NACK, OUTCOME_BUSY, OUTCOME_BUDGET,
       OUTCOME_WRONG_READBACK, OUTCOME_COUNT };

typedef struct {
    uint8_t reg;               /* the chip's control register */
    uint8_t script[16];        /* outcome of the n-th I2C call; OK after the script */
    unsigned script_length;
    unsigned calls;
    unsigned writes;
} chip_t;

static esp_err_t outcome_error(unsigned outcome)
{
    switch (outcome) {
    case OUTCOME_FAIL: return ESP_FAIL;
    case OUTCOME_TIMEOUT: return ESP_ERR_TIMEOUT;
    case OUTCOME_NACK: return ESP_ERR_INVALID_STATE;
    case OUTCOME_BUSY: return SENSOR_BUS_ERR_MUTEX_BUSY;
    case OUTCOME_BUDGET: return SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
    default: return ESP_OK;
    }
}

static unsigned next_outcome(chip_t *chip)
{
    const unsigned index = chip->calls++;
    return index < chip->script_length ? chip->script[index] : OUTCOME_OK;
}

static esp_err_t chip_write(void *context, uint8_t address, uint8_t value, tca9548a_mux_phase_t phase, int channel)
{
    chip_t *chip = context;
    (void)address; (void)phase; (void)channel;
    const unsigned outcome = next_outcome(chip);
    ++chip->writes;
    if (outcome != OUTCOME_OK && outcome != OUTCOME_WRONG_READBACK) {
        return outcome_error(outcome); /* a failed write does not reach the register */
    }
    chip->reg = value;
    return ESP_OK;
}

static esp_err_t chip_read(void *context, uint8_t address, uint8_t *value, uint8_t expected,
    tca9548a_mux_phase_t phase, int channel)
{
    chip_t *chip = context;
    (void)address; (void)expected; (void)phase; (void)channel;
    const unsigned outcome = next_outcome(chip);
    if (outcome == OUTCOME_WRONG_READBACK) {
        *value = (uint8_t)(chip->reg ^ 0x5AU);
        return ESP_OK;
    }
    if (outcome != OUTCOME_OK) return outcome_error(outcome);
    *value = chip->reg;
    return ESP_OK;
}

static bool allowed_error(esp_err_t err)
{
    return err == ESP_FAIL || err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE ||
           err == SENSOR_BUS_ERR_MUTEX_BUSY || err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET ||
           err == ESP_ERR_INVALID_RESPONSE;
}

static bool is_yield(esp_err_t err)
{
    return err == SENSOR_BUS_ERR_MUTEX_BUSY || err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
}

static void setup(tca9548a_mux_t *mux, chip_t *chip, unsigned start)
{
    const tca9548a_mux_io_t io = {chip_write, chip_read, chip, 0x70U};
    memset(chip, 0, sizeof(*chip));
    assert(tca9548a_mux_init(mux, &io) == ESP_OK);
    if (start == 1U) {                      /* known closed */
        assert(tca9548a_mux_close(mux) == ESP_OK);
    } else if (start == 2U) {               /* another channel selected */
        assert(tca9548a_mux_select(mux, 7U) == ESP_OK);
    }
    chip->calls = 0U;
    chip->writes = 0U;
}

static void check_truthful_state(const tca9548a_mux_t *mux, const chip_t *chip)
{
    const tca9548a_mux_state_t state = tca9548a_mux_get_state(mux);
    if (state == TCA9548A_MUX_STATE_CLOSED) {
        /* The service skips closing when it believes the mux is closed. */
        assert(chip->reg == 0U);
        assert(tca9548a_mux_get_selected_channel(mux) == -1);
    } else if (state == TCA9548A_MUX_STATE_SELECTED) {
        /* Believing a channel is open while it is already closed is safe (the
         * next close still runs); only the opposite direction is dangerous. */
        const int channel = tca9548a_mux_get_selected_channel(mux);
        assert(channel >= 0 && channel < 8);
    } else {
        assert(tca9548a_mux_get_selected_channel(mux) == -1);
    }
}

static void select_survives_every_bus_failure_pattern(void)
{
    const unsigned depth = 5U;
    unsigned long runs = 0UL;
    unsigned long successes = 0UL;
    unsigned long yields_without_fault = 0UL;
    unsigned total = 1U;
    for (unsigned i = 0U; i < depth; ++i) total *= OUTCOME_COUNT;

    for (unsigned start = 0U; start < 3U; ++start) {
        for (unsigned channel = 0U; channel < 8U; ++channel) {
            for (unsigned pattern = 0U; pattern < total; ++pattern) {
                tca9548a_mux_t mux;
                chip_t chip;
                setup(&mux, &chip, start);
                unsigned rest = pattern;
                chip.script_length = depth;
                for (unsigned i = 0U; i < depth; ++i) {
                    chip.script[i] = (uint8_t)(rest % OUTCOME_COUNT);
                    rest /= OUTCOME_COUNT;
                }
                const esp_err_t err = tca9548a_mux_select(&mux, (uint8_t)channel);
                ++runs;

                if (err == ESP_OK) {
                    ++successes;
                    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_SELECTED);
                    assert(tca9548a_mux_get_selected_channel(&mux) == (int)channel);
                    assert(chip.reg == (uint8_t)(1U << channel));
                    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_NONE);
                } else {
                    assert(allowed_error(err));
                    if (is_yield(err) && tca9548a_mux_get_state(&mux) != TCA9548A_MUX_STATE_FAULT) {
                        ++yields_without_fault;
                    }
                }
                if (tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED && chip.reg != 0U) {
                    static const char *const names[OUTCOME_COUNT] = {"ok", "fail", "timeout", "nack", "busy", "budget", "wrong-readback"};
                    fprintf(stderr, "start=%u channel=%u err=0x%x reg=0x%02x script:", start, channel, (unsigned)err, chip.reg);
                    for (unsigned i = 0U; i < depth; ++i) fprintf(stderr, " %s", names[chip.script[i]]);
                    fprintf(stderr, "\n");
                }
                check_truthful_state(&mux, &chip);
                assert(chip.calls <= 12U); /* bounded work, no retry storms */

                /* Whatever happened, a clean close afterwards always recovers. */
                chip.script_length = 0U;
                assert(tca9548a_mux_close(&mux) == ESP_OK);
                assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);
                assert(chip.reg == 0U);
                assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_NONE);
            }
        }
    }
    assert(runs == 3UL * 8UL * total);
    assert(successes > 0UL && yields_without_fault > 0UL);
    CASE_PASS("MUX.select_failure_patterns");
}

static void close_survives_every_bus_failure_pattern(void)
{
    const unsigned depth = 5U;
    unsigned total = 1U;
    for (unsigned i = 0U; i < depth; ++i) total *= OUTCOME_COUNT;

    for (unsigned start = 0U; start < 3U; ++start) {
        for (unsigned pattern = 0U; pattern < total; ++pattern) {
            tca9548a_mux_t mux;
            chip_t chip;
            setup(&mux, &chip, start);
            unsigned rest = pattern;
            chip.script_length = depth;
            for (unsigned i = 0U; i < depth; ++i) {
                chip.script[i] = (uint8_t)(rest % OUTCOME_COUNT);
                rest /= OUTCOME_COUNT;
            }
            const esp_err_t err = tca9548a_mux_close(&mux);
            if (err == ESP_OK) {
                assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);
                assert(chip.reg == 0U);
            } else {
                assert(allowed_error(err));
            }
            check_truthful_state(&mux, &chip);
            assert(chip.calls <= 6U);
        }
    }
    CASE_PASS("MUX.close_failure_patterns");
}

static void invalid_arguments_never_touch_the_bus(void)
{
    tca9548a_mux_t mux;
    chip_t chip;
    setup(&mux, &chip, 1U);

    for (unsigned channel = 8U; channel <= 255U; ++channel) {
        assert(tca9548a_mux_select(&mux, (uint8_t)channel) == ESP_ERR_INVALID_ARG);
    }
    assert(chip.calls == 0U && chip.reg == 0U);
    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);

    assert(tca9548a_mux_select(NULL, 0U) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_close(NULL) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_init(NULL, &(tca9548a_mux_io_t){chip_write, chip_read, &chip, 0x70U}) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_init(&mux, NULL) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_init(&mux, &(tca9548a_mux_io_t){NULL, chip_read, &chip, 0x70U}) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_init(&mux, &(tca9548a_mux_io_t){chip_write, NULL, &chip, 0x70U}) == ESP_ERR_INVALID_ARG);
    assert(tca9548a_mux_get_state(NULL) == TCA9548A_MUX_STATE_FAULT);
    assert(tca9548a_mux_get_selected_channel(NULL) == -1);
    tca9548a_mux_latch_bus_stuck(NULL);
    CASE_PASS("MUX.argument_domain");
}

static void exactly_one_channel_bit_is_ever_written_for_a_selection(void)
{
    for (unsigned channel = 0U; channel < 8U; ++channel) {
        tca9548a_mux_t mux;
        chip_t chip;
        setup(&mux, &chip, 2U);
        assert(tca9548a_mux_select(&mux, (uint8_t)channel) == ESP_OK);
        assert(chip.reg == (uint8_t)(1U << channel));
        /* One close, one select: two writes, two reads (close verify + select verify). */
        assert(chip.writes == 2U && chip.calls == 4U);
        assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_SELECTED);
    }
    CASE_PASS("MUX.one_hot_selection");
}

static void fault_state_names_and_latch_are_stable(void)
{
    tca9548a_mux_t mux;
    chip_t chip;
    static const struct { tca9548a_mux_fault_reason_t reason; const char *name; } names[] = {
        {TCA9548A_MUX_FAULT_NONE, "none"},
        {TCA9548A_MUX_FAULT_VERIFY_TIMEOUT, "verify_timeout"},
        {TCA9548A_MUX_FAULT_READBACK_MISMATCH, "readback_mismatch"},
        {TCA9548A_MUX_FAULT_CLOSE_ERROR, "close_error"},
        {TCA9548A_MUX_FAULT_BUS_STUCK, "TCA_BUS_STUCK"},
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(strcmp(tca9548a_mux_fault_reason_name(names[i].reason), names[i].name) == 0);
    }
    assert(strcmp(tca9548a_mux_fault_reason_name((tca9548a_mux_fault_reason_t)77), "close_error") == 0);

    setup(&mux, &chip, 1U);
    tca9548a_mux_latch_bus_stuck(&mux);
    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_FAULT);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_BUS_STUCK);
    assert(tca9548a_mux_get_selected_channel(&mux) == -1);
    /* A confirmed close clears the latch. */
    assert(tca9548a_mux_close(&mux) == ESP_OK);
    assert(tca9548a_mux_get_state(&mux) == TCA9548A_MUX_STATE_CLOSED);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_NONE);
    CASE_PASS("MUX.fault_latch");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    invalid_arguments_never_touch_the_bus();
    exactly_one_channel_bit_is_ever_written_for_a_selection();
    fault_state_names_and_latch_are_stable();
    close_survives_every_bus_failure_pattern();
    select_survives_every_bus_failure_pattern();
    return 0;
}
