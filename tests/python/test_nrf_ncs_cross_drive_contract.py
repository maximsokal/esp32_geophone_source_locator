from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NCS_PS1 = ROOT / "nRF54L15" / "ncs.ps1"


def test_ncs_wrapper_uses_no_space_alias_and_anchors_workspace_with_zephyr_base():
    script = NCS_PS1.read_text(encoding="utf-8")

    assert '$zephyrBase = Join-Path $topDir "zephyr"' in script
    assert '$originalZephyrBase = $env:ZEPHYR_BASE' in script
    assert '$env:ZEPHYR_BASE = $zephyrBase' in script

    assert 'function New-NoSpaceProjectAlias' in script
    assert 'function Remove-NoSpaceProjectAlias' in script
    assert '$ProjectPath -notmatch \'\\s\'' in script
    assert 'subst {0} "{1}"' in script
    assert '$sourceAliasDrive = New-NoSpaceProjectAlias $project' in script
    assert '$buildProject = "$sourceAliasDrive\\"' in script
    assert '$app = Join-Path $buildProject "zephyr"' in script
    assert '$csConf = Join-Path $buildProject "ncs\\channel_sounding.conf"' in script
    assert 'Push-Location $buildProject' in script
    assert 'Push-Location $topDir' not in script

    zephyr_base_pos = script.index('$env:ZEPHYR_BASE = $zephyrBase')
    checkout_pos = script.index('Push-Location $buildProject')
    build_pos = script.index('Invoke-WestBuild $csBuild $true', checkout_pos)
    pop_pos = script.index('Pop-Location', build_pos)
    cleanup_pos = script.index('Remove-NoSpaceProjectAlias $sourceAliasDrive', pop_pos)
    assert zephyr_base_pos < checkout_pos < build_pos < pop_pos < cleanup_pos

    assert 'Remove-Item Env:ZEPHYR_BASE -ErrorAction SilentlyContinue' in script
    assert '$env:ZEPHYR_BASE = $originalZephyrBase' in script
