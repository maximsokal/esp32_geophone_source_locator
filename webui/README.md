# GeoLocator Web UI

React + TypeScript + Vite workspace for the phone/desktop 3D interface.

This workspace provides the browser-only secure 3D scanner, measured point-cloud review, and the coordinate contracts used by the diploma localization pipeline.

## Stack

- React 19
- TypeScript
- Vite 8
- WebXR / ARCore Depth on supported Android Chrome devices
- WebGL point-cloud review

All runtime dependencies are bundled into local static files. The deployed UI does not use CDN imports and does not require internet access.

## Develop

```powershell
cd webui
npm install
npm run dev
```

## Production build

```powershell
cd webui
npm run build
```

The build produces:

```text
webui/dist/
    normal Vite production output

webui/esp-dist/
    index.html
    build-id.txt
    assets/app.js.gz
    assets/app.css.gz
```

`esp-dist` is prepared automatically after the normal Vite build. Hashed Vite asset names are rewritten to stable ESP routes and JavaScript/CSS are pre-compressed with gzip.

The generated `dist/`, `esp-dist/`, and `components/web_server/generated_webui/` artifacts are intentionally ignored by Git.

## WEB-HTTPS-B deployment

The ESP-IDF component checks for `webui/esp-dist` during CMake configure. When a bundle exists before `idf.py reconfigure/build`, a compressed seed is embedded into firmware. During boot, while SD is mounted and before the TFT begins using shared SPI2, the firmware can update the SD backup copy by build-id.

Runtime serving priority is intentionally:

```text
1. embedded React bundle from firmware flash
2. legacy tiny embedded dashboard if no React bundle was built
```

The HTTPS runtime does not read React assets from SD. This prevents HTTP/FATFS/SDSPI traffic from racing the asynchronous TFT driver on shared SPI2. The SD copy remains a boot-time seed/backup artifact and the card remains the persistent store for object/session data.

The browser requests:

```text
https://geolocator.local/
https://geolocator.local/assets/app.js
https://geolocator.local/assets/app.css
```

JavaScript/CSS are served with `Content-Encoding: gzip`.

## Coordinate contract

Application data uses the diploma coordinate convention:

```text
X = local horizontal axis
Y = local horizontal axis
Z = height
MAIN = preferred local frame origin
units = metres
```

WebXR uses X-right / Y-up / -Z-forward. Measured depth points are converted into project coordinates as:

```text
project X = XR X
project Y = -XR Z
project Z = XR Y
```

The external scan constrains visible outer geometry. Unknown internal rubble volume must not be classified as empty just because the phone cannot see it.

## Browser-only secure runtime

The production phone client is the browser application itself. No Android/Capacitor APK is part of the architecture.

GeoLocator uses two local transports:

```text
HTTP :80
  bootstrap / Wi-Fi setup / certificate onboarding

HTTPS :443
  https://geolocator.local/
  React UI
  dashboard APIs
  camera / WebXR / depth capture
```

Browser camera APIs and WebXR run only from the trusted `https://geolocator.local/` secure context.

The ESP32 server certificate is signed by the private GeoLocator Root CA. The Root CA private key stays on the development machine. The public Root CA certificate is delivered by the ESP32 onboarding flow and Android performs the OS-required installation confirmation.

The certificate onboarding/browser verification path is hardware-verified with secure context, camera, WebXR API and `immersive-ar` all passing.

## WEB-XR-A — immersive tracking

`WEB-XR-A` provides:

```text
secure-context capability probe
camera API presence
WebGL availability
navigator.xr availability
immersive-ar support
        ↓
user-triggered immersive-ar session
        ↓
transparent XRWebGLLayer
        ↓
local-floor reference space
        ↓ fallback
local reference space
        ↓
XRFrame.getViewerPose()
        ↓
phone XYZ + orientation quaternion
```

This layer is hardware-verified on Android Chrome.

## WEB-XR-B — ARCore/WebXR Depth

The immersive session optionally requests `depth-sensing` with CPU-optimized depth. On the tested phone the browser provides:

```text
depth usage:  cpu-optimized
depth format: luminance-alpha
depth buffer: 160 × 90
```

For each usable depth frame, the browser reconstructs metric view-space points using the depth projection matrix and transforms them into the common WebXR world frame. No image or depth data is uploaded to a cloud service.

## WEB-XR-C — bounded adaptive point cloud

The scanner does not retain every unique depth voxel until memory fills. It uses a streaming bounded-memory spatial sampler:

```text
ARCore CPU depth
        ↓
local depth-neighbour consistency check
        ↓
metric world/project XYZ
        ↓
40 mm fine voxel identity
        ↓
300 mm spatial coverage cell
        ↓
adaptive representative quota per coverage cell
        ↓
deterministic stability-weighted bottom-k replacement
        ↓
fixed global representative budget (60k)
        ↓
final local-support / repeat-observation filter
        ↓
robust 1st–99th percentile bounds
        ↓
local WebGL point-cloud review
```

### Why adaptive decimation is required

A long scan can observe hundreds of thousands of nearly redundant fine voxels. A simple hard point limit is scan-order biased: early viewpoints consume the budget and later viewpoints cannot add geometry.

The adaptive sampler instead gives newly discovered spatial coverage its own budget. As the number of 300 mm coverage cells grows, the representative quota per cell decreases automatically. Dense already-covered regions are compacted, while new regions continue to enter the retained map.

Within a coverage cell:

- repeated observations update/average an existing 40 mm representative rather than adding another point;
- stable repeated representatives receive higher retention weight;
- deterministic voxel hashing gives scan-order-independent bottom-k selection;
- when the quota shrinks, low-priority representatives are evicted;
- retained representatives are globally bounded, so scan duration no longer implies unbounded browser memory growth.

The only true exhaustion state is when there are more distinct 300 mm coverage cells than the global budget can represent even at one point per cell. That condition is exposed separately as `coverageRejectedCount`.

### Live temporal coverage guide

During immersive scanning the retained map is also used for a lightweight on-camera coverage guide. The guide is intentionally delayed so the first few frames remain a clean camera view and a single noisy depth frame cannot immediately paint a surface as complete.

Coverage confidence is based primarily on how many **different sampled depth frames** observed a 300 mm coverage cell, not how many depth pixels happened to land there in one frame.

Current policy:

```text
warm-up:
  >= 12 sampled depth frames
  >= 600 retained representatives
  >= 20 coverage cells
  >= 120 valid candidate samples / sampled frame

per-cell visibility:
  < 4 sampled-frame hits   -> hidden / transparent
  4..9 hits                -> visible, red/orange -> yellow
  >= 10 hits               -> well-covered, green/teal

rendering:
  max 16k live guide points
  GPU buffer refresh ~ every 550 ms
  one GL_POINTS draw per XRView
  scanner point-cloud processing continues independently
```

The live overlay reports `READY / VISIBLE` coverage-cell counts. Dense well-observed areas become more opaque and greener, while areas with insufficient temporal support remain absent or faint so the operator can see where another pass is useful.

The guide is a UX aid, not a replacement for the final scan filter. The final point cloud still uses the bounded adaptive representatives, repeat-observation/spatial support cleanup and robust bounds.

### Scan diagnostics

A completed scan reports:

```text
Stable 3D points
Retained representatives
Spatial memory budget
Coverage cells
Coverage cell size
Adaptive reps / cell
Candidate depth samples
Samples decimated
In-cell replacements
Adaptive rebalance evictions
Coverage rejects
Depth frames / sampled frames
40 mm fine voxel resolution
Depth range cap
Robust extent X/Y/Z
Raw outlier span XYZ
```

A live retained-point count may plateau or decrease slightly when expanding coverage triggers a rebalance. That is expected and does not mean depth capture stopped.

## Scan operating guidance

A 360° rotation from one position is useful for validating room-scale geometry. It can measure the visible surrounding shell but cannot see surfaces occluded behind furniture or rubble.

For the diploma rubble workflow, the operator should both rotate and translate the phone between viewpoints. The result remains a visible-surface constraint; unknown internal volume stays unknown rather than being marked empty.

## Downstream scan contract

```text
filtered metric point cloud
        ↓
surface / occupancy representation
        ↓
AprilTag observations for MAIN + GEO1…GEO4
        ↓
robust tag pose in the same world frame
        ↓
fixed tag → geophone-contact transform
        ↓
locked sensor XYZ
        ↓
BMP relative-height + IMU motion/tilt + future ranging fusion
```

## Current implementation slices

1. `WEB-HTTPS-A` — HTTPS transport hardening. **COMPLETE / HARDWARE VERIFIED**
2. `WEB-HTTPS-ONBOARDING` — QR Root CA onboarding + secure-context verification. **COMPLETE / HARDWARE VERIFIED**
3. `WEB-HTTPS-B` — React production bundle + gzip + flash runtime serving + boot-time SD seed. **COMPLETE / HARDWARE VERIFIED**
4. `WEB-XR-A` — immersive AR session + phone pose/reference-space runtime. **COMPLETE / HARDWARE VERIFIED**
5. `WEB-XR-B` — ARCore/WebXR CPU depth acquisition. **COMPLETE / HARDWARE VERIFIED**
6. `WEB-XR-C` — metric point-cloud accumulation, adaptive decimation, local review and live temporal coverage guide. **POINT CLOUD HARDWARE VERIFIED; LIVE COVERAGE GUIDE REBUILD/PHONE RETEST REQUIRED**
7. `WEB-XR-D` — surface / occupancy reconstruction. **NEXT AFTER COVERAGE-GUIDE ACCEPTANCE**
8. `WEB-TAG-A` — AprilTag observations and tag-to-geophone offsets for MAIN + GEO1…GEO4.
9. Add ESP API/storage contract for saved geometry and sensor positions.
10. Fuse scan pose with barometer, IMU and future ranging without moving a locked position unless motion is detected.
