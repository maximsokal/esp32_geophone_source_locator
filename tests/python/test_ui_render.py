"""Pixel/layout tests of all production screens and menus on real LVGL."""
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys

from PIL import Image, ImageChops
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from run_ui_render_tests import build, render  # noqa: E402


@pytest.fixture(scope="module")
def screens(tmp_path_factory):
    output = tmp_path_factory.mktemp("real-lvgl")
    binary = build(ROOT / ".test-build/ui-render/native")
    frames = render(binary, output)
    assert len(frames) >= 190
    return frames, output, binary


def _frames(screens, locale):
    return [frame for frame in screens[0] if frame["name"].startswith(locale + "_")]


def _fail(errors):
    assert not errors, f"{len(errors)} screen violations:\n" + "\n".join(errors[:25])


def _visible(frame):
    return [node for node in frame["widgets"] if not node["hidden"]]


def _scroll_parent(frame, node):
    while node["parent"] >= 0:
        node = frame["widgets"][node["parent"]]
        if node.get("scroll_viewport") and node["clips_children"]:
            return node
    return None


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_real_widget_bounds(screens, locale):
    errors = []
    for frame in _frames(screens, locale):
        for node in _visible(frame):
            x, y, right, bottom = node["bounds"]
            parent = frame["widgets"][node["parent"]] if node["parent"] >= 0 else None
            scroll = _scroll_parent(frame, node)
            if not (0 <= x <= right < 320 and 0 <= y <= bottom < 240) and scroll is None:
                errors.append(f"{frame['name']} {node['role']} {node.get('text', '')!r}: {node['bounds']} outside 320x240")
            if parent:
                px, py, pr, pb = parent["bounds"]
                if not (px <= x <= right <= pr and py <= y <= bottom <= pb) and not parent.get("scroll_viewport"):
                    errors.append(f"{frame['name']} {node['role']} {node.get('text', '')!r}: {node['bounds']} outside parent {parent['bounds']}")
    _fail(errors)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_real_body_and_controls_do_not_overlap(screens, locale):
    errors = []
    for frame in _frames(screens, locale):
        nodes = [node for node in _visible(frame) if
                 node["role"].startswith(("line_", "remote_", "action_", "graphs", "progress"))
                 and not node["role"].startswith(("remote_text_", "action_text_"))
                 and node.get("text", "visible") != ""]
        for index, first in enumerate(nodes):
            ax, ay, ar, ab = first["bounds"]
            for second in nodes[index + 1:]:
                bx, by, br, bb = second["bounds"]
                if max(ax, bx) <= min(ar, br) and max(ay, by) <= min(ab, bb):
                    errors.append(f"{frame['name']}: {first['role']} {first['bounds']} overlaps {second['role']} {second['bounds']}")
    _fail(errors)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_real_font_and_text_fit(screens, locale):
    errors = []
    for frame in _frames(screens, locale):
        for node in _visible(frame):
            if "text" not in node:
                continue
            width = node["bounds"][2] - node["bounds"][0] + 1
            height = node["bounds"][3] - node["bounds"][1] + 1
            assert node["font_body"], (frame["name"], node)
            if node["natural"][1] > height:
                errors.append(f"{frame['name']} {node['role']} {node['text']!r}: text height {node['natural'][1]}px > {height}px")
            if node["natural"][0] > width and node["clips_text"]:
                errors.append(f"{frame['name']} {node['role']} {node['text']!r}: text {node['natural'][0]}px > {width}px, silently clipped")
    _fail(errors)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_actions_match_main_menu(screens, locale):
    frames = _frames(screens, locale)
    home = next(frame for frame in frames if frame["name"] == locale + "_home")
    menu = next(node for node in home["widgets"] if node["role"] == "home_0")
    width = menu["bounds"][2] - menu["bounds"][0] + 1
    height = menu["bounds"][3] - menu["bounds"][1] + 1
    assert (width, height) == (300, 31)
    steps = set()
    for frame in frames:
        if frame["step"] < 0:
            continue
        steps.add(frame["step"])
        rows = [node for node in _visible(frame) if node["role"].startswith("action_") and not node["role"].startswith("action_text_")]
        assert len(rows) > 0 or frame["actions"] == 0, frame["name"]
        selected = [row for row in rows if row["border"] == 2]
        assert len(selected) == (1 if rows else 0), frame["name"]
        for row in rows:
            assert row["bounds"][2] - row["bounds"][0] + 1 == width, (frame["name"], row)
            assert row["bounds"][3] - row["bounds"][1] + 1 == height, (frame["name"], row)
            expected = 0x21C7D9 if row["border"] == 2 else 0x1B2D46
            assert row["border_color"] & 0xFFFFFF == expected, (frame["name"], row)
    assert steps == set(range(19))


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_four_remotes_keep_grid_and_status_colors(screens, locale):
    for frame in _frames(screens, locale):
        if "_SENSOR_CHECK_" not in frame["name"]:
            continue
        cards = [node for node in _visible(frame) if node["role"].startswith("remote_") and not node["role"].startswith("remote_text_")]
        assert len(cards) == 4, frame["name"]
        for index, card in enumerate(cards):
            x, y, right, bottom = card["bounds"]
            assert (right - x + 1, bottom - y + 1) == (146, 31)
            assert x == cards[index % 2]["bounds"][0]
            assert y == cards[index // 2 * 2]["bounds"][1]
            expected = 0x4CD681 if index < frame["remotes"] else 0x94A7BC
            assert card["border_color"] & 0xFFFFFF == expected, (frame["name"], card)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_screens_match_reviewed_pngs(screens, locale):
    errors = []
    output = screens[1]
    for frame in _frames(screens, locale):
        if not frame["golden"] or frame["comparison"] == 1:
            continue
        name = frame["name"]
        reference = output / "references" / (name + ".png")
        if reference.exists():
            with Image.open(reference) as expected, Image.open(output / (name + ".png")) as actual:
                diff = ImageChops.difference(expected.convert("RGB"), actual.convert("RGB"))
                diff.save(output / (name + "_diff.png"))
            errors.append(f"{name}: pixel mismatch; actual, _err.png and _diff.png in {output}")
        else:
            errors.append(f"{name}: missing reference; candidate in {output}; explicit --update-goldens required")
    _fail(errors)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_long_instructions_really_scroll(screens, locale):
    frames = _frames(screens, locale)
    start = next(frame for frame in frames if frame["name"] == locale + "_long_text_start")
    body = next(node for node in start["widgets"] if node["role"] == "line_1")
    x, y, right, bottom = body["bounds"]
    with Image.open(screens[1] / (locale + "_long_text_start.png")) as initial, \
         Image.open(screens[1] / (locale + "_long_text_scrolled.png")) as later:
        area = (x, y, right + 1, bottom + 1)
        assert ImageChops.difference(initial.crop(area).convert("RGB"), later.crop(area).convert("RGB")).getbbox(), locale


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_remote_ready_warning_error_disconnected_colors(screens, locale):
    frame = next(frame for frame in _frames(screens, locale) if frame["name"] == locale + "_remote_status")
    for index, expected in enumerate([0x4CD681, 0xF3B84B, 0xF05A67, 0x94A7BC]):
        card = next(node for node in frame["widgets"] if node["role"] == f"remote_{index}")
        assert card["border_color"] & 0xFFFFFF == expected


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_determinate_progress_widgets_are_visible(screens, locale):
    for suffix in ("_calculating_progress", "_ranging_progress"):
        frame = next(frame for frame in _frames(screens, locale) if frame["name"] == locale + suffix)
        progress = next(node for node in frame["widgets"] if node["role"] == "progress")
        assert not progress["hidden"], frame["name"]


MENU_FAMILIES = ["home", "objects", "storage", "wifi", "system", "hardware", "measurements", "boot", "preview"]


def test_every_registered_screen_and_menu_has_both_locales(screens):
    source = (ROOT / "components/ui/ui_internal.h").read_text(encoding="utf-8")
    enum = re.search(r"typedef enum\s*\{([^}]+)\}\s*ui_screen_t", source).group(1)
    names = re.findall(r"\bUI_SCREEN_(?!COUNT\b)\w+", enum)
    for locale in ("en", "uk"):
        frames = _frames(screens, locale)
        assert {frame["screen"] for frame in frames} == set(range(len(names)))
        assert {frame["family"] for frame in frames} == set(MENU_FAMILIES + ["workflow"])
        assert all(frame["golden"] for frame in frames if "_menu_" in frame["name"])


def test_menu_state_enums_have_rendered_examples(screens):
    cases = [
        ("components/ui/ui_internal.h", "ui_objects_mode_t", "UI_OBJECTS_", "objects", "objects_mode"),
        ("components/ui/ui_internal.h", "ui_wifi_confirm_t", "UI_WIFI_CONFIRM_", "wifi", "wifi_confirm"),
        ("components/measurement_service/include/measurement_service.h", "measurement_service_state_t", "MEASUREMENT_SERVICE_", "measurements", "measurement_state"),
        ("components/scan_preview/include/scan_preview_state.h", "scan_preview_screen_status_t", "SCAN_PREVIEW_SCREEN_", "preview", "preview_status"),
    ]
    for path, enum_name, prefix, family, field in cases:
        source = (ROOT / path).read_text(encoding="utf-8")
        enum = re.search(r"typedef enum\s*\{([^}]+)\}\s*" + enum_name, source).group(1)
        names = re.findall(r"\b" + prefix + r"\w+", enum)
        for locale in ("en", "uk"):
            actual = {frame[field] for frame in _frames(screens, locale) if frame["family"] == family}
            assert actual == set(range(len(names))), (family, locale, names, actual)


def test_gallery_has_filters_and_every_reference_image(screens):
    from html.parser import HTMLParser

    class Gallery(HTMLParser):
        def __init__(self):
            super().__init__()
            self.figures = []
            self.filters = set()
            self.images = []

        def handle_starttag(self, tag, attributes):
            attrs = dict(attributes)
            if tag == "figure": self.figures.append(attrs)
            if tag in ("select", "input") and "id" in attrs: self.filters.add(attrs["id"])
            if tag == "img": self.images.append(attrs)

    gallery = Gallery()
    gallery.feed((screens[1] / "index.html").read_text(encoding="utf-8"))
    assert gallery.filters == {"family", "language", "changed"}
    expected = {frame["name"] + ".png" for frame in screens[0] if frame["golden"]}
    assert {image["src"] for image in gallery.images} == expected
    assert len(gallery.figures) == len(expected)
    assert {figure["data-family"] for figure in gallery.figures} == set(MENU_FAMILIES + ["workflow"])
    assert all(image["width"] == "320" and image["height"] == "240" for image in gallery.images)


@pytest.mark.parametrize("family", MENU_FAMILIES)
@pytest.mark.parametrize("locale", ["en", "uk"])
def test_menu_siblings_do_not_cover_each_other(screens, family, locale):
    errors = []
    for frame in _frames(screens, locale):
        if frame["family"] != family:
            continue
        nodes = [node for node in _visible(frame) if node["parent"] >= 0
                 and node["role"] != "preview_canvas" and node.get("text", "visible") != ""]
        for index, first in enumerate(nodes):
            ax, ay, ar, ab = first["bounds"]
            for second in nodes[index + 1:]:
                if first["parent"] != second["parent"]:
                    continue
                bx, by, br, bb = second["bounds"]
                if max(ax, bx) <= min(ar, br) and max(ay, by) <= min(ab, bb):
                    errors.append(f"{frame['name']}: {first.get('text', first['kind'])!r} {first['bounds']} covers {second.get('text', second['kind'])!r} {second['bounds']}")
    _fail(errors)


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_live_diagnostics_are_reachable_by_real_keys(screens, locale):
    frames = _frames(screens, locale)
    top = next(frame for frame in frames if frame["name"] == locale + "_menu_measurements_state0_a0")
    bottom = next(frame for frame in frames if frame["name"] == locale + "_menu_measurements_state0_a0_bottom")
    returned = next(frame for frame in frames if frame["name"] == locale + "_menu_measurements_returned_top")
    for frame, scrolled in [(top, False), (bottom, True), (returned, False)]:
        viewport = next(node for node in frame["widgets"] if node["role"] == "measurement_content")
        assert viewport["scroll_viewport"] and viewport["clips_children"]
        assert (viewport["scroll_y"] > 0) == scrolled
    viewport = next(node for node in bottom["widgets"] if node["role"] == "measurement_content")
    for index in range(5):
        node = next(node for node in bottom["widgets"] if node["role"] == f"measurement_node_{index}")
        assert viewport["bounds"][1] <= node["bounds"][1] <= node["bounds"][3] <= viewport["bounds"][3]
    assert next(node for node in top["widgets"] if node["role"] == "measurement_health")["bounds"] == \
        next(node for node in bottom["widgets"] if node["role"] == "measurement_health")["bounds"]


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_delete_confirmation_is_reachable_and_cancel_does_not_delete(screens, locale):
    frames = _frames(screens, locale)
    confirmed = next(frame for frame in frames if frame["name"] == locale + "_menu_objects_key_delete_confirmation")
    canceled = next(frame for frame in frames if frame["name"] == locale + "_menu_objects_key_delete_cancel")
    assert confirmed["objects_mode"] == 4, "OK in the delete list never opens its confirmation screen"
    assert canceled["objects_mode"] == 2
    assert confirmed["delete_calls"] == canceled["delete_calls"] == 0


def test_screenshot_comparison_rejects_changed_and_missing_reference(screens, tmp_path):
    """Exercise LVGL's comparator, including its no-auto-accept failure path."""
    output = tmp_path
    shutil.copytree(screens[1] / "references", output / "references")
    references = output / "references"
    reference = references / "en_home.png"
    if not reference.exists():
        pytest.fail("Initial reference set must be explicitly created before the comparator self-check")
    original = reference.read_bytes()
    try:
        with Image.open(reference) as source:
            changed = source.convert("RGBA")
            changed.putpixel((0, 0), (255, 0, 255, 255))
            changed.save(reference)
        result = subprocess.run([str(screens[2]), "."], cwd=output, capture_output=True, timeout=90, check=True)
        (tmp_path / "changed-reference.log").write_bytes(result.stdout + result.stderr)
        frames = [json.loads(line) for line in (output / "frames.jsonl").read_text(encoding="utf-8").splitlines()]
        assert next(frame for frame in frames if frame["name"] == "en_home")["comparison"] == 0
        assert (references / "en_home_err.png").exists()
        reference.unlink()
        subprocess.run([str(screens[2]), "."], cwd=output, capture_output=True, timeout=90, check=True)
        frames = [json.loads(line) for line in (output / "frames.jsonl").read_text(encoding="utf-8").splitlines()]
        assert next(frame for frame in frames if frame["name"] == "en_home")["comparison"] == 2
        assert not reference.exists(), "LVGL silently accepted a missing reference"
    finally:
        reference.write_bytes(original)
