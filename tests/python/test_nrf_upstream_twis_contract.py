from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
NRF = ROOT / "nRF54L15"
PINNED_SEEED_COMMIT = "1ec1287f8e4bc4067a6fd593991e36875aef989f"


def test_upstream_twis_overlay_matches_pinned_xiao_wiring():
    overlay = (NRF / "zephyr" / "app.overlay").read_text(encoding="utf-8")

    assert 'compatible = "nordic,nrf-twis";' in overlay
    assert "NRF_PSEL(TWIS_SDA, 1, 10)" in overlay
    assert "NRF_PSEL(TWIS_SCL, 1, 11)" in overlay
    assert "&i2c22_default" in overlay
    assert "&i2c22_sleep" in overlay

    # The private remote-sensor bus must not be converted to target mode.
    assert "&i2c30" not in overlay
    assert "TWIS_SDA, 0, 4" not in overlay
    assert "TWIS_SCL, 0, 3" not in overlay


def test_required_zephyr_target_features_are_enabled():
    config = (NRF / "zephyr" / "prj.conf").read_text(encoding="utf-8")

    for symbol in (
        "CONFIG_I2C=y",
        "CONFIG_I2C_TARGET=y",
        "CONFIG_I2C_TARGET_BUFFER_MODE=y",
        "CONFIG_HWINFO=y",
    ):
        assert symbol in config


def test_seeed_platform_revision_stays_pinned_for_twis_contract():
    platformio = (NRF / "platformio.ini").read_text(encoding="utf-8")
    assert f"platform-seeedboards.git#{PINNED_SEEED_COMMIT}" in platformio


def test_foundation_does_not_smuggle_in_project_addresses_or_main_wiring():
    header = (NRF / "include" / "nrf_upstream_target_zephyr.h").read_text(encoding="utf-8")
    source = (NRF / "src" / "nrf_upstream_target_zephyr.c").read_text(encoding="utf-8")
    main = (NRF / "src" / "main.c").read_text(encoding="utf-8")
    combined = header + source

    assert "DISCOVERY" not in combined
    assert "REMOTE_ADDRESS" not in combined
    assert "MAIN_ADDRESS" not in combined
    assert "nrf_upstream_target_zephyr" not in main
