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
        "localization_extended",
        "localization_results",
        "localization_run_store",
        "localization_semblance",
        "localization_traveltime_inversion",
        "localization_velocity_free",
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
    # joint_record->solved (it lives in the heap workspace).
    joint_write_index = source.index("localization_run_store_write(object_id, session_id, joint_record);")
    solved_check_index = source.index("if (joint_record->solved)")
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
    write_index = source.index("localization_run_store_write_picks(object_id, session_id, pick_record)")
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
    assert '"traveltime_scan_constrained"' in source
    assert '"semblance_scan_constrained"' in source
    # Fusion only ever sees the unconstrained records.
    assert "comparison_methods, fused_record_count, comparison_result" in source
    fused_index = source.index("const size_t fused_record_count = record_count;")
    constrained_index = source.index('"traveltime_scan_constrained"')
    assert fused_index < constrained_index


def test_velocity_free_methods_run_on_the_same_picks_and_are_fused_once():
    """TDCAS, PSAFUVS and SUEA are persisted as their own records before the
    fused prefix ends; the policy puts them in the shared P-pick evidence
    group so the joint fusion never counts the same picks twice."""
    source = orchestrator_source()
    fused_index = source.index("const size_t fused_record_count = record_count;")
    for call in (
        "localization_tdcas_solve(p_observations, p_observation_count, NULL, tdcas_result)",
        "localization_psafuvs_solve(",
        "localization_suea_locate(",
    ):
        assert source.index(call) < fused_index, call
    for adapter in (
        "localization_calculate_tdcas_to_run_record(",
        "localization_calculate_psafuvs_to_run_record(",
        "localization_calculate_suea_to_run_record(",
    ):
        assert source.index(adapter) < fused_index, adapter

    policy = read("components/localization_calculate/localization_calculate_policy.c")
    group_block = policy[policy.index("MAX-212: TDOA, traveltime inversion, the velocity-free methods"):]
    group_block = group_block[:group_block.index("LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP")]
    for name in ('"tdcas"', '"psafuvs"', '"suea"'):
        assert name in group_block


def test_velocity_free_scan_variants_are_constrained_and_never_fused():
    source = orchestrator_source()
    fused_index = source.index("const size_t fused_record_count = record_count;")
    for name in ('"tdcas_scan_constrained"', '"psafuvs_scan_constrained"', '"suea_scan_voids"'):
        assert source.index(name) > fused_index, name
    # Straight-ray methods get both the FREE-air candidate filter and the
    # void ray screening; SUEA gets the scan-classified grid.
    assert ".clear = localization_calculate_scan_ray_clear," in source
    assert "constrained_config.ray_filter.clear = localization_calculate_scan_ray_clear;" in source
    assert "localization_suea_grid_classify(suea_grid, localization_calculate_scan_suea_cell, &occupancy);" in source


def test_s_picks_are_explicit_caller_configuration():
    """MAX-132: PSAFUVS needs S picks, and the S picker's windows/thresholds
    come only from the caller; without them PSAFUVS is persisted as not
    attempted with a reason."""
    header = read("components/localization_calculate/include/localization_calculate.h")
    assert "bool s_pick_enabled;" in header
    assert "geophone_secondary_pick_config_t s_pick_config;" in header
    source = orchestrator_source()
    assert "config->s_pick_enabled" in source
    assert "&config->s_pick_config" in source
    assert "S-wave picking not configured" in source


def test_suea_prior_uses_the_callers_velocity_and_uncertainty():
    source = orchestrator_source()
    assert ".prior_velocity_mps = config->propagation_speed_mps," in source
    assert ".prior_velocity_sigma_mps = config->propagation_speed_uncertainty_mps," in source


def test_run_state_lives_on_the_heap_not_the_https_stack():
    """The orchestrator runs on the 8 KiB HTTPS task: the records and fusion
    state are one heap workspace, allocated before the first write and freed
    on the way out."""
    source = orchestrator_source()
    assert "localization_calculate_workspace_t *workspace = heap_caps_calloc(" in source
    assert "localization_run_record_t *records = workspace->records;" in source
    assert source.index("heap_caps_calloc(") < source.index("localization_run_store_write_picks(")
    assert source.rstrip().endswith("heap_caps_free(workspace);\n    return first_write_err;\n}")


def test_obstructed_sensor_masks_are_mapped_back_to_channels():
    """TDCAS/PSAFUVS report obstructed sensors by OBSERVATION index; the
    observation arrays skip channels without picks, so the run record's
    channel masks must go through the explicit observation -> channel map."""
    source = orchestrator_source()
    assert "workspace->p_channel[p_observation_count] = ch;" in source
    assert "workspace->ps_channel[ps_observation_count] = ch;" in source
    assert "constrained_tdcas->sensor_mask_obstructed, workspace->p_channel, p_observation_count" in source
    assert "constrained_psafuvs->sensor_mask_obstructed, workspace->ps_channel, ps_observation_count" in source
    assert "~constrained_tdcas->sensor_mask_obstructed" not in source
    assert "~constrained_psafuvs->sensor_mask_obstructed" not in source


def test_extended_methods_run_on_the_same_picks_and_are_fused_once():
    """Cross-correlation TDOA, the robust consensus, PSP, the Bayesian
    posterior and eikonal location are persisted before the fused prefix
    ends, and share the P-pick evidence group."""
    source = orchestrator_source()
    fused_index = source.index("const size_t fused_record_count = record_count;")
    for call in (
        "localization_xcorr_measure_pairs(",
        "localization_xcorr_tdoa_locate(",
        "localization_robust_solve(",
        "localization_psp_solve(",
        "localization_bayes_locate(",
        "localization_eikonal_locate(",
    ):
        assert source.index(call) < fused_index, call
    for adapter in (
        "localization_calculate_xcorr_tdoa_to_run_record(",
        "localization_calculate_robust_to_run_record(",
        "localization_calculate_psp_to_run_record(",
        "localization_calculate_bayes_to_run_record(",
        "localization_calculate_eikonal_to_run_record(",
    ):
        assert source.index(adapter) < fused_index, adapter

    policy = read("components/localization_calculate/localization_calculate_policy.c")
    group_block = policy[policy.index("MAX-212: TDOA, traveltime inversion, the velocity-free methods"):]
    group_block = group_block[:group_block.index("LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP")]
    for name in ('"xcorr_tdoa"', '"robust_tdoa"', '"psp_joint"', '"eikonal_fmm"', '"bayesian"'):
        assert name in group_block
    for name in ('"eikonal_scan_voids"', '"bayesian_scan_voids"', '"semblance_eikonal_voids"'):
        assert name not in group_block


def test_eikonal_scan_variants_use_the_scan_voids_and_are_never_fused():
    """The scan's voids enter as an impassable velocity model; the same
    eikonal fields drive location, semblance focusing and the posterior,
    each with the FREE-air candidate filter, all after the fused prefix."""
    source = orchestrator_source()
    fused_index = source.index("const size_t fused_record_count = record_count;")
    for name in ('"eikonal_scan_voids"', '"semblance_eikonal_voids"', '"bayesian_scan_voids"'):
        assert source.index(name) > fused_index, name
    assign = source.index("localization_calculate_scan_eikonal_velocity")
    assert assign > fused_index
    assert ".sample_radius_m = eikonal_model_err == ESP_OK ? 0.5 * eikonal_model->cell_m : 0.0," in source
    assert "eikonal_semblance_config.travel_time_model.travel_time = localization_eikonal_travel_time;" in source
    assert "scan_config.travel_time_model.travel_time = localization_eikonal_travel_time;" in source
    # Nothing leaks: fields after the scan block, the model at the end.
    assert "localization_eikonal_fields_free(&workspace->eikonal_fields);" in source
    assert "localization_eikonal_model_free(eikonal_model);" in source


def test_extended_methods_take_the_callers_velocity_and_uncertainty():
    """Robust / xcorr use the caller's velocity; PSP, eikonal and the
    posterior take its uncertainty as their velocity prior (0 = fixed)."""
    source = orchestrator_source()
    assert ".p_velocity_sigma_mps = config->propagation_speed_uncertainty_mps," in source
    assert ".velocity_sigma_mps = config->propagation_speed_uncertainty_mps," in source
    assert ".velocity_scale_sigma = config->propagation_speed_uncertainty_mps / config->propagation_speed_mps," in source
    assert "robust_config.propagation_speed_mps = config->propagation_speed_mps;" in source
    assert ".likelihood = LOCALIZATION_BAYES_STUDENT_T," in source


def test_extended_budgets_are_documented_constants():
    source = orchestrator_source()
    for define in (
        "#define LOCALIZATION_CALCULATE_EIKONAL_MAX_NODES",
        "#define LOCALIZATION_CALCULATE_BAYES_MAX_CELLS",
        "#define LOCALIZATION_CALCULATE_BAYES_VELOCITY_NODES",
        "#define LOCALIZATION_CALCULATE_SEED_LATTICE_POINTS",
    ):
        assert define in source
    budget = source.index("#define LOCALIZATION_CALCULATE_EIKONAL_MAX_NODES")
    assert "not physics" in source[max(0, budget - 300):budget]
    assert "#define LOCALIZATION_CALCULATE_MAX_RECORDS 20U" in source
