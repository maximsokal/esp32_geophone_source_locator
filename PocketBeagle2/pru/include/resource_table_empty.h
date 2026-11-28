#pragma once

#include <stdint.h>

/*
 * Minimal Linux remoteproc resource table (ver=1, num=0) for AM62x PRU1 (A1 AM6254 / legacy A0 AM6232).
 * Avoids external PSSP dependency while satisfying Linux remoteproc ELF loader.
 */
struct pb2_resource_table {
    uint32_t ver;
    uint32_t num;
    uint32_t reserved[2];
};

#pragma DATA_SECTION(pb2_pru_rsc_table, ".resource_table")
#pragma RETAIN(pb2_pru_rsc_table)
struct pb2_resource_table pb2_pru_rsc_table = {
    1U,
    0U,
    { 0U, 0U }
};
