"""Source contracts for the semblance/coherency location backend (MAX-137)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_is_independent_of_the_tdoa_solver():
    """MAX-137's own requirement: a separate backend so a report/WebUI can
    show both methods' results side by side, not a wrapper around the
    existing Gauss-Newton solver. The header's doc comment names
    localization_tdoa_solve() only to explain that relationship in prose --
    the actual implementation must never call it."""
    source = read("components/localization_semblance/localization_semblance.c")

    assert "localization_tdoa_solve(" not in source


def test_solver_files_stay_unaware_of_the_semblance_backend():
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "semblance" not in solver.lower()
    assert "semblance" not in solver_header.lower()


def test_propagation_speed_is_always_caller_supplied():
    """No named/region velocity may be hardcoded -- same rule as the TDOA
    propagation model tool (MAX-139)."""
    source = read("components/localization_semblance/localization_semblance.c")
    header = read("components/localization_semblance/include/localization_semblance.h")

    assert "default_config" not in source
    assert "default_config" not in header
    assert "propagation_speed_mps <= 0.0" in source


def test_reference_sample_index_is_never_invented():
    header = read("components/localization_semblance/include/localization_semblance.h")
    assert "never invent" in header.lower()


def test_uses_the_real_neidell_taner_semblance_formula():
    """Guards against a fake/simplified stand-in: the formula is
    (sum over time of the cross-channel stack)^2, divided by
    (channel_count * sum over channels and time of value^2)."""
    source = read("components/localization_semblance/localization_semblance.c")

    assert "sum_stack_squares" in source
    assert "sum_squares_all" in source
    assert "(double)channel_count * sum_squares_all" in source


def test_search_is_coarse_to_fine_not_a_single_uniform_grid():
    source = read("components/localization_semblance/localization_semblance.c")

    assert source.count("search_grid(") >= 3  # 1 definition + 2 call sites
    assert "refine_divisor" in source


def test_no_file_io():
    source = read("components/localization_semblance/localization_semblance.c")

    for banned in ("fopen", "fread", "fwrite"):
        assert banned not in source
