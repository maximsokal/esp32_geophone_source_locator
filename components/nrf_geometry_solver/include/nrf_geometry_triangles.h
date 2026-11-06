#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Triangle consistency of the Channel Sounding distance matrix (CS-GEO-01).
 *
 * Any three nodes form a triangle whatever role each had in the Bluetooth procedures, so the three measured sides must
 * satisfy the triangle inequality: no side is longer than the sum of the other two. This is checked on the matrix
 * before the solver sees it, independent of which node was MAIN, initiator or reflector, and it works when the graph is
 * only partly known: every triangle whose three sides were measured is a witness, and the sides of the trusted
 * triangles bound the sides that failed or are missing.
 *
 * What it proves and what it does not. A violation proves that at least one of the three sides is wrong, not which one;
 * the sides of a single violated triangle stay suspects together, and a suspect is named only when one side belongs to
 * more than one violated triangle and to more of them than any other. A set of triangles that all hold proves nothing
 * about accuracy: three sides that are all too long by the same factor still close (a common bias), and the bounds of a
 * missing side are an interval, not a distance.
 */

#define NRF_GEOMETRY_MAX_TRIANGLES 10U      /* C(NRF_CS_MAX_NODES, 3) */

/*
 * A triangle is inconsistent only when its longest side exceeds the sum of the other two by more than the tolerance:
 * the larger of NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM (the solver's own limit for a pair against the solved layout) and
 * twice the root of the sum of the squared spreads of the three sides. Anything between zero and the tolerance is
 * marginal: reported, not rejected.
 */

typedef enum {
    NRF_GEOMETRY_TRIANGLE_CONSISTENT = 0,   /* the sides close (excess <= 0) */
    NRF_GEOMETRY_TRIANGLE_MARGINAL = 1,     /* 0 < excess <= tolerance */
    NRF_GEOMETRY_TRIANGLE_INCONSISTENT = 2, /* excess > tolerance */
} nrf_geometry_triangle_status_t;

typedef struct {
    nrf_cs_node_id_t node_a;                /* node_a < node_b < node_c */
    nrf_cs_node_id_t node_b;
    nrf_cs_node_id_t node_c;
    int32_t distance_ab_mm;
    int32_t distance_ac_mm;
    int32_t distance_bc_mm;
    int32_t excess_mm;                      /* longest side - (sum of the other two); <= 0 when the sides close */
    int32_t tolerance_mm;
    nrf_geometry_triangle_status_t status;
    uint8_t pair_index[3];                  /* indexes into the pairs array: ab, ac, bc */
} nrf_geometry_triangle_t;

typedef struct {
    size_t triangle_count;
    nrf_geometry_triangle_t triangles[NRF_GEOMETRY_MAX_TRIANGLES];
    size_t marginal_count;
    size_t inconsistent_count;
    int32_t worst_excess_mm;                /* largest excess of any triangle, 0 when there is none or all close */
    /* Per pair (index into the pairs array): the inconsistent triangles it belongs to. */
    uint8_t pair_violations[NRF_CS_MAX_PAIRS];
    /* The one pair that belongs to more inconsistent triangles than any other (and to at least two); -1 when none. */
    int suspect_pair;
} nrf_geometry_triangle_report_t;

typedef struct {
    int32_t lower_mm;                       /* the side cannot be shorter than this ... */
    int32_t upper_mm;                       /* ... or longer than this; INT32_MAX when nothing bounds it */
    size_t witnesses;                       /* third nodes that gave a bound */
    bool contradictory;                     /* lower_mm > upper_mm: the witnesses disagree with each other */
} nrf_geometry_edge_bounds_t;

/* A pair that carries a measured distance: valid, or a distance that only exceeded the cable. */
bool nrf_geometry_triangle_pair_usable(const nrf_cs_pair_result_t *pair);

/* Checks every triangle of usable pairs. `report` is fully written. False for bad arguments. */
bool nrf_geometry_triangles_check(
    const nrf_cs_pair_result_t *pairs, size_t pair_count, nrf_geometry_triangle_report_t *report);

/*
 * The interval a side between node_a and node_b must lie in, from the third nodes whose two sides to them are usable
 * and not part of an inconsistent triangle of `report` (NULL = trust every usable side). The measured value of the pair
 * itself is not used, so this answers for a pair that failed or was never measured.
 */
bool nrf_geometry_triangle_bounds(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const nrf_geometry_triangle_report_t *report,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b,
    nrf_geometry_edge_bounds_t *bounds);

/*
 * Cuts the upper end of an interval with a limit that does not come from the triangles, for instance a cable: the side
 * cannot be longer than the cable plus the measurement resolution however loosely the witnesses bound it. A negative
 * cap is no limit. The interval turns contradictory when the lower end now lies above the upper one.
 */
void nrf_geometry_edge_bounds_cap(nrf_geometry_edge_bounds_t *bounds, int32_t cap_mm);

/*
 * Size of a layout in 3D: the surface area of the convex hull of the solved positions, in mm^2 - the sum of the areas of the
 * triangles that close the body the devices span. X and Y come from the Bluetooth distances, Z from the barometers. A flat
 * layout counts both of its faces, so a 5 m square is 2 * 25 m^2. Fewer than three points, or points on one line, give 0.
 * Interior points do not matter. Saturates at INT64_MAX.
 */
int64_t nrf_geometry_hull_surface_area_mm2(const int32_t *x_mm, const int32_t *y_mm, const int32_t *z_mm, size_t count);

/*
 * The largest value of that area for `vertex_count` points (MAIN included) that all lie within `radius_mm` of the MAIN node:
 * the best polyhedron inscribed in the sphere of that radius, which for 3, 4, 5, 6 vertices is the flat triangle
 * (2.598 R^2), the regular tetrahedron (4.619 R^2), the triangular bipyramid (5.809 R^2) and the octahedron (6.928 R^2).
 * More points cannot beat the sphere itself, 4 pi R^2. The hull of points in a ball is inside the ball and so is its surface
 * (Cauchy), and moving a vertex outward only grows it, so no layout within the cables can exceed the value.
 */
int64_t nrf_geometry_max_hull_surface_area_mm2(size_t vertex_count, int32_t radius_mm);

const char *nrf_geometry_triangle_status_name(nrf_geometry_triangle_status_t status);

#ifdef __cplusplus
}
#endif
