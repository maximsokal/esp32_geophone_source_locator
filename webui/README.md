# GeoLocator Web UI

React + TypeScript + Vite workspace for the phone/desktop 3D interface.

This first slice is deliberately independent from future remote sensor hardware. It provides a metric 3D scene, MAIN + GEO1…GEO4 placement, position locking, uncertainty display, and a triangle-mesh surface contract that can later receive a real phone scan.

## Stack

- React 19
- TypeScript
- Vite
- Three.js through `@react-three/fiber`

All runtime dependencies are bundled by Vite into static files. The deployed UI must not use CDN imports. Once built, the browser downloads the JavaScript/CSS from the GeoLocator itself (planned SD-card deployment), so internet access is not required.

## Develop

```powershell
cd webui
npm install
npm run dev
```

Open the Vite URL printed by the terminal.

## Production build

```powershell
cd webui
npm run build
```

Output is written to `webui/dist/`. `vite.config.ts` uses `base: './'`, so generated asset URLs are relative and can be served from a nested ESP32 route or copied to SD storage.

## Deployment direction

Planned production layout:

```text
/sdcard/geolocator/web/
    index.html
    assets/*.js
    assets/*.css
```

The existing tiny embedded ESP-IDF dashboard should remain a fallback if the SD card or Web UI files are unavailable. Large React/Three bundles should not be embedded into application flash unless there is a specific reason.

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

The UI already models the two pieces the phone scan will eventually provide:

```text
triangle surface mesh
+
MAIN / GEO1 / GEO2 / GEO3 / GEO4 poses
```

The intended acquisition flow remains:

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

GeoLocator uses two local web transports with different responsibilities:

```text
HTTP :80
  bootstrap / Wi-Fi setup / certificate onboarding

HTTPS :443
  https://geolocator.local/
  normal dashboard and APIs
  camera / WebXR / future depth capture
```

Browser camera APIs and WebXR require a secure context, therefore the scanning workflow runs from `https://geolocator.local/`.

The ESP32 server certificate is signed by a private GeoLocator Root CA. The Root CA private key stays only on the development machine. The public Root CA certificate is delivered to the phone through the ESP32 onboarding flow and installed with the Android system confirmation required by the OS.

The certificate onboarding and browser verification path is hardware-verified with secure context, camera, WebXR API and `immersive-ar` all passing.

After that trust bootstrap, the intended field workflow is fully local and does not require internet access:

```text
phone browser
    ↓ HTTPS / trusted geolocator.local
ESP32-S3
    ↓
local React/Vite/Three Web UI
    ↓
WebXR / ARCore-backed tracking + supported depth APIs
```

Do not add insecure Chrome flags or make field operation depend on them.

## WEB-XR-A runtime

`WEB-XR-A` adds a browser AR runtime layer without depth acquisition yet.

It currently provides:

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

The AR session requests DOM overlay as an optional feature. When supported, the phone keeps a compact GeoLocator overlay visible over the camera passthrough with live pose information and an `End AR session` control.

Depth sensing is intentionally not requested by this slice. Depth configuration, depth frames and point-cloud generation belong to `WEB-XR-B` so pose/session bring-up can be tested independently.

### WEB-XR-A build acceptance

```powershell
cd webui
npm ci
npm run typecheck
npm run build
```

Expected result:

```text
TypeScript: PASS
Vite production build: PASS
webui/dist/index.html exists
webui/dist/assets/*.js exists
webui/dist/assets/*.css exists
```

The production phone acceptance test must be run only after `WEB-HTTPS-B` serves `webui/dist` through the already trusted `https://geolocator.local/` origin. A plain LAN Vite URL is not an acceptable field-mode WebXR test because it is not the trusted GeoLocator HTTPS origin.

## Next implementation slices

1. `WEB-HTTPS-A` — dual HTTP bootstrap + HTTPS main transport. **COMPLETE**
2. `WEB-HTTPS-ONBOARDING` — QR Root CA onboarding + secure-context verification. **COMPLETE**
3. `WEB-XR-A` — immersive AR session + phone pose/reference-space runtime. **IMPLEMENTED; build/hardware acceptance pending**
4. `WEB-HTTPS-B` — serve the production React/Vite build from SD with the embedded dashboard as fallback.
5. `WEB-XR-B` — request ARCore/WebXR depth sensing and acquire metric depth samples.
6. `WEB-XR-C` — local point-cloud accumulation, registration and mesh reconstruction.
7. `WEB-TAG-A` — AprilTag observations and tag-to-geophone offsets for MAIN + GEO1…GEO4.
8. Add a real ESP API contract for saved scene geometry and sensor positions.
9. Fuse scan pose with barometer, IMU and future ranging without moving a position that is already locked unless motion is detected.
