"""The synthetic source's timestamps (emulator scenario S10).

The stamp arithmetic is pure and has its own host test (tests/host/seismic_test_stamp_test.c). A host test cannot see the loop
around it, and that is where the defects were: an edit once deleted "last_stamp_us = stamp_us;", the monotonic guard never ran
and scenario S10 failed on and off with the first write refused. Nothing noticed, because the guard's absence only shows with
unlucky timing. These tests pin the shape of the loop: a stamp grid that advances by whole ticks, a wait for the clock before
pushing, and the stamps taken from the grid.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _generator() -> str:
    return (ROOT / "components/seismic_test_generator/seismic_test_generator.c").read_text(encoding="utf-8")


def test_the_grid_advances_by_a_whole_tick_after_the_frames_are_pushed() -> None:
    source = _generator()
    push = source.index("seismic_buffer_push")
    advance = source.index("next_stamp_us = first_stamp_us + (int64_t)frames_per_tick * frame_period_us;")
    assert push < advance
    # And the very same variable is what the next tick reads.
    assert "next_stamp_us, frames_per_tick, frame_period_us, esp_timer_get_time()," in source


def test_a_tick_waits_for_the_clock_before_it_pushes_a_frame() -> None:
    source = _generator()
    first = source.index("seismic_test_stamp_first(")
    ready = source.index("seismic_test_stamp_ready_at(first_stamp_us", first)
    wait = source.index("clock_us < ready_us", ready)
    frames = source.index("for (int k = 0; k < frames_per_tick; ++k)", wait)
    assert first < ready < wait < frames
    # The wait yields the CPU (a spin on the real-time core starved IDLE1 and tripped the task watchdog in the emulator).
    assert "vTaskDelay(1)" in source[wait:frames]


def test_every_stamp_comes_from_the_grid() -> None:
    source = _generator()
    assert source.count("first_stamp_us + (int64_t)k * frame_period_us") == 1
    # No second, hand-written stamp expression, and no stamp from the wall clock inside the frame loop.
    assert "tick_us" not in source
    assert "last_stamp_us" not in source
    frames = source.index("for (int k = 0; k < frames_per_tick; ++k)")
    assert "esp_timer_get_time()" not in source[frames:source.index("next_stamp_us = first_stamp_us")]
