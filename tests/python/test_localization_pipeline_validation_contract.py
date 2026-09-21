"""Source contract for the synthetic pipeline validation harness (MAX-141)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_keeps_synthetic_evidence_explicitly_distinct_from_physical_hil():
    """The ticket's own explicit requirement: 'Keep synthetic/host evidence
    distinct from physical HIL/field evidence.'"""
    source = read("tests/host/localization_pipeline_validation_test.c")
    assert "synthetic" in source.lower()
    assert "NOT physical HIL" in source or "not physical hil" in source.lower()


def test_covers_the_required_scenario_matrix():
    source = read("tests/host/localization_pipeline_validation_test.c")

    assert "test_clean_high_resolution_4_sensor_converges_accurately" in source
    assert "test_realistic_500hz_quantization_bounds_error" in source
    assert "test_exactly_3_sensors_reports_planar_2d_unavailable_for_3d_method" in source
    assert "test_missing_channel_is_rejected_not_a_false_position" in source
    assert "test_propagation_speed_error_sensitivity_is_monotonic" in source


def test_noise_is_deterministic_pseudo_random_not_a_periodic_signal():
    """A pure sine as background noise is pathological for an STA/LTA
    energy-ratio detector at small window sizes (a short window's energy
    depends on which part of the periodic cycle it samples). This must stay
    a deterministic PRNG, not a sinusoid, and the seed must be fixed for
    reproducibility."""
    source = read("tests/host/localization_pipeline_validation_test.c")
    assert "xorshift32" in source.lower() or "s_noise_state" in source
    assert "0x9E3779B9" in source  # fixed seed


def test_sensitivity_check_asserts_monotonic_growth_not_just_a_single_bound():
    source = read("tests/host/localization_pipeline_validation_test.c")
    assert "test_propagation_speed_error_sensitivity_is_monotonic" in source
    assert "error_exact < error_mild" in source
    assert "error_mild < error_large" in source
