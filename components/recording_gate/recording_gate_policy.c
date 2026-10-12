#include "recording_gate.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif


recording_gate_result_t recording_gate_evaluate(
    size_t assigned_remote_count,
    size_t accepted_remote_count
)
{
    recording_gate_result_t result = {
        .decision = RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES,
        .assigned_count = assigned_remote_count,
        .accepted_count = accepted_remote_count,
        .dimensionality = LOCALIZATION_DIMENSIONALITY_INSUFFICIENT
    };

    const localization_dimensionality_t dimensionality =
        localization_dimensionality_classify(assigned_remote_count);
    result.dimensionality = dimensionality;

    if (dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT) {
        result.decision = RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES;
    } else if (accepted_remote_count < assigned_remote_count) {
        result.decision = RECORDING_GATE_BLOCKED_NOT_LEVEL;
    } else {
        result.decision =
            dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
            ? RECORDING_GATE_ALLOWED_PLANAR_2D
            : RECORDING_GATE_ALLOWED_3D_CAPABLE;
    }

    const bool diagnostic_allowed =
        result.decision == RECORDING_GATE_ALLOWED_PLANAR_2D ||
        result.decision == RECORDING_GATE_ALLOWED_3D_CAPABLE;
    result.start_allowed = diagnostic_allowed ||
        !CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING;
    result.bypassed = !diagnostic_allowed && result.start_allowed;
    return result;
}


const char *recording_gate_decision_reason(
    recording_gate_decision_t decision
)
{
    switch (decision) {
        case RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES:
            return
                "fewer than 3 REMOTE geophones are active/assigned: "
                "connect and level at least 3 to record";

        case RECORDING_GATE_BLOCKED_NOT_LEVEL:
            return
                "at least one connected REMOTE has not reported verticality "
                "ACCEPTED yet: level it or disconnect it before recording";

        case RECORDING_GATE_ALLOWED_PLANAR_2D:
        case RECORDING_GATE_ALLOWED_3D_CAPABLE:
        default:
            return "";
    }
}


const char *recording_gate_planar_warning(
    recording_gate_decision_t decision
)
{
    if (decision != RECORDING_GATE_ALLOWED_PLANAR_2D) {
        return "";
    }

    return
        "Only 3/4 remote geophones are active: localization uncertainty is "
        "higher and this recording is limited to planar/2D source "
        "localization. Connect at least one more REMOTE for full 3D "
        "localization.";
}


const char *recording_gate_bypass_reason(
    recording_gate_decision_t decision
)
{
    switch (decision) {
        case RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES:
            return "remotes_lt_3";
        case RECORDING_GATE_BLOCKED_NOT_LEVEL:
            return "no_leveling";
        case RECORDING_GATE_ALLOWED_PLANAR_2D:
        case RECORDING_GATE_ALLOWED_3D_CAPABLE:
        default:
            return "";
    }
}
