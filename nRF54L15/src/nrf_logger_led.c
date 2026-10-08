#include "nrf_logger_led.h"

static void nrf_logger_led_increment(uint32_t *counter)
{
    if (*counter < UINT32_MAX) {
        ++(*counter);
    }
}

static void nrf_logger_led_clear_pending(nrf_logger_led_t *led)
{
    for (uint32_t source = 0U; source < (uint32_t)NRF_LOGGER_LED_SOURCE_COUNT;
         ++source) {
        led->pending_active[source] = false;
        led->pending_levels[source] = NRF_LOGGER_LED_SEVERITY_NONE;
        led->pending_since_ms[source] = 0U;
    }
}

static void nrf_logger_led_latch(
    nrf_logger_led_t *led,
    nrf_logger_led_severity_t severity,
    nrf_logger_led_source_t source)
{
    if (severity > led->output.severity_latched) {
        led->output.severity_latched = severity;
        led->output.reason_source = source;
    }
}

static void nrf_logger_led_render(nrf_logger_led_t *led, bool force_red)
{
    led->output.green = false;
    led->output.yellow = false;
    led->output.red = false;

    if (force_red) {
        led->output.red = true;
        return;
    }

    if (led->output.severity_latched == NRF_LOGGER_LED_SEVERITY_NONE) {
        led->output.green = true;
    } else if (led->output.severity_latched == NRF_LOGGER_LED_SEVERITY_WARNING) {
        led->output.yellow = true;
    } else {
        led->output.red = true;
    }
}

static void nrf_logger_led_render_for_state(
    nrf_logger_led_t *led,
    const nrf_node_state_t *node_state)
{
    if (node_state->role == NRF_NODE_ROLE_REMOTE &&
        node_state->lifecycle == NRF_NODE_LIFECYCLE_READY) {
        nrf_logger_led_render(led, false);
    } else {
        led->output.green = false;
        led->output.yellow = false;
        led->output.red = false;
    }
}

static nrf_logger_led_severity_t nrf_logger_led_source_level(
    nrf_logger_led_t *led,
    const nrf_logger_led_input_t *in,
    uint32_t source)
{
    int32_t raw_level = (int32_t)in->source_levels[source];

    if (raw_level < (int32_t)NRF_LOGGER_LED_SEVERITY_NONE ||
        raw_level > (int32_t)NRF_LOGGER_LED_SEVERITY_FATAL) {
        nrf_logger_led_increment(&led->invalid_input_count);
        return NRF_LOGGER_LED_SEVERITY_ERROR;
    }

    return (nrf_logger_led_severity_t)raw_level;
}

static uint32_t nrf_logger_led_confirm_ms(
    nrf_logger_led_severity_t severity)
{
    if (severity == NRF_LOGGER_LED_SEVERITY_WARNING) {
        return NRF_LOGGER_LED_WARNING_CONFIRM_MS;
    }

    return 0U;
}

static void nrf_logger_led_update_source(
    nrf_logger_led_t *led,
    uint32_t source,
    nrf_logger_led_severity_t severity,
    uint32_t now_ms)
{
    uint32_t confirm_ms;

    if (severity == NRF_LOGGER_LED_SEVERITY_NONE) {
        led->pending_active[source] = false;
        led->pending_levels[source] = NRF_LOGGER_LED_SEVERITY_NONE;
        return;
    }

    if (!led->pending_active[source] ||
        led->pending_levels[source] != severity) {
        led->pending_active[source] = true;
        led->pending_levels[source] = severity;
        led->pending_since_ms[source] = now_ms;
    }

    confirm_ms = nrf_logger_led_confirm_ms(severity);
    if ((uint32_t)(now_ms - led->pending_since_ms[source]) >= confirm_ms) {
        nrf_logger_led_latch(
            led, severity, (nrf_logger_led_source_t)source);
    }
}

void nrf_logger_led_reset(nrf_logger_led_t *led)
{
    if (led == 0) {
        return;
    }

    nrf_logger_led_clear_pending(led);
    led->output.green = false;
    led->output.yellow = false;
    led->output.red = false;
    led->output.severity_latched = NRF_LOGGER_LED_SEVERITY_NONE;
    led->output.reason_source = NRF_LOGGER_LED_SOURCE_NONE;
    led->last_update_ms = 0U;
    led->invalid_input_count = 0U;
    led->timestamp_anomaly_count = 0U;
    led->has_last_update = false;
}

nrf_logger_led_result_t nrf_logger_led_update(
    nrf_logger_led_t *led,
    const nrf_logger_led_input_t *in,
    uint32_t now_ms)
{
    uint32_t effective_now_ms;
    uint32_t source;
    bool remote_role;
    bool valid_lifecycle;

    if (led == 0) {
        return NRF_LOGGER_LED_ERR_ARGUMENT;
    }

    if (in == 0) {
        nrf_logger_led_increment(&led->invalid_input_count);
        nrf_logger_led_clear_pending(led);
        led->has_last_update = false;
        nrf_logger_led_latch(
            led,
            NRF_LOGGER_LED_SEVERITY_ERROR,
            NRF_LOGGER_LED_SOURCE_INVALID_INPUT);
        nrf_logger_led_render(led, true);
        return NRF_LOGGER_LED_ERR_ARGUMENT;
    }

    if (!led->has_last_update) {
        led->has_last_update = true;
        led->last_update_ms = now_ms;
        effective_now_ms = now_ms;
    } else {
        uint32_t delta_ms = now_ms - led->last_update_ms;
        if (delta_ms == 0U) {
            nrf_logger_led_increment(&led->timestamp_anomaly_count);
            effective_now_ms = led->last_update_ms;
        } else if (delta_ms >= UINT32_C(0x80000000)) {
            nrf_logger_led_increment(&led->timestamp_anomaly_count);
            effective_now_ms = led->last_update_ms;
        } else {
            led->last_update_ms = now_ms;
            effective_now_ms = now_ms;
        }
    }

    remote_role = in->node_state.role == NRF_NODE_ROLE_REMOTE;
    if (!remote_role) {
        nrf_logger_led_clear_pending(led);
        nrf_logger_led_render_for_state(led, &in->node_state);
        return NRF_LOGGER_LED_OK;
    }

    valid_lifecycle =
        in->node_state.lifecycle == NRF_NODE_LIFECYCLE_BOOTING ||
        in->node_state.lifecycle == NRF_NODE_LIFECYCLE_READY ||
        in->node_state.lifecycle == NRF_NODE_LIFECYCLE_UNAVAILABLE;
    if (!valid_lifecycle) {
        nrf_logger_led_increment(&led->invalid_input_count);
        nrf_logger_led_clear_pending(led);
        nrf_logger_led_render_for_state(led, &in->node_state);
        return NRF_LOGGER_LED_OK;
    }

    for (source = 0U; source < (uint32_t)NRF_LOGGER_LED_SOURCE_COUNT; ++source) {
        nrf_logger_led_severity_t severity =
            nrf_logger_led_source_level(led, in, source);

        if (source == (uint32_t)NRF_LOGGER_LED_SOURCE_BME280_FAULT &&
            in->node_state.lifecycle == NRF_NODE_LIFECYCLE_READY &&
            !in->bme280_ready &&
            severity < NRF_LOGGER_LED_SEVERITY_ERROR) {
            severity = NRF_LOGGER_LED_SEVERITY_ERROR;
        }

        nrf_logger_led_update_source(led, source, severity, effective_now_ms);
    }

    nrf_logger_led_render_for_state(led, &in->node_state);
    return NRF_LOGGER_LED_OK;
}
