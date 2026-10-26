# Local ESP-IDF I2C override

Copied from Espressif ESP-IDF v5.5.4, components/esp_driver_i2c. The upstream source SPDX headers are retained; the original component files are Apache-2.0 licensed.

## Local patches

- `s_i2c_send_commands` shares one tick budget across command-semaphore waits,
  the completion event and the final NACK/bus-busy wait. A long receive no
  longer gets a fresh timeout for every FIFO command. Arithmetic is exercised
  by the `i2c_timeout_budget` host target, including tick wrap and infinite waits.

- `s_i2c_send_commands` polls the command semaphore even when the tick budget is spent, so a task
  that was not scheduled for the whole timeout does not report a bus fault for a transfer that
  finished or never started (geolocator-i2c-3; board log 2026-10-26 20:02: `pending_int=0x0
  bus_busy=0 cmds_left>0` on 1-byte TCA9548A transfers).

- `s_i2c_send_commands` also logs `pending_int`, `bus_busy` and the commands left on a
  software timeout (geolocator-i2c-3): a pending interrupt means this side was too slow to
  service the controller, no pending interrupt means the target never let the transfer progress.

- i2c_master.c:167-174 (a) adds a task-context helper that clears contains_read and read_len_static under the bus spinlock.
- i2c_master.c:690-691,1049-1050,1271-1272,1385-1386,1432-1433 (b) clears stale read metadata at transaction start, synchronous error recovery, bus reset, and before and after the probe operation.
- i2c_master.c:759-762,783-786 (c) returns from receive handling if the selected operation has a null data pointer, in both receive branches.

This addresses the HIL StoreProhibited crash (EXCVADDR=0) after a timed-out read: stale contains_read state can dispatch i2c_isr_receive_handler for a later probe from another task, whose START/STOP operation has null data.

A broader first attempt was withdrawn because it broke normal reads.

Before dropping this override on an ESP-IDF upgrade, verify both the stale-read
metadata fix and the shared command-wait budget in that release.
