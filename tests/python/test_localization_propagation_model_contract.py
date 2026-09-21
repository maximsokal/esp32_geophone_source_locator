"""Source contracts for the propagation-model calibration tool (MAX-139)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_calibration_is_a_real_least_squares_fit_not_a_lookup_table():
    """No named/region velocity may be hardcoded as a default -- every
    value must come from the caller's own tap/impact data."""
    source = read("components/localization/localization_propagation_model.c")
    header = read("components/localization/include/localization_propagation_model.h")

    assert "sum_xy" in source and "sum_xx" in source  # real regression, not a lookup
    assert "default_config" not in source
    assert "default_config" not in header


def test_requires_at_least_three_samples_for_a_meaningful_uncertainty():
    source = read("components/localization/localization_propagation_model.c")
    assert "sample_count < 3u" in source


def test_solver_files_stay_unaware_of_the_propagation_model_tool():
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "propagation_model" not in solver.lower()
    assert "propagation_model" not in solver_header.lower()


def test_velocity_sensitivity_uses_the_real_solver_not_a_closed_form():
    source = read("components/localization/localization_propagation_model.c")
    assert source.count("localization_tdoa_solve(") == 2


def test_run_record_persists_model_name_and_uncertainty():
    """MAX-140's acceptance: 'saved localization result records the
    velocity/model used and uncertainty.'"""
    header = read("components/localization_results/include/localization_run_record.h")

    assert "propagation_model_name" in header
    assert "propagation_speed_uncertainty_mps" in header
    assert "localization_propagation_model.h" in header
