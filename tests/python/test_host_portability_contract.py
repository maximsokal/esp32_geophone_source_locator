from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def host_log_stubs() -> list[Path]:
    paths = list(REPO_ROOT.glob("components/*/tests/stubs/esp_log.h"))
    paths.extend(REPO_ROOT.glob("tests/host/stubs/**/esp_log.h"))
    return sorted(set(paths))


def test_host_log_stubs_avoid_gnu_variadic_comma_elision():
    offenders: list[str] = []

    for path in host_log_stubs():
        source = path.read_text(encoding="utf-8")
        if "##__VA_ARGS__" in source or "## __VA_ARGS__" in source:
            offenders.append(path.relative_to(REPO_ROOT).as_posix())

    assert not offenders, (
        "host C stubs must compile through the repository's strict C11/MSVC "
        "path without GNU variadic comma-elision: " + ", ".join(offenders)
    )


def test_msvc_host_runner_keeps_strict_c11_warning_gate():
    source = (REPO_ROOT / "tools" / "run_host_tests.py").read_text(encoding="utf-8")

    assert '"/std:c11"' in source
    assert '"/W4"' in source
    assert '"/WX"' in source
    assert '"/UNDEBUG"' in source
    assert '"/D_CRT_SECURE_NO_WARNINGS"' in source
    assert '"/Zc:preprocessor-"' not in source
