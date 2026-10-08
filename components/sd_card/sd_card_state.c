#include "sd_card_internal.h"

#include <stddef.h>


static const char *MOUNT_POINT =
    "/sdcard";


static sdmmc_card_t *s_card =
    NULL;

static sd_card_internal_unmount_fn_t s_recovery_unmount =
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


void sd_card_internal_set_recovery_unmount(
    sd_card_internal_unmount_fn_t unmount_fn
)
{
    s_recovery_unmount = unmount_fn;
}


sd_card_internal_unmount_fn_t sd_card_internal_recovery_unmount(void)
{
    return s_recovery_unmount;
}
