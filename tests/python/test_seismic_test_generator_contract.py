"""The synthetic source's timestamps (emulator scenario S10).

The stamp arithmetic is a pure function with its own host test (tests/host/seismic_test_stamp_test.c). What a host test
cannot see is the loop around it, and that is where the defect was: an edit deleted "last_stamp_us = stamp_us;", the
generator kept passing last == 0, the monotonic guard never ran, and scenario S10 failed on and off with the first write
refused (a stamp had stepped back and the V2 encoder rejects such a block). Nothing noticed, because the guard's absence
only shows with unlucky timing.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _generator() -> str:
    return (ROOT / "components/seismic_test_generator/seismic_test_generator.c").read_text(encoding="utf-8")


def test_every_frame_records_its_stamp_as_the_previous_one() -> None:
    source = _generator()
    call = source.index("seismic_test_stamp_next(")
    assign = source.index("last_stamp_us = stamp_us;", call)
    push = source.index("seismic_buffer_push", call)
    # The previous stamp is updated right after it is computed and before the frame is pushed.
    assert call < assign < push
    # And it is the very same variable the call reads.
    assert "last_stamp_us, esp_timer_get_time())" in source[call:assign]


def test_the_stamp_is_computed_in_one_place() -> None:
    source = _generator()
    assert source.count("seismic_test_stamp_next(") == 1
    # No second, hand-written stamp expression next to it.
    assert "tick_us - (int64_t)(frames_per_tick" not in source
    assert "last_stamp_us + frame_period_us" not in source
