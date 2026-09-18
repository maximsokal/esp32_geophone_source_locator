from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NCS_PS1 = ROOT / "nRF54L15" / "ncs.ps1"


def test_ncs_wrapper_uses_no_space_alias_and_anchors_workspace_with_zephyr_base():
    script = NCS_PS1.read_text(encoding="utf-8")

    assert "function Test-IsWindows" in script
    assert "[System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(" in script
    assert "[System.Runtime.InteropServices.OSPlatform]::Windows" in script
    assert "$env:OS" not in script
    assert "if (-not (Test-IsWindows) -or $ProjectPath -notmatch '\\s')" in script
    assert "if (Test-IsWindows) {" in script

    assert '$zephyrBase = Join-Path $topDir "zephyr"' in script
    assert '$originalZephyrBase = $env:ZEPHYR_BASE' in script
    assert '$env:ZEPHYR_BASE = $zephyrBase' in script

    assert 'function New-NoSpaceProjectAlias' in script
    assert 'function Remove-NoSpaceProjectAlias' in script
    assert "$ProjectPath -notmatch '\\s'" in script
    assert 'subst {0} "{1}"' in script
    assert '$sourceAliasDrive = New-NoSpaceProjectAlias $project' in script
    assert '$buildProject = "$sourceAliasDrive\\"' in script
    assert '$app = Join-Path $buildProject "zephyr"' in script
    assert '$csConf = Join-Path $buildProject "ncs\\channel_sounding.conf"' in script
    assert 'Push-Location $buildProject' in script
    assert 'Push-Location $topDir' not in script

    zephyr_base_pos = script.index('$env:ZEPHYR_BASE = $zephyrBase')
    checkout_pos = script.index('Push-Location $buildProject')
    production_build_pos = script.index('Invoke-WestBuild $productionBuild $false', checkout_pos)
    pop_pos = script.index('Pop-Location', production_build_pos)
    cleanup_pos = script.index('Remove-NoSpaceProjectAlias $sourceAliasDrive', pop_pos)
    assert zephyr_base_pos < checkout_pos < production_build_pos < pop_pos < cleanup_pos

    assert 'Remove-Item Env:ZEPHYR_BASE -ErrorAction SilentlyContinue' in script
    assert '$env:ZEPHYR_BASE = $originalZephyrBase' in script


def test_ncs_wrapper_probes_production_and_cs_proof_artifacts_inside_sysbuild():
    script = NCS_PS1.read_text(encoding="utf-8")

    assert 'function Get-ZephyrAppArtifactDir' in script
    assert '$sysbuildAppDir = Join-Path $BuildDir "zephyr\\zephyr"' in script
    assert 'return Join-Path $BuildDir "zephyr"' in script

    production_pos = script.index('function Probe-ProductionBuild')
    production = script[production_pos:script.index('function Probe-CsBuild', production_pos)]
    assert '$appZephyrDir = Get-ZephyrAppArtifactDir $productionBuild' in production
    assert '$configPath = Join-Path $appZephyrDir ".config"' in production
    assert '$elf = Join-Path $appZephyrDir "zephyr.elf"' in production

    probe_pos = script.index('function Probe-CsBuild')
    proof = script[probe_pos:script.index('function Show-Size', probe_pos)]
    assert '$appZephyrDir = Get-ZephyrAppArtifactDir $csProofBuild' in proof
    assert '$configPath = Join-Path $appZephyrDir ".config"' in proof
    assert '$elf = Join-Path $appZephyrDir "zephyr.elf"' in proof

    size_pos = script.index('function Show-Size')
    size = script[size_pos:script.index('function Invoke-SerialMonitor', size_pos)]
    assert '$appZephyrDir = Get-ZephyrAppArtifactDir $BuildDir' in size
    assert '$elf = Join-Path $appZephyrDir "zephyr.elf"' in size
