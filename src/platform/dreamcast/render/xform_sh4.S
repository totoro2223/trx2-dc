! Hand-written SH-4 for the batcher's room vertices (batcher.c, M_XformRoom).
!
!   void dc_xform_room(DC_XFORM_ROOM *run);
!
! The plain case of M_XformRoom: a TR1/TR2 room lit by its own shade (and
! its flicker), with no light passing, in front of a perspective projection;
! white (a table of colours by factor), or tinted/coloured (M_Colour). Per vertex of the ranges it does what the C loop does: the
! vertex a shared corner already placed is copied; the rest go through the
! matrix in XMTRX (loaded by the caller), M_Place's near and side tests, the
! fog (M_Fog, M_FogKeep), the shade (M_ShadeK) and the white colour table.
!
! Registers (r8-r14 and fr12-fr15 are the caller's, and are kept):
!   r13 c + 48 (the M_XFORM flags)  r8 xverts   r9 vertices   r10 vref
!   r11 slot   r12 gen   r7 vertex   r6 end of the range   r14 its xvert
!   r5 its clip   r3 its colour factor   r0-r4 scratch
!   fr0-3 the vertex   fr4-7 scratch   fr8 proj_x   fr9 proj_y   fr10 near z
!   fr11 nearest 1/z   fr12 fog start squared   fr13 fog start   fr14 fog scale
!   fr15 1.0
! The frame on the stack: 0 the run, 4 the ranges' cursor, 8 ranges left,
! 12 shared vertices copied, 16 least shade, 20 flicker, 24 white table (0 if
! not white), 28 the colour of the vertex.
!
! Struct layout, DC_XFORM_ROOM (batcher.c):
!    0 c  4 xverts  8 vertices  12 vref  16 slot  20 gen  24 ranges
!   28 nranges  32 min_shade  36 flicker  40 white (0: not) | 44 hits
!   48 tint a  52 tint r  56 tint g  60 tint b (8.8)
!
! M_XFORM: 0 left 4 right 8 top 12 bottom 48 ortho 49 sides 50 near 51 fog
!   52 proj_x 56 proj_y 60 near_z 64 fog_start 68 fog_start_sq 72 fog_scale
!   76 max_inv_z
! M_XVERT: 0 x 4 y 8 z 12 sx 16 sy 20 inv_z 24 argb 28 clip
! OUTPUT_MESH_VERTEX (28 bytes): 8 x 10 y 12 z (int16) 24 shade (int16)
!   26 light_table_idx

! One channel of a vertex colour times its factor, as M_Colour: the byte of
! the colour (in r4) at \shift, times the tint at 	int (times k, in r3, but
! for alpha), in 8.8, saturated; stored at \dst from r2.
    .macro CHANNEL shift, tint, dst, scale
    mov     r4,r0
    .if \shift == 24
    shlr16  r0
    shlr8   r0
    .elseif \shift == 16
    shlr16  r0
    .elseif \shift == 8
    shlr8   r0
    .endif
    extu.b  r0,r0
    mov.l   @(0,r15),r1
    add     #	int,r1
    mov.l   @r1,r1
    .if \scale
    mul.l   r3,r1
    sts     macl,r1
    shlr8   r1
    .endif
    mul.l   r1,r0
    sts     macl,r0
    shlr8   r0
    mov     r0,r1
    shlr8   r1
    neg     r1,r1
    or      r1,r0
    mov.b   r0,@(\dst,r2)
    .endm

    .text
    .align 2
    .global _dc_xform_room

_dc_xform_room:
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
    fmov.s  fr8,@-r15
    fmov.s  fr9,@-r15
    fmov.s  fr10,@-r15
    fmov.s  fr11,@-r15
    add     #-32,r15
    mov.l   r4,@(0,r15)
    mov     r4,r5
    mov.l   @r5+,r13
    mov.l   @r5+,r8
    mov.l   @r5+,r9
    mov.l   @r5+,r10
    mov.l   @r5+,r11
    mov.l   @r5+,r12
    mov.l   @r5+,r1
    mov.l   r1,@(4,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(8,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(16,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(20,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(24,r15)
    mov     #0,r1
    mov.l   r1,@(12,r15)
    mov     r13,r5
    add     #52,r5
    fmov.s  @r5+,fr8
    fmov.s  @r5+,fr9
    fmov.s  @r5+,fr10
    fmov.s  @r5+,fr13
    fmov.s  @r5+,fr12
    fmov.s  @r5+,fr14
    fmov.s  @r5+,fr11
    fldi1   fr15
    add     #48,r13

range_loop:
    mov.l   @(8,r15),r1
    tst     r1,r1
    bf      1f
    bra     finish
    nop
1:  add     #-1,r1
    mov.l   r1,@(8,r15)
    mov.l   @(4,r15),r4
    mov.l   @r4+,r7                 ! first vertex
    mov.l   @r4+,r6                 ! how many
    mov.l   r4,@(4,r15)
    add     r7,r6                   ! the end

vert_loop:
    cmp/ge  r6,r7                   ! T = past the end
    bf      1f
    bra     range_loop
    nop
1:  mov     r7,r14
    shll2   r14
    shll2   r14
    shll    r14
    add     r8,r14                  ! its xvert
    mov     r7,r0
    shll    r0
    mov.w   @(r0,r10),r1
    extu.w  r1,r1                   ! the vertex it reads
    mov     r1,r2
    shll2   r2
    add     r11,r2                  ! its slot
    mov.l   @r2,r3
    xor     r12,r3
    shlr16  r3
    tst     r3,r3
    bf      miss                    ! not placed in this instance yet
    mov.l   @r2,r3                  ! placed: copy it
    extu.w  r3,r3
    shll2   r3
    shll2   r3
    shll    r3
    add     r8,r3
    mov     r14,r4
    .rept 8
    mov.l   @r3+,r0
    mov.l   r0,@r4
    add     #4,r4
    .endr
    mov.l   @(12,r15),r0
    add     #1,r0
    mov.l   r0,@(12,r15)
    add     #1,r7
    bra     vert_loop
    nop

miss:
    mov     r12,r3
    or      r7,r3
    mov.l   r3,@r2                  ! the slot: gen | vertex
    mov     r1,r2
    shll2   r2
    shll2   r2
    shll    r2                      ! 32 per vertex
    mov     r1,r3
    shll2   r3
    sub     r3,r2                   ! less 4: 28
    add     r9,r2
    mov.l   @(24,r15),r1
    tst     r1,r1
    bf      1f
    mov.l   @(4,r2),r1              ! not white: its colour
    mov.l   r1,@(28,r15)
1:  add     #8,r2
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr0
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr1
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr2
    fldi1   fr3
    ftrv    xmtrx,fv0
    add     #10,r2
    mov.w   @r2+,r3                 ! the shade
    mov.b   @r2,r0
    and     #31,r0
    add     #-16,r0
    mov.l   @(20,r15),r1
    mul.l   r1,r0
    sts     macl,r1
    add     r1,r3                   ! plus the flicker
    mov.l   .Lshade_max,r1
    cmp/gt  r1,r3
    bf      1f
    mov     r1,r3
1:  mov.l   @(16,r15),r1
    cmp/gt  r3,r1
    bf      1f
    mov     r1,r3
1:  shlr2   r3
    shlr2   r3
    mov     #2,r1
    shll8   r1
    sub     r3,r1
    mov     r1,r3                   ! k = 512 - shade / 16

    mov     r14,r4
    add     #12,r4
    fmov.s  fr2,@-r4
    fmov.s  fr1,@-r4
    fmov.s  fr0,@-r4
    mov     #0,r5                   ! the clip
    mov.b   @(2,r13),r0
    tst     r0,r0
    bt      place
    fcmp/gt fr2,fr10                ! near z > z
    bf      place
    mov     #2,r5                   ! before the near plane
    bra     placed
    nop
place:
    fmov    fr2,fr4
    fmul    fr2,fr4
    fsrra   fr4                     ! 1/z
    fmov    fr8,fr5
    fmul    fr0,fr5
    fmul    fr4,fr5                 ! sx
    fmov    fr9,fr6
    fmul    fr1,fr6
    fmul    fr4,fr6                 ! sy
    mov     r14,r4
    add     #24,r4
    fmov.s  fr4,@-r4
    fmov.s  fr6,@-r4
    fmov.s  fr5,@-r4
    fcmp/gt fr11,fr4
    bf      1f
    fmov    fr4,fr11
1:  mov.b   @(1,r13),r0
    tst     r0,r0
    bt      placed
    mov     r13,r4
    add     #-48,r4
    fmov.s  @r4+,fr7                ! left
    fcmp/gt fr5,fr7
    bf      1f
    mov     #4,r1
    or      r1,r5
1:  fmov.s  @r4+,fr7                ! right
    fcmp/gt fr7,fr5
    bf      1f
    mov     #8,r1
    or      r1,r5
1:  fmov.s  @r4+,fr7                ! top
    fcmp/gt fr6,fr7
    bf      1f
    mov     #16,r1
    or      r1,r5
1:  fmov.s  @r4+,fr7                ! bottom
    fcmp/gt fr7,fr6
    bf      1f
    mov     #32,r1
    or      r1,r5
1:
placed:
    mov.b   @(3,r13),r0
    tst     r0,r0
    bf      1f
    bra     shaded
    nop
1:  fldi0   fr3
    fipr    fv0,fv0                 ! fr3 = x*x + y*y + z*z
    fcmp/gt fr12,fr3
    bt      fogging
    bra     shaded                  ! nearer than the fog starts
    nop
fogging:
    fmov    fr3,fr4
    fsrra   fr4
    fmul    fr3,fr4
    fsub    fr13,fr4
    fmul    fr14,fr4                ! how much fog
    fldi0   fr5
    fcmp/gt fr5,fr4
    bt      1f
    bra     shaded                  ! none
    nop
1:  fcmp/gt fr15,fr4
    bt      full
    fcmp/eq fr15,fr4
    bt      full
    mov     #1,r1
    shll8   r1
    lds     r1,fpul
    float   fpul,fr7                ! 256
    fmov    fr15,fr6
    fsub    fr4,fr6
    fmul    fr7,fr6
    ftrc    fr6,fpul
    sts     fpul,r1                 ! what the fog leaves, in 8.8
    mov     #64,r2
    or      r2,r5
    bra     keep
    nop
full:
    mov     #65,r2
    or      r2,r5
    mov     #0,r1
keep:
    mul.l   r1,r3
    sts     macl,r3
    shlr8   r3

shaded:
    mov.l   @(24,r15),r2
    tst     r2,r2
    bt      tinted
    mov     r3,r0
    shll2   r0
    mov.l   @(r0,r2),r1             ! the white of that factor
    mov     r14,r4
    add     #24,r4
    mov.l   r1,@r4
    add     #4,r4
    mov.b   r5,@r4
    add     #1,r7
    bra     vert_loop
    nop
! WIP (TAREAS.md): the tinted colour, not assembling yet and never reached:
! the C gate sends only white rooms here.
tinted:
    .if 0
    mov.l   @(28,r15),r4
    mov     r14,r2
    add     #24,r2
    CHANNEL 16, 60, 0, 1
    CHANNEL 8, 56, 1, 1
    CHANNEL 0, 52, 2, 1
    CHANNEL 24, 48, 3, 0
    add     #4,r2
    mov.b   r5,@r2
    .endif
    add     #1,r7
    bra     vert_loop
    nop

finish:
    mov     r13,r5
    add     #28,r5
    fmov.s  fr11,@r5                ! the nearest 1/z
    mov.l   @(0,r15),r4
    mov.l   @(12,r15),r1
    mov.l   r1,@(44,r4)
    add     #32,r15
    fmov.s  @r15+,fr11
    fmov.s  @r15+,fr10
    fmov.s  @r15+,fr9
    fmov.s  @r15+,fr8
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

    .align 2
.Lshade_max:
    .long   0x1FFF
