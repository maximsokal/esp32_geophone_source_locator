import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/host/manifest.d/max479b-isr-notifications.json"
SOURCE = ROOT / "components/ad7606_acquisition/tests/ad7606_acquisition_runtime_test.c"


def test_max479b_interleavings_are_registered_and_called():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = {entry["name"]: entry for entry in manifest["tests"]}
    entry = entries["max479b_ad7606_notifications"]
    assert "components/ad7606_acquisition/tests/ad7606_acquisition_runtime_test.c" in entry["sources"]

    source = SOURCE.read_text(encoding="utf-8")
    for case in (
        "test_busy_signal_before_first_wait_is_retained",
        "test_two_busy_signals_before_wake_use_pcnt_index",
        "test_stop_isr_worker_lifecycle_interleaving",
        "test_status_writer_lock_blocks_snapshot_reader",
    ):
        assert f"static void {case}(void)" in source
        assert f"    {case}();" in source

