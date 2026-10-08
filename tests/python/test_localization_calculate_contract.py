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
        "power_control",
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
    run_source = source[source.index("esp_err_t localization_calculate_run("):]

    dimensionality_check = run_source.index("LOCALIZATION_DIMENSIONALITY_INSUFFICIENT")
    range_read_call = run_source.index("seismic_recorder_read_session_range_ex(")
    first_write = run_source.index("localization_run_store_write(")

    # Every fail-closed gate (geometry read, dimensionality, raw sample
    # read/capacity) must appear in the source before the first persisted
    # write -- once we start writing, individual method failures are
    # reported as unsolved records, not aborted.
    assert dimensionality_check < first_write
    assert range_read_call < first_write


def test_every_individual_method_is_persisted_whether_solved_or_not():
    source = orchestrator_source()

    # The four individual-method records are written unconditionally (no
    # "if (record.solved)" guard around the persistence loop) -- an
    # unsolved run with a reason must still reach the run store, per the
    # ticket's "preserve unavailable/rejected methods with a reason"
    # requirement.
    persist_loop_start = source.index("/* --- Persist every individual method's own record")
    persist_loop_end = source.index("/* --- Joint quality-weighted fusion", persist_loop_start)
    persist_loop = source[persist_loop_start:persist_loop_end]
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
    policy = read("components/localization_calculate/localization_calculate_policy.c")
    kconfig = read("main/Kconfig.projbuild")

    # MAX-299 bounds the window by time, configured budget, and live PSRAM.
    assert "localization_calculate_analysis_window_frames(" in source
    assert "CONFIG_LOCALIZATION_MAX_ANALYSIS_S" in source
    assert "CONFIG_LOCALIZATION_ANALYSIS_PSRAM_BUDGET_KB" in source
    assert "config LOCALIZATION_MAX_ANALYSIS_S" in kconfig
    assert "config LOCALIZATION_ANALYSIS_PSRAM_BUDGET_KB" in kconfig
    assert "largest_free_block_bytes" in policy
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)" in source
    assert "LOCALIZATION_ANALYSIS_PSRAM_SAFETY_RESERVE_BYTES" in source
    assert '"insufficient_memory"' in source
    assert '"memory"' in source
    assert "session_summary.samples_written > (uint64_t)frame_count" in source
    assert "records[i].analysis_window_start_index = 0U" in source
    assert "LOCALIZATION_ANALYSIS_PSRAM_SAFETY_RESERVE_BYTES (3072U * 1024U)" in source
    assert "max_window_frames < minimum_useful_frames" in source


def test_pick_evidence_persistence_retries_once_then_reports_after_records_saved():
    source = orchestrator_source()

    helper_start = source.index("static esp_err_t localization_calculate_write_picks_retry(")
    helper_end = source.index("\n}\n", helper_start) + 2
    helper = source[helper_start:helper_end]
    write_call = "localization_run_store_write_picks(object_id, session_id, record)"
    assert helper.count(write_call) == 2
    first_write = helper.index(f"esp_err_t err = {write_call};")
    retry_guard = helper.index("if (err != ESP_OK) {")
    retry_write = helper.rindex(f"err = {write_call};")
    return_err = helper.index("return err;")
    assert first_write < retry_guard < retry_write < return_err
    assert "(void)localization_run_store_write_picks" not in source

    run_start = source.index("esp_err_t localization_calculate_run(")
    pick_assignment = (
        "pick_write_err = localization_calculate_write_picks_retry(\n"
        "        object_id, session_id, pick_record\n"
        "    );"
    )
    pick_call_index = source.index(pick_assignment, run_start)

    # The pick record must be built (and therefore written) before any
    # method-record persistence begins, so a WebUI reading "why was channel
    # N rejected" always has evidence for every run sharing that
    # computed_at_ms -- never a race where picks lag behind the runs that
    # depended on them.
    first_run_write = source.index(
        "localization_run_store_write(object_id, session_id, &records[i]);",
        run_start,
    )
    assert pick_call_index < first_run_write

    joint_write_index = source.index(
        "const esp_err_t joint_write_err = localization_run_store_write(object_id, session_id, joint_record);",
        run_start,
    )
    error_merge = (
        "if (first_write_err == ESP_OK && pick_write_err != ESP_OK) {\n"
        "        first_write_err = pick_write_err;\n"
        "    }"
    )
    error_merge_index = source.index(error_merge, run_start)
    returned_error_index = source.index("return first_write_err;", error_merge_index)
    assert joint_write_index < error_merge_index < returned_error_index


def test_scan_constraint_is_session_scoped_fail_closed_and_never_fused():
    """MAX-147: occupancy comes only from THIS session's directory, applies
    only with a registration bound to this session's frozen geometry, and the
    constrained variants are persisted beside -- never fused into -- the
    unconstrained results."""
    source = orchestrator_source()
    assert '"%s/%s/sessions/%s/occupancy-v1.bin"' in source
    assert "scan_occupancy_registration_reason(&occupancy, geometry_computed_at_ms)" in source
    assert "scan_identity_err == ESP_OK && strcmp(occupancy.frame_id, current_scan_frame_id) == 0;" in source
    assert "const bool usable = scan_identity_matches && registration_reason == NULL;" in source
    assert "constraint not applied" in source
    assert '"traveltime_scan_constrained"' in source
    assert '"semblance_scan_constrained"' in source
    # Fusion only ever sees the unconstrained records.
    assert "comparison_methods, fused_record_count, comparison_result" in source
    fused_index = source.index("const size_t fused_record_count = record_count;")
    constrained_index = source.index('"traveltime_scan_constrained"')
    assert fused_index < constrained_index


def test_scan_constraints_require_the_current_whole_file_generation():
    source = orchestrator_source()
    assert '"%s/%s/sessions/%s/scan-v1.bin"' in source
    assert "0xEDB88320U" in source
    assert '"scan-crc32:%08x"' in source
    assert "storage_bulk_admission_run_chunk" in source
    assert "power_control_is_shutdown_requested" in source
    assert "scan_generation_mismatch" in source
    assert "scan_identity_unavailable" in source
    assert "scan_occupancy_registration_reason(&occupancy, geometry_computed_at_ms)" in source
    assert "load_session_scan_frame_id(object_id, session_id, current_scan_frame_id)" in source
    assert "scan_identity_err == ESP_OK && strcmp(occupancy.frame_id, current_scan_frame_id) == 0;" in source
    assert "const bool usable = scan_identity_matches && registration_reason == NULL;" in source
    assert "scan_constraint_applied[i]" in source


def test_velocity_free_methods_run_on_the_same_picks_and_are_fused_once():
    """TDCAS, PSAFUVS and SUEA are persisted as their own records before the
    fused prefix ends; the policy puts them in the shared P-pick evidence
    group so the joint fusion never counts the same picks twice."""
    source = orchestrator_source()
    fused_index = source.index("const size_t fused_record_count = record_count;")
    for call in (
        # MAX-301: TDCAS now receives a config carrying the velocity bounds.
        "localization_tdcas_solve(p_observations, p_observation_count, &tdcas_config, tdcas_result)",
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
    run_start = source.index("esp_err_t localization_calculate_run(")
    workspace_alloc_index = source.index(
        "localization_calculate_workspace_t *workspace = heap_caps_calloc(",
        run_start,
    )
    pick_write_index = source.index(
        "pick_write_err = localization_calculate_write_picks_retry(",
        run_start,
    )
    first_method_write_index = source.index(
        "localization_run_store_write(object_id, session_id, &records[i]);",
        run_start,
    )
    assert workspace_alloc_index < min(pick_write_index, first_method_write_index)
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


def test_every_method_is_timed_and_the_heavy_ones_respect_the_deadline():
    """Each method is bracketed by the time budget; skippable ones only run
    when budget_begin allows, and every record gets budget_end (which marks
    a refused method unsolved with the reason) before it is counted."""
    source = orchestrator_source()
    assert "config->deadline_us" in source
    # Twenty method records plus the joint-fusion timing record.
    assert source.count("localization_calculate_budget_begin(") == 21
    assert source.count("++record_count;") == source.count("localization_calculate_budget_end(&budget, &records[record_count]);")
    for mandatory in ('"tdoa_gauss_newton", false', '"energy_baseline", false'):
        assert mandatory in source
    for skippable in (
        "traveltime_inversion", "semblance", "tdcas", "psafuvs", "suea", "xcorr_tdoa", "robust_tdoa",
        "psp_joint", "bayesian", "eikonal_fmm", "traveltime_scan_constrained", "semblance_scan_constrained",
        "tdcas_scan_constrained", "psafuvs_scan_constrained", "suea_scan_voids", "eikonal_scan_voids",
        "semblance_eikonal_voids", "bayesian_scan_voids",
    ):
        assert f'"{skippable}", true)' in source, skippable


def test_calculate_source_never_runs_on_the_https_task():
    """The HTTP handler only queues the job; the run happens on the
    localization_job worker (16 KiB PSRAM stack, idle priority on the
    signal-processing core), created before the web server starts."""
    control = read("components/web_server/web_api_control.c")
    assert "return localization_calculate_run(" not in control
    assert "localization_calculate_summary_t summary;" not in control
    assert "return localization_job_submit(object_id, session_id, &config, budget_ms, NULL);" in control
    job = read("components/localization_job/localization_job.c")
    assert "localization_calculate_run(" in job
    assert "LOCALIZATION_JOB_STACK_BYTES," in job
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in job
    assert "APP_TASK_CORE_REALTIME" in job
    assert "uxTaskGetStackHighWaterMark(NULL)" in job
    header = read("components/localization_job/include/localization_job.h")
    assert "#define LOCALIZATION_JOB_STACK_BYTES 16384U" in header
    tasks = read("components/app_config/include/app_task_config.h")
    assert "#define APP_TASK_PRIORITY_LOCALIZATION_JOB 0" in tasks
    startup = read("main/app_startup.c")
    assert startup.index("localization_job_init()") < startup.index("web_server_start()")
    assert "localization_job" in read("main/CMakeLists.txt")
    assert "localization_job" in read("components/web_server/CMakeLists.txt")
    # Status shares the runs route: no extra HTTPS handler slot.
    api = read("components/web_server/web_localization_api.c")
    assert 'strcmp(value, "status") == 0' in api
    assert "localization_job_status_json(&json)" in api
