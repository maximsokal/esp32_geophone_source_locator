"""Source contracts for the multi-method comparison/fusion layer (MAX-138 part 4/4)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_is_decoupled_from_every_individual_backend():
    """This layer must stay a generic fuser: it must not #include any of
    the specific backends it is meant to combine, the same way the TDOA
    solver stays unaware of every backend built on top of it."""
    header = read("components/localization_comparison/include/localization_comparison.h")
    source = read("components/localization_comparison/localization_comparison.c")

    for backend_header in (
        "localization_semblance.h",
        "localization_traveltime_inversion.h",
        "localization_energy_baseline.h",
    ):
        assert backend_header not in header
        assert backend_header not in source


def test_never_overwrites_individual_method_outcomes():
    """MAX-138's own requirement: 'never overwrite the individual outputs'."""
    source = read("components/localization_comparison/localization_comparison.c")
    assert "memcpy(result->methods, methods" in source


def test_low_quality_method_is_excluded_from_the_joint_weight_not_clamped_up():
    source = read("components/localization_comparison/localization_comparison.c")
    assert "methods[i].quality <= 0.0" in source


def test_disagreement_between_methods_is_a_concrete_reported_value():
    header = read("components/localization_comparison/include/localization_comparison.h")
    assert "max_pairwise_disagreement_m" in header


def test_unsolved_entries_must_carry_a_reason_and_solved_entries_a_finite_position():
    source = read("components/localization_comparison/localization_comparison.c")
    assert "methods[i].rejection_reason[0] == '\\0'" in source
    assert "isfinite(methods[i].position.x_m)" in source


def test_no_file_io():
    source = read("components/localization_comparison/localization_comparison.c")
    for banned in ("fopen", "fread", "fwrite"):
        assert banned not in source
