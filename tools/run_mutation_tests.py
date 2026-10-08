#!/usr/bin/env python3
"""Run exact source mutations against named host-test fragments."""

from __future__ import annotations

import argparse
import atexit
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from typing import Callable, Sequence
import uuid


BACKUP_DIR = Path(".test-build/mutation-backup")
TEST_BUILD_DIR = Path(".test-build/host-tests-mutation")
TEST_TIMEOUT_SECONDS = 180.0
ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
MUTATION_KEYS = {"id", "description", "file", "find", "replace", "tests", "expected_killer"}


class ConfigurationError(Exception):
    pass


class InfrastructureError(Exception):
    pass


class TestTimeout(Exception):
    pass


@dataclass(frozen=True)
class TestSpec:
    kind: str
    value: str
    only: str | None = None


@dataclass(frozen=True)
class Mutation:
    id: str
    description: str
    file: str
    target: Path
    find: str
    replace: str
    tests: tuple[TestSpec, ...]
    expected_killer: str


@dataclass
class MutationResult:
    id: str
    status: str
    duration: float
    expected_killer: str
    detail: str = ""

    def exit_code(self) -> int:
        if self.status == "ERROR":
            return 3
        if self.status == "SURVIVED":
            return 1
        return 0


@dataclass
class Recovery:
    root: Path
    target: Path
    backup: Path
    marker: Path
    sha256: str
    clean_at_start: bool


_ACTIVE_RECOVERY: Recovery | None = None
_ACTIVE_PROCESS: subprocess.Popen[str] | None = None
_INTERRUPTED = False


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def contained_path(root: Path, relative: str, description: str) -> Path:
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise ConfigurationError(f"{description} escapes repository root: {relative}") from exc
    return path


def source_encoding(data: bytes, path: Path) -> str:
    if data.startswith(b"\xff\xfe"):
        return "utf-16-le"
    if data.startswith(b"\xfe\xff"):
        return "utf-16-be"
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ConfigurationError(f"source is not UTF-8 or BOM-marked UTF-16: {path}") from exc
    return "utf-8"


def source_eol(data: bytes, encoding: str, path: Path) -> str:
    crlf = "\r\n".encode(encoding)
    lf = "\n".encode(encoding)
    cr = "\r".encode(encoding)
    has_crlf = crlf in data
    without_crlf = data.replace(crlf, b"")
    if cr in without_crlf or (has_crlf and lf in without_crlf):
        raise ConfigurationError(f"mixed line endings in target file: {path}")
    return "\r\n" if has_crlf else "\n"


def mutation_bytes(value: str, encoding: str, eol: str) -> bytes:
    # Mutation JSON patterns are LF-authored; normalize legacy CR variants before matching the target EOL.
    normalized = value.replace("\r\n", "\n").replace("\r", "\n")
    return normalized.replace("\n", eol).encode(encoding)


def load_mutations(files: Sequence[Path], root: Path) -> tuple[list[Mutation], list[str]]:
    mutations: list[Mutation] = []
    errors: list[str] = []
    ids: dict[str, int] = {}

    for input_path in files:
        path = input_path if input_path.is_absolute() else root / input_path
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{path}: cannot read mutation JSON: {exc}")
            continue
        if not isinstance(raw, list):
            errors.append(f"{path}: mutation file must contain a JSON array")
            continue

        for index, item in enumerate(raw):
            label = f"{path}[{index}]"
            if not isinstance(item, dict):
                errors.append(f"{label}: mutation must be an object")
                continue
            unknown = set(item) - MUTATION_KEYS
            missing = MUTATION_KEYS - set(item)
            for key in sorted(unknown):
                errors.append(f"{label}: unknown mutation key {key!r}")
            if missing:
                errors.append(f"{label}: missing mutation keys: {', '.join(sorted(missing))}")
                continue

            mutation_id = item["id"]
            if not isinstance(mutation_id, str) or not mutation_id or any(
                char not in ID_CHARS for char in mutation_id
            ):
                errors.append(f"{label}: id must contain only letters, digits, _, ., or -")
                continue
            if mutation_id in ids:
                errors.append(f"{label}: duplicate mutation id {mutation_id!r}")
                continue
            ids[mutation_id] = 1

            string_fields = ("description", "file", "find", "expected_killer")
            bad_fields = [
                name for name in string_fields
                if not isinstance(item[name], str) or not item[name]
            ]
            if not isinstance(item["replace"], str):
                bad_fields.append("replace")
            if bad_fields:
                errors.append(f"{label}: fields must be non-empty strings: {', '.join(bad_fields)}")
                continue
            if not isinstance(item["tests"], list) or not item["tests"]:
                errors.append(f"{label}: tests must be a non-empty array")
                continue

            target_rel = item["file"].replace("\\", "/")
            try:
                target = contained_path(root, target_rel, "target file")
            except ConfigurationError as exc:
                errors.append(f"{label}: {exc}")
                continue
            if not target.is_file():
                errors.append(f"{label}: target file does not exist: {target_rel}")
                continue
            try:
                original = target.read_bytes()
                encoding = source_encoding(original, target)
                eol = source_eol(original, encoding, target)
                find = mutation_bytes(item["find"], encoding, eol)
                replacement = mutation_bytes(item["replace"], encoding, eol)
            except (OSError, UnicodeError, ConfigurationError) as exc:
                errors.append(f"{label}: cannot encode/read target: {exc}")
                continue
            count = original.count(find)
            if count != 1:
                errors.append(f"{label}: find must occur exactly once in {target_rel}; found {count}")
                continue
            if find == replacement:
                errors.append(f"{label}: find and replace must differ")
                continue

            test_specs: list[TestSpec] = []
            valid = True
            for test_index, spec in enumerate(item["tests"]):
                test_label = f"{label}.tests[{test_index}]"
                if not isinstance(spec, dict):
                    errors.append(f"{test_label}: test spec must be an object")
                    valid = False
                    continue
                if set(spec) == {"manifest"} or set(spec) == {"manifest", "only"}:
                    manifest_rel = spec["manifest"]
                    only = spec.get("only")
                    if not isinstance(manifest_rel, str) or not manifest_rel:
                        errors.append(f"{test_label}: manifest must be a non-empty string")
                        valid = False
                        continue
                    if only is not None and (not isinstance(only, str) or not only):
                        errors.append(f"{test_label}: only must be a non-empty string")
                        valid = False
                        continue
                    manifest_rel = manifest_rel.replace("\\", "/")
                    try:
                        manifest_path = contained_path(root, manifest_rel, "manifest")
                    except ConfigurationError as exc:
                        errors.append(f"{test_label}: {exc}")
                        valid = False
                        continue
                    if not manifest_path.is_file():
                        errors.append(f"{test_label}: manifest does not exist: {manifest_rel}")
                        valid = False
                        continue
                    try:
                        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                    except (OSError, json.JSONDecodeError) as exc:
                        errors.append(f"{test_label}: cannot read manifest: {exc}")
                        valid = False
                        continue
                    names = {
                        entry.get("name")
                        for entry in manifest.get("tests", [])
                        if isinstance(entry, dict)
                    } if isinstance(manifest, dict) else set()
                    if only is not None and only not in names:
                        errors.append(f"{test_label}: manifest has no test named {only!r}")
                        valid = False
                        continue
                    test_specs.append(TestSpec("manifest", manifest_rel, only))
                elif set(spec) == {"pytest"}:
                    selector = spec["pytest"]
                    if not isinstance(selector, str) or not selector.strip():
                        errors.append(f"{test_label}: pytest selector must be a non-empty string")
                        valid = False
                        continue
                    test_specs.append(TestSpec("pytest", selector))
                else:
                    errors.append(
                        f"{test_label}: expected {{manifest, only?}} or {{pytest}} test spec"
                    )
                    valid = False

            if valid:
                mutations.append(Mutation(
                    id=mutation_id,
                    description=item["description"],
                    file=target_rel,
                    target=target,
                    find=item["find"],
                    replace=item["replace"],
                    tests=tuple(test_specs),
                    expected_killer=item["expected_killer"],
                ))

    if not mutations and not errors:
        errors.append("no mutations were configured")
    return mutations, errors


def git_output(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            ["git", *args],
            cwd=root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise InfrastructureError(f"cannot run git: {exc}") from exc


def dirty_at_start(root: Path, file: str) -> bool:
    result = git_output(root, "status", "--porcelain", "--", file)
    if result.returncode:
        raise InfrastructureError(f"git status failed for {file}: {result.stderr.strip()}")
    return bool(result.stdout.strip())


def terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                capture_output=True,
                check=False,
            )
        except OSError:
            pass
        if process.poll() is None:
            process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except OSError:
            process.terminate()
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except OSError:
                process.kill()


def run_subprocess(
    command: Sequence[str],
    *,
    cwd: Path,
    timeout: float,
) -> subprocess.CompletedProcess[str]:
    global _ACTIVE_PROCESS
    kwargs: dict[str, object] = {
        "cwd": cwd,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
    }
    if os.name == "nt":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        kwargs["start_new_session"] = True
    process = subprocess.Popen(list(command), **kwargs)  # type: ignore[arg-type]
    _ACTIVE_PROCESS = process
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        terminate_process_tree(process)
        stdout, stderr = process.communicate()
        raise TestTimeout(
            f"timed out after {timeout:g}s\n{stdout or ''}{stderr or ''}"
        ) from exc
    except KeyboardInterrupt:
        terminate_process_tree(process)
        process.communicate()
        raise
    finally:
        if _ACTIVE_PROCESS is process:
            _ACTIVE_PROCESS = None
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def recovery_directory(root: Path) -> Path:
    return root / BACKUP_DIR


def write_recovery(root: Path, mutation: Mutation, original: bytes, clean: bool) -> Recovery:
    global _ACTIVE_RECOVERY
    directory = recovery_directory(root)
    directory.mkdir(parents=True, exist_ok=True)
    backup = directory / f"{mutation.id}.{uuid.uuid4().hex}.backup"
    marker = directory / f"{mutation.id}.json"
    marker_temp = directory / f".{mutation.id}.{uuid.uuid4().hex}.tmp"
    if marker.exists():
        raise InfrastructureError(f"backup path already exists for {mutation.id}")
    digest = sha256(original)
    marker_published = False
    record = {
        "target": mutation.file,
        "backup": backup.relative_to(root).as_posix(),
        "sha256": digest,
        "clean_at_start": clean,
    }
    try:
        with backup.open("xb") as stream:
            stream.write(original)
            stream.flush()
            os.fsync(stream.fileno())
        with marker_temp.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(record, stream, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        marker_published = True
        os.replace(marker_temp, marker)
    except BaseException:
        backup.unlink(missing_ok=True)
        marker_temp.unlink(missing_ok=True)
        if marker_published:
            marker.unlink(missing_ok=True)
        raise
    recovery = Recovery(root, mutation.target, backup, marker, digest, clean)
    _ACTIVE_RECOVERY = recovery
    return recovery


def restore_recovery(recovery: Recovery) -> None:
    global _ACTIVE_RECOVERY
    original = recovery.backup.read_bytes()
    if sha256(original) != recovery.sha256:
        raise InfrastructureError(f"backup checksum mismatch: {recovery.backup}")
    recovery.target.write_bytes(original)
    if sha256(recovery.target.read_bytes()) != recovery.sha256:
        raise InfrastructureError(f"restored checksum mismatch: {recovery.target}")
    recovery.marker.unlink(missing_ok=True)
    if _ACTIVE_RECOVERY == recovery:
        _ACTIVE_RECOVERY = None
    recovery.backup.unlink(missing_ok=True)


def restore_leftovers(root: Path) -> list[str]:
    directory = recovery_directory(root)
    if not directory.exists():
        return []
    restored: list[str] = []
    for marker in sorted(directory.glob("*.json")):
        try:
            record = json.loads(marker.read_text(encoding="utf-8"))
            target_rel = record["target"]
            backup_rel = record["backup"]
            digest = record["sha256"]
            clean = record["clean_at_start"]
            if not isinstance(target_rel, str) or not isinstance(backup_rel, str):
                raise ValueError("target and backup must be paths")
            if not isinstance(digest, str) or not isinstance(clean, bool):
                raise ValueError("invalid checksum or clean state")
            target = contained_path(root, target_rel, "recovery target")
            backup = contained_path(root, backup_rel, "recovery backup")
            restore_recovery(Recovery(root, target, backup, marker, digest, clean))
            if clean:
                diff_error = check_final_diffs(root, [target_rel])
                if diff_error:
                    raise InfrastructureError(diff_error)
            restored.append(target_rel)
        except (OSError, KeyError, ValueError, json.JSONDecodeError, InfrastructureError, ConfigurationError) as exc:
            raise InfrastructureError(f"cannot restore {marker}: {exc}") from exc
    return restored


def restore_active_quietly() -> None:
    recovery = _ACTIVE_RECOVERY
    if recovery is None:
        return
    try:
        restore_recovery(recovery)
    except Exception as exc:
        print(f"MUTATION RESTORE FAILED: {exc}", file=sys.stderr)


def interrupt_handler(signum: int, frame: object) -> None:
    global _INTERRUPTED
    _INTERRUPTED = True
    if _ACTIVE_PROCESS is not None:
        terminate_process_tree(_ACTIVE_PROCESS)
    restore_active_quietly()
    raise KeyboardInterrupt(f"signal {signum}")


def interrupt_recovery():
    class Guard:
        def __enter__(self):
            self.previous = {}
            for name in ("SIGINT", "SIGBREAK"):
                sig = getattr(signal, name, None)
                if sig is not None:
                    self.previous[sig] = signal.getsignal(sig)
                    signal.signal(sig, interrupt_handler)
            atexit.register(restore_active_quietly)
            return self

        def __exit__(self, exc_type, exc, traceback):
            atexit.unregister(restore_active_quietly)
            for sig, previous in self.previous.items():
                signal.signal(sig, previous)
            restore_active_quietly()
            return False

    return Guard()


def command_for(
    spec: TestSpec,
    root: Path,
    build_dir: Path,
    host_runner: Path | None,
) -> list[str]:
    if spec.kind == "manifest":
        runner = host_runner or (root / "tools/run_host_tests.py")
        command = [
            sys.executable,
            str(runner),
            "--manifest",
            spec.value,
            "--build-dir",
            build_dir.relative_to(root).as_posix(),
        ]
        if spec.only is not None:
            command.extend(["--only", spec.only])
        return command
    return [sys.executable, "-m", "pytest", "-q", spec.value]


def execute_one(
    mutation: Mutation,
    root: Path,
    clean_at_start: bool,
    *,
    host_runner: Path | None,
    command_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> MutationResult:
    global _INTERRUPTED
    started = time.perf_counter()
    status = "RUNNING"
    detail = ""
    recovery: Recovery | None = None
    try:
        original = mutation.target.read_bytes()
        encoding = source_encoding(original, mutation.target)
        eol = source_eol(original, encoding, mutation.target)
        find = mutation_bytes(mutation.find, encoding, eol)
        replacement = mutation_bytes(mutation.replace, encoding, eol)
        if original.count(find) != 1:
            raise InfrastructureError(
                f"{mutation.file} changed since validation; find no longer occurs exactly once"
            )
        mutated = original.replace(find, replacement, 1)
        recovery = write_recovery(root, mutation, original, clean_at_start)
        mutation.target.write_bytes(mutated)

        failed = False
        timed_out = False
        build_dir = root / TEST_BUILD_DIR / mutation.id
        for spec in mutation.tests:
            command = command_for(spec, root, build_dir, host_runner)
            try:
                result = command_runner(
                    command,
                    cwd=root,
                    timeout=TEST_TIMEOUT_SECONDS,
                )
            except TestTimeout as exc:
                timed_out = True
                detail = str(exc)
            except KeyboardInterrupt:
                _INTERRUPTED = True
                status = "ERROR"
                detail = "interrupted"
                break
            except Exception as exc:
                status = "ERROR"
                detail = f"test runner failed: {exc}"
                break
            if result.returncode != 0:
                failed = True
                detail = f"test command exited {result.returncode}"
                if result.stdout:
                    detail += f"\n{result.stdout.rstrip()}"
                if result.stderr:
                    detail += f"\n{result.stderr.rstrip()}"

        if status == "RUNNING":
            status = "TIMEOUT" if timed_out else "KILLED" if failed else "SURVIVED"
    except KeyboardInterrupt:
        _INTERRUPTED = True
        status = "ERROR"
        detail = "interrupted"
    except Exception as exc:
        status = "ERROR"
        detail = str(exc)
    finally:
        if recovery is not None:
            try:
                if recovery.marker.exists():
                    restore_recovery(recovery)
                elif sha256(recovery.target.read_bytes()) != recovery.sha256:
                    raise InfrastructureError(f"restored checksum mismatch: {recovery.target}")
                else:
                    recovery.backup.unlink(missing_ok=True)
            except Exception as exc:
                status = "ERROR"
                detail = f"restore failed: {exc}"
    return MutationResult(
        mutation.id,
        status,
        time.perf_counter() - started,
        mutation.expected_killer,
        detail,
    )


def check_final_diffs(root: Path, clean_files: Sequence[str]) -> str | None:
    if not clean_files:
        return None
    result = git_output(root, "diff", "--quiet", "--", *clean_files)
    if result.returncode == 1:
        return "clean-at-start target files differ after mutation run: " + ", ".join(clean_files)
    if result.returncode:
        return f"git diff failed: {result.stderr.strip()}"
    return None


def run_mutations(
    mutation_files: Sequence[Path],
    *,
    repo_root: Path,
    only: Sequence[str] = (),
    allow_dirty: bool = False,
    host_runner: Path | None = None,
    command_runner: Callable[..., subprocess.CompletedProcess[str]] = run_subprocess,
) -> tuple[list[MutationResult], int, list[str]]:
    root = repo_root.resolve()
    backup_dir = recovery_directory(root)
    if backup_dir.exists() and any(backup_dir.glob("*.json")):
        return [], 3, [f"leftover backup marker exists in {backup_dir}; run --restore"]
    mutations, errors = load_mutations(mutation_files, root)
    if errors:
        return [], 2, errors
    available = {mutation.id for mutation in mutations}
    unknown = sorted(set(only) - available)
    if unknown:
        return [], 2, ["--only names not found: " + ", ".join(unknown)]
    selected_ids = set(only)
    selected = [item for item in mutations if not selected_ids or item.id in selected_ids]
    target_clean: dict[str, bool] = {}
    try:
        for mutation in selected:
            target_clean.setdefault(mutation.file, not dirty_at_start(root, mutation.file))
    except InfrastructureError as exc:
        return [], 3, [str(exc)]

    results: list[MutationResult] = []
    global _INTERRUPTED
    _INTERRUPTED = False
    with interrupt_recovery():
        for mutation in selected:
            clean = target_clean[mutation.file]
            if not clean and not allow_dirty:
                results.append(MutationResult(
                    mutation.id,
                    "ERROR",
                    0.0,
                    mutation.expected_killer,
                    "target is dirty at startup; pass --allow-dirty to opt in",
                ))
                continue
            results.append(execute_one(
                mutation,
                root,
                clean,
                host_runner=host_runner,
                command_runner=command_runner,
            ))
            if _INTERRUPTED:
                break

    diff_error = check_final_diffs(
        root,
        [file for file, clean in target_clean.items() if clean],
    )
    if diff_error:
        return results, 3, [diff_error]
    exit_code = max((result.exit_code() for result in results), default=0)
    return results, exit_code, []


def dry_run_rows(
    mutation_files: Sequence[Path],
    *,
    repo_root: Path,
) -> tuple[list[tuple[str, str, str, str]], list[str]]:
    root = repo_root.resolve()
    _, errors = load_mutations(mutation_files, root)
    rows: list[tuple[str, str, str, str]] = []
    for input_path in mutation_files:
        path = input_path if input_path.is_absolute() else root / input_path
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            rows.append(("<config>", str(path), "-", "CONFIG ERROR"))
            continue
        if not isinstance(raw, list):
            rows.append(("<config>", str(path), "-", "CONFIG ERROR"))
            continue
        for index, item in enumerate(raw):
            if not isinstance(item, dict):
                rows.append(("<config>", str(path), "-", "CONFIG ERROR"))
                continue
            mutation_id = item.get("id")
            file = item.get("file")
            killer = item.get("expected_killer")
            mutation_id = mutation_id if isinstance(mutation_id, str) else "<missing>"
            file = file if isinstance(file, str) else "-"
            killer = killer if isinstance(killer, str) else "-"
            label = f"{path}[{index}]"
            invalid = any(
                error.startswith(label + ":") or error.startswith(label + ".")
                for error in errors
            )
            invalid = invalid or any(
                f"duplicate mutation id {mutation_id!r}" in error
                for error in errors
            )
            rows.append((mutation_id, file, killer, "CONFIG ERROR" if invalid else "OK"))
    if errors and not rows:
        rows.append(("<config>", "-", "-", "CONFIG ERROR"))
    return rows, errors


def print_dry_run(rows: Sequence[tuple[str, str, str, str]], errors: Sequence[str]) -> None:
    print(f"{'ID':<24} {'FILE':<54} {'EXPECTED KILLER':<72} STATUS")
    for mutation_id, file, killer, status in rows:
        print(f"{mutation_id:<24} {file:<54} {killer:<72} {status}")
    for error in errors:
        print(f"CONFIG ERROR: {error}", file=sys.stderr)


def print_results(results: Sequence[MutationResult]) -> None:
    print(f"{'ID':<24} {'STATUS':<10} {'SECONDS':>9}  EXPECTED KILLER")
    for result in results:
        print(
            f"{result.id:<24} {result.status:<10} {result.duration:>9.2f}  "
            f"{result.expected_killer}"
        )
        if result.detail and result.status in {"ERROR", "TIMEOUT"}:
            print(f"  {result.detail}", file=sys.stderr)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=(
            "Exit codes: 0 all mutations killed (TIMEOUT counts as killed); "
            "1 at least one survived; 2 configuration error; "
            "3 restore or infrastructure error."
        ),
    )
    parser.add_argument("mutation_files", nargs="*", type=Path, help="JSON mutation lists")
    parser.add_argument("--dry-run", "--list", action="store_true", help="validate and list without writing")
    parser.add_argument("--only", action="append", default=[], metavar="ID", help="select an id; repeatable")
    parser.add_argument("--allow-dirty", action="store_true", help="allow mutation of dirty target files")
    parser.add_argument("--restore", action="store_true", help="restore any leftover mutation backup")
    parser.add_argument("--json-report", type=Path, help="write the execute-mode result report")
    return parser.parse_args(argv)


def main(
    argv: Sequence[str] | None = None,
    *,
    repo_root: Path | None = None,
    host_runner: Path | None = None,
    command_runner: Callable[..., subprocess.CompletedProcess[str]] = run_subprocess,
) -> int:
    args = parse_args(argv)
    root = (repo_root or Path(__file__).resolve().parents[1]).resolve()
    if args.restore:
        if args.dry_run or args.mutation_files or args.only or args.json_report:
            print("--restore cannot be combined with mutation files, --only, --dry-run, or --json-report", file=sys.stderr)
            return 2
        try:
            restored = restore_leftovers(root)
        except InfrastructureError as exc:
            print(f"RESTORE ERROR: {exc}", file=sys.stderr)
            return 3
        if restored:
            for file in restored:
                print(f"RESTORED {file}")
        else:
            print("No leftover mutation backups.")
        return 0
    if not args.mutation_files:
        print("at least one mutation JSON file is required", file=sys.stderr)
        return 2
    if args.dry_run:
        if args.json_report:
            print("--json-report is execute-mode only", file=sys.stderr)
            return 2
        rows, errors = dry_run_rows(args.mutation_files, repo_root=root)
        known_ids = {row[0] for row in rows if row[0] != "<config>"}
        unknown = sorted(set(args.only) - known_ids)
        if unknown:
            errors.append("--only names not found: " + ", ".join(unknown))
            rows.append(("<only>", "-", "-", "CONFIG ERROR"))
        print_dry_run(rows, errors)
        return 2 if errors else 0

    results, exit_code, errors = run_mutations(
        args.mutation_files,
        repo_root=root,
        only=args.only,
        allow_dirty=args.allow_dirty,
        host_runner=host_runner,
        command_runner=command_runner,
    )
    print_results(results)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    if args.json_report:
        report_path = args.json_report if args.json_report.is_absolute() else root / args.json_report
        try:
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_text(json.dumps({
                "exit_code": exit_code,
                "results": [result.__dict__ for result in results],
                "errors": errors,
            }, indent=2) + "\n", encoding="utf-8")
        except OSError as exc:
            print(f"cannot write JSON report: {exc}", file=sys.stderr)
            return 3
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
