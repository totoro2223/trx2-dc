! Hand-written SH-4 for the weather and the water's specks (fx/weather.c,
! fx/water_particles.c): the update of the rain, snow and water particles and
! the staging of each into the batcher's lists, and the walk over the active
! effects (effects/manager.c). The C side keeps the rare paths (a spawn) and
! the room lookups; each is called back through a pointer in the control block.
!
!   void dc_rain_update(CTL *);   void dc_snow_update(CTL *);
!   void dc_water_update(CTL *);
!   void dc_rain_draw(DRAW *);    void dc_snow_draw(DRAW *);
!   void dc_water_draw(DRAW *);   void dc_effect_control(EFFCTL *);
!
! UPDATE control block (words):
!    0 particles  4 slots  8 spawns below this index  12 most spawns per tick
!   16 wind (rain: 4 * wind.x; snow: 2 * wind.x)  20 the same for z
!   24 camera x  28 camera z
!   32 the draw generator's state  36 its frozen flag (byte)  40 the multiplier
!   44 the increment
!   48 the C function that spawns one  52 the C function that tells where a
!   point is (bit 0: stops or dies, bit 1: snow's -3)  56 the longest reach
!
! RAINDROP (44): pos 0, prev_pos 12, prev_yv 24, vel 28, life 40.
! SNOWFLAKE (52): pos 0, prev_pos 12, stopped 24, prev_yv 28, prev_life 32,
! vel 36, life 48.   WATER (40): pos 0, prev_pos 12, life 24, vel 28.
!
! DRAW control block (words):
!    0 particles  4 slots  8 out list  12 room in it  16 written (out)
!   20 interpolate  24 ratio (float)  28 near  32 far  36 fog start / (rain)
!   argb a  40 fog end / (rain) argb b  44 the picture (u 0..3, v 4..7, page)
!   48 the view matrix  52 4 * wind.x (rain) / 1143.0 (water)  56 4 * wind.z
!   (rain) / 1.0e0 unused  60 perspective set (snow)  64 fog range 1.0
!
! r8 the block  r9 the particle  r10 particles left  r11 index / out  r12
! alive / room  r13, r14 as each routine says. Every routine keeps r8-r14.

    .macro JT target
    bf      9f
    bra     \target
    nop
9:
    .endm
    .macro JF target
    bt      9f
    bra     \target
    nop
9:
    .endm
    .macro ABSR r
    cmp/pz  \r
    bt      9f
    neg     \r,\r
9:
    .endm
    .macro PUSHALL
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    .endm
    .macro POPALL
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    .endm
! A velocity held to -4..4.
    .macro CLAMP4 r
    mov     #-4,r3
    cmp/ge  r3,\r
    bt      9f
    mov     r3,\r
9:  mov     #4,r3
    cmp/gt  r3,\r
    bf      9f
    mov     r3,\r
9:
    .endm
! One axis between two ticks: prev + (cur - prev) * ratio, as an integer. The
! ratio is in fr4.
    .macro INTERPX cur, prev, dst
    mov.l   @(\cur,r9),r1
    mov.l   @(\prev,r9),r2
    sub     r2,r1
    lds     r1,fpul
    float   fpul,fr0
    fmul    fr4,fr0
    lds     r2,fpul
    float   fpul,fr1
    fadd    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r1
    mov.l   r1,@(\dst,r15)
    .endm
! One particle at r11 (the slot's start; it moves on to the next): the
! position fr1..fr3, half fr4, argb r3, the picture at r13.
    .macro PARTICLE_OUT
    mov     #72,r0
    add     r0,r11
    mov     r13,r1
    add     #16,r1
    fmov.s  @r1+,fr8
    fmov.s  @r1+,fr9
    fmov.s  @r1+,fr10
    fmov.s  @r1+,fr11
    fmov.s  fr11,@-r11
    fmov.s  fr10,@-r11
    fmov.s  fr9,@-r11
    fmov.s  fr8,@-r11
    mov     r13,r1
    fmov.s  @r1+,fr8
    fmov.s  @r1+,fr9
    fmov.s  @r1+,fr10
    fmov.s  @r1+,fr11
    fmov.s  fr11,@-r11
    fmov.s  fr10,@-r11
    fmov.s  fr9,@-r11
    fmov.s  fr8,@-r11
    mov     #32,r0
    mov.l   @(r0,r13),r2
    mov.l   r2,@-r11                    ! page
    mov.l   r3,@-r11                    ! argb
    mov     #0,r2
    mov.l   r2,@-r11                    ! added
    fldi1   fr0
    fmov.s  fr0,@-r11                   ! cs
    fldi0   fr0
    fmov.s  fr0,@-r11                   ! sn
    fmov.s  fr4,@-r11                   ! half_h
    fmov.s  fr4,@-r11                   ! half
    fmov.s  fr3,@-r11
    fmov.s  fr2,@-r11
    fmov.s  fr1,@-r11
    mov     #72,r0
    add     r0,r11
    .endm
! zv = view row 2 . (r4, r5, r6) + its translation, 64 bits in r5:r4 (high:low);
! r14 is the matrix + 64.
    .macro DEPTH64
    mov.l   @(0,r14),r1
    dmuls.l r1,r4
    sts     macl,r2
    sts     mach,r3
    mov.l   @(8,r14),r1
    dmuls.l r1,r5
    clrt
    sts     macl,r1
    addc    r1,r2
    sts     mach,r1
    addc    r1,r3
    mov.l   @(16,r14),r1
    dmuls.l r1,r6
    clrt
    sts     macl,r1
    addc    r1,r2
    sts     mach,r1
    addc    r1,r3
    clrt
    mov.l   @(24,r14),r1
    addc    r1,r2
    mov.l   @(28,r14),r1
    addc    r1,r3
    mov     r2,r4
    mov     r3,r5
    .endm
! vpos_z = (int)(zv >> 14) into r1, from r5:r4.
    .macro VPOSZ
    mov     r4,r1
    shlr8   r1
    shlr2   r1
    shlr2   r1
    shlr2   r1
    mov     r5,r0
    shll16  r0
    shll2   r0
    or      r0,r1
    .endm

    .text
    .align 2

! The next number of the draw generator, in r2 (r8 the block).
wx_draw:
    mov.l   @(32,r8),r1
    mov.l   @r1,r2
    mov.l   @(36,r8),r0
    mov.b   @r0,r0
    tst     r0,r0
    bf      1f
    mov.l   @(40,r8),r3
    mul.l   r3,r2
    sts     macl,r2
    mov.l   @(44,r8),r3
    add     r3,r2
    mov.l   r2,@r1
1:  shlr8   r2
    shlr2   r2
    extu.w  r2,r2
    shll    r2
    rts
    shlr    r2

! ---- rain: a drop falls with the wind, drifts and dies
    .global _dc_rain_update
_dc_rain_update:
    PUSHALL
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov     #0,r11
    mov     #0,r12
rn_loop:
    tst     r10,r10
    bf      1f
    bra     rn_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    bf      rn_live
    mov.l   @(8,r8),r1
    cmp/ge  r1,r11
    JT      rn_next
    mov.l   @(12,r8),r1
    cmp/ge  r1,r12
    JT      rn_next
    add     #1,r12
    mov     r9,r4
    mov.l   @(48,r8),r1
    jsr     @r1
    nop
    mov.l   @r9,r0
    tst     r0,r0
    JT      rn_next
rn_live:
    mov     r9,r14
    add     #40,r14
    mov.l   @(0,r9),r1
    mov.l   r1,@(12,r9)
    mov.l   @(4,r9),r1
    mov.l   r1,@(16,r9)
    mov.l   @(8,r9),r1
    mov.l   r1,@(20,r9)
    mov.l   @(32,r9),r1
    mov.l   r1,@(24,r9)
    mov.b   @r14,r0
    extu.b  r0,r0
    mov     #-16,r1
    extu.b  r1,r1
    cmp/hi  r1,r0                       ! life > 240
    JT      rn_kill
    mov.l   @(56,r8),r3
    mov.l   @(24,r8),r1
    mov.l   @(0,r9),r2
    sub     r2,r1
    ABSR    r1
    cmp/gt  r3,r1
    JT      rn_kill
    mov.l   @(28,r8),r1
    mov.l   @(8,r9),r2
    sub     r2,r1
    ABSR    r1
    cmp/gt  r3,r1
    JT      rn_kill
    mov     r9,r4
    mov.l   @(52,r8),r1
    jsr     @r1
    nop
    tst     #1,r0                       ! -2 or under water
    JF      rn_kill
    mov.l   @(28,r9),r1                 ! pos += vel + wind
    mov.l   @(16,r8),r2
    add     r2,r1
    mov.l   @(0,r9),r2
    add     r1,r2
    mov.l   r2,@(0,r9)
    mov.l   @(32,r9),r1
    shll2   r1
    shll    r1
    mov.l   @(4,r9),r2
    add     r1,r2
    mov.l   r2,@(4,r9)
    mov.l   @(36,r9),r1
    mov.l   @(20,r8),r2
    add     r2,r1
    mov.l   @(8,r9),r2
    add     r1,r2
    mov.l   r2,@(8,r9)
    bsr     wx_draw
    nop
    mov     r2,r5
    mov     r5,r0
    and     #3,r0
    cmp/eq  #3,r0
    bt      1f
    add     #-1,r0
    mov.l   @(28,r9),r1
    add     r0,r1
    CLAMP4  r1
    mov.l   r1,@(28,r9)
1:  mov     r5,r0
    shlr2   r0
    and     #3,r0
    cmp/eq  #3,r0
    bt      1f
    add     #-1,r0
    mov.l   @(36,r9),r1
    add     r0,r1
    CLAMP4  r1
    mov.l   r1,@(36,r9)
1:  mov.b   @r14,r0
    add     #-2,r0
    mov.b   r0,@r14
    extu.b  r0,r0
    mov     #-16,r1
    extu.b  r1,r1
    cmp/hi  r1,r0
    JF      rn_next
rn_kill:
    mov     #0,r1
    mov.l   r1,@r9
rn_next:
    mov     #44,r0
    add     r0,r9
    add     #1,r11
    add     #-1,r10
    bra     rn_loop
    nop
rn_done:
    POPALL
    rts
    nop

! ---- snow: a flake sinks, settles where it lands and fades
    .global _dc_snow_update
_dc_snow_update:
    PUSHALL
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov     #0,r11
    mov     #0,r12
sn_loop:
    tst     r10,r10
    bf      1f
    bra     sn_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    bf      sn_live
    mov.l   @(8,r8),r1
    cmp/ge  r1,r11
    JT      sn_next
    mov.l   @(12,r8),r1
    cmp/ge  r1,r12
    JT      sn_next
    add     #1,r12
    mov     r9,r4
    mov.l   @(48,r8),r1
    jsr     @r1
    nop
    mov.l   @r9,r0
    tst     r0,r0
    JT      sn_next
sn_live:
    mov     r9,r14
    add     #48,r14                     ! the life
    mov     r9,r13
    add     #24,r13                     ! stopped
    mov.l   @(0,r9),r1
    mov.l   r1,@(12,r9)
    mov.l   @(4,r9),r1
    mov.l   r1,@(16,r9)
    mov.l   @(8,r9),r1
    mov.l   r1,@(20,r9)
    mov.l   @(40,r9),r1
    mov.l   r1,@(28,r9)
    mov.b   @r14,r0
    extu.b  r0,r0
    mov.l   r0,@(32,r9)
    mov.b   @r13,r0
    tst     r0,r0
    JF      sn_settled
    mov.l   @(36,r9),r1
    mov.l   @(0,r9),r2
    add     r1,r2
    mov.l   r2,@(0,r9)
    mov.l   @(40,r9),r1
    mov     #-8,r0
    and     r0,r1
    shlr2   r1
    mov.l   @(4,r9),r2
    add     r1,r2
    mov.l   r2,@(4,r9)
    mov.l   @(44,r9),r1
    mov.l   @(8,r9),r2
    add     r1,r2
    mov.l   r2,@(8,r9)
    mov     r9,r4
    mov.l   @(52,r8),r1
    jsr     @r1
    nop
    tst     #2,r0                       ! -3: gone
    JF      sn_kill
    tst     #1,r0
    JT      sn_settled
    mov     #1,r0                       ! lands: stops where it was
    mov.b   r0,@r13
    mov.l   @(12,r9),r1
    mov.l   r1,@(0,r9)
    mov.l   @(16,r9),r1
    mov.l   r1,@(4,r9)
    mov.l   @(20,r9),r1
    mov.l   r1,@(8,r9)
    mov.b   @r14,r0
    extu.b  r0,r0
    mov     #16,r1
    cmp/hi  r1,r0
    bf      1f
    mov     r1,r0
    mov.b   r0,@r14
1:  mov.l   @(40,r9),r1
    mov     #16,r2
    cmp/gt  r2,r1
    bf      1f
    add     #-16,r1
    mov.l   r1,@(40,r9)
1:
sn_settled:
    mov.b   @r14,r0
    tst     r0,r0
    JT      sn_kill
    mov.l   @(56,r8),r3
    mov.l   @(24,r8),r1
    mov.l   @(0,r9),r2
    sub     r2,r1
    ABSR    r1
    cmp/gt  r3,r1
    bt      2f
    mov.l   @(28,r8),r1
    mov.l   @(8,r9),r2
    sub     r2,r1
    ABSR    r1
    cmp/gt  r3,r1
    bf      3f
2:  mov.b   @r14,r0                     ! far from the camera: fades fast
    extu.b  r0,r0
    mov     #16,r1
    cmp/hi  r1,r0
    bf      3f
    mov     r1,r0
    mov.b   r0,@r14
3:  mov.l   @(36,r9),r1                 ! vel.x towards twice the wind
    mov.l   @(16,r8),r2
    cmp/ge  r2,r1
    bt      4f
    add     #1,r1
    bra     5f
    nop
4:  cmp/gt  r2,r1
    bf      6f
    add     #-1,r1
5:  mov.l   r1,@(36,r9)
6:  mov.l   @(44,r9),r1
    mov.l   @(20,r8),r2
    cmp/ge  r2,r1
    bt      4f
    add     #1,r1
    bra     5f
    nop
4:  cmp/gt  r2,r1
    bf      6f
    add     #-1,r1
5:  mov.l   r1,@(44,r9)
6:  mov.b   @r14,r0
    add     #-2,r0
    mov.b   r0,@r14
    mov.l   @(40,r9),r1
    mov     r1,r0
    and     #7,r0
    cmp/eq  #7,r0
    bt      sn_next
    add     #1,r1
    mov.l   r1,@(40,r9)
    bra     sn_next
    nop
sn_kill:
    mov     #0,r1
    mov.l   r1,@r9
sn_next:
    mov     #52,r0
    add     r0,r9
    add     #1,r11
    add     #-1,r10
    bra     sn_loop
    nop
sn_done:
    POPALL
    rts
    nop

! ---- water: a speck drifts up and goes out
    .global _dc_water_update
_dc_water_update:
    PUSHALL
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov     #0,r12
wu_loop:
    tst     r10,r10
    bf      1f
    bra     wu_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    bf      wu_live
    mov.l   @(12,r8),r1
    cmp/ge  r1,r12
    JT      wu_next
    add     #1,r12
    mov     r9,r4
    mov.l   @(48,r8),r1
    jsr     @r1
    nop
    mov.l   @r9,r0
    tst     r0,r0
    JT      wu_next
wu_live:
    mov     r9,r14
    add     #24,r14
    mov.l   @(0,r9),r1
    mov.l   r1,@(12,r9)
    mov.l   @(4,r9),r1
    mov.l   r1,@(16,r9)
    mov.l   @(8,r9),r1
    mov.l   r1,@(20,r9)
    mov.l   @(28,r9),r1
    mov.l   @(0,r9),r2
    add     r1,r2
    mov.l   r2,@(0,r9)
    mov.l   @(32,r9),r1
    mov     #-8,r0
    and     r0,r1
    shlr2   r1
    shlr2   r1
    shlr2   r1
    mov.l   @(4,r9),r2
    add     r1,r2
    mov.l   r2,@(4,r9)
    mov.l   @(36,r9),r1
    mov.l   @(8,r9),r2
    add     r1,r2
    mov.l   r2,@(8,r9)
    mov.b   @r14,r0
    tst     r0,r0
    bf      1f
    mov     #0,r1
    mov.l   r1,@r9
    bra     wu_next
    nop
1:  add     #-1,r0
    mov.b   r0,@r14
    mov.l   @(32,r9),r1
    mov     r1,r0
    and     #7,r0
    cmp/eq  #7,r0
    bt      wu_next
    add     #1,r1
    mov.l   r1,@(32,r9)
wu_next:
    mov     #40,r0
    add     r0,r9
    add     #-1,r10
    bra     wu_loop
    nop
wu_done:
    POPALL
    rts
    nop

! ---- rain's streaks: from a point behind the drop to the drop
!   r11 the out list  r12 room left  The frame: 0..8 to, 12..20 from.
    .global _dc_rain_draw
_dc_rain_draw:
    PUSHALL
    add     #-32,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov     #0,r0
    mov.l   r0,@(16,r8)
    mov     #24,r0
    fmov.s  @(r0,r8),fr4
rd_loop:
    tst     r10,r10
    bf      1f
    bra     rd_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    JT      rd_next
    tst     r12,r12
    JT      rd_next
    mov.l   @(20,r8),r0
    tst     r0,r0
    bt      rd_plain
    INTERPX 0, 12, 0
    INTERPX 4, 16, 4
    INTERPX 8, 20, 8
    mov.l   @(24,r9),r2                 ! yv = prev_yv + (yv - prev_yv) * ratio
    mov.l   @(32,r9),r1
    sub     r2,r1
    lds     r1,fpul
    float   fpul,fr0
    fmul    fr4,fr0
    lds     r2,fpul
    float   fpul,fr1
    fadd    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r3
    bra     rd_from
    nop
rd_plain:
    mov.l   @(0,r9),r0
    mov.l   r0,@(0,r15)
    mov.l   @(4,r9),r0
    mov.l   r0,@(4,r15)
    mov.l   @(8,r9),r0
    mov.l   r0,@(8,r15)
    mov.l   @(32,r9),r3
rd_from:
    shll2   r3
    shll    r3                          ! yv * 8
    mov.l   @(0,r15),r1
    mov     #52,r0
    mov.l   @(r0,r8),r2
    sub     r2,r1
    mov.l   r1,@(12,r15)
    mov.l   @(4,r15),r1
    sub     r3,r1
    mov.l   r1,@(16,r15)
    mov.l   @(8,r15),r1
    mov     #56,r0
    mov.l   @(r0,r8),r2
    sub     r2,r1
    mov.l   r1,@(20,r15)
    mov     #32,r0
    add     r0,r11
    mov.l   @(40,r8),r1
    mov.l   r1,@-r11
    mov.l   @(36,r8),r1
    mov.l   r1,@-r11
    mov.l   @(8,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov.l   @(4,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov.l   @(0,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov.l   @(20,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov.l   @(16,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov.l   @(12,r15),r1
    lds     r1,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11
    mov     #32,r0
    add     r0,r11
    mov.l   @(16,r8),r1
    add     #1,r1
    mov.l   r1,@(16,r8)
    add     #-1,r12
rd_next:
    mov     #44,r0
    add     r0,r9
    add     #-1,r10
    bra     rd_loop
    nop
rd_done:
    add     #32,r15
    POPALL
    rts
    nop

! ---- snow's quads. The frame: 0..8 the centre, 12 vpos_z.
!   r11 the out list  r12 room left  r13 the picture  r14 the view matrix + 64
    .global _dc_snow_draw
_dc_snow_draw:
    PUSHALL
    add     #-32,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov.l   @(44,r8),r13
    mov.l   @(48,r8),r14
    mov     #64,r0
    add     r0,r14
    mov     #0,r0
    mov.l   r0,@(16,r8)
    mov     #24,r0
    fmov.s  @(r0,r8),fr4
sd_loop:
    tst     r10,r10
    bf      1f
    bra     sd_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    JT      sd_next
    mov     r9,r7
    add     #48,r7                      ! the life
    mov.l   @(20,r8),r0
    tst     r0,r0
    bt      sd_plain
    INTERPX 0, 12, 0
    INTERPX 4, 16, 4
    INTERPX 8, 20, 8
    bra     sd_depth
    nop
sd_plain:
    mov.l   @(0,r9),r0
    mov.l   r0,@(0,r15)
    mov.l   @(4,r9),r0
    mov.l   r0,@(4,r15)
    mov.l   @(8,r9),r0
    mov.l   r0,@(8,r15)
sd_depth:
    mov.l   @(0,r15),r4
    mov.l   @(4,r15),r5
    mov.l   @(8,r15),r6
    DEPTH64
    VPOSZ
    mov.l   r1,@(12,r15)
    mov     #127,r0
    cmp/gt  r0,r1
    bt      sd_far
    mov.b   @r7,r0                      ! behind 128: a quick fade
    extu.b  r0,r0
    mov     #16,r2
    cmp/hi  r2,r0
    bf      sd_far
    mov     r2,r0
    mov.b   r0,@r7
sd_far:
    mov.l   @(28,r8),r2                 ! zv <= near: out
    mov     r2,r3
    mov     #-31,r0
    shad    r0,r3
    cmp/gt  r3,r5
    bt      2f
    cmp/eq  r3,r5
    JF      sd_next
    cmp/hi  r2,r4
    JF      sd_next
2:  mov.l   @(32,r8),r2                 ! zv >= far: out
    mov     r2,r3
    mov     #-31,r0
    shad    r0,r3
    cmp/gt  r5,r3
    bt      3f
    cmp/eq  r5,r3
    JF      sd_next
    cmp/hi  r4,r2
    JF      sd_next
3:  mov.l   @(12,r15),r1
    mov     #127,r0
    cmp/gt  r0,r1
    JF      sd_next
    mov.l   @(60,r8),r2
    tst     r2,r2
    JT      sd_next
    tst     r12,r12
    JT      sd_next
    mov.l   @(20,r8),r0
    tst     r0,r0
    bt      sd_nint
    mov.l   @(28,r9),r2                 ! yv, life between two ticks
    mov.l   @(40,r9),r3
    sub     r2,r3
    lds     r3,fpul
    float   fpul,fr0
    fmul    fr4,fr0
    lds     r2,fpul
    float   fpul,fr1
    fadd    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r2
    mov.l   @(32,r9),r6
    mov.b   @r7,r0
    extu.b  r0,r0
    sub     r6,r0
    lds     r0,fpul
    float   fpul,fr0
    fmul    fr4,fr0
    lds     r6,fpul
    float   fpul,fr1
    fadd    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r3
    bra     sd_col
    nop
sd_nint:
    mov.l   @(40,r9),r2
    mov.b   @r7,r0
    extu.b  r0,r3
sd_col:
    mov     r2,r0
    and     #7,r0
    cmp/eq  #7,r0
    bf      4f
    mov     #18,r1
    cmp/gt  r1,r3
    bt      5f
    mov     r3,r0
    bra     4f
    nop
5:  mov     #15,r0
4:  shll2   r0
    shll    r0                          ! << 3
    mov     #-1,r1
    extu.b  r1,r1
    cmp/hi  r1,r0
    bf      6f
    mov     r1,r0
6:  mov.l   @(36,r8),r1                 ! the fog
    mov.l   @(40,r8),r2
    cmp/gt  r1,r2
    bf      sd_argb
    mov.l   @(12,r15),r3
    cmp/gt  r1,r3
    bf      sd_argb
    sub     r1,r3
    lds     r3,fpul
    float   fpul,fr0
    sub     r1,r2
    lds     r2,fpul
    float   fpul,fr1
    fdiv    fr1,fr0                     ! t
    fldi0   fr2
    fcmp/gt fr0,fr2
    bf      7f
    fmov    fr2,fr0
7:  fldi1   fr3
    fcmp/gt fr3,fr0
    bf      8f
    fmov    fr3,fr0
8:  fsub    fr0,fr3                     ! 1 - t
    lds     r0,fpul
    float   fpul,fr1
    fmul    fr3,fr1
    ftrc    fr1,fpul
    sts     fpul,r0
sd_argb:
    mov     r0,r3
    shll8   r3
    or      r0,r3
    shll8   r3
    or      r0,r3
    mov     #-1,r1
    shll16  r1
    shll8   r1
    or      r1,r3
    mov.l   @(0,r15),r1
    lds     r1,fpul
    float   fpul,fr1
    mov.l   @(4,r15),r1
    lds     r1,fpul
    float   fpul,fr2
    mov.l   @(8,r15),r1
    lds     r1,fpul
    float   fpul,fr3
    mov     #8,r1
    lds     r1,fpul
    float   fpul,fr5
    fmov    fr5,fr4                     ! half 8 (the ratio is kept below)
    PARTICLE_OUT
    mov     #24,r0
    fmov.s  @(r0,r8),fr4
    mov.l   @(16,r8),r1
    add     #1,r1
    mov.l   r1,@(16,r8)
    add     #-1,r12
sd_next:
    mov     #52,r0
    add     r0,r9
    add     #-1,r10
    bra     sd_loop
    nop
sd_done:
    add     #32,r15
    POPALL
    rts
    nop

! ---- the water's specks
    .global _dc_water_draw
_dc_water_draw:
    PUSHALL
    add     #-32,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov.l   @(44,r8),r13
    mov.l   @(48,r8),r14
    mov     #64,r0
    add     r0,r14
    mov     #0,r0
    mov.l   r0,@(16,r8)
    mov     #24,r0
    fmov.s  @(r0,r8),fr4
wd_loop:
    tst     r10,r10
    bf      1f
    bra     wd_done
    nop
1:  mov.l   @r9,r0
    tst     r0,r0
    JT      wd_next
    mov     r9,r7
    add     #24,r7                      ! the life
    mov.l   @(20,r8),r0
    tst     r0,r0
    bt      wd_plain
    INTERPX 0, 12, 0
    INTERPX 4, 16, 4
    INTERPX 8, 20, 8
    bra     wd_depth
    nop
wd_plain:
    mov.l   @(0,r9),r0
    mov.l   r0,@(0,r15)
    mov.l   @(4,r9),r0
    mov.l   r0,@(4,r15)
    mov.l   @(8,r9),r0
    mov.l   r0,@(8,r15)
wd_depth:
    mov.l   @(0,r15),r4
    mov.l   @(4,r15),r5
    mov.l   @(8,r15),r6
    DEPTH64
    VPOSZ
    mov.l   r1,@(12,r15)
    mov     #127,r0
    cmp/gt  r0,r1
    bt      wd_far
    mov.b   @r7,r0                      ! too close: fades, not drawn
    extu.b  r0,r0
    mov     #16,r2
    cmp/hi  r2,r0
    JF      wd_next
    mov     r2,r0
    mov.b   r0,@r7
    bra     wd_next
    nop
wd_far:
    mov.l   @(28,r8),r2
    mov     r2,r3
    mov     #-31,r0
    shad    r0,r3
    cmp/gt  r3,r5
    bt      2f
    cmp/eq  r3,r5
    JF      wd_next
    cmp/hi  r2,r4
    JF      wd_next
2:  mov.l   @(32,r8),r2
    mov     r2,r3
    mov     #-31,r0
    shad    r0,r3
    cmp/gt  r5,r3
    bt      3f
    cmp/eq  r5,r3
    JF      wd_next
    cmp/hi  r4,r2
    JF      wd_next
3:  tst     r12,r12
    JT      wd_next
    mov.l   @(32,r9),r2                 ! size = 381 * (vel.y >> 3) / vpos_z
    shar    r2
    shar    r2
    shar    r2
    mov     #127,r0
    mov     r0,r1
    add     r1,r0
    add     r1,r0                       ! 381
    mul.l   r0,r2
    sts     macl,r2
    lds     r2,fpul
    float   fpul,fr0
    mov.l   @(12,r15),r1
    lds     r1,fpul
    float   fpul,fr6                    ! vpos_z
    fdiv    fr6,fr0
    ftrc    fr0,fpul
    sts     fpul,r2
    tst     r2,r2
    bf      4f
    mov     #6,r2
    bra     5f
    nop
4:  mov     #12,r1
    cmp/gt  r1,r2
    bf      5f
    mov     r1,r2
5:  lds     r2,fpul
    float   fpul,fr5
    fmul    fr6,fr5                     ! size * vpos_z / (381 * 3)
    mov     #52,r0
    fmov.s  @(r0,r8),fr7
    fdiv    fr7,fr5
    mov.l   @(32,r9),r2
    mov     r2,r0
    and     #7,r0
    cmp/eq  #7,r0
    bf      6f
    mov.b   @r7,r0
    extu.b  r0,r0
    mov     #18,r1
    cmp/gt  r1,r0
    bf      6f
    mov     #15,r0
6:  shll2   r0
    shll2   r0                          ! << 4
    mov     #-1,r1
    extu.b  r1,r1
    cmp/hi  r1,r0
    bf      7f
    mov     r1,r0
7:  mov     r0,r3
    shll8   r3
    or      r0,r3
    shll8   r3
    or      r0,r3
    mov     #-1,r1
    shll16  r1
    shll8   r1
    or      r1,r3
    mov.l   @(0,r15),r1
    lds     r1,fpul
    float   fpul,fr1
    mov.l   @(4,r15),r1
    lds     r1,fpul
    float   fpul,fr2
    mov.l   @(8,r15),r1
    lds     r1,fpul
    float   fpul,fr3
    fmov    fr5,fr4
    PARTICLE_OUT
    mov     #24,r0
    fmov.s  @(r0,r8),fr4
    mov.l   @(16,r8),r1
    add     #1,r1
    mov.l   r1,@(16,r8)
    add     #-1,r12
wd_next:
    mov     #40,r0
    add     r0,r9
    add     #-1,r10
    bra     wd_loop
    nop
wd_done:
    add     #32,r15
    POPALL
    rts
    nop

! ---- the walk over the active effects
!   void dc_effect_control(EFFCTL *);
!   0 the effects  4 the first active  8 the C function that gives the control
!   function of an object  12 sizeof(EFFECT)  16 the offset of next_active
!   20 the offset of object_id
    .global _dc_effect_control
_dc_effect_control:
    PUSHALL
    mov     r4,r8
    mov.l   @(4,r8),r9
ec_loop:
    mov     #-1,r0
    cmp/eq  r0,r9
    bf      1f
    bra     ec_done
    nop
1:  mov.l   @(12,r8),r1
    mul.l   r9,r1
    sts     macl,r1
    mov.l   @(0,r8),r2
    add     r2,r1
    mov     r1,r12                      ! the effect
    mov.l   @(16,r8),r0
    mov.w   @(r0,r12),r0
    mov     r0,r10                      ! next_active (sign extended)
    mov.l   @(20,r8),r0
    mov.l   @(r0,r12),r4                ! object_id
    mov.l   @(8,r8),r1
    jsr     @r1
    nop
    tst     r0,r0
    bt      ec_next
    mov     r9,r4
    jsr     @r0
    nop
ec_next:
    mov     r10,r9
    bra     ec_loop
    nop
ec_done:
    POPALL
    rts
    nop
