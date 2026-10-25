"""Execute the production widget builders against a geometry-recording LVGL fake."""

import json
import re
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _function(path: str, declaration: str) -> str:
    result = subprocess.run(
        [
            "ast-grep", "run", "--lang", "c", "--pattern",
            declaration + "($$$ARGS) { $$$BODY }",
            "--json=compact", str(ROOT / path),
        ],
        capture_output=True, text=True, check=True,
    )
    matches = json.loads(result.stdout)
    assert len(matches) == 1, declaration
    return matches[0]["text"]


C_FIXTURE = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_workflow_model.h"
#include "ui_theme.h"
#include "ui_utf8_format.h"
#include "device_status.h"
#include "ui_node_link_status.h"

typedef struct lv_obj_t {
    struct lv_obj_t *parent;
    int x, y, width, height, border;
    bool hidden;
    uint32_t color;
    char text[256];
} lv_obj_t;
static lv_obj_t objects[128];
static size_t object_count;
typedef struct {
    workflow_snapshot_t snapshot;
    ui_workflow_model_t model;
    lv_obj_t *screen;
    size_t line_slots;
    device_status_snapshot_t remote_status_snapshot;
    nrf_node_link_status_t preflight_links[UI_WORKFLOW_REMOTE_SLOT_COUNT + 1U];
    pod_state_t remote_pod_states[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    ui_workflow_remote_slot_t remote_slots[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    lv_obj_t *remote_slot_cards[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    lv_obj_t *remote_slot_labels[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    bool remote_slot_styles_dirty;
    lv_obj_t *action_rows[UI_WORKFLOW_VISIBLE_ACTIONS];
    lv_obj_t *action_labels[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_valid[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_visible[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_selected[UI_WORKFLOW_VISIBLE_ACTIONS];
    int action_row_cache_y[UI_WORKFLOW_VISIBLE_ACTIONS];
} ui_workflow_screen_state_t;

const char *ui_tr(ui_i18n_key_t key) { (void)key; return "Action"; }
static uint32_t lv_color_hex(uint32_t color) { return color; }
static lv_obj_t *ui_create_card_locked(lv_obj_t *parent, int x, int y, int w, int h)
{
    assert(object_count < sizeof(objects) / sizeof(objects[0]));
    lv_obj_t *obj = &objects[object_count++];
    *obj = (lv_obj_t){.parent=parent, .x=x, .y=y, .width=w, .height=h};
    return obj;
}
static lv_obj_t *ui_create_label_locked(lv_obj_t *parent, int x, int y, int w, const char *text, uint32_t color)
{
    lv_obj_t *obj = ui_create_card_locked(parent, x, y, w, 14);
    (void)snprintf(obj->text, sizeof(obj->text), "%s", text);
    obj->color = color;
    return obj;
}
static void lv_obj_set_pos(lv_obj_t *o, int x, int y) { o->x=x; o->y=y; }
static void lv_obj_update_layout(lv_obj_t *o) { (void)o; }
static int lv_obj_get_content_width(const lv_obj_t *o) { return o->width - 2*o->border; }
static void lv_obj_set_hidden(lv_obj_t *o, bool hidden) { o->hidden=hidden; }
static void lv_obj_set_style_bg_color(lv_obj_t *o, uint32_t color, int part) { (void)o; (void)color; (void)part; }
static void lv_obj_set_style_border_color(lv_obj_t *o, uint32_t color, int part) { (void)part; o->color=color; }
static void lv_obj_set_style_border_width(lv_obj_t *o, int width, int part) { (void)part; o->border=width; }
static void lv_obj_set_style_text_color(lv_obj_t *o, uint32_t color, int part) { (void)part; o->color=color; }
static const char *lv_label_get_text(const lv_obj_t *o) { return o->text; }
static void lv_label_set_text(lv_obj_t *o, const char *text) { (void)snprintf(o->text,sizeof(o->text),"%s",text); }
typedef uint32_t lv_color_t;
static bool lv_color_eq(lv_color_t a, lv_color_t b) { return a == b; }
static lv_color_t lv_obj_get_style_text_color(const lv_obj_t *o, int part) { (void)part; return o->color; }
static lv_color_t lv_obj_get_style_border_color(const lv_obj_t *o, int part) { (void)part; return o->color; }
static int lv_obj_get_style_border_width(const lv_obj_t *o, int part) { (void)part; return o->border; }

/* PRODUCTION_FUNCTIONS */

static void prepare(ui_workflow_screen_state_t *state, workflow_step_t step, uint8_t remotes)
{
    object_count = 0U;
    memset(state, 0, sizeof(*state));
    state->screen = ui_create_card_locked(NULL, 0, 0, 320, 240);
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = step;
    core.calibration_ok = true;
    core.ranging_ok = true;
    workflow_core_snapshot(&core, remotes, true, &state->snapshot);
    ui_workflow_model_state_t model_state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_build(&state->snapshot, &ranging, &model_state, 100U, &state->model);
    state->line_slots = ui_workflow_line_slots(&state->snapshot, &state->model);
}

static void check_action_geometry(void)
{
    ui_workflow_screen_state_t state;
    prepare(&state, WORKFLOW_STEP_SENSOR_CHECK, 1U);
    lv_obj_t *home = ui_create_row_locked(state.screen, 40, "Home action", true, true);
    assert(home->height >= 31);
    const int menu_height = home->height;
    const int menu_label_y = objects[object_count-2U].y;
    ui_workflow_create_action_slots(&state);
    ui_workflow_update_actions(&state);
    for (size_t i=0U; i<UI_WORKFLOW_VISIBLE_ACTIONS; ++i) {
        lv_obj_t *row = state.action_rows[i];
        assert(row->height == menu_height);
        assert(row->width == home->width);
        assert(state.action_labels[i]->y == menu_label_y);
        assert(state.action_labels[i]->y + state.action_labels[i]->height <= row->height);
    }
}

static void check_remote_grid(void)
{
    for (uint8_t mask=0U; mask<16U; ++mask) {
        ui_workflow_screen_state_t state;
        prepare(&state, WORKFLOW_STEP_SENSOR_CHECK, 1U);
        ui_workflow_remote_slot_layout(mask, state.remote_slots);
        for (size_t i=0U; i<4U; ++i) state.remote_pod_states[i] =
            (mask & (1U<<i)) != 0U ? POD_STATE_READY : POD_STATE_DISCONNECTED;
        ui_workflow_create_remote_slots(&state);
        ui_workflow_update_remote_slots(&state);
        for (size_t i=0U; i<4U; ++i) {
            lv_obj_t *card=state.remote_slot_cards[i];
            assert(card != NULL && !card->hidden && card->width >= 140 && card->height >= 31);
            assert(card->x >= 10 && card->x + card->width <= 310);
            assert(state.remote_slot_labels[i]->x + state.remote_slot_labels[i]->width <= card->width);
            assert(state.remote_slot_labels[i]->y + state.remote_slot_labels[i]->height <= card->height);
            assert(state.remote_slot_labels[i]->color ==
                (uint32_t)((mask & (1U<<i)) != 0U ? UI_COLOR_GOOD : UI_COLOR_MUTED));
        }
        assert(state.remote_slot_cards[0]->x == state.remote_slot_cards[2]->x);
        assert(state.remote_slot_cards[1]->x == state.remote_slot_cards[3]->x);
        assert(state.remote_slot_cards[0]->y == state.remote_slot_cards[1]->y);
        assert(state.remote_slot_cards[2]->y == state.remote_slot_cards[3]->y);
        assert(state.remote_slot_cards[0]->x + state.remote_slot_cards[0]->width < state.remote_slot_cards[1]->x);
        assert(state.remote_slot_cards[0]->y + state.remote_slot_cards[0]->height < state.remote_slot_cards[2]->y);
    }
}

static void check_all_steps(void)
{
    for (workflow_step_t step=WORKFLOW_STEP_IDLE; step<WORKFLOW_STEP_COUNT; ++step) {
        for (uint8_t remotes=0U; remotes<=4U; ++remotes) {
          for (size_t lines=0U; lines<=UI_WORKFLOW_MODEL_MAX_LINES; ++lines) {
            ui_workflow_screen_state_t state;
            prepare(&state, step, remotes);
            state.model.line_count=lines;
            state.line_slots=ui_workflow_line_slots(&state.snapshot, &state.model);
            ui_workflow_create_action_slots(&state);
            ui_workflow_create_remote_slots(&state);
            for (size_t selected=0U; selected<state.model.action_count; ++selected) {
                state.model.selected_action = selected;
                ui_workflow_update_actions(&state);
                int first_action_y=214;
                size_t selected_rows=0U;
                int previous_bottom=36;
                for (size_t i=0U; i<UI_WORKFLOW_VISIBLE_ACTIONS; ++i) {
                    const lv_obj_t *row=state.action_rows[i];
                    if (row->hidden) continue;
                    assert(row->height >= 31 && row->y >= 36 && row->y + row->height < 216);
                    assert(row->y >= previous_bottom);
                    previous_bottom=row->y + row->height;
                    if (row->y < first_action_y) first_action_y=row->y;
                    selected_rows += state.action_row_cache_selected[i] ? 1U : 0U;
                }
                assert(selected_rows == 1U);
                const int extra_line=step == WORKFLOW_STEP_RANGING && state.line_slots>1U
                    ? UI_WORKFLOW_LINE_HEIGHT : 0;
                assert(UI_WORKFLOW_LINE_TOP + (int)state.line_slots*UI_WORKFLOW_LINE_HEIGHT + extra_line <= first_action_y);
                for (size_t i=0U; i<4U; ++i) {
                    const lv_obj_t *card=state.remote_slot_cards[i];
                    if (card != NULL) assert(card->y + card->height < first_action_y);
                }
            }
          }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) return 0;
    if (strcmp(argv[1], "actions")==0) check_action_geometry();
    else if (strcmp(argv[1], "remotes")==0) check_remote_grid();
    else if (strcmp(argv[1], "steps")==0) check_all_steps();
    else return 2;
    return 0;
}
"""


@pytest.fixture(scope="module")
def layout_program():
    work = ROOT / ".test-build" / ("workflow-layout-" + uuid.uuid4().hex[:8])
    work.mkdir(parents=True)
    workflow = (ROOT / "components/ui/ui_workflow.c").read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define UI_WORKFLOW_.*$", workflow, re.M))
    declarations = [
        "static void ui_workflow_set_label",
        "static void ui_workflow_format_line",
        "static size_t ui_workflow_visible_actions",
        "static size_t ui_workflow_line_slots",
        "static void ui_workflow_create_action_slots",
        "static uint32_t ui_workflow_remote_pod_color",
        "static bool ui_workflow_remote_slots_visible",
        "static void ui_workflow_create_remote_slots",
        "static const char *ui_workflow_nrf_indicator",
        "static void ui_workflow_update_remote_slots",
        "static void ui_workflow_update_actions",
    ]
    functions = "\n".join(_function("components/ui/ui_workflow.c", d) for d in declarations)
    functions += "\n" + _function("components/ui/ui_shell.c", "lv_obj_t *ui_create_row_locked")
    source = work / "layout.c"
    source.write_text(defines + "\n" + C_FIXTURE.replace("/* PRODUCTION_FUNCTIONS */", functions), encoding="utf-8")
    entry = json.loads((ROOT / "tests/host/manifest.d/coverage-wfT.json").read_text())["tests"][0]
    entry["name"] = "workflow_widget_geometry"
    entry["include_dirs"].insert(0, "components/device_status/include")
    entry["include_dirs"].append("components/nrf_topology_service/include")
    entry["sources"] = [s for s in entry["sources"] if not s.endswith("/ui_workflow_model_test.c")] + [str(source)]
    manifest = work / "manifest.json"
    manifest.write_text(json.dumps({"version": 1, "tests": [entry]}), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools/run_host_tests.py"), "--manifest", str(manifest),
         "--build-dir", str(work / "build")],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return work / "build/workflow_widget_geometry" / ("workflow_widget_geometry.exe" if sys.platform == "win32" else "workflow_widget_geometry")


@pytest.mark.parametrize("case", ["actions", "remotes", "steps"])
def test_workflow_widget_geometry(layout_program, case):
    result = subprocess.run([str(layout_program), case], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
