# GeoLocator Web UI

React + TypeScript + Vite workspace for the phone/desktop 3D interface.

This workspace provides a metric 3D scene, MAIN + GEO1…GEO4 placement, position locking, uncertainty display, a triangle-mesh surface contract, and the browser WebXR runtime used by the diploma scanning pipeline.

## Stack

- React 19
- TypeScript
- Vite 8
- Three.js through `@react-three/fiber`

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

The build now produces two outputs:

```text
webui/dist/
    normal Vite production output

webui/esp-dist/
    index.html
    build-id.txt
    assets/app.js.gz
    assets/app.css.gz
```

`esp-dist` is prepared automatically after the normal Vite build. The hashed Vite asset names are rewritten to stable ESP routes and JavaScript/CSS are pre-compressed with gzip so the ESP32 does not compress a ~1.1 MB Three.js bundle at runtime.

The generated `dist/`, `esp-dist/`, and `components/web_server/generated_webui/` directories are local build artifacts and are intentionally ignored by Git.

## WEB-HTTPS-B deployment

The ESP-IDF component checks for `webui/esp-dist` during CMake configure.

When the bundle exists before `idf.py reconfigure/build`:

```text
webui/esp-dist
        ↓ CMake copies stable build artifacts
components/web_server/generated_webui
        ↓ EMBED_FILES / EMBED_TXTFILES
firmware flash seed
        ↓ first secure-server start
/sdcard/geolocator/web/
    index.html
    build-id.txt
    assets/app.js.gz
    assets/app.css.gz
```

The SD update is build-id based. If the same bundle is already present, boot does not rewrite it. When a new Web UI build is flashed, files are replaced through temporary files and `build-id.txt` is written last, so an interrupted update is detected on the next boot.

Serving priority is:

```text
1. React Web UI from SD
2. embedded React seed from firmware flash
3. legacy tiny embedded dashboard
```

This removes the need to take the SD card out and manually copy web files for normal firmware updates. The legacy dashboard remains available as the final fallback when no React bundle was prepared before the IDF build.

The browser requests:

```text
https://geolocator.local/
https://geolocator.local/assets/app.js
https://geolocator.local/assets/app.css
```

The ESP serves the JavaScript/CSS responses with `Content-Encoding: gzip` from the corresponding `.gz` files.

## Coordinate contract

Application data uses the diploma coordinate convention:

```text
X = local horizontal axis
Y = local horizontal axis
Z = height
MAIN = preferred local frame origin
units = metres
```

Three.js is Y-up internally, therefore the renderer maps application `[x, y, z]` to Three.js `[x, z, -y]`. The application/domain model always stays in X/Y/Z with Z as height.

The current `demoScene` is only a UI fixture. It must never be treated as measured sensor coordinates.

## Scan pipeline contract

```text
phone tracking + camera/depth
        ↓
metric scan / surface mesh
        ↓
AprilTag observations for MAIN + GEO1…GEO4
        ↓
robust pose estimate in the same world frame
        ↓
fixed tag → geophone-contact transform
        ↓
locked sensor XYZ
        ↓
BMP relative-height + IMU motion/tilt + future ranging fusion
```

The external scan constrains the visible outer geometry. Unknown internal rubble volume must not be classified as empty just because the phone cannot see it.

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
  camera / WebXR / future depth capture
```

Browser camera APIs and WebXR run only from the trusted `https://geolocator.local/` secure context.

The ESP32 server certificate is signed by the private GeoLocator Root CA. The Root CA private key stays on the development machine. The public Root CA certificate is delivered by the ESP32 onboarding flow and Android performs the OS-required installation confirmation.

The certificate onboarding/browser verification path is hardware-verified with secure context, camera, WebXR API and `immersive-ar` all passing.

## WEB-XR-A runtime

`WEB-XR-A` adds the first real immersive browser AR runtime without depth acquisition yet.

It provides:

```text
secure-context capability probe
camera API presence
WebGL availability
navigator.xr availability
immersive-ar support probe
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
phone XYZ + orientation quaternion + frame/tracking diagnostics
```

The session requests DOM overlay as an optional feature. When supported, a compact GeoLocator overlay remains visible over camera passthrough with live pose information and an `End AR session` control. The normal Three.js scene viewer is suspended while immersive AR is active to avoid running two heavy WebGL render loops at the same time.

Depth sensing is intentionally not requested by this slice. Depth configuration, depth frames and point-cloud generation belong to `WEB-XR-B`.

### WEB-XR-A build acceptance

Hardware-independent build acceptance completed on 2026-08-31:

```text
npm ci: PASS
npm run typecheck: PASS
npm run build: PASS
Vite 8.2.2: PASS
production JS: ~1.11 MB / ~303 kB gzip
```

The Vite warning about a chunk larger than 500 kB is informational and does not fail the build. Code splitting can be revisited after the first phone AR hardware acceptance if startup/network measurements show it is useful.

## Current implementation slices

1. `WEB-HTTPS-A` — dual HTTP bootstrap + HTTPS main transport. **COMPLETE / HARDWARE VERIFIED**
2. `WEB-HTTPS-ONBOARDING` — QR Root CA onboarding + secure-context verification. **COMPLETE / HARDWARE VERIFIED**
3. `WEB-XR-A` — immersive AR session + phone pose/reference-space runtime. **BUILD VERIFIED; PHONE AR ACCEPTANCE NEXT**
4. `WEB-HTTPS-B` — React production bundle, gzip, firmware seed, automatic SD deployment, flash/legacy fallback. **IMPLEMENTED; IDF BUILD + HARDWARE ACCEPTANCE NEXT**
5. `WEB-XR-B` — request ARCore/WebXR depth sensing and acquire metric depth samples.
6. `WEB-XR-C` — local point-cloud accumulation, registration and mesh reconstruction.
7. `WEB-TAG-A` — AprilTag observations and tag-to-geophone offsets for MAIN + GEO1…GEO4.
8. Add an ESP API contract for saved scene geometry and sensor positions.
9. Fuse scan pose with barometer, IMU and future ranging without moving a locked position unless motion is detected.
