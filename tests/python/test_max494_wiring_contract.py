import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/host/manifest.d/max494-nrf-fsm.json"


# characterization: the focused manifest must compile each requested model against production modules.
def test_manifest_registers_max494_models_and_mutation_targets():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    tests = {entry["name"]: entry for entry in manifest["tests"]}
    assert {
        "max494_nrf_fsm_reference_model",
        "max494_nrf_fsm_runtime_model",
        "max494_cs_pair_runtime_model",
    } <= tests.keys()
    assert "tests/host/nrf_fsm_reference_model_test.c" in tests["max494_nrf_fsm_reference_model"]["sources"]
    assert "tests/host/nrf_fsm_runtime_model_test.c" in tests["max494_nrf_fsm_runtime_model"]["sources"]
    assert "tests/host/nrf_cs_pair_runtime_model_test.c" in tests["max494_cs_pair_runtime_model"]["sources"]
    assert "nRF54L15/src/nrf_bootstrap_runtime_zephyr.c" in tests["max494_nrf_fsm_runtime_model"]["sources"]


# characterization: replay uses independent enums, fixed seeds, bounded traces, and chunk deletion.
def test_reference_model_replay_and_test_labels_are_explicit():
    model = (ROOT / "tests/host/nrf_fsm_reference_model_test.c").read_text(encoding="utf-8")
    assert "typedef enum { M_ROLE_NONE" in model
    assert "0x00000001U, 0x0000C0FFU, 0x000051A7U, 0xFFFFFFFFU" in model
    assert "#define MODEL_MAX_TRACE 64U" in model
    assert "shrink_trace" in model and "chunk /= 2U" in model
    category = re.compile(r"/\*\s*(?:fail-before\(real\)|mutation-only|characterization):")
    for relative in (
        "tests/host/nrf_fsm_reference_model_test.c",
        "tests/host/nrf_fsm_runtime_model_test.c",
        "tests/host/nrf_cs_pair_runtime_model_test.c",
    ):
        source = (ROOT / relative).read_text(encoding="utf-8")
        for match in re.finditer(r"(?:static\s+void|int)\s+(?:test_[A-Za-z0-9_]+|main)\s*\(", source):
            preceding = source[max(0, match.start() - 180) : match.start()]
            assert category.search(preceding), f"missing MAX-494 test label before {relative}:{match.start()}"


# characterization: mutation dry-run includes each requested failure surface without running mutants.
def test_mutation_file_names_all_max494_guards():
    mutations = json.loads((ROOT / "tools/mutations/max494_nrf_fsm.json").read_text(encoding="utf-8"))
    descriptions = " ".join(item["description"].lower() for item in mutations)
    assert "membership generation" in descriptions
    assert "duplicate" in descriptions and "request" in descriptions and "step" in descriptions
    assert "node" in descriptions and "channel" in descriptions
    assert "address" in descriptions or "role" in descriptions
    assert all({"id", "description", "file", "find", "replace", "tests", "expected_killer"} == item.keys() for item in mutations)

