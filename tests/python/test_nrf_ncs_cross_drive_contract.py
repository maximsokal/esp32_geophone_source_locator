from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NCS_PS1 = ROOT / "nRF54L15" / "ncs.ps1"


def test_ncs_wrapper_keeps_west_on_checkout_drive_and_anchors_workspace_with_zephyr_base():
    script = NCS_PS1.read_text(encoding="utf-8")

    assert '$zephyrBase = Join-Path $topDir "zephyr"' in script
    assert '$originalZephyrBase = $env:ZEPHYR_BASE' in script
    assert '$env:ZEPHYR_BASE = $zephyrBase' in script
    assert 'Push-Location $project' in script
    assert 'Push-Location $topDir' not in script

    zephyr_base_pos = script.index('$env:ZEPHYR_BASE = $zephyrBase')
    checkout_pos = script.index('Push-Location $project')
    build_pos = script.index('Invoke-WestBuild $csBuild $true', checkout_pos)
    assert zephyr_base_pos < checkout_pos < build_pos

    assert 'Remove-Item Env:ZEPHYR_BASE -ErrorAction SilentlyContinue' in script
    assert '$env:ZEPHYR_BASE = $originalZephyrBase' in script
