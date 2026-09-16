#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "seismic_recorder_session_selection.h"


static void make_id(
    unsigned number,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE]
)
{
    snprintf(
        session_id,
        SEISMIC_RECORDER_SESSION_ID_SIZE,
        "S%06u",
        number
    );
}


static void feed_ascending(seismic_recorder_session_selection_t *selection)
{
    for (unsigned number = 1U; number <= 40U; ++number) {
        char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
        make_id(number, session_id);
        seismic_recorder_session_selection_consider(selection, session_id);
    }
}


static void feed_descending(seismic_recorder_session_selection_t *selection)
{
    for (unsigned number = 40U; number >= 1U; --number) {
        char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
        make_id(number, session_id);
        seismic_recorder_session_selection_consider(selection, session_id);
    }
}


static void feed_interleaved(seismic_recorder_session_selection_t *selection)
{
    static const unsigned order[] = {
        17, 2, 39, 8, 31, 1, 22, 14, 40, 5,
        27, 11, 35, 4, 19, 29, 7, 33, 16, 24,
        10, 37, 3, 21, 30, 6, 34, 13, 26, 18,
        32, 9, 38, 12, 25, 20, 36, 15, 28, 23
    };

    for (size_t i = 0U; i < sizeof(order) / sizeof(order[0]); ++i) {
        char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
        make_id(order[i], session_id);
        seismic_recorder_session_selection_consider(selection, session_id);
    }
}


static void assert_same_selection(
    const seismic_recorder_session_selection_t *left,
    const seismic_recorder_session_selection_t *right
)
{
    assert(seismic_recorder_session_selection_count(left) ==
           seismic_recorder_session_selection_count(right));
    for (
        size_t i = 0U;
        i < seismic_recorder_session_selection_count(left);
        ++i
    ) {
        assert(strcmp(
            seismic_recorder_session_selection_id(left, i),
            seismic_recorder_session_selection_id(right, i)
        ) == 0);
    }
}


static void test_permutation_independence(void)
{
    seismic_recorder_session_selection_t ascending;
    seismic_recorder_session_selection_t descending;
    seismic_recorder_session_selection_t interleaved;

    seismic_recorder_session_selection_init(&ascending, 32U);
    seismic_recorder_session_selection_init(&descending, 32U);
    seismic_recorder_session_selection_init(&interleaved, 32U);
    feed_ascending(&ascending);
    feed_descending(&descending);
    feed_interleaved(&interleaved);

    assert_same_selection(&ascending, &descending);
    assert_same_selection(&ascending, &interleaved);
    assert(seismic_recorder_session_selection_count(&ascending) == 32U);
    assert(strcmp(
        seismic_recorder_session_selection_id(&ascending, 0U),
        "S000040"
    ) == 0);
    assert(strcmp(
        seismic_recorder_session_selection_id(&ascending, 31U),
        "S000009"
    ) == 0);
}


static void test_capacity_invalid_names_gaps_and_duplicates(void)
{
    seismic_recorder_session_selection_t selection;
    seismic_recorder_session_selection_init(&selection, 4U);

    const char *candidates[] = {
        "S000003",
        "not-a-session",
        "S000010",
        "S000001",
        "S000007",
        "S000010",
        "S00000x",
        "S000000",
        "S999999",
        "S000005"
    };
    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        seismic_recorder_session_selection_consider(
            &selection,
            candidates[i]
        );
    }

    assert(seismic_recorder_session_selection_count(&selection) == 4U);
    assert(strcmp(seismic_recorder_session_selection_id(&selection, 0U), "S999999") == 0);
    assert(strcmp(seismic_recorder_session_selection_id(&selection, 1U), "S000010") == 0);
    assert(strcmp(seismic_recorder_session_selection_id(&selection, 2U), "S000007") == 0);
    assert(strcmp(seismic_recorder_session_selection_id(&selection, 3U), "S000005") == 0);
}


static void test_request_above_public_capacity_clamps(void)
{
    seismic_recorder_session_selection_t selection;
    seismic_recorder_session_selection_init(&selection, 1000U);
    feed_ascending(&selection);

    assert(selection.capacity == SEISMIC_RECORDER_MAX_SESSIONS);
    assert(seismic_recorder_session_selection_count(&selection) ==
           SEISMIC_RECORDER_MAX_SESSIONS);
}


static void test_canonical_parser(void)
{
    unsigned number = 0U;
    assert(seismic_recorder_session_id_parse("S000001", &number));
    assert(number == 1U);
    assert(seismic_recorder_session_id_parse("S999999", &number));
    assert(number == 999999U);

    const char *invalid[] = {
        NULL,
        "S000000",
        "S00000a",
        "S000001x",
        "S1",
        "s000001",
        "S1000000"
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!seismic_recorder_session_id_parse(invalid[i], &number));
    }
}


int main(void)
{
    test_permutation_independence();
    test_capacity_invalid_names_gaps_and_duplicates();
    test_request_above_public_capacity_clamps();
    test_canonical_parser();
    puts("SEISMIC_RECORDER_SESSION_SELECTION_HOST_PASS");
    return 0;
}
