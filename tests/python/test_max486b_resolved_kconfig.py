from pathlib import Path

import pytest


PRODUCTION_CONFIG = (
    Path.home()
    / ".ncs-workspaces"
    / "esp32_geophone_source_locator"
    / "nrf54l15-production-v3.4.0"
    / "zephyr"
    / "zephyr"
    / ".config"
)
STACK_OPTIONS = (
    "CONFIG_STACK_SENTINEL",
    "CONFIG_HW_STACK_PROTECTION",
    "CONFIG_THREAD_STACK_INFO",
    "CONFIG_THREAD_ANALYZER",
)
EXPECTED_STACK_OPTIONS = {
    "CONFIG_STACK_SENTINEL": False,
    "CONFIG_HW_STACK_PROTECTION": True,
    "CONFIG_THREAD_STACK_INFO": True,
    "CONFIG_THREAD_ANALYZER": False,
}


def _read_bool_options(path: Path, options: tuple[str, ...]) -> dict[str, bool]:
    lines = {line.strip() for line in path.read_text(encoding="utf-8").splitlines()}
    actual = {}
    for option in options:
        enabled = f"{option}=y"
        disabled = f"# {option} is not set"
        assert enabled in lines or disabled in lines, f"{option} missing from resolved {path}"
        actual[option] = enabled in lines
    return actual


def test_stack_kconfigs_match_the_resolved_ncs_build():
    config = PRODUCTION_CONFIG
    if not config.is_file():
        pytest.skip(f"resolved NCS production build config not found: {config}")

    actual = _read_bool_options(config, STACK_OPTIONS)
    assert actual == EXPECTED_STACK_OPTIONS, f"unexpected resolved stack Kconfig: {actual}"
