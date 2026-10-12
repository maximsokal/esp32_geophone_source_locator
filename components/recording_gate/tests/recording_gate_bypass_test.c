#include <stdio.h>
#include <string.h>

#include "recording_gate.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

static unsigned s_failures;

static void check(bool condition, const char *case_name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", case_name);
        ++s_failures;
    }
}

int main(void)
{
#if CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
    for (size_t assigned = 0U; assigned < 3U; ++assigned) {
        const recording_gate_result_t result =
            recording_gate_evaluate(assigned, assigned);
        check(!result.start_allowed, "required gate rejects fewer than 3 remotes");
        check(!result.bypassed, "required gate does not mark blocked result bypassed");
        check(result.decision == RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES,
            "required gate keeps insufficient-remotes decision");
    }
    recording_gate_result_t result = recording_gate_evaluate(3U, 2U);
    check(!result.start_allowed, "required gate rejects unlevel remote");
    check(result.decision == RECORDING_GATE_BLOCKED_NOT_LEVEL,
        "required gate keeps not-level decision");
    result = recording_gate_evaluate(3U, 3U);
    check(result.start_allowed, "required gate accepts 3 leveled remotes");
    check(!result.bypassed, "required gate does not bypass valid conditions");
#else
    for (size_t assigned = 0U; assigned < 3U; ++assigned) {
        const recording_gate_result_t result =
            recording_gate_evaluate(assigned, assigned);
        check(result.start_allowed, "default bypass admits 0, 1, and 2 remotes");
        check(result.bypassed, "default bypass is recorded for insufficient remotes");
        check(result.decision == RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES,
            "default bypass retains insufficient-remotes diagnostic");
        check(strcmp(
            recording_gate_bypass_reason(result.decision), "remotes_lt_3") == 0,
            "default bypass emits insufficient-remotes reason token");
    }
    recording_gate_result_t result = recording_gate_evaluate(3U, 2U);
    check(result.start_allowed && result.bypassed,
        "default bypass admits unlevel remotes");
    check(result.decision == RECORDING_GATE_BLOCKED_NOT_LEVEL,
        "default bypass retains not-level diagnostic");
    check(strcmp(recording_gate_bypass_reason(result.decision), "no_leveling") == 0,
        "default bypass emits not-level reason token");
    result = recording_gate_evaluate(3U, 3U);
    check(result.start_allowed && !result.bypassed,
        "default policy admits valid conditions without bypass");
#endif

    if (s_failures != 0U) {
        fprintf(stderr, "%u recording gate checks failed\n", s_failures);
        return 1;
    }
    puts("recording gate policy tests passed");
    return 0;
}
