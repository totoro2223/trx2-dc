! Hand-written SH-4 for the relief's offset colour at one vertex (batcher.c,
! M_BumpVertex with M_BumpGatherLights, M_BumpAt, M_BumpShade and M_Atan2).
!
!   uint32_t dc_bump_vertex(M_BUMPRUN *run);
!
! The C (M_BumpVertexC) stays as the reference: a build with DC_EMIT_VERIFY runs
! both and compares them. The same maths, in the same order: fsrra is the C
! M_InvSqrt; dot products are fipr's.
!
! Struct M_BUMPRUN (batcher.c):
!    0 lights  4 frame  8 vert  12 glow  16 fade out  20 by_light out
!   24 K (the constants)  28 view[1][0]  32 view[1][1]  36 view[1][2]
!   40 light count  44 radius  48 power  52 passing (the arrays of the lights)
! M_BUMP_LIGHTS: pos[16][3] 0, radius 192, power 256, passing 320 (bytes)
! M_BUMP_FRAME: t 0, bt 12, n 24, tl 36, bl 40, nl 44
! M_XVERT: x 0, y 4, z 8, argb 24
!
! Registers (r8-r14 and fr12-fr15 are kept):
!   r8 run  r9 lights  r10 frame  r11 vert  r12 glow  r13 locals
!   r14 K + 128 (so that the 33 constants lie within a signed byte)
!   fr14 1.0  fr15 0.0
! The locals, in bytes from r13: 0 pos  12 room_pos  24 passing_pos
!   36 room_total  40 passing  44 light  56 scale  60 tilt  64 room_dir
!   76 passing_dir  (from the BumpAt on: 12 l  24 steady_smooth  28 shine_smooth
!   32 strength  76 h)

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

! A constant of the table, by index.
    .macro KL fr, idx
    mov     #(4*\idx-128),r0
    fmov.s  @(r0,r14),\fr
    .endm

    .macro LDL fr, off
    mov     #\off,r0
    fmov.s  @(r0,r13),\fr
    .endm

    .macro STL fr, off
    mov     #\off,r0
    fmov.s  \fr,@(r0,r13)
    .endm

! Three floats at base + off into fv0 (w 0) or fv4.
    .macro LV0 base, off
    mov     \base,r1
    add     #\off,r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi0   fr3
    .endm

    .macro LV4 base, off
    mov     \base,r1
    add     #\off,r1
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fldi0   fr7
    .endm

! M_Clamp(x, lo, hi), x in \f.
    .macro CLAMP f, lo, hi
    fcmp/gt \f,\lo
    bf      1f
    fmov    \lo,\f
    bra     2f
    nop
1:  fcmp/gt \hi,\f
    bf      2f
    fmov    \hi,\f
2:
    .endm

    .macro CLAMP01 f
    CLAMP   \f, fr15, fr14
    .endm

! M_PackUnit(\src) into r1 (destroys fr12, fr13).
    .macro PACKU src
    fmov    \src,fr12
    CLAMP01 fr12
    KL      fr13, 12
    fmul    fr13,fr12
    KL      fr13, 13
    fadd    fr13,fr12
    ftrc    fr12,fpul
    sts     fpul,r1
    .endm

! The dot product of the two vectors at fv0 and fv4 into fr7.
    .macro DOT
    fipr    fv0,fv4
    .endm

    .text
    .align 2
    .global _dc_bump_vertex
_dc_bump_vertex:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    add     #-128,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov.l   @(24,r8),r14
    add     #127,r14
    add     #1,r14
    mov     r15,r13
    fldi1   fr14
    fldi0   fr15

    ! the vertex's place, and the sums of the lights at zero
    fmov.s  @r11,fr0
    mov     #4,r0
    fmov.s  @(r0,r11),fr1
    mov     #8,r0
    fmov.s  @(r0,r11),fr2
    STL     fr0, 0
    STL     fr1, 4
    STL     fr2, 8
    STL     fr15, 12
    STL     fr15, 16
    STL     fr15, 20
    STL     fr15, 24
    STL     fr15, 28
    STL     fr15, 32
    STL     fr15, 36
    STL     fr15, 40

    ! M_BumpGatherLights
    mov.l   @(40,r8),r5
    tst     r5,r5
    JT      bv_gathered
    mov     r9,r1
    mov.l   @(44,r8),r2
    mov.l   @(48,r8),r3
    mov.l   @(52,r8),r4
bv_gl:
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fmov    fr4,fr7
    fsub    fr0,fr7
    fmov    fr5,fr8
    fsub    fr1,fr8
    fmov    fr6,fr9
    fsub    fr2,fr9
    fmov    fr7,fr10
    fmul    fr7,fr10
    fmov    fr8,fr11
    fmul    fr8,fr11
    fadd    fr11,fr10
    fmov    fr9,fr11
    fmul    fr9,fr11
    fadd    fr11,fr10               ! dist_sq
    fmov.s  @r2+,fr11
    fmov    fr11,fr12
    fmul    fr11,fr12               ! radius squared
    fcmp/gt fr10,fr12
    bf      bv_gnext
    fdiv    fr12,fr10               ! ratio_sq
    fmov    fr10,fr11
    fsqrt   fr11
    fmov    fr14,fr12
    fsub    fr11,fr12               ! near
    fmov.s  @r3,fr11
    fmul    fr12,fr11
    fmul    fr12,fr11
    fmov    fr14,fr13
    fadd    fr10,fr13
    fdiv    fr13,fr11               ! weight
    mov.b   @r4,r0
    tst     r0,r0
    mov     r13,r6
    add     #12,r6
    mov     r13,r7
    add     #36,r7
    bt      bv_groom
    add     #12,r6
    add     #4,r7
bv_groom:
    fmov.s  @r6,fr8
    fmov    fr4,fr9
    fmul    fr11,fr9
    fadd    fr9,fr8
    fmov.s  fr8,@r6
    add     #4,r6
    fmov.s  @r6,fr8
    fmov    fr5,fr9
    fmul    fr11,fr9
    fadd    fr9,fr8
    fmov.s  fr8,@r6
    add     #4,r6
    fmov.s  @r6,fr8
    fmov    fr6,fr9
    fmul    fr11,fr9
    fadd    fr9,fr8
    fmov.s  fr8,@r6
    fmov.s  @r7,fr8
    fadd    fr11,fr8
    fmov.s  fr8,@r7
bv_gnext:
    add     #4,r3
    add     #1,r4
    dt      r5
    JF      bv_gl
bv_gathered:

    ! the blend of the lights: scale, and the place the light is taken at
    LDL     fr0, 36                 ! room_total
    LDL     fr1, 40                 ! passing
    fmov    fr0,fr2
    fadd    fr1,fr2                 ! total
    fmov    fr14,fr3                ! scale
    fcmp/gt fr15,fr2
    bf      bv_scaled
    KL      fr4, 1
    fmul    fr2,fr4
    CLAMP01 fr4
    fmov    fr4,fr3
bv_scaled:
    STL     fr3, 56
    fcmp/gt fr15,fr0
    bf      bv_nolight
    LDL     fr4, 12
    fdiv    fr0,fr4
    STL     fr4, 44
    LDL     fr4, 16
    fdiv    fr0,fr4
    STL     fr4, 48
    LDL     fr4, 20
    fdiv    fr0,fr4
    STL     fr4, 52
    bra     bv_lit
    nop
bv_nolight:
    KL      fr6, 2
    LDL     fr4, 0
    mov     #28,r0
    fmov.s  @(r0,r8),fr5
    fmul    fr6,fr5
    fsub    fr5,fr4
    STL     fr4, 44
    LDL     fr4, 4
    mov     #32,r0
    fmov.s  @(r0,r8),fr5
    fmul    fr6,fr5
    fsub    fr5,fr4
    STL     fr4, 48
    LDL     fr4, 8
    mov     #36,r0
    fmov.s  @(r0,r8),fr5
    fmul    fr6,fr5
    fsub    fr5,fr4
    STL     fr4, 52
bv_lit:

    ! a passing light turns the direction
    LDL     fr1, 40
    fcmp/gt fr15,fr1
    JF      bv_tilted
    KL      fr4, 1
    fmul    fr1,fr4
    CLAMP01 fr4
    KL      fr5, 3
    fmul    fr5,fr4
    STL     fr4, 60                 ! tilt
    LDL     fr4, 44
    LDL     fr5, 0
    fsub    fr5,fr4
    STL     fr4, 64
    LDL     fr4, 48
    LDL     fr5, 4
    fsub    fr5,fr4
    STL     fr4, 68
    LDL     fr4, 52
    LDL     fr5, 8
    fsub    fr5,fr4
    STL     fr4, 72
    LDL     fr4, 24
    fdiv    fr1,fr4
    LDL     fr5, 0
    fsub    fr5,fr4
    STL     fr4, 76
    LDL     fr4, 28
    fdiv    fr1,fr4
    LDL     fr5, 4
    fsub    fr5,fr4
    STL     fr4, 80
    LDL     fr4, 32
    fdiv    fr1,fr4
    LDL     fr5, 8
    fsub    fr5,fr4
    STL     fr4, 84
    LV0     r13, 64
    LV4     r13, 64
    DOT
    fmov    fr7,fr8                 ! room_sq
    LV4     r13, 76
    fmov    fr4,fr9
    fmov    fr5,fr10
    fmov    fr6,fr11
    LV0     r13, 76
    DOT
    fmov    fr7,fr9                 ! passing_sq
    fcmp/gt fr14,fr8
    JF      bv_tilted
    fcmp/gt fr14,fr9
    JF      bv_tilted
    fsrra   fr8                     ! room_inv
    fsrra   fr9                     ! passing_inv
    LDL     fr10, 60                ! tilt
    fmov    fr14,fr11
    fsub    fr10,fr11               ! 1 - tilt
    LDL     fr0, 64
    fmul    fr8,fr0
    fmul    fr11,fr0
    LDL     fr1, 76
    fmul    fr9,fr1
    fmul    fr10,fr1
    fadd    fr1,fr0
    STL     fr0, 64
    LDL     fr0, 68
    fmul    fr8,fr0
    fmul    fr11,fr0
    LDL     fr1, 80
    fmul    fr9,fr1
    fmul    fr10,fr1
    fadd    fr1,fr0
    STL     fr0, 68
    LDL     fr0, 72
    fmul    fr8,fr0
    fmul    fr11,fr0
    LDL     fr1, 84
    fmul    fr9,fr1
    fmul    fr10,fr1
    fadd    fr1,fr0
    STL     fr0, 72
    LV0     r13, 64
    LV4     r13, 64
    DOT
    fmov    fr7,fr8                 ! mixed_sq
    KL      fr0, 4
    fcmp/gt fr0,fr8
    JF      bv_tilted
    fsrra   fr8
    KL      fr0, 2
    fmul    fr0,fr8                 ! far
    LDL     fr0, 0
    LDL     fr1, 64
    fmul    fr8,fr1
    fadd    fr1,fr0
    STL     fr0, 44
    LDL     fr0, 4
    LDL     fr1, 68
    fmul    fr8,fr1
    fadd    fr1,fr0
    STL     fr0, 48
    LDL     fr0, 8
    LDL     fr1, 72
    fmul    fr8,fr1
    fadd    fr1,fr0
    STL     fr0, 52
bv_tilted:

    ! how much of the relief the vertex takes
    mov     #8,r0
    fmov.s  @(r0,r11),fr0           ! z
    tst     r12,r12
    bt      bv_plain
    KL      fr1, 7
    KL      fr2, 8
    bra     bv_range
    nop
bv_plain:
    KL      fr1, 5
    KL      fr2, 6
bv_range:
    fsub    fr0,fr1
    fmul    fr2,fr1
    CLAMP01 fr1                     ! near
    LDL     fr2, 40
    KL      fr3, 1
    fmul    fr2,fr3
    CLAMP01 fr3                     ! lit_by_light
    fmov    fr1,fr4
    fmul    fr1,fr4
    KL      fr5, 20
    fmul    fr1,fr5
    KL      fr6, 19
    fsub    fr5,fr6
    fmul    fr6,fr4
    LDL     fr7, 56
    fmul    fr7,fr4
    fcmp/gt fr3,fr4
    bt      bv_fade
    fmov    fr3,fr4
bv_fade:
    mov.l   @(16,r8),r1
    fmov.s  fr4,@r1
    mov.l   @(20,r8),r2
    fmov.s  fr3,@r2
    tst     r12,r12
    bt      bv_depth
    KL      fr5, 10
    fmul    fr4,fr5                 ! strength
    mov     #24,r0
    mov.l   @(r0,r11),r1            ! argb
    mov     r1,r2
    shlr16  r2
    extu.b  r2,r2                   ! r
    mov     r1,r3
    shlr8   r3
    extu.b  r3,r3                   ! g
    extu.b  r1,r1                   ! b
    cmp/gt  r2,r3
    bf      bv_m1
    mov     r3,r2
bv_m1:
    cmp/gt  r2,r1
    bf      bv_m2
    mov     r1,r2
bv_m2:
    lds     r2,fpul
    float   fpul,fr6
    KL      fr7, 11
    fmul    fr7,fr6                 ! lit
    fmul    fr6,fr6
    fmul    fr6,fr5
    bra     bv_strength
    nop
bv_depth:
    KL      fr5, 9
    fmul    fr4,fr5
bv_strength:
    STL     fr5, 32

    ! M_BumpAt: l, the light from the vertex
    LDL     fr0, 44
    LDL     fr1, 0
    fsub    fr1,fr0
    STL     fr0, 12
    LDL     fr0, 48
    LDL     fr1, 4
    fsub    fr1,fr0
    STL     fr0, 16
    LDL     fr0, 52
    LDL     fr1, 8
    fsub    fr1,fr0
    STL     fr0, 20
    LV0     r13, 12
    LV4     r13, 12
    DOT
    fmov    fr7,fr8                 ! l_sq
    LV4     r13, 0
    LV0     r13, 0
    DOT
    fmov    fr7,fr9                 ! c_sq
    tst     r12,r12
    JT      bv_noglow
    fcmp/gt fr15,fr8
    JF      bv_noglow
    fcmp/gt fr15,fr9
    JF      bv_noglow
    fmov    fr8,fr10
    fsrra   fr10                    ! li
    fmov    fr9,fr11
    fsrra   fr11                    ! ci
    LDL     fr0, 12
    fmul    fr10,fr0
    LDL     fr1, 0
    fmul    fr11,fr1
    fsub    fr1,fr0
    STL     fr0, 76
    LDL     fr0, 16
    fmul    fr10,fr0
    LDL     fr1, 4
    fmul    fr11,fr1
    fsub    fr1,fr0
    STL     fr0, 80
    LDL     fr0, 20
    fmul    fr10,fr0
    LDL     fr1, 8
    fmul    fr11,fr1
    fsub    fr1,fr0
    STL     fr0, 84
    LV0     r13, 76
    LV4     r13, 76
    DOT
    KL      fr0, 14
    fcmp/gt fr0,fr7
    JF      bv_noglow
    LDL     fr0, 76
    STL     fr0, 12
    LDL     fr0, 80
    STL     fr0, 16
    LDL     fr0, 84
    STL     fr0, 20
bv_noglow:
    LV0     r13, 12
    LV4     r13, 12
    DOT
    fmov    fr7,fr8                 ! ll
    fcmp/gt fr15,fr8
    JF      bv_ret0
    LV4     r10, 0
    DOT
    fmov    fr7,fr9
    mov     #36,r0
    fmov.s  @(r0,r10),fr4
    fmul    fr8,fr4
    fsrra   fr4
    fmul    fr4,fr9                 ! lx
    LV4     r10, 12
    DOT
    fmov    fr7,fr10
    mov     #40,r0
    fmov.s  @(r0,r10),fr4
    fmul    fr8,fr4
    fsrra   fr4
    fmul    fr4,fr10                ! ly
    LV4     r10, 24
    DOT
    fmov    fr7,fr11
    mov     #44,r0
    fmov.s  @(r0,r10),fr4
    fmul    fr8,fr4
    fsrra   fr4
    fmul    fr4,fr11                ! lz
    fmov    fr11,fr0
    fmul    fr11,fr0
    fmov    fr14,fr1
    fsub    fr0,fr1
    fcmp/gt fr15,fr1
    bt      bv_off
    fmov    fr15,fr1
bv_off:
    fsqrt   fr1                     ! off
    KL      fr2, 15
    fmov    fr1,fr3
    fsub    fr2,fr3
    KL      fr2, 16
    fmul    fr2,fr3
    CLAMP01 fr3
    fmov    fr3,fr4
    fmul    fr3,fr4
    KL      fr5, 20
    fmul    fr3,fr5
    KL      fr6, 19
    fsub    fr5,fr6
    fmul    fr6,fr4
    STL     fr4, 24                 ! steady_smooth
    KL      fr2, 17
    fmov    fr1,fr3
    fsub    fr2,fr3
    KL      fr2, 18
    fmul    fr2,fr3
    CLAMP01 fr3
    fmov    fr3,fr4
    fmul    fr3,fr4
    KL      fr5, 20
    fmul    fr3,fr5
    KL      fr6, 19
    fsub    fr5,fr6
    fmul    fr6,fr4
    STL     fr4, 28                 ! shine_smooth
    KL      fr2, 21
    CLAMP   fr11, fr2, fr14         ! lz

    ! the direction Q, by M_Atan2
    fmov    fr9,fr0
    fabs    fr0                     ! |x|
    fmov    fr10,fr1
    fabs    fr1                     ! |y|
    fcmp/gt fr0,fr1
    bt      bv_ylarger
    fmov    fr1,fr2
    fmov    fr0,fr3
    bra     bv_lohi
    nop
bv_ylarger:
    fmov    fr0,fr2
    fmov    fr1,fr3
bv_lohi:
    fldi0   fr6
    fcmp/gt fr15,fr3
    bf      bv_atan_done
    fmov    fr2,fr4
    fdiv    fr3,fr4                 ! a
    fmov    fr4,fr5
    fmul    fr4,fr5                 ! s
    KL      fr6, 22
    fmul    fr5,fr6
    KL      fr7, 23
    fadd    fr7,fr6
    fmul    fr5,fr6
    KL      fr7, 24
    fsub    fr7,fr6
    fmul    fr5,fr6
    fmul    fr4,fr6
    fadd    fr4,fr6                 ! r
    fcmp/gt fr0,fr1
    bf      bv_atan_a
    KL      fr7, 25
    fsub    fr6,fr7
    fmov    fr7,fr6
bv_atan_a:
    fcmp/gt fr9,fr15
    bf      bv_atan_b
    KL      fr7, 26
    fsub    fr6,fr7
    fmov    fr7,fr6
bv_atan_b:
    fcmp/gt fr10,fr15
    bf      bv_atan_done
    fneg    fr6
bv_atan_done:
    fcmp/gt fr6,fr15
    bf      bv_q
    KL      fr7, 27
    fadd    fr7,fr6
bv_q:
    KL      fr7, 28
    fmul    fr7,fr6
    ftrc    fr6,fpul
    sts     fpul,r7
    extu.b  r7,r7                   ! q8

    tst     r12,r12
    bt      bv_shade
    LDL     fr4, 32
    LDL     fr5, 28
    fmul    fr5,fr4
    PACKU   fr4
    shll8   r1
    or      r7,r1
    bra     bv_exit
    mov     r1,r0

    ! M_BumpShade
bv_shade:
    LDL     fr0, 32                 ! depth
    LDL     fr1, 24                 ! steady_smooth
    KL      fr4, 31
    KL      fr5, 32
    fmov    fr0,fr6
    fmul    fr5,fr6
    fmul    fr6,fr4
    fmov    fr14,fr7
    fsub    fr4,fr7                 ! flat
    fmov    fr0,fr8
    fmul    fr11,fr8
    fcmp/gt fr8,fr7
    bt      bv_k2
    fmov    fr7,fr8
bv_k2:                              ! fr8: k2
    fmov    fr11,fr9
    fmul    fr9,fr9
    fmov    fr14,fr10
    fsub    fr9,fr10
    fsqrt   fr10
    fmov    fr0,fr9
    fmul    fr10,fr9
    fmul    fr1,fr9                 ! k3
    KL      fr10, 30                ! keep
    fmov    fr7,fr4
    fsub    fr8,fr4
    fmul    fr10,fr4
    KL      fr5, 29
    fadd    fr5,fr4
    PACKU   fr4
    shll16  r1
    shll8   r1
    mov     r1,r2
    fmov    fr8,fr4
    fmul    fr10,fr4
    PACKU   fr4
    shll16  r1
    or      r1,r2
    fmov    fr9,fr4
    fmul    fr10,fr4
    PACKU   fr4
    shll8   r1
    or      r1,r2
    or      r7,r2
    bra     bv_exit
    mov     r2,r0

bv_ret0:
    mov     #0,r0
bv_exit:
    add     #127,r15
    add     #1,r15
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
