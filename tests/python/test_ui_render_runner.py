"""Reference lifecycle and failure reporting at the native runner boundary."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import run_ui_render_tests as runner  # noqa: E402


@pytest.fixture
def native_run(tmp_path, monkeypatch):
    goldens = tmp_path / "golden"
    goldens.mkdir()
    (goldens / "en_home.png").write_bytes(b"approved reference")
    binary = tmp_path / "native" / "ui_render.exe"
    binary.parent.mkdir()
    binary.write_bytes(b"native executable")
    (binary.parent / "build.json").write_text(json.dumps({
        "status": "PASS", "head": "compiled-revision",
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    }), encoding="utf-8")
    monkeypatch.setattr(runner, "GOLDENS", goldens)
    monkeypatch.setattr(runner, "build", lambda _: binary)
    output = tmp_path / "report"
    mode = {"result": "frames"}

    def execute(command, *, cwd, **kwargs):
        assert command == [str(binary), "."]
        if mode["result"] == "timeout":
            raise subprocess.TimeoutExpired(command, 90, output=b"partial render", stderr=b"timeout diagnostic")
        if mode["result"] == "error":
            return subprocess.CompletedProcess(command, 3, "partial render", "native error")
        if mode["result"] == "frames":
            frame = {"name": "en_home", "family": "home", "golden": True,
                     "comparison": 1 if (Path(cwd) / "references/en_home.png").exists() else 2}
            (Path(cwd) / "frames.jsonl").write_text(json.dumps(frame) + "\n", encoding="utf-8")
        if mode["result"] == "empty-frames":
            (Path(cwd) / "frames.jsonl").write_text("", encoding="utf-8")
        return subprocess.CompletedProcess(command, 0, "rendered", "")

    monkeypatch.setattr(runner.subprocess, "run", execute)
    monkeypatch.setattr(sys, "argv", ["run_ui_render_tests.py", "--output-dir", str(output)])
    return binary, output, goldens, mode


def test_reused_output_cannot_supply_a_missing_source_reference(native_run):
    binary, output, goldens, _ = native_run
    assert runner.render(binary, output)[0]["comparison"] == 1
    diagnostic = output / "references/en_home_err.png"
    diagnostic.write_bytes(b"previous mismatch diagnostic")
    (goldens / "en_home.png").unlink()

    frames = runner.render(binary, output)
    assert frames[0]["comparison"] == 2, "A stale copied PNG hid a missing authoritative reference"
    assert not (output / "references/en_home.png").exists()
    assert (output / "previous-errors/en_home_err.png").read_bytes() == b"previous mismatch diagnostic"


def test_cli_reused_output_fails_for_missing_source_reference(native_run):
    _, output, goldens, _ = native_run
    runner.main()
    (goldens / "en_home.png").unlink()
    with pytest.raises(SystemExit, match="mismatch/missing reference"):
        runner.main()
    assert json.loads((output / "run.json").read_text())["status"] == "FAIL"


@pytest.mark.parametrize("mode", ["error", "timeout", "no-frames", "empty-frames"])
def test_render_failure_cannot_reuse_previous_success(native_run, mode):
    binary, output, _, state = native_run
    runner.render(binary, output)
    diagnostic = output / "en_home_diff.png"
    diagnostic.write_bytes(b"diagnostic")
    state["result"] = mode
    with pytest.raises((RuntimeError, subprocess.TimeoutExpired, FileNotFoundError)):
        runner.render(binary, output)
    metadata = json.loads((output / "run.json").read_text())
    assert metadata["status"] == "FAIL"
    assert metadata["phase"] == "render"
    assert diagnostic.read_bytes() == b"diagnostic"
    assert 'data-result="1"' not in (output / "index.html").read_text(encoding="utf-8")
    if mode == "timeout":
        assert (output / "render.log").read_text() == "partial rendertimeout diagnostic"


def test_build_failure_invalidates_previous_gallery(native_run, monkeypatch):
    binary, output, _, _ = native_run
    runner.render(binary, output)

    def fail(_):
        raise RuntimeError("compiler unavailable")

    monkeypatch.setattr(runner, "build", fail)
    with pytest.raises(RuntimeError, match="compiler unavailable"):
        runner.main()
    metadata = json.loads((output / "run.json").read_text())
    assert metadata["status"] == "FAIL" and metadata["phase"] == "build"
    assert "compiler unavailable" in metadata["error"]
    assert 'data-result="1"' not in (output / "index.html").read_text(encoding="utf-8")


def test_run_identity_keeps_build_identity_and_tracks_reference_changes(native_run):
    binary, output, goldens, _ = native_run
    runner.render(binary, output)
    first = json.loads((output / "run.json").read_text())
    (goldens / "en_home.png").write_bytes(b"new isolated test reference")
    runner.render(binary, output)
    second = json.loads((output / "run.json").read_text())
    assert second["status"] == "PASS" and second["phase"] == "compare"
    assert second["build"] == first["build"]
    assert second["build"]["head"] == "compiled-revision"
    assert second["references"]["sha256"] != first["references"]["sha256"]
    assert second["references"]["files"] == 1
    assert second["comparisons"] == {"matched": 1, "missing": 0, "different": 0}
    assert second["checks"] == ["native_render", "screenshot_comparison"]
    assert second["environment"]["python"]


def test_replaced_binary_cannot_reuse_successful_build_identity(native_run):
    binary, output, _, _ = native_run
    binary.write_bytes(b"different executable")
    with pytest.raises(RuntimeError, match="matching successful build identity"):
        runner.render(binary, output)
    assert json.loads((output / "run.json").read_text())["status"] == "FAIL"


def test_failed_build_invalidates_old_executable(tmp_path, monkeypatch):
    binary = tmp_path / "ui_render.exe"
    binary.write_bytes(b"old executable")
    (tmp_path / "build.json").write_text('{"status":"PASS"}', encoding="utf-8")

    def fail(*_):
        raise RuntimeError("build failed")

    monkeypatch.setattr(runner, "build_native", fail)
    with pytest.raises(RuntimeError, match="build failed"):
        runner.build(tmp_path)
    assert json.loads((tmp_path / "build.json").read_text())["status"] == "FAIL"
    assert binary.read_bytes() == b"old executable"


@pytest.mark.parametrize("source_as_copy", [False, True])
def test_output_cannot_overwrite_authoritative_references(tmp_path, monkeypatch, source_as_copy):
    goldens = tmp_path / "report" / "references" if source_as_copy else tmp_path / "golden"
    goldens.mkdir(parents=True)
    source = goldens / "en_home.png"
    source.write_bytes(b"approved reference")
    monkeypatch.setattr(runner, "GOLDENS", goldens)
    output = goldens.parent if source_as_copy else goldens
    with pytest.raises(ValueError, match="must not overwrite source goldens"):
        runner.render(tmp_path / "unused-binary", output)
    assert source.read_bytes() == b"approved reference"


def test_compiler_named_on_path_keeps_supported_cc_override(tmp_path, monkeypatch):
    compiler = tmp_path / "compiler.exe"
    compiler.write_bytes(b"native compiler on PATH")
    monkeypatch.setattr(runner, "compiler_from_args", lambda _: ["native-cc"])
    monkeypatch.setattr(runner, "compiler_description", lambda _: "native compiler")
    monkeypatch.setattr(runner, "ensure_native_compiler", lambda *_: None)
    monkeypatch.setattr(runner.shutil, "which", lambda name: str(compiler) if name == "native-cc" else name)
    monkeypatch.setattr(runner, "find_tool", lambda name: name)
    monkeypatch.setattr(runner.subprocess, "check_output", lambda *_, **__: "build-revision")

    def execute(command, **_):
        if "--build" in command:
            binary = tmp_path / ("ui_render.exe" if runner.os.name == "nt" else "ui_render")
            binary.write_bytes(b"compiled output")
        return subprocess.CompletedProcess(command, 0, "built", "")

    monkeypatch.setattr(runner.subprocess, "run", execute)
    assert runner.build(tmp_path).is_file()
    metadata = json.loads((tmp_path / "build.json").read_text())
    assert metadata["status"] == "PASS"
    assert metadata["compiler"]["command"] == ["native-cc"]
    assert metadata["compiler"]["sha256"] == hashlib.sha256(compiler.read_bytes()).hexdigest()
