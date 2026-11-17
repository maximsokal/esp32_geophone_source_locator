#!/usr/bin/env python3
"""Issue the GeoLocator Device CA: a name-constrained intermediate the ESP32 uses to sign its own HTTPS certificate.

Why: a phone reaches the board by whatever address the router handed out (https://192.168.0.191), and a certificate fixed at
build time can only name "geolocator.local". The board therefore signs a fresh leaf for its current address at HTTPS start.
It must not hold the Root CA key (certs/README.md), so it holds this intermediate instead. The intermediate is
constrained to the name "geolocator.local" and to private IPv4 ranges, so even a key pulled out of a board cannot
impersonate a public site to a phone that trusts the Root CA.

Needs the Root CA from tools/generate_geolocator_tls.ps1 (certs/rootCA.crt and certs/rootCA.key) and openssl in PATH.
Writes certs/deviceCA.crt and certs/deviceCA.key (both ignored by git). Phones keep trusting the same Root CA. Valid 2026-01-01 .. 2034-01-01 (inside the Root CA's life; backdated so a phone with a
slightly slow clock still accepts it).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CERTS = ROOT / "components" / "web_server" / "certs"

# nameConstraints is deliberately NOT critical: the ESP32's mbedtls rejects a certificate carrying a critical extension it does not
# know (MBEDTLS_ERR_X509_INVALID_EXTENSIONS) and would refuse to start HTTPS. OpenSSL, BoringSSL (Chrome, Android) and Windows
# apply the constraint whether or not it is critical; tests/python/test_web_tls_cert.py proves it with openssl.
EXTENSIONS = """\
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
nameConstraints = @name_constraints

[name_constraints]
permitted;DNS.0 = geolocator.local
permitted;IP.0 = 10.0.0.0/255.0.0.0
permitted;IP.1 = 172.16.0.0/255.240.0.0
permitted;IP.2 = 192.168.0.0/255.255.0.0
"""

SUBJECT = "/C=UA/O=GeoLocator/CN=GeoLocator Device CA"


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--certs-dir", type=Path, default=CERTS, help="where rootCA.* are read and deviceCA.* written")
    certs = parser.parse_args().certs_dir

    root_crt = certs / "rootCA.crt"
    root_key = certs / "rootCA.key"
    if not root_crt.is_file() or not root_key.is_file():
        print("rootCA.crt / rootCA.key missing: run tools/generate_geolocator_tls.ps1 first", file=sys.stderr)
        return 1

    device_key = certs / "deviceCA.key"
    device_crt = certs / "deviceCA.crt"
    with tempfile.TemporaryDirectory() as tmp:
        extfile = Path(tmp) / "device-ca.ext"
        csr = Path(tmp) / "device-ca.csr"
        extfile.write_text(EXTENSIONS, encoding="ascii")
        openssl("genrsa", "-out", str(device_key), "2048")
        openssl("req", "-new", "-key", str(device_key), "-subj", SUBJECT, "-out", str(csr))
        openssl(
            "x509", "-req", "-in", str(csr), "-CA", str(root_crt), "-CAkey", str(root_key), "-CAcreateserial",
            "-out", str(device_crt), "-not_before", "20260101000000Z", "-not_after", "20340101000000Z", "-sha256",
            "-extfile", str(extfile),
        )
    openssl("verify", "-CAfile", str(root_crt), str(device_crt))
    print(f"wrote {device_crt} and {device_key}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
