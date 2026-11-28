; TI PRU Assembly deterministic 32-clock DUAL_DOUT readout for AD7606 Rev. G @ 200 MHz
; Prototype: void read_dual_dout_32_asm(int16_t out_ch[4]);
;   R14 = out_ch pointer (V1, V2, V5, V6)
;   R30 bits: bit 11 = RD/SCLK, bit 12 = CS_N
;   R31 bits: bit 6  = DOUTA (V1,V2), bit 7 = DOUTB (V5,V6)
;
; Timing @ 200 MHz (5 ns/cycle), VDRIVE = 3.3 V (Table 3):
;   t18 (CS_N fall to SCLK fall/MSB valid): 7 cycles (35 ns)
;   SCLK period: 16 cycles (80 ns = 12.5 MHz <= 12.5 MHz max)
;     SCLK LOW : 7 cycles (35 ns >= t20=15 ns, >= t22=30 ns)
;     SCLK HIGH: 9 cycles (45 ns >= t21=15 ns)
;   Total execution: ~527 cycles (2.635 us) < tCONV_min (3.45 us = 690 cycles)

        .sect   ".text:read_dual_dout_32_asm"
        .global read_dual_dout_32_asm

read_dual_dout_32_asm:
        CLR     R30, R30, 12            ; CS_N = 0 (t18 start)
        LDI     R16, 0                  ; Clear DOUTA 32-bit accumulator (V1:hi16, V2:lo16)
        LDI     R17, 0                  ; Clear DOUTB 32-bit accumulator (V5:hi16, V6:lo16)
        MOV     R0, R0                  ; NOP (4 cycles setup)
        MOV     R0, R0
        MOV     R0, R0
        MOV     R0, R0                  ; Total 7 cycles since CS_N = 0

        LOOP    L_dual_loop_end, 32     ; 32 bits, zero loop overhead
        CLR     R30, R30, 11            ; cy 1: SCLK = 0
        LSL     R16, R16, 1             ; cy 2: shift DOUTA acc
        LSL     R17, R17, 1             ; cy 3: shift DOUTB acc
        MOV     R0, R0                  ; cy 4
        MOV     R0, R0                  ; cy 5
        MOV     R0, R0                  ; cy 6
        MOV     R0, R0                  ; cy 7 (35 ns SCLK LOW >= t20, t22)
        SET     R30, R30, 11            ; cy 8: SCLK = 1
        MOV     R0, R0                  ; cy 9
        MOV     R0, R31                 ; cy 10: single R31 read for both DOUTA & DOUTB
        LSR     R1, R0, 6               ; cy 11: bit 0 = DOUTA
        AND     R1, R1, 1               ; cy 12
        OR      R16, R16, R1            ; cy 13
        LSR     R1, R0, 7               ; cy 14: bit 0 = DOUTB
        AND     R1, R1, 1               ; cy 15
        OR      R17, R17, R1            ; cy 16
L_dual_loop_end:

        MOV     R0, R0                  ; t17 hold before CS_N = 1
        SET     R30, R30, 12            ; CS_N = 1

        ; Store V1 (R16.w2), V2 (R16.w0), V5 (R17.w2), V6 (R17.w0) into out_ch[0..3]
        SBBO    &R16.w2, R14, 0, 2
        SBBO    &R16.w0, R14, 2, 2
        SBBO    &R17.w2, R14, 4, 2
        SBBO    &R17.w0, R14, 6, 2
        JMP     R3.w2
