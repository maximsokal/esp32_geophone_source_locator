# GeoLocator TLS credentials

This directory intentionally contains no generated credentials in Git.

GeoLocator uses these local files when they exist:

- `servercert.pem` — HTTPS server certificate for `geolocator.local`.
- `prvtkey.pem` — HTTPS server private key.
- `rootCA.crt` — public GeoLocator Root CA certificate. The firmware embeds this public certificate and serves it from the setup SoftAP at `/setup/ca.crt` for QR onboarding.
- `rootCA.key` — Root CA private key. Keep this only on the development machine and never copy it to the ESP32 or GitHub.

Generate the files from the repository root with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\generate_geolocator_tls.ps1
```

The script requires `openssl` in `PATH`.

After credentials are generated, rerun ESP-IDF configure/build so CMake detects and embeds `servercert.pem`, `prvtkey.pem`, and the public `rootCA.crt`:

```powershell
idf.py reconfigure
idf.py build
```

The QR onboarding sequence is:

1. Connect the phone to `GEOLOCATOR_SETUP` using the TFT Wi-Fi QR.
2. Scan the TFT Root CA QR, which opens `http://192.168.4.1/setup/ca`.
3. Download `GeoLocator-Root-CA.crt` directly from the ESP32 and install it in the phone certificate store. Android requires explicit user confirmation; firmware cannot install a CA silently.
4. Return to the CA page and tap `I installed the CA - continue`.
5. Scan the TFT hotspot setup QR and save the phone hotspot credentials.
6. After GeoLocator switches to STA and HTTPS starts, scan the TFT HTTPS verification QR.
7. The verifier checks secure context, real camera access, WebXR API availability and `immersive-ar` support, then reports the result back to the ESP32.

A clean checkout without local TLS files still builds. In that case the HTTP bootstrap/setup server remains available; HTTPS and Root CA delivery stay disabled until the local credentials are generated.
