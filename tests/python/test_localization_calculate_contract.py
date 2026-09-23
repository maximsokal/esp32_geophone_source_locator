"""Source contracts for MAX-168's orchestrator (localization_calculate.c):
composes every localization backend against one session's frozen geometry
and raw samples, fails closed before any persistence, and never invents a
physically-measured parameter."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPONENT_DIR = REPO_ROOT / "components" / "localization_calculate"


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def orchestrator_source() -> str:
    return read("components/localization_calculate/localization_calculate.c")


def test_component_is_registered_in_cmake():
    cmake = read("components/localization_calculate/CMakeLists.txt")
    assert "localization_calculate_policy.c" in cmake
    assert "localization_calculate.c" in cmake
    for required in (
        "geophone_events",
        "localization",
        "localization_comparison",
        "localization_energy_baseline",
        "localization_results",
        "localization_run_store",
        "localization_semblance",
        "localization_traveltime_inversion",
        "object_store",
        "seismic_dsp",
        "seismic_recorder",
    ):
        assert required in cmake, f"missing REQUIRES entry: {required}"


def test_run_never_invents_a_physically_measured_parameter():
    source = orchestrator_source()

    # MAX-132: propagation speed, propagation uncertainty and timing
    # uncertainty must come from the caller's config, never a literal
    # default assigned to the solver-facing config structs.
    validate_start = source.index("esp_err_t localization_calculate_run(")
    validate_block = source[validate_start:source.index("object_store_geometry_t geometry;")]
    assert "config->propagation_speed_mps > 0.0" in validate_block
    assert "config->propagation_speed_uncertainty_mps >= 0.0" in validate_block
    assert "config->timing_uncertainty_us > 0.0" in validate_block

    assert "tdoa_config.propagation_speed_mps = config->propagation_speed_mps;" in source
    assert ".propagation_speed_mps = config->propagation_speed_mps," in source
    assert ".timing_uncertainty_us = config->timing_uncertainty_us," in source


def test_fails_closed_before_any_run_store_write():
    source = orchestrator_source()

    dimensionality_check = source.index("LOCALIZATION_DIMENSIONALITY_INSUFFICIENT")
    raw_read_call = source.index("seismic_recorder_read_session_raw(")
    first_write = source.index("localization_run_store_write(")

    # Every fail-closed gate (geometry read, dimensionality, raw sample
    # read/capacity) must appear in the source before the first persisted
    # write -- once we start writing, individual method failures are
    # reported as unsolved records, not aborted.
    assert dimensionality_check < first_write
    assert raw_read_call < first_write


def test_every_individual_method_is_persisted_whether_solved_or_not():
    source = orchestrator_source()

    # The four individual-method records are written unconditionally (no
    # "if (record.solved)" guard around the persistence loop) -- an
    # unsolved run with a reason must still reach the run store, per the
    # ticket's "preserve unavailable/rejected methods with a reason"
    # requirement.
    persist_loop_start = source.index("for (size_t i = 0U; i < record_count; ++i) {\n        const esp_err_t write_err")
    persist_loop = source[persist_loop_start:persist_loop_start + 400]
    assert "if (records[i].solved)" not in persist_loop
    assert "localization_run_store_write(object_id, session_id, &records[i]);" in persist_loop

    # The joint fusion record is also always written, regardless of
    # joint_record.solved.
    joint_write_index = source.index("localization_run_store_write(object_id, session_id, &joint_record);")
    solved_check_index = source.index("if (joint_record.solved)")
    assert solved_check_index < joint_write_index


def test_pick_status_never_defaults_to_a_silently_accepted_zero_value():
    source = orchestrator_source()

    # GEOPHONE_PICK_ACCEPTED is enum value 0: a memset-zeroed pick struct
    # must never be mistaken for an accepted one. Both the "not
    # participating" and "participating" branches must explicitly set a
    # rejected default before any pick is possibly left unset.
    assert source.count("event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;") >= 2


def test_traveltime_search_is_streaming_and_surface_never_leaks():
    """MAX-208: the orchestrator never asks for the per-candidate surface
    (it was ~49 MiB for a 5x5 m 3D array and was never persisted); the
    result's surface is NULL then, and free(NULL) keeps the cleanup
    unconditional and leak-proof if that ever changes."""
    source = orchestrator_source()
    assert ".retain_surface = false," in source
    assert "free(traveltime_result.surface);" in source
    # The result is zero-initialized before the call, so an early error
    # return can never leave a garbage pointer to free.
    assert "memset(&traveltime_result, 0, sizeof(traveltime_result));" in source


def test_max_frames_bound_is_documented_not_guessed():
    source = orchestrator_source()
    define_index = source.index("#define LOCALIZATION_CALCULATE_MAX_FRAMES")
    comment_window = source[max(0, define_index - 500):define_index]
    assert "500 Hz" in comment_window or "MAX-141" in comment_window


def test_pick_evidence_persistence_is_best_effort_not_gating():
    source = orchestrator_source()

    # MAX-148 slice 3: a pick-record write failure must never be checked /
    # propagated -- it is diagnostic context, not part of the run's own
    # solved/unsolved outcome or the recording-gate's admission promise.
    write_index = source.index("localization_run_store_write_picks(object_id, session_id, &pick_record)")
    call_line_start = source.rindex("\n", 0, write_index) + 1
    call_line_end = source.index("\n", write_index)
    call_line = source[call_line_start:call_line_end]
    assert call_line.strip().startswith("(void)")

    # The pick record must be built (and therefore written) before any
    # method-record persistence begins, so a WebUI reading "why was channel
    # N rejected" always has evidence for every run sharing that
    # computed_at_ms -- never a race where picks lag behind the runs that
    # depended on them.
    first_run_write = source.index("localization_run_store_write(object_id, session_id, &records[i]);")
    assert write_index < first_run_write


def test_scan_constraint_is_session_scoped_fail_closed_and_never_fused():
    """MAX-147: occupancy comes only from THIS session's directory, applies
    only with a registration bound to this session's frozen geometry, and the
    constrained variants are persisted beside -- never fused into -- the
    unconstrained results."""
    source = orchestrator_source()
    assert '"%s/%s/sessions/%s/occupancy-v1.bin"' in source
    assert "scan_occupancy_registration_usable(&occupancy, geometry_computed_at_ms)" in source
    assert "constraint not applied" in source
    assert '"traveltime_inversion_scan_constrained"' in source
    assert '"semblance_scan_constrained"' in source
    # Fusion only ever sees the unconstrained records.
    assert "comparison_methods, fused_record_count, &comparison_result" in source
    fused_index = source.index("const size_t fused_record_count = record_count;")
    constrained_index = source.index('"traveltime_inversion_scan_constrained"')
    assert fused_index < constrained_index
