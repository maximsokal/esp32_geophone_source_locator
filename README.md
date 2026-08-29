# ESP32 Geophone Source Locator

ESP32-S3 diploma prototype for seismic/acoustic search under rubble. The device acquires multiple geophone channels simultaneously, stores object/measurement data on microSD, exposes a local web interface, and is being prepared for later event detection and TDOA/source-localization processing.

The repository is an ESP-IDF project. Current development target is ESP-IDF **v5.5.4** on an ESP32-S3 N16R8 board.

## Current hardware architecture

### ESP32-S3

The controller is an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

Octal PSRAM uses GPIO33-GPIO37 internally, so those pins are intentionally not assigned to application peripherals.

### SPI2: UI and storage

SPI2 is shared only by the TFT and microSD card:

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| MOSI | 11 |
| SCLK | 12 |
| MISO | 13 |
| TFT CS | 14 |
| TFT DC | 15 |
| TFT RESET | 16 |
| microSD CS | 17 |

The microSD SPI clock is intentionally limited to **2 MHz** because higher rates were not stable on the current hardware assembly.

### SPI3: dedicated AD7606 acquisition bus

AD7606 is isolated from the display/storage bus and owns SPI3:

| AD7606 signal | ESP32-S3 GPIO |
| --- | ---: |
| DOUTA / DB7 / MISO | 39 |
| RD / SCLK | 40 |
| CS | 41 |
| CONVST A + CONVST B | 42 |
| BUSY | 47 |
| RST | 2 |

AD7606 serial mode is read-only, therefore SPI3 MOSI is not used.

A 10 kOhm pull-up is used on the AD7606 CS line.

Current AD7606 board configuration is:

- RANGE low: +/-5 V input range;
- OS0/OS1/OS2 low: no hardware oversampling;
- 8 simultaneous channels;
- SPI mode 2;
- 1 MHz SPI clock;
- default acquisition rate: 500 Hz.

The first four AD7606 channels currently feed the application seismic buffer. All eight channels are read on every conversion.

### Other current GPIO assignments

| Function | GPIO |
| --- | ---: |
| Analog keypad KS0466 | 8 |
| Pololu power-off control | 18 |
| External power-button sense | 21 |
| Temporary ESP ADC bench/divider input | 1 |

GPIO1 is reserved by the existing divider/bench wiring and must not be reused for AD7606.

## CPU/task policy

Application-owned work is split by role:

### Core 0 - services

- UI event handling;
- keypad polling;
- object service;
- storage service;
- power-button monitor;
- Wi-Fi/HTTP-facing application work.

### Core 1 - real-time acquisition

- AD7606 acquisition;
- ESP ADC bench acquisition source;
- synthetic seismic test source;
- future signal preprocessing, event detection and localization.

The AD7606 task initializes and owns SPI3 on Core 1 so the SPI3 interrupt affinity follows the real-time acquisition side.

## Seismic acquisition pipeline

For the AD7606 source:

```text
geophone
   -> low-noise analog amplifier/filter
   -> AD7606 simultaneous analog inputs
   -> CONVST
   -> BUSY
   -> SPI3 read of V1..V8
   -> timestamped seismic frame
   -> lock-minimized ring buffer
   -> web/UI/recording consumers
```

The frame timestamp is captured at the CONVST sampling event, not after the later SPI transfer.

The seismic ring buffer uses short metadata critical sections plus per-slot publication sequencing. Web snapshots do not hold a mutex while copying hundreds of waveform frames, so HTTP reads cannot block the acquisition producer for the duration of a large copy.

## Acquisition sources

The source is selected at build time in `menuconfig`:

- `AD7606 SPI acquisition` - normal hardware path;
- `ESP32-S3 ADC geophone bench input` - temporary direct ADC bench mode;
- `Synthetic test waveform` - software-only waveform source.

The selected backend is represented by one acquisition driver descriptor, which owns start/stop/name/sample-rate behavior and updates the shared device acquisition status.

## Storage and safe shutdown

Object and measurement state is stored under `/sdcard/geolocator`.

The shutdown sequence stops producers/services before cutting power:

```text
keypad monitor
-> UI event worker
-> storage service
-> object service
-> seismic acquisition
-> HTTP/Wi-Fi
-> storage manager
-> SD unmount
-> display off
-> power-off pulse
```

The SD card handle is cleared only after a successful FAT unmount. If unmount fails, the handle remains available so software state still reflects the mounted card.

## Web interface

The firmware starts a local HTTP server and exposes device/seismic data APIs, including:

- `/api/status`
- `/api/seismic`

`/api/seismic` can return a snapshot of recent waveform frames without holding the acquisition producer behind a long copy operation.

## Build

Install ESP-IDF v5.5.4 and open an ESP-IDF terminal in the repository directory.

```powershell
idf.py build
```

To select another seismic source:

```powershell
idf.py menuconfig
```

Then open **Seismic Acquisition** and select the required backend.

## Flash and monitor

Use the board programming port for flashing:

```powershell
idf.py -p COM4 flash
```

In the current bench setup an external USB-to-TTL adapter can be used as a passive UART monitor while the device runs from the 2x18650 power system. The external adapter must not power the ESP32 board.

Typical passive monitor wiring is:

```text
ESP32 GPIO43 TX -> USB-TTL RXD
ESP32 GND       -> USB-TTL GND
```

The USB-TTL TXD and power pins are not required for log-only monitoring.

Example:

```powershell
idf.py -p COM6 monitor
```

COM numbers are development-machine specific and may be different on another PC.

## Hardware bring-up checks

A normal AD7606 start should contain messages equivalent to:

```text
AD7606_DRIVER: GPIO configured: RESET=2 CONVST=42 BUSY=47 CS=41
AD7606_DRIVER: Initializing dedicated SPI3: MISO=39 SCLK=40
AD7606_DRIVER: SPI3 initialized successfully
AD7606_DRIVER: Self-test conversion + SPI read OK
AD7606_ACQ: Started: 500 Hz, 8 channels, SPI3 dedicated, core=1
```

For a known approximately 1.6 V divider connected to V8, a representative reading is around:

```text
raw=[...,10500]
mV=[...,1602]
```

Exact values depend on the measured supply/reference/input voltage and must not be treated as a calibration result.

## Project status

Implemented foundations include:

- TFT UI and keypad navigation;
- microSD object storage and recovery;
- safe power-down flow;
- Wi-Fi and local web server;
- simultaneous AD7606 acquisition;
- dedicated SPI3 acquisition bus;
- Core 0/Core 1 task separation;
- CONVST-time frame timestamps;
- non-blocking waveform snapshot architecture.

Still planned for later stages:

- final low-noise geophone analog front ends;
- sensor-pod barometer/IMU integration;
- event detection and signal preprocessing;
- deterministic higher-precision acquisition timing where required;
- TDOA/source-localization algorithm;
- calibration and quantified measurement accuracy.

This repository is a development/diploma prototype and is not a certified rescue instrument.
