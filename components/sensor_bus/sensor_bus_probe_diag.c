#include "sensor_bus_probe_diag.h"

#include "sensor_bus_lines.h"

#include "esp_log.h"
#include "esp_timer.h"

#ifndef CONFIG_APP_I2C_PROBE_DIAGNOSTICS
#define CONFIG_APP_I2C_PROBE_DIAGNOSTICS 0
#endif
#ifndef CONFIG_APP_I2C_PROBE_DIAGNOSTICS_FULL_PASSES
#define CONFIG_APP_I2C_PROBE_DIAGNOSTICS_FULL_PASSES 3
#endif

#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
#define TAG "I2C_PROBE"

_Static_assert(sizeof(sensor_bus_probe_diag_group_t) <= 48U,
               "I2C probe group record must stay compact");
_Static_assert(sizeof(sensor_bus_probe_diag_policy_t) <=
                   SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES,
               "I2C probe policy exceeds static RAM budget");
_Static_assert(sizeof(sensor_bus_probe_diag_storage_t) <=
                   SENSOR_BUS_PROBE_DIAG_STATIC_RAM_BUDGET_BYTES,
               "I2C probe static storage exceeds 6144 bytes");

/* All diagnostic calls run on the serialized topology/service I2C path. */
static sensor_bus_probe_diag_storage_t s_storage;

static bool is_probe_address(uint8_t address)
{
    return (address >= 0x30U && address <= 0x32U) ||
        (address >= 0x70U && address <= 0x75U);
}

static sensor_bus_probe_result_t result_class(
    esp_err_t err,
    bool readback_mismatch)
{
    if (readback_mismatch) {
        return SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH;
    }
    if (err == ESP_OK) {
        return SENSOR_BUS_PROBE_RESULT_OK;
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_FAIL) {
        return SENSOR_BUS_PROBE_RESULT_NACK;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return SENSOR_BUS_PROBE_RESULT_TIMEOUT;
    }
    return SENSOR_BUS_PROBE_RESULT_OTHER;
}

static uint64_t now_us(void)
{
    const int64_t value = esp_timer_get_time();
    return value > 0 ? (uint64_t)value : 0U;
}

static void emit_transaction(
    const sensor_bus_probe_diag_transaction_t *transaction)
{
    const bool has_rx = transaction->rx_len > 0U &&
        (transaction->err == ESP_OK || transaction->readback_mismatch);
    const bool has_expected = transaction->expected_valid;
    const unsigned err_code = (unsigned)transaction->err;

    (void)sensor_bus_probe_diag_format_hex(
        s_storage.tx_hex, sizeof(s_storage.tx_hex), transaction->tx,
        transaction->tx_len);
    (void)sensor_bus_probe_diag_format_hex(
        s_storage.rx_hex, sizeof(s_storage.rx_hex), has_rx ? transaction->rx : NULL,
        has_rx ? transaction->rx_len : 0U);
    (void)sensor_bus_probe_diag_format_hex(
        s_storage.expected_hex, sizeof(s_storage.expected_hex),
        has_expected ? &transaction->expected : NULL,
        has_expected ? 1U : 0U);

    if (transaction->channel < 0) {
        if (transaction->err == ESP_OK && !transaction->readback_mismatch) {
            ESP_LOGI(TAG,
                     "I2C_PROBE phase=%s addr=0x%02X ch=direct tx=%s rx=%s exp=%s err=0x%X(%s) us=%llu lvl=SDA%d/SCL%d>%d%d",
                     sensor_bus_probe_diag_phase_name(transaction->phase),
                     (unsigned)transaction->address, s_storage.tx_hex,
                     s_storage.rx_hex, s_storage.expected_hex, err_code,
                     esp_err_to_name((esp_err_t)transaction->err),
                     (unsigned long long)transaction->elapsed_us,
                     (int)transaction->sda_before, (int)transaction->scl_before,
                     (int)transaction->sda_after, (int)transaction->scl_after);
        } else {
            ESP_LOGW(TAG,
                     "I2C_PROBE phase=%s addr=0x%02X ch=direct tx=%s rx=%s exp=%s err=0x%X(%s) us=%llu lvl=SDA%d/SCL%d>%d%d",
                     sensor_bus_probe_diag_phase_name(transaction->phase),
                     (unsigned)transaction->address, s_storage.tx_hex,
                     s_storage.rx_hex, s_storage.expected_hex, err_code,
                     esp_err_to_name((esp_err_t)transaction->err),
                     (unsigned long long)transaction->elapsed_us,
                     (int)transaction->sda_before, (int)transaction->scl_before,
                     (int)transaction->sda_after, (int)transaction->scl_after);
        }
        return;
    }

    if (transaction->err == ESP_OK && !transaction->readback_mismatch) {
        ESP_LOGI(TAG,
                 "I2C_PROBE phase=%s addr=0x%02X ch=%d tx=%s rx=%s exp=%s err=0x%X(%s) us=%llu lvl=SDA%d/SCL%d>%d%d",
                 sensor_bus_probe_diag_phase_name(transaction->phase),
                 (unsigned)transaction->address, (int)transaction->channel,
                 s_storage.tx_hex, s_storage.rx_hex, s_storage.expected_hex,
                 err_code, esp_err_to_name((esp_err_t)transaction->err),
                 (unsigned long long)transaction->elapsed_us,
                 (int)transaction->sda_before, (int)transaction->scl_before,
                 (int)transaction->sda_after, (int)transaction->scl_after);
    } else {
        ESP_LOGW(TAG,
                 "I2C_PROBE phase=%s addr=0x%02X ch=%d tx=%s rx=%s exp=%s err=0x%X(%s) us=%llu lvl=SDA%d/SCL%d>%d%d",
                 sensor_bus_probe_diag_phase_name(transaction->phase),
                 (unsigned)transaction->address, (int)transaction->channel,
                 s_storage.tx_hex, s_storage.rx_hex, s_storage.expected_hex,
                 err_code, esp_err_to_name((esp_err_t)transaction->err),
                 (unsigned long long)transaction->elapsed_us,
                 (int)transaction->sda_before, (int)transaction->scl_before,
                 (int)transaction->sda_after, (int)transaction->scl_after);
    }
}

static void emit_summary(const sensor_bus_probe_diag_group_t *summary)
{
    const int8_t channel = sensor_bus_probe_diag_group_channel(summary);
    const sensor_bus_probe_phase_t phase =
        sensor_bus_probe_diag_group_phase(summary);
    const uint8_t address = sensor_bus_probe_diag_group_address(summary);

    if (channel < 0) {
        ESP_LOGI(TAG,
                 "I2C_PROBE_SUMMARY phase=%s addr=0x%02X ch=direct ok=%lu nack=%lu timeout=%lu other=%lu readback_mismatch=%lu min_us=%llu max_us=%llu avg_us=%llu last_err=0x%X(%s)",
                 sensor_bus_probe_diag_phase_name(phase), (unsigned)address,
                 (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_OK],
                 (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_NACK],
                 (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_TIMEOUT],
                 (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_OTHER],
                 (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH],
                 (unsigned long long)summary->min_us,
                 (unsigned long long)summary->max_us,
                 (unsigned long long)summary->avg_us,
                 (unsigned)summary->last_err,
                 esp_err_to_name((esp_err_t)summary->last_err));
        return;
    }
    ESP_LOGI(TAG,
             "I2C_PROBE_SUMMARY phase=%s addr=0x%02X ch=%d ok=%lu nack=%lu timeout=%lu other=%lu readback_mismatch=%lu min_us=%llu max_us=%llu avg_us=%llu last_err=0x%X(%s)",
             sensor_bus_probe_diag_phase_name(phase), (unsigned)address,
             (int)channel,
             (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_OK],
             (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_NACK],
             (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_TIMEOUT],
             (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_OTHER],
             (unsigned long)summary->counts[SENSOR_BUS_PROBE_RESULT_READBACK_MISMATCH],
             (unsigned long long)summary->min_us,
             (unsigned long long)summary->max_us,
             (unsigned long long)summary->avg_us,
             (unsigned)summary->last_err,
             esp_err_to_name((esp_err_t)summary->last_err));
}

static void emit_due_summaries(uint64_t time_us)
{
    for (size_t i = 0U; i < SENSOR_BUS_PROBE_DIAG_GROUP_LIMIT; ++i) {
        if (sensor_bus_probe_diag_policy_take_summary(
                &s_storage.policy, i, time_us, &s_storage.summary)) {
            emit_summary(&s_storage.summary);
        }
    }
}
#endif

esp_err_t sensor_bus_probe_diag_transaction(
    sensor_bus_probe_phase_t phase,
    uint8_t address,
    int channel,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len,
    bool expected_valid,
    uint8_t expected,
    sensor_bus_probe_transfer_fn transfer,
    void *context)
{
#if !CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    (void)phase;
    (void)address;
    (void)channel;
    (void)tx;
    (void)tx_len;
    (void)rx;
    (void)rx_len;
    (void)expected_valid;
    (void)expected;
    return transfer != NULL
        ? transfer(context, tx, tx_len, rx, rx_len)
        : ESP_ERR_INVALID_ARG;
#else
    esp_err_t err;

    if (transfer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_probe_address(address)) {
        return transfer(context, tx, tx_len, rx, rx_len);
    }

    sensor_bus_lines_read(&s_storage.sda_before, &s_storage.scl_before);
    s_storage.started_us = esp_timer_get_time();
    err = transfer(context, tx, tx_len, rx, rx_len);
    s_storage.finished_us = esp_timer_get_time();
    sensor_bus_lines_read(&s_storage.sda_after, &s_storage.scl_after);
    s_storage.transaction.phase = phase;
    s_storage.transaction.address = address;
    s_storage.transaction.channel = channel < 0 ? -1 : (int8_t)channel;
    s_storage.transaction.tx_len = (uint8_t)(tx_len > SENSOR_BUS_PROBE_DIAG_BYTES
        ? SENSOR_BUS_PROBE_DIAG_BYTES : tx_len);
    s_storage.transaction.rx_len = (uint8_t)(rx_len > SENSOR_BUS_PROBE_DIAG_BYTES
        ? SENSOR_BUS_PROBE_DIAG_BYTES : rx_len);
    s_storage.transaction.tx[0] = 0U;
    s_storage.transaction.tx[1] = 0U;
    s_storage.transaction.tx[2] = 0U;
    s_storage.transaction.tx[3] = 0U;
    s_storage.transaction.rx[0] = 0U;
    s_storage.transaction.rx[1] = 0U;
    s_storage.transaction.rx[2] = 0U;
    s_storage.transaction.rx[3] = 0U;
    for (size_t i = 0U; i < (size_t)s_storage.transaction.tx_len && tx != NULL; ++i) {
        s_storage.transaction.tx[i] = tx[i];
    }
    for (size_t i = 0U; i < (size_t)s_storage.transaction.rx_len && rx != NULL; ++i) {
        s_storage.transaction.rx[i] = rx[i];
    }
    s_storage.transaction.expected_valid = expected_valid;
    s_storage.transaction.expected = expected;
    s_storage.readback_mismatch = expected_valid && rx_len > 0U && rx != NULL &&
        err == ESP_OK && rx[0] != expected;
    s_storage.transaction.readback_mismatch = s_storage.readback_mismatch;
    s_storage.transaction.result = result_class(err, s_storage.readback_mismatch);
    s_storage.transaction.err = err;
    s_storage.transaction.elapsed_us = s_storage.finished_us >= s_storage.started_us
        ? (uint64_t)(s_storage.finished_us - s_storage.started_us) : 0U;
    s_storage.transaction.sda_before = s_storage.sda_before ? 1 : 0;
    s_storage.transaction.scl_before = s_storage.scl_before ? 1 : 0;
    s_storage.transaction.sda_after = s_storage.sda_after ? 1 : 0;
    s_storage.transaction.scl_after = s_storage.scl_after ? 1 : 0;

    s_storage.completed_us = s_storage.finished_us > 0
        ? (uint64_t)s_storage.finished_us : 0U;
    if (!sensor_bus_probe_diag_policy_record(
            &s_storage.policy, &s_storage.transaction, s_storage.completed_us,
            &s_storage.changed, &s_storage.should_print)) {
        emit_transaction(&s_storage.transaction);
    } else if (s_storage.should_print) {
        emit_transaction(&s_storage.transaction);
    }
    emit_due_summaries(s_storage.completed_us);
    return err;
#endif
}

void sensor_bus_probe_diag_begin_pass(uint32_t pass_index)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    sensor_bus_probe_diag_policy_begin_pass(
        &s_storage.policy, pass_index,
        CONFIG_APP_I2C_PROBE_DIAGNOSTICS_FULL_PASSES);
#else
    (void)pass_index;
#endif
}

void sensor_bus_probe_diag_end_pass(void)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    const uint32_t groups_dropped =
        sensor_bus_probe_diag_policy_end_pass(&s_storage.policy);
    if (groups_dropped > 0U) {
        ESP_LOGW(TAG, "I2C_PROBE_SUMMARY groups_dropped=%lu",
                 (unsigned long)groups_dropped);
    }
    emit_due_summaries(now_us());
#endif
}

void sensor_bus_probe_diag_log_bus_idle(void)
{
#if CONFIG_APP_I2C_PROBE_DIAGNOSTICS
    bool sda_high = false;
    bool scl_high = false;
    sensor_bus_lines_read(&sda_high, &scl_high);
    if (sensor_bus_probe_diag_policy_idle_changed(
            &s_storage.policy, sda_high ? 1 : 0, scl_high ? 1 : 0)) {
        ESP_LOGI(TAG, "I2C_BUS idle lvl=SDA%d/SCL%d",
                 sda_high ? 1 : 0, scl_high ? 1 : 0);
    }
#endif
}
