#include <assert.h>
#include <stddef.h>

#include "nrf_topology_bus_recovery.h"

/* MAX-243: topology worker reaction to a failed upstream transfer. */

typedef struct
{
    esp_err_t recover_err;
    i2c_line_result_t lines;
    esp_err_t mux_close_err;
    int recover_calls;
    int mux_close_calls;

} fake_t;

static esp_err_t fake_recover(void *context, i2c_line_result_t *result)
{
    fake_t *fake = context;
    ++fake->recover_calls;
    *result = fake->lines;
    return fake->recover_err;
}

static esp_err_t fake_mux_close(void *context)
{
    fake_t *fake = context;
    ++fake->mux_close_calls;
    return fake->mux_close_err;
}

static nrf_topology_bus_recovery_ops_t ops_for(fake_t *fake)
{
    const nrf_topology_bus_recovery_ops_t ops = {
        .recover = fake_recover, .mux_close = fake_mux_close, .context = fake,
    };
    return ops;
}

static void test_non_bus_errors_never_touch_the_bus(void)
{
    const esp_err_t codes[] = { ESP_OK, ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_RESPONSE };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        fake_t fake = { .recover_err = ESP_OK, .lines = I2C_LINES_RECOVERED };
        const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
        nrf_topology_bus_recovery_outcome_t outcome = NRF_TOPOLOGY_BUS_STUCK;
        assert(nrf_topology_bus_recover_after_error(codes[i], &ops, &outcome) == codes[i]);
        assert(outcome == NRF_TOPOLOGY_BUS_NOT_CHECKED);
        assert(fake.recover_calls == 0 && fake.mux_close_calls == 0);
    }
}

static void test_nack_with_idle_lines_leaves_mux_alone(void)
{
    fake_t fake = { .recover_err = ESP_OK, .lines = I2C_LINES_IDLE };
    const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
    nrf_topology_bus_recovery_outcome_t outcome;
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_INVALID_STATE, &ops, &outcome) == ESP_ERR_INVALID_STATE);
    assert(outcome == NRF_TOPOLOGY_BUS_LINES_IDLE);
    assert(fake.recover_calls == 1);
    assert(fake.mux_close_calls == 0);
}

static void test_recovered_bus_closes_and_verifies_mux(void)
{
    fake_t fake = { .recover_err = ESP_OK, .lines = I2C_LINES_RECOVERED, .mux_close_err = ESP_OK };
    const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
    nrf_topology_bus_recovery_outcome_t outcome;
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, &ops, &outcome) == ESP_ERR_TIMEOUT);
    assert(outcome == NRF_TOPOLOGY_BUS_RECOVERED_MUX_CLOSED);
    assert(fake.mux_close_calls == 1);

    /* Close readback mismatch is surfaced, not swallowed. */
    fake.mux_close_err = ESP_ERR_INVALID_RESPONSE;
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, &ops, &outcome) == ESP_ERR_INVALID_RESPONSE);
    assert(outcome == NRF_TOPOLOGY_BUS_RECOVERED_MUX_FAULT);
    assert(fake.mux_close_calls == 2);

    /* No mux bound -> cannot verify -> fault. */
    const nrf_topology_bus_recovery_ops_t no_mux = { .recover = fake_recover, .mux_close = NULL, .context = &fake };
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, &no_mux, &outcome) == ESP_ERR_INVALID_RESPONSE);
    assert(outcome == NRF_TOPOLOGY_BUS_RECOVERED_MUX_FAULT);
}

static void test_stuck_bus_is_a_fault_and_mux_untouched(void)
{
    const i2c_line_result_t stuck[] = { I2C_LINES_SCL_STUCK, I2C_LINES_SDA_STUCK };
    for (size_t i = 0; i < 2; ++i) {
        fake_t fake = { .recover_err = ESP_ERR_TIMEOUT, .lines = stuck[i] };
        const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
        nrf_topology_bus_recovery_outcome_t outcome;
        assert(nrf_topology_bus_recover_after_error(ESP_ERR_INVALID_STATE, &ops, &outcome) == ESP_ERR_TIMEOUT);
        assert(outcome == NRF_TOPOLOGY_BUS_STUCK);
        assert(fake.mux_close_calls == 0);
    }
}

static void test_backoff_touches_nothing(void)
{
    fake_t fake = { .recover_err = ESP_ERR_INVALID_STATE, .lines = I2C_LINES_RECOVERED };
    const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
    nrf_topology_bus_recovery_outcome_t outcome;
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, &ops, &outcome) == ESP_ERR_TIMEOUT);
    assert(outcome == NRF_TOPOLOGY_BUS_BACKOFF);
    assert(fake.mux_close_calls == 0);
}

static void test_null_safety(void)
{
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, NULL, NULL) == ESP_ERR_TIMEOUT);
    fake_t fake = { .recover_err = ESP_OK, .lines = I2C_LINES_IDLE };
    const nrf_topology_bus_recovery_ops_t ops = ops_for(&fake);
    assert(nrf_topology_bus_recover_after_error(ESP_ERR_TIMEOUT, &ops, NULL) == ESP_ERR_TIMEOUT);
}

int main(void)
{
    test_non_bus_errors_never_touch_the_bus();
    test_nack_with_idle_lines_leaves_mux_alone();
    test_recovered_bus_closes_and_verifies_mux();
    test_stuck_bus_is_a_fault_and_mux_untouched();
    test_backoff_touches_nothing();
    test_null_safety();
    return 0;
}
