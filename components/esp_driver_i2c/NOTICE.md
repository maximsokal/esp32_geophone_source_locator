# Local ESP-IDF I2C override

This component is copied from Espressif ESP-IDF v5.5.4, `components/esp_driver_i2c`. The upstream source files retain their SPDX headers and Apache-2.0 licensing.

The local `.gitattributes` keeps the copied and patched text files LF under this checkout's `core.autocrlf=true` setting.

## Local patches

- `i2c_private.h:106` adds the full operation-array length to each transaction descriptor so the ISR can validate a read cursor while `cmd_count` is decremented during execution.
- `i2c_master.c:167-179,560-561,569-570,617-618,626-627,702-703,860-861,897-898,1080-1081,1206-1207,1303-1304,1419-1420,1459-1460` adds a spinlock-protected read-state reset for transaction restarts, synchronous errors/timeouts, asynchronous aborts, bus reset, and probe cleanup.
- `i2c_master.c:756-820` guards receive handling against error interrupts/status, a missing or out-of-range ops array cursor, a null RX destination, and an out-of-range optional trailing NACK operation.
- `i2c_master.c:831-861` prevents the default ISR from dispatching receive handling for NACK, timeout, or arbitration interrupts.
- `i2c_master.c:986-987,1026-1027,1064-1065,1434-1435` records the full op-array bound for synchronous, asynchronous, queued, and probe transactions.
- `i2c_master.c:1206-1207` initializes the atomic transaction cursor before reset helpers use it.

The patches address the HIL `StoreProhibited` crash with `EXCVADDR=0` in `i2c_isr_receive_handler`, caused by stale read-cursor state reaching an operation with a null data pointer after a failed or timed-out transfer.

Drop this project override when the selected ESP-IDF release contains an upstream fix for the same crash.
