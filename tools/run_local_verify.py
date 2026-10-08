#!/usr/bin/env python3
"""Run deterministic repository verification locally without GitHub Actions."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
from typing import Iterable, Sequence


SCOPES = ("software", "host", "python", "web", "nrf", "firmware", "all")


class VerificationError(RuntimeError):
    """Local verification cannot be planned or executed safely."""


@dataclass(frozen=True)
class Step:
    name: str
    cwd: Path
    command: tuple[str, ...]
    skip_reason: str | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scope",
        choices=SCOPES,
        default="software",
        help="verification surface; software = host + python + web; nrf = isolated nRF54L15 host policy + canonical NCS build/test-build boundary",
    )
    parser.add_argument(
        "--project-path",
        type=Path,
        help="repository root; defaults to the parent of tools/",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="ESP-IDF build directory used by firmware scope",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=2,
        help="parallel jobs for the ESP-IDF firmware build",
    )
    parser.add_argument(
        "--skip-npm-ci",
        action="store_true",
        help="reuse current webui/node_modules instead of running npm ci",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the local commands without executing them",
    )
    return parser.parse_args()


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_repo_root(project_path: Path | None) -> Path:
    root = (project_path or default_repo_root()).resolve()
    required = (
        root / "CMakeLists.txt",
        root / "tools" / "run_host_tests.py",
        root / "tests" / "host" / "manifest.json",
        root / "tests" / "python",
        root / "webui" / "package.json",
    )
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise VerificationError(
            "project path is not a complete repository root; missing: "
            + ", ".join(missing)
        )
    return root


def quote_command(command: Sequence[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(list(command))
    return shlex.join(command)


def host_steps(root: Path, python_executable: str) -> list[Step]:
    runner = root / "tools" / "run_host_tests.py"
    steps = [
        Step(
            "host/base",
            root,
            (
                python_executable,
                str(runner),
                "--keep-going",
            ),
        )
    ]

    manifest_dir = root / "tests" / "host" / "manifest.d"
    if manifest_dir.is_dir():
        for manifest in sorted(manifest_dir.glob("*.json")):
            suite = manifest.stem
            steps.append(
                Step(
                    f"host/{suite}",
                    root,
                    (
                        python_executable,
                        str(runner),
                        "--manifest",
                        str(manifest),
                        "--build-dir",
                        str(root / ".test-build" / f"host-tests-{suite}"),
                        "--keep-going",
                    ),
                )
            )
    return steps


def python_steps(root: Path, python_executable: str) -> list[Step]:
    return [
        Step(
            "python/deterministic",
            root,
            (
                python_executable,
                "-m",
                "pytest",
                "-q",
                str(root / "tests" / "python"),
            ),
        )
    ]


def web_steps(
    root: Path,
    npm_executable: str,
    *,
    skip_npm_ci: bool,
) -> list[Step]:
    webui = root / "webui"
    steps: list[Step] = []
    if not skip_npm_ci:
        steps.append(
            Step(
                "web/npm-ci",
                webui,
                (npm_executable, "ci", "--no-audit", "--no-fund"),
            )
        )

    for name, script in (
        ("web/typecheck", "typecheck"),
        ("web/console", "test:console"),
        ("web/xr", "test:xr"),
        ("web/build", "build"),
        ("web/bundle", "verify:bundle"),
    ):
        steps.append(
            Step(
                name,
                webui,
                (npm_executable, "run", script),
            )
        )
    return steps


def firmware_steps(
    root: Path,
    *,
    build_dir: str,
    jobs: int,
    system_name: str,
    powershell_executable: str | None,
    idf_executable: str | None,
) -> list[Step]:
    if jobs < 1 or jobs > 32:
        raise VerificationError("--jobs must be in range 1..32")

    if system_name == "Windows":
        if powershell_executable is None:
            raise VerificationError(
                "firmware scope on Windows requires powershell.exe or pwsh"
            )
        runner = root / "tools" / "run_idf.ps1"
        if not runner.is_file():
            raise VerificationError(f"missing ESP-IDF helper: {runner}")
        command: tuple[str, ...] = (
            powershell_executable,
            "-NoLogo",
            "-NoProfile",
        )
        if Path(powershell_executable).name.lower() == "powershell.exe":
            command += ("-ExecutionPolicy", "Bypass")
        command += (
            "-File",
            str(runner),
            "-Action",
            "build",
            "-ProjectPath",
            str(root),
            "-BuildDir",
            build_dir,
            "-Jobs",
            str(jobs),
        )
        return [Step("firmware/esp-idf", root, command)]

    if idf_executable is None:
        raise VerificationError(
            "firmware scope requires idf.py on PATH outside Windows; "
            "GitHub Actions are intentionally not used as a fallback"
        )
    return [
        Step(
            "firmware/esp-idf",
            root,
            (
                idf_executable,
                "-B",
                str(root / build_dir),
                "build",
            ),
        )
    ]


def nrf_host_policy_step(root: Path, python_executable: str) -> Step:
    runner = root / "tools" / "run_host_tests.py"
    manifest = root / "tests" / "host" / "manifest.d" / "coverage-nrf.json"
    if not runner.is_file():
        raise VerificationError(f"missing host test helper: {runner}")
    if not manifest.is_file():
        raise VerificationError(f"missing nRF host coverage manifest: {manifest}")

    return Step(
        "nrf/host-policy",
        root,
        (
            python_executable,
            str(runner),
            "--manifest",
            str(manifest),
            "--build-dir",
            str(root / ".test-build" / "host-tests-coverage-nrf"),
            "--keep-going",
        ),
    )


def nrf_steps(
    root: Path,
    *,
    python_executable: str,
    system_name: str,
    powershell_executable: str | None,
    include_host_policy: bool = True,
) -> list[Step]:
    if system_name != "Windows":
        raise VerificationError(
            "nRF54L15 verification currently requires the Windows path-safe "
            "nRF54L15/ncs.ps1 NCS v3.4.0 wrapper"
        )
    if powershell_executable is None:
        raise VerificationError(
            "nRF54L15 verification on Windows requires powershell.exe or pwsh"
        )

    runner = root / "nRF54L15" / "ncs.ps1"
    if not runner.is_file():
        raise VerificationError(f"missing canonical nRF54L15 NCS helper: {runner}")

    command_prefix: tuple[str, ...] = (
        powershell_executable,
        "-NoLogo",
        "-NoProfile",
    )
    if Path(powershell_executable).name.lower() == "powershell.exe":
        command_prefix += ("-ExecutionPolicy", "Bypass")

    steps: list[Step] = []
    if include_host_policy:
        steps.append(nrf_host_policy_step(root, python_executable))
    steps.extend(
        [
            Step(
                "nrf/build",
                root,
                command_prefix
                + ("-File", str(runner), "-Action", "build"),
            ),
            Step(
                "nrf/tests",
                root,
                command_prefix
                + ("-File", str(runner), "-Action", "ncs-test-build"),
            ),
            Step(
                "nrf/cs-tests",
                root,
                command_prefix
                + ("-File", str(runner), "-Action", "ncs-cs-test-build"),
            ),
        ]
    )
    return steps


def scope_includes(scope: str, surface: str) -> bool:
    if scope == "all":
        return True
    if scope == "software":
        return surface in {"host", "python", "web"}
    return scope == surface


def find_required_executable(candidates: Iterable[str], purpose: str) -> str:
    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
    raise VerificationError(
        f"required executable for {purpose} was not found: "
        + ", ".join(candidates)
    )


def build_plan(
    root: Path,
    *,
    scope: str,
    build_dir: str,
    jobs: int,
    skip_npm_ci: bool,
    python_executable: str,
    npm_executable: str | None,
    system_name: str,
    powershell_executable: str | None,
    idf_executable: str | None,
) -> list[Step]:
    steps: list[Step] = []
    if scope_includes(scope, "host"):
        steps.extend(host_steps(root, python_executable))
    if scope_includes(scope, "python"):
        steps.extend(python_steps(root, python_executable))
    if scope_includes(scope, "web"):
        if npm_executable is None:
            raise VerificationError("web verification requires npm")
        steps.extend(
            web_steps(
                root,
                npm_executable,
                skip_npm_ci=skip_npm_ci,
            )
        )
    if scope_includes(scope, "nrf"):
        steps.extend(
            nrf_steps(
                root,
                python_executable=python_executable,
                system_name=system_name,
                powershell_executable=powershell_executable,
                include_host_policy=scope == "nrf",
            )
        )
    if scope_includes(scope, "firmware"):
        steps.extend(
            firmware_steps(
                root,
                build_dir=build_dir,
                jobs=jobs,
                system_name=system_name,
                powershell_executable=powershell_executable,
                idf_executable=idf_executable,
            )
        )
    return steps


def run_step(step: Step) -> str:
    print(f"\n=== {step.name} ===", flush=True)
    if step.skip_reason is not None:
        print(f"SKIPPED: {step.skip_reason}", flush=True)
        return "SKIPPED"

    print(f"cwd: {step.cwd}", flush=True)
    print(f"> {quote_command(step.command)}", flush=True)
    result = subprocess.run(step.command, cwd=step.cwd, check=False)
    if result.returncode != 0:
        raise VerificationError(
            f"step failed with exit code {result.returncode}: {step.name}"
        )
    print(f"{step.name}: PASS", flush=True)
    return "PASS"


def main() -> int:
    args = parse_args()
    try:
        root = resolve_repo_root(args.project_path)
        system_name = platform.system()

        npm_executable = None
        if scope_includes(args.scope, "web"):
            npm_executable = find_required_executable(
                ("npm.cmd", "npm") if system_name == "Windows" else ("npm",),
                "WebUI verification",
            )

        powershell_executable = None
        idf_executable = None
        if scope_includes(args.scope, "firmware") or scope_includes(args.scope, "nrf"):
            if system_name == "Windows":
                powershell_executable = find_required_executable(
                    ("powershell.exe", "pwsh"),
                    "local ESP-IDF/nRF54L15 verification",
                )
            else:
                idf_executable = shutil.which("idf.py")

        plan = build_plan(
            root,
            scope=args.scope,
            build_dir=args.build_dir,
            jobs=args.jobs,
            skip_npm_ci=args.skip_npm_ci,
            python_executable=sys.executable,
            npm_executable=npm_executable,
            system_name=system_name,
            powershell_executable=powershell_executable,
            idf_executable=idf_executable,
        )

        print(
            f"Local verification: scope={args.scope} steps={len(plan)} "
            f"repo={root}",
            flush=True,
        )
        print("GitHub Actions: not used", flush=True)

        if args.dry_run:
            for step in plan:
                if step.skip_reason is not None:
                    print(f"[{step.name}] SKIPPED: {step.skip_reason}")
                else:
                    print(f"[{step.name}] ({step.cwd}) {quote_command(step.command)}")
            return 0

        statuses = [run_step(step) for step in plan]
        skipped = sum(status == "SKIPPED" for status in statuses)
        if skipped:
            print(
                f"\nLOCAL_VERIFY_RESULT: PASS_WITH_SKIPS ({skipped} skipped step(s))",
                flush=True,
            )
        else:
            print("\nLOCAL_VERIFY_RESULT: PASS", flush=True)
        return 0
    except (OSError, VerificationError) as exc:
        print(f"LOCAL_VERIFY_RESULT: FAIL: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
