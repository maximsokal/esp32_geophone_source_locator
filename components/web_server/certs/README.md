# GeoLocator TLS credentials

This directory intentionally contains no generated credentials in Git.

`WEB-HTTPS-A` uses these local files when they exist:

- `servercert.pem` — HTTPS server certificate for `geolocator.local`.
- `prvtkey.pem` — HTTPS server private key.
- `rootCA.crt` — public GeoLocator Root CA certificate; this will be delivered to the phone by the later QR onboarding slice.
- `rootCA.key` — Root CA private key. Keep this only on the development machine and never copy it to the ESP32 or GitHub.

Generate the files from the repository root with:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\generate_geolocator_tls.ps1
```

The script requires `openssl` in `PATH`.

After credentials are generated, rerun ESP-IDF configure/build so CMake detects and embeds `servercert.pem` and `prvtkey.pem`:

```powershell
idf.py reconfigure
idf.py build
```

A clean checkout without these local files still builds. In that case the HTTP bootstrap/setup server remains available and firmware logs that HTTPS is disabled until local credentials are generated.
