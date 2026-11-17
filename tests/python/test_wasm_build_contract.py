"""The WebAssembly build of the firmware's calculation (tools/build_localization_wasm.py).

Board 2026-11-16: "the calculation stopped with error 258" on every real recording. The module had been built with the host
tests' trimmed cJSON.h found before the real one, while cJSON.c came from ESP-IDF: numbers and booleans still read, strings and
objects did not, so any settings naming a material or a velocity range were refused. The hand-made settings of the tests
named neither, which is why nothing failed. These checks pin the two build facts that caused it and the real-data test.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (ROOT / "tools/build_localization_wasm.py").read_text(encoding="utf-8")


def test_the_real_cjson_header_comes_before_the_host_test_stubs() -> None:
    flags = SCRIPT.index("include_flags = ")
    line = SCRIPT[flags:SCRIPT.index("\n", flags)]
    assert line.index('f"-I{cjson}"') < line.index("for i in includes"), line
    # The same header the module's cJSON.c comes from.
    assert 'cjson = IDF / "components/json/cJSON"' in SCRIPT
    assert 'str(cjson / "cJSON.c")' in SCRIPT


def test_the_build_refuses_a_module_with_functions_the_linker_did_not_find() -> None:
    assert "def foreign_imports(" in SCRIPT
    assert "missing = foreign_imports(output.read_bytes())" in SCRIPT
    assert "return 1" in SCRIPT[SCRIPT.index("missing = foreign_imports("):]


def test_a_real_board_card_is_part_of_the_wasm_tests() -> None:
    fixtures = ROOT / "webui/tests/wasm/fixtures/board-2026-11-16"
    for name in ("calc_settings.json", "S000022.session.json", "S000022.samples.bin"):
        assert (fixtures / name).is_file(), name
    test = (ROOT / "webui/tests/wasm/boardCard.test.ts").read_text(encoding="utf-8")
    # The settings of the real card name a material and a velocity range; the test must keep using them.
    settings = (fixtures / "calc_settings.json").read_text(encoding="utf-8")
    assert '"material":"concrete"' in settings and '"velocity_range"' in settings
    assert "calc_settings.json" in test and "S000022.samples.bin" in test
