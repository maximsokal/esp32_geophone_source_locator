"""Validation, generation, print geometry, and loopback API tests for AprilTag sheets."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
from apriltag_sheet import model  # noqa: E402
from apriltag_sheet.apriltag_sheet import PT_PER_MM, make_server, render_pdf  # noqa: E402


TOOL = REPO_ROOT / "tools" / "apriltag_sheet" / "apriltag_sheet.py"
DEFAULT = model.load_bindings(REPO_ROOT)


@pytest.mark.parametrize(
    ("edit", "message"),
    [
        (lambda data: data["tags"][1].update(node="MAIN"), "duplicated"),
        (lambda data: data["tags"].pop(), "exactly 0, 1, 2, 3, and 4"),
        (lambda data: data["tags"][4].update(tag_id=5), "integer from 0 through 4"),
        (lambda data: data["tags"][0].update(size_mm=14), "range 15..150"),
        (lambda data: data["tags"][0].update(size_mm=151), "range 15..150"),
        (lambda data: data.update(extra=True), "exactly family, tags, and sheet"),
        (lambda data: data.update(family="tag25h9"), "family must be"),
        (lambda data: data["tags"][0].update(node="G1"), "duplicated"),
        (lambda data: data["tags"][0].update(node="UNKNOWN"), "one of MAIN, G1"),
        (lambda data: data["sheet"].update(copies_per_tag=0), "range 1..4"),
        (lambda data: data["sheet"].update(copies_per_tag=5), "range 1..4"),
        (lambda data: data["sheet"].update(paper="Letter"), "paper must be A4"),
        (lambda data: data["tags"][0].update(unexpected=True), "exactly tag_id, node, and size_mm"),
    ],
)
def test_binding_validation_rejects_invalid_matrix_cases(edit, message):
    invalid = deepcopy(DEFAULT)
    edit(invalid)
    with pytest.raises(model.BindingError, match=message):
        model.validate_bindings(invalid)


def test_binding_validation_accepts_permutation_and_inclusive_limits():
    document = deepcopy(DEFAULT)
    document["tags"][0].update(node="G1", size_mm=15)
    document["tags"][1].update(node="MAIN", size_mm=150)
    document["sheet"]["copies_per_tag"] = 4
    validated = model.validate_bindings(document)
    assert validated["tags"][0] == {"tag_id": 0, "node": "G1", "size_mm": 15}
    assert validated["tags"][1] == {"tag_id": 1, "node": "MAIN", "size_mm": 150}
    assert validated["sheet"]["copies_per_tag"] == 4


def _copy_minimum_repo(root: Path) -> Path:
    tool_dir = root / "tools" / "apriltag_sheet"
    tool_dir.mkdir(parents=True)
    for name in ("__init__.py", "model.py", "apriltag_sheet.py", "index.html", "tag_bindings.json", "tag_patterns.gen.json"):
        shutil.copy2(REPO_ROOT / "tools" / "apriltag_sheet" / name, tool_dir / name)
    generated = root / model.GENERATED_TS_PATH
    generated.parent.mkdir(parents=True)
    shutil.copy2(REPO_ROOT / model.GENERATED_TS_PATH, generated)
    dictionary = root / model.DICTIONARY_PATH
    dictionary.parent.mkdir(parents=True)
    shutil.copy2(REPO_ROOT / model.DICTIONARY_PATH, dictionary)
    return root


def _snapshot(root: Path) -> dict[str, bytes]:
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}


def _post(url: str, body: bytes, *, headers: dict[str, str] | None = None) -> tuple[int, bytes]:
    request = Request(url, data=body, headers=headers or {}, method="POST")
    try:
        with urlopen(request, timeout=3) as response:
            return response.status, response.read()
    except HTTPError as error:
        return error.code, error.read()


def test_generated_outputs_are_current_and_check_mode_detects_stale_bindings(tmp_path):
    model.check_generated(REPO_ROOT)
    checked = subprocess.run(
        [sys.executable, str(TOOL), "generate", "--check", "--root", str(REPO_ROOT)],
        check=False,
    )
    assert checked.returncode == 0

    fixture_root = _copy_minimum_repo(tmp_path / "stale-repo")
    config = fixture_root / model.BINDINGS_PATH
    document = json.loads(config.read_text(encoding="utf-8"))
    document["tags"][0]["size_mm"] = 33
    config.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    stale = subprocess.run(
        [sys.executable, str(TOOL), "check", "--root", str(fixture_root)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert stale.returncode == 1
    assert "generated AprilTag outputs are stale" in stale.stderr


def test_generated_patterns_match_the_pinned_js_aruco_dictionary():
    document = model.pattern_document(REPO_ROOT)
    codes = model.load_dictionary_codes(REPO_ROOT)
    assert len(document["tags"]) == 5
    for tag_id, entry in enumerate(document["tags"]):
        code = codes[tag_id]
        bits = format(code, "036b")
        assert entry["tag_id"] == tag_id
        assert entry["code"] == f"0x{code:09x}"
        assert entry["data_bits"] == bits
        assert "".join(str(bit) for row in entry["data_matrix"] for bit in row) == bits
        assert all(entry["modules"][0]) and all(entry["modules"][7])
        assert all(row[0] == row[7] == 1 for row in entry["modules"])
        assert [row[1:7] for row in entry["modules"][1:7]] == [[1 - bit for bit in row] for row in entry["data_matrix"]]


def _fmt(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".") or "0"


def _expected_tag_origins(bindings: dict, patterns: dict) -> list[tuple[int, float, float, float]]:
    """Independent shelf placement from the A4 margins and module quiet-zone rule."""
    placements = []
    x, top, row_height = 5.0, 8.0, 0.0
    for binding in bindings["tags"]:
        for _ in range(bindings["sheet"]["copies_per_tag"]):
            size = float(binding["size_mm"])
            quiet = size / 8.0
            footprint = size + quiet * 2 + 6.0
            item_height = footprint + 9.0
            if x + footprint > 205.0:
                x, top, row_height = 5.0, top + row_height + 7.0, 0.0
            if top + item_height > 264.0:
                x, top, row_height = 5.0, 8.0, 0.0
            placements.append((binding["tag_id"], x + 3.0 + quiet, top + 3.0 + quiet, size))
            x += footprint + 6.0
            row_height = max(row_height, item_height)
    return placements


def test_pdf_has_exact_a4_geometry_black_module_squares_captions_bar_and_stable_bytes(tmp_path):
    first = render_pdf(REPO_ROOT)
    assert first == render_pdf(REPO_ROOT)
    assert first.startswith(b"%PDF-1.4\n%APRILTAG-SHEET\n")
    assert b"/CreationDate" not in first
    assert len(re.findall(rb"/Type /Page\b", first)) == 1
    assert b"/MediaBox [0 0 595.275591 841.889764]" in first
    streams = re.findall(rb"/Length (\d+) >>\nstream\n(.*?)\nendstream", first, re.DOTALL)
    assert len(streams) == 1
    content = streams[0][1]
    assert int(streams[0][0]) == len(content) + 1
    for binding in DEFAULT["tags"]:
        caption = f"{binding['node']} | tag36h11 id {binding['tag_id']} | {binding['size_mm']:.1f} mm".encode("ascii")
        assert caption in content

    expected_rectangles = []
    patterns = model.pattern_document(REPO_ROOT)
    for tag_id, tag_x, tag_top, size_mm in _expected_tag_origins(DEFAULT, patterns):
        matrix = patterns["tags"][tag_id]["modules"]
        module_mm = size_mm / 8.0
        for row, values in enumerate(matrix):
            for column, black in enumerate(values):
                if black:
                    x_pt = (tag_x + column * module_mm) * PT_PER_MM
                    y_pt = (297.0 - tag_top - (row + 1) * module_mm) * PT_PER_MM
                    edge_pt = module_mm * PT_PER_MM
                    expected_rectangles.append(tuple(map(float, (_fmt(x_pt), _fmt(y_pt), _fmt(edge_pt), _fmt(edge_pt)))))
    actual_rectangles = [
        tuple(map(float, match))
        for match in re.findall(rb"(?m)^([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+) re f$", content)
    ]
    assert actual_rectangles == expected_rectangles
    assert all(abs(rectangle[2] - (32 / 8) * PT_PER_MM) < 1e-6 for rectangle in actual_rectangles)

    line_segments = [
        tuple(map(float, match))
        for match in re.findall(rb"(?m)^[0-9.]+ w ([0-9.]+) ([0-9.]+) m ([0-9.]+) ([0-9.]+) l S$", content)
    ]
    assert any(abs(abs(x2 - x1) - 50 * PT_PER_MM) < 1e-6 and abs(y2 - y1) < 1e-6 for x1, y1, x2, y2 in line_segments)
    assert b"Calibration bar: 50 mm. Measure after printing." in content
    assert b"White quiet zone >= tag edge / 8." in content
    assert b"Print at 100% scale / no fit-to-page." in content

    output = tmp_path / "sample.pdf"
    output.write_bytes(first)
    assert output.stat().st_size == len(first)


def test_pdf_paginates_high_size_multiple_copies(tmp_path):
    root = _copy_minimum_repo(tmp_path / "copies-repo")
    document = deepcopy(DEFAULT)
    document["sheet"]["copies_per_tag"] = 4
    for tag in document["tags"]:
        tag["size_mm"] = 150
    (root / model.BINDINGS_PATH).write_text(json.dumps(document), encoding="utf-8")
    content = render_pdf(root)
    assert len(re.findall(rb"/Type /Page\b", content)) == 20
    assert len(re.findall(rb"tag36h11 id", content)) == 20


def test_loopback_server_reads_state_rejects_unsafe_posts_and_writes_only_bindings(tmp_path):
    root = _copy_minimum_repo(tmp_path / "server-repo")
    server = make_server(root)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        with urlopen(base + "/api/state", timeout=3) as response:
            initial = json.loads(response.read())
        assert initial["valid"] is True
        assert [entry["tag_id"] for entry in initial["bindings"]["tags"]] == [0, 1, 2, 3, 4]
        assert len(initial["patterns"]["tags"]) == 5
        with urlopen(base + "/", timeout=3) as response:
            assert response.status == 200 and b'id="bind-button"' in response.read()
        with urlopen(base + "/sheet.pdf", timeout=3) as response:
            assert response.status == 200 and response.headers.get_content_type() == "application/pdf"
            assert response.read().startswith(b"%PDF-1.4")

        before = _snapshot(root)
        good_headers = {"Content-Type": "application/json", "X-Apriltag-Sheet": "1"}
        bad_requests = [
            _post(base + "/api/bind", b"{broken", headers=good_headers),
            _post(base + "/api/bind", b" " * (16 * 1024 + 1), headers=good_headers),
            _post(base + "/api/bind", json.dumps(DEFAULT).encode(), headers={"Content-Type": "application/json"}),
            _post(base + "/api/bind", json.dumps(DEFAULT).encode(), headers={**good_headers, "Host": "attacker.example"}),
            _post(base + "/api/bind", json.dumps(DEFAULT).encode(), headers={**good_headers, "Content-Type": "text/plain"}),
        ]
        assert [status for status, _ in bad_requests] == [400, 413, 403, 403, 415]
        assert _snapshot(root) == before

        swapped = deepcopy(DEFAULT)
        swapped["tags"][0]["node"], swapped["tags"][1]["node"] = "G1", "MAIN"
        status, response_body = _post(base + "/api/bind", json.dumps(swapped).encode(), headers=good_headers)
        assert status == 200
        saved = json.loads(response_body)
        assert saved["valid"] is True
        assert [(entry["tag_id"], entry["node"]) for entry in saved["bindings"]["tags"][:2]] == [(0, "G1"), (1, "MAIN")]
        after = _snapshot(root)
        changed = {name for name in before if before[name] != after[name]}
        assert changed == {model.BINDINGS_PATH.as_posix(), model.GENERATED_TS_PATH.as_posix()}
        assert set(after) == set(before)
        generated_ts = (root / model.GENERATED_TS_PATH).read_text(encoding="utf-8")
        assert "{ tag_id: 0, node: 'G1', size_mm: 32 }" in generated_ts
        assert "G1: 0" in generated_ts and "MAIN: 1" in generated_ts
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def test_page_controls_and_every_i18n_key_have_english_and_ukrainian_text():
    source = (REPO_ROOT / "tools" / "apriltag_sheet" / "index.html").read_text(encoding="utf-8")
    table = re.search(r'<script id="i18n-table" type="application/json">(.*?)</script>', source, re.DOTALL)
    assert table is not None
    translations = json.loads(table.group(1))
    assert set(translations) == {"en", "uk"}
    keys = set(re.findall(r'data-i18n="([a-z0-9_]+)"', source))
    assert keys <= set(translations["en"]) and keys <= set(translations["uk"])
    assert all(translations[locale][key].strip() for locale in ("en", "uk") for key in translations[locale])
    for element_id in (
        "language-select", "all-size-mm", "tag-cards", "binding-table", "bindings-body",
        "bind-button", "print-button", "download-pdf", "calibration-bar", "validation-errors",
    ):
        assert f'id="{element_id}"' in source
    assert 'id="node-select-${binding.tag_id}"' in source
    assert 'id="status-chip-${binding.tag_id}${suffix}"' in source
    assert "pending.sheet.copies_per_tag" in source
    assert translations["en"]["bind"] == "Bind"
    assert translations["uk"]["bind"] == "Прив’язати"
    # MAX-438: four ADC channels CH1..CH4 -> MAIN, G1, G2, G3; G4 has no ADC channel.
    assert "CH1 = MAIN, CH2 = G1, CH3 = G2, CH4 = G3" in translations["en"]["connection_order"]
    assert "CH5" not in translations["en"]["connection_order"]
    assert "CH5" not in translations["uk"]["connection_order"]
    assert not re.search(r'<(?:script|link)[^>]+(?:https?:)?//', source, re.IGNORECASE)


def test_vscode_tasks_and_local_python_scope_expose_the_sheet_tool():
    tasks = json.loads((REPO_ROOT / ".vscode" / "tasks.json").read_text(encoding="utf-8"))["tasks"]
    by_label = {task["label"]: task for task in tasks}
    browser = by_label["AprilTag: open sheet generator (browser)"]
    assert browser["command"] == "python"
    assert browser["args"] == ["${workspaceFolder}/tools/apriltag_sheet/apriltag_sheet.py", "serve"]
    assert browser["presentation"]["reveal"] == "always"
    pdf = by_label["AprilTag: write sheet PDF"]
    assert pdf["command"] == "python"
    assert pdf["args"][-2:] == ["--out", "${workspaceFolder}/.test-build/apriltag_sheet.pdf"]

    plan = subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "run_local_verify.py"), "--scope", "python", "--dry-run"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert plan.returncode == 0
    assert "python/apriltag-sheet" in plan.stdout
    assert 'apriltag_sheet.py" check' in plan.stdout
