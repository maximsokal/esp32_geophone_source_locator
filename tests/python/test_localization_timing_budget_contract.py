"""Source contracts for the timing-jitter error budget tool (MAX-142)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_never_invents_a_measured_jitter_value():
    """jitter_us must always be a caller-supplied parameter -- this tool
    computes what a given jitter *costs*, it never guesses what the real
    hardware's jitter actually is (that needs a real measurement)."""
    header = read("components/localization/include/localization_timing_budget.h")
    source = read("components/localization/localization_timing_budget.c")

    assert "int64_t jitter_us" in header
    assert "jitter_us =" not in source  # never assigned a default/guessed value


def test_solver_files_stay_unaware_of_timing_budget_tooling():
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "jitter" not in solver.lower()
    assert "jitter" not in solver_header.lower()


def test_is_an_empirical_perturbation_not_a_claimed_closed_form():
    source = read("components/localization/localization_timing_budget.c")
    header = read("components/localization/include/localization_timing_budget.h")

    # calls the real nonlinear solver twice (baseline + perturbed), rather
    # than a linearized/closed-form Jacobian approximation
    assert source.count("localization_tdoa_solve(") == 2
    assert "empirical" in header.lower()
