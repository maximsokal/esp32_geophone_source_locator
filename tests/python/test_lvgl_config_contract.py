"""Contracts for the LVGL 9.6 configuration migration."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_lvgl_defaults_use_byte_pool_and_no_deprecated_compatibility_values():
    defaults = read("sdkconfig.lvgl.defaults")

    assert "CONFIG_LV_MEM_SIZE=65536" in defaults
    assert "# CONFIG_LV_MEM_SIZE_KILOBYTES is not set" in defaults
    assert "# CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES is not set" in defaults
    assert "# CONFIG_LV_ASSERT_HANDLER_INCLUDE is not set" in defaults
    assert "# CONFIG_LV_ASSERT_USE_CUSTOM_INCLUDE is not set" in defaults


def test_idf_runner_migrates_stale_generated_lvgl_values():
    runner = read("tools/run_idf.ps1")

    assert "Sync-LvglSdkconfigMigration" in runner
    assert "CONFIG_LV_MEM_SIZE_KILOBYTES" in runner
    assert "CONFIG_LV_ASSERT_HANDLER_INCLUDE_IS_NON_EMPTY" in runner
    assert "Sync-LvglSdkconfigMigration $effectiveSdkconfig" in runner
