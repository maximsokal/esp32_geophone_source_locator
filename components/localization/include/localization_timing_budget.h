#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * MAX-142: turns a measured or assumed arrival-time jitter bound into a
 * concrete position-error bound for a specific session's geometry,
 * *without* claiming to know the real hardware's actual jitter -- that
 * number can only come from a real measurement (a logic analyzer/scope on
 * CONVST, or in-firmware scheduled-vs-actual timestamp instrumentation).
 * This is the empirical decision tool the ticket's acceptance criterion
 * ("quantify resulting location error bounds... do not switch timing
 * mechanisms from intuition alone") calls for: feed it a real measured
 * jitter_us and this session's real geometry, and it reports what that
 * jitter actually costs in meters for *this* deployment -- not a generic
 * textbook number.
 *
 * Method: solves the exact-arrival-time baseline once, then perturbs every
 * observation's arrival_time_us by +/-jitter_us in an alternating pattern
 * (maximizing relative inter-channel TDOA distortion) and re-solves. The
 * distance between the two solved positions is the reported bound. This is
 * an empirical perturbation study against the real nonlinear solver, not a
 * closed-form linearized error-propagation formula.
 */
esp_err_t localization_timing_jitter_error_bound(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    int64_t jitter_us,
    double *position_error_m_out
);
