"""Source contracts for the TDOA event-bridge -> run-record adapter (MAX-135/MAX-140)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_is_a_pure_translation_layer_with_no_file_io():
    source = read("components/localization_results/localization_event_run_record.c")
    cmake = read("components/localization_results/CMakeLists.txt")

    assert "storage_manager" not in source
    assert "storage_manager" not in cmake
    assert "fopen(" not in source


def test_reuses_the_shared_dimensionality_policy_reasons_not_bespoke_text():
    """Rejection reasons for insufficient-channels / requires-3D must come
    from the one shared policy (MAX-170), not a duplicated string that
    could drift out of sync with it."""
    source = read("components/localization_results/localization_event_run_record.c")

    assert "localization_method_availability_reason(" in source
    assert "LOCALIZATION_METHOD_SKIPPED_INSUFFICIENT_SENSORS" in source
    assert "LOCALIZATION_METHOD_SKIPPED_REQUIRES_3D" in source


def test_confidence_score_uses_the_documented_gaussian_recipe():
    """Must match the exact convention localization_comparison's header
    documents for this method family, not an independently-invented one."""
    source = read("components/localization_results/localization_event_run_record.c")

    assert "timing_uncertainty_us > 0.0" in source
    assert "exp(-0.5 * normalized * normalized)" in source


def test_rejection_reason_buffer_was_widened_for_a_real_truncation_bug():
    """Regression guard for a real bug: localization_method_availability_
    reason()'s requires-3D string is 105 chars + NUL, once longer than
    this field's 96-byte buffer -- caught only once
    localization_event_run_record_test.c actually round-tripped the real
    policy string through this field (via copy_bounded, which fails
    closed on truncation) instead of assuming it would fit. The C host
    test is the faithful check (it calls the real function and compares
    the real string); this just guards the buffer size wasn't shrunk back."""
    header = read("components/localization_results/include/localization_run_record.h")
    assert "LOCALIZATION_RUN_REJECTION_REASON_SIZE 128U" in header
