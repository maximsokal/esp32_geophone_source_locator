"""FreeRTOS timer callbacks run on the shared "Tmr Svc" task.

Field log 2026-10-25 20:16: "A stack overflow in task Tmr Svc has been
detected" rebooted the ESP32. The only xTimerCreate() callback formatted an
ESP_LOGW with seven arguments on that 2048-byte stack. Timer callbacks must
therefore stay tiny: no logging, formatting, allocation or blocking calls.
"""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (REPO_ROOT / "main", REPO_ROOT / "components")
SDKCONFIG_DEFAULTS = REPO_ROOT / "sdkconfig.defaults"
MIN_TIMER_TASK_STACK_BYTES = 2048

FORBIDDEN_IN_TIMER_CALLBACK = re.compile(
    r"\b(ESP_LOG[A-Z]*|ESP_EARLY_LOG[A-Z]*|esp_log_write|printf|snprintf|vsnprintf|"
    r"malloc|calloc|realloc|heap_caps_[a-z_]+|vTaskDelay|vTaskDelayUntil|"
    r"xSemaphoreTake|xQueueReceive|portMAX_DELAY)\b"
)
TIMER_CREATE = re.compile(r"\bxTimerCreate\s*\(")


def _production_sources():
    for root in SOURCE_ROOTS:
        for path in sorted(root.rglob("*.c")):
            if "tests" in path.relative_to(REPO_ROOT).parts:
                continue
            yield path


def _call_arguments(text: str, open_paren: int) -> list[str]:
    depth = 0
    args: list[str] = []
    start = open_paren + 1
    for index in range(open_paren, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                args.append(text[start:index].strip())
                return args
        elif char == "," and depth == 1:
            args.append(text[start:index].strip())
            start = index + 1
    raise AssertionError("unterminated call")


def _function_body(text: str, name: str) -> str | None:
    match = re.search(
        r"^[A-Za-z_][\w\s\*]*\b" + re.escape(name) + r"\s*\([^;{)]*\)\s*\{",
        text,
        re.MULTILINE,
    )
    if match is None:
        return None
    depth = 0
    for index in range(match.end() - 1, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[match.end():index]
    raise AssertionError("unterminated function " + name)


def timer_callback_violations(text: str) -> list[tuple[str, str]]:
    """Return (callback, forbidden call) pairs for every xTimerCreate callback in text."""
    found: list[tuple[str, str]] = []
    for call in TIMER_CREATE.finditer(text):
        callback = _call_arguments(text, call.end() - 1)[-1]
        body = _function_body(text, callback)
        assert body is not None, "callback %s of xTimerCreate has no definition in the same file" % callback
        for hit in FORBIDDEN_IN_TIMER_CALLBACK.finditer(body):
            found.append((callback, hit.group(1)))
    return found


def test_scanner_flags_logging_in_a_timer_callback():
    source = """
static void tick(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "x=%d", 1);
}
void init(void) { xTimerCreate("t", 10, pdTRUE, NULL, tick); }
"""
    assert timer_callback_violations(source) == [("tick", "ESP_LOGW")]


def test_scanner_accepts_a_quiet_timer_callback():
    source = """
static void tick(TimerHandle_t timer)
{
    (void)timer;
    (void)app_events_post_best_effort(1, 0);
}
void init(void) { xTimerCreate("t", 10, pdTRUE, NULL, tick); }
"""
    assert timer_callback_violations(source) == []


def test_no_freertos_timer_callback_logs_or_blocks():
    checked = 0
    violations = []
    for path in _production_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        if not TIMER_CREATE.search(text):
            continue
        checked += 1
        violations += [(path.name, *pair) for pair in timer_callback_violations(text)]
    assert checked >= 1, "status_indicator.c health timer should be covered by this scan"
    assert violations == [], violations


def test_timer_task_stack_is_not_reduced():
    depths = [
        int(value)
        for value in re.findall(
            r"^CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=(\d+)$",
            SDKCONFIG_DEFAULTS.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
    ]
    assert depths and min(depths) >= MIN_TIMER_TASK_STACK_BYTES
