"""Source contracts for the statistical traveltime-inversion backend (MAX-136)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_is_independent_of_the_tdoa_solver():
    """MAX-136's own requirement: an independent backend so a report/WebUI
    can compare more than one source-location mathematics path, not a
    wrapper around the existing Gauss-Newton solver."""
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )

    assert "localization_tdoa_solve(" not in source


def test_solver_files_stay_unaware_of_the_traveltime_inversion_backend():
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "traveltime_inversion" not in solver.lower()
    assert "traveltime_inversion" not in solver_header.lower()


def test_feeds_the_same_observation_type_as_the_existing_solver():
    """'feed the same event picks/sensor geometry used by the existing
    Gauss-Newton TDOA solver' -- this backend must wrap, not reinvent, the
    solver's own localization_observation_t."""
    header = read(
        "components/localization_traveltime_inversion/include/"
        "localization_traveltime_inversion.h"
    )

    assert '#include "localization.h"' in header
    assert "localization_observation_t observation;" in header


def test_propagation_speed_and_timing_uncertainty_are_always_caller_supplied():
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )
    header = read(
        "components/localization_traveltime_inversion/include/"
        "localization_traveltime_inversion.h"
    )

    assert "default_config" not in source
    assert "default_config" not in header
    assert "propagation_speed_mps <= 0.0" in source
    assert "timing_uncertainty_us <= 0.0" in source


def test_bad_picks_can_be_excluded_or_down_weighted():
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )
    assert "picks[i].weight > 0.0" in source


def test_uses_the_project_wide_dimensionality_policy_not_a_bespoke_threshold():
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )
    header = read(
        "components/localization_traveltime_inversion/include/"
        "localization_traveltime_inversion.h"
    )

    assert "localization_dimensionality_classify(" in source
    assert '#include "localization_dimensionality_policy.h"' in header


def test_retains_a_residual_likelihood_surface_not_only_a_point_estimate():
    """MAX-136's own acceptance criterion: uncertainty/likelihood or
    residual surface is retained, not only a point estimate."""
    header = read(
        "components/localization_traveltime_inversion/include/"
        "localization_traveltime_inversion.h"
    )

    assert "localization_traveltime_candidate_t *surface;" in header
    assert "size_t surface_count;" in header


def test_search_is_coarse_to_fine_not_a_single_uniform_grid():
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )

    assert source.count("evaluate_candidate(") >= 3  # 1 definition + 2 call sites
    assert "refine_divisor" in source


def test_likelihood_is_normalized_relative_to_the_global_best_not_absolute():
    """Guards against the real underflow bug found during development: a raw
    exp(-0.5*chi-square) collapses to indistinguishable zeros across nearly
    the whole grid at realistic timing precision, silently picking whichever
    candidate happened to be evaluated first instead of the true best."""
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )

    assert "chi_square[i] - best_chi_square" in source
    # Ranking uses the raw chi-square (streaming pass, MAX-208).
    assert "candidate_chi_square < best_chi_square" in source


def test_no_file_io():
    source = read(
        "components/localization_traveltime_inversion/localization_traveltime_inversion.c"
    )

    for banned in ("fopen", "fread", "fwrite"):
        assert banned not in source
