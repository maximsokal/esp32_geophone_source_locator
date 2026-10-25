# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

set(EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay")

# PlatformIO retains the historical Zephyr 4.2.1 compatibility smoke path.
# Its package does not provide the NCS 3.4.0 SoftDevice/RAS/Channel Sounding
# symbols used by the production prj.conf, so select the deliberately minimal
# compatibility fragment only for that build system. NCS production and its
# cs-proof continue to consume prj.conf unchanged.
if(DEFINED PIO_PACKAGES_DIR)
    set(CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/prj.platformio.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(geophone_nrf54l15_node)

target_sources(app PRIVATE
    ../src/main.c
    ../src/nrf_boot_diagnostics_console_zephyr.c
    ../src/nrf_node_state.c
    ../src/nrf_sensor_bus.c
    ../src/nrf_sensor_bus_zephyr.c
    ../src/nrf_bmi160.c
    ../src/nrf_bmi160_zephyr.c
    ../src/nrf_bme280.c
    ../src/nrf_bme280_zephyr.c
    ../src/nrf_baro_altitude.c
    ../src/nrf_sensor_snapshot.c
    ../src/nrf_ema_filter.c
    ../src/nrf_remote_sensor_runtime.c
    ../src/nrf_remote_sensor_runtime_zephyr.c
    ../src/nrf_orientation.c
    ../src/nrf_height_reference.c
    ../src/nrf_imu_calibration.c
    ../src/nrf_verticality_state.c
    ../src/nrf_blue_led.c
    ../src/nrf_blue_led_zephyr.c
    ../src/nrf_logger_led.c
    ../src/nrf_logger_led_zephyr.c
    ../src/nrf_stage_signal.c
    ../src/nrf_upstream_protocol.c
    ../src/nrf_log_protocol.c
    ../src/nrf_log_ring.c
    ../src/nrf_log_runtime_zephyr.c
    ../src/nrf_fault_record.c
    ../src/nrf_fault_record_zephyr.c
    ../src/nrf_thread_stack_inventory.c
    ../src/nrf_boot_failure_reason.c
    ../src/nrf_cs_event_overflow.c
    ../src/nrf_sensor_session.c
    ../src/nrf_sensor_upstream_runtime_zephyr.c
    ../src/nrf_bootstrap.c
    ../src/nrf_upstream_target_zephyr.c
    ../src/nrf_hci_workqueue.c
    ../src/nrf_bootstrap_runtime_zephyr.c
    ../src/nrf_cs_scheduler.c
    ../src/nrf_cs_pair_adapter.c
    ../src/nrf_cs_measurement_quality.c
    ../src/nrf_cs_pair_runtime_zephyr.c
    ../src/nrf_cs_control_protocol.c
    ../src/nrf_cs_control_runtime.c
    ../src/nrf_watchdog_health.c
    ../src/nrf_watchdog_zephyr.c
)

target_include_directories(app PRIVATE ../include)

# Production NCS uses NRFX 4.x; the historical PlatformIO smoke SDK has an older ABI.
if(NOT DEFINED PIO_PACKAGES_DIR)
    target_sources(app PRIVATE ../src/nrf_twis_trace_zephyr.c)
    target_link_options(app PUBLIC
        -Wl,--wrap=nrfx_twis_init
        -Wl,--wrap=nrfx_twis_tx_prepare
    )
endif()

# NCS production and cs-proof both use Nordic SoftDevice Controller. When that
# controller is selected, fail configuration if the permanent MAX-51
# RAS/Channel-Sounding runtime contract did not resolve as intended. The
# historical PlatformIO compatibility build does not select SoftDevice and
# therefore continues to compile the runtime fallback without becoming a
# production artifact.
if(CONFIG_BT_LL_SOFTDEVICE)
    if(NOT CONFIG_BT_CENTRAL OR NOT CONFIG_BT_PERIPHERAL OR
       NOT CONFIG_BT_CHANNEL_SOUNDING OR NOT CONFIG_BT_RAS OR
       NOT CONFIG_BT_RAS_RREQ OR NOT CONFIG_BT_RAS_RRSP OR
       NOT CONFIG_BT_CS_DE)
        message(FATAL_ERROR "MAX-51 requires Central+Peripheral+CS+RAS RREQ/RRSP+CS_DE in the NCS image")
    endif()
    if(NOT CONFIG_BT_MAX_CONN EQUAL 1 OR
       NOT CONFIG_BT_CTLR_SDC_CS_COUNT EQUAL 1 OR
       NOT CONFIG_BT_RAS_RREQ_MAX_ACTIVE_CONN EQUAL 1 OR
       NOT CONFIG_BT_RAS_RRSP_MAX_ACTIVE_CONN EQUAL 1)
        message(FATAL_ERROR "MAX-51 requires one serialized BLE/CS/RAS connection context")
    endif()
endif()
