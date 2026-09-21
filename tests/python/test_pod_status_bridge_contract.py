"""Source contracts for the device_status_pod_bridge live pod telemetry (MAX-118)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_local_position_is_published_alongside_barometer():
    bridge = read("components/device_status_pod_bridge/app_pod_status_bridge.c")
    cmake = read("components/device_status_pod_bridge/CMakeLists.txt")

    assert "device_status_set_pod_local_position(" in bridge
    assert "app_pod_status_bridge_compute_local_position_reading(" in bridge
    assert "object_store" in cmake


def test_active_object_profile_is_fetched_once_per_cycle_not_per_channel():
    """object_store_get_active()/get_profile() are per-object, not
    per-channel; fetching them inside the per-channel loop would mean 4
    redundant lookups (and 4 chances to race with the active object
    changing mid-cycle) for what must be one consistent snapshot."""
    bridge = read("components/device_status_pod_bridge/app_pod_status_bridge.c")

    cycle_start = bridge.index("static void publish_cycle(void)")
    loop_start = bridge.index("for (", cycle_start)
    fetch_section = bridge[cycle_start:loop_start]

    assert "object_store_get_active(" in fetch_section
    assert "object_store_get_profile(" in fetch_section
    assert "object_store_get_active(" not in bridge[loop_start:]
    assert "object_store_get_profile(" not in bridge[loop_start:]


def test_no_active_object_never_falls_back_to_stale_profile():
    """has_profile gates every field the reading depends on; a failed or
    absent active-object lookup must zero the profile, not reuse whatever
    the previous cycle (or the stack) happened to leave behind."""
    bridge = read("components/device_status_pod_bridge/app_pod_status_bridge.c")

    cycle_start = bridge.index("static void publish_cycle(void)")
    cycle_end = bridge.index("\n}\n", cycle_start)
    cycle_body = bridge[cycle_start:cycle_end]

    assert "memset(&profile, 0, sizeof(profile));" in cycle_body
    assert "active.active" in cycle_body
    assert "active.id[0] != '\\0'" in cycle_body
