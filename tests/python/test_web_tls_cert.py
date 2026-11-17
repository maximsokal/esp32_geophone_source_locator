"""The board issues its own HTTPS certificate for the address it has (components/web_server/web_tls_cert.c).

Board 2026-11-17: a phone opened the page by address ("https://192.168.0.191"), the build-time certificate named only
"geolocator.local", the browser aborted ~80 handshakes and the page's module and API fetches failed with them. The board now
signs a leaf for its current addresses with a name-constrained Device CA. These tests build the real C file against ESP-IDF's own
mbedtls on the host, run it, and let openssl decide whether a client would accept the result.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
IDF_MBEDTLS = Path(os.environ.get("IDF_PATH", "C:/Espressif/frameworks/esp-idf-v5.5.4")) / "components" / "mbedtls" / "mbedtls"
BUILD_DIR = ROOT / ".test-build" / "tls-cert-host"
DRIVER = BUILD_DIR / "web_tls_cert_issue.exe"
BUILD_SCRIPT = ROOT / "tests" / "tls_cert" / "build_host.bat"
SOURCES = [
    ROOT / "components" / "web_server" / "web_tls_cert.c",
    ROOT / "components" / "web_server" / "web_tls_cert.h",
    ROOT / "tests" / "tls_cert" / "web_tls_cert_issue_main.c",
]

needs_host_tools = pytest.mark.skipif(
    sys.platform != "win32" or shutil.which("openssl") is None or not IDF_MBEDTLS.is_dir(),
    reason="needs Windows MSVC (build_host.bat), openssl and ESP-IDF's mbedtls",
)


def _openssl(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["openssl", *args], check=check, capture_output=True, text=True)


@pytest.fixture(scope="module")
def driver() -> Path:
    newest = max(path.stat().st_mtime for path in SOURCES)
    if not DRIVER.is_file() or DRIVER.stat().st_mtime < newest:
        BUILD_DIR.mkdir(parents=True, exist_ok=True)
        # One quoted command line: the repository path has a space, which a list argument would split under cmd.exe.
        result = subprocess.run(
            f'"{BUILD_SCRIPT}" "{IDF_MBEDTLS}" "{BUILD_DIR}"', shell=True, capture_output=True, text=True
        )
        assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
    return DRIVER


@pytest.fixture(scope="module")
def pki(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """A throwaway Root CA (same shape as tools/generate_geolocator_tls.ps1) and the Device CA the real tool issues from it."""
    certs = tmp_path_factory.mktemp("pki")
    config = certs / "root.cnf"
    config.write_text(
        "[req]\ndistinguished_name=dn\nx509_extensions=v3_ca\nprompt=no\n[dn]\nC=UA\nO=GeoLocator\nCN=GeoLocator Root CA 2\n"
        "[v3_ca]\nbasicConstraints=critical,CA:TRUE,pathlen:1\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n",
        encoding="ascii",
    )
    _openssl("genrsa", "-out", str(certs / "rootCA.key"), "2048")
    _openssl("req", "-x509", "-new", "-key", str(certs / "rootCA.key"), "-sha256", "-days", "3650",
             "-out", str(certs / "rootCA.crt"), "-config", str(config))
    _openssl("genrsa", "-out", str(certs / "leaf.key"), "2048")
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "generate_geolocator_device_ca.py"), "--certs-dir", str(certs)],
        check=True, capture_output=True,
    )
    return certs


def _issue(driver: Path, pki: Path, name: str, *addresses: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(driver), str(pki / "leaf.key"), str(pki / "deviceCA.crt"), str(pki / "deviceCA.key"), str(pki / name), *addresses],
        capture_output=True, text=True,
    )


def _verify(pki: Path, chain: str, address: str) -> subprocess.CompletedProcess[str]:
    return _openssl(
        "verify", "-CAfile", str(pki / "rootCA.crt"), "-untrusted", str(pki / chain), "-purpose", "sslserver",
        "-verify_ip", address, "-verify_hostname", "geolocator.local", str(pki / chain), check=False,
    )


@needs_host_tools
def test_a_client_accepts_the_issued_chain_by_address_and_by_name(driver: Path, pki: Path) -> None:
    assert _issue(driver, pki, "chain.pem", "192.168.0.191", "192.168.4.1").returncode == 0
    for address in ("192.168.0.191", "192.168.4.1"):
        result = _verify(pki, "chain.pem", address)
        assert result.returncode == 0, result.stdout + result.stderr


@needs_host_tools
def test_the_chain_names_exactly_the_addresses_it_was_issued_for(driver: Path, pki: Path) -> None:
    assert _issue(driver, pki, "chain2.pem", "10.84.187.209").returncode == 0
    assert _verify(pki, "chain2.pem", "10.84.187.209").returncode == 0
    other = _verify(pki, "chain2.pem", "10.84.187.210")
    assert other.returncode != 0 and "IP address mismatch" in other.stdout + other.stderr


@needs_host_tools
def test_the_board_cannot_issue_for_a_public_address(driver: Path, pki: Path) -> None:
    # The Device CA is name-constrained: even if the board's key were extracted, a certificate for a public IP is refused.
    assert _issue(driver, pki, "public.pem", "8.8.8.8").returncode == 0
    result = _verify(pki, "public.pem", "8.8.8.8")
    assert result.returncode != 0 and "permitted subtree violation" in result.stdout + result.stderr


@needs_host_tools
def test_the_leaf_is_a_server_certificate_with_the_name_and_the_addresses(driver: Path, pki: Path) -> None:
    assert _issue(driver, pki, "text.pem", "192.168.0.191", "192.168.0.191", "not-an-ip", "").returncode == 0
    text = _openssl("x509", "-in", str(pki / "text.pem"), "-noout", "-text").stdout
    assert "CA:FALSE" in text and "TLS Web Server Authentication" in text
    assert "DNS:geolocator.local" in text
    assert text.count("IP Address:192.168.0.191") == 1  # a repeated address is written once
    assert "not-an-ip" not in text


@needs_host_tools
def test_without_a_usable_address_nothing_is_issued(driver: Path, pki: Path) -> None:
    result = _issue(driver, pki, "none.pem", "", "999.1.1.1", "1.2.3")
    assert result.returncode != 0
    assert not (pki / "none.pem").exists()


def test_the_firmware_signs_in_its_own_task_and_falls_back_to_the_build_time_certificate() -> None:
    source = (ROOT / "components" / "web_server" / "web_server.c").read_text(encoding="utf-8", errors="replace")
    assert "web_server_tls_issue_for_current_address(config)" in source
    # mbedtls writes a certificate with several KB of stack: it must not run on the lifecycle task.
    assert "xTaskCreate(web_server_tls_issue_task" in source
    assert "using the build-time certificate" in source
    cmake = (ROOT / "components" / "web_server" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert '"web_tls_cert.c"' in cmake and "certs/deviceCA.key" in cmake
