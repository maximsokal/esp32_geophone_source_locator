#include "sensor_bus_diag.h"

#include <stdio.h>
#include <string.h>

static const char *line_level_name(int level)
{
    return level == 0 ? "0" : level == 1 ? "1" : "?";
}

static int address_channel(int channel)
{
    return channel >= 0 && channel <= 7 ? channel : -1;
}

bool sensor_bus_diag_rate_limit_allow(
    sensor_bus_diag_state_t *state,
    uint32_t now_ms,
    const char *source)
{
    sensor_bus_diag_slot_t *free_slot = NULL;
    if (state == NULL || source == NULL) {
        return false;
    }

    for (size_t i = 0U; i < SENSOR_BUS_DIAG_SOURCE_LIMIT; ++i) {
        sensor_bus_diag_slot_t *slot = &state->slots[i];
        if (slot->used && strcmp(slot->source, source) == 0) {
            if ((uint32_t)(now_ms - slot->last_emit_ms) <
                SENSOR_BUS_DIAG_INTERVAL_MS) {
                return false;
            }
            slot->last_emit_ms = now_ms;
            return true;
        }
        if (!slot->used && free_slot == NULL) {
            free_slot = slot;
        }
    }

    if (free_slot == NULL) {
        return false;
    }
    free_slot->source = source;
    free_slot->last_emit_ms = now_ms;
    free_slot->used = true;
    return true;
}

const char *sensor_bus_diag_device_name(uint8_t address, const char *source)
{
    if (address == 0x76U || address == 0x77U) {
        if (source != NULL && strcmp(source, "main_barometer") == 0) {
            return "BMP280";
        }
        if (source != NULL && strcmp(source, "tca9548a") == 0) {
            return "TCA9548A";
        }
        return "TCA9548A/BMP280?";
    }
    if (address == 0x40U) {
        return "INA3221";
    }
    if (address >= 0x70U && address <= 0x77U) {
        return "TCA9548A";
    }
    if (address >= 0x30U && address <= 0x32U) {
        return "XIAO_UPSTREAM";
    }
    return "unknown";
}

bool sensor_bus_diag_format(
    char *buffer,
    size_t buffer_size,
    const char *source,
    uint8_t address,
    int channel,
    const char *error_name,
    int sda,
    int scl)
{
    if (buffer == NULL || buffer_size == 0U) {
        return false;
    }

    const char *source_name = source != NULL ? source : "bus";
    const char *device_name = sensor_bus_diag_device_name(address, source_name);
    const char *err_name = error_name != NULL ? error_name : "unknown";
    const int normalized_channel = address_channel(channel);
    const int written = normalized_channel < 0
        ? snprintf(buffer, buffer_size,
                   "I2C_TIMEOUT_DIAG src=%s addr=0x%02X dev=%s ch=direct err=%s sda=%s scl=%s",
                   source_name, (unsigned)address, device_name, err_name,
                   line_level_name(sda), line_level_name(scl))
        : snprintf(buffer, buffer_size,
                   "I2C_TIMEOUT_DIAG src=%s addr=0x%02X dev=%s ch=%d err=%s sda=%s scl=%s",
                   source_name, (unsigned)address, device_name, normalized_channel,
                   err_name, line_level_name(sda), line_level_name(scl));
    return written >= 0 && (size_t)written < buffer_size;
}

bool sensor_bus_diag_format_limited(
    sensor_bus_diag_state_t *state,
    uint32_t now_ms,
    const char *source,
    uint8_t address,
    int channel,
    const char *error_name,
    int sda,
    int scl,
    char *buffer,
    size_t buffer_size)
{
    const char *caller = source != NULL ? source : "bus";
    return sensor_bus_diag_format(
               buffer, buffer_size, caller, address, channel,
               error_name, sda, scl) &&
        sensor_bus_diag_rate_limit_allow(state, now_ms, caller);
}

static sensor_bus_diag_address_t *find_or_add_address(
    sensor_bus_diag_stats_t *stats,
    uint32_t now_ms,
    uint8_t address)
{
    sensor_bus_diag_address_t *free_entry = NULL;
    for (size_t i = 0U; i < SENSOR_BUS_DIAG_ADDRESS_LIMIT; ++i) {
        sensor_bus_diag_address_t *entry = &stats->addresses[i];
        if (entry->used && entry->address == address) {
            return entry;
        }
        if (!entry->used && free_entry == NULL) {
            free_entry = entry;
        }
    }

    if (free_entry == NULL) {
        ++stats->overflow_count;
        stats->last_overflow_ms = now_ms;
        return NULL;
    }

    memset(free_entry, 0, sizeof(*free_entry));
    free_entry->used = 1U;
    free_entry->address = address;
    free_entry->last_channel = -1;
    free_entry->last_sda = -1;
    free_entry->last_scl = -1;
    return free_entry;
}

static void copy_source(char destination[24], const char *source)
{
    const char *value = source != NULL ? source : "bus";
    size_t i = 0U;
    while (i < 23U && value[i] != '\0') {
        destination[i] = value[i];
        ++i;
    }
    destination[i] = '\0';
}

bool sensor_bus_diag_record_result(
    sensor_bus_diag_stats_t *stats,
    uint32_t now_ms,
    uint8_t address,
    int channel,
    int err,
    sensor_bus_diag_result_t result,
    int sda,
    int scl,
    const char *source)
{
    if (stats == NULL || address > 0x7FU) {
        return false;
    }

    sensor_bus_diag_address_t *entry =
        find_or_add_address(stats, now_ms, address);
    if (entry == NULL) {
        return false;
    }

    if (result == SENSOR_BUS_DIAG_RESULT_OK) {
        ++entry->ok_count;
        entry->last_ok_ms = now_ms;
        return true;
    }

    switch (result) {
        case SENSOR_BUS_DIAG_RESULT_NACK:
            ++entry->nack_count;
            break;
        case SENSOR_BUS_DIAG_RESULT_NACK_INVALID_STATE:
            ++entry->nack_count;
            ++entry->nack_invalid_state_count;
            break;
        case SENSOR_BUS_DIAG_RESULT_TIMEOUT:
            ++entry->timeout_count;
            break;
        case SENSOR_BUS_DIAG_RESULT_OTHER:
        case SENSOR_BUS_DIAG_RESULT_OK:
        default:
            ++entry->other_count;
            break;
    }

    entry->last_err = err;
    entry->last_channel = (int8_t)address_channel(channel);
    entry->last_sda = (int8_t)(sda == 0 || sda == 1 ? sda : -1);
    entry->last_scl = (int8_t)(scl == 0 || scl == 1 ? scl : -1);
    entry->last_failure_ms = now_ms;
    copy_source(entry->last_failure_source, source);
    return true;
}

bool sensor_bus_diag_summary_capture(
    sensor_bus_diag_stats_t *stats,
    uint32_t now_ms,
    sensor_bus_diag_summary_t *summary)
{
    if (stats == NULL || summary == NULL) {
        return false;
    }

    memset(summary, 0, sizeof(*summary));
    summary->now_ms = now_ms;
    summary->overflow_count = stats->overflow_count;
    if (!stats->summary_started) {
        stats->summary_started = true;
        stats->last_summary_ms = now_ms;
    } else if ((uint32_t)(now_ms - stats->last_summary_ms) <
               SENSOR_BUS_DIAG_SUMMARY_INTERVAL_MS) {
        return false;
    } else {
        stats->last_summary_ms = now_ms;
    }

    bool recent_failure = stats->overflow_count > 0U &&
        (uint32_t)(now_ms - stats->last_overflow_ms) <=
            SENSOR_BUS_DIAG_SUMMARY_INTERVAL_MS;
    size_t last_captured = stats->summary_cursor;
    for (size_t scanned = 0U;
         scanned < SENSOR_BUS_DIAG_ADDRESS_LIMIT;
         ++scanned) {
        const size_t index =
            (stats->summary_cursor + scanned) % SENSOR_BUS_DIAG_ADDRESS_LIMIT;
        const sensor_bus_diag_address_t *entry = &stats->addresses[index];
        if (!entry->used ||
            entry->nack_count + entry->timeout_count + entry->other_count == 0U ||
            (uint32_t)(now_ms - entry->last_failure_ms) >
                SENSOR_BUS_DIAG_SUMMARY_INTERVAL_MS) {
            continue;
        }
        recent_failure = true;
        if (summary->address_count < SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT) {
            summary->addresses[summary->address_count++] = *entry;
            last_captured = index;
        } else {
            break;
        }
    }

    if (summary->address_count > 0U) {
        stats->summary_cursor =
            (last_captured + 1U) % SENSOR_BUS_DIAG_ADDRESS_LIMIT;
    }
    summary->should_emit = recent_failure;
    return summary->should_emit;
}

bool sensor_bus_diag_format_summary_header(
    char *buffer,
    size_t buffer_size,
    uint32_t recovery_attempts,
    uint32_t recoveries,
    uint32_t recovery_failures,
    uint32_t consecutive_failures,
    uint32_t table_overflow,
    int sda,
    int scl)
{
    if (buffer == NULL || buffer_size == 0U) {
        return false;
    }
    const int written = snprintf(
        buffer, buffer_size,
        "I2C_BUS_SUMMARY recover_attempts=%lu recoveries=%lu recover_failures=%lu recover_consecutive=%lu table_overflow=%lu sda=%s scl=%s",
        (unsigned long)recovery_attempts,
        (unsigned long)recoveries,
        (unsigned long)recovery_failures,
        (unsigned long)consecutive_failures,
        (unsigned long)table_overflow,
        line_level_name(sda), line_level_name(scl));
    return written >= 0 && (size_t)written < buffer_size;
}

bool sensor_bus_diag_format_summary_address(
    char *buffer,
    size_t buffer_size,
    const sensor_bus_diag_address_t *address,
    uint32_t now_ms,
    const char *error_name)
{
    if (buffer == NULL || buffer_size == 0U || address == NULL ||
        !address->used) {
        return false;
    }
    const char *err_name = error_name != NULL ? error_name : "unknown";
    const char *channel_name = address->last_channel < 0
        ? "direct" : NULL;
    char channel_buffer[4];
    if (channel_name == NULL) {
        (void)snprintf(channel_buffer, sizeof(channel_buffer), "%d",
                       (int)address->last_channel);
        channel_name = channel_buffer;
    }
    const uint32_t age_ok_ms = address->ok_count == 0U
        ? UINT32_MAX : (uint32_t)(now_ms - address->last_ok_ms);
    const uint32_t age_failure_ms =
        (uint32_t)(now_ms - address->last_failure_ms);
    const int written = snprintf(
        buffer, buffer_size,
        "I2C_BUS_SUMMARY addr=0x%02X dev=%s ok=%lu nack=%lu timeout=%lu other=%lu last_err=%s last_ch=%s sda=%s scl=%s age_ok_ms=%lu age_fail_ms=%lu",
        (unsigned)address->address,
        sensor_bus_diag_device_name(
            address->address, address->last_failure_source),
        (unsigned long)address->ok_count,
        (unsigned long)address->nack_count,
        (unsigned long)address->timeout_count,
        (unsigned long)address->other_count,
        err_name,
        channel_name,
        line_level_name(address->last_sda),
        line_level_name(address->last_scl),
        (unsigned long)age_ok_ms,
        (unsigned long)age_failure_ms);
    return written >= 0 && (size_t)written < buffer_size;
}

size_t sensor_bus_diag_emit_summary(
    const sensor_bus_diag_summary_t *summary,
    uint32_t recovery_attempts,
    uint32_t recoveries,
    uint32_t recovery_failures,
    uint32_t consecutive_failures,
    int sda,
    int scl,
    sensor_bus_diag_error_name_fn error_name,
    sensor_bus_diag_print_fn print,
    void *context)
{
    if (summary == NULL || !summary->should_emit || print == NULL ||
        summary->address_count > SENSOR_BUS_DIAG_SUMMARY_ADDRESS_LIMIT) {
        return 0U;
    }

    size_t lines = 0U;
    char line[256];
    if (sensor_bus_diag_format_summary_header(
            line, sizeof(line), recovery_attempts, recoveries,
            recovery_failures, consecutive_failures, summary->overflow_count,
            sda, scl)) {
        print(line, context);
        ++lines;
    }

    for (size_t i = 0U; i < summary->address_count; ++i) {
        const sensor_bus_diag_address_t *address = &summary->addresses[i];
        const char *name = error_name != NULL
            ? error_name(address->last_err, context) : NULL;
        if (sensor_bus_diag_format_summary_address(
                line, sizeof(line), address, summary->now_ms, name)) {
            print(line, context);
            ++lines;
        }
    }
    return lines;
}
