from __future__ import annotations

import importlib.util
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("gen_build_identity", ROOT / "tools" / "gen_build_identity.py")
assert _spec is not None and _spec.loader is not None
tool = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = tool
_spec.loader.exec_module(tool)

COMMIT = re.compile(r"^[0-9a-f]{12}$")
DIRTY = re.compile(r"^[0-9a-f]{12}\+dirty\.[0-9a-f]{8}$")


def put(path: Path, text: str) -> None:
    path.write_bytes(text.encode())     # bytes: write_text would rewrite the line ends on Windows and dirty the tree


def git(repo: Path, *args: str) -> str:
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    return subprocess.run(["git", *args], cwd=repo, env=env, check=True, capture_output=True, text=True).stdout.strip()


@pytest.fixture()
def repo(tmp_path: Path) -> Path:
    git(tmp_path, "init", "-q")
    put(tmp_path / "a.c", "int a;\n")
    put(tmp_path / "b.c", "int b;\n")
    git(tmp_path, "add", ".")
    git(tmp_path, "commit", "-q", "-m", "first")
    return tmp_path


def test_a_clean_tree_is_its_commit(repo: Path):
    value = tool.identity(repo)
    assert COMMIT.match(value)
    assert git(repo, "rev-parse", "HEAD").startswith(value)


def test_an_edit_makes_the_identity_dirty_and_a_different_edit_a_different_identity(repo: Path):
    clean = tool.identity(repo)
    put(repo / "a.c", "int a = 1;\n")
    first = tool.identity(repo)
    assert DIRTY.match(first) and first.startswith(clean)
    assert tool.identity(repo) == first                      # same state, same identity
    put(repo / "a.c", "int a = 2;\n")
    second = tool.identity(repo)
    assert DIRTY.match(second) and second != first           # two uncommitted states never share an identity
    put(repo / "b.c", "int b = 1;\n")
    assert tool.identity(repo) not in (first, second)        # a second changed file counts too


def test_reverting_the_edit_gives_the_clean_identity_back(repo: Path):
    clean = tool.identity(repo)
    put(repo / "a.c", "int a = 1;\n")
    assert tool.identity(repo) != clean
    put(repo / "a.c", "int a;\n")
    assert tool.identity(repo) == clean


def test_a_file_that_was_only_touched_or_resaved_is_not_dirty(repo: Path):
    # Bench 2026-11-03: the firmware said "<commit>+dirty.e3b0c442" (the hash of an empty diff) because git diff-index reports
    # a file whose stamp or line ends changed even though its content does not. The identity follows the diff.
    clean = tool.identity(repo)
    time.sleep(1.1)
    (repo / "a.c").write_bytes((repo / "a.c").read_bytes())          # same bytes, new stamp
    assert tool.identity(repo) == clean
    git(repo, "config", "core.autocrlf", "true")
    (repo / "a.c").write_bytes(b"int a;" + bytes([13, 10]))      # the same text with Windows line ends
    assert tool.identity(repo) == clean
    assert "e3b0c442" not in tool.identity(repo)


def test_a_commit_changes_the_commit_part_and_clears_dirty(repo: Path):
    before = tool.identity(repo)
    put(repo / "a.c", "int a = 1;\n")
    git(repo, "commit", "-q", "-am", "second")
    after = tool.identity(repo)
    assert COMMIT.match(after) and after != before           # the stale-version case: HEAD moved, the identity follows


def test_untracked_files_do_not_make_it_dirty(repo: Path):
    clean = tool.identity(repo)
    put(repo / "scratch.txt", "notes\n")
    assert tool.identity(repo) == clean


def test_a_directory_that_is_not_a_repository_is_unknown(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GIT_CEILING_DIRECTORIES", str(tmp_path.parent))   # the test directory sits inside this repository
    assert tool.identity(tmp_path) == "unknown"
    git(tmp_path, "init", "-q")                              # a repository without a commit has no HEAD either
    assert tool.identity(tmp_path) == "unknown"


def test_the_generated_file_declares_the_symbol_and_is_rewritten_only_on_change(repo: Path, tmp_path_factory):
    out = tmp_path_factory.mktemp("out") / "app_build_identity.c"
    assert tool.main(["--repo", str(repo), "--out", str(out)]) == 0
    text = out.read_text()
    assert '#include "app_build_identity.h"' in text
    assert re.search(r'const char app_build_identity\[\] = "[0-9a-f]{12}";', text)
    stamp = out.stat().st_mtime_ns
    assert tool.main(["--repo", str(repo), "--out", str(out)]) == 0
    assert out.stat().st_mtime_ns == stamp                   # unchanged tree: the file is not touched, no rebuild
    put(repo / "a.c", "int a = 9;\n")
    assert tool.main(["--repo", str(repo), "--out", str(out)]) == 0
    assert re.search(r'= "[0-9a-f]{12}\+dirty\.[0-9a-f]{8}";', out.read_text())
    assert out.stat().st_mtime_ns != stamp


def test_the_script_runs_from_the_command_line(repo: Path, tmp_path_factory):
    out = tmp_path_factory.mktemp("cli") / "nested" / "app_build_identity.c"
    done = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "gen_build_identity.py"),
                           "--repo", str(repo), "--out", str(out)], capture_output=True, text=True, check=True)
    assert "build identity" in done.stdout and out.exists()


def test_the_value_is_rendered_without_anything_that_needs_c_escaping():
    with pytest.raises(AssertionError):
        tool.render('abc"def')
    assert 'app_build_identity[] = "unknown";' in tool.render("unknown")


def test_the_firmware_build_uses_it_and_logs_it():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
    assert "gen_build_identity.py" in cmake and "add_custom_target(app_build_identity ALL" in cmake
    assert "target_sources(${COMPONENT_LIB} PRIVATE" in cmake and "add_dependencies(${COMPONENT_LIB} app_build_identity)" in cmake
    main_c = (ROOT / "main" / "main.c").read_text()
    assert '#include "app_build_identity.h"' in main_c
    assert 'ESP_LOGI(TAG, "FW_IDENTITY %s", app_build_identity);' in main_c
    assert "extern const char app_build_identity[];" in (ROOT / "main" / "app_build_identity.h").read_text()
    assert "app_build_identity.c" not in cmake.split("idf_component_register(")[1].split(")")[0]   # a build output, not a source
