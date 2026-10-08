"""Source contract: the TFT scan preview must route scan generation decisions
through ui_scan_preview_policy (MAX-415 occupancy identity, MAX-414 run binding).
No host manifest entry compiles ui_scan_preview.c, so the wiring is pinned here."""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index]
    raise AssertionError("unterminated function " + signature)


def test_policy_source_is_registered_in_the_component():
    assert '"ui_scan_preview_policy.c"' in read("components/ui/CMakeLists.txt")


def test_occupancy_fallback_requires_the_current_scan_identity():
    source = read("components/ui/ui_scan_preview.c")
    body = function_body(source, "static void load_session(")
    check = body.index("ui_scan_preview_occupancy_matches_scan(result->occupancy.frame_id, scan_crc)")
    ready = body.index("SCAN_PREVIEW_SCREEN_READY", check)
    free_call = body.index("scan_occupancy_free(&result->occupancy)", ready)
    parse = body.index("scan_occupancy_parse(")
    assert parse < check < ready < free_call


def test_sources_are_filtered_by_run_eligibility_before_selection():
    source = read("components/ui/ui_scan_preview.c")
    body = function_body(source, "static void load_sources(")
    eligibility = body.index("ui_scan_preview_run_eligibility(")
    select = body.index("ui_preview3d_select_sources(")
    assert eligibility < select
    assert "UI_SCAN_PREVIEW_RUN_ELIGIBLE" in body
    for field in ("scan_provenance_present", "scan_constraint_applied", "scan_frame_id", "geometry_computed_at_ms"):
        assert "run->" + field in body


def test_scan_identity_is_known_before_sources_are_loaded():
    body = function_body(read("components/ui/ui_scan_preview.c"), "static void load_session(")
    assert body.index("read_scan_crc(") < body.index("load_sources(request, result, ")
    assert re.search(r"load_sources\(request, result, scan_identity", body)
