"""Fast stack guard for the production topology worker dependency set.

The parser is intentionally small: it scans C function bodies for local
declarations, resolves literal and simple integer-macro array dimensions, and
uses target-ABI sizes for the large service/CS structs listed in MAX-107.
"""

import ast
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests" / "host" / "manifest.json"
TASK_CONFIG = ROOT / "components" / "app_config" / "include" / "app_task_config.h"
KNOWN_STRUCT_SIZES = {
    "nrf_topology_snapshot_t": 224,
    "nrf_remote_sensor_cache_t": 136,
    "nrf_cs_authority_t": 1576,
    "nrf_cs_cycle_snapshot_t": 380,
}
SCALAR_SIZES = {
    "char": 1,
    "int8_t": 1,
    "uint8_t": 1,
    "int16_t": 2,
    "uint16_t": 2,
    "int32_t": 4,
    "uint32_t": 4,
    "int64_t": 8,
    "uint64_t": 8,
    "size_t": 4,
    "float": 4,
    "double": 8,
}
TYPE_SIZES = {**SCALAR_SIZES, **KNOWN_STRUCT_SIZES}
CONTROL_WORDS = {"if", "for", "while", "switch", "return", "sizeof"}
DECLARATION = re.compile(
    r"(?:^|[;{}])\s*(?P<modifiers>(?:(?:static|const|volatile|register|auto)\s+)*)"
    r"(?P<type>" + "|".join(sorted(map(re.escape, TYPE_SIZES), key=len, reverse=True)) + r")\b"
    r"\s+(?P<pointer>\*+\s*)?(?P<name>[A-Za-z_]\w*)"
    r"(?P<dimensions>(?:\s*\[[^\]]*\]\s*)*)\s*(?:=[^;]*)?;",
    re.MULTILINE,
)
MACRO_DEFINITION = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*$", re.MULTILINE)
INTEGER_SUFFIX = re.compile(r"(?<=\d)[uUlL]+\b")


def _c_code_only(source: str) -> str:
    """Blank comments and literals so braces/declarations inside them do not count."""
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group().count("\n"), source, flags=re.DOTALL)
    source = re.sub(r"//[^\n]*", "", source)
    source = re.sub(r'"(?:\\.|[^"\\])*"', '""', source)
    return re.sub(r"'(?:\\.|[^'\\])*'", "''", source)


def _function_bodies(source: str):
    """Yield bodies by balanced braces; only top-level `name(args) {` opens functions."""
    code = _c_code_only(source)
    segment_start = 0
    depth = 0
    index = 0
    while index < len(code):
        char = code[index]
        if depth == 0 and char == ";":
            segment_start = index + 1
        elif char == "{" and depth == 0:
            prefix = code[segment_start:index]
            match = re.search(r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*$", prefix, re.DOTALL)
            depth = 1
            if match is not None and match.group(1) not in CONTROL_WORDS:
                body_start = index + 1
                body_depth = 1
                end = body_start
                while end < len(code) and body_depth:
                    if code[end] == "{":
                        body_depth += 1
                    elif code[end] == "}":
                        body_depth -= 1
                    end += 1
                yield match.group(1), code[body_start:end - 1]
                index = end - 1
                depth = 0
                segment_start = end
        elif char == "{" and depth > 0:
            depth += 1
        elif char == "}" and depth > 0:
            depth -= 1
            if depth == 0:
                segment_start = index + 1
        index += 1


def _integer(expression: str, macros: dict[str, int]) -> int | None:
    expression = INTEGER_SUFFIX.sub("", expression.strip())
    try:
        node = ast.parse(expression, mode="eval").body
    except SyntaxError:
        return None

    def visit(item):
        if isinstance(item, ast.Constant) and isinstance(item.value, int):
            return item.value
        if isinstance(item, ast.Name):
            return macros.get(item.id)
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, (ast.UAdd, ast.USub)):
            value = visit(item.operand)
            return None if value is None else (value if isinstance(item.op, ast.UAdd) else -value)
        if isinstance(item, ast.BinOp) and isinstance(item.op, (ast.Add, ast.Sub, ast.Mult, ast.FloorDiv)):
            left, right = visit(item.left), visit(item.right)
            if left is None or right is None:
                return None
            if isinstance(item.op, ast.Add):
                return left + right
            if isinstance(item.op, ast.Sub):
                return left - right
            if isinstance(item.op, ast.Mult):
                return left * right
            return left // right if right else None
        return None

    return visit(node)


def _integer_macros(paths) -> dict[str, int]:
    definitions = {}
    for path in paths:
        source = _c_code_only(path.read_text(encoding="utf-8"))
        for name, expression in MACRO_DEFINITION.findall(source):
            if "(" in name:
                continue
            definitions[name] = expression.split("//", 1)[0].strip()
    values = {}
    for _ in range(len(definitions) + 1):
        updated = False
        for name, expression in definitions.items():
            value = _integer(expression, values)
            if value is not None and values.get(name) != value:
                values[name] = value
                updated = True
        if not updated:
            break
    return values


def _large_automatic_locals(source: str, macros: dict[str, int]) -> list[str]:
    offenders = []
    for function, body in _function_bodies(source):
        for declaration in DECLARATION.finditer(body):
            if re.search(r"\bstatic\b", declaration.group("modifiers")):
                continue
            if declaration.group("pointer"):
                continue
            type_name = declaration.group("type")
            dimensions = re.findall(r"\[([^\]]*)\]", declaration.group("dimensions"))
            if type_name in KNOWN_STRUCT_SIZES:
                offenders.append(f"{function}: {type_name} {declaration.group('name')}")
                continue
            if not dimensions:
                continue
            count = 1
            for dimension in dimensions:
                length = _integer(dimension, macros)
                if length is None:
                    count = 0
                    break
                count *= length
            size = TYPE_SIZES[type_name] * count
            if size >= 256:
                offenders.append(f"{function}: {type_name} {declaration.group('name')}[{count}] ({size} bytes)")
    return offenders


def _worker_sources():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entry = next(test for test in manifest["tests"] if test["name"] == "nrf_topology_service_integration")
    sources = set()
    for item in entry["sources"]:
        if item.startswith("components/nrf_topology_service/") and item.endswith(".c") and "/tests/" not in item:
            sources.add(ROOT / item)
        elif item.startswith("components/nrf_cs_authority/") and item.endswith(".c"):
            sources.add(ROOT / item)
        elif item in (
            "nRF54L15/src/nrf_cs_scheduler.c",
            "nRF54L15/src/nrf_cs_control_protocol.c",
        ):
            sources.add(ROOT / item)
    assert any(path.name == "nrf_sensor_transport.c" for path in sources)
    assert any(path.name == "nrf_cs_authority.c" for path in sources)
    assert any(path.name == "nrf_cs_scheduler.c" for path in sources)
    return sorted(sources)


def _macro_paths(sources):
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    paths = set(sources)
    entry = next(test for test in manifest["tests"] if test["name"] == "nrf_topology_service_integration")
    for include_dir in entry["include_dirs"]:
        directory = ROOT / include_dir
        if directory.is_dir():
            paths.update(directory.rglob("*.h"))
    return sorted(paths)


def test_parser_self_check_finds_reintroduced_large_automatic_locals():
    sample = """
    void worker(void) {
        nrf_cs_cycle_snapshot_t old_snapshot;
        uint8_t too_large[2 * LIMIT];
        static nrf_cs_authority_t retained;
        uint8_t small[128];
    }
    """
    found = _large_automatic_locals(sample, {"LIMIT": 128})
    assert found == [
        "worker: nrf_cs_cycle_snapshot_t old_snapshot",
        "worker: uint8_t too_large[256] (256 bytes)",
    ]


def test_worker_sources_have_no_large_automatic_locals_or_overlay_include():
    sources = _worker_sources()
    macros = _integer_macros(_macro_paths(sources))
    offenders = []
    for path in sources:
        source = path.read_text(encoding="utf-8")
        assert re.search(r'^\s*#\s*include\s*"nrf_topology_service\.c"', source, re.MULTILINE) is None
        offenders.extend(f"{path.relative_to(ROOT)}: {item}" for item in _large_automatic_locals(source, macros))
    assert offenders == []


def test_topology_worker_stack_remains_at_least_4096_bytes():
    source = TASK_CONFIG.read_text(encoding="utf-8")
    match = re.search(r"^\s*#\s*define\s+APP_TASK_STACK_NRF_TOPOLOGY\s+(\d+)\b", source, re.MULTILINE)
    assert match is not None
    assert int(match.group(1)) >= 4096
