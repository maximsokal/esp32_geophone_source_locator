"""Source contracts for the localization run record schema (MAX-140)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_schema_is_versioned_and_rejects_unknown_future_versions():
    header = read("components/localization_results/include/localization_run_record.h")
    source = read("components/localization_results/localization_run_record_json.c")

    assert "LOCALIZATION_RUN_SCHEMA_VERSION" in header
    assert "ESP_ERR_NOT_FOUND" in source  # unknown schema_version -> not silently accepted


def test_component_owns_no_file_io():
    """This is pure serialize/parse; storage path/versioning conventions
    belong to whatever caller writes the JSON (MAX-166/167/168), not here."""
    source = read("components/localization_results/localization_run_record_json.c")
    cmake = read("components/localization_results/CMakeLists.txt")

    assert "storage_manager" not in source
    assert "storage_manager" not in cmake
    assert "fopen(" not in source


def test_unsolved_records_always_carry_a_reason_never_a_stale_position():
    source = read("components/localization_results/localization_run_record_json.c")
    header = read("components/localization_results/include/localization_run_record.h")

    assert "rejection_reason" in header
    # to_json refuses to serialize an unsolved record with an empty reason
    assert "rejection_reason[0] == '\\0'" in source
    # source_position is only written into the JSON when solved is true
    to_json = source[: source.index("static bool read_string")]
    assert "record->solved" in to_json


def test_persists_the_channel_mask_and_dimensionality_from_max_135_and_max_170():
    header = read("components/localization_results/include/localization_run_record.h")

    assert "channel_mask_used" in header
    assert "channel_mask_rejected" in header
    assert "localization_dimensionality_t dimensionality;" in header


def test_ties_the_run_to_the_frozen_max_166_geometry_snapshot():
    header = read("components/localization_results/include/localization_run_record.h")
    assert "geometry_computed_at_ms" in header


def test_confidence_score_lets_non_iterative_methods_persist_their_own_quality():
    """v2 could only cleanly represent the iterative Gauss-Newton TDOA
    solver's own shape (iterations/converged) -- semblance (MAX-137),
    traveltime inversion (MAX-136), and the joint fusion (MAX-138) have no
    equivalent and need a generic quality field instead."""
    header = read("components/localization_results/include/localization_run_record.h")
    source = read("components/localization_results/localization_run_record_json.c")

    assert "double confidence_score;" in header
    assert '"confidence_score"' in source
    assert "confidence_score < 0.0" in source
