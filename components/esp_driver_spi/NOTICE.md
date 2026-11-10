# Local ESP-IDF SPI override

Copied from Espressif ESP-IDF v5.5.4, components/esp_driver_spi (without `test_apps`). The upstream source SPDX headers are retained; the original component files are Apache-2.0 licensed.

## Local patches

- `src/gpspi/spi_master.c`, `setup_priv_desc`: a failed setup frees the temporary DMA buffers it
  allocated and returns the error. Upstream called `uninstall_priv_desc()` on a descriptor whose
  `buffer_to_rcv` was still NULL, which copied the receive data back from address 0.

  Board log 2026-11-07 21:41: the internal DMA heap was down to `dma_largest=640`,
  `spi_master: setup_dma_priv_buffer(1208): Failed to allocate priv RX buffer` was followed by
  `LoadProhibited` with `EXCVADDR=0` in ROM `memcpy`, three boots in a row. SDSPI keeps its command
  buffers on the caller's stack; storage tasks have PSRAM stacks, so every SD command needs a
  temporary internal DMA buffer and an allocation failure there must be an ordinary
  `ESP_ERR_NO_MEM`, not a panic.

No other file differs from upstream.

Before dropping this override on an ESP-IDF upgrade, verify that `setup_priv_desc` in that release
no longer reaches `uninstall_priv_desc` with unassigned private buffers.
