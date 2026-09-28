import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _production_sources():
    return [
        path
        for path in [*ROOT.glob("components/**/*.c"), *ROOT.glob("main/**/*.c")]
        if "tests" not in path.parts
    ]


def test_gpio_isr_service_has_one_iram_owner_and_iram_callbacks():
    install_sites = []
    registrations = []
    for path in _production_sources():
        source = path.read_text(encoding="utf-8")
        if re.search(r"\bgpio_install_isr_service\s*\(", source):
            install_sites.append(path.relative_to(ROOT).as_posix())
        for match in re.finditer(
            r"\bgpio_isr_handler_add\s*\(\s*[^,]+,\s*([A-Za-z_]\w*)\s*,",
            source,
            re.DOTALL,
        ):
            registrations.append((path, source, match.group(1)))

    assert install_sites == ["components/board_config/board_config.c"]
    assert registrations
    for path, source, callback in registrations:
        definition = re.compile(
            rf"\b(?:static\s+)?void\s+IRAM_ATTR\s+{re.escape(callback)}\s*\("
        )
        assert definition.search(source), (
            f"{path.relative_to(ROOT)} registers {callback} without IRAM_ATTR"
        )


def test_iram_service_is_installed_before_application_startup():
    source = (ROOT / "main/main.c").read_text(encoding="utf-8")
    install = source.index("board_gpio_isr_service_install()")
    startup = source.index("app_startup_run()")
    assert install < startup
