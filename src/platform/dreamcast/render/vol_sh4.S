! Hand-written SH-4 for the shadow volume of a box (batcher.c, M_BuildVolume).
!
!   int32_t dc_build_volume(M_VOLRUN *run);
!
! What M_BuildVolumeC does: the box's eight corners in world space, the push of
! each along the shadow (from the light, or the common direction), the length cut
! short of the near plane, the corners and their pushed copies projected, the
! faces towards the light, the triangles of the near and far caps and those of
! the outline between a lit and an unlit face. Returns the triangles written to
! run->out (0: no volume).
!
! The caller loads the frame's view matrix into XMTRX (mat_load): the dot
! products of the projection are ftrv's. fsrra stands for 1/sqrt, as in the C
! M_InvSqrt (the C also divides by a root where the push is tilted).
!
! Registers (r8-r14 and fr12-fr15 are kept):
!   r8 run  r9 corners  r10 push  r11 near  r12 far  r13 misc  r14 out cursor
!   (r9 and r10 are the face counters from the face loop on)
! Frame (432 bytes) from r15: corners 0, push 96, near 192, far 288, misc 384:
!   misc +0 center (3 floats)  +20 lit[6] bytes  +28 the pushed corner
!
! Struct M_VOLRUN (batcher.c):
!    0 caster  4 dir  16 origin  28 has_origin  32 length_max  36 out
!   40 near_z  44 proj_x  48 proj_y  52 center_x  56 center_y  60 view
!   64 depth  68 faces  72 min_drop  76 margin (near_z * 1.5)  80 32.0
!   84 0.125  88 0.25
! M_CASTER: 4 bounds[2][3], 28 world[3][4]

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

! One corner of the box, in world space: bit 0 of i picks x's bound, bit 1 y's,
! bit 2 z's. The three rows of the world matrix are in fv4, fv8 and fv12.
    .macro CORNER i, b0, b1, b2
    mov     #(4+12*\b0),r0
    fmov.s  @(r0,r7),fr0
    mov     #(8+12*\b1),r0
    fmov.s  @(r0,r7),fr1
    mov     #(12+12*\b2),r0
    fmov.s  @(r0,r7),fr2
    fldi1   fr3
    fipr    fv4,fv0
    mov     #(12*\i),r0
    fmov.s  fr3,@(r0,r9)
    fldi1   fr3
    fipr    fv8,fv0
    mov     #(12*\i+4),r0
    fmov.s  fr3,@(r0,r9)
    fldi1   fr3
    fipr    fv12,fv0
    mov     #(12*\i+8),r0
    fmov.s  fr3,@(r0,r9)
    .endm

! The next corner of a face (the face's index list at r4), a quarter of it
! added to fr0-fr2.
    .macro FACEPT
    mov.b   @r4+,r0
    mov     r0,r1
    shll2   r1
    mov     r1,r2
    shll    r2
    add     r2,r1
    add     r9,r1
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fmul    fr15,fr4
    fmul    fr15,fr5
    fmul    fr15,fr6
    fadd    fr4,fr0
    fadd    fr5,fr1
    fadd    fr6,fr2
    .endm

! Index \i of face f (r1) against each of face g's (r2): a shared corner goes to
! r6, then r7; r3 counts them.
    .macro SHARE i
    mov.b   @(\i,r1),r0
    mov     r0,r4
    SHARE1  0
    SHARE1  1
    SHARE1  2
    SHARE1  3
    .endm

    .macro SHARE1 j
    mov.b   @(\j,r2),r0
    cmp/eq  r4,r0
    bf      9f
    mov     #2,r0
    cmp/ge  r0,r3
    bt      9f
    tst     r3,r3
    bf      8f
    mov     r4,r6
    bra     7f
    nop
8:  mov     r4,r7
7:  add     #1,r3
9:
    .endm

! A vertex of the near or far set (base in r4, its index in \idx) to the output.
    .macro EVB base, idx
    mov     \base,r4
    mov     \idx,r2
    bsr     vb_emit_v
    nop
    .endm

    .text
    .align 2
    .global _dc_build_volume
_dc_build_volume:
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
    sts.l   pr,@-r15
    add     #-108,r15
    add     #-108,r15
    add     #-108,r15
    add     #-108,r15
    mov     r4,r8
    mov     r15,r9
    mov     r15,r10
    add     #96,r10
    mov     r10,r11
    add     #96,r11
    mov     r11,r12
    add     #96,r12
    mov     r12,r13
    add     #96,r13

    ! the eight corners: the world matrix's rows in fv4, fv8, fv12
    mov.l   @(0,r8),r7
    mov     r7,r1
    add     #28,r1
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fmov.s  @r1+,fr7
    fmov.s  @r1+,fr8
    fmov.s  @r1+,fr9
    fmov.s  @r1+,fr10
    fmov.s  @r1+,fr11
    fmov.s  @r1+,fr12
    fmov.s  @r1+,fr13
    fmov.s  @r1+,fr14
    fmov.s  @r1+,fr15
    CORNER  0, 0, 0, 0
    CORNER  1, 1, 0, 0
    CORNER  2, 0, 1, 0
    CORNER  3, 1, 1, 0
    CORNER  4, 0, 0, 1
    CORNER  5, 1, 0, 1
    CORNER  6, 0, 1, 1
    CORNER  7, 1, 1, 1

    ! the box's middle: a eighth of each corner, added
    mov     #84,r0
    fmov.s  @(r0,r8),fr14
    fldi0   fr0
    fldi0   fr1
    fldi0   fr2
    mov     r9,r1
    mov     #8,r2
vb_center:
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fmul    fr14,fr4
    fmul    fr14,fr5
    fmul    fr14,fr6
    fadd    fr4,fr0
    fadd    fr5,fr1
    fadd    fr6,fr2
    dt      r2
    bf      vb_center
    fmov.s  fr0,@r13
    mov     #4,r0
    fmov.s  fr1,@(r0,r13)
    mov     #8,r0
    fmov.s  fr2,@(r0,r13)

    ! the push of each corner, and the length that keeps the far end before
    ! the near plane
    mov     r9,r2
    mov     r10,r3
    mov     #8,r4
    mov     #16,r0
    fmov.s  @(r0,r8),fr8
    mov     #20,r0
    fmov.s  @(r0,r8),fr9
    mov     #24,r0
    fmov.s  @(r0,r8),fr10
    mov     #72,r0
    fmov.s  @(r0,r8),fr11
    mov     #32,r0
    fmov.s  @(r0,r8),fr12
vb_loop:
    fmov.s  @r2+,fr0
    fmov.s  @r2+,fr1
    fmov.s  @r2+,fr2
    mov.l   @(28,r8),r0
    tst     r0,r0
    JT      vb_dir
    fmov    fr0,fr4
    fsub    fr8,fr4
    fmov    fr1,fr5
    fsub    fr9,fr5
    fmov    fr2,fr6
    fsub    fr10,fr6
    fmov    fr4,fr13
    fmul    fr4,fr13
    fmov    fr5,fr14
    fmul    fr5,fr14
    fadd    fr14,fr13
    fmov    fr6,fr14
    fmul    fr6,fr14
    fadd    fr14,fr13               ! the square of its length
    fldi1   fr14
    fcmp/gt fr14,fr13
    bf      vb_short
    fsrra   fr13
    fmul    fr13,fr4
    fmul    fr13,fr5
    fmul    fr13,fr6
    bra     vb_clamp
    nop
vb_short:
    mov     #4,r0
    fmov.s  @(r0,r8),fr4
    mov     #8,r0
    fmov.s  @(r0,r8),fr5
    mov     #12,r0
    fmov.s  @(r0,r8),fr6
vb_clamp:
    fcmp/gt fr5,fr11                ! tilted no further off the vertical
    bf      vb_store
    fmov    fr11,fr5
    fmov    fr4,fr13
    fmul    fr4,fr13
    fmov    fr5,fr14
    fmul    fr5,fr14
    fadd    fr14,fr13
    fmov    fr6,fr14
    fmul    fr6,fr14
    fadd    fr14,fr13
    fsrra   fr13
    fmul    fr13,fr4
    fmul    fr13,fr5
    fmul    fr13,fr6
    bra     vb_store
    nop
vb_dir:
    mov     #4,r0
    fmov.s  @(r0,r8),fr4
    mov     #8,r0
    fmov.s  @(r0,r8),fr5
    mov     #12,r0
    fmov.s  @(r0,r8),fr6
vb_store:
    fmov.s  fr4,@r3
    add     #4,r3
    fmov.s  fr5,@r3
    add     #4,r3
    fmov.s  fr6,@r3
    add     #4,r3
    fldi0   fr7
    fldi1   fr3
    ftrv    xmtrx,fv0               ! fr2: the corner's z
    ftrv    xmtrx,fv4               ! fr6: the push's z
    mov     #76,r0
    fmov.s  @(r0,r8),fr13           ! the margin
    fcmp/gt fr2,fr13
    JT      vb_ret0
    fldi0   fr14
    fcmp/gt fr6,fr14
    bf      vb_next
    fmov    fr6,fr14
    fmul    fr12,fr14
    fadd    fr2,fr14
    fcmp/gt fr14,fr13
    bf      vb_next
    fsub    fr2,fr13
    fdiv    fr6,fr13
    fmov    fr13,fr12
vb_next:
    dt      r4
    JF      vb_loop

    ! a volume too short is none
    mov     #80,r0
    fmov.s  @(r0,r8),fr13
    fcmp/gt fr12,fr13
    JT      vb_ret0

    ! the corners and their pushed copies on the screen
    mov     #0,r14
vc_loop:
    mov     r9,r4
    add     r14,r4
    mov     r11,r5
    add     r14,r5
    bsr     vb_proj
    nop
    tst     r0,r0
    JT      vb_ret0
    mov     r9,r6
    add     r14,r6
    mov     r10,r7
    add     r14,r7
    mov     r13,r3
    add     #28,r3
    fmov.s  @r6+,fr0
    fmov.s  @r7+,fr1
    fmul    fr12,fr1
    fadd    fr1,fr0
    fmov.s  fr0,@r3
    add     #4,r3
    fmov.s  @r6+,fr0
    fmov.s  @r7+,fr1
    fmul    fr12,fr1
    fadd    fr1,fr0
    fmov.s  fr0,@r3
    add     #4,r3
    fmov.s  @r6+,fr0
    fmov.s  @r7+,fr1
    fmul    fr12,fr1
    fadd    fr1,fr0
    fmov.s  fr0,@r3
    mov     r13,r4
    add     #28,r4
    mov     r12,r5
    add     r14,r5
    bsr     vb_proj
    nop
    tst     r0,r0
    JT      vb_ret0
    add     #12,r14
    mov     r14,r0
    cmp/eq  #96,r0
    JF      vc_loop

    ! which faces are towards the light
    mov     #88,r0
    fmov.s  @(r0,r8),fr15
    mov.l   @(28,r8),r6
    mov     #0,r0
    fmov.s  @r13,fr8
    mov     #4,r0
    fmov.s  @(r0,r13),fr9
    mov     #8,r0
    fmov.s  @(r0,r13),fr10
    mov     #16,r0
    fmov.s  @(r0,r8),fr11
    mov     #20,r0
    fmov.s  @(r0,r8),fr12
    mov     #24,r0
    fmov.s  @(r0,r8),fr13
    mov     #68,r0
    mov.l   @(r0,r8),r4
    mov     #0,r14
vd_loop:
    fldi0   fr0
    fldi0   fr1
    fldi0   fr2
    FACEPT
    FACEPT
    FACEPT
    FACEPT
    tst     r6,r6
    bt      vd_dir
    fmov    fr0,fr4
    fsub    fr11,fr4
    fmov    fr1,fr5
    fsub    fr12,fr5
    fmov    fr2,fr6
    fsub    fr13,fr6
    bra     vd_n
    nop
vd_dir:
    mov     #4,r0
    fmov.s  @(r0,r8),fr4
    mov     #8,r0
    fmov.s  @(r0,r8),fr5
    mov     #12,r0
    fmov.s  @(r0,r8),fr6
vd_n:
    fsub    fr8,fr0
    fsub    fr9,fr1
    fsub    fr10,fr2
    fmul    fr4,fr0
    fmul    fr5,fr1
    fmul    fr6,fr2
    fadd    fr1,fr0
    fadd    fr2,fr0
    fldi0   fr3
    fcmp/gt fr0,fr3
    movt    r1
    mov     r13,r2
    add     #20,r2
    add     r14,r2
    mov.b   r1,@r2
    add     #1,r14
    mov     r14,r0
    cmp/eq  #6,r0
    JF      vd_loop

    ! the caps: the faces towards the light at the near end, the others pushed
    mov.l   @(36,r8),r14
    mov     #68,r0
    mov.l   @(r0,r8),r10
    mov     #0,r9
ve_loop:
    mov.b   @r10+,r0
    mov     r0,r5
    mov.b   @r10+,r0
    mov     r0,r6
    mov.b   @r10+,r0
    mov     r0,r7
    mov.b   @r10+,r0
    mov     r0,r3
    mov     r13,r2
    add     #20,r2
    add     r9,r2
    mov.b   @r2,r0
    tst     r0,r0
    bt      ve_far
    mov     r11,r4
    bra     ve_go
    nop
ve_far:
    mov     r12,r4
ve_go:
    mov     r5,r2
    bsr     vb_emit_v
    nop
    mov     r6,r2
    bsr     vb_emit_v
    nop
    mov     r7,r2
    bsr     vb_emit_v
    nop
    mov     r5,r2
    bsr     vb_emit_v
    nop
    mov     r7,r2
    bsr     vb_emit_v
    nop
    mov     r3,r2
    bsr     vb_emit_v
    nop
    add     #1,r9
    mov     r9,r0
    cmp/eq  #6,r0
    JF      ve_loop

    ! the outline: the edge of a lit face and one that is not
    mov     #0,r9
vg_f:
    mov     r9,r10
    add     #1,r10
vg_g:
    mov     r10,r0
    cmp/eq  #6,r0
    JT      vg_nextf
    mov     r13,r2
    add     #20,r2
    mov     r2,r1
    add     r9,r1
    mov.b   @r1,r0
    mov     r0,r3
    add     r10,r2
    mov.b   @r2,r0
    cmp/eq  r3,r0
    JT      vg_skip
    mov     #68,r0
    mov.l   @(r0,r8),r4
    mov     r9,r1
    shll2   r1
    add     r4,r1
    mov     r10,r2
    shll2   r2
    add     r4,r2
    mov     #0,r3
    SHARE   0
    SHARE   1
    SHARE   2
    SHARE   3
    mov     r3,r0
    cmp/eq  #2,r0
    JF      vg_skip
    EVB     r11, r6
    EVB     r11, r7
    EVB     r12, r7
    EVB     r11, r6
    EVB     r12, r7
    EVB     r12, r6
vg_skip:
    add     #1,r10
    bra     vg_g
    nop
vg_nextf:
    add     #1,r9
    mov     r9,r0
    cmp/eq  #6,r0
    JF      vg_f

    ! the count: the bytes written over 36
    mov.l   @(36,r8),r1
    sub     r1,r14
    shlr2   r14
    mov.l   vb_inv9,r2
    bra     vb_count
    mul.l   r2,r14
    .align  2
vb_inv9:
    .long   0x38E38E39
vb_count:
    bra     vb_exit
    sts     macl,r0

vb_ret0:
    mov     #0,r0
vb_exit:
    add     #108,r15
    add     #108,r15
    add     #108,r15
    add     #108,r15
    lds.l   @r15+,pr
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

! A vertex of three floats (the base in r4, its index in r2) to the output at
! r14. Destroys r0-r2.
vb_emit_v:
    mov     r2,r1
    shll2   r1
    shll2   r2
    shll    r2
    add     r2,r1
    add     r4,r1
    mov.l   @r1+,r0
    mov.l   r0,@r14
    add     #4,r14
    mov.l   @r1+,r0
    mov.l   r0,@r14
    add     #4,r14
    mov.l   @r1+,r0
    mov.l   r0,@r14
    add     #4,r14
    rts
    nop

! A world point (three floats at r4) on the screen, to r5: x, y and the depth
! the PowerVR takes. r0: 1, or 0 when it lies before the near plane.
vb_proj:
    sts.l   pr,@-r15
    fmov.s  @r4+,fr0
    fmov.s  @r4+,fr1
    fmov.s  @r4+,fr2
    fldi1   fr3
    ftrv    xmtrx,fv0
    mov     #40,r0
    fmov.s  @(r0,r8),fr4
    fcmp/gt fr2,fr4
    bf      1f
    lds.l   @r15+,pr
    rts
    mov     #0,r0
1:  fldi1   fr5
    fdiv    fr2,fr5
    mov     #44,r0
    fmov.s  @(r0,r8),fr6
    fmul    fr0,fr6
    fmul    fr5,fr6
    mov     #52,r0
    fmov.s  @(r0,r8),fr7
    fadd    fr6,fr7
    fmov.s  fr7,@r5
    mov     #48,r0
    fmov.s  @(r0,r8),fr6
    fmul    fr1,fr6
    fmul    fr5,fr6
    mov     #56,r0
    fmov.s  @(r0,r8),fr7
    fadd    fr6,fr7
    mov     r5,r6
    add     #4,r6
    fmov.s  fr7,@r6
    fmul    fr5,fr4
    ! the C function takes its float in fr5 (little-endian SH-4 ABI), as fr4's twin
    fmov    fr4,fr5
    mov.l   r5,@-r15
    mov     #64,r0
    mov.l   @(r0,r8),r1
    jsr     @r1
    nop
    mov.l   @r15+,r5
    add     #8,r5
    fmov.s  fr0,@r5
    lds.l   @r15+,pr
    rts
    mov     #1,r0
