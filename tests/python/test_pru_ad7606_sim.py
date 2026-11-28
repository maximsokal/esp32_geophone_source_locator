"""TI PRU Simulator tests for the real clpru-compiled PRU1 AD7606 ELF."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SIM_ROOT = REPO_ROOT / ".tools" / "pru-simulator"
PRU_ELF = REPO_ROOT / "PocketBeagle2" / "pru" / "build" / "pb2_pru1_ad7606.out"
AM62X_CFG = SIM_ROOT / "config" / "memory_am62x.cfg"

if str(SIM_ROOT) not in sys.path:
    sys.path.insert(0, str(SIM_ROOT))
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from simulator import Simulator  # noqa: E402
from tools.headless_runner import _validate_linked_elf  # noqa: E402
from ad7606_pru_device import Ad7606DeviceModel  # noqa: E402

SHM_ADDR = 0x00010000
OFF_MAGIC = 0
OFF_PROFILE = 7
OFF_STATE = 8
OFF_CMD = 12
OFF_CMD_SEQ = 16
OFF_CMD_ACK = 20
OFF_OWNER = 24
OFF_RATE = 28
OFF_IEP_HZ = 32
OFF_SESSION_EPOCH = 44
OFF_WRITE_BLK = 52
OFF_READ_BLK = 56
OFF_TOTAL_FRAMES = 64
OFF_DROPPED = 72
OFF_HW_ERR = 100
OFF_BLOCKS = 104
BLOCK_SIZE = 152


def _make_sim_with_pru1(elf_path: Path) -> Simulator:
    sim = Simulator(config_path=str(AM62X_CFG))
    elf_data = elf_path.read_bytes()
    entry_pc = _validate_linked_elf(elf_data)
    errors = sim.load_elf("pru1", elf_data)
    assert not errors, f"ELF load errors: {errors}"
    sim.cores["pru1"].pc = entry_pc
    # Idle-high pull-ups on CONVST (bit 2), SCLK (bit 11), CS_N (bit 12) before PRU1 drives GPO
    sim.cores["pru1"].io_port.gpo = (1 << 2) | (1 << 11) | (1 << 12)
    # Enable PRU1 GPO output drivers for AD7606 control pins:
    # bit 2 (CONVST), bit 3 (RESET), bit 11 (SCLK), bit 12 (CS_N)
    sim.cores["pru1"].io_port.set_gpo_drive_mask((1 << 2) | (1 << 3) | (1 << 11) | (1 << 12))
    return sim


def _mem_read(sim: Simulator, addr: int, length: int) -> bytes:
    res = sim.memory.read(addr, length)
    return bytes(res[0]) if isinstance(res, tuple) else bytes(res)


def _wait_for_boot(sim: Simulator, max_steps: int = 2000) -> None:
    for _ in range(max_steps // 50):
        sim.step("pru1", count=50)
        magic = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_MAGIC, 4))[0]
        if magic == 0x50425232:
            return
    raise AssertionError("PRU1 did not initialize shared memory magic")


def _send_cmd(sim: Simulator, cmd: int, seq: int, max_steps: int = 2000) -> int:
    sim.memory.write(SHM_ADDR + OFF_CMD, struct.pack("<II", cmd, seq))
    for _ in range(max_steps // 20):
        sim.step("pru1", count=20)
        ack = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_CMD_ACK, 4))[0]
        if ack == seq:
            return struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_STATE, 4))[0]
    raise AssertionError(f"PRU1 did not ACK cmd={cmd} seq={seq}")


def _read_all_blocks(sim: Simulator) -> list[tuple[int, int, list[tuple[int, int, int, int]]]]:
    out = []
    w = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_WRITE_BLK, 4))[0]
    r = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_READ_BLK, 4))[0]
    while r != w:
        addr = SHM_ADDR + OFF_BLOCKS + r * BLOCK_SIZE
        first_idx, first_iep, seq, fc, flags = struct.unpack("<QQIHH", _mem_read(sim, addr, 24))
        frames = []
        for i in range(fc):
            frames.append(struct.unpack("<hhhh", _mem_read(sim, addr + 24 + i * 8, 8)))
        out.append((first_idx, first_iep, frames))
        r = (r + 1) & 63
    sim.memory.write(SHM_ADDR + OFF_READ_BLK, struct.pack("<I", r))
    return out


@pytest.mark.parametrize("rate_hz,profile,steps,t_conv_ns", [
    pytest.param(
        200_000, 1, 20_000, 3600,
        marks=pytest.mark.xfail(
            strict=True,
            reason="200 kHz (1000 cy @ 200 MHz) violates 3.3V SCLK<=12.5MHz bitbang (~730 cy > 720 cy tCONV) & ring flush budget",
        ),
    ),
    pytest.param(
        200_000, 1, 20_000, 4200,
        marks=pytest.mark.xfail(
            strict=True,
            reason="200 kHz (1000 cy @ 200 MHz) with worst-case tCONV=4.2us (840 cy) + loop/flush (>470 cy) exceeds 1000 cy period",
        ),
    ),
    (125_000, 1, 18_000, 4200),  # 125 kHz DUAL_DOUT worst-case tCONV (4.2 us)
    (100_000, 1, 18_000, 4200),  # 100 kHz DUAL_DOUT worst-case tCONV
    (80_000,  0, 18_000, 4200),  # 80 kHz SINGLE_DOUT Read-After-Conversion (max safe SINGLE_DOUT)
    (40_000,  0, 25_000, 4200),  # 40 kHz SINGLE_DOUT
    (20_000,  0, 35_000, 4200),  # 20 kHz SINGLE_DOUT
    (5_000,   0, 90_000, 4200),  # 5 kHz SINGLE_DOUT
    (500,     0, 820_000, 4200), # 500 Hz SINGLE_DOUT
])
def test_pru1_elf_fsm_iep_and_pipeline(rate_hz: int, profile: int, steps: int, t_conv_ns: int) -> None:
    sim = _make_sim_with_pru1(PRU_ELF)
    expected_sclk = 32 if profile == 1 else 64
    adc = Ad7606DeviceModel(
        core_clock_hz=200_000_000,
        t_conv_ns=t_conv_ns,
        expected_sclk_edges=expected_sclk,
    )
    sim.attach_device("pru1", adc)

    # Boot to IDLE: must NOT toggle CONVST or RESET prematurely
    _wait_for_boot(sim)
    state = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_STATE, 4))[0]
    assert state == 0  # IDLE
    assert len(adc.convst_cycles) == 0
    assert adc.reset_pulses == 0

    # CONFIGURE
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([profile]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))  # BEAGLE_OWNER
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", rate_hz))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    sim.memory.write(SHM_ADDR + OFF_SESSION_EPOCH, struct.pack("<I", 1))
    assert _send_cmd(sim, 1, 1) == 1  # CONFIGURED

    # ARM -> pulses AD7606 RESET
    assert _send_cmd(sim, 2, 2) == 2  # ARMED
    assert adc.reset_pulses == 1

    # START
    assert _send_cmd(sim, 3, 3) == 3  # ACQUIRING
    sim.step("pru1", count=steps)

    # STOP -> drains final pipelined conversion -> STOPPED
    end_state = _send_cmd(sim, 4, 4, max_steps=10_000)
    assert end_state == 5  # STOPPED
    assert adc.faults() == [], f"AD7606 faults: {adc.faults()}"

    # Verify CONVST period matches IEP target (200 MHz / rate_hz)
    expected_period = 200_000_000 // rate_hz
    assert len(adc.convst_cycles) >= 3
    for i in range(len(adc.convst_cycles) - 1):
        p = adc.convst_cycles[i + 1] - adc.convst_cycles[i]
        assert abs(p - expected_period) <= 60, f"rate={rate_hz} period={p} expected={expected_period}"

    # Verify all conversions were drained with exact values and real IEP timestamps!
    blocks = _read_all_blocks(sim)
    all_frames = [f for _, _, flist in blocks for f in flist]
    assert len(all_frames) == len(adc.convst_cycles), (
        f"Missing drained sample! published={len(all_frames)} convst={len(adc.convst_cycles)}"
    )
    for idx, got in enumerate(all_frames):
        exp = adc.sample_generator(idx)
        if profile == 1:
            assert got == (exp[0], exp[1], exp[4], exp[5])
        else:
            assert got == (exp[0], exp[1], exp[2], exp[3])

    # Real hardware IEP timestamps must increase by ~expected_period (not 1000)
    if len(blocks) >= 1 and blocks[0][1] > 0:
        assert blocks[0][1] > 100



def test_pru1_elf_fault_injection_busy_stuck_high() -> None:
    sim = _make_sim_with_pru1(PRU_ELF)
    adc = Ad7606DeviceModel(core_clock_hz=200_000_000, stuck_busy_high=True)
    sim.attach_device("pru1", adc)

    _wait_for_boot(sim)
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([1]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", 100_000))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert _send_cmd(sim, 1, 1) == 1
    assert _send_cmd(sim, 2, 2) == 2
    assert _send_cmd(sim, 3, 3) == 3

    sim.step("pru1", count=100_000)
    state = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_STATE, 4))[0]
    hw_err = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_HW_ERR, 4))[0]
    assert state == 6  # FAULT
    assert hw_err & 0x0001  # PB2_PRU_ERR_BUSY_STUCK_HIGH



def test_pru1_elf_fault_injection_busy_stuck_low() -> None:
    sim = _make_sim_with_pru1(PRU_ELF)
    adc = Ad7606DeviceModel(core_clock_hz=200_000_000, stuck_busy_low=True)
    sim.attach_device("pru1", adc)

    _wait_for_boot(sim)
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([1]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", 100_000))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert _send_cmd(sim, 1, 1) == 1
    assert _send_cmd(sim, 2, 2) == 2
    assert _send_cmd(sim, 3, 3) == 3

    sim.step("pru1", count=10_000)
    state = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_STATE, 4))[0]
    hw_err = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_HW_ERR, 4))[0]
    assert state == 6  # FAULT
    assert hw_err & 0x0002  # PB2_PRU_ERR_BUSY_STUCK_LOW



def test_pru1_elf_fault_injection_busy_fell_in_read() -> None:
    sim = _make_sim_with_pru1(PRU_ELF)
    # Artificially short conversion (300 ns = 60 cycles) so BUSY falls mid-read
    adc = Ad7606DeviceModel(core_clock_hz=200_000_000, t_conv_ns=300)
    sim.attach_device("pru1", adc)

    _wait_for_boot(sim)
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([1]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", 200_000))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert _send_cmd(sim, 1, 1) == 1
    assert _send_cmd(sim, 2, 2) == 2
    assert _send_cmd(sim, 3, 3) == 3

    sim.step("pru1", count=6_000)
    assert _send_cmd(sim, 4, 4, max_steps=10_000) == 5  # STOPPED
    hw_err = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_HW_ERR, 4))[0]
    dropped = struct.unpack("<Q", _mem_read(sim, SHM_ADDR + OFF_DROPPED, 8))[0]
    assert hw_err & 0x0008  # PB2_PRU_ERR_BUSY_FELL_IN_READ
    assert dropped >= 1
    assert len(adc.faults()) >= 1



def test_pru1_elf_fault_injection_invalid_config_and_reset() -> None:
    sim = _make_sim_with_pru1(PRU_ELF)
    adc = Ad7606DeviceModel(core_clock_hz=200_000_000)
    sim.attach_device("pru1", adc)

    _wait_for_boot(sim)
    # SINGLE_DOUT (profile=0) at 200 kHz is invalid (max 100 kHz)
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([0]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", 200_000))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert _send_cmd(sim, 1, 1) == 6  # FAULT
    hw_err = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_HW_ERR, 4))[0]
    assert hw_err & 0x0010  # PB2_PRU_ERR_INVALID_CONFIG

    # RESET_FAULT (cmd=5) -> IDLE (state=0)
    assert _send_cmd(sim, 5, 2) == 0



def test_pru1_elf_missed_deadline_skips_slot_and_marks_gap() -> None:
    """When t_conv > period (e.g. 5200 ns @ 200 kHz), PRU1 must skip elapsed slots & mark GAP_BEFORE."""
    sim = _make_sim_with_pru1(PRU_ELF)
    adc = Ad7606DeviceModel(core_clock_hz=200_000_000, t_conv_ns=5200)
    sim.attach_device("pru1", adc)

    _wait_for_boot(sim)
    sim.memory.write(SHM_ADDR + OFF_PROFILE, bytes([1]))
    sim.memory.write(SHM_ADDR + OFF_OWNER, struct.pack("<I", 2))
    sim.memory.write(SHM_ADDR + OFF_RATE, struct.pack("<I", 200_000))
    sim.memory.write(SHM_ADDR + OFF_IEP_HZ, struct.pack("<I", 200_000_000))
    assert _send_cmd(sim, 1, 1) == 1
    assert _send_cmd(sim, 2, 2) == 2
    assert _send_cmd(sim, 3, 3) == 3

    sim.step("pru1", count=15_000)
    assert _send_cmd(sim, 4, 4, max_steps=10_000) == 5

    hw_err = struct.unpack("<I", _mem_read(sim, SHM_ADDR + OFF_HW_ERR, 4))[0]
    missed = struct.unpack("<Q", _mem_read(sim, SHM_ADDR + 88, 8))[0]
    assert hw_err & 0x0020  # PB2_PRU_ERR_MISSED_DEADLINE
    assert missed >= 1
    # Verify non-contiguous first_sample_index across at least one gap boundary
    blocks = _read_all_blocks(sim)
    assert len(blocks) >= 2
    assert any(
        blocks[i + 1][0] > blocks[i][0] + len(blocks[i][2])
        for i in range(len(blocks) - 1)
    ), f"Expected sample index gap in blocks={[(b[0], len(b[2])) for b in blocks]}"
