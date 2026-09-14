# nRF54L15 subproject

This is an independent PlatformIO + Zephyr project for all Seeed Studio XIAO
nRF54L15 nodes used by `esp32_geophone_source_locator`. The repository root
remains the ESP-IDF project and is not a PlatformIO project.

## Architecture rule

There is one source tree, one PlatformIO environment and one firmware binary
for every XIAO nRF54L15. Runtime identity, main/remote topology and
initiator/reflector role are not compile-time variants. Do not create separate
`central` and `remote` environments or firmware copies.

The planned topology has one main-enclosure node on the ESP32-S3 upstream I2C
bus and one remote node per connected geophone behind the TCA9548A. With all
downstream channels disabled, the main node remains visible; selecting a mux
channel exposes that remote node. ESP32-S3 remains the wired-I2C controller.
The possible identical-I2C-address problem must be resolved before an I2C/TWIS
protocol is implemented. A remote BME280 has independent 3.3 V power and is
not powered or assumed to be mastered by this nRF node.

See the authoritative topology, Sense IMU, Channel Sounding and LED contract
in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). The dedicated future work and
test matrix are in [`../docs/nrf54l15-test-task.md`](../docs/nrf54l15-test-task.md).

Remote external LED wiring is:

| Pin | LED |
| --- | --- |
| `P1.04` | BLUE |
| `P1.05` | GREEN |
| `P1.06` | YELLOW |
| `P1.07` | RED |

The main-enclosure board has none of these four external LEDs. The current
`src/main.c` is only a temporary board/toolchain blink through the onboard
`led0` alias; it is not the future logger/orientation implementation.

## Supported Windows build interface

The checkout path contains spaces, and direct Seeed Zephyr generation through
that path is unreliable. From the repository root, use the wrapper:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
.\nRF54L15\pio.ps1 test
```

The wrapper uses a temporary no-space `SUBST` drive and keeps generated
PlatformIO/Zephyr files under `%USERPROFILE%\.platformio-workspaces\nrf54l15`.
Do not use a direct `pio run -d ".\nRF54L15"` command as the normal
workflow. The `test` action is also the supported PlatformIO test entry point;
it preserves the no-space mapping and passes `--without-uploading`, so it does
not flash a test image implicitly. When the future test suites have not yet
been added under `test/test_*`, it reports an explicit `SKIPPED` result.

In VS Code, run `nRF54L15: Flash firmware` to build and upload the same image
through `pio.ps1 upload`. This task requires a connected XIAO nRF54L15 and is
separate from the no-flash test tasks.
