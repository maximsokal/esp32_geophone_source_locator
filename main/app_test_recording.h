#pragma once

/*
 * TEST ONLY (CONFIG_APP_TEST_RECORDING_SCENARIO, ESP-EMU): drive the real measurement_service START -> record -> STOP
 * path with the synthetic seismic source, a confirmed setup seeded into the nRF topology service and a flash FAT volume
 * in place of the SD card. Reports through TEST_RECORDING log markers.
 */
void app_test_recording_scenario_start(void);
