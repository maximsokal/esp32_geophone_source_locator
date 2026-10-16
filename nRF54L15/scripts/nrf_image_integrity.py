#!/usr/bin/env python3
"""Prepare and verify a full-region digest for the padded nRF Intel HEX image."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


class TruncatedImageError(ValueError):
    pass


def read_ihex(path: Path) -> dict[int, int]:
    memory: dict[int, int] = {}
    base = 0
    eof = False
    lines = path.read_text(encoding="ascii").splitlines()
    if not lines:
        raise ValueError(f"Intel HEX is empty: {path}")
    if any(
        not line or line != line.strip() or re.fullmatch(r":[0-9A-Fa-f]+", line) is None
        for line in lines
    ):
        raise ValueError(f"Intel HEX contains a malformed record: {path}")
    for line_number, line in enumerate(lines, 1):
        if eof or not line.startswith(":"):
            raise ValueError(f"invalid Intel HEX record at line {line_number}")
        try:
            record = bytes.fromhex(line[1:])
        except ValueError as exc:
            raise ValueError(f"invalid Intel HEX bytes at line {line_number}") from exc
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 0xFF:
            raise ValueError(f"invalid Intel HEX length/checksum at line {line_number}")
        count = record[0]
        address = (record[1] << 8) | record[2]
        record_type = record[3]
        data = record[4:-1]
        if address + count > 0x10000:
            raise ValueError(f"Intel HEX record crosses 64 KiB at line {line_number}")
        if record_type == 0:
            start = base + address
            if start + count > 0x100000000:
                raise ValueError(f"Intel HEX address overflow at line {line_number}")
            for offset, value in enumerate(data):
                absolute = start + offset
                if absolute in memory:
                    raise ValueError(f"overlapping Intel HEX data at 0x{absolute:x}")
                memory[absolute] = value
        elif record_type == 1:
            if count or address or line_number != len(lines):
                raise ValueError(f"invalid Intel HEX EOF at line {line_number}")
            eof = True
        elif record_type in (2, 4):
            if count != 2 or address:
                raise ValueError(f"invalid Intel HEX base record at line {line_number}")
            base = int.from_bytes(data, "big") * (16 if record_type == 2 else 65536)
        elif record_type in (3, 5):
            if count != 4 or address:
                raise ValueError(f"invalid Intel HEX start record at line {line_number}")
        else:
            raise ValueError(f"unsupported Intel HEX record type {record_type}")
    if not eof or not memory:
        raise ValueError(f"Intel HEX is missing data or EOF: {path}")
    return memory


def image_region(memory: dict[int, int]) -> tuple[int, bytes]:
    start = min(memory)
    end = max(memory) + 1
    length = end - start
    if length > 64 * 1024 * 1024:
        raise ValueError(f"image address span is unexpectedly large: {length} bytes")
    payload = bytearray(b"\xff") * length
    for address, value in memory.items():
        payload[address - start] = value
    return start, bytes(payload)


def contiguous_runs(memory: dict[int, int]) -> list[tuple[int, int]]:
    addresses = sorted(memory)
    runs: list[tuple[int, int]] = []
    start = previous = addresses[0]
    for address in addresses[1:]:
        if address != previous + 1:
            runs.append((start, previous + 1))
            start = address
        previous = address
    runs.append((start, previous + 1))
    return runs


def prepare(args: argparse.Namespace) -> int:
    source = read_ihex(args.input_hex)
    source_start, source_payload = image_region(source)
    expected_length = args.expected_bin.stat().st_size
    if len(source_payload) < expected_length:
        raise TruncatedImageError(
            f"truncated input image: HEX spans {len(source_payload)} bytes, "
            f"ELF binary expects {expected_length}"
        )

    padded = read_ihex(args.padded_hex)
    padded_start, payload = image_region(padded)
    if padded_start != source_start:
        raise ValueError("padded HEX image start differs from the input HEX image")
    if any(padded.get(address) != value for address, value in source.items()):
        raise ValueError("padded HEX omits or changes input image data")
    if any(value != 0xFF for address, value in padded.items() if address not in source):
        raise ValueError("padded HEX adds non-0xFF bytes")
    if any(end % 16 for _, end in contiguous_runs(padded)):
        raise ValueError("padded HEX has an unaligned data run; RRAM line padding is missing")
    if len(payload) < expected_length:
        raise ValueError("padded HEX region is shorter than the ELF binary")

    metadata = {
        "schema_version": 1,
        "image_start": padded_start,
        "region_length": len(payload),
        "payload_length": len(payload),
        "source_length": len(source_payload),
        "expected_bin_length": expected_length,
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    args.manifest.write_text(json.dumps(metadata, sort_keys=True) + "\n", encoding="utf-8")
    return 0


def verify(args: argparse.Namespace) -> int:
    metadata = json.loads(args.manifest.read_text(encoding="utf-8"))
    payload = args.readback.read_bytes()
    if len(payload) != metadata["region_length"]:
        raise ValueError(
            f"full image readback length mismatch: got {len(payload)}, "
            f"expected {metadata['region_length']}"
        )
    digest = hashlib.sha256(payload).hexdigest()
    if digest != metadata["sha256"]:
        raise ValueError("full image readback SHA-256 digest mismatch")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--input-hex", type=Path, required=True)
    prepare_parser.add_argument("--padded-hex", type=Path, required=True)
    prepare_parser.add_argument("--expected-bin", type=Path, required=True)
    prepare_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.add_argument("--readback", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        return prepare(args) if args.command == "prepare" else verify(args)
    except TruncatedImageError as exc:
        print(str(exc), file=sys.stderr)
        return 4
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
