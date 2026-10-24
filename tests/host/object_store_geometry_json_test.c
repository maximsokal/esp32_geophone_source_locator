#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "object_store.h"

static object_store_geometry_t parse_geometry(const char *geometry_json)
{
    char text[1024];
    (void)snprintf(text, sizeof(text), "{\"geometry\":%s}", geometry_json);
    cJSON *root = cJSON_Parse(text);
    assert(root != NULL);
    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    object_store_geometry_from_json(root, &geometry);
    cJSON_Delete(root);
    return geometry;
}

static void test_valid_pod_is_read_completely(void)
{
    const object_store_geometry_t geometry = parse_geometry(
        "{\"anchored\":true,\"pods\":[{\"computed\":true,\"leveled\":true,"
        "\"x_mm\":1250,\"y_mm\":-2200,\"z_mm\":30,\"residual_mm\":7,"
        "\"uncertainty_mm\":9,\"computed_at_ms\":123456}]}"
    );
    assert(geometry.anchored);
    assert(geometry.pods[0].computed && geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 1250 && geometry.pods[0].y_mm == -2200);
    assert(geometry.pods[0].z_mm == 30);
    assert(geometry.pods[0].residual_mm == 7 && geometry.pods[0].uncertainty_mm == 9);
    assert(geometry.pods[0].computed_at_ms == 123456U);
    assert(!geometry.pods[1].computed);
}

static void assert_pod0_not_computed(const char *pod_json)
{
    char geometry_json[512];
    (void)snprintf(geometry_json, sizeof(geometry_json), "{\"pods\":[%s]}", pod_json);
    const object_store_geometry_t geometry = parse_geometry(geometry_json);
    assert(!geometry.pods[0].computed);
    assert(geometry.pods[0].x_mm == 0 && geometry.pods[0].y_mm == 0 && geometry.pods[0].z_mm == 0);
}

static void test_out_of_range_or_fractional_coordinates_are_rejected(void)
{
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":1e30,\"y_mm\":0,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":0,\"y_mm\":-1e30,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":0,\"y_mm\":0,\"z_mm\":1e999}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":2147483648,\"y_mm\":0,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":0,\"y_mm\":-2147483649,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":10.5,\"y_mm\":0,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":0,\"y_mm\":0}");
    assert_pod0_not_computed("{\"computed\":true,\"x_mm\":\"7\",\"y_mm\":0,\"z_mm\":0}");
    assert_pod0_not_computed("{\"computed\":false,\"x_mm\":5,\"y_mm\":5,\"z_mm\":5}");
}

static void test_one_bad_pod_does_not_affect_the_others(void)
{
    const object_store_geometry_t geometry = parse_geometry(
        "{\"pods\":[{\"computed\":true,\"x_mm\":1e30,\"y_mm\":1,\"z_mm\":1},"
        "{\"computed\":true,\"x_mm\":-5,\"y_mm\":6,\"z_mm\":7}]}"
    );
    assert(!geometry.pods[0].computed);
    assert(geometry.pods[1].computed);
    assert(geometry.pods[1].x_mm == -5 && geometry.pods[1].y_mm == 6 && geometry.pods[1].z_mm == 7);
}

static void test_optional_fields_fall_back_to_zero_when_invalid(void)
{
    const object_store_geometry_t geometry = parse_geometry(
        "{\"pods\":[{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,"
        "\"residual_mm\":-4,\"uncertainty_mm\":1e30,\"computed_at_ms\":-1},"
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,"
        "\"residual_mm\":2.5,\"uncertainty_mm\":2147483647,\"computed_at_ms\":1e30}]}"
    );
    assert(geometry.pods[0].computed);
    assert(geometry.pods[0].residual_mm == 0);
    assert(geometry.pods[0].uncertainty_mm == 0);
    assert(geometry.pods[0].computed_at_ms == 0U);
    assert(geometry.pods[1].computed);
    assert(geometry.pods[1].residual_mm == 0);
    assert(geometry.pods[1].uncertainty_mm == INT32_MAX);
    assert(geometry.pods[1].computed_at_ms == 0U);
}

static void test_absent_or_malformed_geometry_leaves_defaults(void)
{
    object_store_geometry_t geometry;
    cJSON *root = cJSON_Parse("{\"id\":\"OBJ_0001\"}");
    assert(root != NULL);
    memset(&geometry, 0, sizeof(geometry));
    object_store_geometry_from_json(root, &geometry);
    cJSON_Delete(root);
    assert(!geometry.anchored && !geometry.pods[0].computed);

    root = cJSON_Parse("{\"geometry\":5}");
    assert(root != NULL);
    object_store_geometry_from_json(root, &geometry);
    cJSON_Delete(root);
    assert(!geometry.anchored && !geometry.pods[0].computed);

    object_store_geometry_from_json(NULL, &geometry);
    object_store_geometry_from_json(NULL, NULL);
}

int main(void)
{
    test_valid_pod_is_read_completely();
    test_out_of_range_or_fractional_coordinates_are_rejected();
    test_one_bad_pod_does_not_affect_the_others();
    test_optional_fields_fall_back_to_zero_when_invalid();
    test_absent_or_malformed_geometry_leaves_defaults();
    return 0;
}
