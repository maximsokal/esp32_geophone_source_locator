"""Source contracts for the geophone_events component (MAX-134)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPONENT = REPO_ROOT / "components" / "geophone_events"


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_no_invented_default_configuration_exists():
    """MAX-132's rule ("explicit method configuration must remain
    authoritative") means this component must never offer a factory that
    invents a window length, ratio or threshold on the caller's behalf."""
    header = read("components/geophone_events/include/geophone_events.h")
    sources = "".join(
        path.read_text(encoding="utf-8") for path in COMPONENT.glob("*.c")
    )

    assert "default_config" not in header
    assert "default_config" not in sources


def test_all_five_required_methods_are_present_and_separately_callable():
    header = read("components/geophone_events/include/geophone_events.h")

    assert "geophone_event_energy_trigger_detect(" in header
    assert "geophone_event_sta_lta_detect(" in header
    assert "geophone_event_refine_arrival(" in header
    assert "geophone_event_cross_correlate(" in header
    assert "geophone_event_compute_snr(" in header
    assert "geophone_event_pick_is_acceptable(" in header


def test_combined_pick_reports_method_snr_uncertainty_and_rejection_reason():
    header = read("components/geophone_events/include/geophone_events.h")

    assert "geophone_pick_status_t status;" in header
    assert "geophone_pick_method_t method;" in header
    assert "float snr;" in header
    assert "size_t uncertainty_samples;" in header
    assert "GEOPHONE_PICK_REJECTED_NO_TRIGGER" in header
    assert "GEOPHONE_PICK_REJECTED_NO_REFINEMENT" in header
    assert "GEOPHONE_PICK_REJECTED_LOW_SNR" in header


def test_pick_pipeline_gates_on_snr_before_accepting():
    orchestrator = read("components/geophone_events/geophone_event_pick.c")

    trigger = orchestrator.index("geophone_event_sta_lta_detect(")
    refine = orchestrator.index("geophone_event_refine_arrival(", trigger)
    snr = orchestrator.index("geophone_event_compute_snr(", refine)
    gate = orchestrator.index("geophone_event_pick_is_acceptable(", snr)
    accepted = orchestrator.index("GEOPHONE_PICK_ACCEPTED", gate)

    assert trigger < refine < snr < gate < accepted


def test_no_stage_carries_esp_idf_dependency():
    """This is offline/batch analysis over an already-captured buffer, not a
    FreeRTOS task; keeping it framework-free is what makes every method
    directly host-testable with synthetic vectors."""
    header = read("components/geophone_events/include/geophone_events.h")
    sources = "".join(
        path.read_text(encoding="utf-8") for path in COMPONENT.glob("*.c")
    )

    assert "esp_err.h" not in header
    assert "freertos" not in sources.lower()
    assert "xTaskCreate" not in sources
