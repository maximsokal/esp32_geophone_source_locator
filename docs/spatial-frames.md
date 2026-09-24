# Spatial frames (MAX-151)

The system has four coordinate frames. Each has a fixed convention. The transforms between them are either measured or recorded as unknown. They are never assumed.

| Frame | Origin | Axes | Units | Defined by |
|---|---|---|---|---|
| **GPS anchor** (WGS84) | the phone's position when the object was saved | latitude / longitude / altitude | degrees, m | `object.location` (source, accuracy, timestamp) |
| **Sensor frame** | MAIN node | +X points from MAIN to the first REMOTE the geometry solver places (the best-conditioned baseline). Y completes a plane whose mirror side is **arbitrary**. +Z is up (relative barometric altitude). | m (mm in firmware) | `nrf_geometry_solver`, frozen per recording in `session.json` |
| **Scan frame** | WebXR reference-space origin of the scanning session | WebXR convention: +Y up (gravity-aligned), X/Z horizontal. The heading is **arbitrary per scan session**. | m | the scan artifact of that recording |
| **Localization / report frame** | identical to the sensor frame | identical to the sensor frame | m | the persisted localization runs of the recording |

## What is and is not known

* **Heading (true north) is unknown in every frame.** A single GPS fix only gives a translation on Earth; it cannot give an orientation. Channel Sounding measures distance only, so the sensor layout has an arbitrary rotation. The firmware and the WebUI never infer heading from GPS. Reports and the UI say "heading unknown".
* **The sensor-frame mirror is arbitrary.** The layout and its mirror image fit the same distances. Only a later absolute reference, such as AprilTag/ArUco anchors or a calibrated compass, can resolve it.
* **Scan ↔ sensor frame: no transform yet.** It needs the AprilTag/ArUco registration (MAX-145). Until then, localization results are never drawn on the scan (MAX-161, MAX-150).
* **GPS ↔ sensor frame: translation only.** MAIN sits at the GPS anchor within the stated GPS accuracy. There is no rotation.

* **BLE RSSI node hints (MAX-146) live in the scan frame of one XR session.** They are reset when a session starts, never persisted as geometry, and never better than ±1.5 m. They are logged only as diagnostics (`XR_BLE_RSSI_DIAGNOSTICS`).

## Reproducibility

Everything a view or report draws comes from values frozen with the recording: the per-session geometry, the localization runs and the scan artifact. No step applies a live heading, a live sensor position or another recording's data. Reloading or reconnecting therefore reproduces the same sensor and source coordinates and the same top and side projections.

## Future transforms

When a transform is measured (AprilTag registration, a calibrated magnetometer), it is recorded as `{from, to, source, timestamp, rotation, translation, uncertainty}` (see `webui/src/domain/spatialFrames.ts`). A phone compass near rubble or metal is separate evidence that needs its own calibration. It is never silently trusted.
