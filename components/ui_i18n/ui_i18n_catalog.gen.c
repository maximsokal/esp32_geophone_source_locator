/* generated — do not edit */
#include "ui_i18n_catalog.gen.h"

static const char *const s_ui_i18n_en[UI_I18N_KEY_COUNT] = {
    "GeoLocator 3D", /* app.title */
    "CHECK CARD, CS AND WIRING", /* boot.check_wiring */
    "NO STORAGE - CONTINUING", /* boot.continue_no_storage */
    "PRESS POWER TO SHUT DOWN", /* boot.error_footer */
    "BOOT ERROR", /* boot.error_title */
    "ESP32-S3  |  INITIALIZING", /* boot.footer */
    "FORMAT COMPLETE", /* boot.format_complete */
    "DO NOT POWER OFF", /* boot.format_do_not_power_off */
    "FORMAT FAILED", /* boot.format_failed */
    "POWER OFF: INSERT CARD", /* boot.format_power_hint */
    "FORMATTING CARD,", /* boot.format_progress */
    "OK: RETRY FORMAT", /* boot.format_retry_hint */
    "MOUNT FAILED", /* boot.mount_failed */
    "CARD NOT RESPONDING", /* boot.reason_card_unavailable */
    "FILESYSTEM NOT READABLE", /* boot.reason_fs_mount */
    "SD INTERFACE FAILED", /* boot.reason_host_init */
    "STORAGE LAYOUT INIT FAILED", /* boot.reason_layout_init */
    "CARD READ/WRITE TEST FAILED", /* boot.reason_write_probe */
    "LEFT/UP/DOWN: NO SD", /* boot.recovery_cancel_hint */
    "CARD READY - CONTINUING", /* boot.recovery_card_ready */
    "OK: FORMAT", /* boot.recovery_format_hint */
    "INSERT CARD", /* boot.recovery_insert_card */
    "RIGHT: RETRY", /* boot.recovery_retry_hint */
    "SD RECOVERY", /* boot.recovery_title */
    "ERASES ALL CARD DATA!", /* boot.recovery_warning */
    "SD SPI CEILING 20 MHz", /* boot.sd_limit */
    "ACTIVE OBJECT...", /* boot.stage_active_object */
    "NETWORK + UI...", /* boot.stage_network */
    "OBJECT STORE...", /* boot.stage_object_store */
    "STARTING...", /* boot.stage_starting */
    "STORAGE...", /* boot.stage_storage */
    "SYSTEM STARTUP", /* boot.startup */
    "SD CARD / STORAGE FAILURE", /* boot.storage_failure */
    "GEOPHONE LOCATOR", /* boot.title */
    "WRITE TEST FAILED", /* boot.write_test_failed */
    "ADC --", /* header.adc_idle */
    "NET --", /* header.network_offline */
    "NET OK", /* header.network_ready */
    "SETUP", /* header.network_setup */
    "--", /* header.power_empty */
    "%s%s%s", /* header.power_format */
    "%s %s", /* header.source_format */
    "CHECKING STORAGE...", /* home.checking_storage */
    "UP/DOWN NAV   OK/RIGHT OPEN", /* home.navigation */
    "STORAGE CHECK INCOMPLETE", /* home.storage_incomplete */
    "MAIN", /* home.title */
    "SAVE   > DELETE   3D", /* measurement.action_delete */
    "SAVE    DELETE   >3D", /* measurement.action_preview */
    "> SAVE   DELETE   3D", /* measurement.action_save */
    "CH%u DATA? DIGITAL", /* measurement.channel_digital */
    "CH%u  --", /* measurement.channel_empty */
    "FLAT", /* measurement.channel_flat */
    "CH%u INVALID %s", /* measurement.channel_invalid */
    "CH%u SERIAL? %s", /* measurement.channel_serial */
    "CH%u%+6.2fmV %s", /* measurement.channel_value */
    "VAR", /* measurement.channel_variable */
    "DELETE FAILED", /* measurement.delete_failed */
    "DELETING SESSION...", /* measurement.deleting_session */
    "ADC DATA? DIGITAL  %s", /* measurement.digital_fault */
    "ERROR %s   OK RETRY", /* measurement.error_retry */
    "UP/DN SCROLL/SEL  OK ACT  LEFT BACK", /* measurement.footer */
    "TFT GRAPHS OFF  ADC+SD ON", /* measurement.graphs_off */
    "TFT GRAPHS OFF  %s", /* measurement.graphs_status */
    "OK START", /* measurement.health_start */
    "%s  %luHz  CH1-CH4", /* measurement.idle_summary */
    "FAILED TO KEEP SESSION", /* measurement.keep_failed */
    "FAILED TO LEAVE MEASUREMENT", /* measurement.leave_failed */
    "LEAVING WHEN OPERATION FINISHES...", /* measurement.leaving_after_operation */
    "connected", /* measurement.link_connected */
    "LINK", /* measurement.link_legend */
    "measuring", /* measurement.link_measuring */
    "offline", /* measurement.link_not_connected */
    "ready", /* measurement.link_ready */
    "LISTEN ONLY BEFORE RECORDING", /* measurement.listen_only */
    "NO SAVED SESSION TO PREVIEW", /* measurement.no_session_preview */
    "G1", /* measurement.node_g1 */
    "G2", /* measurement.node_g2 */
    "G3", /* measurement.node_g3 */
    "G4", /* measurement.node_g4 */
    "LEVEL", /* measurement.node_level */
    "MAIN", /* measurement.node_main */
    "MOVE", /* measurement.node_moving */
    "N/D", /* measurement.node_no_data */
    "SOL", /* measurement.node_solved */
    "TILT", /* measurement.node_tilted */
    "UNS", /* measurement.node_unsolved */
    "OBJECT %s  [%s]", /* measurement.object_format */
    "NO ACTIVE OBJECT", /* measurement.object_none */
    "RESOLVING OBJECT...", /* measurement.object_resolving */
    "REC %s  %lus  %luHz", /* measurement.recording */
    "OK STOP   S:%llu   DROP:%llu", /* measurement.recording_status */
    "%s S:%llu D:%llu", /* measurement.review_counts */
    "SAVE FAILED", /* measurement.save_failed */
    "SELECT ACTIVE OBJECT FIRST", /* measurement.select_object */
    "ADC SERIAL?  %s", /* measurement.serial_fault */
    "SESSION SAVED", /* measurement.session_saved */
    "START FAILED", /* measurement.start_failed */
    "STARTING RECORDING...", /* measurement.starting_recording */
    "STOPPED", /* measurement.state_complete */
    "DELETING", /* measurement.state_deleting */
    "ERROR", /* measurement.state_error */
    "LISTEN", /* measurement.state_listen */
    "LIVE", /* measurement.state_live */
    "REC", /* measurement.state_recording */
    "STARTING", /* measurement.state_starting */
    "STOPPING", /* measurement.state_stopping */
    "STOP FAILED", /* measurement.stop_failed */
    "STOPPED %s  %lus", /* measurement.stopped */
    "STOPPING AND LEAVING...", /* measurement.stopping_leaving */
    "STOPPING RECORDING...", /* measurement.stopping_recording */
    "LIVE --", /* measurement.summary_live */
    "OBJECT --", /* measurement.summary_object */
    "SIGNALS", /* measurement.title */
    "ADC INVALID TRANSPORT  %s", /* measurement.transport_fault */
    "UNKNOWN", /* measurement.unknown */
    "PLEASE WAIT...", /* measurement.wait */
    "PLEASE WAIT - LEAVING...", /* measurement.wait_leaving */
    "MEASUREMENTS / AD7606", /* menu.measurements */
    "OBJECTS", /* menu.objects */
    "STORAGE / SD CARD", /* menu.storage */
    "ACTIVE  %s  %s", /* objects.active */
    "ACTIVE %s", /* objects.active_done */
    "[ACTIVE]", /* objects.active_marker */
    "ACTIVE  NONE", /* objects.active_none */
    "DELETE OBJECT?", /* objects.confirm_delete */
    "OK/RIGHT CONFIRM   LEFT CANCEL", /* objects.confirm_footer */
    "CREATED %s", /* objects.created */
    "CREATING OBJECT...", /* objects.creating */
    "DELETE OBJECT", /* objects.delete */
    "Scans/files: %u", /* objects.delete_scope.artifacts */
    "Localization runs: %u", /* objects.delete_scope.runs */
    "Sessions: %u", /* objects.delete_scope.sessions */
    "Delete counts unavailable", /* objects.delete_scope.unavailable */
    "DELETE FAILED TO START", /* objects.delete_start_failed */
    "DELETED %s", /* objects.deleted */
    "DELETING...", /* objects.deleting */
    "%s  %s%s", /* objects.list_entry */
    "OBJECT LIST FAILED", /* objects.list_failed */
    "LOADING OBJECTS...", /* objects.loading */
    "%s  %s", /* objects.name_pair */
    "NEW OBJECT", /* objects.new */
    "NO OBJECTS", /* objects.none */
    "OBJECT OPERATION FAILED", /* objects.operation_failed */
    "SELECT OBJECT TO ACTIVATE", /* objects.select_activate */
    "SELECT OBJECT TO DELETE", /* objects.select_delete */
    "OBJECT SERVICE BUSY/FAILED", /* objects.service_busy */
    "SET ACTIVE OBJECT", /* objects.set_active */
    "SET ACTIVE FAILED TO START", /* objects.set_active_start_failed */
    "SETTING ACTIVE...", /* objects.setting_active */
    "USB CONNECTED - SHUTDOWN CANCELLED", /* power.alert_cancelled */
    "CRITICAL BATTERY - USB OR STOP", /* power.alert_critical */
    "LOW BATTERY - CONNECT USB SOON", /* power.alert_low */
    "CRITICAL BATTERY - STOPPING", /* power.alert_shutdown */
    "CRITICAL - USB %us OR STOP", /* power.banner_grace */
    "CRITICAL BATTERY - STOPPING", /* power.banner_stop */
    "BAT", /* power.battery */
    "CHG", /* power.charging */
    "CRIT", /* power.critical */
    "FULL", /* power.full */
    "USB %us", /* power.grace */
    "OFF", /* power.off */
    "%d%%", /* power.percent */
    "USB", /* power.usb */
    "LOW", /* power_badge.low */
    "LEFT BACK RIGHT YAW UP/DN PITCH OK VIEW", /* scan.footer */
    "LOADING...", /* scan.loading */
    "Method estimates, one source", /* scan.method_estimates */
    "NO 3D SCAN", /* scan.no_3d_scan */
    "NO PREVIEW", /* scan.no_preview */
    "OCCUPANCY %u  %s", /* scan.occupancy */
    "canvas memory unavailable", /* scan.reason_canvas_memory */
    "invalid preview", /* scan.reason_invalid_preview */
    "invalid session id", /* scan.reason_invalid_session */
    "loader unavailable", /* scan.reason_loader */
    "no preview or occupancy", /* scan.reason_no_data */
    "no valid preview or occupancy", /* scan.reason_no_valid_data */
    "preview memory", /* scan.reason_preview_memory */
    "preview missing", /* scan.reason_preview_missing */
    "preview unavailable", /* scan.reason_preview_unavailable */
    "preview unreadable", /* scan.reason_preview_unreadable */
    "recording active", /* scan.reason_recording_active */
    "scan missing", /* scan.reason_scan_missing */
    "scan unreadable", /* scan.reason_scan_unreadable */
    "screen allocation failed", /* scan.reason_screen_memory */
    "stale scan CRC", /* scan.reason_stale_crc */
    "SENSOR FRAME, NO SCAN", /* scan.sensor_estimates_frame */
    "3D PREVIEW", /* scan.title */
    "VOXELS %u  COARSEN %u%s%s", /* scan.voxels */
    "CHECKING STORAGE...", /* shared.checking_storage */
    "--", /* shared.empty */
    "UP/DOWN NAV   OK SELECT   LEFT BACK", /* shared.footer_back */
    "UP/DOWN NAV   OK SELECT   LEFT CANCEL", /* shared.footer_cancel */
    "MEASUREMENT OPERATION IN PROGRESS", /* shared.measure_operation_busy */
    "MEASUREMENT SERVICE UNAVAILABLE", /* shared.measure_service_unavailable */
    "CONFIRM ERASE DATA", /* storage.confirm_erase */
    "PRESS OK AGAIN TO ERASE DATA", /* storage.confirm_prompt */
    "DATA ERASE COMPLETE - REFRESH INFO", /* storage.erase_complete */
    "ERASE DEVICE DATA", /* storage.erase_device */
    "DATA ERASE FAILED", /* storage.erase_failed */
    "ERASING DATA...", /* storage.erasing */
    "SD INFO FAILED", /* storage.info_failed */
    "SD INFORMATION NOT LOADED", /* storage.not_loaded */
    "RECOVERY COMPLETE - REOPEN OBJECTS", /* storage.recovery_complete */
    "RECOVERY FAILED - REOPEN OBJECTS", /* storage.recovery_failed */
    "REFRESH SD INFO", /* storage.refresh */
    "REFRESHING SD INFO...", /* storage.refreshing */
    "SD RUNTIME CEILING 20 MHz", /* storage.runtime_limit */
    "STORAGE SERVICE BUSY/FAILED", /* storage.service_busy */
    "FREE %.2f GB   SPI %.2f MHz", /* storage.space */
    "%s  %s  %.2f GB", /* storage.summary */
    "STORAGE", /* storage.title */
    "CORE0 SERVICES/LVGL   CORE1 ACQUISITION", /* system.cores */
    "FW      %s", /* system.firmware */
    "HEAP     %lu KB internal free", /* system.heap */
    "HEAP     -- KB internal free", /* system.heap_empty */
    "ESP-IDF  %s", /* system.idf_version */
    "LANGUAGE: ENGLISH", /* system.language_english */
    "UP/DN SELECT  OK SET", /* system.language_footer */
    "LANGUAGE: UKRAINIAN", /* system.language_ukrainian */
    "LVGL     %d.%d.%d + esp_lvgl_port", /* system.lvgl_version */
    "PSRAM    %lu KB free", /* system.psram */
    "PSRAM    -- KB free", /* system.psram_empty */
    "SYSTEM", /* system.title */
    "UPTIME   %lld s", /* system.uptime */
    "UPTIME   -- s", /* system.uptime_empty */
    "Required module issue", /* tft.hw.degraded_banner */
    "AD7606", /* tft.hw.module.ad7606 */
    "Barometer", /* tft.hw.module.barometer */
    "Main XIAO", /* tft.hw.module.main_xiao */
    "Power monitor", /* tft.hw.module.power_monitor */
    "SD card", /* tft.hw.module.sd_card */
    "Sensor mux", /* tft.hw.module.sensor_mux */
    "OPT", /* tft.hw.optional */
    "Remote sensors: {count}", /* tft.hw.remotes */
    "REQ", /* tft.hw.required */
    "Absent", /* tft.hw.state.absent */
    "Degraded", /* tft.hw.state.degraded */
    "Fault", /* tft.hw.state.fault */
    "Present", /* tft.hw.state.present */
    "NOT TESTED", /* tft.hw.state.unknown */
    "HARDWARE", /* tft.hw.title */
    "Back", /* tft.workflow.action.back */
    "Wi-Fi OFF, calculate", /* tft.workflow.action.calculate_positions */
    "Wi-Fi ON, calculate", /* tft.workflow.action.calculate_positions_keep_wifi */
    "Calculate source", /* tft.workflow.action.calculate_sources */
    "Confirm", /* tft.workflow.action.confirm */
    "Delete recording", /* tft.workflow.action.delete_recording */
    "Go to calibration", /* tft.workflow.action.go_calibration */
    "Live signals", /* tft.workflow.action.live_signals */
    "Move sensors", /* tft.workflow.action.move_sensors */
    "Place sensors", /* tft.workflow.action.place_sensors */
    "Sensors placed", /* tft.workflow.action.placement_confirmed */
    "Record again", /* tft.workflow.action.record_again */
    "Retry", /* tft.workflow.action.retry */
    "Change rate", /* tft.workflow.action.sample_rate */
    "Sensors placed", /* tft.workflow.action.sensors_placed */
    "Skip", /* tft.workflow.action.skip */
    "Start measurement", /* tft.workflow.action.start_measurement */
    "Start recording", /* tft.workflow.action.start_recording */
    "Stop recording", /* tft.workflow.action.stop_recording */
    "Progress %s%%", /* tft.workflow.calculating.progress */
    "Next: place the geophones", /* tft.workflow.calib_result.next */
    "Phase %s", /* tft.workflow.calibration.header */
    "Place all sensors next to the main housing", /* tft.workflow.calibration.prep */
    "%s: %s", /* tft.workflow.calibration.reason */
    "%s", /* tft.workflow.calibration.reason_single */
    "%s H:%s z:%scm I:%s b:%s", /* tft.workflow.calibration.row */
    "%s H:%s z:%scm", /* tft.workflow.calibration.row_main */
    "Calibration: %s", /* tft.workflow.calibration.status */
    "Delete recording? OK = yes, LEFT = no", /* tft.workflow.confirm.delete */
    "Choose the pair to correct", /* tft.workflow.distance.choose */
    "UP/DOWN step: %s m", /* tft.workflow.distance.digit */
    "Enter distances", /* tft.workflow.distance.edit */
    "Edit this pair", /* tft.workflow.distance.enter */
    "%s-%s: %s cm (manual)", /* tft.workflow.distance.manual */
    "Pair %s-%s", /* tft.workflow.distance.pair */
    "UP/DOWN pair OK edit LEFT cancel", /* tft.workflow.distance.pair_hint */
    "Rejected: check distance and cable", /* tft.workflow.distance.rejected */
    "Save and recalculate", /* tft.workflow.distance.save */
    "Saved. Next pair, or LEFT to finish", /* tft.workflow.distance.saved */
    "Saving and recalculating...", /* tft.workflow.distance.saving */
    "Distance: %s m", /* tft.workflow.distance.value */
    "UP/DOWN digit L/R move OK save", /* tft.workflow.distance.value_hint */
    "Waiting for phone GPS", /* tft.workflow.gps.waiting */
    "UP/DOWN select OK run LEFT back RIGHT 3D", /* tft.workflow.hint.3d */
    "UP/DOWN actions  OK run  LEFT back", /* tft.workflow.hint.navigation */
    "UP/DOWN action OK select RIGHT more", /* tft.workflow.hint.ranging_pages */
    "Select an object", /* tft.workflow.idle.select_object */
    "Under 3 geophones: no localization", /* tft.workflow.insufficient_geophones */
    "Layout ready to calculate", /* tft.workflow.layout.ready */
    "Wi-Fi off = better ranging", /* tft.workflow.layout.wifi_hint */
    "Phone link returns itself", /* tft.workflow.layout.wifi_phone */
    "Set uncertainty (m/s)", /* tft.workflow.material.custom_sigma */
    "+/- %s m/s", /* tft.workflow.material.custom_sigma_value */
    "Set speed (m/s)", /* tft.workflow.material.custom_speed */
    "v: %s m/s", /* tft.workflow.material.custom_speed_value */
    "UP/DOWN adjust OK next LEFT back", /* tft.workflow.material.hint_adjust */
    "UP/DOWN adjust OK save LEFT back", /* tft.workflow.material.hint_save */
    "UP/DOWN choose OK select LEFT back", /* tft.workflow.material.hint_select */
    "Brick masonry", /* tft.workflow.material.name.brick_masonry */
    "Concrete (monolithic)", /* tft.workflow.material.name.concrete */
    "Custom value", /* tft.workflow.material.name.custom */
    "Reinforced concrete", /* tft.workflow.material.name.reinforced_concrete */
    "Construction debris / rubble", /* tft.workflow.material.name.rubble */
    "Dry soil / backfill", /* tft.workflow.material.name.soil_dry */
    "Saturated soil", /* tft.workflow.material.name.soil_saturated */
    "Steel structures", /* tft.workflow.material.name.steel */
    "Unknown material", /* tft.workflow.material.name.unknown */
    "%s", /* tft.workflow.material.option */
    "auto", /* tft.workflow.material.range_auto */
    "Range: %s-%s m/s", /* tft.workflow.material.range_current */
    "UP set OK save LEFT back", /* tft.workflow.material.range_hint_auto */
    "Max velocity (m/s)", /* tft.workflow.material.range_max */
    "Min velocity (m/s)", /* tft.workflow.material.range_min */
    "%s m/s", /* tft.workflow.material.range_value */
    "Material: %s", /* tft.workflow.material.row */
    "Save failed", /* tft.workflow.material.save_failed */
    "Saved", /* tft.workflow.material.saved */
    "Saving...", /* tft.workflow.material.saving */
    "%s +/- %s m/s", /* tft.workflow.material.speed */
    "MATERIAL", /* tft.workflow.material.title */
    "Material: not set", /* tft.workflow.material.unset */
    "Not enough memory. Try again.", /* tft.workflow.memory_unavailable */
    "Busy", /* tft.workflow.notice.busy */
    "Not stable yet", /* tft.workflow.notice.placement_blocked */
    "Stop live signals first", /* tft.workflow.notice.sample_rate_busy */
    "State changed", /* tft.workflow.notice.state_changed */
    "%s", /* tft.workflow.object.name */
    "Select object for measurement", /* tft.workflow.object.select */
    "cancelled", /* tft.workflow.phase.cancelled */
    "complete", /* tft.workflow.phase.complete */
    "failed", /* tft.workflow.phase.failed */
    "idle", /* tft.workflow.phase.idle */
    "running", /* tft.workflow.phase.running */
    "All stable", /* tft.workflow.placement.all_stable */
    "%s: %s%s", /* tft.workflow.placement.blocked */
    "Clock went back", /* tft.workflow.placement.reason.clock_backward */
    "Collecting", /* tft.workflow.placement.reason.collecting */
    "No MAIN status", /* tft.workflow.placement.reason.device_status_busy */
    "MAIN baro n/a", /* tft.workflow.placement.reason.main_barometer */
    "No MAIN ref", /* tft.workflow.placement.reason.main_reference */
    "Node baro n/a", /* tft.workflow.placement.reason.node_environment */
    "Ref mismatch", /* tft.workflow.placement.reason.node_reference */
    "No fresh data", /* tft.workflow.placement.reason.node_stale */
    "Not upright", /* tft.workflow.placement.reason.not_vertical */
    "Buffer full", /* tft.workflow.placement.reason.overflow_blocked */
    "Not settled yet", /* tft.workflow.placement.reason.range_exceeded */
    "Sample too old", /* tft.workflow.placement.reason.sample_stale */
    "Stable", /* tft.workflow.placement.reason.stable */
    "%s V:%s dH:%scm ASL:%sm St:%s", /* tft.workflow.placement.row */
    "Waiting for stability", /* tft.workflow.placement.waiting */
    "OK again to stop recording", /* tft.workflow.preview.stop_confirm */
    "State changed", /* tft.workflow.preview.stop_error */
    "OK twice: stop recording", /* tft.workflow.preview.stop_hint */
    "Stopping recording...", /* tft.workflow.preview.stop_pending */
    "%s-%s: %s cm > cable", /* tft.workflow.ranging.cable_exceeded */
    "longer than the cable", /* tft.workflow.ranging.cause.cable_exceeded */
    "the two ways disagree", /* tft.workflow.ranging.cause.directions_disagree */
    "a node disconnected", /* tft.workflow.ranging.cause.disconnected */
    "one way: too few results", /* tft.workflow.ranging.cause.few_samples */
    "one way: low confidence", /* tft.workflow.ranging.cause.low_confidence */
    "one way: no result", /* tft.workflow.ranging.cause.no_measurement */
    "%s-%s: %s cm", /* tft.workflow.ranging.distance */
    "Measurement failed", /* tft.workflow.ranging.failed */
    "%s-%s: -- (%s / %s cm)", /* tft.workflow.ranging.pair_diag */
    "Measurements %s/%s residual %s cm", /* tft.workflow.ranging.pairs */
    "Partially measured", /* tft.workflow.ranging.partial */
    "%s sigma:%s cm", /* tft.workflow.ranging.sigma */
    "Distances measured", /* tft.workflow.ranging.success */
    "Ranging by radio, keep still", /* tft.workflow.ranging.wait */
    "Ranging %s%%, keep still", /* tft.workflow.ranging.wait_percent */
    "Wi-Fi is turning back on", /* tft.workflow.ranging.wifi_back */
    "Wi-Fi is off, it returns itself", /* tft.workflow.ranging.wifi_off */
    "Wi-Fi turns off for measuring", /* tft.workflow.ranging.wifi_pending */
    "CS pair failed", /* tft.workflow.reason.cs_pair */
    "CS spread high", /* tft.workflow.reason.cs_spread */
    "operation failed", /* tft.workflow.reason.failed */
    "height: moving", /* tft.workflow.reason.height_moving */
    "IMU: gravity", /* tft.workflow.reason.imu_gravity */
    "IMU: moving", /* tft.workflow.reason.imu_moving */
    "sensor error", /* tft.workflow.reason.sensor_invalid */
    "Sensors are ready to record", /* tft.workflow.record_ready.instruction */
    "Elapsed %s", /* tft.workflow.recording.elapsed */
    "G%s: %s m", /* tft.workflow.result.distance */
    "G%s: —", /* tft.workflow.result.distance_unknown */
    "Internal error", /* tft.workflow.result.internal */
    "Result details unavailable", /* tft.workflow.result.load_failed */
    "Loading...", /* tft.workflow.result.loading */
    "Move sensors", /* tft.workflow.result.move_sensors */
    "Details: %s", /* tft.workflow.result.reason */
    "Record again", /* tft.workflow.result.record_again */
    "Source X%s Y%s m, 2D, no Z", /* tft.workflow.result.source_2d */
    "Source X%s Y%s Z%s m", /* tft.workflow.result.source_xyz */
    "Source calculated successfully", /* tft.workflow.result.success */
    "Waveform segment unavailable", /* tft.workflow.result.waveform_unavailable */
    "Rate %s Hz, max %s s", /* tft.workflow.sample_rate.limited */
    "Rate %s Hz, no time limit", /* tft.workflow.sample_rate.unlimited */
    "Scan on phone", /* tft.workflow.scan.phone */
    "Under 3: limited position; calibration can continue", /* tft.workflow.sensor.insufficient */
    "ADC:%s MAIN N%s B%s INA:%s %sV", /* tft.workflow.sensor.main */
    "ADC:%s MAIN nRF: absent (4 REMOTE mode) B%s INA:%s %sV", /* tft.workflow.sensor.main_absent */
    "%s N%s I%s B%s C%s", /* tft.workflow.sensor.node */
    "OK", /* tft.workflow.sensor.ok */
    "2D possible; connect >=4 for 3D", /* tft.workflow.sensor.planar */
    "3D ready", /* tft.workflow.sensor.ready */
    "REMOTE:%s N:nRF I:IMU B:baro C:cal", /* tft.workflow.sensor.remote_count */
    "hPa / °C / gyro °/s | MAIN %s %s", /* tft.workflow.sensor.values_legend */
    "calibrated", /* tft.workflow.state.calibrated */
    "gravity", /* tft.workflow.state.gravity */
    "moving", /* tft.workflow.state.moving */
    "error", /* tft.workflow.state.sensor_invalid */
    "valid", /* tft.workflow.state.valid */
    "Recording stopped", /* tft.workflow.stopped.instruction */
    "Calculation parameters not set for this object", /* tft.workflow.stopped.settings_missing */
    "CALCULATING SOURCE", /* tft.workflow.title.calculating */
    "CALIBRATION SETUP", /* tft.workflow.title.calib_prep */
    "CALIBRATION RESULT", /* tft.workflow.title.calib_result */
    "CALIBRATION", /* tft.workflow.title.calibrating */
    "PHONE GPS", /* tft.workflow.title.gps_choice */
    "WAITING FOR GPS", /* tft.workflow.title.gps_pending */
    "IDLE", /* tft.workflow.title.idle */
    "LAYOUT READY", /* tft.workflow.title.layout_ready */
    "OBJECT", /* tft.workflow.title.object */
    "SENSOR PLACEMENT", /* tft.workflow.title.placement */
    "BLUETOOTH RANGING", /* tft.workflow.title.ranging */
    "RANGING RESULT", /* tft.workflow.title.ranging_result */
    "READY TO RECORD", /* tft.workflow.title.record_ready */
    "RECORDING", /* tft.workflow.title.recording */
    "RESULT", /* tft.workflow.title.result */
    "3D SCAN", /* tft.workflow.title.scan_choice */
    "SCANNING", /* tft.workflow.title.scanning */
    "SENSOR CHECK", /* tft.workflow.title.sensor_check */
    "RECORDING STOPPED", /* tft.workflow.title.stopped */
    "Not all leveled: no localization", /* tft.workflow.unleveled_geophones */
    "FAIL", /* tft.workflow.value.fail */
    "X", /* tft.workflow.value.no */
    "OK", /* tft.workflow.value.ok */
    "...", /* tft.workflow.value.pending */
    "—", /* tft.workflow.value.unknown */
    "AP  %s", /* wifi.ap_ssid */
    "PRESS OK AGAIN: FORGET NETWORK", /* wifi.ask_forget */
    "PRESS OK AGAIN: ENTER SETUP", /* wifi.ask_setup */
    "CAMERA + WEBXR TEST", /* wifi.camera_webxr */
    "WI-FI COMMAND FAILED", /* wifi.command_failed */
    "WI-FI COMMAND ACCEPTED", /* wifi.command_ok */
    "CONNECT SAVED NETWORK", /* wifi.connect_saved */
    "DOWNLOAD PUBLIC CA", /* wifi.download_ca */
    "ENABLE PHONE HOTSPOT", /* wifi.enable_hotspot */
    "FORGET SAVED NETWORK", /* wifi.forget */
    "CONFIRM FORGET NETWORK", /* wifi.forget_confirm */
    "INSTALL CA IN ANDROID", /* wifi.install_android */
    "IP    %s", /* wifi.ip */
    "MODE  %s", /* wifi.mode */
    "NEXT: HTTPS VERIFY QR", /* wifi.next_verify */
    "CA CONFIRMED - SCAN HOTSPOT SETUP QR", /* wifi.notice_ca_confirmed */
    "CA DOWNLOADED - INSTALL IT, THEN CONTINUE", /* wifi.notice_ca_downloaded */
    "WI-FI CONNECTED - STARTING HTTPS", /* wifi.notice_connected */
    "CONNECTING TO SAVED WI-FI...", /* wifi.notice_connecting */
    "WI-FI DISCONNECTED - RETRYING", /* wifi.notice_disconnected */
    "HTTPS READY - SCAN VERIFY QR ON PHONE", /* wifi.notice_https_ready */
    "INSTALL CA, RECONNECT, THEN CONFIRM", /* wifi.notice_install_reconnect */
    "SETUP LINK CLOSED - SWITCHING TO WI-FI", /* wifi.notice_link_closed */
    "PHONE DISCONNECTED - SCAN WI-FI QR", /* wifi.notice_phone_disconnected */
    "PHONE CONNECTED - SCAN ROOT CA QR", /* wifi.notice_phone_ready */
    "SAVED - ENABLE HOTSPOT, CONNECTING...", /* wifi.notice_saved */
    "SETUP PAGE OPEN - ENTER HOTSPOT DATA", /* wifi.notice_setup_open */
    "HTTPS/CAMERA/WEBXR VERIFIED", /* wifi.notice_verified */
    "VERIFY INCOMPLETE - CHECK PHONE RESULTS", /* wifi.notice_verify_incomplete */
    "PHONE REPORT RECEIVED", /* wifi.phone_report */
    "RETURN + TAP CONTINUE", /* wifi.return_continue */
    "ROOT CA CONFIRMED", /* wifi.root_ca_confirmed */
    "SCAN WI-FI QR", /* wifi.scan_join */
    "SCAN QR ON PHONE", /* wifi.scan_phone */
    "CONFIRM ENTER SETUP", /* wifi.setup_confirm */
    "ENTER SETUP MODE", /* wifi.setup_enter */
    "SSID  %s", /* wifi.ssid */
    "STEP 1/4  CONNECT PHONE", /* wifi.step1_connect */
    "STEP 2/4  CA DOWNLOADED", /* wifi.step2_downloaded */
    "STEP 2/4  INSTALL ROOT CA", /* wifi.step2_install */
    "STEP 3/4  CONFIGURE HOTSPOT", /* wifi.step3_configure */
    "STEP 3/4  SETUP PAGE OPEN", /* wifi.step3_page_open */
    "STEP 3/4  SETTINGS SAVED", /* wifi.step3_saved */
    "STEP 4/4  VERIFY INCOMPLETE", /* wifi.step4_incomplete */
    "STEP 4/4  HTTPS / WEBXR VERIFIED", /* wifi.step4_verified */
    "WI-FI / WEB", /* wifi.title */
    "SEC %s  CAM %s  XR %s  AR %s", /* wifi.verify_codes */
    "STEP 4/4  VERIFY HTTPS", /* wifi.verify_https */
};

static const char *const s_ui_i18n_uk[UI_I18N_KEY_COUNT] = {
    "GeoLocator 3D", /* app.title */
    "ПЕРЕВІРТЕ КАРТУ, CS І ДРОТИ", /* boot.check_wiring */
    "БЕЗ SD - ПРОДОВЖУЄМО", /* boot.continue_no_storage */
    "НАТИСНІТЬ ЖИВЛЕННЯ ДЛЯ ВИМКНЕННЯ", /* boot.error_footer */
    "ПОМИЛКА ЗАПУСКУ", /* boot.error_title */
    "ESP32-S3  |  ЗАПУСК", /* boot.footer */
    "ФОРМАТ ЗАВЕРШЕНО", /* boot.format_complete */
    "НЕ ВИМИКАЙТЕ ЖИВЛЕННЯ", /* boot.format_do_not_power_off */
    "ФОРМАТ НЕ ВДАВСЯ", /* boot.format_failed */
    "ВИМКНІТЬ: ЗМІНІТЬ КАРТУ", /* boot.format_power_hint */
    "ФОРМАТУВАННЯ КАРТИ,", /* boot.format_progress */
    "OK: ПОВТОРИТИ", /* boot.format_retry_hint */
    "НЕ ВДАЛОСЯ ЗМОНТУВАТИ", /* boot.mount_failed */
    "КАРТА НЕ ВІДПОВІДАЄ", /* boot.reason_card_unavailable */
    "ФАЙЛОВУ СИСТЕМУ НЕ ПРОЧИТАНО", /* boot.reason_fs_mount */
    "ЗБІЙ SD-ІНТЕРФЕЙСУ", /* boot.reason_host_init */
    "ЗБІЙ ІНІЦІАЛІЗАЦІЇ СХОВИЩА", /* boot.reason_layout_init */
    "ЗБІЙ ПЕРЕВІРКИ ЧИТАННЯ/ЗАПИСУ", /* boot.reason_write_probe */
    "LEFT/UP/DOWN: БЕЗ SD", /* boot.recovery_cancel_hint */
    "КАРТА ГОТОВА - ЗАПУСК ДАЛІ", /* boot.recovery_card_ready */
    "OK: ФОРМАТ", /* boot.recovery_format_hint */
    "ВСТАВТЕ КАРТУ", /* boot.recovery_insert_card */
    "RIGHT: ПОВТОРИТИ", /* boot.recovery_retry_hint */
    "ЗБІЙ SD", /* boot.recovery_title */
    "СТИРАЄ ВСІ ДАНІ КАРТИ!", /* boot.recovery_warning */
    "ЛІМІТ SD 20 MHz", /* boot.sd_limit */
    "АКТИВНИЙ ОБ’ЄКТ...", /* boot.stage_active_object */
    "МЕРЕЖА + UI...", /* boot.stage_network */
    "СХОВИЩЕ ОБ’ЄКТІВ...", /* boot.stage_object_store */
    "ЗАПУСК...", /* boot.stage_starting */
    "СХОВИЩЕ...", /* boot.stage_storage */
    "ЗАПУСК СИСТЕМИ", /* boot.startup */
    "ЗБІЙ SD-КАРТИ / СХОВИЩА", /* boot.storage_failure */
    "ГЕОФОННИЙ ЛОКАТОР", /* boot.title */
    "ТЕСТ ЗАПИСУ НЕ ПРОЙШОВ", /* boot.write_test_failed */
    "ADC --", /* header.adc_idle */
    "Wi-Fi --", /* header.network_offline */
    "Wi-Fi OK", /* header.network_ready */
    "НАЛАШТ.", /* header.network_setup */
    "--", /* header.power_empty */
    "%s%s%s", /* header.power_format */
    "%s %s", /* header.source_format */
    "ПЕРЕВІРКА СХОВИЩА...", /* home.checking_storage */
    "UP/DOWN НАВ   OK/RIGHT ВІДКРИТИ", /* home.navigation */
    "ПЕРЕВІРКУ СХОВИЩА НЕ ЗАВЕРШЕНО", /* home.storage_incomplete */
    "MAIN", /* home.title */
    "ЗБЕРЕГТИ   > ВИДАЛИТИ   3D", /* measurement.action_delete */
    "ЗБЕРЕГТИ   ВИДАЛИТИ   >3D", /* measurement.action_preview */
    "> ЗБЕРЕГТИ   ВИДАЛИТИ   3D", /* measurement.action_save */
    "CH%u ЦИФР. ЗАВАДА", /* measurement.channel_digital */
    "CH%u  --", /* measurement.channel_empty */
    "СТАЛ", /* measurement.channel_flat */
    "CH%u ЗБІЙ %s", /* measurement.channel_invalid */
    "CH%u SERIAL? %s", /* measurement.channel_serial */
    "CH%u%+6.2fмВ %s", /* measurement.channel_value */
    "ЗМІН", /* measurement.channel_variable */
    "НЕ ВДАЛОСЯ ВИДАЛИТИ", /* measurement.delete_failed */
    "ВИДАЛЕННЯ СЕАНСУ...", /* measurement.deleting_session */
    "ADC ЦИФР. ЗАВАДА  %s", /* measurement.digital_fault */
    "ПОМИЛКА %s   OK ПОВТОРИТИ", /* measurement.error_retry */
    "UP/DN ОГЛЯД/ВИБ  OK ДІЯ  LEFT НАЗАД", /* measurement.footer */
    "ГРАФІКИ TFT ВИМК.  ADC+SD УВІМК.", /* measurement.graphs_off */
    "ГРАФІКИ TFT ВИМК.  %s", /* measurement.graphs_status */
    "OK ПОЧАТИ", /* measurement.health_start */
    "%s  %luГц  CH1-CH4", /* measurement.idle_summary */
    "НЕ ВДАЛОСЯ ЗБЕРЕГТИ СЕАНС", /* measurement.keep_failed */
    "НЕ ВДАЛОСЯ ЗАВЕРШИТИ ВИМІРЮВАННЯ", /* measurement.leave_failed */
    "ВИХІД ПІСЛЯ ЗАВЕРШЕННЯ ДІЇ...", /* measurement.leaving_after_operation */
    "є зв’язок", /* measurement.link_connected */
    "ЗВ’ЯЗ.", /* measurement.link_legend */
    "вимірює", /* measurement.link_measuring */
    "офлайн", /* measurement.link_not_connected */
    "готовий", /* measurement.link_ready */
    "ЛИШЕ ПРОСЛУХОВУВАННЯ ДО ЗАПИСУ", /* measurement.listen_only */
    "НЕМАЄ ЗБЕРЕЖЕНОГО СЕАНСУ ДЛЯ 3D ОГЛЯДУ", /* measurement.no_session_preview */
    "G1", /* measurement.node_g1 */
    "G2", /* measurement.node_g2 */
    "G3", /* measurement.node_g3 */
    "G4", /* measurement.node_g4 */
    "РІВНО", /* measurement.node_level */
    "MAIN", /* measurement.node_main */
    "РУХ", /* measurement.node_moving */
    "Н/Д", /* measurement.node_no_data */
    "РОЗВ", /* measurement.node_solved */
    "НАХИЛ", /* measurement.node_tilted */
    "НЕРОЗВ", /* measurement.node_unsolved */
    "ОБ’ЄКТ %s  [%s]", /* measurement.object_format */
    "НЕМАЄ АКТИВНОГО ОБ’ЄКТА", /* measurement.object_none */
    "ПОШУК ОБ’ЄКТА...", /* measurement.object_resolving */
    "ЗАПИС %s  %luс  %luГц", /* measurement.recording */
    "OK ЗУПИНИТИ   S:%llu   ВТРАТИ:%llu", /* measurement.recording_status */
    "%s S:%llu ВТР:%llu", /* measurement.review_counts */
    "НЕ ВДАЛОСЯ ЗБЕРЕГТИ", /* measurement.save_failed */
    "СПОЧАТКУ ОБЕРІТЬ АКТИВНИЙ ОБ’ЄКТ", /* measurement.select_object */
    "ADC SERIAL?  %s", /* measurement.serial_fault */
    "СЕАНС ЗБЕРЕЖЕНО", /* measurement.session_saved */
    "НЕ ВДАЛОСЯ ПОЧАТИ", /* measurement.start_failed */
    "ПОЧАТОК ЗАПИСУ...", /* measurement.starting_recording */
    "ЗАВЕРШЕНО", /* measurement.state_complete */
    "ВИДАЛЕННЯ", /* measurement.state_deleting */
    "ПОМИЛКА", /* measurement.state_error */
    "СЛУХ", /* measurement.state_listen */
    "НАЖИВО", /* measurement.state_live */
    "ЗАПИС", /* measurement.state_recording */
    "ЗАПУСК", /* measurement.state_starting */
    "ЗУПИНКА", /* measurement.state_stopping */
    "НЕ ВДАЛОСЯ ЗУПИНИТИ", /* measurement.stop_failed */
    "ЗАВЕРШЕНО %s  %luс", /* measurement.stopped */
    "ЗУПИНКА ПЕРЕД ВИХОДОМ...", /* measurement.stopping_leaving */
    "ЗУПИНКА ЗАПИСУ...", /* measurement.stopping_recording */
    "НАЖИВО --", /* measurement.summary_live */
    "ОБ’ЄКТ --", /* measurement.summary_object */
    "СИГНАЛИ", /* measurement.title */
    "ADC ЗБІЙ ПЕРЕДАЧІ  %s", /* measurement.transport_fault */
    "НЕВІДОМО", /* measurement.unknown */
    "ЗАЧЕКАЙТЕ...", /* measurement.wait */
    "ЗАЧЕКАЙТЕ — ВИХІД...", /* measurement.wait_leaving */
    "ВИМІРЮВАННЯ / AD7606", /* menu.measurements */
    "ОБ’ЄКТИ", /* menu.objects */
    "СХОВИЩЕ / SD CARD", /* menu.storage */
    "АКТИВНИЙ  %s  %s", /* objects.active */
    "АКТИВНИЙ %s", /* objects.active_done */
    "[АКТИВНИЙ]", /* objects.active_marker */
    "АКТИВНИЙ: НЕМАЄ", /* objects.active_none */
    "ВИДАЛИТИ ОБ’ЄКТ?", /* objects.confirm_delete */
    "OK/RIGHT ПІДТВЕРДИТИ   LEFT СКАСУВАТИ", /* objects.confirm_footer */
    "СТВОРЕНО %s", /* objects.created */
    "СТВОРЕННЯ ОБ’ЄКТА...", /* objects.creating */
    "ВИДАЛИТИ ОБ’ЄКТ", /* objects.delete */
    "Скани/файли: %u", /* objects.delete_scope.artifacts */
    "Запуски локалізації: %u", /* objects.delete_scope.runs */
    "Сеансів: %u", /* objects.delete_scope.sessions */
    "Кількість невідома", /* objects.delete_scope.unavailable */
    "НЕ ВДАЛОСЯ ПОЧАТИ ВИДАЛЕННЯ", /* objects.delete_start_failed */
    "ВИДАЛЕНО %s", /* objects.deleted */
    "ВИДАЛЕННЯ...", /* objects.deleting */
    "%s  %s%s", /* objects.list_entry */
    "НЕ ВДАЛОСЯ ЗЧИТАТИ ОБ’ЄКТИ", /* objects.list_failed */
    "ЗАВАНТАЖЕННЯ ОБ’ЄКТІВ...", /* objects.loading */
    "%s  %s", /* objects.name_pair */
    "НОВИЙ ОБ’ЄКТ", /* objects.new */
    "НЕМАЄ ОБ’ЄКТІВ", /* objects.none */
    "ДІЯ З ОБ’ЄКТОМ НЕ ВДАЛАСЯ", /* objects.operation_failed */
    "ОБЕРІТЬ ОБ’ЄКТ ДЛЯ АКТИВАЦІЇ", /* objects.select_activate */
    "ОБЕРІТЬ ОБ’ЄКТ ДЛЯ ВИДАЛЕННЯ", /* objects.select_delete */
    "СЛУЖБА ОБ’ЄКТІВ ЗАЙНЯТА / ЗБІЙ", /* objects.service_busy */
    "ЗРОБИТИ АКТИВНИМ", /* objects.set_active */
    "НЕ ВДАЛОСЯ ПОЧАТИ АКТИВАЦІЮ", /* objects.set_active_start_failed */
    "АКТИВАЦІЯ ОБ’ЄКТА...", /* objects.setting_active */
    "USB ПІДКЛЮЧЕНО — ВИМКНЕННЯ СКАСОВАНО", /* power.alert_cancelled */
    "КРИТИЧНИЙ ЗАРЯД — USB АБО СТОП", /* power.alert_critical */
    "НИЗЬКИЙ ЗАРЯД — ПІДКЛЮЧІТЬ USB", /* power.alert_low */
    "КРИТИЧНИЙ ЗАРЯД — ЗУПИНКА", /* power.alert_shutdown */
    "КРИТИЧНО — USB %uс АБО СТОП", /* power.banner_grace */
    "КРИТИЧНИЙ ЗАРЯД — ЗУПИНКА", /* power.banner_stop */
    "АКБ", /* power.battery */
    "ЗАРЯД", /* power.charging */
    "КРИТ.", /* power.critical */
    "ПОВНА", /* power.full */
    "USB %uс", /* power.grace */
    "ВИМК.", /* power.off */
    "%d%%", /* power.percent */
    "USB", /* power.usb */
    "НИЗЬК.", /* power_badge.low */
    "LEFT НАЗАД RIGHT ОБЕРТ UP/DN НАХИЛ OK ОГЛЯД", /* scan.footer */
    "ЗАВАНТАЖЕННЯ...", /* scan.loading */
    "Оцінки методів, одне джерело", /* scan.method_estimates */
    "НЕМАЄ 3D-СКАНУ", /* scan.no_3d_scan */
    "НЕМАЄ ОГЛЯДУ", /* scan.no_preview */
    "ЗАПОВНЕННЯ %u  %s", /* scan.occupancy */
    "ПАМ’ЯТІ ДЛЯ ПОЛОТНА НЕДОСТАТНЬО", /* scan.reason_canvas_memory */
    "НЕКОРЕКТНИЙ ОГЛЯД", /* scan.reason_invalid_preview */
    "НЕКОРЕКТНИЙ СЕАНС", /* scan.reason_invalid_session */
    "ЗАВАНТАЖУВАЧ НЕДОСТУПНИЙ", /* scan.reason_loader */
    "НЕМАЄ ОГЛЯДУ АБО ЗАПОВНЕННЯ", /* scan.reason_no_data */
    "НЕМАЄ КОРЕКТНОГО ОГЛЯДУ АБО ЗАПОВНЕННЯ", /* scan.reason_no_valid_data */
    "НЕДОСТАТНЬО ПАМ’ЯТІ ДЛЯ 3D ОГЛЯДУ", /* scan.reason_preview_memory */
    "ОГЛЯД НЕ ЗНАЙДЕНО", /* scan.reason_preview_missing */
    "3D ОГЛЯД НЕДОСТУПНИЙ", /* scan.reason_preview_unavailable */
    "НЕ ВДАЛОСЯ ПРОЧИТАТИ ОГЛЯД", /* scan.reason_preview_unreadable */
    "ТРИВАЄ ЗАПИС", /* scan.reason_recording_active */
    "СКАН НЕ ЗНАЙДЕНО", /* scan.reason_scan_missing */
    "НЕ ВДАЛОСЯ ПРОЧИТАТИ СКАН", /* scan.reason_scan_unreadable */
    "НЕ ВДАЛОСЯ СТВОРИТИ ЕКРАН", /* scan.reason_screen_memory */
    "СКАН CRC ЗАСТАРІВ", /* scan.reason_stale_crc */
    "РАМКА, БЕЗ СКАНА", /* scan.sensor_estimates_frame */
    "3D ОГЛЯД", /* scan.title */
    "ВОКСЕЛІ %u  СПРОЩЕННЯ %u%s%s", /* scan.voxels */
    "ПЕРЕВІРКА СХОВИЩА...", /* shared.checking_storage */
    "--", /* shared.empty */
    "UP/DOWN НАВ   OK ВИБІР   LEFT НАЗАД", /* shared.footer_back */
    "UP/DOWN НАВ   OK ВИБІР   LEFT СКАСУВАТИ", /* shared.footer_cancel */
    "ТРИВАЄ ДІЯ ВИМІРЮВАННЯ", /* shared.measure_operation_busy */
    "СЛУЖБА ВИМІРЮВАНЬ НЕДОСТУПНА", /* shared.measure_service_unavailable */
    "ПІДТВЕРДИТИ СТИРАННЯ ДАНИХ", /* storage.confirm_erase */
    "НАТИСНІТЬ OK ЩЕ РАЗ, ЩОБ СТЕРТИ ДАНІ", /* storage.confirm_prompt */
    "ДАНІ СТЕРТО. ОНОВІТЬ ІНФОРМАЦІЮ", /* storage.erase_complete */
    "СТЕРТИ ДАНІ ПРИСТРОЮ", /* storage.erase_device */
    "НЕ ВДАЛОСЯ СТЕРТИ ДАНІ", /* storage.erase_failed */
    "СТИРАННЯ ДАНИХ...", /* storage.erasing */
    "НЕ ВДАЛОСЯ ПРОЧИТАТИ SD", /* storage.info_failed */
    "ДАНІ SD НЕ ЗАВАНТАЖЕНО", /* storage.not_loaded */
    "?????? ? ????? ????????? ???????", /* storage.recovery_complete */
    "???? ? ????? ????????? ???????", /* storage.recovery_failed */
    "ОНОВИТИ ДАНІ SD", /* storage.refresh */
    "ОНОВЛЕННЯ ДАНИХ SD...", /* storage.refreshing */
    "ЛІМІТ SD ПІД ЧАС РОБОТИ 20 MHz", /* storage.runtime_limit */
    "СЛУЖБА СХОВИЩА ЗАЙНЯТА / ЗБІЙ", /* storage.service_busy */
    "ВІЛЬНО %.2f GB   SPI %.2f MHz", /* storage.space */
    "%s  %s  %.2f GB", /* storage.summary */
    "СХОВИЩЕ", /* storage.title */
    "CORE0 СЕРВІСИ/LVGL   CORE1 ЗБІР ДАНИХ", /* system.cores */
    "ПЗ      %s", /* system.firmware */
    "ВНУТР. КУЧА %lu KB вільно", /* system.heap */
    "ВНУТР. КУЧА -- KB вільно", /* system.heap_empty */
    "ESP-IDF  %s", /* system.idf_version */
    "МОВА: АНГЛІЙСЬКА", /* system.language_english */
    "UP/DN ВИБІР  OK ЗАСТОС.", /* system.language_footer */
    "МОВА: УКРАЇНСЬКА", /* system.language_ukrainian */
    "LVGL     %d.%d.%d + esp_lvgl_port", /* system.lvgl_version */
    "PSRAM    %lu KB вільно", /* system.psram */
    "PSRAM    -- KB вільно", /* system.psram_empty */
    "СИСТЕМА", /* system.title */
    "ЧАС      %lld с", /* system.uptime */
    "ЧАС      -- с", /* system.uptime_empty */
    "Проблема з обов’язковим модулем", /* tft.hw.degraded_banner */
    "AD7606", /* tft.hw.module.ad7606 */
    "Барометр", /* tft.hw.module.barometer */
    "Основний XIAO", /* tft.hw.module.main_xiao */
    "Монітор живлення", /* tft.hw.module.power_monitor */
    "SD-картка", /* tft.hw.module.sd_card */
    "Мультиплексор", /* tft.hw.module.sensor_mux */
    "НЕОБ.", /* tft.hw.optional */
    "Віддалені датчики: {count}", /* tft.hw.remotes */
    "ОБОВ.", /* tft.hw.required */
    "Не підключено", /* tft.hw.state.absent */
    "Обмежено", /* tft.hw.state.degraded */
    "Несправність", /* tft.hw.state.fault */
    "Підключено", /* tft.hw.state.present */
    "Не тестовано", /* tft.hw.state.unknown */
    "ОБЛАДНАННЯ", /* tft.hw.title */
    "Назад", /* tft.workflow.action.back */
    "Wi-Fi ВИМК, розрахувати", /* tft.workflow.action.calculate_positions */
    "Wi-Fi УВІМК, розрахувати", /* tft.workflow.action.calculate_positions_keep_wifi */
    "Визначити джерело", /* tft.workflow.action.calculate_sources */
    "Підтвердити", /* tft.workflow.action.confirm */
    "Видалити запис", /* tft.workflow.action.delete_recording */
    "Перейти до калібрування", /* tft.workflow.action.go_calibration */
    "Живі сигнали", /* tft.workflow.action.live_signals */
    "Перемістити датчики", /* tft.workflow.action.move_sensors */
    "Розмістити датчики", /* tft.workflow.action.place_sensors */
    "Датчики встановлено", /* tft.workflow.action.placement_confirmed */
    "Записати ще раз", /* tft.workflow.action.record_again */
    "Повторити", /* tft.workflow.action.retry */
    "Змінити частоту", /* tft.workflow.action.sample_rate */
    "Датчики розміщено", /* tft.workflow.action.sensors_placed */
    "Пропустити", /* tft.workflow.action.skip */
    "Почати вимірювання", /* tft.workflow.action.start_measurement */
    "Почати запис", /* tft.workflow.action.start_recording */
    "Зупинити запис", /* tft.workflow.action.stop_recording */
    "Виконано %s%%", /* tft.workflow.calculating.progress */
    "Далі: розставте геофони", /* tft.workflow.calib_result.next */
    "Етап %s", /* tft.workflow.calibration.header */
    "Розмістіть усі датчики поруч із головним корпусом", /* tft.workflow.calibration.prep */
    "%s: %s", /* tft.workflow.calibration.reason */
    "%s", /* tft.workflow.calibration.reason_single */
    "%s В:%s z:%sсм І:%s b:%s", /* tft.workflow.calibration.row */
    "%s В:%s z:%sсм", /* tft.workflow.calibration.row_main */
    "Калібрування: %s", /* tft.workflow.calibration.status */
    "Видалити запис? OK = так, LEFT = ні", /* tft.workflow.confirm.delete */
    "Оберіть пару для виправлення", /* tft.workflow.distance.choose */
    "Крок UP/DOWN: %s м", /* tft.workflow.distance.digit */
    "Ввести відстані", /* tft.workflow.distance.edit */
    "Редагувати цю пару", /* tft.workflow.distance.enter */
    "%s-%s: %s см (вручну)", /* tft.workflow.distance.manual */
    "Пара %s-%s", /* tft.workflow.distance.pair */
    "UP/DOWN пара OK ввід LEFT скасувати", /* tft.workflow.distance.pair_hint */
    "Відхилено: перевірте відстань і кабель", /* tft.workflow.distance.rejected */
    "Зберегти й перерахувати", /* tft.workflow.distance.save */
    "Збережено. Далі пара або LEFT", /* tft.workflow.distance.saved */
    "Збереження й перерахунок...", /* tft.workflow.distance.saving */
    "Відстань: %s м", /* tft.workflow.distance.value */
    "UP/DOWN цифра L/R зсув OK зберегти", /* tft.workflow.distance.value_hint */
    "Очікування GPS телефона", /* tft.workflow.gps.waiting */
    "UP/DOWN вибір OK дія LEFT назад RIGHT 3D", /* tft.workflow.hint.3d */
    "UP/DOWN дії  OK вибір  LEFT назад", /* tft.workflow.hint.navigation */
    "UP/DOWN дії OK вибір RIGHT далі", /* tft.workflow.hint.ranging_pages */
    "Виберіть об’єкт", /* tft.workflow.idle.select_object */
    "Менше 3 геофонів: джерело не знайти", /* tft.workflow.insufficient_geophones */
    "Схема готова до розрахунку", /* tft.workflow.layout.ready */
    "Wi-Fi вимк. = точніше", /* tft.workflow.layout.wifi_hint */
    "Звʼязок із телефоном повернеться", /* tft.workflow.layout.wifi_phone */
    "Задати невизначеність (м/с)", /* tft.workflow.material.custom_sigma */
    "+/- %s м/с", /* tft.workflow.material.custom_sigma_value */
    "Задати швидкість (м/с)", /* tft.workflow.material.custom_speed */
    "v: %s м/с", /* tft.workflow.material.custom_speed_value */
    "UP/DOWN зміна OK далі LEFT назад", /* tft.workflow.material.hint_adjust */
    "UP/DOWN зміна OK зберегти LEFT назад", /* tft.workflow.material.hint_save */
    "UP/DOWN вибір OK вибрати LEFT назад", /* tft.workflow.material.hint_select */
    "Цегляна кладка", /* tft.workflow.material.name.brick_masonry */
    "Бетон (монолітний)", /* tft.workflow.material.name.concrete */
    "Власне значення", /* tft.workflow.material.name.custom */
    "Залізобетон", /* tft.workflow.material.name.reinforced_concrete */
    "Будівельне сміття / завали", /* tft.workflow.material.name.rubble */
    "Сухий ґрунт / засипка", /* tft.workflow.material.name.soil_dry */
    "Водонасичений ґрунт", /* tft.workflow.material.name.soil_saturated */
    "Сталеві конструкції", /* tft.workflow.material.name.steel */
    "Невідомий матеріал", /* tft.workflow.material.name.unknown */
    "%s", /* tft.workflow.material.option */
    "авто", /* tft.workflow.material.range_auto */
    "Межі: %s-%s м/с", /* tft.workflow.material.range_current */
    "UP задати OK зберегти LEFT назад", /* tft.workflow.material.range_hint_auto */
    "Макс. швидкість (м/с)", /* tft.workflow.material.range_max */
    "Мін. швидкість (м/с)", /* tft.workflow.material.range_min */
    "%s м/с", /* tft.workflow.material.range_value */
    "Матеріал: %s", /* tft.workflow.material.row */
    "Помилка збереження", /* tft.workflow.material.save_failed */
    "Збережено", /* tft.workflow.material.saved */
    "Збереження...", /* tft.workflow.material.saving */
    "%s +/- %s м/с", /* tft.workflow.material.speed */
    "МАТЕРІАЛ", /* tft.workflow.material.title */
    "Матеріал: не задано", /* tft.workflow.material.unset */
    "Недостатньо пам’яті. Спробуйте ще раз.", /* tft.workflow.memory_unavailable */
    "Зайнято", /* tft.workflow.notice.busy */
    "Ще не стабільно", /* tft.workflow.notice.placement_blocked */
    "Спершу зупиніть сигнали", /* tft.workflow.notice.sample_rate_busy */
    "Стан змінився", /* tft.workflow.notice.state_changed */
    "%s", /* tft.workflow.object.name */
    "Виберіть об’єкт для вимірювання", /* tft.workflow.object.select */
    "Скасовано", /* tft.workflow.phase.cancelled */
    "Завершено", /* tft.workflow.phase.complete */
    "Не вдалося", /* tft.workflow.phase.failed */
    "Очікування", /* tft.workflow.phase.idle */
    "Виконується", /* tft.workflow.phase.running */
    "Усі стабільні", /* tft.workflow.placement.all_stable */
    "%s: %s%s", /* tft.workflow.placement.blocked */
    "Час пішов назад", /* tft.workflow.placement.reason.clock_backward */
    "Збір відліків", /* tft.workflow.placement.reason.collecting */
    "Немає стану MAIN", /* tft.workflow.placement.reason.device_status_busy */
    "Барометр MAIN н/д", /* tft.workflow.placement.reason.main_barometer */
    "Немає опори MAIN", /* tft.workflow.placement.reason.main_reference */
    "Барометр вузла н/д", /* tft.workflow.placement.reason.node_environment */
    "Опора не збігається", /* tft.workflow.placement.reason.node_reference */
    "Немає свіжих даних", /* tft.workflow.placement.reason.node_stale */
    "Не вертикально", /* tft.workflow.placement.reason.not_vertical */
    "Буфер повний", /* tft.workflow.placement.reason.overflow_blocked */
    "Ще не усталилось", /* tft.workflow.placement.reason.range_exceeded */
    "Відлік застарів", /* tft.workflow.placement.reason.sample_stale */
    "Стабільно", /* tft.workflow.placement.reason.stable */
    "%s В:%s dH:%sсм над.р.м:%sм Ст:%s", /* tft.workflow.placement.row */
    "Очікування стабільності", /* tft.workflow.placement.waiting */
    "Натисніть OK ще раз, щоб зупинити запис", /* tft.workflow.preview.stop_confirm */
    "Стан змінився", /* tft.workflow.preview.stop_error */
    "OK двічі: зупинити запис", /* tft.workflow.preview.stop_hint */
    "Зупинення запису...", /* tft.workflow.preview.stop_pending */
    "%s-%s: %s см > кабель", /* tft.workflow.ranging.cable_exceeded */
    "довше за кабель", /* tft.workflow.ranging.cause.cable_exceeded */
    "напрямки не збігаються", /* tft.workflow.ranging.cause.directions_disagree */
    "вузол від’єднався", /* tft.workflow.ranging.cause.disconnected */
    "в один бік: замало результатів", /* tft.workflow.ranging.cause.few_samples */
    "в один бік: низька довіра", /* tft.workflow.ranging.cause.low_confidence */
    "в один бік: немає результату", /* tft.workflow.ranging.cause.no_measurement */
    "%s-%s: %s см", /* tft.workflow.ranging.distance */
    "Вимірювання не вдалося", /* tft.workflow.ranging.failed */
    "%s-%s: -- (%s / %s см)", /* tft.workflow.ranging.pair_diag */
    "Виміри %s/%s нев’язка %s см", /* tft.workflow.ranging.pairs */
    "Частково виміряно", /* tft.workflow.ranging.partial */
    "%s сигма:%s см", /* tft.workflow.ranging.sigma */
    "Відстані виміряно", /* tft.workflow.ranging.success */
    "Вимірювання по радіо, не чіпати", /* tft.workflow.ranging.wait */
    "Вимірювання %s%%, не чіпати", /* tft.workflow.ranging.wait_percent */
    "Wi-Fi вмикається знову", /* tft.workflow.ranging.wifi_back */
    "Wi-Fi вимкнено, увімкнеться саме", /* tft.workflow.ranging.wifi_off */
    "Wi-Fi вимкнеться на час виміру", /* tft.workflow.ranging.wifi_pending */
    "помилка пари CS", /* tft.workflow.reason.cs_pair */
    "великий розкид CS", /* tft.workflow.reason.cs_spread */
    "операція не вдалася", /* tft.workflow.reason.failed */
    "висота: рух", /* tft.workflow.reason.height_moving */
    "IMU: гравітація", /* tft.workflow.reason.imu_gravity */
    "IMU: рух", /* tft.workflow.reason.imu_moving */
    "помилка датчика", /* tft.workflow.reason.sensor_invalid */
    "Датчики готові до запису", /* tft.workflow.record_ready.instruction */
    "Тривалість %s", /* tft.workflow.recording.elapsed */
    "G%s: %s м", /* tft.workflow.result.distance */
    "G%s: —", /* tft.workflow.result.distance_unknown */
    "Внутрішня помилка", /* tft.workflow.result.internal */
    "Деталі результату недоступні", /* tft.workflow.result.load_failed */
    "Завантаження...", /* tft.workflow.result.loading */
    "Перемістіть датчики", /* tft.workflow.result.move_sensors */
    "Деталі: %s", /* tft.workflow.result.reason */
    "Запишіть ще раз", /* tft.workflow.result.record_again */
    "Джерело X%s Y%s м, 2D, без Z", /* tft.workflow.result.source_2d */
    "Джерело X%s Y%s Z%s м", /* tft.workflow.result.source_xyz */
    "Джерело успішно визначено", /* tft.workflow.result.success */
    "Фрагмент хвильової форми недоступний", /* tft.workflow.result.waveform_unavailable */
    "Частота %s Гц, до %s с", /* tft.workflow.sample_rate.limited */
    "Частота %s Гц, без ліміту часу", /* tft.workflow.sample_rate.unlimited */
    "Скануйте на телефоні", /* tft.workflow.scan.phone */
    "Менше 3: позиція обмежена; калібрування триває", /* tft.workflow.sensor.insufficient */
    "ADC:%s MAIN N%s B%s INA:%s %sV", /* tft.workflow.sensor.main */
    "ADC:%s MAIN nRF: відсутній (режим 4 REMOTE) B%s INA:%s %sV", /* tft.workflow.sensor.main_absent */
    "%s N%s I%s B%s C%s", /* tft.workflow.sensor.node */
    "OK", /* tft.workflow.sensor.ok */
    "Можливе 2D; для 3D підключіть щонайменше 4", /* tft.workflow.sensor.planar */
    "3D готове", /* tft.workflow.sensor.ready */
    "REMOTE:%s N:nRF I:IMU B:баро C:кал", /* tft.workflow.sensor.remote_count */
    "гПа / °C / гіро °/с | MAIN %s %s", /* tft.workflow.sensor.values_legend */
    "Відкалібровано", /* tft.workflow.state.calibrated */
    "Гравітація", /* tft.workflow.state.gravity */
    "Рух", /* tft.workflow.state.moving */
    "Помилка", /* tft.workflow.state.sensor_invalid */
    "Дійсна", /* tft.workflow.state.valid */
    "Запис зупинено", /* tft.workflow.stopped.instruction */
    "Для цього об’єкта не задано параметри розрахунку", /* tft.workflow.stopped.settings_missing */
    "Визначення джерела", /* tft.workflow.title.calculating */
    "Підготовка до калібрування", /* tft.workflow.title.calib_prep */
    "Результат калібрування", /* tft.workflow.title.calib_result */
    "Калібрування", /* tft.workflow.title.calibrating */
    "GPS ТЕЛЕФОНА", /* tft.workflow.title.gps_choice */
    "ОЧІКУВАННЯ GPS", /* tft.workflow.title.gps_pending */
    "Очікування", /* tft.workflow.title.idle */
    "СХЕМА ГОТОВА", /* tft.workflow.title.layout_ready */
    "Об’єкт", /* tft.workflow.title.object */
    "Розміщення датчиків", /* tft.workflow.title.placement */
    "Вимірювання дальності", /* tft.workflow.title.ranging */
    "Результат дальності", /* tft.workflow.title.ranging_result */
    "Готово до запису", /* tft.workflow.title.record_ready */
    "Запис", /* tft.workflow.title.recording */
    "Результат", /* tft.workflow.title.result */
    "3D СКАНУВАННЯ", /* tft.workflow.title.scan_choice */
    "СКАНУВАННЯ", /* tft.workflow.title.scanning */
    "Перевірка датчиків", /* tft.workflow.title.sensor_check */
    "Запис зупинено", /* tft.workflow.title.stopped */
    "Не всі вирівняні: без локалізації", /* tft.workflow.unleveled_geophones */
    "НЕВДАЛО", /* tft.workflow.value.fail */
    "Ні", /* tft.workflow.value.no */
    "OK", /* tft.workflow.value.ok */
    "...", /* tft.workflow.value.pending */
    "—", /* tft.workflow.value.unknown */
    "AP  %s", /* wifi.ap_ssid */
    "НАТИСНІТЬ OK ЩЕ РАЗ: ЗАБУТИ МЕРЕЖУ", /* wifi.ask_forget */
    "НАТИСНІТЬ OK ЩЕ РАЗ: НАЛАШТУВАТИ", /* wifi.ask_setup */
    "ТЕСТ КАМЕРИ + WEBXR", /* wifi.camera_webxr */
    "КОМАНДА WI-FI НЕ ВДАЛАСЯ", /* wifi.command_failed */
    "КОМАНДУ WI-FI ПРИЙНЯТО", /* wifi.command_ok */
    "ПІДКЛЮЧИТИСЯ ДО ЗБЕРЕЖЕНОЇ МЕРЕЖІ", /* wifi.connect_saved */
    "ЗАВАНТАЖТЕ ПУБЛІЧНИЙ CA", /* wifi.download_ca */
    "УВІМКНІТЬ ТОЧКУ ДОСТУПУ ТЕЛЕФОНА", /* wifi.enable_hotspot */
    "ЗАБУТИ ЗБЕРЕЖЕНУ МЕРЕЖУ", /* wifi.forget */
    "ПІДТВЕРДИТИ ЗАБУВАННЯ МЕРЕЖІ", /* wifi.forget_confirm */
    "ВСТАНОВІТЬ CA В ANDROID", /* wifi.install_android */
    "IP    %s", /* wifi.ip */
    "РЕЖИМ  %s", /* wifi.mode */
    "ДАЛІ: QR ДЛЯ ПЕРЕВІРКИ HTTPS", /* wifi.next_verify */
    "CA ПІДТВЕРДЖЕНО — СКАНУЙТЕ QR НАЛАШТУВАННЯ", /* wifi.notice_ca_confirmed */
    "CA ЗАВАНТАЖЕНО — ВСТАНОВІТЬ І ПРОДОВЖТЕ", /* wifi.notice_ca_downloaded */
    "WI-FI ПІДКЛЮЧЕНО — ЗАПУСК HTTPS", /* wifi.notice_connected */
    "ПІДКЛЮЧЕННЯ ДО ЗБЕРЕЖЕНОЇ МЕРЕЖІ WI-FI...", /* wifi.notice_connecting */
    "WI-FI ВІД’ЄДНАНО — ПОВТОРНА СПРОБА", /* wifi.notice_disconnected */
    "HTTPS ГОТОВЕ — СКАНУЙТЕ QR ПЕРЕВІРКИ", /* wifi.notice_https_ready */
    "ВСТАНОВІТЬ CA, ПІДКЛЮЧІТЬСЯ Й ПІДТВЕРДЬТЕ", /* wifi.notice_install_reconnect */
    "ПОСИЛАННЯ ЗАКРИТО — ПЕРЕХІД НА WI-FI", /* wifi.notice_link_closed */
    "ТЕЛЕФОН ВІД’ЄДНАНО — СКАНУЙТЕ QR WI-FI", /* wifi.notice_phone_disconnected */
    "ТЕЛЕФОН ПІДКЛЮЧЕНО — СКАНУЙТЕ QR ROOT CA", /* wifi.notice_phone_ready */
    "ЗБЕРЕЖЕНО — УВІМКНІТЬ ТОЧКУ ДОСТУПУ", /* wifi.notice_saved */
    "СТОРІНКУ ВІДКРИТО — ВВЕДІТЬ ДАНІ ТОЧКИ ДОСТУПУ", /* wifi.notice_setup_open */
    "HTTPS/КАМЕРА/WEBXR ПЕРЕВІРЕНО", /* wifi.notice_verified */
    "ПЕРЕВІРКУ НЕ ЗАВЕРШЕНО — ПЕРЕВІРТЕ ТЕЛЕФОН", /* wifi.notice_verify_incomplete */
    "ЗВІТ З ТЕЛЕФОНА ОТРИМАНО", /* wifi.phone_report */
    "ПОВЕРНІТЬСЯ Й НАТИСНІТЬ ДАЛІ", /* wifi.return_continue */
    "ROOT CA ПІДТВЕРДЖЕНО", /* wifi.root_ca_confirmed */
    "СКАНУЙТЕ QR WI-FI", /* wifi.scan_join */
    "СКАНУЙТЕ QR ТЕЛЕФОНОМ", /* wifi.scan_phone */
    "ПІДТВЕРДИТИ НАЛАШТУВАННЯ", /* wifi.setup_confirm */
    "УВІЙТИ В РЕЖИМ НАЛАШТУВАННЯ", /* wifi.setup_enter */
    "SSID  %s", /* wifi.ssid */
    "КРОК 1/4  ПІДКЛЮЧІТЬ ТЕЛЕФОН", /* wifi.step1_connect */
    "КРОК 2/4  CA ЗАВАНТАЖЕНО", /* wifi.step2_downloaded */
    "КРОК 2/4  ВСТАНОВІТЬ ROOT CA", /* wifi.step2_install */
    "КРОК 3/4  НАЛАШТУЙТЕ HOTSPOT", /* wifi.step3_configure */
    "КРОК 3/4  СТОРІНКУ ВІДКРИТО", /* wifi.step3_page_open */
    "КРОК 3/4  НАЛАШТУВАННЯ ЗБЕРЕЖЕНО", /* wifi.step3_saved */
    "КРОК 4/4  ПЕРЕВІРКУ НЕ ЗАВЕРШЕНО", /* wifi.step4_incomplete */
    "КРОК 4/4  HTTPS / WEBXR ПЕРЕВІРЕНО", /* wifi.step4_verified */
    "Wi-Fi / ВЕБ", /* wifi.title */
    "SEC %s  CAM %s  XR %s  AR %s", /* wifi.verify_codes */
    "КРОК 4/4  ПЕРЕВІРТЕ HTTPS", /* wifi.verify_https */
};

const char *ui_i18n_catalog_en(ui_i18n_key_t key)
{
    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {
        return 0;
    }
    return s_ui_i18n_en[(unsigned)key];
}

const char *ui_i18n_catalog_uk(ui_i18n_key_t key)
{
    if ((unsigned)key >= (unsigned)UI_I18N_KEY_COUNT) {
        return 0;
    }
    return s_ui_i18n_uk[(unsigned)key];
}
