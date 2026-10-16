import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(os.environ.get(
    "NRF_IMAGE_INTEGRITY_SCRIPT",
    str(ROOT / "nRF54L15" / "scripts" / "nrf_image_integrity.py"),
))


def ihex_record(record_type: int, address: int, data: bytes) -> str:
    body = bytes((len(data), (address >> 8) & 0xFF, address & 0xFF, record_type)) + data
    return ":" + (body + bytes((-sum(body) & 0xFF,))).hex().upper()


def write_hex(path: Path, address: int, data: bytes) -> None:
    records = []
    upper = address >> 16
    if upper:
        records.append(ihex_record(4, 0, upper.to_bytes(2, "big")))
    records.extend((ihex_record(0, address & 0xFFFF, data), ihex_record(1, 0, b"")))
    path.write_bytes(("\r\n".join(records) + "\r\n").encode("ascii"))


def run_helper(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def prepare_args(source: Path, padded: Path, expected_bin: Path, manifest: Path) -> tuple[str, ...]:
    return (
        "prepare",
        "--input-hex", str(source),
        "--padded-hex", str(padded),
        "--expected-bin", str(expected_bin),
        "--manifest", str(manifest),
    )


def test_prepare_records_padded_payload_length_and_sha256(tmp_path: Path) -> None:
    source = tmp_path / "zephyr.hex"
    padded = tmp_path / "zephyr.padded.hex"
    expected_bin = tmp_path / "zephyr.bin"
    manifest = tmp_path / "integrity.json"
    raw = bytes(range(1, 11))
    payload = raw + b"\xff" * 6
    write_hex(source, 0x1000, raw)
    write_hex(padded, 0x1000, payload)
    expected_bin.write_bytes(raw)

    result = run_helper(*prepare_args(source, padded, expected_bin, manifest))

    assert result.returncode == 0, result.stderr
    actual = json.loads(manifest.read_text(encoding="utf-8"))
    assert actual["image_start"] == 0x1000
    assert actual["region_length"] == len(payload)
    assert actual["payload_length"] == len(payload)
    assert actual["sha256"] == hashlib.sha256(payload).hexdigest()


def test_prepare_rejects_hex_shorter_than_elf_binary_with_distinct_exit(tmp_path: Path) -> None:
    source = tmp_path / "zephyr.hex"
    padded = tmp_path / "zephyr.padded.hex"
    expected_bin = tmp_path / "zephyr.bin"
    manifest = tmp_path / "integrity.json"
    write_hex(source, 0x2000, b"short")
    write_hex(padded, 0x2000, b"short" + b"\xff" * 11)
    expected_bin.write_bytes(bytes(range(16)))

    result = run_helper(*prepare_args(source, padded, expected_bin, manifest))

    assert result.returncode == 4
    assert "truncated" in result.stderr.lower()
    assert not manifest.exists()


def test_prepare_rejects_skipped_padding_and_verify_rejects_last_byte_change(tmp_path: Path) -> None:
    source = tmp_path / "zephyr.hex"
    padded = tmp_path / "zephyr.padded.hex"
    expected_bin = tmp_path / "zephyr.bin"
    manifest = tmp_path / "integrity.json"
    readback = tmp_path / "readback.bin"
    raw = bytes(range(1, 11))
    payload = raw + b"\xff" * 6
    write_hex(source, 0x3000, raw)
    expected_bin.write_bytes(raw)

    write_hex(padded, 0x3000, raw)
    skipped = run_helper(*prepare_args(source, padded, expected_bin, manifest))
    assert skipped.returncode != 0

    write_hex(padded, 0x3000, payload)
    prepared = run_helper(*prepare_args(source, padded, expected_bin, manifest))
    assert prepared.returncode == 0, prepared.stderr
    readback.write_bytes(payload)
    valid = run_helper("verify", "--manifest", str(manifest), "--readback", str(readback))
    assert valid.returncode == 0, valid.stderr

    changed = bytearray(payload)
    changed[-1] ^= 0x01
    readback.write_bytes(changed)
    altered = run_helper("verify", "--manifest", str(manifest), "--readback", str(readback))
    assert altered.returncode == 1
    assert "sha-256" in altered.stderr.lower() or "digest" in altered.stderr.lower()
