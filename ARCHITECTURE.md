# Architecture

This document describes the runtime ownership, real-time boundaries, component interfaces, and extension points of `esp32_geophone_source_locator`.

The design target is ESP32-S3 N16R8 on ESP-IDF v5.5.4. The firmware is split into ESP-IDF components rather than one application translation unit. Public headers live under each component's `include/` directory; multi-file component-private state and contracts use private headers inside the component directory.

## Runtime boot sequence

`main/main.c` delegates boot and shutdown to the application orchestration layer.

```text
app_main
  -> app_startup_run
       -> app_event_bus_init
       -> power_control_init
       -> device_status_init
       -> seismic_buffer_init
       -> adc_service_init
       -> keypad_init
       -> SPI2 init
       -> isolated SD startup check
       -> display hardware init
       -> LVGL boot UI
       -> storage_manager init
       -> object recovery
       -> object store
       -> object service
       -> storage service
       -> Wi-Fi manager
       -> HTTP server
       -> interactive LVGL UI
       -> keypad event monitor
       -> selected seismic source start
  -> wait for shutdown request
  -> app_shutdown_run
```

Each major boot stage is timed and logged so slow storage or hardware bring-up is visible in the serial log.

## Core ownership

The project deliberately separates service work from acquisition work.

### Core 0 - services

- LVGL handler task (`esp_lvgl_port`)
- UI event worker
- analog keypad monitor
- power-button monitor
- object service
- storage service
- Wi-Fi / HTTP-facing application work

### Core 1 - real-time side

- AD7606 acquisition
- ESP ADC bench acquisition source
- synthetic seismic source
- future preprocessing / arrival picking
- future localization worker if it becomes a continuous task

The dedicated AD7606 SPI3 bus is initialized by the Core 1 acquisition worker so SPI interrupt affinity remains on the acquisition side.

## Task policy

Application-owned task stack sizes, priorities and core assignments are centralized in `components/app_config/include/app_task_config.h`.

Current policy:

| Task | Core | Priority | Stack |
| --- | ---: | ---: | ---: |
| AD7606 acquisition | 1 | 7 | 4096 |
| ESP ADC bench | 1 | 6 | 4096 |
| Synthetic seismic source | 1 | 4 | 3072 |
| UI event worker | 0 | 6 | 6144 |
| LVGL handler | 0 | 4 | 7168 |
| Power button | 0 | 5 | 3072 |
| Keypad | 0 | 4 | 8192 |
| Storage service | 0 | 3 | 8192 |
| Object service | 0 | 3 | 8192 |

Do not introduce new application tasks with ad-hoc priorities. Add them to `app_task_config.h` so the whole scheduling hierarchy stays reviewable in one place.

## Hardware bus ownership

### SPI2 - display and SD

```text
GPIO11 MOSI
GPIO12 SCLK
GPIO13 MISO
GPIO14 TFT CS
GPIO15 TFT DC
GPIO16 TFT RESET
GPIO17 SD CS
```

The SD card stays at 2 MHz on the current prototype because higher rates were not stable on the physical assembly.

### SPI3 - AD7606 only

```text
GPIO39 DOUTA/MISO
GPIO40 RD/SCLK
GPIO41 CS
GPIO42 CONVST A+B
GPIO47 BUSY
GPIO2  RESET
```

SPI3 is read-only for this AD7606 connection; MOSI is unused.

## Seismic acquisition interface

`seismic_acquisition` is the orchestration layer. It does not contain source-specific start/stop/status logic.

Each backend implements one private `seismic_source_driver_t` descriptor:

```c
typedef struct {
    const char *name;
    const char *transport_name;
    uint32_t sample_rate_hz;
    uint8_t channel_count;
    int core_id;
    esp_err_t (*start)(void);
    void (*stop)(void);
    esp_err_t (*populate_snapshot)(seismic_acquisition_snapshot_t *snapshot);
} seismic_source_driver_t;
```

Current descriptors are:

- `seismic_source_test.c`
- `seismic_source_esp_adc.c`
- `seismic_source_ad7606.c`

`seismic_source_registry.c` performs the compile-time Kconfig selection. Runtime source switching is intentionally not exposed yet; changing the selected backend requires a firmware configuration/build change. Adding a backend should not require editing the acquisition orchestrator.

## AD7606 sample timing

The AD7606 provides simultaneous sampling for all eight channels.

The software sequence is:

```text
periodic acquisition wake
  -> CONVST rising edge
  -> timestamp captured immediately at the sampling event
  -> CONVST pulse completes
  -> BUSY completion
  -> SPI3 read V1..V8
  -> publish acquisition status
  -> push selected channels into seismic ring buffer
```

The frame timestamp therefore represents the real simultaneous conversion trigger, not the later completion of SPI readout.

The current periodic wake still uses `esp_timer` plus a task notification. That is suitable for the present bench stage. If later TDOA error budgeting shows that inter-sample scheduling jitter is material, the timing source must be upgraded to a hardware-timed CONVST path rather than merely changing where timestamps are read.

## Seismic ring buffer

`seismic_buffer` has one acquisition producer and potentially slow readers such as HTTP clients.

Readers must never hold a long mutex that blocks the producer while hundreds of samples are copied. The current implementation uses:

- a short metadata critical section;
- per-slot sequence publication;
- optimistic reader retries if a producer wraps over a slot being read.

This keeps waveform snapshot latency on the consumer side instead of injecting it into the acquisition path.

## LVGL UI architecture

The display component owns only the physical ILI9341 / `esp_lcd` layer. Widget layout, text, QR rendering, navigation and refresh logic belong to LVGL through `esp_lvgl_port`.

```text
ILI9341
  -> esp_lcd
  -> esp_lvgl_port
  -> LVGL 9
  -> ui component
```

Screen routing uses one `ui_screen_driver_t` registry instead of repeated enum/switch dispatch:

```c
typedef struct {
    const char *name;
    const char *home_label;
    void (*open)(void);
    void (*render)(void);
    void (*handle_key)(keypad_key_t key);
    void (*refresh)(void);
} ui_screen_driver_t;
```

The Home menu is generated from the same registry, so adding a screen cannot silently update rendering but forget input routing or the menu label.

Current screens:

- Home
- Objects
- Measurements
- Storage
- Wi-Fi / Web
- System

The Measurements screen consumes `seismic_acquisition_snapshot_t`. It never touches AD7606 GPIO or SPI functions directly.

## Storage ownership and shutdown

Storage calls are serialized through `storage_manager` / `storage_service`. SD lifecycle state belongs to `sd_card`.

Safe shutdown order:

```text
keypad monitor
  -> UI event task / LVGL
  -> storage service
  -> object service
  -> seismic acquisition
  -> HTTP server / Wi-Fi
  -> storage manager
  -> FAT SD unmount
  -> display disable/deinit
  -> power-off pulse
```

The SD handle is cleared only after a successful FAT unmount so software state does not claim the card is gone if unmount failed.

## TDOA localization boundary

A reusable 3D TDOA solver now lives in the independent `localization` component.

`localization_tdoa_solve()` accepts:

- 4..8 sensor positions in meters;
- one arrival timestamp per sensor for the same detected event;
- propagation speed in m/s;
- an optional initial source-position guess.

It solves the nonlinear TDOA equations with damped Gauss-Newton iterations and reports:

- estimated `(x, y, z)`;
- estimated emission time;
- RMS timing residual;
- iteration count;
- convergence status.

The solver rejects degenerate 3D sensor geometry. At least four non-coplanar sensor positions are required for a 3D solution.

This component deliberately does **not** decide where an event begins in raw geophone data. The future signal-processing pipeline must provide reliable arrival picks first:

```text
AD7606 frames
  -> preprocessing / filtering
  -> event detection
  -> per-channel arrival-time picking
  -> sensor positions / heights
  -> localization_tdoa_solve
  -> source estimate + residual/quality
```

Keeping arrival picking and geometry solving separate allows both stages to be bench-tested independently.

## Web component convention

The public web API is only `include/web_server.h`. Internal route/JSON/setup helpers are implementation details of the `web_server` component and should use component-private declarations, not grow additional public headers.

## Extension rules

When adding hardware or features:

1. Put hardware ownership in one component.
2. Expose a small public API; keep implementation state private.
3. Add new application task priorities/core assignments to `app_task_config.h`.
4. Do not let UI/web code call low-level ADC/SPI functions directly.
5. Keep Core 0 consumers from blocking Core 1 acquisition.
6. Add new UI screens through the screen registry.
7. Add new acquisition backends through `seismic_source_driver_t` and the source registry.
8. Keep raw arrival detection separate from the localization solver.
9. Preserve safe shutdown ordering for every new producer or storage writer.
