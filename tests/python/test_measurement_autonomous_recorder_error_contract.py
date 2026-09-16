from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEASUREMENT = ROOT / "components/measurement_service/measurement_service.c"
RECORDER = ROOT / "components/seismic_recorder/seismic_recorder.c"

measurement = MEASUREMENT.read_text(encoding="utf-8")
recorder = RECORDER.read_text(encoding="utf-8")

assert "#define MEASUREMENT_SERVICE_RECORDING_POLL_MS  250U" in measurement
assert "#define MEASUREMENT_SERVICE_STOPPING_POLL_MS   100U" in measurement
assert "measurement_service_worker_wait_ticks(state)" in measurement
assert "state == MEASUREMENT_SERVICE_RECORDING ||" in measurement
assert "state == MEASUREMENT_SERVICE_STOPPING" in measurement
assert "measurement_service_process_stop_poll();" in measurement

poll_start = measurement.index("static void measurement_service_process_stop_poll")
poll_end = measurement.index("static void measurement_service_process_stop", poll_start + 1)
poll = measurement[poll_start:poll_end]
assert "measurement_state != MEASUREMENT_SERVICE_RECORDING" in poll
assert "measurement_state != MEASUREMENT_SERVICE_STOPPING" in poll
assert "recorder_still_finishing" in poll
assert "s_snapshot.state = MEASUREMENT_SERVICE_STOPPING" in poll
assert "s_snapshot.state = MEASUREMENT_SERVICE_ERROR" in poll
assert "became_terminal_error" in poll
assert "measurement_service_notify_terminal_error" in poll
assert poll.index("if (recorder_still_finishing)") < poll.rindex("measurement_service_notify_terminal_error")

terminal_start = measurement.index("static void measurement_service_notify_terminal_error")
terminal_end = measurement.index("static void measurement_service_process_stop_poll", terminal_start)
terminal = measurement[terminal_start:terminal_end]
assert "measurement_service_notify_storage_refresh();" in terminal
assert "after-recorder-error-finalization" in terminal

worker_start = measurement.index("static void measurement_service_task")
worker_end = measurement.index("static bool measurement_service_command_allowed_locked", worker_start)
worker = measurement[worker_start:worker_end]
assert "measurement_service_worker_wait_ticks(state)" in worker
assert "MEASUREMENT_SERVICE_RECORDING" in worker
assert "MEASUREMENT_SERVICE_STOPPING" in worker
assert "measurement_service_process_stop_poll();" in worker
assert "storage_manager" not in worker
assert "fopen(" not in worker
assert "fread(" not in worker
assert "fwrite(" not in worker

# Explicit STOP after an already-terminal recorder error must be idempotent at
# the recorder boundary: production STOP returns success before creating a new
# stop/finalization request when running is already false.
stop_start = recorder.index("esp_err_t seismic_recorder_stop(void)")
stop_end = recorder.index("esp_err_t seismic_recorder_wait_for_stop", stop_start)
stop = recorder[stop_start:stop_end]
assert "if (!s_snapshot.running)" in stop
assert "return ESP_OK;" in stop
assert stop.index("if (!s_snapshot.running)") < stop.index("s_stop_requested = true;")

print("MEASUREMENT_AUTONOMOUS_ERROR_SOURCE_PASS")
