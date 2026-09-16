#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "object_store_archive_policy.h"

#include "../object_store_archive_policy.c"


static void make_id(unsigned number, char id[OBJECT_FORMAT_ID_SIZE])
{
    snprintf(id, OBJECT_FORMAT_ID_SIZE, "OBJ_%04u", number);
}


static void feed_ascending(object_store_archive_selection_t *selection)
{
    for (unsigned number = 1U; number <= 40U; ++number) {
        char id[OBJECT_FORMAT_ID_SIZE];
        make_id(number, id);
        object_store_archive_selection_consider(selection, id);
    }
}


static void feed_descending(object_store_archive_selection_t *selection)
{
    for (unsigned number = 40U; number >= 1U; --number) {
        char id[OBJECT_FORMAT_ID_SIZE];
        make_id(number, id);
        object_store_archive_selection_consider(selection, id);
    }
}


static void feed_permuted(object_store_archive_selection_t *selection)
{
    static const unsigned order[] = {
        17, 2, 39, 8, 31, 1, 22, 14, 40, 5,
        27, 11, 35, 4, 19, 29, 7, 33, 16, 24,
        10, 37, 3, 21, 30, 6, 34, 13, 26, 18,
        32, 9, 38, 12, 25, 20, 36, 15, 28, 23
    };

    for (size_t i = 0U; i < sizeof(order) / sizeof(order[0]); ++i) {
        char id[OBJECT_FORMAT_ID_SIZE];
        make_id(order[i], id);
        object_store_archive_selection_consider(selection, id);
    }
}


static void assert_same_selection(
    const object_store_archive_selection_t *left,
    const object_store_archive_selection_t *right
)
{
    assert(object_store_archive_selection_count(left) ==
           object_store_archive_selection_count(right));
    for (size_t i = 0U; i < object_store_archive_selection_count(left); ++i) {
        assert(strcmp(
            object_store_archive_selection_id(left, i),
            object_store_archive_selection_id(right, i)
        ) == 0);
    }
}


static void test_permutation_independence(void)
{
    object_store_archive_selection_t ascending;
    object_store_archive_selection_t descending;
    object_store_archive_selection_t permuted;

    object_store_archive_selection_init(&ascending, 32U, NULL);
    object_store_archive_selection_init(&descending, 32U, NULL);
    object_store_archive_selection_init(&permuted, 32U, NULL);
    feed_ascending(&ascending);
    feed_descending(&descending);
    feed_permuted(&permuted);
    object_store_archive_selection_finalize(&ascending);
    object_store_archive_selection_finalize(&descending);
    object_store_archive_selection_finalize(&permuted);

    assert_same_selection(&ascending, &descending);
    assert_same_selection(&ascending, &permuted);
    assert(strcmp(object_store_archive_selection_id(&ascending, 0U), "OBJ_0009") == 0);
    assert(strcmp(object_store_archive_selection_id(&ascending, 31U), "OBJ_0040") == 0);
}


static void test_old_active_pin_is_permutation_independent(void)
{
    object_store_archive_selection_t ascending;
    object_store_archive_selection_t permuted;

    object_store_archive_selection_init(&ascending, 32U, "OBJ_0001");
    object_store_archive_selection_init(&permuted, 32U, "OBJ_0001");
    feed_ascending(&ascending);
    feed_permuted(&permuted);
    object_store_archive_selection_finalize(&ascending);
    object_store_archive_selection_finalize(&permuted);

    assert_same_selection(&ascending, &permuted);
    assert(strcmp(object_store_archive_selection_id(&ascending, 0U), "OBJ_0001") == 0);
    assert(strcmp(object_store_archive_selection_id(&ascending, 1U), "OBJ_0010") == 0);
    assert(object_store_archive_selection_is_active(&ascending, "OBJ_0001"));
}


static void test_missing_active_is_not_invented(void)
{
    object_store_archive_selection_t selection;
    object_store_archive_selection_init(&selection, 4U, "OBJ_9999");

    for (unsigned number = 1U; number <= 6U; ++number) {
        char id[OBJECT_FORMAT_ID_SIZE];
        make_id(number, id);
        object_store_archive_selection_consider(&selection, id);
    }
    object_store_archive_selection_finalize(&selection);

    assert(object_store_archive_selection_count(&selection) == 4U);
    assert(strcmp(object_store_archive_selection_id(&selection, 0U), "OBJ_0003") == 0);
    assert(strcmp(object_store_archive_selection_id(&selection, 3U), "OBJ_0006") == 0);
    assert(!object_store_archive_selection_is_active(&selection, "OBJ_9999"));
}


int main(void)
{
    test_permutation_independence();
    test_old_active_pin_is_permutation_independent();
    test_missing_active_is_not_invented();
    puts("OBJECT_STORE_ARCHIVE_POLICY_HOST_PASS");
    return 0;
}
