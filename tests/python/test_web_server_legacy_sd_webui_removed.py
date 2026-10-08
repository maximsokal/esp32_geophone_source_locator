"""Source contracts keeping the HTTPS WebUI on the embedded flash bundle."""

import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
LEGACY_SD_SYMBOLS = (
    "web_webui_build_path",
    "web_webui_file_exists",
    "web_webui_make_directory",
    "web_webui_prepare_directories",
    "web_webui_write_atomic",
    "web_webui_sd_build_matches",
    "web_webui_sd_has_bundle",
    "web_webui_seed_sd",
    "web_webui_stream_file",
    "web_webui_send_embedded",
    "web_webui_send_asset",
    "web_webui_js_handler",
    "web_webui_css_handler",
    "web_webui_asset_t",
    "WEB_WEBUI_ASSET_JS",
    "WEB_WEBUI_ASSET_CSS",
    "WEB_WEBUI_IO_CHUNK_BYTES",
    "WEB_WEBUI_PATH_BYTES",
    "WEBUI_RELATIVE_ROOT",
)


def read(relative: str) -> str:
    path = REPO_ROOT / relative
    assert path.is_file(), f"Expected source file is missing: {relative}"
    return path.read_text(encoding="utf-8")


def _component_and_main_sources():
    for root_name in ("components", "main"):
        root = REPO_ROOT / root_name
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in {".c", ".h"}:
                yield path.relative_to(REPO_ROOT).as_posix(), path.read_text(encoding="utf-8")


def _host_manifests():
    host_root = REPO_ROOT / "tests" / "host"
    manifest = host_root / "manifest.json"
    if manifest.is_file():
        yield manifest
    yield from sorted((host_root / "manifest.d").glob("*.json"))


def test_production_webui_root_and_assets_are_flash_backed():
    routes = read("components/web_server/web_server_routes.c")
    web_server = read("components/web_server/web_server.c")

    assert re.search(r"\bweb_webui_flash_send_index\s*\(", routes), (
        "components/web_server/web_server_routes.c must serve HTTPS / from the flash handler"
    )
    assert re.search(r"\bweb_webui_flash_register\s*\(", routes), (
        "components/web_server/web_server_routes.c must register flash-backed WebUI assets"
    )
    assert re.search(r"\bweb_server_routes_register\s*\(", web_server), (
        "components/web_server/web_server.c must install the production route table"
    )
    assert re.search(r"\bweb_webui_flash_prepare_cache\s*\(", web_server), (
        "components/web_server/web_server.c must prepare only the flash WebUI cache"
    )

    forbidden_route_symbols = (
        "web_webui_send_index",
        "web_webui_register",
        *LEGACY_SD_SYMBOLS,
    )
    for path, source in (
        ("components/web_server/web_server_routes.c", routes),
        ("components/web_server/web_server.c", web_server),
    ):
        for symbol in forbidden_route_symbols:
            assert not re.search(rf"\b{re.escape(symbol)}\b", source), (
                f"{path} must not reference legacy SD WebUI symbol {symbol}"
            )


def test_sd_webui_helpers_are_absent_and_remaining_prepare_hook_is_safe():
    sources = tuple(_component_and_main_sources())
    for relative, source in sources:
        for symbol in LEGACY_SD_SYMBOLS:
            assert not re.search(rf"\b{re.escape(symbol)}\b", source), (
                f"Legacy SD WebUI symbol {symbol} remains in {relative}"
            )

    webui = read("components/web_server/web_webui.c")
    assert '#include "sd_card.h"' not in webui, (
        "components/web_server/web_webui.c must not depend on SD card serving"
    )
    assert re.search(r"esp_err_t\s+web_webui_prepare\s*\(\s*void\s*\)", webui), (
        "components/web_server/web_webui.c must retain the production preparation hook"
    )
    assert re.search(r"return\s+ESP_OK\s*;", webui), (
        "components/web_server/web_webui.c preparation hook must remain a successful no-op"
    )
    assert re.search(
        r"esp_err_t\s+web_webui_send_index\s*\(\s*httpd_req_t\s*\*\s*request\s*\)"
        r"\s*\{\s*return\s+request\s*==\s*NULL\s*\?\s*ESP_ERR_INVALID_ARG"
        r"\s*:\s*ESP_ERR_NOT_FOUND\s*;\s*\}",
        webui,
        re.DOTALL,
    ), "components/web_server/web_webui.c must keep the declared root API fail-closed"
    assert re.search(
        r"esp_err_t\s+web_webui_register\s*\(\s*httpd_handle_t\s+server\s*\)"
        r"\s*\{\s*return\s+server\s*==\s*NULL\s*\?\s*ESP_ERR_INVALID_ARG"
        r"\s*:\s*ESP_ERR_NOT_SUPPORTED\s*;\s*\}",
        webui,
        re.DOTALL,
    ), "components/web_server/web_webui.c must keep the declared registration API fail-closed"
    assert not re.search(r"httpd_resp_(?:send|send_chunk|send_err)\s*\(", webui), (
        "components/web_server/web_webui.c must not serve an alternate WebUI response"
    )

    prepare = read("components/web_server/web_server_prepare.c")
    assert re.search(r"\bweb_webui_prepare\s*\(\s*\)", prepare), (
        "components/web_server/web_server_prepare.c must retain the existing fallback hook"
    )
    startup = read("main/app_startup.c")
    assert re.search(r"\bweb_server_prepare_webui\s*\(\s*\)", startup), (
        "main/app_startup.c must keep its existing WebUI preparation call"
    )


def test_component_and_host_manifests_only_list_a_used_webui_source():
    webui_source = REPO_ROOT / "components" / "web_server" / "web_webui.c"
    component_cmake = read("components/web_server/CMakeLists.txt")
    source_listed = bool(re.search(r"[\"']web_webui\.c[\"']", component_cmake))

    if webui_source.exists():
        assert source_listed, (
            "components/web_server/CMakeLists.txt must compile the retained web_webui_prepare hook"
        )
        prepare = read("components/web_server/web_server_prepare.c")
        assert re.search(r"\bweb_webui_prepare\s*\(\s*\)", prepare), (
            "web_webui.c is allowed in the source list only while production uses its prepare hook"
        )
    else:
        assert not source_listed, (
            "components/web_server/CMakeLists.txt must not list deleted web_webui.c"
        )

    for manifest_path in _host_manifests():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for entry in manifest.get("tests", []):
            sources = entry.get("sources", [])
            if any("web_webui.c" in source for source in sources):
                assert webui_source.exists() and source_listed, (
                    f"{manifest_path.relative_to(REPO_ROOT)} lists web_webui.c without a retained production use"
                )


def test_flash_asset_contracts_remain_pinned_after_legacy_removal():
    flash_host_manifest = read("tests/host/manifest.json")
    flash_host_test = read("components/web_server/tests/web_webui_flash_host_test.c")
    runtime_contract = read("tests/python/test_memory_runtime_policy.py")

    assert '"name": "web_webui_flash"' in flash_host_manifest, (
        "tests/host/manifest.json must retain the flash WebUI host suite"
    )
    assert "web_webui_flash_host_test.c" in flash_host_manifest, (
        "tests/host/manifest.json must continue compiling the flash WebUI route and asset tests"
    )
    assert "web_webui_flash_register" in flash_host_test, (
        "components/web_server/tests/web_webui_flash_host_test.c must retain route registration coverage"
    )
    assert "web_webui_flash.c" in runtime_contract and "WEB_FLASH_SEND_CHUNK_BYTES" in runtime_contract, (
        "tests/python/test_memory_runtime_policy.py must keep pinning flash asset behavior"
    )

    current_test = Path(__file__).resolve()
    for path in (REPO_ROOT / "tests" / "python").glob("*.py"):
        if path.resolve() == current_test:
            continue
        source = path.read_text(encoding="utf-8")
        for symbol in LEGACY_SD_SYMBOLS:
            assert not re.search(rf"\b{re.escape(symbol)}\b", source), (
                f"{path.relative_to(REPO_ROOT)} must not pin removed SD WebUI symbol {symbol}"
            )
