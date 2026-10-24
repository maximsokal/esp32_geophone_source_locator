from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(
    os.environ.get("VERIFY_SCRIPT", str(ROOT / "nRF54L15" / "verify_rram_tail.ps1"))
).resolve()


def ihex_record(record_type: int, address: int, data: bytes) -> str:
    """Independent Intel HEX encoder for verifier fixtures."""
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


def data_hex(path: Path, address: int, data: bytes) -> None:
    records = []
    upper = address >> 16
    if upper:
        records.append(ihex_record(4, 0, upper.to_bytes(2, "big")))
    records.extend(
        (
            ihex_record(0, address & 0xFFFF, data),
            ihex_record(1, 0, b""),
        )
    )
    write_hex(path, records)


def mdw_line(address: int, data: bytes) -> str:
    assert len(data) % 4 == 0
    words = [
        f"{int.from_bytes(data[offset : offset + 4], 'little'):08x}"
        for offset in range(0, len(data), 4)
    ]
    return f"0x{address:08x}: " + " ".join(words)


def padded_run_fixture(directory: Path):
    runs = ((0x1000, 0x108C), (0x1100, 0x118C), (0x1200, 0x128C))
    source_records = []
    padded_records = []
    image_bytes = {}
    source_path = directory / "zephyr.hex"
    padded_path = directory / "zephyr.padded.hex"
    for start, end in runs:
        data = bytes(((start + index) % 251) + 1 for index in range(end - start))
        source_records.append(ihex_record(0, start, data))
        padded_records.append(ihex_record(0, start, data))
        image_bytes.update({start + index: value for index, value in enumerate(data)})
        padding = (-end) % 16
        if padding:
            padded_records.append(ihex_record(0, end, b"\xff" * padding))
            image_bytes.update({end + index: 0xFF for index in range(padding)})
    source_records.append(ihex_record(1, 0, b""))
    padded_records.append(ihex_record(1, 0, b""))
    write_hex(source_path, source_records)
    write_hex(padded_path, padded_records)
    return runs, image_bytes, padded_path


def mdw_windows(runs, image_bytes, *, bytes_to_check: int = 64, mismatch=None) -> str:
    lines = []
    for start, end in runs:
        line_start = max(start, ((end - 1) // 16) * 16)
        line_end = ((end + 15) // 16) * 16
        tail_start = max(start, end - bytes_to_check)
        first_end = min(end, start + 16)
        for window_start, window_end in (
            (line_start, line_end),
            (tail_start, end),
            (start, first_end),
        ):
            word_start = window_start & ~3
            word_end = (window_end + 3) & ~3
            data = bytearray(image_bytes[address] for address in range(word_start, word_end))
            if mismatch is not None and word_start <= mismatch < word_end:
                data[mismatch - word_start] ^= 0xFF
            lines.append(mdw_line(word_start, bytes(data)))
    return "\n".join(lines)


@pytest.fixture(scope="session")
def powershell() -> str:
    executable = shutil.which("powershell.exe")
    if executable is None:
        pytest.skip("powershell.exe is unavailable; RRAM readback tests cannot run")
    return executable


def runner_fixture(tmp_path: Path, stub_output: str, stub_exit: int = 0, *, dump_payload: bytes | None = None):
    app_dir = tmp_path / "build" / "zephyr" / "zephyr"
    board_dir = tmp_path / "board with spaces"
    board_cfg = board_dir / "support" / "openocd.cfg"
    scripts_dir = tmp_path / "OpenOCD scripts"
    app_dir.mkdir(parents=True)
    board_cfg.parent.mkdir(parents=True)
    board_cfg.write_text("# stub board config\n", encoding="ascii")
    scripts_dir.mkdir()

    stub = tmp_path / "stub openocd.cmd"
    args_file = tmp_path / "openocd-args.txt"
    lines = [
        "@echo off",
        'if defined RRAM_STUB_ARGS_FILE echo %*>"%RRAM_STUB_ARGS_FILE%"',
    ]
    if dump_payload is not None:
        payload_file = tmp_path / "stub dump.bin"
        payload_file.write_bytes(dump_payload)
        dump_helper = tmp_path / "stub dump.py"
        dump_helper.write_text(
            "from pathlib import Path\nimport re, sys\n"
            "command = next(arg for arg in sys.argv[2:] if arg.startswith('dump_image '))\n"
            "match = re.fullmatch(r'dump_image \\{(.+)\\} 0x[0-9a-f]+ [0-9]+', command)\n"
            "assert match is not None, command\n"
            "Path(match[1]).write_bytes(Path(sys.argv[1]).read_bytes())\n",
            encoding="utf-8",
        )
        lines.extend((f'"{sys.executable}" "{dump_helper}" "{payload_file}" %*',
                      "if errorlevel 1 exit /b 1"))
    if stub_output:
        lines.extend(f"echo {line}" for line in stub_output.splitlines())
    lines.append(f"exit /b {stub_exit}")
    stub.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")
    runners = (
        "config:\n"
        f"  board_dir: {board_dir.as_posix()}\n"
        f"  openocd: {stub.as_posix()}\n"
        "  openocd_search:\n"
        f"    - {scripts_dir.as_posix()}\n"
        f"    - {(tmp_path / 'ignored scripts').as_posix()}\n"
    )
    (app_dir / "runners.yaml").write_text(runners, encoding="ascii")
    return app_dir, stub, scripts_dir, board_cfg, args_file


def run_verifier(
    powershell: str,
    image: Path,
    app_dir: Path,
    args_file: Path,
    *,
    bytes_to_check: int | None = None,
    openocd: Path | None = None,
    scripts_dir: Path | None = None,
    board_cfg: Path | None = None,
    image_manifest: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        powershell,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(SCRIPT),
        "-Hex",
        str(image),
        "-BuildAppDir",
        str(app_dir),
    ]
    for name, value in (
        ("-Bytes", bytes_to_check),
        ("-OpenOcd", openocd),
        ("-ScriptsDir", scripts_dir),
        ("-BoardCfg", board_cfg),
        ("-ImageManifest", image_manifest),
    ):
        if value is not None:
            command.extend((name, str(value)))
    env = os.environ.copy()
    env["RRAM_STUB_ARGS_FILE"] = str(args_file)
    if image_manifest is not None:
        dump_dir = image_manifest.parent / "readback-artifacts"
        dump_dir.mkdir(exist_ok=True)
        env["TEMP"] = env["TMP"] = str(dump_dir)
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="ascii",
        errors="replace",
        env=env,
    )


def expected_openocd_arguments(
    scripts_dir: Path,
    board_cfg: Path,
    address: int,
    word_count: int,
) -> str:
    tokens = (
        "-s",
        scripts_dir.resolve().as_posix(),
        "-f",
        board_cfg.resolve().as_posix(),
        "-c",
        "init",
        "-c",
        "targets nrf54l.cpu",
        "-c",
        "halt",
        "-c",
        f"mdw 0x{address:x} {word_count}",
        "-c",
        "reset run",
        "-c",
        "shutdown",
    )
    return " ".join(f'"{token}"' for token in tokens)


def make_override_paths_with_backslashes(tmp_path: Path) -> tuple[Path, Path, str, str]:
    override_root = tmp_path / "override paths with spaces"
    scripts_dir = override_root / "OpenOCD scripts"
    board_cfg = override_root / "board config" / "openocd.cfg"
    scripts_dir.mkdir(parents=True)
    board_cfg.parent.mkdir()
    board_cfg.write_text("# override board config\n", encoding="ascii")

    scripts_input = str(scripts_dir).replace("/", "\\")
    board_input = str(board_cfg).replace("/", "\\")
    assert "\\" in scripts_input
    assert "\\" in board_input
    return scripts_dir, board_cfg, scripts_input, board_input


def test_matching_tail_passes_and_runners_yaml_paths_are_used(powershell: str, tmp_path: Path):
    address = 0x21000
    expected = bytes(range(1, 65))
    image = tmp_path / "zephyr.padded.hex"
    write_hex(
        image,
        [
            ihex_record(4, 0, b"\x00\x02"),
            ihex_record(0, address & 0xFFFF, expected),
            ihex_record(2, 0, b"\x10\x00"),
            ihex_record(0, 0x0020, b"\xA5" * 4),
            ihex_record(1, 0, b""),
        ],
    )
    secondary = b"\xA5" * 4
    app_dir, _, scripts_dir, board_cfg, args_file = runner_fixture(
        tmp_path,
        mdw_line(0x10020, secondary) + "\n" + mdw_line(address, expected),
    )

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"rram tail check: PASS addr=0x10020 bytes=68 runs=2"
    args = args_file.read_text(encoding="ascii")
    assert "-s" in args and scripts_dir.resolve().as_posix() in args
    assert "-f" in args and board_cfg.resolve().as_posix() in args
    assert f'mdw 0x{address:x} 16' in args
    assert "mdw 0x10020 1" in args


def test_one_byte_mismatch_at_tail_reports_correct_address(powershell: str, tmp_path: Path):
    address = 0x22000
    expected = bytes(range(1, 65))
    actual = expected[:-1] + bytes((expected[-1] ^ 0xFF,))
    image = tmp_path / "mismatch.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, mdw_line(address, actual))

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 1
    assert result.stdout.strip() == (
        f"rram tail check: FAIL addr=0x{address:x} first_diff=0x{address + 63:x} "
        f"expected={expected[-1]:02X} got={actual[-1]:02X}"
    )


def test_all_ff_erased_tail_fails_at_data_comparison(powershell: str, tmp_path: Path):
    address = 0x23000
    expected = bytes(range(1, 65))
    image = tmp_path / "erased-tail.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(
        tmp_path, mdw_line(address, b"\xff" * len(expected))
    )

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 1
    assert result.stdout.strip() == (
        f"rram tail check: FAIL addr=0x{address:x} first_diff=0x{address:x} expected=01 got=FF"
    )
    assert "OpenOCD failure" not in result.stderr


@pytest.mark.parametrize("output", ("", "not an mdw response"))
def test_missing_or_garbled_openocd_output_fails_closed(
    powershell: str, tmp_path: Path, output: str
):
    address = 0x24000
    expected = bytes(range(1, 17))
    image = tmp_path / "no-output.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, output)

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 1
    assert "OpenOCD failure" in result.stderr


def test_fewer_words_than_requested_fails_closed(powershell: str, tmp_path: Path):
    address = 0x25000
    expected = bytes(range(1, 17))
    image = tmp_path / "short-read.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, mdw_line(address, expected[:4]))

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 1
    assert "OpenOCD failure" in result.stderr
    assert "1 of 4 requested words" in result.stderr


@pytest.mark.parametrize("stub_exit", (0, 7))
def test_openocd_error_output_or_nonzero_exit_warns_when_words_are_complete(
    powershell: str, tmp_path: Path, stub_exit: int
):
    address = 0x25500
    expected = bytes(range(1, 17))
    image = tmp_path / "openocd-error.hex"
    data_hex(image, address, expected)
    output = mdw_line(address, expected)
    if stub_exit == 0:
        output = "Error: simulated OpenOCD target failure\n" + output
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, output, stub_exit=stub_exit)

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 0, result.stderr
    assert f"rram tail check: PASS addr=0x{address:x} bytes=16 runs=1" in result.stdout
    if stub_exit == 0:
        assert "rram tail check: WARNING OpenOCD: Error: simulated OpenOCD target failure" in result.stdout
    else:
        assert "rram tail check: WARNING OpenOCD exited with code 7" in result.stdout


def test_override_paths_take_precedence_over_runners_yaml(powershell: str, tmp_path: Path):
    address = 0x26000
    expected = bytes(range(1, 17))
    image = tmp_path / "override.hex"
    data_hex(image, address, expected)
    app_dir, default_stub, _, _, args_file = runner_fixture(tmp_path, "wrong output")
    override_root = tmp_path / "override paths"
    override_root.mkdir()
    override_scripts = override_root / "scripts"
    override_scripts.mkdir()
    override_board = override_root / "board.cfg"
    override_board.write_text("# override\n", encoding="ascii")
    override_stub = tmp_path / "override openocd.cmd"
    override_stub.write_text(
        "@echo off\r\n"
        'if defined RRAM_STUB_ARGS_FILE echo %*>"%RRAM_STUB_ARGS_FILE%"\r\n'
        f"echo {mdw_line(address, expected)}\r\n"
        "exit /b 0\r\n",
        encoding="ascii",
    )

    result = run_verifier(
        powershell,
        image,
        app_dir,
        args_file,
        bytes_to_check=16,
        openocd=override_stub,
        scripts_dir=override_scripts,
        board_cfg=override_board,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"rram tail check: PASS addr=0x{address:x} bytes=16 runs=1"
    args = args_file.read_text(encoding="ascii")
    assert override_scripts.resolve().as_posix() in args
    assert override_board.resolve().as_posix() in args
    assert str(default_stub) not in args


def test_openocd_arguments_quote_tokens_and_normalize_backslash_overrides(
    powershell: str, tmp_path: Path
):
    address = 0x2A000
    expected = bytes(range(1, 17))
    image = tmp_path / "quoted-arguments.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, mdw_line(address, expected))
    scripts_dir, board_cfg, scripts_input, board_input = make_override_paths_with_backslashes(
        tmp_path
    )

    result = run_verifier(
        powershell,
        image,
        app_dir,
        args_file,
        bytes_to_check=16,
        scripts_dir=scripts_input,
        board_cfg=board_input,
    )

    assert result.returncode == 0, result.stderr
    args = args_file.read_text(encoding="ascii").strip()
    expected_args = expected_openocd_arguments(scripts_dir, board_cfg, address, 4)
    assert args == expected_args
    assert f'"-s" "{scripts_dir.resolve().as_posix()}" "-f" "{board_cfg.resolve().as_posix()}"' in args
    assert '"-c" "init"' in args
    assert '"-c" "targets nrf54l.cpu"' in args
    assert f'"-c" "mdw 0x{address:x} 4"' in args
    assert '"-c" "reset run"' in args
    assert '"-c" "shutdown"' in args
    path_arguments = re.search(r'"-s" "([^"]+)" "-f" "([^"]+)"', args)
    assert path_arguments is not None
    assert "\\" not in path_arguments.group(1)
    assert "\\" not in path_arguments.group(2)
    assert f'-s" {scripts_dir.resolve().as_posix()}' not in args
    assert f'-f" {board_cfg.resolve().as_posix()}' not in args


def test_openocd_failure_reports_the_same_quoted_command(
    powershell: str, tmp_path: Path
):
    address = 0x2B000
    expected = bytes(range(1, 17))
    image = tmp_path / "quoted-arguments-failure.hex"
    data_hex(image, address, expected)
    app_dir, stub, _, _, args_file = runner_fixture(tmp_path, "", stub_exit=1)
    scripts_dir, board_cfg, scripts_input, board_input = make_override_paths_with_backslashes(
        tmp_path
    )

    result = run_verifier(
        powershell,
        image,
        app_dir,
        args_file,
        bytes_to_check=16,
        scripts_dir=scripts_input,
        board_cfg=board_input,
    )

    assert result.returncode == 1
    args = expected_openocd_arguments(scripts_dir, board_cfg, address, 4)
    recorded_args = args_file.read_text(encoding="ascii").strip()
    assert recorded_args == args
    command_line = next(
        line for line in result.stderr.splitlines() if line.startswith("rram tail check: command:")
    )
    assert command_line.endswith(args + '"')
    assert str(stub.resolve()) in command_line


def test_verify_rram_tail_has_no_powershell_parse_errors(powershell: str):
    env = os.environ.copy()
    env["VERIFY_SCRIPT"] = str(SCRIPT)
    parse_command = (
        "$tokens = $null; $parseErrors = $null; "
        "[System.Management.Automation.Language.Parser]::ParseFile(" 
        "$env:VERIFY_SCRIPT, [ref]$tokens, [ref]$parseErrors) | Out-Null; "
        "if ($parseErrors.Count -gt 0) { "
        "$parseErrors | ForEach-Object { [Console]::Error.WriteLine($_.Message) }; exit 1 }"
    )
    result = subprocess.run(
        [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", parse_command],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    assert result.returncode == 0, result.stderr


def test_bytes_clips_to_shorter_available_tail(powershell: str, tmp_path: Path):
    address = 0x27000
    expected = bytes(range(1, 13))
    image = tmp_path / "short-image.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, mdw_line(address, expected))

    result = run_verifier(powershell, image, app_dir, args_file, bytes_to_check=64)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == f"rram tail check: PASS addr=0x{address:x} bytes=12 runs=1"
    assert "mdw 0x27000 3" in args_file.read_text(encoding="ascii")


def test_all_runs_match_and_each_run_has_first_tail_and_line_reads(
    powershell: str, tmp_path: Path
):
    runs, image_bytes, image = padded_run_fixture(tmp_path)
    response = mdw_windows(runs, image_bytes)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, response)

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "rram tail check: PASS addr=0x1000 bytes=252 runs=3"
    args = args_file.read_text(encoding="ascii")
    for address in (0x1000, 0x1080, 0x104C, 0x1100, 0x1180, 0x114C, 0x1200, 0x1280, 0x124C):
        assert f"mdw 0x{address:x}" in args


def test_mismatch_in_second_run_tail_reports_its_address(powershell: str, tmp_path: Path):
    runs, image_bytes, image = padded_run_fixture(tmp_path)
    mismatch_address = runs[1][1] - 1
    response = mdw_windows(runs, image_bytes, mismatch=mismatch_address)
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, response)

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 1
    assert f"first_diff=0x{mismatch_address:x}" in result.stdout


def test_lockup_error_with_complete_readback_compares_then_exits_three(
    powershell: str, tmp_path: Path
):
    runs, image_bytes, image = padded_run_fixture(tmp_path)
    response = (
        "Error: [nrf54l.cpu] clearing lockup after double fault\n"
        + mdw_windows(runs, image_bytes)
    )
    app_dir, _, _, _, args_file = runner_fixture(tmp_path, response)

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 3
    assert "rram tail check: PASS addr=0x1000 bytes=252 runs=3" in result.stdout
    assert "rram tail check: WARNING target is in LOCKUP after reset (firmware crashed at boot)" in result.stdout


def test_openocd_error_with_all_words_is_a_warning_and_passes(
    powershell: str, tmp_path: Path
):
    address = 0x28000
    expected = bytes(range(1, 17))
    image = tmp_path / "error-warning.padded.hex"
    data_hex(image, address, expected)
    app_dir, _, _, _, args_file = runner_fixture(
        tmp_path,
        "Error: [nrf54l.cpu] recoverable diagnostic\n" + mdw_line(address, expected),
    )

    result = run_verifier(powershell, image, app_dir, args_file)

    assert result.returncode == 0, result.stderr
    assert "rram tail check: WARNING OpenOCD: Error: [nrf54l.cpu] recoverable diagnostic" in result.stdout
    assert "rram tail check: PASS addr=0x28000 bytes=16 runs=1" in result.stdout


@pytest.mark.parametrize("fault", ["none", "byte", "short", "mdw_byte"])
def test_full_image_readback_is_halted_and_failure_evidence_is_retained(
    powershell: str, tmp_path: Path, fault: str
):
    address = 0x1000
    expected = bytes(range(16))
    actual = bytearray(expected)
    if fault == "byte":
        actual[6] = 0xA5
    elif fault == "short":
        del actual[-4:]
    image = tmp_path / "full.padded.hex"
    data_hex(image, address, expected)
    manifest = tmp_path / "integrity.json"
    manifest.write_text(json.dumps({
        "schema_version": 1, "image_start": address, "region_length": len(expected),
        "sha256": hashlib.sha256(expected).hexdigest(),
    }), encoding="utf-8")
    mdw_data = bytearray(expected)
    if fault == "mdw_byte":
        mdw_data[6] = 0xA5
    app_dir, _, _, _, args_file = runner_fixture(
        tmp_path, mdw_line(address, mdw_data), dump_payload=bytes(actual)
    )

    result = run_verifier(powershell, image, app_dir, args_file, image_manifest=manifest)

    args = args_file.read_text(encoding="ascii")
    assert args.index('"halt"') < args.index('"dump_image ')
    dumps = list((tmp_path / "readback-artifacts").glob("nrf54l15-readback-*.bin"))
    if fault == "none":
        assert result.returncode == 0, result.stderr
        assert "rram tail check: PASS" in result.stdout
        assert dumps == []
    else:
        assert result.returncode == 1
        assert "rram tail check: readback retained:" in result.stderr
        assert len(dumps) == 1
        assert dumps[0].read_bytes() == bytes(actual)
        if fault in {"byte", "mdw_byte"}:
            assert "first_diff=0x1006" in result.stdout + result.stderr
            assert "expected=06 got=A5" in result.stdout + result.stderr
        else:
            assert "readback length mismatch" in result.stderr
