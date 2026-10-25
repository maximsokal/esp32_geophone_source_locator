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
} s_logger[MAX_NODES];
static uint8_t s_expected_log_mask;
static uint8_t s_completed_log_mask;
static bool s_queue_calibration;
static bool s_calibration_queued;
static bool s_slow_log_output;
static bool s_fail_block_read;
static unsigned s_firmware_lines;
static unsigned s_rx_lines;

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
    if (length < 4U || (data[3] != NRF_LOG_MESSAGE_STATUS_REQUEST &&
        data[3] != NRF_LOG_MESSAGE_READ_REQUEST && data[3] != NRF_LOG_MESSAGE_FIRMWARE_REQUEST)) {
        if (length >= 4U && (data[3] == NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE ||
            data[3] == NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE)) {
            assert(s_completed_log_mask == s_expected_log_mask);
        }
        s_logger[index].selected = false;
        return false;
    }
    assert(!s_logger[index].pending);
    s_logger[index].owner_release_count = s_bus_owner_log_count;
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
        error = nrf_log_ring_build_block_frame(&s_logger[index].ring, request.request_id,
            request.expected_boot_id, request.cursor_seq, request.max_records,
            s_logger[index].response, sizeof(s_logger[index].response), &s_logger[index].response_length);
    }
    assert(error == NRF_LOG_ERROR_OK);
    s_logger[index].selected = true;
    s_logger[index].pending = true;
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
    if (length >= s_logger[index].response_length) {
        s_logger[index].pending = false;
        if (s_logger[index].response[3] == NRF_LOG_MESSAGE_BLOCK) {
            assert(length == NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH);
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
    memcpy(data, s_logger[index].response,
        length < s_logger[index].response_length ? length : s_logger[index].response_length);
    *result = ESP_OK;
    return true;
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
    s_firmware_lines = 0U;
    s_rx_lines = 0U;
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

int main(void)
{
    start_log_world(true);
    s_queue_calibration = true;
    s_slow_log_output = true;
    script(EV_WAIT_MS, 10U, NULL);
    script(EV_ACTION, 0U, verify_log_pages);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_calibration_queued && s_height_reference_writes == 2U && s_imu_calibration_writes == 2U);
    assert(s_firmware_lines == 3U && s_rx_lines >= 9U);
    assert(s_max_bus_owner_hold_ms <= SENSOR_BUS_OWNER_MAX_HOLD_MS);
    assert(s_collisions == 0U && s_tca_control == 0U);

    start_log_world(false);
    s_queue_calibration = false;
    script(EV_ACTION, 0U, verify_log_pages);
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(s_firmware_lines == 1U && s_logger[0].block_reads == 1U);

    start_log_world(true);
    s_fail_block_read = true;
    script(EV_STOP, 0U, NULL);
    run_service();
    assert(!s_fail_block_read && s_logger[2].block_reads == 1U);
    assert(s_completed_log_mask == s_expected_log_mask);
    assert(s_tca_control == 0U && s_collisions == 0U);
    return 0;
}
