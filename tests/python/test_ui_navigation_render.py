"""Real LVGL navigation, async ownership and reproducible application-key stress."""
import hashlib
import html
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import run_ui_render_tests as runner  # noqa: E402
import test_ui_render as pixels  # noqa: E402

CASES = runner.NAVIGATION_RUNS


@pytest.fixture(scope="module")
def navigation_root(tmp_path_factory):
    output = tmp_path_factory.mktemp("navigation-lvgl")
    yield output
    cards = []
    for metadata in sorted(output.glob("*/run.json")):
        run = json.loads(metadata.read_text(encoding="utf-8"))
        cards.append(f'<li><a href="{metadata.parent.name}/index.html">{html.escape(metadata.parent.name)}</a>: '
                     f'{run["status"]}, {run.get("frames", 0)} снимков, {run.get("actions", 0)} действий</li>')
    (output / "index.html").write_text(
        '<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>Geophone — переходы TFT</title><style>body{font:16px system-ui;padding:24px}li{margin:12px 0}</style>'
        '<h1>Переходы TFT: настоящий LVGL и реестр экранов</h1>'
        '<p>EN + UK · 320×240 RGB565 · управляемые ответы сервисов. Физические кнопки и TFT не проверены.</p>'
        '<ul>' + ''.join(cards) + '</ul></html>', encoding="utf-8")


@pytest.mark.parametrize("case,seed", CASES, ids=[f"{case}-{seed}" for case, seed in CASES])
def test_native_navigation(ui_render_binary, navigation_root, case, seed):
    binary = ui_render_binary.with_name("ui_navigation_render" + ui_render_binary.suffix)
    output = navigation_root / f"{case}-{seed}"
    frames = runner.navigate(binary, output, case, seed)
    screens = frames, output, binary
    for locale in ("en", "uk"):
        pixels.test_real_widget_bounds(screens, locale)
        pixels.test_real_font_and_text_fit(screens, locale)
        pixels.test_real_body_and_controls_do_not_overlap(screens, locale)
    run = json.loads((output / "run.json").read_text(encoding="utf-8"))
    assert run["status"] == "PASS" and run["actions"] > 0
    assert "screenshot_comparison" not in run["checks"]
    assert all(frame["comparison"] == -1 and not frame["golden"] for frame in frames)
    assert {frame["name"][:2] for frame in frames} == {"en", "uk"}
    if case == "replay":
        repeat = output / "repeat"
        repeated = runner.navigate(binary, repeat, case, seed)
        assert repeated == frames
        assert (repeat / "actions.jsonl").read_bytes() == (output / "actions.jsonl").read_bytes()
        for frame in frames:
            name = frame["name"] + ".png"
            assert hashlib.sha256((repeat / name).read_bytes()).digest() == hashlib.sha256((output / name).read_bytes()).digest()
