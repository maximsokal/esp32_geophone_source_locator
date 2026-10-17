"""Validation and deterministic generators shared by the AprilTag tool and tests."""

from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import tempfile
from typing import Any


FAMILY = "tag36h11"
TAG_IDS = (0, 1, 2, 3, 4)
NODES = ("MAIN", "G1", "G2", "G3", "G4")
APRILTAG_DIR = Path("tools") / "apriltag_sheet"
BINDINGS_PATH = APRILTAG_DIR / "tag_bindings.json"
PATTERNS_PATH = APRILTAG_DIR / "tag_patterns.gen.json"
GENERATED_TS_PATH = Path("webui/src/xr/fiducial/tagBindings.gen.ts")
DICTIONARY_PATH = Path("webui/node_modules/js-aruco2/src/dictionaries/apriltag_36h11.js")


class BindingError(ValueError):
    """A binding document is invalid or a generated output is stale."""


def _object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BindingError(f"duplicate JSON field {key!r}")
        result[key] = value
    return result


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_object_pairs)
    except (OSError, json.JSONDecodeError) as exc:
        raise BindingError(f"cannot read {path}: {exc}") from exc


def validate_bindings(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BindingError("binding document must be an object")
    if set(value) != {"family", "tags", "sheet"}:
        raise BindingError("binding document fields must be exactly family, tags, and sheet")
    if value["family"] != FAMILY:
        raise BindingError(f"family must be {FAMILY!r}")
    if not isinstance(value["tags"], list):
        raise BindingError("tags must be an array")
    tags: list[dict[str, Any]] = []
    ids: set[int] = set()
    nodes: set[str] = set()
    for item in value["tags"]:
        if not isinstance(item, dict) or set(item) != {"tag_id", "node", "size_mm"}:
            raise BindingError("each tag must contain exactly tag_id, node, and size_mm")
        tag_id = item["tag_id"]
        if type(tag_id) is not int or tag_id not in TAG_IDS:
            raise BindingError("tag_id must be an integer from 0 through 4")
        if tag_id in ids:
            raise BindingError(f"tag_id {tag_id} is duplicated")
        ids.add(tag_id)
        node = item["node"]
        if not isinstance(node, str) or node not in NODES:
            raise BindingError("node must be one of MAIN, G1, G2, G3, or G4")
        if node in nodes:
            raise BindingError(f"node {node} is duplicated")
        nodes.add(node)
        size = item["size_mm"]
        if isinstance(size, bool) or not isinstance(size, (int, float)) or not math.isfinite(size):
            raise BindingError("size_mm must be a finite number")
        if not 15 <= size <= 150:
            raise BindingError("size_mm must be in the range 15..150")
        tags.append({"tag_id": tag_id, "node": node, "size_mm": size})
    if ids != set(TAG_IDS):
        raise BindingError("tag_ids must be exactly 0, 1, 2, 3, and 4")
    if nodes != set(NODES):
        raise BindingError("nodes must contain MAIN, G1, G2, G3, and G4 exactly once")
    sheet = value["sheet"]
    if not isinstance(sheet, dict) or set(sheet) != {"paper", "copies_per_tag"}:
        raise BindingError("sheet must contain exactly paper and copies_per_tag")
    if sheet["paper"] != "A4":
        raise BindingError("paper must be A4")
    copies = sheet["copies_per_tag"]
    if type(copies) is not int or not 1 <= copies <= 4:
        raise BindingError("copies_per_tag must be an integer in the range 1..4")
    tags.sort(key=lambda item: item["tag_id"])
    return {"family": FAMILY, "tags": tags, "sheet": {"paper": "A4", "copies_per_tag": copies}}


def load_bindings(root: Path) -> dict[str, Any]:
    return validate_bindings(read_json(root / BINDINGS_PATH))


def load_dictionary_codes(root: Path) -> list[int]:
    path = root / DICTIONARY_PATH
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise BindingError(f"cannot read AprilTag dictionary {path}: {exc}") from exc
    n_bits = re.search(r"\bnBits\s*:\s*(\d+)", source)
    code_list = re.search(r"\bcodeList\s*:\s*\[([^\]]*)\]", source)
    if n_bits is None or int(n_bits.group(1)) != 36 or code_list is None:
        raise BindingError("js-aruco2 dictionary must define a 36-bit codeList")
    codes = [int(token, 16) for token in re.findall(r"0[xX]([0-9a-fA-F]+)", code_list.group(1))]
    if len(codes) < 5 or any(code >= (1 << 36) for code in codes):
        raise BindingError("js-aruco2 dictionary has fewer than five valid 36-bit codes")
    return codes


def pattern_document(root: Path) -> dict[str, Any]:
    codes = load_dictionary_codes(root)
    tags = []
    for tag_id, code in enumerate(codes[:5]):
        data_bits = format(code, "036b")
        data_matrix = [[int(bit) for bit in data_bits[row * 6 : (row + 1) * 6]] for row in range(6)]
        modules = [[int(row in (0, 7) or column in (0, 7)) for column in range(8)] for row in range(8)]
        # js-aruco2 reads a 1-bit as white; module entries use 1 for printed black.
        for row in range(6):
            modules[row + 1][1:7] = [1 - bit for bit in data_matrix[row]]
        tags.append({
            "tag_id": tag_id,
            "code": f"0x{code:09x}",
            "data_bits": data_bits,
            "data_matrix": data_matrix,
            "modules": modules,
        })
    return {"family": FAMILY, "tags": tags}


def generated_typescript(bindings: dict[str, Any]) -> str:
    tags = bindings["tags"]
    ids = " | ".join(str(tag_id) for tag_id in TAG_IDS)
    id_to_node = ",\n".join(f"  {item['tag_id']}: '{item['node']}'" for item in tags)
    node_to_id = ",\n".join(f"  {item['node']}: {item['tag_id']}" for item in sorted(tags, key=lambda item: NODES.index(item["node"])))
    sizes = ",\n".join(f"  {item['tag_id']}: {item['size_mm']}" for item in tags)
    binding_rows = ",\n".join(
        f"  {{ tag_id: {item['tag_id']}, node: '{item['node']}', size_mm: {item['size_mm']} }}" for item in tags
    )
    return f'''// Generated by tools/apriltag_sheet/apriltag_sheet.py; edit tag_bindings.json instead.
export type AprilTagId = {ids};
export type TagNode = 'MAIN' | 'G1' | 'G2' | 'G3' | 'G4';

export interface TagBinding {{
  readonly tag_id: AprilTagId;
  readonly node: TagNode;
  readonly size_mm: number;
}}

export const TAG_BINDINGS: readonly TagBinding[] = [
{binding_rows},
];

export const TAG_ID_TO_NODE: Readonly<Record<AprilTagId, TagNode>> = Object.freeze({{
{id_to_node},
}});

export const NODE_TO_TAG_ID: Readonly<Record<TagNode, AprilTagId>> = Object.freeze({{
{node_to_id},
}});

export const TAG_SIZE_MM_BY_ID: Readonly<Record<AprilTagId, number>> = Object.freeze({{
{sizes},
}});

export function tagBindingForId(tagId: number, bindings: readonly TagBinding[] = TAG_BINDINGS): TagBinding {{
  const binding = bindings.find((entry) => entry.tag_id === tagId);
  if (binding === undefined) throw new Error(`No AprilTag binding exists for id ${{tagId}}.`);
  return binding;
}}

export function tagNodeForId(tagId: number, bindings: readonly TagBinding[] = TAG_BINDINGS): TagNode {{
  return tagBindingForId(tagId, bindings).node;
}}

export function tagIdForNode(node: TagNode, bindings: readonly TagBinding[] = TAG_BINDINGS): AprilTagId {{
  const binding = bindings.find((entry) => entry.node === node);
  if (binding === undefined) throw new Error(`No AprilTag id is bound to node ${{node}}.`);
  return binding.tag_id;
}}

export function tagSizeMmForId(tagId: number, bindings: readonly TagBinding[] = TAG_BINDINGS): number {{
  return tagBindingForId(tagId, bindings).size_mm;
}}
'''


def binding_json_text(bindings: dict[str, Any]) -> str:
    return json.dumps(bindings, indent=2, ensure_ascii=True) + "\n"


def pattern_json_text(root: Path) -> str:
    return json.dumps(pattern_document(root), indent=2, ensure_ascii=True) + "\n"


def _atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="", dir=path.parent, prefix=f".{path.name}.", suffix=".tmp", delete=False) as stream:
            temp_name = stream.name
            stream.write(content)
        os.replace(temp_name, path)
    finally:
        if temp_name is not None and os.path.exists(temp_name):
            os.unlink(temp_name)


def write_bindings_and_types(root: Path, value: Any) -> dict[str, Any]:
    bindings = validate_bindings(value)
    binding_path = root / BINDINGS_PATH
    types_path = root / GENERATED_TS_PATH
    old_binding = binding_path.read_bytes() if binding_path.exists() else None
    _atomic_write(binding_path, binding_json_text(bindings))
    try:
        _atomic_write(types_path, generated_typescript(bindings))
    except OSError:
        if old_binding is None:
            binding_path.unlink(missing_ok=True)
        else:
            binding_path.write_bytes(old_binding)
        raise
    return bindings


def generate(root: Path) -> None:
    bindings = load_bindings(root)
    _atomic_write(root / GENERATED_TS_PATH, generated_typescript(bindings))
    _atomic_write(root / PATTERNS_PATH, pattern_json_text(root))


def check_generated(root: Path) -> None:
    bindings = load_bindings(root)
    expected = {
        GENERATED_TS_PATH: generated_typescript(bindings),
        PATTERNS_PATH: pattern_json_text(root),
    }
    stale = [str(path) for path, content in expected.items() if not (root / path).is_file() or (root / path).read_text(encoding="utf-8") != content]
    if stale:
        raise BindingError("generated AprilTag outputs are stale: " + ", ".join(stale))
