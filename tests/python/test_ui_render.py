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
from run_ui_render_tests import render  # noqa: E402
import run_ui_render_tests as runner  # noqa: E402


@pytest.fixture(scope="module")
def screens(tmp_path_factory, ui_render_binary):
    output = tmp_path_factory.mktemp("real-lvgl")
    binary = ui_render_binary
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
    assert steps == set(range(21))


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


@pytest.mark.parametrize("locale", ["en", "uk"])
@pytest.mark.parametrize("suffix,nodes,done,total,percent", [
    ("", 5, 10, 20, "50"),
    ("_3_nodes", 3, 3, 6, "50"),
    ("_4_nodes", 4, 3, 12, "25"),
    ("_5_nodes", 5, 3, 20, "15"),
])
def test_ranging_progress_percent_grows_with_the_number_of_nodes(screens, locale, suffix, nodes, done, total, percent):
    entries = json.loads((ROOT / "i18n/tft.json").read_text(encoding="utf-8"))["entries"]
    assert total == nodes * (nodes - 1), "the plan holds one execution per ordered pair"
    frame = next(frame for frame in _frames(screens, locale) if frame["name"] == f"{locale}_ranging_progress{suffix}")
    assert frame["golden"] and frame["comparison"] == 1, frame["name"]
    assert (ROOT / "tests/ui_render/golden" / (frame["name"] + ".png")).is_file()
    lines = [node["text"] for node in _visible(frame) if node["role"].startswith("line_")]
    assert entries["tft.workflow.ranging.pairs"][locale] % (str(done), str(total), _cm(12)) == lines[0]
    # The plan holds one execution per direction, so n*(n-1) counts measurements; the pairs are n*(n-1)/2.
    assert lines[0].startswith("Measurements " if locale == "en" else "Виміри "), lines[0]
    assert entries["tft.workflow.ranging.wait_percent"][locale] % percent == lines[1]
    assert entries["tft.workflow.ranging.wait"][locale] not in lines
    assert next(node for node in frame["widgets"] if node["role"] == "progress")["hidden"] is False


@pytest.mark.parametrize("locale", ["en", "uk"])
@pytest.mark.parametrize("outcome", ["success", "partial", "failed", "cable_exceeded"])
def test_ranging_outcomes_have_reviewed_pages_and_exact_pair_values(screens, locale, outcome):
    entries = json.loads((ROOT / "i18n/tft.json").read_text(encoding="utf-8"))["entries"]
    prefix = f"{locale}_ranging_result_{outcome}_page"
    pages = [frame for frame in _frames(screens, locale) if frame["name"].startswith(prefix)]
    assert len(pages) > 1, "All pair pages must be captured, not only the first screen"
    assert [frame["name"] for frame in pages] == [prefix + str(i + 1) for i in range(len(pages))]
    for frame in pages:
        assert frame["golden"] and frame["comparison"] == 1, frame["name"]
        assert (ROOT / "tests/ui_render/golden" / (frame["name"] + ".png")).is_file()
    status = "partial" if outcome == "cable_exceeded" else outcome
    first_lines = [node["text"] for node in _visible(pages[0]) if node["role"].startswith("line_")]
    assert first_lines[0] == entries[f"tft.workflow.ranging.{status}"][locale]
    texts = [node["text"] for frame in pages for node in _visible(frame) if node["role"].startswith("line_")]
    pair_lines = [text for text in texts if re.match(r"^[0-4]-[0-4]:", text)]
    expected = []
    for a in range(4):
        for b in range(a + 1, 5):
            index = len(expected)
            missing = outcome == "failed" or (outcome == "partial" and index == 3)
            exceeded = outcome == "cable_exceeded" and index == 3
            value = entries["tft.workflow.value.unknown"][locale] if missing else _cm(6001 if exceeded else 1234 + index)
            key = "cable_exceeded" if exceeded else "distance"
            expected.append(entries[f"tft.workflow.ranging.{key}"][locale] % (str(a), str(b), value))
    assert pair_lines == expected
    action = "confirm" if outcome == "success" else "retry"
    for frame in pages:
        assert entries[f"tft.workflow.action.{action}"][locale] in [
            node["text"] for node in _visible(frame) if node["role"].startswith("action_text_")]
        assert entries["tft.workflow.hint.ranging_pages"][locale] in [
            node["text"] for node in _visible(frame) if node["role"] == "footer"]


def _cm(millimetres):
    """The TFT shows ranging values in whole centimetres, rounded to the nearest (halves up for positives)."""
    return str((millimetres + 5) // 10)


MENU_FAMILIES = ["home", "objects", "storage", "wifi", "system", "hardware", "measurements", "boot", "preview"]


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_manual_distance_entry_rejection_saving_and_completed_pages(screens, locale):
    entries = json.loads((ROOT / "i18n/tft.json").read_text(encoding="utf-8"))["entries"]
    captures = {frame["name"]: frame for frame in _frames(screens, locale) if "_ranging_manual_" in frame["name"]}
    for suffix in ("pair", "value", "rejected", "saving"):
        frame = captures[f"{locale}_ranging_manual_{suffix}"]
        assert frame["golden"] and frame["comparison"] == 1
    def texts(frame):
        return [node.get("text", "") for node in _visible(frame)]
    assert entries["tft.workflow.distance.pair"][locale] % ("0", "4") in texts(captures[f"{locale}_ranging_manual_pair"])
    assert entries["tft.workflow.distance.value"][locale] % "6.0" in texts(captures[f"{locale}_ranging_manual_value"])
    assert entries["tft.workflow.distance.rejected"][locale] in texts(captures[f"{locale}_ranging_manual_rejected"])
    assert entries["tft.workflow.distance.saving"][locale] in texts(captures[f"{locale}_ranging_manual_saving"])
    saved = [frame for name, frame in captures.items() if "_saved_page" in name]
    assert len(saved) > 1
    assert all(frame["golden"] and frame["comparison"] == 1 for frame in saved)
    assert entries["tft.workflow.ranging.success"][locale] in texts(saved[0])
    assert entries["tft.workflow.distance.manual"][locale] % ("0", "4", _cm(1500)) in [text for frame in saved for text in texts(frame)]
    assert not any("cable_exceeded" in text for frame in saved for text in texts(frame))


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
        if "_critical_battery_" in frame["name"]:
            continue  # the red bar covers the footer on purpose; test_critical_battery_stop_is_shown checks it
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


def test_reused_report_rejects_removed_source_reference(screens, tmp_path, monkeypatch):
    """Run the actual LVGL binary twice through the runner, using isolated goldens."""
    import hashlib

    source = tmp_path / "golden"
    shutil.copytree(runner.GOLDENS, source)
    monkeypatch.setattr(runner, "GOLDENS", source)
    output = tmp_path / "report"
    first = runner.render(screens[2], output)
    assert all(frame["comparison"] == 1 for frame in first if frame["golden"])
    identity = json.loads((output / "run.json").read_text(encoding="utf-8"))
    assert identity["status"] == "PASS"
    assert identity["build"]["lvgl"]["version"] == "9.6.0"
    assert identity["build"]["font_sha256"] == hashlib.sha256(
        (ROOT / "components/ui/fonts/ui_font_montserrat_14.c").read_bytes()).hexdigest()
    assert identity["build"]["binary_sha256"] == hashlib.sha256(screens[2].read_bytes()).hexdigest()
    assert identity["build"]["compiler"]["sha256"]
    assert identity["build"]["source"]["files"] > 0
    assert identity["build"]["configuration"]["tests/ui_render/lv_conf.h"] == hashlib.sha256(
        (ROOT / "tests/ui_render/lv_conf.h").read_bytes()).hexdigest()
    (source / "en_home.png").unlink()

    second = runner.render(screens[2], output)
    assert next(frame for frame in second if frame["name"] == "en_home")["comparison"] == 2
    assert not (output / "references/en_home.png").exists()
    assert (ROOT / "tests/ui_render/golden/en_home.png").is_file()
    identity = json.loads((output / "run.json").read_text(encoding="utf-8"))
    assert identity["status"] == "FAIL" and identity["comparisons"]["missing"] == 1


def _tft(key, locale):
    catalog = json.loads((ROOT / "i18n/tft.json").read_text(encoding="utf-8"))
    entries = catalog["entries"]
    return entries[key][locale]


def _bar_text_nodes(frame):
    """Visible red-bar labels: a label whose parent is the 24 px bar over the footer."""
    nodes = []
    for node in frame["widgets"]:
        if node.get("kind") != "label" or node["hidden"] or node["parent"] < 0:
            continue
        parent = frame["widgets"][node["parent"]]
        if (parent["bg"] & 0xFFFFFF) == 0xF05A67 and parent["bounds"] == [0, 216, 319, 239]:
            nodes.append((node, parent))
    return nodes


@pytest.mark.parametrize("locale", ["en", "uk"])
def test_critical_battery_stop_is_shown_on_the_device_screen(screens, locale):
    """Critical charge: the TFT says it is stopping on every screen, in both languages."""
    stop = _tft("power.banner_stop", locale)
    grace = _tft("power.banner_grace", locale).replace("%u", "8")
    frames = [f for f in _frames(screens, locale) if f["name"].startswith(f"{locale}_critical_battery_")]
    names = {f["name"] for f in frames}
    for scene in ("ranging", "calculating", "recording"):
        for phase in ("grace", "stopping", "cleared"):
            assert f"{locale}_critical_battery_{scene}_{phase}" in names
    for phase in ("grace", "stopping", "cleared"):
        assert f"{locale}_critical_battery_ranging_{phase}_after_read_failure" in names
    assert f"{locale}_critical_battery_ranging_cleared_after_recovery" in names
    assert f"{locale}_critical_battery_home_stopping" in names
    errors = []
    for frame in frames:
        shown = _bar_text_nodes(frame)
        name = frame["name"]
        if "_cleared" in name:
            if shown:
                errors.append(f"{name}: bar still visible although the battery is not critical: {shown[0][0]['text']!r}")
            continue
        expected = grace if "_grace" in name else stop
        if len(shown) != 1:
            errors.append(f"{name}: expected exactly one visible critical bar, got {len(shown)}")
            continue
        label, bar = shown[0]
        if label["text"] != expected:
            errors.append(f"{name}: bar says {label['text']!r}, expected {expected!r}")
        if label["natural"][0] > label["bounds"][2] - label["bounds"][0] + 1:
            errors.append(f"{name}: bar text {label['natural'][0]}px does not fit and would scroll")
        # Above every other widget of the screen, so no screen can hide it.
        bar_index = frame["widgets"].index(bar)
        siblings = [i for i, n in enumerate(frame["widgets"]) if n["parent"] == bar["parent"]]
        if bar_index != max(siblings):
            errors.append(f"{name}: a widget is drawn over the critical bar")
        badge = next(n for n in frame["widgets"] if n["role"] == "header_power")
        if "_grace" not in name and badge["text"].split()[-1] not in {"OFF", "ВИМК."}:
            errors.append(f"{name}: header badge {badge['text']!r} does not show the power-off state")
    _fail(errors)
