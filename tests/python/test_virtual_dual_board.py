"""V3 PRU1 Simulator -> PB2 RuntimePipeline -> GEOSEIS1 v2 compatibility regression."""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PB2_DIR = REPO_ROOT / "PocketBeagle2"
PB2_NODE = PB2_DIR / "build" / "host-debug" / "pb2_node"
PRU_ELF = PB2_DIR / "pru" / "build" / "pb2_pru1_ad7606.out"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_pru_ad7606_sim as pru_sim  # noqa: E402


def _to_wsl_path(win_path: Path) -> str:
    s = str(win_path.resolve()).replace("\\", "/")
    if len(s) >= 2 and s[1] == ":":
        return f"/mnt/{s[0].lower()}{s[2:]}"
    return s


def test_scenario_b_pru1_sim_dump_to_pb2_pipeline_geoseis_v2(tmp_path: Path) -> None:
    """Virtual AD7606 -> real PRU1 ELF in TI pru-simulator -> PBR2 dump -> pb2_node -> GEOSEIS1 v2."""
    assert PRU_ELF.exists(), f"Missing required PRU1 ELF: {PRU_ELF}"
    assert PB2_NODE.exists(), f"Missing required pb2_node (host-debug preset): {PB2_NODE}"
    assert shutil.which("wsl") is not None, "WSL2 is required for pb2_node execution"

    sim = pru_sim._make_sim_with_pru1(PRU_ELF)
    adc = pru_sim.Ad7606DeviceModel(core_clock_hz=200_000_000, t_conv_ns=4200)
    sim.attach_device("pru1", adc)

    pru_sim._wait_for_boot(sim)
    sim.memory.write(pru_sim.SHM_ADDR + pru_sim.OFF_PROFILE, bytes([1]))  # DUAL_DOUT
    sim.memory.write(pru_sim.SHM_ADDR + pru_sim.OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(pru_sim.SHM_ADDR + pru_sim.OFF_RATE, struct.pack("<I", 100_000))
    sim.memory.write(pru_sim.SHM_ADDR + pru_sim.OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert pru_sim._send_cmd(sim, 1, 1) == 1  # CONFIGURED
    assert pru_sim._send_cmd(sim, 2, 2) == 2  # ARMED
    assert pru_sim._send_cmd(sim, 3, 3) == 3  # ACQUIRING

    sim.step("pru1", count=40_000)
    assert pru_sim._send_cmd(sim, 4, 4, max_steps=10_000) == 5  # STOPPED
    assert adc.faults() == []

    # Dump 32 KB ICSS_SHARED from simulator (PBR2 ring)
    shm_bytes = pru_sim._mem_read(sim, pru_sim.SHM_ADDR, 32768)
    dump_file = tmp_path / "pru_shm_dump.bin"
    dump_file.write_bytes(shm_bytes)

    rec_bin = tmp_path / "geoseis_out.bin"
    wsl_node = _to_wsl_path(PB2_NODE)
    wsl_dump = _to_wsl_path(dump_file)
    wsl_out = _to_wsl_path(rec_bin)

    proc = subprocess.run(
        ["wsl", "bash", "-lc", f"'{wsl_node}' --mode record --pru-file '{wsl_dump}' --rate 100000 --frames 0 --out '{wsl_out}'"],
        capture_output=True,
        text=True,
        check=True,
    )
    assert "mode=record" in proc.stdout
    assert rec_bin.exists(), f"Missing {rec_bin}; stdout={proc.stdout}"
    data = rec_bin.read_bytes()

    # Verify GEOSEIS1 v2 header (24 bytes base + 44 bytes v2 metadata = 68 bytes)
    magic, version, ch_count, sample_rate_hz, frame_bytes, _reserved = struct.unpack("<8sHHIII", data[:24])
    assert magic == b"GEOSEIS1"
    assert version == 2
    assert ch_count == 4
    assert sample_rate_hz == 100_000
    assert frame_bytes == 8

    # Parse GEOSEIS1 v2 blocks starting at offset 68 (20-byte block header + fc * 8-byte int16[4])
    off = 68
    decoded_frames: list[tuple[int, int, int, int]] = []
    while off + 20 <= len(data):
        first_idx, first_ts, fc, bflags = struct.unpack("<QQHH", data[off : off + 20])
        off += 20
        for _ in range(fc):
            decoded_frames.append(struct.unpack("<hhhh", data[off : off + 8]))
            off += 8

    assert len(decoded_frames) == len(adc.convst_cycles)
    for idx, got in enumerate(decoded_frames):
        exp = adc.sample_generator(idx)
        assert got == (exp[0], exp[1], exp[4], exp[5])
