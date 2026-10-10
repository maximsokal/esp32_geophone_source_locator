from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "nRF54L15" / "pad_hex_rram_line.ps1"


def ihex_record(record_type: int, address: int, data: bytes) -> str:
    """Small independent Intel HEX encoder used to build and check fixtures."""
    body = bytes(
        (
            len(data),
            (address >> 8) & 0xFF,
            address & 0xFF,
            record_type,
        )
    ) + data
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes((checksum,))).hex().upper()


def write_hex(path: Path, records: list[str]) -> None:
    path.write_bytes(("\r\n".join(records) + "\r\n").encode("ascii"))


@pytest.fixture(scope="session")
def powershell() -> str:
    executable = shutil.which("powershell.exe")
    if executable is None:
        pytest.skip("powershell.exe is unavailable; Intel HEX PowerShell tests cannot run")
    return executable


def run_padder(
    powershell: str,
    input_hex: Path,
    output_hex: Path,
    alignment: int | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        powershell,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(SCRIPT),
        "-InputHex",
        str(input_hex),
        "-OutputHex",
        str(output_hex),
    ]
    if alignment is not None:
        command.extend(("-Alignment", str(alignment)))
    return subprocess.run(command, capture_output=True, text=True, encoding="ascii", errors="replace")


def output_records(path: Path) -> list[str]:
    return path.read_text(encoding="ascii").splitlines()


@pytest.mark.parametrize(
    ("data_size", "padding"),
    ((8, 8), (4, 12), (1, 15)),
)
def test_partial_tail_is_padded_with_ff_and_valid_checksum(
    powershell: str, tmp_path: Path, data_size: int, padding: int
):
    data = bytes(range(1, data_size + 1))
    input_records = [ihex_record(0, 0x1000, data), ihex_record(1, 0, b"")]
    input_path = tmp_path / f"partial-{data_size}.hex"
    output_path = tmp_path / f"partial-{data_size}.padded.hex"
    write_hex(input_path, input_records)

    result = run_padder(powershell, input_path, output_path)

    expected_pad = ihex_record(0, 0x1000 + data_size, b"\xff" * padding)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"hex tail pad: end=0x{0x1000 + data_size:x} pad={padding} bytes"
    assert output_records(output_path) == [input_records[0], expected_pad, input_records[1]]
    assert sum(bytes.fromhex(expected_pad[1:])) & 0xFF == 0

    second_output = tmp_path / f"partial-{data_size}.second.hex"
    second = run_padder(powershell, output_path, second_output)
    assert second.returncode == 0, second.stderr
    assert second.stdout.strip() == f"hex tail pad: end=0x{0x1000 + data_size + padding:x} pad=0 bytes"
    assert second_output.read_bytes() == output_path.read_bytes()


def test_aligned_tail_is_copied_without_a_data_record(powershell: str, tmp_path: Path):
    input_records = [ihex_record(0, 0x2000, bytes(range(16))), ihex_record(1, 0, b"")]
    input_path = tmp_path / "aligned.hex"
    output_path = tmp_path / "aligned.output.hex"
    write_hex(input_path, input_records)

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "hex tail pad: end=0x2010 pad=0 bytes"
    assert output_records(output_path) == input_records
    assert output_path.read_bytes() == ("\n".join(input_records) + "\n").encode("ascii")


def test_only_global_highest_data_run_is_padded(powershell: str, tmp_path: Path):
    first = ihex_record(0, 0x0100, b"\x11\x12")
    highest = ihex_record(0, 0x0200, b"\x21\x22\x23")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "gap.hex"
    output_path = tmp_path / "gap.output.hex"
    write_hex(input_path, [first, highest, eof])

    result = run_padder(powershell, input_path, output_path)

    padding = ihex_record(0, 0x0203, b"\xff" * 13)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "hex tail pad: end=0x203 pad=13 bytes"
    assert output_records(output_path) == [first, highest, padding, eof]


def test_extended_linear_padding_splits_at_64k_boundary(powershell: str, tmp_path: Path):
    base_one = ihex_record(4, 0, b"\x00\x01")
    data = ihex_record(0, 0xFFF9, b"\x5A")
    later_data = ihex_record(0, 0x0010, b"\xA5")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "linear-boundary.hex"
    output_path = tmp_path / "linear-boundary.output.hex"
    write_hex(input_path, [base_one, data, later_data, eof])

    result = run_padder(powershell, input_path, output_path, alignment=24)

    first_pad = ihex_record(0, 0xFFFA, b"\xff" * 6)
    base_two = ihex_record(4, 0, b"\x00\x02")
    second_pad = ihex_record(0, 0, b"\xff" * 16)
    restore_base_one = ihex_record(4, 0, b"\x00\x01")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "hex tail pad: end=0x1fffa pad=22 bytes"
    assert output_records(output_path) == [
        base_one,
        data,
        first_pad,
        base_two,
        second_pad,
        restore_base_one,
        later_data,
        eof,
    ]
    assert all(
        sum(bytes.fromhex(record[1:])) & 0xFF == 0
        for record in (first_pad, base_two, second_pad, restore_base_one)
    )


def test_extended_segment_base_is_applied_to_end_address(powershell: str, tmp_path: Path):
    segment = ihex_record(2, 0, b"\x12\x34")
    data = ihex_record(0, 0x0020, b"\xAA\xBB\xCC\xDD")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "segment.hex"
    output_path = tmp_path / "segment.output.hex"
    write_hex(input_path, [segment, data, eof])

    result = run_padder(powershell, input_path, output_path)

    padding = ihex_record(0, 0x0024, b"\xff" * 12)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "hex tail pad: end=0x12364 pad=12 bytes"
    assert output_records(output_path) == [segment, data, padding, eof]


def test_start_address_records_and_eof_are_preserved(powershell: str, tmp_path: Path):
    data = ihex_record(0, 0x3000, b"\x31" * 8)
    start_segment = ihex_record(3, 0, b"\x00\x01\x02\x03")
    start_linear = ihex_record(5, 0, b"\x12\x34\x56\x78")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "start-addresses.hex"
    output_path = tmp_path / "start-addresses.output.hex"
    write_hex(input_path, [data, start_segment, start_linear, eof])

    result = run_padder(powershell, input_path, output_path)

    padding = ihex_record(0, 0x3008, b"\xff" * 8)
    assert result.returncode == 0, result.stderr
    assert output_records(output_path) == [data, padding, start_segment, start_linear, eof]


def test_bad_checksum_fails_with_clear_error(powershell: str, tmp_path: Path):
    valid_data = ihex_record(0, 0x1000, b"\xAA")
    invalid_data = valid_data[:-2] + ("00" if valid_data[-2:] != "00" else "01")
    input_path = tmp_path / "bad-checksum.hex"
    output_path = tmp_path / "bad-checksum.output.hex"
    write_hex(input_path, [invalid_data, ihex_record(1, 0, b"")])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode != 0
    assert "checksum is invalid" in result.stderr.lower()
    assert not output_path.exists()


def test_missing_eof_fails_with_clear_error(powershell: str, tmp_path: Path):
    input_path = tmp_path / "missing-eof.hex"
    output_path = tmp_path / "missing-eof.output.hex"
    write_hex(input_path, [ihex_record(0, 0x1000, b"\xAA")])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode != 0
    assert "missing its eof record" in result.stderr.lower()
    assert not output_path.exists()
