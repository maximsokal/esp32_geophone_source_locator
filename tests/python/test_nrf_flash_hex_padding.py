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
    script_path: Path = SCRIPT,
) -> subprocess.CompletedProcess[str]:
    command = [
        powershell,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script_path),
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


def contiguous_records(start: int, end: int) -> list[str]:
    """Encode one contiguous absolute-address run using independent type-04 records."""
    records = []
    active_base = 0
    cursor = start
    while cursor < end:
        base = cursor & ~0xFFFF
        if base != active_base:
            records.append(ihex_record(4, 0, (base >> 16).to_bytes(2, "big")))
            active_base = base
        offset = cursor - base
        count = min(255, end - cursor, 0x10000 - offset)
        data = bytes(((cursor + index) % 251) + 1 for index in range(count))
        records.append(ihex_record(0, offset, data))
        cursor += count
    return records


def assert_three_production_runs_padded(
    powershell: str, tmp_path: Path, script_path: Path = SCRIPT
) -> None:
    spans = ((0x00000, 0x0047C), (0x00480, 0x4913C), (0x49140, 0x4F17C))
    original = [record for start, end in spans for record in contiguous_records(start, end)]
    original.append(ihex_record(1, 0, b""))
    input_path = tmp_path / "production-three-runs.hex"
    output_path = tmp_path / "production-three-runs.padded.hex"
    write_hex(input_path, original)

    result = run_padder(powershell, input_path, output_path, script_path=script_path)

    expected = []
    active_base = 0
    run_ends = {0x0047C: 4, 0x4913C: 4, 0x4F17C: 4}
    observed_ends = set()
    for record in original:
        count = int(record[1:3], 16)
        address = int(record[3:7], 16)
        record_type = int(record[7:9], 16)
        data = bytes.fromhex(record[9:-2])
        expected.append(record)
        if record_type == 4:
            active_base = int.from_bytes(data, "big") << 16
        elif record_type == 0 and count:
            absolute_end = active_base + address + count
            if absolute_end in run_ends:
                expected.append(ihex_record(0, address + count, b"\xff" * run_ends[absolute_end]))
                observed_ends.add(absolute_end)
    assert observed_ends == set(run_ends)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x4f17c pad=4 bytes",
        "hex run pad: run=0x0..0x47c pad=4 bytes",
        "hex run pad: run=0x480..0x4913c pad=4 bytes",
        "hex run pad: run=0x49140..0x4f17c pad=4 bytes",
        "hex total pad: bytes=12",
    ], result.stdout
    assert output_records(output_path) == expected

    second_output = tmp_path / "production-three-runs.second.hex"
    second = run_padder(powershell, output_path, second_output, script_path=script_path)
    assert second.returncode == 0, second.stderr
    assert second_output.read_bytes() == output_path.read_bytes()


def test_three_production_runs_are_each_padded_and_only_padding_is_inserted(
    powershell: str, tmp_path: Path
):
    assert_three_production_runs_padded(powershell, tmp_path)


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
    assert result.stdout.splitlines() == [
        f"hex tail pad: end=0x{0x1000 + data_size:x} pad={padding} bytes",
        f"hex run pad: run=0x1000..0x{0x1000 + data_size:x} pad={padding} bytes",
        f"hex total pad: bytes={padding}",
    ]
    assert output_records(output_path) == [input_records[0], expected_pad, input_records[1]]
    assert sum(bytes.fromhex(expected_pad[1:])) & 0xFF == 0

    second_output = tmp_path / f"partial-{data_size}.second.hex"
    second = run_padder(powershell, output_path, second_output)
    assert second.returncode == 0, second.stderr
    assert second.stdout.splitlines() == [
        f"hex tail pad: end=0x{0x1000 + data_size + padding:x} pad=0 bytes",
        "hex total pad: bytes=0",
    ]
    assert second_output.read_bytes() == output_path.read_bytes()


def test_aligned_tail_is_copied_without_a_data_record(powershell: str, tmp_path: Path):
    input_records = [ihex_record(0, 0x2000, bytes(range(16))), ihex_record(1, 0, b"")]
    input_path = tmp_path / "aligned.hex"
    output_path = tmp_path / "aligned.output.hex"
    write_hex(input_path, input_records)

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x2010 pad=0 bytes",
        "hex total pad: bytes=0",
    ]
    assert output_records(output_path) == input_records
    assert output_path.read_bytes() == ("\n".join(input_records) + "\n").encode("ascii")


def test_every_contiguous_run_is_padded(powershell: str, tmp_path: Path):
    first = ihex_record(0, 0x0100, b"\x11\x12")
    highest = ihex_record(0, 0x0200, b"\x21\x22\x23")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "gap.hex"
    output_path = tmp_path / "gap.output.hex"
    write_hex(input_path, [first, highest, eof])

    result = run_padder(powershell, input_path, output_path)

    first_padding = ihex_record(0, 0x0102, b"\xff" * 14)
    highest_padding = ihex_record(0, 0x0203, b"\xff" * 13)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x203 pad=13 bytes",
        "hex run pad: run=0x100..0x102 pad=14 bytes",
        "hex run pad: run=0x200..0x203 pad=13 bytes",
        "hex total pad: bytes=27",
    ]
    assert output_records(output_path) == [first, first_padding, highest, highest_padding, eof]


def test_next_run_inside_same_line_fills_only_the_four_byte_gap(
    powershell: str, tmp_path: Path
):
    first = ihex_record(0, 0x1000, b"\x11\x12\x13\x14")
    second = ihex_record(0, 0x1008, b"\x21" * 8)
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "same-line-gap.hex"
    output_path = tmp_path / "same-line-gap.padded.hex"
    write_hex(input_path, [first, second, eof])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x1010 pad=0 bytes",
        "hex run pad: run=0x1000..0x1004 pad=4 bytes",
        "hex total pad: bytes=4",
    ]
    assert output_records(output_path) == [
        first,
        ihex_record(0, 0x1004, b"\xff" * 4),
        second,
        eof,
    ]


def test_runs_separated_by_sixteen_or_more_bytes_are_independently_padded(
    powershell: str, tmp_path: Path
):
    first = ihex_record(0, 0x2000, b"\x31" * 4)
    second = ihex_record(0, 0x2020, b"\x42" * 8)
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "wide-gap.hex"
    output_path = tmp_path / "wide-gap.padded.hex"
    write_hex(input_path, [first, second, eof])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x2028 pad=8 bytes",
        "hex run pad: run=0x2000..0x2004 pad=12 bytes",
        "hex run pad: run=0x2020..0x2028 pad=8 bytes",
        "hex total pad: bytes=20",
    ]
    assert output_records(output_path) == [
        first,
        ihex_record(0, 0x2004, b"\xff" * 12),
        second,
        ihex_record(0, 0x2028, b"\xff" * 8),
        eof,
    ]


def test_run_ending_one_byte_before_boundary_adds_one_byte(
    powershell: str, tmp_path: Path
):
    data = ihex_record(0, 0x3000, bytes(range(1, 16)))
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "one-before-boundary.hex"
    output_path = tmp_path / "one-before-boundary.padded.hex"
    write_hex(input_path, [data, eof])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x300f pad=1 bytes",
        "hex run pad: run=0x3000..0x300f pad=1 bytes",
        "hex total pad: bytes=1",
    ]
    assert output_records(output_path) == [data, ihex_record(0, 0x300F, b"\xff"), eof]


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
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x1fffa pad=22 bytes",
        "hex run pad: run=0x10010..0x10011 pad=15 bytes",
        "hex run pad: run=0x1fff9..0x1fffa pad=22 bytes",
        "hex total pad: bytes=37",
    ]
    assert output_records(output_path) == [
        base_one,
        data,
        first_pad,
        base_two,
        second_pad,
        restore_base_one,
        later_data,
        ihex_record(0, 0x0011, b"\xff" * 15),
        eof,
    ]
    assert all(
        sum(bytes.fromhex(record[1:])) & 0xFF == 0
        for record in (first_pad, base_two, second_pad, restore_base_one)
    )


def test_extended_segment_padding_splits_at_64k_boundary(powershell: str, tmp_path: Path):
    segment_base = ihex_record(2, 0, b"\x0F\xFF")
    high_data = ihex_record(0, 0xFFF9, b"\x5A")
    later_data = ihex_record(0, 0x0010, b"\xA5")
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / "segment-boundary.hex"
    output_path = tmp_path / "segment-boundary.output.hex"
    write_hex(input_path, [segment_base, high_data, later_data, eof])

    result = run_padder(powershell, input_path, output_path, alignment=24)

    first_pad = ihex_record(0, 0xFFFA, b"\xff" * 6)
    next_segment_base = ihex_record(2, 0, b"\x1F\xFF")
    second_pad = ihex_record(0, 0, b"\xff" * 8)
    restore_segment_base = ihex_record(2, 0, b"\x0F\xFF")
    later_pad = ihex_record(0, 0x0011, b"\xff" * 7)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x1ffea pad=14 bytes",
        "hex run pad: run=0x10000..0x10001 pad=7 bytes",
        "hex run pad: run=0x1ffe9..0x1ffea pad=14 bytes",
        "hex total pad: bytes=21",
    ]
    assert output_records(output_path) == [
        segment_base,
        high_data,
        first_pad,
        next_segment_base,
        second_pad,
        restore_segment_base,
        later_data,
        later_pad,
        eof,
    ]


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
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x12364 pad=12 bytes",
        "hex run pad: run=0x12360..0x12364 pad=12 bytes",
        "hex total pad: bytes=12",
    ]
    assert output_records(output_path) == [segment, data, padding, eof]


@pytest.mark.parametrize(
    ("base_type", "first_base", "second_base"),
    ((4, b"\x00\x01", b"\x00\x02"), (2, b"\x10\x00", b"\x20\x00")),
)
def test_contiguous_run_crosses_64k_boundary_with_segment_and_linear_bases(
    powershell: str,
    tmp_path: Path,
    base_type: int,
    first_base: bytes,
    second_base: bytes,
):
    first_base_record = ihex_record(base_type, 0, first_base)
    first_data = ihex_record(0, 0xFFFC, b"\x51" * 4)
    second_base_record = ihex_record(base_type, 0, second_base)
    second_data = ihex_record(0, 0, b"\x62" * 4)
    eof = ihex_record(1, 0, b"")
    input_path = tmp_path / f"cross-base-{base_type}.hex"
    output_path = tmp_path / f"cross-base-{base_type}.padded.hex"
    write_hex(input_path, [first_base_record, first_data, second_base_record, second_data, eof])

    result = run_padder(powershell, input_path, output_path)

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "hex tail pad: end=0x20004 pad=12 bytes",
        "hex run pad: run=0x1fffc..0x20004 pad=12 bytes",
        "hex total pad: bytes=12",
    ]
    assert output_records(output_path) == [
        first_base_record,
        first_data,
        second_base_record,
        second_data,
        ihex_record(0, 4, b"\xff" * 12),
        eof,
    ]


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
