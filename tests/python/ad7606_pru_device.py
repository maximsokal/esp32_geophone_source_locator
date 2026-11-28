"""Virtual AD7606 DeviceModel for TI PRU Simulator (AM62x PRU1).

Models AD7606 Rev. G serial interface (Table 3, Figures 2, 3, 6, 46) at
VDRIVE = 3.3 V independently of the PRU firmware:
  Inputs observed from PRU1 R30:
    bit 2  (P2.06): CONVST A+B
    bit 3  (P2.08): RESET
    bit 11 (P2.32): RD/SCLK
    bit 12 (P2.30): CS_N
  Outputs driven to PRU1 R31:
    bit 4  (P2.10): BUSY
    bit 6  (P2.24): DOUTA (V1..V4)
    bit 7  (P2.33): DOUTB (V5..V8)
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Callable, Sequence

_SIM_ROOT = Path(__file__).resolve().parents[2] / ".tools" / "pru-simulator"
if str(_SIM_ROOT) not in sys.path:
    sys.path.insert(0, str(_SIM_ROOT))

from pru_io.device_model import PUSH_PULL, DeviceModel  # noqa: E402

PIN_CONVST = 2
PIN_RESET = 3
PIN_SCLK = 11
PIN_CS_N = 12

PIN_BUSY = 4
PIN_DOUTA = 6
PIN_DOUTB = 7


class Ad7606DeviceModel(DeviceModel):
    name = "ad7606"
    time_driven = True
    nets = {
        PIN_BUSY: PUSH_PULL,
        PIN_DOUTA: PUSH_PULL,
        PIN_DOUTB: PUSH_PULL,
    }

    def __init__(
        self,
        sample_generator: Callable[[int], Sequence[int]] | None = None,
        core_clock_hz: int = 200_000_000,
        t_conv_ns: int = 3600,
        t_reset_min_ns: int = 50,
        t6_guard_ns: int = 25,
        stuck_busy_high: bool = False,
        stuck_busy_low: bool = False,
    ) -> None:
        self.sample_generator = sample_generator or (lambda idx: (idx + 1, -(idx + 1), 100 + idx, -100 - idx, 200 + idx, -200 - idx, 0, 0))
        self.core_clock_hz = core_clock_hz
        self.t_conv_cycles = max(1, (t_conv_ns * core_clock_hz) // 1_000_000_000)
        self.t_reset_min_cycles = max(1, (t_reset_min_ns * core_clock_hz) // 1_000_000_000)
        self.t6_guard_cycles = max(1, (t6_guard_ns * core_clock_hz) // 1_000_000_000)
        self.stuck_busy_high = stuck_busy_high
        self.stuck_busy_low = stuck_busy_low
        self.reset()

    def reset(self) -> None:
        self.prev_convst = 1
        self.prev_reset = 0
        self.prev_cs_n = 1
        self.prev_sclk = 1

        self.busy = 1 if self.stuck_busy_high else 0
        self.converting = False
        self.conv_start_cycle = 0
        self.conv_end_cycle = 0
        self.conversion_count = 0
        self.reset_high_start: int | None = None
        self.reset_pulses = 0

        # Output registers hold previous conversion's 8x int16 values
        self.out_regs: list[int] = [0] * 8
        self.pending_regs: list[int] = [0] * 8
        self.shift_a = 0
        self.shift_b = 0
        self.sclk_edges_in_cs = 0
        self.cs_rise_cycle = -10000

        self.convst_cycles: list[int] = []
        self._events: list[dict] = []
        self._faults: list[str] = []

    def events(self) -> list[dict]:
        return list(self._events)

    def faults(self) -> list[str]:
        return list(self._faults)

    def _load_shift_registers(self) -> None:
        # DOUTA shifts V1..V4 (MSB first, 64 bits); DOUTB shifts V5..V8 (MSB first, 64 bits)
        a = 0
        b = 0
        for i in range(4):
            a = (a << 16) | (self.out_regs[i] & 0xFFFF)
            b = (b << 16) | (self.out_regs[4 + i] & 0xFFFF)
        self.shift_a = a
        self.shift_b = b
        self.sclk_edges_in_cs = 0

    def tick(self, cycle: int, bus: int) -> tuple[int, int]:
        convst = (bus >> PIN_CONVST) & 1
        rst = (bus >> PIN_RESET) & 1
        sclk = (bus >> PIN_SCLK) & 1
        cs_n = (bus >> PIN_CS_N) & 1

        # 1. RESET pulse tracking
        if rst == 1 and self.prev_reset == 0:
            self.reset_high_start = cycle
        elif rst == 0 and self.prev_reset == 1:
            if self.reset_high_start is not None:
                width = cycle - self.reset_high_start
                if width < self.t_reset_min_cycles:
                    self._faults.append(f"cycle {cycle}: RESET pulse too short ({width} < {self.t_reset_min_cycles})")
                else:
                    self.reset_pulses += 1
                    self.converting = False
                    self.busy = 1 if self.stuck_busy_high else 0
                    self.out_regs = [0] * 8
            self.reset_high_start = None

        # 2. Conversion completion -> update output registers on BUSY falling edge
        if self.converting and cycle >= self.conv_end_cycle:
            if cs_n == 0:
                self._faults.append(f"cycle {cycle}: CS_N still LOW at BUSY falling edge (data corruption)")
            elif (cycle - self.cs_rise_cycle) < self.t6_guard_cycles and self.cs_rise_cycle >= self.conv_start_cycle:
                self._faults.append(f"cycle {cycle}: t6 guard violated ({cycle - self.cs_rise_cycle} < {self.t6_guard_cycles} cycles)")
            self.out_regs = list(self.pending_regs)
            self.converting = False
            if not self.stuck_busy_high:
                self.busy = 0

        # 3. CONVST rising edge -> start conversion
        if convst == 1 and self.prev_convst == 0 and rst == 0:
            self.convst_cycles.append(cycle)
            raw8 = list(self.sample_generator(self.conversion_count))
            if len(raw8) != 8:
                raise ValueError("sample_generator must return 8 int16 values (V1..V8)")
            self.pending_regs = [((v + 0x8000) & 0xFFFF) - 0x8000 for v in raw8]
            self.conversion_count += 1
            self.converting = True
            self.conv_start_cycle = cycle
            self.conv_end_cycle = cycle + self.t_conv_cycles
            if not self.stuck_busy_low:
                self.busy = 1
            self._events.append({"cycle": cycle, "kind": "convst", "index": self.conversion_count - 1})

        # 4. CS_N & SCLK serial readout
        if cs_n == 0 and self.prev_cs_n == 1:
            self._load_shift_registers()
        elif cs_n == 1 and self.prev_cs_n == 0:
            self.cs_rise_cycle = cycle
            if self.sclk_edges_in_cs not in (0, 32, 64, 128):
                self._faults.append(f"cycle {cycle}: invalid SCLK count {self.sclk_edges_in_cs} in CS frame")
            self._events.append({"cycle": cycle, "kind": "cs_frame", "sclk_count": self.sclk_edges_in_cs})

        if cs_n == 0 and sclk == 1 and self.prev_sclk == 0:
            # SCLK rising edge: PRU samples current MSB, then shifts on next falling edge (or shifts after 1st rising edge)
            if self.sclk_edges_in_cs > 0:
                self.shift_a = (self.shift_a << 1) & ((1 << 64) - 1)
                self.shift_b = (self.shift_b << 1) & ((1 << 64) - 1)
            self.sclk_edges_in_cs += 1

        self.prev_convst = convst
        self.prev_reset = rst
        self.prev_cs_n = cs_n
        self.prev_sclk = sclk

        douta = (self.shift_a >> 63) & 1 if cs_n == 0 else 0
        doutb = (self.shift_b >> 63) & 1 if cs_n == 0 else 0

        mask = (1 << PIN_BUSY) | (1 << PIN_DOUTA) | (1 << PIN_DOUTB)
        vals = (self.busy << PIN_BUSY) | (douta << PIN_DOUTA) | (doutb << PIN_DOUTB)
        return mask, vals
