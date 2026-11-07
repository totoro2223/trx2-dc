! Hand-written SH-4 for the test an instance's bounding sphere gets before it is
! transformed (batcher.c, M_TransformInstance): its middle in the eye space, its
! radius, and from them whether it lies wholly past the end of the fog, wholly
! before the fog starts, wholly past the near plane, wholly past one side of the
! screen or wholly within them all.
!
!   int dc_inst_cull(M_CULL *p);
!
! M_CULL: 0 mv (the 16 floats, GL's layout)  4 lo x y z  16 hi x y z  28 scale
!   32 fog_end  36 fog_start  40 near_z  44 planes (4 pairs a, b)
!   76 fog (the frame has fog)  80 billboard
! out:  84 eye_sq  88 z_near (the eye z less the radius)  92 flags  96 eye x y z
! flags: 1 wholly past the fog's end (reject)  2 wholly before the fog starts
!   4 some of it may lie before the near plane  8 some of it may lie past a
!   side (always, for a billboard)  16 wholly past one side (reject)
! The result is also returned. Uses fr0-fr11 only.

    .text
    .align 2
    .global _dc_inst_cull

_dc_inst_cull:
    mov.l   @r4,r6                  ! mv
    mov     r4,r2
    add     #4,r2
    fmov.s  @r2+,fr0
    fmov.s  @r2+,fr1
    fmov.s  @r2+,fr2                ! lo
    fmov.s  @r2+,fr4
    fmov.s  @r2+,fr5
    fmov.s  @r2+,fr6                ! hi
    mov.l   .Lhalf,r0
    lds     r0,fpul
    fsts    fpul,fr7
    fsub    fr0,fr4
    fsub    fr1,fr5
    fsub    fr2,fr6
    fmul    fr7,fr4
    fmul    fr7,fr5
    fmul    fr7,fr6                 ! half
    fadd    fr4,fr0
    fadd    fr5,fr1
    fadd    fr6,fr2                 ! the middle
    fmul    fr4,fr4
    fmul    fr5,fr5
    fmul    fr6,fr6
    fadd    fr5,fr4
    fadd    fr6,fr4                 ! radius squared
    fsqrt   fr4
    fmov.s  @r2,fr8                 ! scale
    fmul    fr8,fr4                 ! the radius
    fldi1   fr3
    mov     r4,r7
    add     #108,r7                 ! the eye position goes in at 96, last first
    mov     r6,r5
    add     #8,r5                   ! &mv[0][2]
    mov     #3,r3
1:  mov     r5,r1
    fmov.s  @r1,fr8
    add     #16,r1
    fmov.s  @r1,fr9
    add     #16,r1
    fmov.s  @r1,fr10
    add     #16,r1
    fmov.s  @r1,fr11
    fipr    fv0,fv8                 ! fr11 = the row . (middle, 1)
    fmov.s  fr11,@-r7
    add     #-4,r5
    dt      r3
    bf      1b
    fmov.s  @r7+,fr8
    fmov.s  @r7+,fr9
    fmov.s  @r7+,fr10               ! the eye position
    fldi0   fr11
    fipr    fv8,fv8                 ! fr11 = eye squared
    mov     r4,r7
    add     #84,r7
    fmov.s  fr11,@r7
    fmov    fr10,fr5
    fsub    fr4,fr5                 ! z - radius
    add     #4,r7
    fmov.s  fr5,@r7
    mov     #0,r3                   ! the flags
    mov     r4,r2
    add     #32,r2
    fmov.s  @r2+,fr0                ! fog_end
    fmov.s  @r2+,fr1                ! fog_start
    fmov.s  @r2+,fr2                ! near_z; r2 is now at the planes
    mov     r4,r6
    add     #76,r6
    mov.l   @r6+,r1                 ! fog
    mov.l   @r6,r6                  ! billboard
    fldi0   fr7
    tst     r1,r1
    bt      2f
    fadd    fr4,fr0                 ! reach
    fmul    fr0,fr0
    fcmp/gt fr0,fr11
    bf      2f
    mov     #1,r0
    or      r0,r3
2:  fsub    fr4,fr1                 ! clear
    fcmp/gt fr7,fr1
    bf      3f
    fmul    fr1,fr1
    fcmp/gt fr11,fr1
    bf      3f
    mov     #2,r0
    or      r0,r3
3:  fcmp/gt fr5,fr2                 ! near_z > z - radius
    bf      4f
    mov     #4,r0
    or      r0,r3
4:  tst     r6,r6
    bt      5f
    mov     #8,r0                   ! a billboard: the sides stay in question
    or      r0,r3
    bra     9f
    nop
5:  fmov    fr4,fr6
    fmul    fr4,fr6                 ! radius squared again, as C finds it
    mov     #1,r6                   ! inside, until a side says no
    .macro SIDE ec
    fmov.s  @r2+,fr0                ! a
    fmov.s  @r2+,fr1                ! b
    fmov    fr0,fr2
    fmul    fr0,fr2
    fmov    fr1,fr3
    fmul    fr1,fr3
    fadd    fr3,fr2
    fmul    fr6,fr2                 ! rr
    fmul    \ec,fr0
    fmul    fr10,fr1
    fadd    fr1,fr0                 ! d
    fmov    fr0,fr3
    fmul    fr0,fr3                 ! d * d
    fcmp/gt fr2,fr3
    movt    r5                      ! d * d > rr
    fcmp/gt fr7,fr0
    movt    r1                      ! d > 0
    and     r5,r1
    mov     #16,r0
    tst     r1,r1
    bt      6f
    or      r0,r3
6:  fcmp/gt fr0,fr7
    movt    r1                      ! d < 0
    and     r5,r1
    and     r1,r6
    .endm
    SIDE    fr8
    SIDE    fr8
    SIDE    fr9
    SIDE    fr9
    mov     r6,r0
    tst     r0,r0
    bf      9f                      ! wholly within them all
    mov     #8,r0
    or      r0,r3
9:  mov     r4,r7
    add     #92,r7
    mov.l   r3,@r7
    rts
    mov     r3,r0

    .align 2
.Lhalf:
    .long   0x3F000000
