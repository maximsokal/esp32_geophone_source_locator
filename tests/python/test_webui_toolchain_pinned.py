"""MAX-388: the WebUI test runner must come from the locked dependency graph.

`npm ci` is the single installation authority: Vitest and its coverage
provider must be exact devDependencies recorded in package-lock.json, and the
test scripts must invoke the local binary instead of `npx --yes <remote pkg>`.
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui"
PACKAGE = json.loads((WEBUI / "package.json").read_text(encoding="utf-8"))
LOCK = json.loads((WEBUI / "package-lock.json").read_text(encoding="utf-8"))
EXACT = re.compile(r"^\d+\.\d+\.\d+$")


def test_vitest_and_coverage_are_exact_dev_dependencies():
    dev = PACKAGE.get("devDependencies", {})
    for name in ("vitest", "@vitest/coverage-v8"):
        assert name in dev, f"{name} must be a devDependency"
        assert EXACT.match(dev[name]), f"{name} must be pinned exactly, got {dev[name]!r}"
    assert dev["vitest"] == dev["@vitest/coverage-v8"], "vitest and its coverage provider must match"


def test_lockfile_records_the_pinned_runner_packages():
    dev = PACKAGE["devDependencies"]
    packages = LOCK["packages"]
    root_dev = packages[""].get("devDependencies", {})
    for name in ("vitest", "@vitest/coverage-v8"):
        assert root_dev.get(name) == dev[name]
        entry = packages.get(f"node_modules/{name}")
        assert entry is not None, f"{name} missing from package-lock.json"
        assert entry["version"] == dev[name]
        assert entry.get("resolved") and entry.get("integrity")


def test_test_scripts_do_not_install_remote_tooling():
    scripts = PACKAGE["scripts"]
    for name in ("test:console", "test:xr", "test:xr:coverage"):
        command = scripts[name]
        assert "npx" not in command, f"{name} must not use npx: {command[:80]}"
        assert "--yes" not in command, f"{name} must not allow surprise installs"
        assert "--package=" not in command
        assert "vitest" in command
