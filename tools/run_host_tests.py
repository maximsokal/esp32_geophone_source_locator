#!/usr/bin/env python3
"""Build and run the repository's standalone native C tests."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from typing import Any


DEFAULT_COMPILER_CANDIDATES = ("clang", "gcc", "cc", "cl")
TEST_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


class RunnerError(RuntimeError):
    """A malformed manifest or invalid runner configuration."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path("tests/host/manifest.json"))
    parser.add_argument("--build-dir", type=Path, default=Path("build/host-tests"))
    parser.add_argument("--compiler", type=Path, help="native compiler; otherwise HOST_CC/CC/PATH")
    parser.add_argument(
        "--sanitizers",
        choices=("none", "address", "undefined", "address,undefined"),
        default="none",
        help="optional sanitizer mode; unsupported compilers fail explicitly",
    )
    parser.add_argument("--coverage", action="store_true", help="enable compiler coverage instrumentation")
    parser.add_argument("--list", action="store_true", help="validate and list tests without compiling")
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="NAME",
        help="run only the named manifest test; repeat the option to select several tests",
    )
    parser.add_argument("--keep-going", action="store_true", help="run remaining tests after a failure")
    return parser.parse_args()


def resolve_repo_path(repo_root: Path, relative: str, description: str) -> Path:
    candidate = (repo_root / relative).resolve()
    try:
        candidate.relative_to(repo_root)
    except ValueError as exc:
        raise RunnerError(f"{description} escapes repository root: {relative}") from exc
    return candidate


def load_manifest(path: Path, repo_root: Path) -> list[dict[str, Any]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RunnerError(f"cannot read manifest {path}: {exc}") from exc
    if not isinstance(raw, dict) or raw.get("version") != 1:
        raise RunnerError("manifest must be an object with version 1")
    entries = raw.get("tests")
    if not isinstance(entries, list) or not entries:
        raise RunnerError("manifest tests must be a non-empty array")

    validated: list[dict[str, Any]] = []
    names: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise RunnerError(f"manifest test {index} must be an object")
        name = entry.get("name")
        sources = entry.get("sources")
        include_dirs = entry.get("include_dirs", [])
        defines = entry.get("defines", [])
        libraries = entry.get("libraries", [])
        if not isinstance(name, str) or not TEST_NAME.fullmatch(name):
            raise RunnerError(f"manifest test {index} has invalid name: {name!r}")
        if name in names:
            raise RunnerError(f"duplicate manifest test name: {name}")
        if not isinstance(sources, list) or not sources or not all(isinstance(item, str) for item in sources):
            raise RunnerError(f"manifest test {name} sources must be a non-empty array")
        if not isinstance(include_dirs, list) or not all(isinstance(item, str) for item in include_dirs):
            raise RunnerError(f"manifest test {name} include_dirs must be an array")
        if not isinstance(defines, list) or not all(isinstance(item, str) for item in defines):
            raise RunnerError(f"manifest test {name} defines must be an array")
        if not isinstance(libraries, list) or not all(isinstance(item, str) for item in libraries):
            raise RunnerError(f"manifest test {name} libraries must be an array")

        resolved_sources = [resolve_repo_path(repo_root, item, f"source for {name}") for item in sources]
        resolved_includes = [
            resolve_repo_path(repo_root, item, f"include directory for {name}") for item in include_dirs
        ]
        for source in resolved_sources:
            if not source.is_file():
                raise RunnerError(f"manifest test {name} source does not exist: {source}")
        for include_dir in resolved_includes:
            if not include_dir.is_dir():
                raise RunnerError(f"manifest test {name} include directory does not exist: {include_dir}")
        names.add(name)
        validated.append(
            {
                "name": name,
                "sources": resolved_sources,
                "include_dirs": resolved_includes,
                "defines": defines,
                "libraries": libraries,
            }
        )
    return validated


def compiler_from_args(requested: Path | None) -> list[str] | None:
    if requested is not None:
        if not requested.is_file():
            raise RunnerError(f"compiler does not exist: {requested}")
        return [str(requested)]
    for variable in ("HOST_CC", "CC"):
        value = os.environ.get(variable)
        if value:
            direct_path = Path(value.strip('"'))
            if direct_path.is_file():
                return [str(direct_path)]
            parts = shlex.split(value, posix=os.name != "nt")
            if parts and shutil.which(parts[0]):
                return parts
            raise RunnerError(f"{variable} points to an unavailable compiler: {value}")
    for candidate in DEFAULT_COMPILER_CANDIDATES:
        found = shutil.which(candidate)
        if found:
            return [found]
    # Visual Studio Build Tools is often installed without adding cl.exe to
    # the user's PATH. Discover the x64 compiler without changing global
    # environment state; the runner loads its vcvars file for each run.
    for root_name in ("ProgramFiles(x86)", "ProgramFiles"):
        root_value = os.environ.get(root_name)
        if not root_value:
            continue
        root = Path(root_value) / "Microsoft Visual Studio"
        candidates = sorted(
            root.glob("*/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"),
            reverse=True,
        )
        if candidates:
            return [str(candidates[0])]
    return None


def compiler_description(compiler: list[str]) -> str:
    if Path(compiler[0]).name.lower() in {"cl.exe", "clang-cl.exe"}:
        return "MSVC cl.exe"
    else:
        result = subprocess.run(
            [*compiler, "--version"], capture_output=True, text=True, encoding="utf-8", errors="replace", check=False
        )
    lines = (result.stdout or result.stderr).splitlines()
    return lines[0] if lines else shlex.join(compiler)


def ensure_native_compiler(compiler: list[str], description: str) -> None:
    """Reject ESP-IDF's target compiler before producing misleading failures."""

    if Path(compiler[0]).name.lower() in {"cl.exe", "clang-cl.exe"}:
        return

    target_text = description.lower()
    details = subprocess.run(
        [*compiler, "-dumpmachine"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    target_text += " " + details.stdout.lower()
    if "esp-unknown-elf" in target_text or "riscv32-esp" in target_text or "xtensa-esp" in target_text:
        raise RunnerError(
            "the selected compiler targets ESP-IDF firmware, not the host; "
            "select a native clang/gcc/MSVC compiler"
        )


def sanitizer_flags(mode: str) -> list[str]:
    if mode == "none":
        return []
    return [f"-fsanitize={mode}", "-fno-omit-frame-pointer"]


def is_msvc(compiler: list[str]) -> bool:
    return Path(compiler[0]).name.lower() in {"cl.exe", "clang-cl.exe"}


def is_msvc_cl(compiler: list[str]) -> bool:
    """True only for Microsoft's cl.exe, not the clang-cl compatible driver."""

    return Path(compiler[0]).name.lower() == "cl.exe"


def load_msvc_environment(compiler: list[str]) -> dict[str, str]:
    """Load the Visual C++ x64 environment without mutating the parent shell."""

    compiler_path = Path(compiler[0]).resolve()
    vcvars = next(
        (
            parent / "Auxiliary" / "Build" / "vcvars64.bat"
            for parent in compiler_path.parents
            if (parent / "Auxiliary" / "Build" / "vcvars64.bat").is_file()
        ),
        None,
    )
    if vcvars is None:
        raise RunnerError(f"cannot find vcvars64.bat for {compiler[0]}")
    # Passing a command containing an embedded quoted Windows path as the
    # final item of a subprocess argument list makes Python escape those
    # quotes for ``cmd.exe`` (``\"``), which cmd then treats as part of the
    # filename.  Use the native shell for this one fixed, tool-discovered
    # command so the batch-file quoting has its normal cmd semantics.
    command = f'call "{vcvars}" >nul && set'
    result = subprocess.run(
        command,
        shell=True,
        capture_output=True,
        text=True,
        encoding="mbcs" if os.name == "nt" else "utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise RunnerError(f"vcvars64.bat failed with exit code {result.returncode}")
    environment = os.environ.copy()
    for line in result.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            environment[key] = value
    return environment


def run_command(
    command: list[str],
    timeout: float = 60.0,
    *,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
        cwd=cwd,
        env=environment,
    )


def print_process_diagnostics(result: dict[str, Any]) -> None:
    """Show compiler/runtime diagnostics regardless of the process stream used."""

    for stream_name in ("stdout", "stderr"):
        output = result.get(stream_name)
        if output:
            print(f"{stream_name.upper()}:", file=sys.stderr)
            print(output, file=sys.stderr, end="" if output.endswith("\n") else "\n")


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    manifest_path = args.manifest if args.manifest.is_absolute() else repo_root / args.manifest
    tests = load_manifest(manifest_path.resolve(), repo_root)
    if args.only:
        available = {entry["name"] for entry in tests}
        unknown = sorted(set(args.only) - available)
        if unknown:
            raise RunnerError(
                "--only names are not present in the manifest: " + ", ".join(unknown)
            )
        selected = set(args.only)
        tests = [entry for entry in tests if entry["name"] in selected]
        if not tests:
            raise RunnerError("--only selected no tests")
    if args.list:
        for entry in tests:
            print(f"{entry['name']}: {len(entry['sources'])} source files")
        print(f"Listed {len(tests)} host tests from {manifest_path}")
        return 0

    compiler = compiler_from_args(args.compiler)
    if compiler is None:
        print(
            "HOST TESTS UNSUPPORTED: no native C compiler found. Install/activate clang, gcc, or MSVC, or set HOST_CC.",
            file=sys.stderr,
        )
        return 2

    build_dir = args.build_dir if args.build_dir.is_absolute() else repo_root / args.build_dir
    build_dir = build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    report_path = build_dir / "host-test-report.json"
    description = compiler_description(compiler)
    ensure_native_compiler(compiler, description)
    msvc = is_msvc(compiler)
    if msvc and args.coverage:
        raise RunnerError("coverage is not supported by the MSVC host runner; select native clang/gcc")
    if msvc and args.sanitizers in {"undefined", "address,undefined"}:
        raise RunnerError("UBSan is not supported by the MSVC host runner; select native clang/gcc")
    if msvc:
        common_flags = ["/nologo", "/std:c11", "/W4", "/WX", "/UNDEBUG", "/D_CRT_SECURE_NO_WARNINGS"]
        if is_msvc_cl(compiler):
            # MSVC's C11 <stdatomic.h> implementation remains opt-in. Keep
            # the production atomic API in host tests instead of replacing it
            # with a fake header.
            common_flags.append("/experimental:c11atomics")
        if args.sanitizers == "address":
            common_flags.append("/fsanitize=address")
    else:
        common_flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG", *sanitizer_flags(args.sanitizers)]
        if args.coverage:
            common_flags.append("--coverage")
    compiler_environment = load_msvc_environment(compiler) if msvc else None
    print(f"Compiler: {description}")
    print(f"Manifest: {manifest_path}")
    print(f"Tests: {len(tests)}")

    results: list[dict[str, Any]] = []
    for entry in tests:
        entry_dir = build_dir / entry["name"]
        entry_dir.mkdir(parents=True, exist_ok=True)
        output = entry_dir / (entry["name"] + (".exe" if os.name == "nt" else ""))
        if msvc:
            command = [
                *compiler,
                *common_flags,
                *(f"/I{directory}" for directory in entry["include_dirs"]),
                *(f"/D{define}" for define in entry.get("defines", [])),
                *(str(source) for source in entry["sources"]),
                f"/Fe:{output}",
            ]
        else:
            command = [
                *compiler,
                *common_flags,
                *(f"-I{directory}" for directory in entry["include_dirs"]),
                *(f"-D{define}" for define in entry.get("defines", [])),
                *(str(source) for source in entry["sources"]),
                *(f"-l{library}" for library in entry.get("libraries", [])),
                "-o",
                str(output),
            ]
        print(f"[BUILD] {entry['name']}: {shlex.join(command)}")
        try:
            compile_result = run_command(command, cwd=entry_dir, environment=compiler_environment)
        except (OSError, subprocess.TimeoutExpired) as exc:
            result = {"name": entry["name"], "status": "FAIL", "phase": "compile", "error": str(exc)}
        else:
            if compile_result.returncode != 0:
                result = {
                    "name": entry["name"],
                    "status": "FAIL",
                    "phase": "compile",
                    "exit_code": compile_result.returncode,
                    "stderr": compile_result.stderr,
                    "stdout": compile_result.stdout,
                }
            else:
                print(f"[RUN]   {entry['name']}")
                try:
                    test_result = run_command([str(output)], cwd=entry_dir, environment=compiler_environment)
                except (OSError, subprocess.TimeoutExpired) as exc:
                    result = {"name": entry["name"], "status": "FAIL", "phase": "run", "error": str(exc)}
                else:
                    result = {
                        "name": entry["name"],
                        "status": "PASS" if test_result.returncode == 0 else "FAIL",
                        "phase": "run",
                        "exit_code": test_result.returncode,
                        "stdout": test_result.stdout,
                        "stderr": test_result.stderr,
                    }
        results.append(result)
        if result["status"] == "PASS":
            print(f"[PASS] {entry['name']}")
        else:
            print(f"[FAIL] {entry['name']} ({result['phase']})", file=sys.stderr)
            print_process_diagnostics(result)
            if not args.keep_going:
                break

    passed = sum(result["status"] == "PASS" for result in results)
    failed = len(results) - passed
    report = {
        "manifest": str(manifest_path),
        "only": args.only,
        "compiler": compiler,
        "compiler_description": description,
        "flags": common_flags,
        "results": results,
        "passed": passed,
        "failed": failed,
        "total": len(tests),
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Host test report: {report_path}")
    print(f"Host tests: {passed} passed, {failed} failed, {len(tests)} total")
    return 0 if failed == 0 and len(results) == len(tests) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"HOST TEST CONFIGURATION ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
