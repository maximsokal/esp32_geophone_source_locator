# ESP-IDF 5.5.4 PSRAM/DMA audit

This note records the source-level decision for the ESP32-S3 N16R8 memory
refactor. The inspected implementation is the installed
`C:/Espressif/frameworks/esp-idf-v5.5.4` tree, not a generic SPI example.

## Generic SPI master

The relevant implementation is:

```text
components/esp_driver_spi/src/gpspi/spi_master.c
  setup_dma_priv_buffer()
```

For an external pointer, the driver checks both the transaction flag and the
external-DMA capability:

```c
bool use_psram = is_ptr_ext && (flags & SPI_TRANS_DMA_USE_PSRAM);
bool need_malloc = is_ptr_ext
    ? (!use_psram || !esp_ptr_dma_ext_capable(buffer))
    : !esp_ptr_dma_capable(buffer);
uint32_t mem_cap = MALLOC_CAP_DMA |
    (use_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
```

The same function applies separate external/internal alignment and cache-line
requirements. Therefore `SPI_TRANS_DMA_USE_PSRAM` is a real generic SPI-master
mechanism, but it is only safe when the peripheral, SoC, buffer capability and
alignment all support it.

## SDSPI path

The relevant implementation is:

```text
components/esp_driver_sdspi/src/sdspi_host.c
  get_block_buf()
  start_command_read_blocks()
  start_command_write_blocks()
```

`get_block_buf()` allocates `SDSPI_BLOCK_BUF_SIZE` with
`heap_caps_malloc(..., MALLOC_CAP_DMA)`. The read path always receives a block
into that buffer and then copies it to the FatFS destination:

```c
rx_data = get_block_buf(...);
spi_device_transmit(... rx_buffer = rx_data ...);
memcpy(data + extra_data_size, rx_data, will_receive);
```

The write path explicitly tests `!esp_ptr_in_dram(tx_data)`. External data is
copied into the same internal DMA block before `spi_device_transmit()`:

```c
if (!esp_ptr_in_dram(tx_data)) {
    get_block_buf(...);
    memcpy(tmp, tx_data, will_send);
    tx_data = tmp;
}
```

The FatFS bridge in
`components/fatfs/diskio/diskio_sdmmc.c` calls
`sdmmc_read_sectors()`/`sdmmc_write_sectors()`. That bridge does not add a
direct PSRAM-DMA guarantee to the SDSPI implementation above.

Conclusion: the current SDSPI/FatFS path must retain its internal DMA bounce
buffer. Enabling `SPI_TRANS_DMA_USE_PSRAM` at an application call site would
not remove this SDSPI-specific copy path and would not be a justified memory
fix.

## Application decisions

| Path | Decision | Reason |
|---|---|---|
| TFT / LVGL | Keep internal DMA buffer, 2560 bytes | Existing `esp_lcd`/LVGL path is stable; the buffer is small and direct PSRAM DMA was not proven for this exact stack. |
| SDSPI / FatFS | Keep IDF internal DMA bounce buffer | `sdspi_host.c` allocates and uses `block_buf` for external application buffers. |
| AD7606 | Keep 16-byte RX transaction internal | The buffer is too small for meaningful RAM savings; acquisition remains realtime-sensitive on Core 1. |
| DMA descriptors | Keep internal | Descriptor placement is a driver/SoC requirement and is not interchangeable with payload placement. |
| Wi-Fi hardware buffers | Do not relocate | No application-level evidence authorizes moving critical driver DMA buffers to PSRAM. |

No direct PSRAM-DMA change is enabled in production by this refactor. Any
future experiment must be a separate benchmark with TFT integrity, SD latency,
recorder drops, CPU load and AD7606 jitter measured together.
