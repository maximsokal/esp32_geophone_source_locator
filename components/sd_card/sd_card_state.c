#include "sd_card_internal.h"

#include <stddef.h>


static const char *MOUNT_POINT =
    "/sdcard";


static sdmmc_card_t *s_card =
    NULL;


const char *sd_card_internal_mount_point(void)
{
    return
        MOUNT_POINT;
}


sdmmc_card_t *sd_card_internal_card(void)
{
    return
        s_card;
}


void sd_card_internal_set_card(
    sdmmc_card_t *card
)
{
    s_card =
        card;
}


sdmmc_card_t *sd_card_internal_take_card(void)
{
    sdmmc_card_t *card =
        s_card;

    s_card =
        NULL;

    return card;
}
