"""Static contracts for post-operation FreeRTOS task liveness beats."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def _positions(source: str, needle: str) -> list[int]:
    positions: list[int] = []
    offset = 0
    while True:
        position = source.find(needle, offset)
        if position < 0:
            return positions
        positions.append(position)
        offset = position + len(needle)


def test_ui_heartbeat_follows_event_receive_and_lvgl_dispatch():
    source = (REPO_ROOT / "components/ui/ui_event_task.c").read_text(encoding="utf-8")
    task = _function_body(source, "static void ui_event_task_main")

    assert "runtime_observability_beat(" not in task
    observe_positions = _positions(task, "ui_event_observe(")
    assert len(observe_positions) == 3
    receive = task.index("app_event_bus_receive_timeout(")
    unlock = task.rindex("lvgl_port_unlock();")
    assert all(position > receive for position in observe_positions)
    assert observe_positions[-1] > unlock


def test_keypad_heartbeat_follows_each_poll_delay():
    source = (REPO_ROOT / "components/keypad/keypad_event_worker.c").read_text(encoding="utf-8")
    task = _function_body(source, "void keypad_event_worker")

    beats = _positions(task, "runtime_observability_beat(")
    delays = _positions(task, "vTaskDelay(")
    assert len(beats) == len(delays) == 2
    assert all(beat > delay for beat, delay in zip(beats, delays))


def test_power_button_heartbeat_follows_poll_delay():
    source = (REPO_ROOT / "components/power_control/power_button_monitor_task.c").read_text(encoding="utf-8")
    task = _function_body(source, "void power_button_monitor_worker")

    beat = task.index("runtime_observability_beat(")
    delay = task.index("vTaskDelay(")
    assert beat > delay


def test_acquisition_heartbeat_follows_read_and_buffer_publication():
    source = (REPO_ROOT / "components/ad7606_acquisition/ad7606_acquisition.c").read_text(encoding="utf-8")
    task = _function_body(source, "static void ad7606_worker(")

    beats = _positions(task, "runtime_observability_beat(")
    assert len(beats) == 2
    read = task.index("ad7606_driver_read(")
    publish = task.index("seismic_buffer_push(")
    assert beats[0] > read
    assert beats[1] > publish
