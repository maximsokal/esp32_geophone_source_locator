"""Static WebUI copy guard.

Scans JSX text nodes throughout webui/src, literal arguments to the setters in
ERROR_SETTERS, and Error constructors in the component boundaries where their
messages reach the UI. It excludes test/spec files, test directories, generated
`.gen.` files, and dev/debug paths or filenames. Lower-level API/domain error
messages are handled by formatErrorText at these boundaries. Tag-mode error
messages are exempt from literal scanning because tagModeErrorLabel maps known
messages and prefixes unknown details.

The one invariant allowlist covers product/device/locale labels, protocol and
sensor identifiers, standard measurement units, and non-linguistic symbols.
"""

from html import unescape
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui" / "src"
CATALOG_KEYS = frozenset(re.findall(
    r'^\s*"([^"]+)",?\s*$',
    (WEBUI / "i18n" / "catalog.keys.gen.ts").read_text(encoding="utf-8"),
    re.MULTILINE,
))
ERROR_BOUNDARY_FILES = {
    "App.tsx",
    "DeviceConsoleApp.tsx",
    "LocalizationCalculatePanel.tsx",
    "XrRuntimePanel.tsx",
    "ReportView.tsx",
    "reportExport.tsx",
}
ERROR_SETTERS = {"setError", "setScanStorageError", "setWorkspaceError", "setLinkError"}
INVARIANT_ALLOWLIST = {
    "exact": frozenset({
        "GEOLOCATOR", "ESP32-S3", "EN", "UK", "3D", "WEBXR", "WebXR", "WebGPU", "MAIN",
        "G", "S", "V", "X", "Y", "Z",
        "mdps", "mg",  # fixed sensor units; their abbreviations do not change by locale
        "m", "mm", "cm", "m/s", "m/s²", "m²", "m³", "ms", "rad/s", "Hz", "KiB", "MiB",
    }),
    "patterns": (
        re.compile(r"G[1-4]"),  # sensor identifiers
    ),
}
# Firmware and protocol error codes use underscores; a single lowercase word can be UI copy.
MACHINE_CODE = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)+$")
TAG_NAME = re.compile(r"<[A-Za-z][A-Za-z0-9_.:-]*")
ERROR_CALL = re.compile(r"\b(set[A-Za-z]*Error|new\s+Error)\s*\(")
JS_LITERAL = re.compile(r"'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|`(?:\\.|[^`\\])*`", re.DOTALL)


def _included_source(path: Path) -> bool:
    relative = path.relative_to(WEBUI)
    parts = {part.lower() for part in relative.parts}
    name = path.name.lower()
    return (
        path.suffix in {".tsx", ".ts"}
        and not name.endswith((".test.ts", ".test.tsx", ".spec.ts", ".spec.tsx"))
        and ".gen." not in name
        and not parts.intersection({"tests", "test", "__tests__", "dev", "debug", "tag-lab"})
        and not any(marker in name for marker in (".dev.", ".debug."))
    )


def _tag_end(source: str, start: int) -> int | None:
    match = TAG_NAME.match(source, start)
    if match is None:
        return None
    depth = 0
    quote = ""
    index = match.end()
    while index < len(source):
        character = source[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = ""
        elif character in "'\"`":
            quote = character
        elif character == "{":
            depth += 1
        elif character == "}" and depth:
            depth -= 1
        elif character == ">" and depth == 0:
            return index
        index += 1
    return None


def _is_jsx_candidate(source: str, start: int) -> bool:
    previous = source[start - 1] if start else ""
    if previous and (previous.isalnum() or previous in "_.$"):
        return False
    before = source[:start].rstrip()
    return (
        not before
        or previous in ">)}]=>,;:?!&|"
        or re.search(r"(?:\breturn|\belse|=>|&&|\|\||\?\?|[=(,:;?!&|])$", before) is not None
    )


def _expression_end(source: str, start: int) -> int | None:
    depth = 0
    quote = ""
    index = start
    while index < len(source):
        character = source[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = ""
        elif character in "'\"`":
            quote = character
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return None


def _jsx_text_nodes(source: str):
    index = 0
    while index < len(source):
        start = source.find("<", index)
        if start < 0:
            return
        if not _is_jsx_candidate(source, start):
            index = start + 1
            continue
        end = _tag_end(source, start)
        if end is None:
            index = start + 1
            continue
        if source[start + 1:start + 2] == "/" or source[start:end].rstrip().endswith("/"):
            index = end + 1
            continue
        content_start = end + 1
        content_index = content_start
        has_expression = False
        while content_index < len(source):
            if source[content_index] == "<":
                break
            if source[content_index] == "{":
                has_expression = True
                expression_end = _expression_end(source, content_index)
                if expression_end is None:
                    break
                content_index = expression_end
                continue
            boundaries = [position for position in (source.find("<", content_index), source.find("{", content_index)) if position >= 0]
            content_end = min(boundaries, default=len(source))
            value = unescape(" ".join(source[content_index:content_end].split()))
            if value:
                yield source.count("\n", 0, content_index) + 1, value
            content_index = content_end
        index = content_start if has_expression else content_index


def _first_argument(source: str, opening_paren: int) -> str:
    depth = {"(": 1, "[": 0, "{": 0}
    quote = ""
    index = opening_paren + 1
    while index < len(source):
        character = source[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = ""
        elif character in "'\"`":
            quote = character
        elif character == "(":
            depth["("] += 1
        elif character == ")":
            depth["("] -= 1
            if depth["("] == 0:
                return source[opening_paren + 1:index]
        elif character == "[":
            depth["["] += 1
        elif character == "]":
            depth["["] -= 1
        elif character == "{":
            depth["{"] += 1
        elif character == "}":
            depth["{"] -= 1
        elif character == "," and depth == {"(": 1, "[": 0, "{": 0}:
            return source[opening_paren + 1:index]
        index += 1
    return source[opening_paren + 1:]


def _literal_texts(expression: str):
    for match in JS_LITERAL.finditer(expression):
        literal = match.group()
        value = literal[1:-1]
        if literal.startswith("`"):
            value = re.sub(r"\$\{[^{}]*\}", "", value)
        yield value


def _is_exempt(value: str) -> bool:
    value = " ".join(value.split())
    token = value.strip(" :;,.·/()[]")
    return (
        not any(character.isalpha() for character in value)
        or token in INVARIANT_ALLOWLIST["exact"]
        or any(pattern.fullmatch(token) for pattern in INVARIANT_ALLOWLIST["patterns"])
        or value in CATALOG_KEYS
        or MACHINE_CODE.fullmatch(value) is not None
    )


def _find_untranslated_literals_in_source(path: Path, source: str):
    findings = []
    relative = path.relative_to(ROOT).as_posix()
    if path.suffix == ".tsx":
        for line, value in _jsx_text_nodes(source):
            if not _is_exempt(value):
                findings.append(f"{relative}:{line}: JSX text {value!r}")
    for call in ERROR_CALL.finditer(source):
        name = call.group(1).replace(" ", "")
        if name.startswith("set") and name not in ERROR_SETTERS:
            continue
        if name == "newError" and path.name not in ERROR_BOUNDARY_FILES:
            continue
        argument = _first_argument(source, source.find("(", call.start()))
        for value in _literal_texts(argument):
            if not _is_exempt(value):
                line = source.count("\n", 0, call.start()) + 1
                findings.append(f"{relative}:{line}: {name} text {value!r}")
    return findings


def find_untranslated_literals():
    findings = []
    for path in sorted(WEBUI.rglob("*")):
        if path.is_file() and _included_source(path):
            findings.extend(_find_untranslated_literals_in_source(path, path.read_text(encoding="utf-8")))
    return findings


def test_webui_src_has_no_untranslated_user_visible_literals():
    findings = find_untranslated_literals()
    assert not findings, "Untranslated WebUI literals:\n" + "\n".join(findings)


def test_single_word_copy_is_flagged_and_catalog_key_is_exempt():
    findings = _find_untranslated_literals_in_source(
        WEBUI / "Synthetic.tsx",
        "const Example = () => <span>failed</span>;\nsetError('web.error_prefix');\n",
    )
    assert any("JSX text 'failed'" in finding for finding in findings)
    assert not any("web.error_prefix" in finding for finding in findings)
