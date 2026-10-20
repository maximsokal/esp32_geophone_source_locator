# Local ESP-IDF I2C override

Copied from Espressif ESP-IDF v5.5.4, components/esp_driver_i2c. The upstream source SPDX headers are retained; the original component files are Apache-2.0 licensed.

## Local patches

- i2c_master.c:167-174 (a) adds a task-context helper that clears contains_read and read_len_static under the bus spinlock.
- i2c_master.c:690-691,1049-1050,1271-1272,1385-1386,1432-1433 (b) clears stale read metadata at transaction start, synchronous error recovery, bus reset, and before and after the probe operation.
- i2c_master.c:759-762,783-786 (c) returns from receive handling if the selected operation has a null data pointer, in both receive branches.

This addresses the HIL StoreProhibited crash (EXCVADDR=0) after a timed-out read: stale contains_read state can dispatch i2c_isr_receive_handler for a later probe from another task, whose START/STOP operation has null data.

A broader first attempt was withdrawn because it broke normal reads.

Drop this project override when moving to an ESP-IDF release that fixes this issue.
