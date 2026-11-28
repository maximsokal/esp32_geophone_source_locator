/*
 * TI AM6232 PRU-SS PRU1 Linker Command File (PocketBeagle 2 Rev A0)
 * PRU1 local view:
 *   IMEM  : 0x00000000 .. 0x00002FFF (12 KB)
 *   DMEM1 : 0x00000000 .. 0x00001FFF (8 KB, own data RAM)
 *   SHARED: 0x00010000 .. 0x00012FFF (12 KB, PRU shared RAM)
 */

-cr
--stack_size=0x400
--heap_size=0x0

MEMORY
{
    PAGE 0:
      PRU_IMEM        : org = 0x00000000 len = 0x00003000

    PAGE 1:
      PRU_DMEM1       : org = 0x00000000 len = 0x00002000
      PRU_SHAREDMEM   : org = 0x00010000 len = 0x00003000
}

SECTIONS
{
    .text:_c_int00* : > 0x00000000, PAGE 0
    .text           : > PRU_IMEM, PAGE 0
    .stack          : > PRU_DMEM1, PAGE 1
    .bss            : > PRU_DMEM1, PAGE 1
    .cio            : > PRU_DMEM1, PAGE 1
    .data           : > PRU_DMEM1, PAGE 1
    .switch         : > PRU_DMEM1, PAGE 1
    .sysmem         : > PRU_DMEM1, PAGE 1
    .cinit          : > PRU_DMEM1, PAGE 1
    .rodata         : > PRU_DMEM1, PAGE 1
    .rofardata      : > PRU_DMEM1, PAGE 1
    .farbss         : > PRU_DMEM1, PAGE 1
    .fardata        : > PRU_DMEM1, PAGE 1
    .resource_table : > PRU_DMEM1, PAGE 1
    .pb2_shared     : > PRU_DMEM1, PAGE 1
}
