! Hand-written SH-4 for the game logic that is moved out of C (PLAN_ASM_TOTAL,
! phase 9). Each routine is a function of C, branch by branch; the C stays as
! the reference (a build with DC_EMIT_VERIFY runs both and compares) and as the
! other platforms' code.
!
!   void dc_clamp_step(int16_t *f, int32_t required, int32_t max_change,
!                      int32_t arc);
!     creature.c Creature_Head / Neck / Joint: change = required - *f, held to
!     +-max_change; *f += change; *f held to +-arc. int16 wraps as in C.
!   void dc_creature_tilt(int16_t *rot_z, int32_t angle, int32_t max);
!     Creature_Tilt: angle = angle * 4 - *rot_z, held to +-max; *rot_z += angle.
!   void dc_creature_turn_to(int16_t *rot_y, int32_t angle, int32_t max_turn);
!     Creature_TurnTo.

    .text
    .align 2
    .global _dc_clamp_step
    .global _dc_creature_tilt
    .global _dc_creature_turn_to

_dc_clamp_step:
    mov.w   @r4,r0
    mov     r5,r1
    sub     r0,r1
    exts.w  r1,r1                   ! change, as an int16
    neg     r6,r2                   ! the low bound
    cmp/gt  r1,r2
    bf      1f
    mov     r2,r1                   ! change < -max_change
    bra     2f
    nop
1:  cmp/gt  r6,r1
    bf      2f
    mov     r6,r1                   ! change > max_change
2:  add     r1,r0
    exts.w  r0,r0                   ! *f += change, as an int16
    neg     r7,r2
    cmp/gt  r0,r2
    bf      3f
    mov     r2,r0
    bra     4f
    nop
3:  cmp/gt  r7,r0
    bf      4f
    mov     r7,r0
4:  mov.w   r0,@r4
    rts
    nop

_dc_creature_tilt:
    mov.w   @r4,r0                  ! rot.z
    shll2   r5
    sub     r0,r5
    exts.w  r5,r5                   ! angle * 4 - rot.z, as an int16
    neg     r6,r2
    cmp/gt  r5,r2
    bf      1f
    mov     r2,r5
    bra     2f
    nop
1:  cmp/gt  r6,r5
    bf      2f
    mov     r6,r5
2:  add     r5,r0
    mov.w   r0,@r4
    rts
    nop

_dc_creature_turn_to:
    mov.w   @r4,r0                  ! rot.y
    cmp/gt  r6,r5                   ! angle > max_turn
    bf      1f
    add     r6,r0
    bra     3f
    nop
1:  neg     r6,r2
    cmp/gt  r5,r2                   ! angle < -max_turn
    bf      2f
    sub     r6,r0
    bra     3f
    nop
2:  add     r5,r0
3:  mov.w   r0,@r4
    rts
    nop
