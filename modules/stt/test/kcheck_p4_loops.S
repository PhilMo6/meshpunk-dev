// kcheck_p4_loops.S — for test/kcheck_p4.cpp only: tlib_dots_s8_esp32p4
// (src/tlib_ops/kernels_p4/tlib_dots_s8_esp32p4.S) with its block loop
// arranged in other ways, same arguments and results, branch loops only:
//   kc_dots_rot  each column of b loaded one block ahead of its
//                multiply-accumulate;
//   kc_dots_u2   kc_dots_rot with two blocks per loop pass;
//   kc_dots_u4   kc_dots_rot with four blocks per loop pass;
//   kc_dots_p2   four blocks per loop pass, a and b each loaded two blocks
//                ahead of their multiply-accumulate (the kernel's loop).
// Loads per column: b 16 * k16 bytes; a 16 * k16 + 16 bytes (the 16 after
// the end of a, as tlib_dots_s8_esp32p4), kc_dots_p2 16 * k16 bytes.
// Registers: q0-q7; t0, t1 count; t3 walks a; a1 walks b.

    .text

// ── kc_dots_rot ─────────────────────────────────────────────────────────────

    .align  4
    .global kc_dots_rot
    .type   kc_dots_rot, @function
kc_dots_rot:
    beqz    a4, .rot_end
.rot_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16                      // q0 = a block 0
    esp.vld.128.ip  q1, a1, 16                      // q1 = b block 0
    addi    t0, a3, -1                              // blocks after the first
    beqz    t0, .rot_last
.rot_block:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1  // xacc += q0 . q1; q0 = next a block
    esp.vld.128.ip  q1, a1, 16                      // q1 = next b block
    addi    t0, t0, -1
    bnez    t0, .rot_block
.rot_last:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1  // the last block (loads the 16 bytes after a)
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .rot_row
.rot_end:
    ret
    .size   kc_dots_rot, . - kc_dots_rot

// ── kc_dots_u2 ──────────────────────────────────────────────────────────────

    .align  4
    .global kc_dots_u2
    .type   kc_dots_u2, @function
kc_dots_u2:
    beqz    a4, .u2_end
.u2_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16
    esp.vld.128.ip  q1, a1, 16
    addi    t0, a3, -1
    srli    t1, t0, 1                               // pairs of blocks after the first
    andi    t0, t0, 1                               // one more block after the pairs
    beqz    t1, .u2_odd
.u2_pair:
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q0, q1
    esp.vld.128.ip  q3, a1, 16
    addi    t1, t1, -1
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q2, q3
    esp.vld.128.ip  q1, a1, 16
    bnez    t1, .u2_pair
.u2_odd:
    beqz    t0, .u2_last
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1
    esp.vld.128.ip  q1, a1, 16
.u2_last:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .u2_row
.u2_end:
    ret
    .size   kc_dots_u2, . - kc_dots_u2

// ── kc_dots_u4 ──────────────────────────────────────────────────────────────

    .align  4
    .global kc_dots_u4
    .type   kc_dots_u4, @function
kc_dots_u4:
    beqz    a4, .u4_end
.u4_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16
    esp.vld.128.ip  q1, a1, 16
    addi    t0, a3, -1
    srli    t1, t0, 2                               // runs of four blocks after the first
    andi    t0, t0, 3                               // 0-3 more blocks after them
    beqz    t1, .u4_rest
.u4_quad:
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q0, q1
    esp.vld.128.ip  q3, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q2, q3
    esp.vld.128.ip  q5, a1, 16
    addi    t1, t1, -1
    esp.vmulas.s8.xacc.ld.ip    q6, t3, 16, q4, q5
    esp.vld.128.ip  q7, a1, 16
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q6, q7
    esp.vld.128.ip  q1, a1, 16
    bnez    t1, .u4_quad
.u4_rest:
    beqz    t0, .u4_last
.u4_one:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1
    esp.vld.128.ip  q1, a1, 16
    addi    t0, t0, -1
    bnez    t0, .u4_one
.u4_last:
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q0, q1
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .u4_row
.u4_end:
    ret
    .size   kc_dots_u4, . - kc_dots_u4

// ── kc_dots_p2 ──────────────────────────────────────────────────────────────
// Entering .p2_quad: q0/q1 = a/b block i, q2/q3 = block i + 1, t3 and a1 at
// block i + 2. A pass multiplies blocks i .. i + 3 and loads i + 2 .. i + 5,
// so it needs 4 blocks not loaded yet; the 2 loaded blocks left after the
// passes and the 0-3 not loaded are done after them.

    .align  4
    .global kc_dots_p2
    .type   kc_dots_p2, @function
kc_dots_p2:
    beqz    a4, .p2_end
.p2_row:
    esp.zero.xacc
    mv      t3, a0
    esp.vld.128.ip  q0, t3, 16                      // a block 0
    esp.vld.128.ip  q1, a1, 16                      // b block 0
    li      t0, 1
    beq     a3, t0, .p2_one
    esp.vld.128.ip  q2, t3, 16                      // a block 1
    esp.vld.128.ip  q3, a1, 16                      // b block 1
    addi    t0, a3, -2                              // blocks not loaded yet
    srli    t1, t0, 2                               // passes
    andi    t0, t0, 3                               // not loaded after the passes
    beqz    t1, .p2_tail
.p2_quad:
    esp.vmulas.s8.xacc.ld.ip    q4, t3, 16, q0, q1  // block i;     q4 = a block i + 2
    esp.vld.128.ip  q5, a1, 16                      //              q5 = b block i + 2
    esp.vmulas.s8.xacc.ld.ip    q6, t3, 16, q2, q3  // block i + 1; q6 = a block i + 3
    esp.vld.128.ip  q7, a1, 16                      //              q7 = b block i + 3
    addi    t1, t1, -1
    esp.vmulas.s8.xacc.ld.ip    q0, t3, 16, q4, q5  // block i + 2; q0 = a block i + 4
    esp.vld.128.ip  q1, a1, 16                      //              q1 = b block i + 4
    esp.vmulas.s8.xacc.ld.ip    q2, t3, 16, q6, q7  // block i + 3; q2 = a block i + 5
    esp.vld.128.ip  q3, a1, 16                      //              q3 = b block i + 5
    bnez    t1, .p2_quad
.p2_tail:
    esp.vmulas.s8.xacc  q0, q1
    esp.vmulas.s8.xacc  q2, q3
    beqz    t0, .p2_store
.p2_rest:
    esp.vld.128.ip  q0, t3, 16
    esp.vld.128.ip  q1, a1, 16
    addi    t0, t0, -1
    esp.vmulas.s8.xacc  q0, q1
    bnez    t0, .p2_rest
    j       .p2_store
.p2_one:
    esp.vmulas.s8.xacc  q0, q1
.p2_store:
    esp.movx.r.xacc.l   t4
    sw      t4, 0(a2)
    addi    a2, a2, 4
    addi    a4, a4, -1
    bnez    a4, .p2_row
.p2_end:
    ret
    .size   kc_dots_p2, . - kc_dots_p2
