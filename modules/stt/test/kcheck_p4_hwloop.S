// kcheck_p4_hwloop.S — for test/kcheck_p4.cpp only: tlib_dots_s8_esp32p4
// (src/tlib_ops/kernels_p4/tlib_dots_s8_esp32p4.S) with its block loop on
// the zero-overhead loop (esp.lp.setup) instead of a branch, the form
// esp-dsp's dspi_dotprod_s8_arp4 uses. Same arguments and results.

    .text
    .align  4
    .global kc_dots_s8_hwloop
    .type   kc_dots_s8_hwloop, @function

kc_dots_s8_hwloop:
    beqz    a4, .hw_end
.hw_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16
    esp.lp.setup    0, a3, .hw_block
    esp.vld.128.ip  q1, a1, 16
.hw_block:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .hw_row
.hw_end:
    ret

    .size   kc_dots_s8_hwloop, . - kc_dots_s8_hwloop
