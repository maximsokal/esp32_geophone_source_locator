#include "nrf_log_ring.h"
#include "nrf_log_events.h"

/* Reuse the simulated wire and the real service/driver fixture. */
#define main topology_baseline_tests
#include "../../components/nrf_topology_service/tests/nrf_topology_service_integration_test.c"
#undef main

static struct {
    nrf_log_ring_t ring;
    uint8_t response[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t response_length;
    bool selected;
    bool pending;
    bool firmware_supported;
    unsigned owner_release_count;
    unsigned block_reads;
    unsigned status_reads;
    uint8_t requested_records;
    size_t delivered;       /* bytes of the selected reply the master has read so far */
    bool window_armed;
    size_t window_offset;
    unsigned window_requests;
} s_logger[MAX_NODES];
static uint8_t s_expected_log_mask;
static uint8_t s_completed_log_mask;
static bool s_queue_calibration;
static bool s_calibration_queued;
static bool s_slow_log_output;
static bool s_fail_block_read;
static bool s_calibration_preempted_logs;
static bool s_ina_monitor_waiting;
static bool s_logger_pair_completed;
static unsigned s_logger_pair_owner_release_count;
static unsigned s_ina_accesses;
static int s_ina_task_token;
static int64_t s_ina_requested_us;
static int64_t s_ina_max_wait_us;
static unsigned s_ina_accesses_before_log_completion;
static unsigned s_firmware_lines;
static unsigned s_rx_lines;
static unsigned s_log_requests;
static unsigned s_log_requests_at_stale;
static bool s_corrupt_log_block;
static bool s_corrupt_log_status;
static bool s_corrupt_log_length;
static bool s_capture_rx_counters;
/* MAIN LOG_STATUS is 38 bytes: a 28 byte window, a READ_WINDOW write, a 10 byte tail. */
static unsigned s_fail_status_window_writes;
static unsigned s_fail_status_tail_reads;
static unsigned s_injected_status_faults;
static uint32_t s_rx_transport_ok;
static uint32_t s_rx_crc_ok;
static uint32_t s_rx_crc_fail;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

void heap_caps_free(void *memory) { free(memory); }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { (void)semaphore; }

static nrf_log_lock_key_t sim_ring_lock(void *context) { (void)context; return 0U; }
static uint32_t sim_next_boot(void *context) { (void)context; return 1000U; }
static void sim_ring_unlock(void *context, nrf_log_lock_key_t key)
{
    (void)context;
    assert(key == 0U);
}

static uint8_t log_slot(const sim_node_t *node)
{
    return node->channel == UPSTREAM_CHANNEL ? 0U : (uint8_t)(node->channel + 1U);
}

static void log_test_mux_write(void)
{
    for (size_t i = 0U; i < MAX_NODES; ++i) {
        assert(!s_logger[i].pending); /* No TCA change before a complete reply. */
    }
}

static void log_test_probe(sim_node_t *node)
{
    const size_t index = (size_t)(node - s_nodes);
    assert(!s_logger[index].pending);
    s_logger[index].selected = false;
}

static bool log_test_write(sim_node_t *node, const uint8_t *data, size_t length,
    esp_err_t *result)
{
    const size_t index = (size_t)(node - s_nodes);
    if (length == NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH && data[3] == NRF_UPSTREAM_MESSAGE_READ_WINDOW &&
        s_logger[index].selected) {
        nrf_upstream_read_window_t window;
        assert(nrf_upstream_protocol_decode_read_window(data, length, &window) == NRF_UPSTREAM_RESULT_OK);
        /* Each window continues where the previous read ended, inside the reply. */
        assert(window.offset == s_logger[index].delivered);
        assert(window.offset < s_logger[index].response_length);
        if (s_fail_status_window_writes > 0U && index == 0U &&
            s_logger[index].response[3] == NRF_LOG_MESSAGE_STATUS) {
            --s_fail_status_window_writes;
            ++s_injected_status_faults;
            s_logger[index].pending = false; /* the master abandons the half-read reply */
            *result = ESP_ERR_INVALID_STATE; /* IDF 5.5 NACK / timeout */
            return true;
        }
        s_logger[index].window_armed = true;
        s_logger[index].window_offset = window.offset;
        ++s_logger[index].window_requests;
        *result = ESP_OK;
        return true;
    }
    if (length < 4U || (data[3] != NRF_LOG_MESSAGE_STATUS_REQUEST &&
        data[3] != NRF_LOG_MESSAGE_READ_REQUEST && data[3] != NRF_LOG_MESSAGE_FIRMWARE_REQUEST)) {
        if (length >= 4U && (data[3] == NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE ||
            data[3] == NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE)) {
            if (s_completed_log_mask != s_expected_log_mask) {
                s_calibration_preempted_logs = true;
            }
        }
        s_logger[index].selected = false;
        return false;
    }
    ++s_log_requests;
    assert(!s_logger[index].pending);
    s_logger[index].owner_release_count = s_bus_owner_log_count;
    s_logger_pair_owner_release_count = s_bus_owner_log_count;
    if (s_vtask_delay_hook != NULL) {
        /* Priority-1 INA becomes runnable while the priority-2 logger owns the pair. */
        assert(!s_ina_monitor_waiting);
        s_ina_requested_us = esp_timer_get_time();
        s_simulated_task = &s_ina_task_token;
        assert(sensor_bus_owner_try_acquire() == SENSOR_BUS_ERR_MUTEX_BUSY);
        s_simulated_task = NULL;
        s_ina_monitor_waiting = true;
    }
    nrf_log_error_t error;
    if (data[3] == NRF_LOG_MESSAGE_STATUS_REQUEST) {
        nrf_log_status_request_t request;
        assert(nrf_log_protocol_decode_status_request(data, length, &request) == NRF_LOG_ERROR_OK);
        nrf_log_status_t status;
        assert(nrf_log_ring_status_snapshot(&s_logger[index].ring, &status) == NRF_LOG_ERROR_OK);
        status.request_id = request.request_id;
        if (s_logger[index].firmware_supported) status.flags |= NRF_LOG_STATUS_FLAG_FIRMWARE_INFO;
        error = nrf_log_protocol_encode_status(&status, s_logger[index].response);
        s_logger[index].response_length = NRF_LOG_PROTOCOL_STATUS_LENGTH;
        if (s_queue_calibration && !s_calibration_queued && node->channel == 0U) {
            uint8_t mask = 0U;
            uint16_t generation = 0U;
            assert(nrf_topology_service_request_height_reference_capture(&mask, &generation) == ESP_OK);
            assert(mask == (uint8_t)((1U << 0U) | (1U << 3U)));
            assert(nrf_topology_service_request_imu_calibration(generation, mask) == ESP_OK);
            s_calibration_queued = true;
        }
    } else if (data[3] == NRF_LOG_MESSAGE_FIRMWARE_REQUEST) {
        nrf_log_status_request_t request;
        assert(s_logger[index].firmware_supported);
        assert(nrf_log_protocol_decode_firmware_request(data, length, &request) == NRF_LOG_ERROR_OK);
        nrf_log_firmware_info_t info = {
            .request_id = request.request_id,
            .boot_id = s_logger[index].ring.boot_id,
            .flags = NRF_LOG_FIRMWARE_FLAG_DIRTY,
        };
        for (size_t i = 0U; i < sizeof(info.git_commit); ++i) {
            info.git_commit[i] = (uint8_t)(index * 16U + i);
        }
        error = nrf_log_protocol_encode_firmware(&info, s_logger[index].response);
        s_logger[index].response_length = NRF_LOG_PROTOCOL_FIRMWARE_LENGTH;
    } else {
        nrf_log_read_request_t request;
        assert(nrf_log_protocol_decode_read_request(data, length, &request) == NRF_LOG_ERROR_OK);
        s_logger[index].requested_records = request.max_records;
        error = nrf_log_ring_build_block_frame(&s_logger[index].ring, request.request_id,
            request.expected_boot_id, request.cursor_seq, request.max_records,
            s_logger[index].response, sizeof(s_logger[index].response), &s_logger[index].response_length);
    }
    assert(error == NRF_LOG_ERROR_OK);
    s_logger[index].selected = true;
    s_logger[index].pending = true;
    s_logger[index].delivered = 0U;
    s_logger[index].window_armed = false;
    *result = ESP_OK;
    return true;
}

static bool log_test_read(sim_node_t *node, uint8_t *data, size_t length, esp_err_t *result)
{
    const size_t index = (size_t)(node - s_nodes);
    if (!s_logger[index].selected) return false;
    if (s_logger[index].pending) {
        assert(s_bus_owner_log_count == s_logger[index].owner_release_count);
    }
    if (node->channel != UPSTREAM_CHANNEL) assert(s_tca_control == (uint8_t)(1U << node->channel));
    /*
     * No read is longer than one window: the first read of a reply is the
     * ordinary 28 byte read, every further window is announced by a READ_WINDOW
     * write and starts where the previous read ended. The master reads exactly
     * what is left of the reply, up to one window.
     */
    const size_t total = s_logger[index].response_length;
    size_t offset = 0U;
    if (s_logger[index].window_armed) {
        offset = s_logger[index].window_offset;
        s_logger[index].window_armed = false;
        assert(offset == s_logger[index].delivered);
    } else {
        s_logger[index].delivered = 0U; /* an ordinary read starts the reply again */
    }
    if (offset != 0U && s_fail_status_tail_reads > 0U && index == 0U &&
        s_logger[index].response[3] == NRF_LOG_MESSAGE_STATUS) {
        --s_fail_status_tail_reads;
        ++s_injected_status_faults;
        s_logger[index].pending = false;
        *result = ESP_ERR_INVALID_STATE;
        return true;
    }
    const size_t remaining = total - offset;
    assert(length == (remaining < NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES
                          ? remaining : NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES));
    s_logger[index].delivered = offset + length;
    if (s_logger[index].delivered >= total) {
        if (s_logger[index].response[3] == NRF_LOG_MESSAGE_STATUS) {
            assert(total == NRF_LOG_PROTOCOL_STATUS_LENGTH);
            ++s_logger[index].status_reads;
        } else if (s_logger[index].response[3] == NRF_LOG_MESSAGE_FIRMWARE) {
            assert(total == NRF_LOG_PROTOCOL_FIRMWARE_LENGTH);
        } else {
            assert(s_logger[index].response[3] == NRF_LOG_MESSAGE_BLOCK);
            assert(total == NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
                            (size_t)NRF_LOG_PROTOCOL_RECORD_LENGTH * s_logger[index].requested_records);
        }
        s_logger[index].pending = false;
        s_logger_pair_completed = true;
        if (s_logger[index].response[3] == NRF_LOG_MESSAGE_BLOCK) {
            ++s_logger[index].block_reads;
            s_completed_log_mask |= (uint8_t)(1U << log_slot(node));
            if (s_fail_block_read && node->channel == 0U) {
                s_fail_block_read = false;
                *result = ESP_ERR_TIMEOUT;
                return true;
            }
        }
    }
    memset(data, 0xFF, length);
    memcpy(data, &s_logger[index].response[offset], length);
    if (s_corrupt_log_block && s_logger[index].response[3] == NRF_LOG_MESSAGE_BLOCK &&
        offset == 0U && length > 10U) {
        data[10] ^= 1U;
        s_corrupt_log_block = false;
    }
    if (s_corrupt_log_status && s_logger[index].response[3] == NRF_LOG_MESSAGE_STATUS &&
        offset == 0U && length > 10U) {
        data[10] ^= 1U;
        s_corrupt_log_status = false;
    }
    if (s_corrupt_log_length && s_logger[index].response[3] == NRF_LOG_MESSAGE_BLOCK &&
        offset == 0U && length > 5U) {
        data[4] = 0xFFU;
        data[5] = 0U;
        s_corrupt_log_length = false;
    }
    *result = ESP_OK;
    return true;
}

static void log_test_ina_monitor(TickType_t ticks)
{
    if (!s_ina_monitor_waiting || !s_logger_pair_completed || ticks == 0U ||
        s_bus_owner_log_count == s_logger_pair_owner_release_count) {
        return;
    }
    sensor_bus_handles_t handles;
    uint8_t registers[2] = {0};
    assert(s_tca_control == 0U);
    s_simulated_task = &s_ina_task_token;
    assert(xTaskGetCurrentTaskHandle() != &s_task_token);
    assert(sensor_bus_owner_try_acquire() == ESP_OK);
    const int64_t waited_us = esp_timer_get_time() - s_ina_requested_us;
    if (waited_us > s_ina_max_wait_us) s_ina_max_wait_us = waited_us;
    assert(waited_us > 0 && waited_us <= 50000);
    assert(sensor_bus_get_handles(&handles) == ESP_OK);
    assert(sensor_bus_read_bytes(
        handles.ina3221, NULL_I2C_MEM_ADDR, sizeof(registers), registers) == ESP_OK);
    sensor_bus_owner_release();
    s_simulated_task = NULL;
    s_ina_monitor_waiting = false;
    s_logger_pair_completed = false;
    ++s_ina_accesses;
    if (s_completed_log_mask != s_expected_log_mask) ++s_ina_accesses_before_log_completion;
}

static void log_test_record(const char *line)
{
    if (strstr(line, "NRF_LOG_RX node=") != NULL || strstr(line, "NRF_FIRMWARE node=") != NULL) {
        log_test_mux_write();
        assert(s_tca_control == 0U);
        if (strstr(line, "NRF_FIRMWARE node=") != NULL) {
            ++s_firmware_lines;
            assert(strstr(line, "commit=") != NULL);
            assert(strstr(line, "dirty=1 supported=1") != NULL);
        } else {
            ++s_rx_lines;
            if (s_capture_rx_counters && strstr(line, "NRF_LOG_RX node=0 ") != NULL) {
                const char *transport = strstr(line, "transport_ok=");
                const char *crc_ok = strstr(line, "crc_ok=");
                const char *crc_fail = strstr(line, "crc_fail=");
                assert(transport != NULL && crc_ok != NULL && crc_fail != NULL);
                s_rx_transport_ok = (uint32_t)strtoul(transport + strlen("transport_ok="), NULL, 10);
                s_rx_crc_ok = (uint32_t)strtoul(crc_ok + strlen("crc_ok="), NULL, 10);
                s_rx_crc_fail = (uint32_t)strtoul(crc_fail + strlen("crc_fail="), NULL, 10);
            }
        }
        if (s_slow_log_output) s_fake_subtick_us += 100000U;
    }
}

static void start_log_world(bool with_mux)
{
    reset_world();
    memset(s_logger, 0, sizeof(s_logger));
    s_completed_log_mask = 0U;
    s_calibration_queued = false;
    s_calibration_preempted_logs = false;
    s_ina_monitor_waiting = false;
    s_logger_pair_completed = false;
    s_logger_pair_owner_release_count = 0U;
    s_ina_accesses = 0U;
    s_ina_max_wait_us = 0;
    s_ina_accesses_before_log_completion = 0U;
    s_firmware_lines = 0U;
    s_rx_lines = 0U;
    s_log_requests = 0U;
    s_corrupt_log_block = false;
    s_corrupt_log_status = false;
    s_corrupt_log_length = false;
    s_capture_rx_counters = false;
    s_fail_status_window_writes = 0U;
    s_fail_status_tail_reads = 0U;
    s_injected_status_faults = 0U;
    s_rx_transport_ok = 0U;
    s_rx_crc_ok = 0U;
    s_rx_crc_fail = 0U;
    s_tca_present = with_mux;
    node_boot(&s_nodes[0], UPSTREAM_CHANNEL, 0x11U);
    if (with_mux) {
        node_boot(&s_nodes[1], 0U, 0x22U);
        node_boot(&s_nodes[2], 3U, 0x33U);
    }
    s_expected_log_mask = with_mux ? (uint8_t)((1U << 0U) | (1U << 1U) | (1U << 4U)) : 1U;
    for (size_t index = 0U; index < (with_mux ? 3U : 1U); ++index) {
        const nrf_log_lock_interface_t lock = {
            .lock = sim_ring_lock, .unlock = sim_ring_unlock, .context = NULL,
        };
        assert(nrf_log_ring_init(&s_logger[index].ring, (uint32_t)index + 100U,
            0U, &lock, sim_next_boot, NULL) == NRF_LOG_ERROR_OK);
        s_logger[index].firmware_supported = true;
        s_nodes[index].snapshot_result = NRF_UPSTREAM_SENSOR_RESULT_OK;
        for (uint8_t record = 0U; record < 5U; ++record) {
            assert(nrf_log_ring_append(&s_logger[index].ring, NRF_LOG_SUBSYSTEM_SENSOR,
                NRF_LOG_SEVERITY_ERROR, NRF_LOG_EVENT_SENSOR_INIT_FAILED,
                record % 2U == 0U ? NRF_UPSTREAM_SENSOR_VALID_IMU : NRF_UPSTREAM_SENSOR_VALID_ENV,
                record % 2U == 0U ? (uint32_t)-5 : (uint32_t)-19, record) == NRF_LOG_ERROR_OK);
        }
    }
    s_fake_transaction_duration_us = 700U;
}

static void verify_log_pages(void)
{
    nrf_log_node_page_t page;
    assert(s_completed_log_mask == s_expected_log_mask);
    for (uint8_t slot = 0U; slot < NRF_LOG_COLLECTOR_NODE_COUNT; ++slot) {
        if ((s_expected_log_mask & (uint8_t)(1U << slot)) == 0U) continue;
        assert(nrf_topology_service_get_nrf_log_page(slot, false, 0U, 0U, 10U, &page) == ESP_OK);
        assert(page.count == 5U);
        assert(page.records[0].event_id == NRF_LOG_EVENT_SENSOR_INIT_FAILED);
        assert(page.records[0].arg0 == NRF_UPSTREAM_SENSOR_VALID_IMU && page.records[0].arg1 == (uint32_t)-5);
        assert(page.records[1].arg0 == NRF_UPSTREAM_SENSOR_VALID_ENV && page.records[1].arg1 == (uint32_t)-19);
    }
}

static void capture_stale_log_requests(void)
{
    capture();
    assert(s_seen.stale && !s_seen.bus_busy);
    assert(!s_fail_next_main_status_read);
    s_log_requests_at_stale = s_log_requests;
}

static void assert_stale_topology_blocks_log_requests(void)
{
    capture();
    assert(s_seen.stale);
    assert(s_log_requests == s_log_requests_at_stale);
}

/*
 * 2026-10-27 capture: MAIN answered the first 28 bytes of the 38 byte LOG_STATUS
 * and the rest of the exchange failed with ESP_ERR_INVALID_STATE. The collector
 * must start over with a fresh request, keep the topology, and still deliver
 * MAIN's firmware and log blocks. The 100 ms steps matter: the worker repeats a
 * yielded iteration at once, while 1 s steps would let the pending status expire.
 */
static void run_main_status_fault_scenario(unsigned window_write_faults, unsigned tail_read_faults)
{
    start_log_world(true);
    s_queue_calibration = false;
    s_slow_log_output = false;
    s_fail_status_window_writes = window_write_faults;
    s_fail_status_tail_reads = tail_read_faults;
    script(EV_WAIT_MS, 1000U, NULL);
    script_waits(16U, 10U);
    script_waits(200U, 100U);
    script(EV_ACTION, 0U, capture);
    script(EV_ACTION, 0U, verify_log_pages);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_injected_status_faults == window_write_faults + tail_read_faults);
    assert(s_fail_status_window_writes == 0U && s_fail_status_tail_reads == 0U);
    assert(s_logger[0].status_reads >= 1U && s_logger[0].block_reads >= 1U);
    assert(s_completed_log_mask == s_expected_log_mask);
    assert(assigned_node_count(&s_seen) == 3U && !s_seen.stale && !s_seen.bus_busy);
    assert(s_tca_control == 0U && s_collisions == 0U);
}

int main(void)
{
    run_main_status_fault_scenario(0U, 0U); /* control: same script without a fault */
    run_main_status_fault_scenario(1U, 0U);
    run_main_status_fault_scenario(0U, 1U);
    run_main_status_fault_scenario(1U, 1U);

    start_log_world(true);
    s_queue_calibration = true;
    s_slow_log_output = true;
    s_vtask_delay_hook = log_test_ina_monitor;
    script(EV_WAIT_MS, 1000U, NULL);
    script_waits(16U, 10U);
    script(EV_ACTION, 0U, verify_log_pages);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_calibration_queued && s_height_reference_writes == 2U && s_imu_calibration_writes == 2U);
    assert(s_calibration_preempted_logs);
    assert(s_firmware_lines == 3U && s_rx_lines >= 3U);
    assert(s_ina_accesses >= 3U && s_ina_accesses_before_log_completion >= 3U);
    assert(!s_ina_monitor_waiting && s_ina_max_wait_us > 0 && s_ina_max_wait_us <= 50000);
    assert(s_max_bus_owner_hold_ms <= SENSOR_BUS_OWNER_MAX_HOLD_MS);
    assert(s_collisions == 0U && s_tca_control == 0U);

    start_log_world(false);
    s_queue_calibration = false;
    script_waits(4U, 10U);
    script(EV_ACTION, 0U, verify_log_pages);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_firmware_lines == 1U && s_logger[0].block_reads == 1U);

    start_log_world(true);
    s_fail_block_read = true;
    script_waits(16U, 10U);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(!s_fail_block_read && s_logger[2].block_reads == 1U);
    assert(s_completed_log_mask == s_expected_log_mask);
    assert(s_tca_control == 0U && s_collisions == 0U);

    start_log_world(true);
    s_queue_calibration = false;
    s_slow_log_output = false;
    script_waits(16U, 10U);
    script(EV_ACTION, 0U, request_main_timeout_reconcile);
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, capture_stale_log_requests);
    script(EV_WAIT_MS, 1100U, NULL);
    script(EV_ACTION, 0U, assert_stale_topology_blocks_log_requests);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_tca_control == 0U && s_collisions == 0U);

    start_log_world(false);
    s_corrupt_log_block = true;
    s_capture_rx_counters = true;
    script_waits(20U, 10U);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(!s_corrupt_log_block);
    assert(s_rx_transport_ok >= 2U && s_rx_crc_ok >= 1U && s_rx_crc_fail == 1U);
    assert(s_rx_transport_ok == s_rx_crc_ok + s_rx_crc_fail);

    start_log_world(false);
    s_corrupt_log_length = true;
    s_capture_rx_counters = true;
    script_waits(20U, 10U);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(!s_corrupt_log_length);
    assert(s_rx_crc_ok >= 1U && s_rx_crc_fail == 0U);
    assert(s_rx_transport_ok > s_rx_crc_ok + s_rx_crc_fail);

    start_log_world(false);
    s_corrupt_log_status = true;
    s_capture_rx_counters = true;
    script_waits(20U, 10U);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(!s_corrupt_log_status);
    assert(s_rx_transport_ok >= 1U && s_rx_crc_fail == 1U);
    return 0;
}
