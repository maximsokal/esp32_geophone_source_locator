"""Live bidirectional PRU lifecycle test: pb2_node --mode acquire <-> TI PRU Simulator."""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PB2_DIR = REPO_ROOT / "PocketBeagle2"
PB2_NODE = PB2_DIR / "build" / "host-debug" / "pb2_node"
PRU_ELF = PB2_DIR / "pru" / "build" / "pb2_pru1_ad7606.out"


def _to_wsl_path(win_path: Path) -> str:
    s = str(win_path.resolve()).replace("\\", "/")
    if len(s) >= 2 and s[1] == ":":
        return f"/mnt/{s[0].lower()}{s[2:]}"
    return s


def test_live_pb2_node_drives_pru1_elf_lifecycle() -> None:
    """pb2_node --mode acquire drives CONFIGURE->ARM->START->ACQUIRE->STOP->DRAIN->FLUSH on real PRU1 ELF."""
    assert PRU_ELF.exists(), f"Missing PRU ELF: {PRU_ELF}"
    assert PB2_NODE.exists(), f"Missing pb2_node: {PB2_NODE}"
    assert shutil.which("wsl") is not None

    wsl_repo = _to_wsl_path(REPO_ROOT)
    driver_py = f"""
import mmap, os, struct, subprocess, sys, time
from pathlib import Path

repo = Path({wsl_repo!r})
sim_root = repo / ".tools" / "pru-simulator"
sys.path.insert(0, str(sim_root))
sys.path.insert(0, str(repo / "tests" / "python"))
from simulator import Simulator
from tools.headless_runner import _validate_linked_elf
from ad7606_pru_device import Ad7606DeviceModel

SHM_ADDR = 0x00010000
def mem_read(sim, addr, length):
    res = sim.memory.read(addr, length)
    return bytes(res[0]) if isinstance(res, tuple) else bytes(res)

sim = Simulator(config_path=str(sim_root / "config" / "memory_am62x.cfg"))
elf_data = (repo / "PocketBeagle2" / "pru" / "build" / "pb2_pru1_ad7606.out").read_bytes()
entry_pc = _validate_linked_elf(elf_data)
assert not sim.load_elf("pru1", elf_data)
sim.cores["pru1"].pc = entry_pc
sim.cores["pru1"].io_port.gpo = (1 << 2) | (1 << 11) | (1 << 12)
sim.cores["pru1"].io_port.set_gpo_drive_mask((1 << 2) | (1 << 3) | (1 << 11) | (1 << 12))
adc = Ad7606DeviceModel(core_clock_hz=200_000_000, t_conv_ns=4200)
sim.attach_device("pru1", adc)
for _ in range(40):
    sim.step("pru1", count=50)
    if struct.unpack("<I", mem_read(sim, SHM_ADDR, 4))[0] == 0x50425232:
        break
assert struct.unpack("<I", mem_read(sim, SHM_ADDR, 4))[0] == 0x50425232

shm_path = Path("/tmp/pb2_live_pru_shm.bin")
rec_path = Path("/tmp/pb2_live_rec.bin")
for p in (shm_path, rec_path):
    if p.exists():
        p.unlink()

shm_path.write_bytes(mem_read(sim, SHM_ADDR, 32768))
pb2_node = str(repo / "PocketBeagle2" / "build" / "host-debug" / "pb2_node")

proc = subprocess.Popen(
    [pb2_node, "--mode", "acquire", "--pru-live-file", str(shm_path), "--rate", "100000", "--frames", "16", "--out", str(rec_path)],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

try:
    last_cmd_seq = 0
    last_w = 0
    deadline = time.monotonic() + 45.0
    with shm_path.open("r+b") as f:
        mm = mmap.mmap(f.fileno(), 32768)
        while proc.poll() is None and time.monotonic() < deadline:
            cmd, cmd_seq = struct.unpack("<II", mm[12:20])
            r_idx = struct.unpack("<I", mm[56:60])[0]
            if cmd_seq != last_cmd_seq:
                sim.memory.write(SHM_ADDR + 7, bytes(mm[7:8]))
                sim.memory.write(SHM_ADDR + 24, bytes(mm[24:52]))
                sim.memory.write(SHM_ADDR + 12, bytes(mm[12:20]))
                last_cmd_seq = cmd_seq
            sim.memory.write(SHM_ADDR + 56, struct.pack("<I", r_idx))

            sim.step("pru1", count=5000)

            sim_bytes = mem_read(sim, SHM_ADDR, 32768)
            w_idx = struct.unpack("<I", sim_bytes[52:56])[0]
            if w_idx != last_w:
                mm[104:32768] = sim_bytes[104:32768]
                last_w = w_idx
            mm[52:56] = sim_bytes[52:56]
            mm[64:104] = sim_bytes[64:104]
            mm[8:12] = sim_bytes[8:12]
            mm[20:24] = sim_bytes[20:24]
            time.sleep(0.002)
        mm.close()

    stdout, stderr = proc.communicate(timeout=5)
    assert proc.returncode == 0, f"rc={{proc.returncode}} stdout={{stdout}} stderr={{stderr}}"
    assert "mode=acquire" in stdout, stdout
    assert adc.reset_pulses >= 1
    assert adc.faults() == [], adc.faults()
    assert rec_path.exists() and rec_path.read_bytes()[:8] == b"GEOSEIS1"
    print("LIVE_LIFECYCLE_OK:", stdout.strip())
finally:
    if proc.poll() is None:
        proc.kill()
    for p in (shm_path, rec_path):
        if p.exists():
            p.unlink()
"""
    proc = subprocess.run(
        ["wsl", "python3", "-c", driver_py],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, f"stdout={proc.stdout}\nstderr={proc.stderr}"
    assert "LIVE_LIFECYCLE_OK" in proc.stdout
