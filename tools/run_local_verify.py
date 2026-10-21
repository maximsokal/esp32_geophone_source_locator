#!/usr/bin/env python3
"""Run deterministic repository verification locally without GitHub Actions."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
from typing import Iterable, Sequence


SCOPES = ("software", "ast", "rtos-evidence", "host", "python", "web", "ui", "nrf", "firmware", "all")


class VerificationError(RuntimeError):
    """Local verification cannot be planned or executed safely."""


@dataclass(frozen=True)
class Step:
    name: str
    cwd: Path
    command: tuple[str, ...]
    skip_reason: str | None = None
    environment: dict[str, str] | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scope",
        choices=SCOPES,
        default="software",
        help="verification surface; ui = opt-in native UI + production browser smoke and report (not added to software/all); software = host + python + web + ast; ast = structural rule tests + scan; nrf = isolated nRF54L15 host policy + canonical NCS build/test-build boundary; rtos-evidence = fast evidence-matrix check (included by all, not software/default)",
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
            "python/apriltag-sheet",
            root,
            (python_executable, str(root / "tools" / "apriltag_sheet" / "apriltag_sheet.py"), "check"),
        ),
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


def ast_steps(root: Path) -> list[Step]:
    executable = find_required_executable(("ast-grep",), "ast-grep verification")
    config = str(root / "sgconfig.yml")
    return [
        Step(
            "ast/rules", root,
            (executable, "test", "--config", config, "--skip-snapshot-tests"),
        ),
        Step(
            "ast/scan", root,
            (executable, "scan", "--config", config,
             "main", "components", "nRF54L15/src", "webui/src"),
        ),
    ]


def rtos_evidence_steps(root: Path, python_executable: str) -> list[Step]:
    runner = root / "tools" / "rtos_evidence" / "check_rtos_evidence.py"
    return [Step("rtos-evidence", root, (python_executable, str(runner)))]


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
        ("web/test-types", "typecheck:tests"),
        ("web/console", "test:console"),
        ("web/domain", "test:domain"),
        ("web/components", "test:components"),
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


def ui_steps(root: Path, python_executable: str, npm_executable: str,
             *, skip_npm_ci: bool, output: Path) -> list[Step]:
    steps = [
        Step('ui-native/bootstrap', root, (python_executable, str(root / 'tools/prepare_ui_render.py'))),
        Step('ui-native/tests', root, (python_executable, '-m', 'pytest', '-q',
             'tests/python/test_ui_render_runner.py', 'tests/python/test_ui_render.py',
             '--basetemp', str(output / 'native'), '--junitxml', str(output / 'native.xml'))),
    ]
    webui = root / 'webui'
    if not skip_npm_ci:
        steps.append(Step('ui-web/npm-ci', webui, (npm_executable, 'ci', '--no-audit', '--no-fund')))
    for name, script in (('typecheck', 'typecheck'), ('test-types', 'typecheck:tests'),
                         ('console', 'test:console'), ('domain', 'test:domain'),
                         ('components', 'test:components'), ('e2e-types', 'typecheck:e2e'), ('smoke', 'test:ui:smoke'),
                         ('bundle', 'verify:bundle')):
        env = {'PLAYWRIGHT_JSON_OUTPUT_FILE': str(output / 'web/playwright.json'),
               'PLAYWRIGHT_HTML_OUTPUT_DIR': str(output / 'web/html')}
        command = (npm_executable, 'run', script)
        if name == 'smoke':
            command += ('--', '--output', str(output / 'web/results'))
        if name in ('components', 'domain'):
            command += ('--', '--reporter=default', '--reporter=junit', '--outputFile', str(output / f'{name}.xml'))
        steps.append(Step(f'ui-web/{name}', webui, command, environment=env if name == 'smoke' else None))
    return steps


def run_ui_verification(root: Path, python_executable: str, npm_executable: str,
                        *, skip_npm_ci: bool) -> int:
    import ui_report

    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = root / '.test-build/ui-regression' / run_id
    started = datetime.now(timezone.utc).timestamp()
    ui_report.initialize(output)
    failed_groups: set[str] = set()
    execution = []
    for step in ui_steps(root, python_executable, npm_executable, skip_npm_ci=skip_npm_ci, output=output):
        group = step.name.split('/')[0]
        if group in failed_groups:
            execution.append({'step': step.name, 'status': 'NOT RUN', 'reason': 'Earlier prerequisite failed'})
            continue
        try:
            status = run_step(step)
            execution.append({'step': step.name, 'status': status})
        except (OSError, VerificationError) as exc:
            failed_groups.add(group)
            execution.append({'step': step.name, 'status': 'FAIL', 'reason': str(exc)})
    build_dir = root / '.test-build/ui-render/native'
    for name in ('configure.log', 'build.log', 'build.json'):
        source = build_dir / name
        if source.is_file() and source.stat().st_mtime >= started:
            (output / 'native-build').mkdir(exist_ok=True)
            shutil.copy2(source, output / 'native-build' / name)
    import json
    (output / 'execution.json').write_text(json.dumps(execution, indent=2), encoding='utf-8')
    report_exit = ui_report.main([
        '--output', str(output), '--identity', str(output / 'identity.json'),
        '--web', str(output / 'web/playwright.json'), '--web-exit-code', '1' if 'ui-web' in failed_groups else '0',
        '--native-junit', str(output / 'native.xml'), '--native-root', str(output / 'native'),
        '--native-exit-code', '1' if 'ui-native' in failed_groups else '0',
        '--components-junit', str(output / 'components.xml'), '--domain-junit', str(output / 'domain.xml'),
    ])
    result = 1 if failed_groups or report_exit else 0
    print(f"LOCAL_VERIFY_RESULT: {'FAIL' if result else 'PASS'} (UI artifacts: {output})", flush=True)
    return result


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
        return surface in {"host", "python", "web", "ast"}
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
    if scope == 'ui':
        if npm_executable is None:
            raise VerificationError('UI verification requires npm')
        return ui_steps(root, python_executable, npm_executable, skip_npm_ci=skip_npm_ci,
                        output=root / '.test-build/ui-regression/RUN_ID')
    steps: list[Step] = []
    if scope_includes(scope, "host"):
        steps.extend(host_steps(root, python_executable))
    if scope_includes(scope, "ast"):
        steps.extend(ast_steps(root))
    if scope_includes(scope, "rtos-evidence"):
        steps.extend(rtos_evidence_steps(root, python_executable))
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
    env = {**os.environ, **step.environment} if step.environment is not None else None
    result = subprocess.run(step.command, cwd=step.cwd, check=False, env=env)
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
        if scope_includes(args.scope, "web") or args.scope == 'ui':
            npm_executable = find_required_executable(
                ("npm.cmd", "npm") if system_name == "Windows" else ("npm",),
                "WebUI verification",
            )

        if args.scope == 'ui' and not args.dry_run:
            assert npm_executable is not None
            return run_ui_verification(root, sys.executable, npm_executable, skip_npm_ci=args.skip_npm_ci)

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
