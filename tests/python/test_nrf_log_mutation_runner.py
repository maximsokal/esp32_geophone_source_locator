from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest


sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_mutation_tests as runner


def project(tmp_path: Path, contents: bytes = b"ORIGINAL\n") -> tuple[Path, Path, Path, Path]:
    root = tmp_path / "repo"
    root.mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    target = root / "target.txt"
    target.write_bytes(contents)
    manifest = root / "tests" / "host" / "manifest.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(
        json.dumps({"version": 1, "tests": [{"name": "fake_test", "sources": ["target.txt"]}]}),
        encoding="utf-8",
    )
    fail_runner = root / "fake_fail.py"
    fail_runner.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "assert '--manifest' in sys.argv and '--build-dir' in sys.argv\n"
        "sys.exit(1 if b'MUTATED' in Path('target.txt').read_bytes() else 0)\n",
        encoding="utf-8",
    )
    pass_runner = root / "fake_pass.py"
    pass_runner.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "assert '--manifest' in sys.argv and '--build-dir' in sys.argv\n"
        "assert Path('target.txt').exists()\n"
        "sys.exit(0)\n",
        encoding="utf-8",
    )
    definition = root / "mutations.json"
    definition.write_text(json.dumps([{
        "id": "fake_mutation",
        "description": "temporary test mutation",
        "file": "target.txt",
        "find": "ORIGINAL",
        "replace": "MUTATED",
        "tests": [{
            "manifest": "tests/host/manifest.json",
            "only": "fake_test",
        }],
        "expected_killer": "fake_test",
    }]), encoding="utf-8")
    return root, target, definition, fail_runner


def execute(
    root: Path,
    definition: Path,
    fake_runner: Path,
    *,
    command_runner=runner.run_subprocess,
) -> tuple[list[runner.MutationResult], int, list[str]]:
    return runner.run_mutations(
        [definition],
        repo_root=root,
        allow_dirty=True,
        host_runner=fake_runner,
        command_runner=command_runner,
    )


def tree_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        digest.update(path.relative_to(root).as_posix().encode())
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()


def test_killed_and_survived_classification_and_exit_codes(tmp_path: Path) -> None:
    root, target, definition, fail_runner = project(tmp_path)
    results, exit_code, errors = execute(root, definition, fail_runner)
    assert not errors
    assert exit_code == 0, (results, errors)
    assert results[0].status == "KILLED"
    assert target.read_bytes() == b"ORIGINAL\n"

    pass_runner = root / "fake_pass.py"
    results, exit_code, errors = execute(root, definition, pass_runner)
    assert not errors
    assert exit_code == 1
    assert results[0].status == "SURVIVED"
    assert target.read_bytes() == b"ORIGINAL\n"


@pytest.mark.parametrize("contents,find_count", [
    (b"OTHER\n", 0),
    (b"ORIGINAL ORIGINAL\n", 2),
])
def test_missing_or_non_unique_find_is_config_error_without_writes(
    tmp_path: Path,
    contents: bytes,
    find_count: int,
) -> None:
    root, target, definition, _ = project(tmp_path, contents)
    before = tree_hash(root)
    results, exit_code, errors = runner.run_mutations(
        [definition],
        repo_root=root,
        allow_dirty=True,
    )
    assert results == []
    assert exit_code == 2
    assert any(f"found {find_count}" in error for error in errors)
    assert tree_hash(root) == before
    assert target.read_bytes() == contents


def test_dry_run_writes_nothing(tmp_path: Path) -> None:
    root, _, definition, _ = project(tmp_path)
    before = tree_hash(root)
    rows, errors = runner.dry_run_rows([definition], repo_root=root)
    assert not errors
    assert rows[0][3] == "OK"
    assert tree_hash(root) == before
    assert not (root / "build" / "mutation-backup").exists()


def test_file_restored_after_test_runner_exception(tmp_path: Path) -> None:
    root, target, definition, _ = project(tmp_path)

    def fail_to_launch(command, *, cwd, timeout):
        raise OSError("simulated launch failure")

    results, exit_code, errors = execute(root, definition, root / "fake_fail.py", command_runner=fail_to_launch)
    assert not errors
    assert exit_code == 3
    assert results[0].status == "ERROR"
    assert target.read_bytes() == b"ORIGINAL\n"
    assert not list((root / "build" / "mutation-backup").glob("*.json"))


def test_file_restored_after_simulated_keyboard_interrupt(tmp_path: Path) -> None:
    root, target, definition, _ = project(tmp_path)

    def interrupt(command, *, cwd, timeout):
        raise KeyboardInterrupt

    results, exit_code, errors = execute(root, definition, root / "fake_fail.py", command_runner=interrupt)
    assert not errors
    assert exit_code == 3
    assert results[0].status == "ERROR"
    assert results[0].detail == "interrupted"
    assert target.read_bytes() == b"ORIGINAL\n"
    assert not list((root / "build" / "mutation-backup").glob("*.json"))


def test_leftover_marker_blocks_execution_and_restore_recovers_it(tmp_path: Path) -> None:
    root, target, definition, _ = project(tmp_path)
    backup_dir = root / runner.BACKUP_DIR
    backup_dir.mkdir(parents=True)
    backup = backup_dir / "leftover.backup"
    original = target.read_bytes()
    backup.write_bytes(original)
    target.write_bytes(b"MUTATED\n")
    marker = backup_dir / "leftover.json"
    marker.write_text(json.dumps({
        "target": "target.txt",
        "backup": backup.relative_to(root).as_posix(),
        "sha256": runner.sha256(original),
        "clean_at_start": False,
    }), encoding="utf-8")

    results, exit_code, errors = runner.run_mutations(
        [definition],
        repo_root=root,
        allow_dirty=True,
    )
    assert results == []
    assert exit_code == 3
    assert "run --restore" in errors[0]
    assert target.read_bytes() == b"MUTATED\n"

    assert runner.main(["--restore"], repo_root=root) == 0
    assert target.read_bytes() == original
    assert not marker.exists()
    assert not backup.exists()


def test_restore_is_byte_exact_for_crlf_source(tmp_path: Path) -> None:
    contents = b"before\r\nORIGINAL\r\nafter\r\n"
    root, target, definition, fail_runner = project(tmp_path, contents)
    results, exit_code, errors = execute(root, definition, fail_runner)
    assert not errors
    assert exit_code == 0, (results, errors)
    assert results[0].status == "KILLED"
    assert target.read_bytes() == contents
