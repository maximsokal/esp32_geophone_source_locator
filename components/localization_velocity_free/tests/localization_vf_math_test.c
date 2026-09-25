#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "localization_vf_math.h"


static void assert_near(double actual, double expected, double epsilon)
{
    if (!(fabs(actual - expected) <= epsilon)) {
        fprintf(stderr, "expected %.12g, got %.12g (eps %.3g)\n", expected, actual, epsilon);
        assert(0);
    }
}


static void test_least_squares_exact_and_rank_deficient(void)
{
    /* y = 2 + 3x sampled exactly: an overdetermined, consistent system. */
    double a[5 * 2];
    double b[5];
    for (size_t i = 0U; i < 5U; ++i) {
        a[i * 2U] = 1.0;
        a[i * 2U + 1U] = (double)i * 1.0e-3; /* badly scaled column */
        b[i] = 2.0 + 3.0 * a[i * 2U + 1U];
    }
    double x[2];
    assert(vf_least_squares(a, b, 5U, 2U, 1.0e-12, x));
    assert_near(x[0], 2.0, 1.0e-10);
    assert_near(x[1], 3.0, 1.0e-7);

    /* Two identical columns: rank deficient, refused. */
    double c[3 * 2] = { 1.0, 1.0, 2.0, 2.0, 3.0, 3.0 };
    double d[3] = { 1.0, 2.0, 3.0 };
    assert(!vf_least_squares(c, d, 3U, 2U, 1.0e-10, x));
    /* Fewer rows than unknowns: refused. */
    assert(!vf_least_squares(c, d, 1U, 2U, 1.0e-10, x));
}


static void test_square_solver(void)
{
    const double a[9] = { 0.0, 2.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 0.0 };
    const double b[3] = { 3.0, 6.0, 5.0 };
    double x[3];
    assert(vf_solve_square(a, b, 3U, x));
    for (size_t r = 0U; r < 3U; ++r) {
        assert_near(a[r * 3U] * x[0] + a[r * 3U + 1U] * x[1] + a[r * 3U + 2U] * x[2], b[r], 1.0e-12);
    }
    const double singular[4] = { 1.0, 2.0, 2.0, 4.0 };
    assert(!vf_solve_square(singular, b, 2U, x));
}


static void test_plane_frame_round_trip_and_orientation(void)
{
    /* Points on a plane tilted 20 degrees about the x axis. */
    const double tilt = 20.0 * 3.14159265358979323846 / 180.0;
    localization_point3d_t points[6];
    const double uv[6][2] = { { 0, 0 }, { 3, 0 }, { 0, 2 }, { 3, 2 }, { 1.5, 1 }, { 2, 0.5 } };
    for (size_t i = 0U; i < 6U; ++i) {
        points[i] = (localization_point3d_t) { uv[i][0] + 10.0, uv[i][1] * cos(tilt), uv[i][1] * sin(tilt) + 5.0 };
    }
    vf_plane_frame_t frame;
    assert(vf_fit_plane(points, 6U, &frame));
    /* Normal points up and is perpendicular to the tilted plane. */
    assert(frame.normal.z_m > 0.0);
    assert_near(frame.normal.x_m, 0.0, 1.0e-9);
    assert_near(frame.normal.y_m, -sin(tilt), 1.0e-9);
    assert_near(frame.normal.z_m, cos(tilt), 1.0e-9);
    for (size_t i = 0U; i < 6U; ++i) {
        double u = 0.0, v = 0.0, w = 0.0;
        vf_to_plane(&frame, points[i], &u, &v, &w);
        assert_near(w, 0.0, 1.0e-9);
        const localization_point3d_t back = vf_from_plane(&frame, u, v, w);
        assert_near(back.x_m, points[i].x_m, 1.0e-9);
        assert_near(back.y_m, points[i].y_m, 1.0e-9);
        assert_near(back.z_m, points[i].z_m, 1.0e-9);
    }
    /* Negative w is below the surface. */
    const localization_point3d_t below = vf_from_plane(&frame, 0.0, 0.0, -1.0);
    assert(below.z_m < frame.centroid.z_m);

    /* Collinear points span no plane. */
    const localization_point3d_t line[3] = { { 0, 0, 0 }, { 1, 1, 1 }, { 2, 2, 2 } };
    assert(!vf_fit_plane(line, 3U, &frame));
}


static void test_logistic_fit(void)
{
    /* Exact logistic quantiles recover (mu, s). */
    const double mu = 1.25;
    const double s = 0.08;
    double samples[40];
    for (size_t k = 0U; k < 40U; ++k) {
        const double p = ((double)k + 0.5) / 40.0;
        samples[k] = mu + s * log(p / (1.0 - p));
    }
    double fitted_mu = 0.0, fitted_s = 0.0;
    assert(vf_fit_logistic(samples, 40U, &fitted_mu, &fitted_s));
    assert_near(fitted_mu, mu, 1.0e-9);
    assert_near(fitted_s, s, 0.01);

    /* A wild combination pulls the logistic location far less than the
     * mean (its score is bounded; the scale inflates instead). It is not a
     * hard outlier rejector: one extreme value in ten still shifts it. */
    double robust[10] = { 1.00, 1.02, 0.98, 1.01, 0.99, 1.03, 0.97, 1.00, 1.01, 25.0 };
    assert(vf_fit_logistic(robust, 10U, &fitted_mu, &fitted_s));
    double mean = 0.0;
    for (size_t i = 0U; i < 10U; ++i) mean += robust[i] / 10.0;
    assert(fabs(fitted_mu - 1.0) < 0.6);
    assert(fabs(mean - 1.0) > 2.0);
    assert(fitted_s > 1.0);

    /* A few scattered bad combinations among many good ones: the mode
     * stays on the good cluster. */
    double combos[28];
    for (size_t i = 0U; i < 28U; ++i) {
        combos[i] = 1.0 + 0.01 * (double)((int)((i * 7U) % 11U) - 5);
    }
    combos[3] = 4.0;
    combos[9] = 5.0;
    combos[20] = -2.0;
    assert(vf_fit_logistic(combos, 28U, &fitted_mu, &fitted_s));
    assert(fabs(fitted_mu - 1.0) < 0.05);

    /* Degenerate inputs. */
    const double one = 3.5;
    assert(vf_fit_logistic(&one, 1U, &fitted_mu, &fitted_s));
    assert(fitted_mu == 3.5 && fitted_s == 0.0);
    const double same[3] = { 2.0, 2.0, 2.0 };
    assert(vf_fit_logistic(same, 3U, &fitted_mu, &fitted_s));
    assert(fitted_mu == 2.0 && fitted_s == 0.0);
    const double bad[2] = { 1.0, NAN };
    assert(!vf_fit_logistic(bad, 2U, &fitted_mu, &fitted_s));
    assert(!vf_fit_logistic(samples, 0U, &fitted_mu, &fitted_s));
}


static void test_travel_time_fit(void)
{
    const double lengths[4] = { 1.0, 2.0, 2.5, 4.0 };
    double arrivals[4];
    for (size_t i = 0U; i < 4U; ++i) {
        arrivals[i] = 0.010 + lengths[i] / 250.0;
    }
    vf_time_fit_t fit;
    /* Velocity-free. */
    assert(vf_fit_travel_times(arrivals, lengths, NULL, 4U, 1.0e-4, 0.0, -1.0, &fit));
    assert_near(1.0 / fit.slowness_s_per_m, 250.0, 1.0e-6);
    assert_near(fit.t0_s, 0.010, 1.0e-12);
    assert_near(fit.rms_s, 0.0, 1.0e-12);

    /* Fixed (wrong) velocity: the misfit shows up in chi-square. */
    assert(vf_fit_travel_times(arrivals, lengths, NULL, 4U, 1.0e-4, 1.0 / 300.0, 0.0, &fit));
    assert_near(fit.slowness_s_per_m, 1.0 / 300.0, 0.0);
    assert(fit.chi_square > 1.0);

    /* A loose prior lets the data win; a tight one pins the slowness. */
    assert(vf_fit_travel_times(arrivals, lengths, NULL, 4U, 1.0e-4, 1.0 / 300.0, 1.0, &fit));
    assert_near(1.0 / fit.slowness_s_per_m, 250.0, 0.5);
    assert(vf_fit_travel_times(arrivals, lengths, NULL, 4U, 1.0e-4, 1.0 / 300.0, 1.0e-9, &fit));
    assert_near(1.0 / fit.slowness_s_per_m, 300.0, 0.5);

    /* Identical path lengths cannot determine a velocity without a prior. */
    const double equal[3] = { 2.0, 2.0, 2.0 };
    assert(!vf_fit_travel_times(arrivals, equal, NULL, 3U, 1.0e-4, 0.0, -1.0, &fit));

    /* Zero-weight entries are ignored. */
    const double weights[4] = { 1.0, 1.0, 0.0, 1.0 };
    double corrupted[4] = { arrivals[0], arrivals[1], 99.0, arrivals[3] };
    assert(vf_fit_travel_times(corrupted, lengths, weights, 4U, 1.0e-4, 0.0, -1.0, &fit));
    assert_near(1.0 / fit.slowness_s_per_m, 250.0, 1.0e-6);
}


int main(void)
{
    test_least_squares_exact_and_rank_deficient();
    test_square_solver();
    test_plane_frame_round_trip_and_orientation();
    test_logistic_fit();
    test_travel_time_fit();
    printf("localization velocity-free math tests passed\n");
    return 0;
}
