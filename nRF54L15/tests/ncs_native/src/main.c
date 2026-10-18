#include <errno.h>

#include <zephyr/ztest.h>
#include <zephyr/sys/printk.h>

#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_log_protocol.h"
#include "nrf_node_state.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_bus_zephyr.h"
#include "nrf_watchdog_zephyr.h"

uint32_t nrf_watchdog_zephyr_boot_reset_cause(void)
{
	return 0U;
}

ZTEST(nrf_log_runtime, test_runtime_builds_and_measures_a_maximum_block)
{
	nrf_log_status_t status;
	nrf_log_block_t block;
	nrf_log_read_request_t request = {
		.request_id = 1U,
		.cursor_seq = 0U,
		.max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
	};
	uint8_t status_frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
	uint8_t block_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
	size_t status_length = 0U;
	size_t block_length = 0U;
	uint32_t max_block_build_cycles;
#if defined(CONFIG_BOARD_NATIVE_SIM)
	uint32_t previous_max_block_build_cycles;
#endif

	zassert_equal(nrf_log_runtime_zephyr_init(), 0, "log runtime failed to initialize");
	zassert_equal(
		nrf_log_runtime_zephyr_build_status(
			request.request_id,
			status_frame,
			sizeof(status_frame),
			&status_length),
		NRF_LOG_ERROR_OK,
		"status build failed");
	zassert_equal(
		nrf_log_protocol_decode_status(status_frame, status_length, &status),
		NRF_LOG_ERROR_OK,
		"status decode failed");
	request.expected_boot_id = status.boot_id;
	zassert_not_equal(status.boot_id, 0U, "boot id must be nonzero");
	zassert_equal(
		nrf_log_runtime_zephyr_build_block(
			&request,
			block_frame,
			sizeof(block_frame),
			&block_length),
		NRF_LOG_ERROR_OK,
		"initial block build failed");
	zassert_equal(
		nrf_log_protocol_decode_block(
			block_frame,
			block_length,
			NULL,
			&block),
		NRF_LOG_ERROR_OK,
		"block decode failed");
	zassert_equal(block.records[0].event_id, NRF_LOG_EVENT_BOOT, "BOOT must be first");
	zassert_equal(block.record_count, 1U, "initial ring must only contain BOOT");

	for (uint32_t index = 0U; index < NRF_LOG_PROTOCOL_MAX_RECORDS; ++index) {
		nrf_log_runtime_zephyr_append_event(
			NRF_LOG_EVENT_LOG_TEST_EVENT,
			index,
			(uint32_t)k_uptime_get());
	}
#if defined(CONFIG_BOARD_NATIVE_SIM)
	previous_max_block_build_cycles = nrf_log_runtime_zephyr_max_block_build_cycles();
#endif
	for (uint32_t index = 0U; index < 32U; ++index) {
		request.request_id++;
		zassert_equal(
			nrf_log_runtime_zephyr_build_block(
				&request,
				block_frame,
				sizeof(block_frame),
				&block_length),
			NRF_LOG_ERROR_OK,
			"maximum block build failed");
#if defined(CONFIG_BOARD_NATIVE_SIM)
		max_block_build_cycles = nrf_log_runtime_zephyr_max_block_build_cycles();
		zassert_true(
			max_block_build_cycles >= previous_max_block_build_cycles,
			"maximum block cycle metric decreased from %u to %u",
			previous_max_block_build_cycles,
			max_block_build_cycles);
		previous_max_block_build_cycles = max_block_build_cycles;
#endif
	}
#if defined(CONFIG_BOARD_NATIVE_SIM)
	max_block_build_cycles = nrf_log_runtime_zephyr_max_block_build_cycles();
	zassert_true(
		max_block_build_cycles >= previous_max_block_build_cycles,
		"maximum block cycle metric decreased after repeated builds");
#else
	max_block_build_cycles = nrf_log_runtime_zephyr_max_block_build_cycles();
	zassert_not_equal(
		max_block_build_cycles,
		0U,
		"block cycle metric was not captured");
#endif
	printk("nrf_log_block_build_max_cycles=%u (native_sim host estimate)\n",
	       max_block_build_cycles);
}

ZTEST_SUITE(nrf_log_runtime, NULL, NULL, NULL, NULL, NULL);

ZTEST(nrf_zephyr_bridges, test_private_bus_uses_real_zephyr_i2c_api)
{
	nrf_sensor_bus_t bus;
	uint8_t register_address = 0xD0U;
	uint8_t chip_id = 0U;

	zassert_true(nrf_sensor_bus_zephyr_init_private(&bus), "i2c30 fake device was not ready");
	zassert_not_null(bus.transfer, "bridge did not publish transfer callback");
	zassert_equal(
		bus.transfer(bus.context, 0x76U, NULL, 0U, NULL, 0U),
		-EINVAL,
		"empty transfer must be rejected");
	zassert_equal(
		bus.transfer(bus.context, 0x76U, &register_address, 1U, &chip_id, 1U),
		0,
		"write/read transfer must reach Zephyr I2C API");
	zassert_equal(chip_id, 0x60U, "fake BME280 register response was not read");
}

ZTEST(nrf_zephyr_bridges, test_sensor_bridges_reject_null_devices)
{
	zassert_equal(
		nrf_bmi160_zephyr_init_remote(NULL),
		NRF_BMI160_ERR_ARGUMENT,
		"BMI160 bridge accepted NULL");
	zassert_equal(
		nrf_bme280_zephyr_init_remote(NULL),
		NRF_BME280_ERR_ARGUMENT,
		"BME280 bridge accepted NULL");
}

ZTEST(nrf_zephyr_bridges, test_remote_runtime_skips_sensor_io_for_main_role)
{
	nrf_node_state_t node_state;
	nrf_remote_sensor_runtime_zephyr_t runtime;

	nrf_node_state_init(&node_state);
	zassert_equal(nrf_node_state_mark_ready(&node_state), NRF_NODE_STATE_OK, NULL);
	zassert_equal(
		nrf_node_state_assign_role(&node_state, NRF_NODE_ROLE_MAIN),
		NRF_NODE_STATE_OK,
		NULL);
	nrf_remote_sensor_runtime_zephyr_init(&runtime);

	zassert_equal(
		nrf_remote_sensor_runtime_zephyr_prepare(&node_state, &runtime),
		NRF_REMOTE_SENSOR_PREPARE_SKIPPED,
		"MAIN must not initialize remote-only sensors");
	zassert_false(runtime.core.imu_ready, "MAIN initialized BMI160");
	zassert_false(runtime.core.env_ready, "MAIN initialized BME280");
}

ZTEST(nrf_zephyr_bridges, test_remote_runtime_uses_both_zephyr_sensor_bridges)
{
	nrf_node_state_t node_state;
	nrf_remote_sensor_runtime_zephyr_t runtime;

	nrf_node_state_init(&node_state);
	zassert_equal(nrf_node_state_mark_ready(&node_state), NRF_NODE_STATE_OK, NULL);
	zassert_equal(
		nrf_node_state_assign_role(&node_state, NRF_NODE_ROLE_REMOTE),
		NRF_NODE_STATE_OK,
		NULL);
	nrf_remote_sensor_runtime_zephyr_init(&runtime);

	zassert_equal(
		nrf_remote_sensor_runtime_zephyr_prepare(&node_state, &runtime),
		NRF_REMOTE_SENSOR_PREPARE_OK,
		"REMOTE sensor bridges did not initialize against the fake I2C device");
	zassert_true(runtime.core.imu_ready, "BMI160 bridge was not connected");
	zassert_true(runtime.core.env_ready, "BME280 bridge was not connected");
}

ZTEST_SUITE(nrf_zephyr_bridges, NULL, NULL, NULL, NULL, NULL);

ZTEST(nrf_cs_pair_runtime, test_runtime_rejects_invalid_public_inputs)
{
	nrf_cs_step_t step = {0};
	int expected_execution_error;

#if defined(CONFIG_BT_RAS) && defined(CONFIG_BT_RAS_RREQ) && \
	defined(CONFIG_BT_RAS_RRSP) && defined(CONFIG_BT_CS_DE)
	expected_execution_error = -EINVAL;
#else
	expected_execution_error = -ENOTSUP;
#endif

	zassert_equal(
		nrf_cs_pair_runtime_zephyr_set_node_id(0xFFU),
		-EINVAL,
		"broadcast node id must never be accepted");
	zassert_equal(
		nrf_cs_pair_runtime_zephyr_execute(&step, NULL, NULL),
		expected_execution_error,
		"runtime must reject an incomplete execution request");
}

ZTEST(nrf_cs_pair_runtime, test_runtime_start_contract)
{
	int result = nrf_cs_pair_runtime_zephyr_start();

#if defined(CONFIG_BT_RAS) && defined(CONFIG_BT_RAS_RREQ) && \
	defined(CONFIG_BT_RAS_RRSP) && defined(CONFIG_BT_CS_DE)
	zassert_true(result == 0 || result == -EALREADY, "Bluetooth runtime failed to start");
#else
	zassert_equal(result, 0, "fallback runtime start must be a no-op success");
#endif
	zassert_false(nrf_cs_pair_runtime_zephyr_is_busy(), "invalid input made runtime busy");
}

ZTEST_SUITE(nrf_cs_pair_runtime, NULL, NULL, NULL, NULL, NULL);
