from __future__ import annotations

import json
from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_DIR = REPO_ROOT / "tests" / "host" / "manifest.d"
RECORDER_FS_TEST = "components/seismic_recorder/tests/seismic_recorder_fs_contract_test.c"
POSIX_DECLARATIONS = re.compile(
    r"^\s*(?:extern\s+)?(?:int\s+fileno\s*\([^;]*\)|"
    r"size_t\s+strnlen\s*\([^;]*\))\s*;\s*$"
)


def _excludes_msvc(kind: str, expression: str) -> bool:
    expression = " ".join(expression.split())
    return (kind == "ifndef" and expression == "_MSC_VER") or (
        kind in {"if", "elif"}
        and expression in {"!defined(_MSC_VER)", "!defined _MSC_VER"}
    )


def _unguarded_posix_declarations(source: str) -> list[str]:
    msvc_exclusion_stack: list[bool] = []
    unguarded: list[str] = []

    for line_number, line in enumerate(source.splitlines(), start=1):
        directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", line)
        if directive is not None:
            kind, expression = directive.groups()
            expression = expression.strip()
            if kind in {"if", "ifdef", "ifndef"}:
                msvc_exclusion_stack.append(_excludes_msvc(kind, expression))
            elif kind == "elif":
                msvc_exclusion_stack[-1] = _excludes_msvc(kind, expression)
            elif kind == "else":
                msvc_exclusion_stack[-1] = False
            else:
                msvc_exclusion_stack.pop()
            continue

        if POSIX_DECLARATIONS.match(line) and not any(msvc_exclusion_stack):
            unguarded.append(f"{line_number}: {line.strip()}")

    return unguarded


def _recorder_fragment_targets() -> list[str]:
    targets: list[str] = []
    for manifest_path in sorted(MANIFEST_DIR.glob("*.json")):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for entry in manifest.get("tests", []):
            if RECORDER_FS_TEST in entry.get("sources", []):
                targets.append(f"{manifest_path.name}:{entry['name']}")
    return targets


def test_recorder_fragments_guard_posix_prototypes_from_msvc() -> None:
    targets = _recorder_fragment_targets()
    assert targets, "no host manifest fragments include the recorder filesystem contract test"

    source = (REPO_ROOT / RECORDER_FS_TEST).read_text(encoding="utf-8")
    unguarded = _unguarded_posix_declarations(source)
    assert not unguarded, (
        "MSVC recorder fragments must not compile POSIX compatibility prototypes: "
        f"targets={targets}, declarations={unguarded}"
    )
