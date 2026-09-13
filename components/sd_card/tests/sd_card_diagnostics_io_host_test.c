#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sd_card_diagnostics_internal.h"


static const char *PATH = "sd_card_diagnostics_host.tmp";


static void cleanup(void)
{
    (void)remove(PATH);
}


static void test_round_trip_and_bounded_read(void)
{
    cleanup();

    assert(sd_card_diagnostics_write_file(PATH, "abcdef") == ESP_OK);

    char full[16] = {0};
    assert(sd_card_diagnostics_read_file(PATH, full, sizeof(full)) == ESP_OK);
    assert(strcmp(full, "abcdef") == 0);

    char bounded[4] = {0};
    assert(sd_card_diagnostics_read_file(PATH, bounded, sizeof(bounded)) == ESP_OK);
    assert(strcmp(bounded, "abc") == 0);

    cleanup();
}


static void test_read_argument_and_missing_file_errors(void)
{
    cleanup();

    char buffer[8] = {0};
    assert(sd_card_diagnostics_read_file(PATH, NULL, sizeof(buffer)) == ESP_ERR_INVALID_ARG);
    assert(sd_card_diagnostics_read_file(PATH, buffer, 1U) == ESP_ERR_INVALID_ARG);
    assert(sd_card_diagnostics_read_file(PATH, buffer, sizeof(buffer)) == ESP_FAIL);
}


int main(void)
{
    test_round_trip_and_bounded_read();
    test_read_argument_and_missing_file_errors();
    cleanup();
    return 0;
}
