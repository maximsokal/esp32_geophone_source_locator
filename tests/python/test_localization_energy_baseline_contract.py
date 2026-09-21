"""Source contracts for the energy-only spatial baseline (MAX-138 part 1/4)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_needs_no_timing_or_propagation_speed():
    """The whole point of this baseline is being usable when the timing-
    based methods can't run -- it must not secretly need arrival times or a
    propagation speed."""
    header = read(
        "components/localization_energy_baseline/include/localization_energy_baseline.h"
    )
    source = read(
        "components/localization_energy_baseline/localization_energy_baseline.c"
    )

    assert "arrival_time" not in header.lower()
    assert "propagation_speed" not in header.lower()
    assert "arrival_time" not in source.lower()
    assert "propagation_speed" not in source.lower()


def test_bad_channels_can_be_excluded():
    source = read(
        "components/localization_energy_baseline/localization_energy_baseline.c"
    )
    assert "channels[i].weight <= 0.0" in source


def test_negative_energy_is_rejected_not_silently_accepted():
    source = read(
        "components/localization_energy_baseline/localization_energy_baseline.c"
    )
    assert "channels[i].energy < 0.0" in source


def test_is_a_real_weighted_centroid_not_a_stub():
    source = read(
        "components/localization_energy_baseline/localization_energy_baseline.c"
    )
    assert "weighted_energy * channels[i].sensor_position.x_m" in source
    assert "sum_x / total_weighted_energy" in source
