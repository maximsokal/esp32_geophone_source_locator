# GeoLocator Web UI

Browser-only React + TypeScript + Vite client for the diploma 3D scanning and sensor-localization workflow.

The production phone client runs from trusted local HTTPS served by the ESP32. No Android/Capacitor APK and no cloud processing are required.

## Build

```powershell
cd webui
npm install
npm run typecheck
npm run build
```

Production output:

```text
webui/dist/
webui/esp-dist/
  index.html
  build-id.txt
  assets/app.js.gz
  assets/app.css.gz
```

The generated build folders and `components/web_server/generated_webui/` are ignored by Git.

## Runtime serving

Normal STA mode is HTTPS-only:

```text
https://geolocator.local/
```

The React bundle is served from firmware flash. The SD card keeps the boot-time seed/backup and object/session data; the HTTPS server does not read React assets from SD while the TFT and SD share SPI2.

Certificate onboarding and the secure browser gate are hardware-verified on Android Chrome.

## Coordinate contract

GeoLocator project coordinates:

```text
X = local horizontal
Y = local horizontal
Z = height
units = metres
```

WebXR coordinates are converted as:

```text
project X = XR X
project Y = -XR Z
project Z = XR Y
```

The phone scan constrains visible external geometry only. Unseen rubble interior remains unknown, not empty.

## WebXR depth input

The immersive session requests:

```text
immersive-ar
local-floor (local fallback)
dom-overlay
depth-sensing
light-estimation
CPU-optimized depth
matchDepthView = true
raw depth preferred, smooth fallback
```

The tested phone has previously supplied:

```text
depth usage  = cpu-optimized
depth format = luminance-alpha
depth buffer = 160 × 90
```

The current runtime also exposes the selected WebXR `depthType` so the phone test can verify whether Chromium actually grants `raw`, `smooth`, or leaves the type unspecified.

## Robust multi-view surface fusion

The old live guide counted repeated sampled frames. That was intentionally removed because a stationary phone could repeatedly observe the same noisy region and manufacture false confidence.

The current pipeline separates **depth frames** from **independent map evidence**:

```text
WebXR / ARCore 6DoF pose
        ↓
motion-quality estimate
        ↓
keyframe selector
        ├─ novel slow pose     → EVIDENCE KEYFRAME
        ├─ redundant/still     → low-weight POLISH only
        └─ excessive motion    → SKIP
        ↓
ARCore/WebXR depth
        ↓
local depth-neighbour residual gate
        ↓
metric project XYZ
        ↓
40 mm surface hypotheses
        ↓
robust capped-weight fusion
        ↓
independent-view confirmation
        ↓
PROMOTED stable surface
        ↓
300 mm coverage quality
        ↓
local live coverage guide + final point cloud
```

### Keyframe policy

A new depth frame is allowed to add coverage evidence only when the WebXR pose is geometrically novel. Current starting policy:

```text
minimum keyframe interval        260 ms
translation novelty             ~55 mm
or translation + view change    ~25 mm + 10°
```

Pose-derived linear/angular velocity and acceleration reduce frame quality. Fast/shaky frames are skipped instead of being allowed to create high-confidence geometry.

The WebXR pose is used rather than a separately clocked `DeviceMotionEvent` stream because the XR pose already represents the platform's camera/IMU tracking result and is synchronized to the XR frame loop.

### Stationary-frame rule

A stationary/redundant frame:

```text
CAN:
  refine the centroid/variance of an already matched surface at low weight

CANNOT:
  create a new voxel
  increment independent-view coverage
  turn an area "ready" by waiting in place
```

This is the critical anti-saturation invariant.

### Surface hypothesis lifecycle

Each retained representative tracks:

```text
bounded fusion weight
independent keyframe views
spatial residual / variance
last evidence keyframe
view-direction diversity
promotion state
```

Fusion weight is capped so an old surface never becomes immutable. Even a mature voxel keeps a finite update gain and can self-polish when later consistent observations arrive.

A new hypothesis is tentative. It must be confirmed by independent keyframes with a sufficiently small residual before being promoted. Tentative hypotheses that remain unsupported for later keyframes are pruned.

Depth residual gates increase with range because depth uncertainty grows with distance, but are capped so unrelated surfaces are not merged.

### Bounded adaptive memory

The phone still uses bounded spatial memory:

```text
fine surface voxel             40 mm
coverage cell                 300 mm
global representative budget  60,000
max representatives/cell      28 before adaptive rebalance
```

As new coverage cells appear, dense earlier regions can be compacted so new geometry is not blocked by scan order.

## Live coverage guide

The live guide now visualizes **multi-view surface confidence**, not elapsed time or raw frame count.

A coverage cell becomes visible only after stable promoted surface exists from independent views. Readiness combines:

```text
promoted surface density
independent keyframes
camera baseline
view-angle diversity
surface residual quality
```

Standing still therefore keeps the HUD at `MOVE PHONE` / `NEW ANGLE`; it may polish an already known surface but does not make the region greener simply because time passed.

The guide uses a separate bounded render budget and refresh cadence so reconstruction does not rebuild a huge GPU buffer every XR frame.

## Screen-off / interruption lifecycle

While scanning, the page requests a **Screen Wake Lock** so Android should not turn the display off due to normal timeout.

If the document/XR session is temporarily hidden:

```text
scan state = PAUSED
no depth integration
same XR session may continue when visible again
wake lock is reacquired when possible
```

If the browser/OS actually ends the XR session, WebXR cannot restore that same session. GeoLocator now reports `SCAN INTERRUPTED` and does **not** falsely mark the partial map as READY.

A new XR session is not automatically fused with the partial map because its local reference frame is not guaranteed to match. Safe cross-session resume requires an explicit relocalization step (future ICP / tag-based alignment) before fusion.

## Lighting and camera policy

The runtime requests WebXR Lighting Estimation when supported and records primary-light magnitude as diagnostic telemetry. It is not treated as calibrated lux and is not currently a hard depth-confidence gate.

The project deliberately does **not** use a strobe or start parallel physical cameras during immersive WebXR:

- WebXR does not provide a portable frame-synchronized torch/strobe control for the AR camera;
- a second `getUserMedia()` camera can contend with the camera owned by the immersive AR session;
- arbitrary phone cameras are not exposed as a guaranteed synchronized/calibrated multi-camera rig;
- ARCore itself can use supported hardware depth/stereo/ToF sensors when available.

WebXR Raw Camera Access can be evaluated later for low-rate image-quality metrics (brightness/texture/blur) if the tested Chrome exposes `camera-access`; it should not be made mandatory unless hardware testing shows a measurable reconstruction benefit.

## Operator-facing scan UI

The main scan UI is intentionally small. During immersive scanning it shows only information that changes operator behavior:

```text
scan state
stable surface count
independent/new views
ready / seen coverage
motion instruction
Finish scan
```

Internal counters and diagnostics live under `Technical details` instead of occupying the scan page.

A completed scan emphasizes:

```text
stable surface points
ready coverage cells
independent keyframes
surface residual
X / Y / Z extent
```

## Downstream contract

```text
stable metric visible-surface map
        ↓
surface / probabilistic occupancy representation
        ↓
TAG0 MAIN + TAG1…TAG4 GEO1…GEO4 observations
        ↓
robust tag poses in common frame
        ↓
tag → geophone-contact offsets
        ↓
locked sensor XYZ
        ↓
BMP relative height + IMU + future ranging fusion
```

## Stage status

1. `WEB-HTTPS-A` — HTTPS transport hardening. **COMPLETE / HARDWARE VERIFIED**
2. `WEB-HTTPS-ONBOARDING` — Root CA + secure browser gate. **COMPLETE / HARDWARE VERIFIED**
3. `WEB-HTTPS-B` — React gzip/flash serving + SD seed. **COMPLETE / HARDWARE VERIFIED**
4. `WEB-XR-A` — immersive AR + phone pose. **COMPLETE / HARDWARE VERIFIED**
5. `WEB-XR-B` — WebXR/ARCore CPU depth acquisition. **COMPLETE / HARDWARE VERIFIED**
6. `WEB-XR-C v1` — adaptive cloud / old temporal guide. **HARDWARE TESTED; TEMPORAL CONFIDENCE MODEL REJECTED**
7. `WEB-XR-C v2` — motion-qualified keyframes + robust self-polishing voxel/surfel fusion + multi-view coverage + wake-lock lifecycle. **IMPLEMENTED; TYPECHECK/PHONE RETEST REQUIRED**
8. `WEB-XR-D` — surface / occupancy reconstruction. **BLOCKED UNTIL v2 ACCEPTANCE**
9. `WEB-TAG-A` — AprilTag MAIN + GEO1…GEO4.
10. ESP storage/API contract for saved geometry and sensor positions.
11. Fusion with barometer, IMU and future ranging.
