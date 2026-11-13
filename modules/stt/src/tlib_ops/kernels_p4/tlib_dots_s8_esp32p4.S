// tlib_dots_s8_esp32p4.S — the int8 sums of the linear kernels on the
// ESP32-P4 (tlib_kernels_c.cpp, STT_KERNELS_P4), on the P4's 128-bit
// multiply-accumulate: upstream conformer-stt-s3's ESP32-S3 loop
// (EE.VMULAS.S8.ACCX.LD.IP, tlib_ops/kernels/*_esp32s3.s) in the P4's
// instructions, as esp-dsp's dspi_dotprod_s8_arp4 uses them. Assembled with
// -march=rv32imafc_zicsr_zifencei_xesppie (build.ps1).
//
// void tlib_dots_s8_esp32p4(const int8_t *a, const int8_t *b, int32_t *out,
//                           uint32_t k16, uint32_t m)
//   out[j] = the low 32 bits of the 40-bit accumulator after the sum over
//   i < 16 * k16 of a[i] * b[16 * k16 * j + i], for j < m.
// a and b 16-byte aligned, k16 > 0. It loads 16 * k16 bytes of a and of
// each column of b, nothing past them.
// The PIE instructions take address and data registers x8-x15 and x24-x31
// only (s0, s1, a0-a5, s8-s11, t3-t6).
//
// The block loop is a branch, not the zero-overhead loop (esp.lp.setup):
// the T-Display P4 is pre-v3.0 silicon, whose hardware-loop state has a bug
// (sdkconfig CONFIG_SOC_CPU_HAS_HWLOOP_STATE_BUG; IDF's FreeRTOS port works
// around it at context switches). test/kcheck_p4.cpp runs both against C
// sums on the P4: the zero-overhead loop gave sums one 16-byte block short
// (2 of 2940 sums with interrupts only; 15,079 to 17,859 of about 13
// million in each 4 s run with the worker preempted), the branch loops
// none. A loop pass multiplies four blocks, each loaded two blocks ahead
// of its product; k16 = 11 has its own path without a block loop. Cycles
// at k 176 x 93 columns: 3,089 (the zero-overhead loop, one block a pass:
// 4,575); at k 704 x 23: 3,217 (3,422).

    .text
    .align  4
    .global tlib_dots_s8_esp32p4
    .type   tlib_dots_s8_esp32p4, @function

// Entering .dots_quad: q0/q1 = a/b block i, q2/q3 = block i + 1, t3 (a) and
// a1 (b) at block i + 2. A pass multiplies blocks i .. i + 3 and loads
// i + 2 .. i + 5, so it needs 4 blocks not loaded yet; the 2 loaded blocks
// left after the passes and the 0-3 not loaded are done after them.
tlib_dots_s8_esp32p4:
    // a0 = a, a1 = b (row after row), a2 = out, a3 = k16, a4 = m
    beqz    a4, .dots_end
    li      t0, 11
    beq     a3, t0, .k11_row
.dots_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16                      // a block 0
    esp.vld.128.ip  q1, a1, 16                      // this column's b block 0
    li      t0, 1
    beq     a3, t0, .dots_one
    esp.vld.128.ip  q2, t3, 16                      // a block 1
    esp.vld.128.ip  q3, a1, 16                      // b block 1
    addi    t0, a3, -2                              // blocks not loaded yet
    srli    t1, t0, 2                               // passes
    andi    t0, t0, 3                               // not loaded after the passes
    beqz    t1, .dots_tail
.dots_quad:
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q0, q1  // xacc += block i;     q4 = a block i + 2
    esp.vld.128.ip  q5, a1, 16                      //                      q5 = b block i + 2
    esp.vmulas.s8.xacc.ld.ip    q6, t3, 16, q2, q3  // xacc += block i + 1; q6 = a block i + 3
    esp.vld.128.ip  q7, a1, 16                      //                      q7 = b block i + 3
    addi    t1, t1, -1
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q4, q5  // xacc += block i + 2; q0 = a block i + 4
    esp.vld.128.ip  q1, a1, 16                      //                      q1 = b block i + 4
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q6, q7  // xacc += block i + 3; q2 = a block i + 5
    esp.vld.128.ip  q3, a1, 16                      //                      q3 = b block i + 5
    bnez    t1, .dots_quad
.dots_tail:
    esp.vmulas.s8.xacc  q0, q1
    esp.vmulas.s8.xacc  q2, q3
    beqz    t0, .dots_store
.dots_rest:
    esp.vld.128.ip  q0, t3, 16
    esp.vld.128.ip  q1, a1, 16
    addi    t0, t0, -1
    esp.vmulas.s8.xacc  q0, q1
    bnez    t0, .dots_rest
    j       .dots_store
.dots_one:
    esp.vmulas.s8.xacc  q0, q1
.dots_store:
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .dots_row
.dots_end:
    ret

// k16 = 11 (rows of 176, the model's width: most of its layers): the same
// products, each block loaded two ahead, with no block loop: 31
// instructions a column instead of 46.
.k11_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16                      // a block 0
    esp.vld.128.ip  q1, a1, 16                      // b block 0
    esp.vld.128.ip  q2, t3, 16                      // a block 1
    esp.vld.128.ip  q3, a1, 16                      // b block 1
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q0, q1  // block 0;  q4 = a block 2
    esp.vld.128.ip  q5, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q6, t3, 16, q2, q3  // block 1;  q6 = a block 3
    esp.vld.128.ip  q7, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q4, q5  // block 2;  q0 = a block 4
    esp.vld.128.ip  q1, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q6, q7  // block 3;  q2 = a block 5
    esp.vld.128.ip  q3, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q0, q1  // block 4;  q4 = a block 6
    esp.vld.128.ip  q5, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q6, t3, 16, q2, q3  // block 5;  q6 = a block 7
    esp.vld.128.ip  q7, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q4, q5  // block 6;  q0 = a block 8
    esp.vld.128.ip  q1, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q6, q7  // block 7;  q2 = a block 9
    esp.vld.128.ip  q3, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q0, q1  // block 8;  q4 = a block 10
    esp.vld.128.ip  q5, a1, 16
    esp.vmulas.s8.xacc  q2, q3                      // block 9
    esp.vmulas.s8.xacc  q4, q5                      // block 10
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .k11_row
    ret

    .size   tlib_dots_s8_esp32p4, . - tlib_dots_s8_esp32p4
