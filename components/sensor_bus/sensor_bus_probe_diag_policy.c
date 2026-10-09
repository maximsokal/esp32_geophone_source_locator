#include "sensor_bus_probe_diag_policy.h"

#include <limits.h>
#include <string.h>

#define GROUP_FLAG_USED 0x01U
#define GROUP_FLAG_PREVIOUS_RESULT 0x02U
#define GROUP_RESULT_MISMATCH 0x80U
#define GROUP_KEY_CHANNEL_MASK 0x0FU
#define GROUP_KEY_ADDRESS_MASK 0x7FU
#define GROUP_KEY_PHASE_MASK 0x07U
#define GROUP_KEY_ADDRESS_SHIFT 4U
#define GROUP_KEY_PHASE_SHIFT 11U
#define GROUP_CHANNEL_DIRECT 0x0FU

uint32_t sensor_bus_probe_diag_saturating_add_u32(uint32_t left, uint32_t right)
{
    return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}

static uint16_t saturating_increment_u16(uint16_t value)
{
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1U);
}

static uint32_t clamp_elapsed_us(uint64_t elapsed_us)
{
    return elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
}

static uint32_t elapsed_seconds(uint64_t now_us)
{
    return (uint32_t)(now_us / UINT64_C(1000000));
}

static uint32_t fractional_microseconds(uint64_t now_us)
{
    return (uint32_t)(now_us % UINT64_C(1000000));
}

static uint16_t make_group_key(
    const sensor_bus_probe_diag_transaction_t *transaction)
{
    const uint16_t phase = (uint16_t)transaction->phase;
    const uint16_t address = (uint16_t)transaction->address;
    const uint16_t channel = transaction->channel < 0
        ? GROUP_CHANNEL_DIRECT : (uint16_t)transaction->channel;

    return (uint16_t)((phase << GROUP_KEY_PHASE_SHIFT) |
        (address << GROUP_KEY_ADDRESS_SHIFT) | channel);
}

static sensor_bus_probe_diag_group_t *find_group(
    sensor_bus_probe_diag_policy_t *state,
    uint16_t key,
    uint32_t now_seconds,
    uint32_t now_fraction_us,
    bool *is_new)
{
    sensor_bus_probe_diag_group_t *free_group = NULL;

    for (size_t i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        sensor_bus_probe_diag_group_t *group = &state->groups[i];
        if ((group->flags & GROUP_FLAG_USED) != 0U && group->key == key) {
            *is_new = false;
            return group;
        }
        if ((group->flags & GROUP_FLAG_USED) == 0U && free_group == NULL) {
            free_group = group;
        }
    }

    if (free_group == NULL) {
        return NULL;
    }

    memset(free_group, 0, sizeof(*free_group));
    free_group->key = key;
    free_group->flags = GROUP_FLAG_USED;
    free_group->last_summary_seconds = now_seconds;
    free_group->last_summary_fraction_us = now_fraction_us;
    *is_new = true;
    return free_group;
}

static void update_average(sensor_bus_probe_diag_group_t *group, uint32_t sample_us)
{
    if (group->sample_count == 0U || group->sample_count == UINT32_MAX) {
        /* Restart the mean window at count saturation; counters remain saturated. */
        group->avg_us = sample_us;
        group->sample_count = 1U;
        return;
    }

    const uint32_t next_count = group->sample_count + 1U;
    const uint64_t weighted_total =
        (uint64_t)group->avg_us * group->sample_count + sample_us;
    group->avg_us = (uint32_t)(weighted_total / next_count);
    group->sample_count = next_count;
}

void sensor_bus_probe_diag_policy_reset(sensor_bus_probe_diag_policy_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void sensor_bus_probe_diag_policy_begin_pass(
    sensor_bus_probe_diag_policy_t *state,
    uint32_t pass_index,
    uint32_t full_passes)
{
    if (state == NULL) {
        return;
    }
    state->pass_index = pass_index;
    state->pass_active = true;
    state->pass_full = pass_index > 0U && pass_index <= full_passes;
}

bool sensor_bus_probe_diag_policy_record(
    sensor_bus_probe_diag_policy_t *state,
    const sensor_bus_probe_diag_transaction_t *transaction,
    uint64_t now_us,
    bool *changed,
    bool *should_print)
{
    sensor_bus_probe_diag_group_t *group;
    uint16_t key;
    uint8_t result_code;
    uint32_t seconds;
    uint32_t fraction_us;
    uint32_t elapsed;
    bool is_new = false;
    bool result_changed;

    if (changed != NULL) {
        *changed = false;
    }
    if (should_print != NULL) {
        *should_print = false;
    }
    if (state == NULL || transaction == NULL ||
        (unsigned)transaction->phase >= (unsigned)SENSOR_BUS_PROBE_PHASE_COUNT ||
        (unsigned)transaction->result >= (unsigned)SENSOR_BUS_PROBE_RESULT_COUNT ||
        (unsigned)transaction->address > GROUP_KEY_ADDRESS_MASK ||
        transaction->channel < -1 || transaction->channel > 7) {
        return false;
    }

    seconds = elapsed_seconds(now_us);
    fraction_us = fractional_microseconds(now_us);
    key = make_group_key(transaction);
    group = find_group(state, key, seconds, fraction_us, &is_new);
    if (group == NULL) {
        /* Each untracked arrival counts once because its key is not retained. */
        state->groups_dropped = sensor_bus_probe_diag_saturating_add_u32(
            state->groups_dropped, 1U);
        if (changed != NULL) {
            *changed = true;
        }
        if (should_print != NULL) {
            *should_print = true;
        }
        return true;
    }

    result_code = (uint8_t)transaction->result;
    if (transaction->readback_mismatch) {
        result_code = (uint8_t)(result_code | GROUP_RESULT_MISMATCH);
    }
    result_changed = (group->flags & GROUP_FLAG_PREVIOUS_RESULT) == 0U ||
        group->last_result != result_code || group->last_err != transaction->err;
    group->last_result = result_code;
    group->last_err = transaction->err;
    group->flags = (uint8_t)(group->flags | GROUP_FLAG_PREVIOUS_RESULT);
    if (changed != NULL) {
        *changed = result_changed;
    }
    if (should_print != NULL) {
        *should_print = state->pass_full || result_changed || is_new;
    }

    group->counts[transaction->result] =
        saturating_increment_u16(group->counts[transaction->result]);
    elapsed = clamp_elapsed_us(transaction->elapsed_us);
    if (group->sample_count == 0U) {
        group->min_us = elapsed;
        group->max_us = elapsed;
    } else {
        if (elapsed < group->min_us) {
            group->min_us = elapsed;
        }
        if (elapsed > group->max_us) {
            group->max_us = elapsed;
        }
    }
    update_average(group, elapsed);
    return true;
}

uint32_t sensor_bus_probe_diag_policy_end_pass(
    sensor_bus_probe_diag_policy_t *state)
{
    uint32_t groups_dropped;
    if (state == NULL || !state->pass_active) {
        return 0U;
    }
    state->pass_active = false;
    groups_dropped = state->groups_dropped;
    state->groups_dropped = 0U;
    return groups_dropped;
}

bool sensor_bus_probe_diag_policy_idle_changed(
    sensor_bus_probe_diag_policy_t *state,
    int sda,
    int scl)
{
    bool changed;
    bool emit;
    if (state == NULL || (sda != 0 && sda != 1) ||
        (scl != 0 && scl != 1)) {
        return false;
    }
    changed = !state->idle_valid || state->idle_sda != sda ||
        state->idle_scl != scl;
    emit = (state->pass_active && state->pass_full) || changed;
    state->idle_valid = true;
    state->idle_sda = (int8_t)sda;
    state->idle_scl = (int8_t)scl;
    return emit;
}

bool sensor_bus_probe_diag_policy_take_summary(
    sensor_bus_probe_diag_policy_t *state,
    size_t group_index,
    uint64_t now_us,
    sensor_bus_probe_diag_group_t *summary)
{
    sensor_bus_probe_diag_group_t *group;
    const uint32_t seconds = elapsed_seconds(now_us);
    const uint32_t fraction_us = fractional_microseconds(now_us);
    uint32_t elapsed_summary_seconds;
    if (state == NULL || summary == NULL ||
        group_index >= SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT) {
        return false;
    }
    group = &state->groups[group_index];
    elapsed_summary_seconds = seconds - group->last_summary_seconds;
    if ((group->flags & GROUP_FLAG_USED) == 0U || group->sample_count == 0U ||
        elapsed_summary_seconds < SENSOR_BUS_PROBE_DIAG_SUMMARY_INTERVAL_SECONDS ||
        (elapsed_summary_seconds == SENSOR_BUS_PROBE_DIAG_SUMMARY_INTERVAL_SECONDS &&
         fraction_us < group->last_summary_fraction_us)) {
        return false;
    }

    *summary = *group;
    memset(group->counts, 0, sizeof(group->counts));
    group->sample_count = 0U;
    group->min_us = 0U;
    group->max_us = 0U;
    group->avg_us = 0U;
    group->last_summary_seconds = seconds;
    group->last_summary_fraction_us = fraction_us;
    return true;
}

sensor_bus_probe_phase_t sensor_bus_probe_diag_group_phase(
    const sensor_bus_probe_diag_group_t *group)
{
    if (group == NULL) {
        return SENSOR_BUS_PROBE_PHASE_COUNT;
    }
    return (sensor_bus_probe_phase_t)(
        (group->key >> GROUP_KEY_PHASE_SHIFT) & GROUP_KEY_PHASE_MASK);
}

uint8_t sensor_bus_probe_diag_group_address(
    const sensor_bus_probe_diag_group_t *group)
{
    if (group == NULL) {
        return 0U;
    }
    return (uint8_t)((group->key >> GROUP_KEY_ADDRESS_SHIFT) &
        GROUP_KEY_ADDRESS_MASK);
}

int8_t sensor_bus_probe_diag_group_channel(
    const sensor_bus_probe_diag_group_t *group)
{
    uint8_t channel;
    if (group == NULL) {
        return -1;
    }
    channel = (uint8_t)(group->key & GROUP_KEY_CHANNEL_MASK);
    return channel == GROUP_CHANNEL_DIRECT ? -1 : (int8_t)channel;
}

const char *sensor_bus_probe_diag_phase_name(sensor_bus_probe_phase_t phase)
{
    switch (phase) {
        case SENSOR_BUS_PROBE_PHASE_TCA_CLOSE: return "tca_close";
        case SENSOR_BUS_PROBE_PHASE_TCA_SELECT: return "tca_select";
        case SENSOR_BUS_PROBE_PHASE_TCA_VERIFY: return "tca_verify";
        case SENSOR_BUS_PROBE_PHASE_PROBE: return "probe";
        case SENSOR_BUS_PROBE_PHASE_READ: return "read";
        case SENSOR_BUS_PROBE_PHASE_WRITE: return "write";
        case SENSOR_BUS_PROBE_PHASE_COUNT:
        default: return "unknown";
    }
}

static char hex_digit(uint8_t nibble)
{
    return (unsigned)nibble < 10U
        ? (char)('0' + nibble)
        : (char)('A' + (nibble - 10U));
}

bool sensor_bus_probe_diag_format_hex(
    char *buffer,
    size_t buffer_size,
    const uint8_t *bytes,
    size_t byte_count)
{
    if (buffer == NULL || buffer_size == 0U ||
        (byte_count > 0U && bytes == NULL)) {
        return false;
    }
    if (byte_count > SENSOR_BUS_PROBE_DIAG_BYTES) {
        byte_count = SENSOR_BUS_PROBE_DIAG_BYTES;
    }
    const size_t required = byte_count == 0U ? 2U : byte_count * 2U + 1U;
    if (buffer_size < required) {
        buffer[0] = '\0';
        return false;
    }
    if (byte_count == 0U) {
        buffer[0] = '-';
        buffer[1] = '\0';
        return true;
    }
    for (size_t i = 0U; i < byte_count; ++i) {
        buffer[i * 2U] = hex_digit((uint8_t)((bytes[i] >> 4U) & 0x0FU));
        buffer[i * 2U + 1U] = hex_digit((uint8_t)(bytes[i] & 0x0FU));
    }
    buffer[byte_count * 2U] = '\0';
    return true;
}
