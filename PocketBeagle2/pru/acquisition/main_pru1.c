#include <stdint.h>
#include "pru_adc_shared.h"
#include "resource_table_empty.h"

extern void pru1_fsm_run(volatile pb2_pru_shared_t *shm);

int main(void) {
    volatile pb2_pru_shared_t *shm = (volatile pb2_pru_shared_t *)PB2_PRU_SHARED_MEM_ADDR;
    pru1_fsm_run(shm);
    return 0;
}
