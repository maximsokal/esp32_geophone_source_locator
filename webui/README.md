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

## Important browser security boundary

The current ESP32 page is served as plain HTTP (`http://geolocator.local/`). That is sufficient for the dashboard and this 3D viewer, but browser camera APIs and WebXR are secure-context features. A production phone scan therefore cannot simply be enabled on the current plain-HTTP page.

Keep the 3D application architecture reusable while the capture runtime is decided. Practical options are:

1. an HTTPS/PWA origin that is cached for offline use and talks to the ESP over the local network where the target browser permits Local Network Access; or
2. a small Android/Capacitor shell around this same React/Vite application, with native camera/ARCore access and no internet dependency.

Do not add insecure Chrome flags or make field operation depend on them.

## Next implementation slices

1. Replace `demoScene` with a local JSON scene store and import/export.
2. Add GLB/PLY or project-specific triangle-mesh import.
3. Add AprilTag observation records and tag-to-geophone offsets.
4. Add a real ESP API contract for saved scene geometry and sensor positions.
5. Add the secure Android capture path (camera/tracking/depth).
6. Fuse scan pose with barometer, IMU and future ranging without moving a position that is already locked unless motion is detected.
