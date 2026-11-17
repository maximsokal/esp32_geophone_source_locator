"""Plain HTTP on port 80 sends a browser to HTTPS on the host it asked for.

Board 2026-11-17: a phone that could not resolve "geolocator.local" typed the device's IP address, was redirected to the
very name it could not resolve, and could not connect. The decision is a pure function with its own host test
(components/web_server/tests/web_redirect_policy_test.c); this pins that the handler really asks it.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _routes() -> str:
    return (ROOT / "components/web_server/web_server_routes.c").read_text(encoding="utf-8")


def test_the_bootstrap_redirect_keeps_the_host_the_client_used() -> None:
    source = _routes()
    start = source.index("static esp_err_t web_server_bootstrap_index_handler(")
    handler = source[start:source.index("static esp_err_t web_server_secure_index_handler(")]
    assert 'httpd_req_get_hdr_value_str(request, "Host"' in handler
    assert "web_redirect_https_location(" in handler
    assert handler.index("web_redirect_https_location(") < handler.index('httpd_resp_set_hdr(request, "Location"')
    # The Location comes from the policy, never straight from the mDNS name.
    assert 'httpd_resp_set_hdr(request, "Location", APP_MDNS_WEB_URL)' not in handler
    assert 'httpd_resp_set_hdr(request, "Location", location)' in handler
    # The mDNS name stays as the fallback for a request with no usable Host.
    assert "APP_MDNS_WEB_URL, location" in handler


def test_the_policy_is_part_of_the_firmware_build() -> None:
    cmake = (ROOT / "components/web_server/CMakeLists.txt").read_text(encoding="utf-8")
    assert '"web_redirect_policy.c"' in cmake
