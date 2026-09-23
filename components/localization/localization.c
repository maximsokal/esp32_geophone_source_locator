#include "localization.h"

#include <float.h>
#include <math.h>
#include <string.h>


#define LOCALIZATION_DEFAULT_ITERATIONS       20
#define LOCALIZATION_DEFAULT_CONVERGENCE_M  0.001
#define LOCALIZATION_DEFAULT_DAMPING         1.0e-6
#define LOCALIZATION_DISTANCE_EPSILON_M      1.0e-9
/*
 * MAX-211: minimum |6 x tetrahedron volume| / scale^3 for the sensor layout
 * to resolve depth. The old 1e-6 accepted any barometric noise as "3D": a
 * flat 5x5 m layout with 5 cm of Z noise scores ~0.0035 and would have let
 * noise determine the depth. 0.05 needs roughly >= 0.7 m of vertical relief
 * across a 5 m array (a regular tetrahedron scores ~0.71).
 */
#define LOCALIZATION_MIN_DEPTH_RESOLVING_VOLUME_RATIO 0.05


static localization_point3d_t point_subtract(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    return (localization_point3d_t) {
        .x_m = a.x_m - b.x_m,
        .y_m = a.y_m - b.y_m,
        .z_m = a.z_m - b.z_m,
    };
}


static double point_dot(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    return
        a.x_m * b.x_m +
        a.y_m * b.y_m +
        a.z_m * b.z_m;
}


static localization_point3d_t point_cross(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    return (localization_point3d_t) {
        .x_m = a.y_m * b.z_m - a.z_m * b.y_m,
        .y_m = a.z_m * b.x_m - a.x_m * b.z_m,
        .z_m = a.x_m * b.y_m - a.y_m * b.x_m,
    };
}


static bool point_is_finite(localization_point3d_t point)
{
    return
        isfinite(point.x_m) &&
        isfinite(point.y_m) &&
        isfinite(point.z_m);
}


static double point_distance(
    localization_point3d_t a,
    localization_point3d_t b
)
{
    const localization_point3d_t delta = point_subtract(a, b);
    return sqrt(point_dot(delta, delta));
}


static double localization_geometry_scale(
    const localization_observation_t *observations,
    size_t count
)
{
    double maximum = 0.0;

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            const double distance = point_distance(
                observations[i].sensor_position,
                observations[j].sensor_position
            );

            if (distance > maximum) {
                maximum = distance;
            }
        }
    }

    return maximum;
}


static bool localization_geometry_is_3d(
    const localization_observation_t *observations,
    size_t count,
    double scale
)
{
    if (scale <= LOCALIZATION_DISTANCE_EPSILON_M) {
        return false;
    }

    const localization_point3d_t origin =
        observations[0].sensor_position;

    double max_six_volume = 0.0;

    for (size_t i = 1; i < count; ++i) {
        const localization_point3d_t a = point_subtract(
            observations[i].sensor_position,
            origin
        );

        for (size_t j = i + 1; j < count; ++j) {
            const localization_point3d_t b = point_subtract(
                observations[j].sensor_position,
                origin
            );

            for (size_t k = j + 1; k < count; ++k) {
                const localization_point3d_t c = point_subtract(
                    observations[k].sensor_position,
                    origin
                );

                const double six_volume = fabs(
                    point_dot(a, point_cross(b, c))
                );

                if (six_volume > max_six_volume) {
                    max_six_volume = six_volume;
                }
            }
        }
    }

    return max_six_volume >
        LOCALIZATION_MIN_DEPTH_RESOLVING_VOLUME_RATIO * scale * scale * scale;
}


static localization_point3d_t localization_centroid(
    const localization_observation_t *observations,
    size_t count
)
{
    localization_point3d_t centroid = {0};

    for (size_t i = 0; i < count; ++i) {
        centroid.x_m += observations[i].sensor_position.x_m;
        centroid.y_m += observations[i].sensor_position.y_m;
        centroid.z_m += observations[i].sensor_position.z_m;
    }

    const double inverse = 1.0 / (double)count;
    centroid.x_m *= inverse;
    centroid.y_m *= inverse;
    centroid.z_m *= inverse;

    return centroid;
}


static bool solve_3x3(
    double matrix[3][3],
    double rhs[3],
    double solution[3]
)
{
    double augmented[3][4];

    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            augmented[row][column] = matrix[row][column];
        }
        augmented[row][3] = rhs[row];
    }

    for (size_t pivot = 0; pivot < 3; ++pivot) {
        size_t best_row = pivot;
        double best_value = fabs(augmented[pivot][pivot]);

        for (size_t row = pivot + 1; row < 3; ++row) {
            const double value = fabs(augmented[row][pivot]);
            if (value > best_value) {
                best_value = value;
                best_row = row;
            }
        }

        if (best_value <= DBL_EPSILON) {
            return false;
        }

        if (best_row != pivot) {
            for (size_t column = pivot; column < 4; ++column) {
                const double temporary = augmented[pivot][column];
                augmented[pivot][column] = augmented[best_row][column];
                augmented[best_row][column] = temporary;
            }
        }

        const double divisor = augmented[pivot][pivot];
        for (size_t column = pivot; column < 4; ++column) {
            augmented[pivot][column] /= divisor;
        }

        for (size_t row = 0; row < 3; ++row) {
            if (row == pivot) {
                continue;
            }

            const double factor = augmented[row][pivot];
            for (size_t column = pivot; column < 4; ++column) {
                augmented[row][column] -=
                    factor * augmented[pivot][column];
            }
        }
    }

    for (size_t row = 0; row < 3; ++row) {
        solution[row] = augmented[row][3];
    }

    return true;
}


static esp_err_t localization_validate(
    const localization_observation_t *observations,
    size_t count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_result_t *result,
    double *geometry_scale
)
{
    if (
        observations == NULL ||
        config == NULL ||
        result == NULL ||
        geometry_scale == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        count < LOCALIZATION_MIN_SENSORS_3D ||
        count > LOCALIZATION_MAX_SENSORS ||
        !isfinite(config->propagation_speed_mps) ||
        config->propagation_speed_mps <= 0.0 ||
        config->max_iterations == 0 ||
        !isfinite(config->convergence_m) ||
        config->convergence_m <= 0.0 ||
        !isfinite(config->damping) ||
        config->damping < 0.0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!point_is_finite(observations[i].sensor_position)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (
        initial_guess != NULL &&
        !point_is_finite(*initial_guess)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *geometry_scale = localization_geometry_scale(observations, count);

    if (
        !isfinite(*geometry_scale) ||
        !localization_geometry_is_3d(observations, count, *geometry_scale)
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


localization_config_t localization_default_config(void)
{
    return (localization_config_t) {
        /* Caller must provide a measured/modelled propagation speed. */
        .propagation_speed_mps = 0.0,
        .max_iterations = LOCALIZATION_DEFAULT_ITERATIONS,
        .convergence_m = LOCALIZATION_DEFAULT_CONVERGENCE_M,
        .damping = LOCALIZATION_DEFAULT_DAMPING,
    };
}


esp_err_t localization_tdoa_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    localization_result_t *result
)
{
    double geometry_scale = 0.0;

    esp_err_t err = localization_validate(
        observations,
        observation_count,
        config,
        initial_guess,
        result,
        &geometry_scale
    );

    if (err != ESP_OK) {
        return err;
    }

    memset(result, 0, sizeof(*result));

    localization_point3d_t estimate =
        initial_guess != NULL
            ? *initial_guess
            : localization_centroid(observations, observation_count);

    const double speed = config->propagation_speed_mps;
    const double reference_arrival_us =
        (double)observations[0].arrival_time_us;

    const localization_point3d_t reference_sensor =
        observations[0].sensor_position;

    for (
        uint8_t iteration = 0;
        iteration < config->max_iterations;
        ++iteration
    ) {
        double normal[3][3] = {{0}};
        double gradient[3] = {0};

        const double d0 = point_distance(estimate, reference_sensor);
        if (d0 <= LOCALIZATION_DISTANCE_EPSILON_M) {
            return ESP_ERR_INVALID_STATE;
        }

        const localization_point3d_t delta0 = point_subtract(
            estimate,
            reference_sensor
        );

        for (size_t i = 1; i < observation_count; ++i) {
            const localization_point3d_t sensor =
                observations[i].sensor_position;

            const localization_point3d_t delta = point_subtract(
                estimate,
                sensor
            );

            const double distance = point_distance(estimate, sensor);
            if (distance <= LOCALIZATION_DISTANCE_EPSILON_M) {
                return ESP_ERR_INVALID_STATE;
            }

            const double arrival_delta_us =
                (double)observations[i].arrival_time_us -
                reference_arrival_us;

            const double measured_delta_m =
                speed * arrival_delta_us / 1000000.0;

            const double residual_m =
                (distance - d0) - measured_delta_m;

            const double jacobian[3] = {
                delta.x_m / distance - delta0.x_m / d0,
                delta.y_m / distance - delta0.y_m / d0,
                delta.z_m / distance - delta0.z_m / d0,
            };

            for (size_t row = 0; row < 3; ++row) {
                gradient[row] += jacobian[row] * residual_m;

                for (size_t column = 0; column < 3; ++column) {
                    normal[row][column] +=
                        jacobian[row] * jacobian[column];
                }
            }
        }

        for (size_t axis = 0; axis < 3; ++axis) {
            normal[axis][axis] += config->damping;
            gradient[axis] = -gradient[axis];
        }

        double step[3];
        if (!solve_3x3(normal, gradient, step)) {
            return ESP_ERR_INVALID_STATE;
        }

        double step_norm = sqrt(
            step[0] * step[0] +
            step[1] * step[1] +
            step[2] * step[2]
        );

        if (!isfinite(step_norm)) {
            return ESP_ERR_INVALID_STATE;
        }

        const double max_step = fmax(geometry_scale, 1.0);
        if (step_norm > max_step) {
            const double scale = max_step / step_norm;
            step[0] *= scale;
            step[1] *= scale;
            step[2] *= scale;
            step_norm = max_step;
        }

        estimate.x_m += step[0];
        estimate.y_m += step[1];
        estimate.z_m += step[2];

        result->iterations = (uint8_t)(iteration + 1U);

        if (step_norm <= config->convergence_m) {
            result->converged = true;
            break;
        }
    }

    if (!point_is_finite(estimate)) {
        return ESP_ERR_INVALID_STATE;
    }

    result->source_position = estimate;

    double residual_sum_sq_us = 0.0;
    const double solved_d0 = point_distance(estimate, reference_sensor);

    for (size_t i = 1; i < observation_count; ++i) {
        const double distance = point_distance(
            estimate,
            observations[i].sensor_position
        );

        const double predicted_delta_us =
            ((distance - solved_d0) / speed) * 1000000.0;

        const double measured_delta_us =
            (double)observations[i].arrival_time_us -
            reference_arrival_us;

        const double residual_us =
            predicted_delta_us - measured_delta_us;

        residual_sum_sq_us += residual_us * residual_us;
    }

    result->rms_residual_us = sqrt(
        residual_sum_sq_us /
        (double)(observation_count - 1U)
    );

    double emission_sum_us = 0.0;

    for (size_t i = 0; i < observation_count; ++i) {
        const double distance = point_distance(
            estimate,
            observations[i].sensor_position
        );

        emission_sum_us +=
            (double)observations[i].arrival_time_us -
            (distance / speed) * 1000000.0;
    }

    result->emission_time_us = (int64_t)llround(
        emission_sum_us / (double)observation_count
    );

    return ESP_OK;
}
