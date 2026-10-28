! Hand-written SH-4 for the batcher's statics and sprites (batcher.c, M_XformOwn).
!
!   void dc_xform_own(DC_XFORM_OWN *run);
!
! The common case of M_XformOwn: every vertex of the mesh has the same flags,
! none of them a billboard or an absolute sprite, in front of a perspective
! projection. The flags are read once, by the caller, which says how the
! factor k is found (mode), whether the fog applies, and whether it is TR3's
! coloured light. Per vertex it does what the C loop does: the matrix in XMTRX
! (loaded by the caller), M_Place's near and side tests, the shade, the fog,
! and the colour (the white table, the tint, or TR3's per-channel light).
!
! Registers (r8-r14 and fr12-fr15 are the caller's, and are kept):
!   r13 c + 48 (the M_XFORM flags)  r8 xverts  r9 vertices  r10 vref
!   r12 white table (0: tinted)  r7 vertex  r6 count  r14 its xvert
!   r5 its clip  r3 its colour factor  r0-r4 scratch
!   fr0-3 the vertex  fr4-7 scratch  fr8 proj_x  fr9 proj_y  fr10 near z
!   fr11 nearest 1/z  fr12 fog start squared  fr13 fog start  fr14 fog scale
!   fr15 1.0
! The frame on the stack: 0 the run, 4 the vertex's colour, 8 TR3's f, 12 kk,
! 16 the vertex.
!
! Struct layout, DC_XFORM_OWN (batcher.c):
!    0 c  4 xverts  8 vertices  12 vref  16 count  20 min_shade  24 white
!   28 adder  32 k0  36 mode (0 k0, 1 the vertex's shade, 2 adder + shade)
!   40 fog  44 coloured  48 tr3_double  52 ambient r  56 g  60 b
!   64 tint a  68 r  72 g  76 b (8.8)  80 sargb (0: no cel colour)  84 dark
!   88 light (TR3's table of factors by the shade's step; see batcher.c)
!
! M_XFORM and M_XVERT: as xform_sh4.S. OUTPUT_MESH_VERTEX (28 bytes): 4 color
!   8 x 10 y 12 z (int16) 24 shade (int16)

! One channel of a vertex colour times its factor, as M_Colour: the byte of
! the colour (in r4) at \shift, times the tint at run + \off (times k, in r3,
! but for alpha), in 8.8, saturated; stored at \dst from r2.
    .macro CHANNEL shift, off, dst, scale
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
    add     #\off,r1
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

! One channel of a vertex colour (in r4) at \shift times the whole factor the
! table gives (the next halfword at r3), 8.8, saturated; stored at \dst from r2.
    .macro CHANF shift, dst
    mov     r4,r0
    .if \shift == 16
    shlr16  r0
    .elseif \shift == 8
    shlr8   r0
    .endif
    extu.b  r0,r0
    mov.w   @r3+,r1
    extu.w  r1,r1
    mul.l   r1,r0
    sts     macl,r0
    shlr8   r0
    mov     r0,r1
    shlr8   r1
    neg     r1,r1
    or      r1,r0
    mov.b   r0,@(\dst,r2)
    .endm

! A channel of TR3's own light: the ambient colour at run + \off, times f,
! kept from 0 to 1, times kk; whole, in r3.
    .macro OWNK off
    mov.l   @(0,r15),r1
    add     #\off,r1
    fmov.s  @r1,fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    fmul    fr2,fr1
    fldi0   fr0
    fcmp/gt fr1,fr0
    bf      1f
    fmov    fr0,fr1
1:  fcmp/gt fr15,fr1
    bf      1f
    fmov    fr15,fr1
1:  mov     #12,r0
    fmov.s  @(r0,r15),fr4
    fmul    fr4,fr1
    ftrc    fr1,fpul
    sts     fpul,r3
    .endm

    .text
    .align 2
    .global _dc_xform_own

_dc_xform_own:
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
    add     #-24,r15
    mov.l   r4,@(0,r15)
    mov.l   @(0,r4),r13
    mov.l   @(4,r4),r8
    mov.l   @(8,r4),r9
    mov.l   @(12,r4),r10
    mov.l   @(16,r4),r6
    mov.l   @(24,r4),r12
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
    mov     #0,r7

own_loop:
    cmp/ge  r6,r7                   ! T = past the end
    bf      1f
    bra     own_finish
    nop
1:  mov     r7,r14
    shll2   r14
    shll2   r14
    shll    r14
    add     r8,r14                  ! its xvert
    mov     r7,r1
    tst     r10,r10
    bt      1f
    mov     r7,r0
    shll    r0
    mov.w   @(r0,r10),r1
    extu.w  r1,r1                   ! the vertex it reads
1:  mov     r1,r2
    shll2   r2
    shll2   r2
    shll    r2                      ! 32 per vertex
    mov     r1,r3
    shll2   r3
    sub     r3,r2                   ! less 4: 28
    add     r9,r2                   ! the vertex
    mov.l   r2,@(16,r15)
    mov.l   @(4,r2),r1
    mov.l   r1,@(4,r15)             ! its colour
    mov.l   @(0,r15),r4
    mov.l   @(36,r4),r1
    tst     r1,r1
    bf      1f
    mov.l   @(32,r4),r3             ! mode 0: k is the same for all
    bra     kdone
    nop
1:  mov     r1,r0
    cmp/eq  #1,r0
    mov.w   @(24,r2),r0             ! the vertex's shade
    bt      1f
    lds     r0,fpul
    float   fpul,fr0
    mov     r4,r1
    add     #28,r1
    fmov.s  @r1,fr1
    fadd    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r0                 ! mode 2: adder + shade, whole
1:  mov     r0,r3
    mov.l   .Lshade_maxa,r1
    cmp/gt  r1,r3
    bf      1f
    mov     r1,r3
1:  mov.l   @(20,r4),r1
    cmp/gt  r3,r1
    bf      1f
    mov     r1,r3
1:  shlr2   r3
    shlr2   r3
    mov     #2,r1
    shll8   r1
    sub     r3,r1
    mov     r1,r3                   ! k = 512 - shade / 16
kdone:
    mov.l   @(44,r4),r1
    tst     r1,r1
    bt      1f
    mov     #1,r3
    shll8   r3                      ! TR3's own light: k is the fog's alone
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
#if defined(TRX_DC_XFORM_PREF)
    mov     r2,r1                   ! two vertices on: into the cache meanwhile
    add     #50,r1
    pref    @r1
#endif
    fldi1   fr3
    ftrv    xmtrx,fv0

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
    .align 2
.Lshade_maxa:
    .long   0x1FFF
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
    mov.l   @(0,r15),r1
    mov.l   @(40,r1),r1             ! whether the fog applies
    tst     r1,r1
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
    mov.l   @(0,r15),r4
    mov.l   @(44,r4),r1
    tst     r1,r1
    bt      1f
    bra     own_coloured
    nop
1:  tst     r12,r12
    bf      1f
    mov     r15,r4
    add     #4,r4
    mov.l   @r4,r4                  ! its colour
    mov     r14,r2
    add     #24,r2
    CHANNEL 16, 76, 0, 1
    CHANNEL 8, 72, 1, 1
    CHANNEL 0, 68, 2, 1
    CHANNEL 24, 64, 3, 0
    bra     clipped
    nop
1:  mov     r3,r0
    shll2   r0
    mov.l   @(r0,r12),r1            ! the white of that factor
    mov     r14,r4
    add     #24,r4
    mov.l   r1,@r4
clipped:
    mov     r14,r4
    add     #28,r4
    mov.b   r5,@r4
    mov.l   @(0,r15),r4
    mov     r4,r1
    add     #80,r1
    mov.l   @r1,r2                  ! the cel colours (0: none)
    tst     r2,r2
    bf      1f
    bra     own_next
    nop
1:  mov.l   @(24,r14),r1            ! the colour, as M_ScaleRGB8 darkens it
    mov     r4,r3
    add     #84,r3
    mov.l   @r3,r3                  ! dark
    mov     r1,r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    mov     r0,r4                   ! blue
    mov     r1,r0
    shlr8   r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    shll8   r0
    or      r0,r4                   ! green
    mov     r1,r0
    shlr16  r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    shll16  r0
    or      r0,r4                   ! red; the alpha is 0
    mov     r7,r0
    shll2   r0
    mov.l   r4,@(r0,r2)
own_next:
    add     #1,r7
    bra     own_loop
    nop

! TR3's own light with no fog on the vertex: each channel's factor comes from
! the table the caller made for the instance, by the shade's step. A vertex in
! the fog (or whose shade has no step in the table) takes the long way.
own_coloured:
    mov     #1,r1
    shll8   r1
    cmp/eq  r1,r3
    bt      1f
    bra     own_coloured_slow
    nop
1:  mov.l   @(16,r15),r2
    mov.w   @(24,r2),r0             ! the vertex's shade
    mov.l   .Lshade_max,r1
    sub     r0,r1
    shar    r1
    shar    r1
    shar    r1
    shar    r1
    shar    r1                      ! the step, as (max - shade) / 32
    cmp/pl  r1
    bt      1f
    mov     #0,r1
1:  mov     #-1,r2
    extu.b  r2,r2
    cmp/gt  r2,r1
    bf      1f
    bra     own_coloured_slow
    nop
1:  shll2   r1
    shll    r1                      ! 8 bytes an entry
    mov     r4,r3
    add     #88,r3
    mov.l   @r3,r3
    add     r1,r3                   ! this step's factors: blue, green, red
    mov     r15,r4
    add     #4,r4
    mov.l   @r4,r4                  ! the vertex's colour
    mov     r14,r2
    add     #24,r2
    CHANF   16, 0
    CHANF   8, 1
    CHANF   0, 2
    CHANNEL 24, 64, 3, 0
    bra     clipped
    nop

! The same by arithmetic: the ambient colour dimmed by the vertex's shade,
! per channel, times the fog's share (r3) and the curve's doubling.
own_coloured_slow:
    lds     r3,fpul
    float   fpul,fr4
    mov     r4,r1
    add     #48,r1
    fmov.s  @r1,fr5
    fmul    fr5,fr4                 ! kk
    mov     #12,r0
    fmov.s  fr4,@(r0,r15)
    mov.l   @(16,r15),r2
    mov.w   @(24,r2),r0             ! the vertex's shade
    mov.l   .Lshade_max,r1
    sub     r0,r1
    lds     r1,fpul
    float   fpul,fr6
    mov.l   .Lone_32,r1
    lds     r1,fpul
    fsts    fpul,fr7
    fmul    fr7,fr6
    ftrc    fr6,fpul
    sts     fpul,r1                 ! step
    cmp/pl  r1
    bt      1f
    mov     #-1,r1
    extu.b  r1,r1                   ! 255
1:  lds     r1,fpul
    float   fpul,fr6
    mov.l   .Lone_255,r1
    lds     r1,fpul
    fsts    fpul,fr7
    fmul    fr7,fr6                 ! f
    mov     #8,r0
    fmov.s  fr6,@(r0,r15)
    mov     r15,r4
    add     #4,r4
    mov.l   @r4,r4                  ! the vertex's colour
    mov     r14,r2
    add     #24,r2
    OWNK    60
    CHANNEL 16, 76, 0, 1
    OWNK    56
    CHANNEL 8, 72, 1, 1
    OWNK    52
    CHANNEL 0, 68, 2, 1
    CHANNEL 24, 64, 3, 0
    bra     clipped
    nop

own_finish:
    mov     r13,r5
    add     #28,r5
    fmov.s  fr11,@r5                ! the nearest 1/z
    add     #24,r15
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
.Lone_32:
    .long   0x3D000000
.Lone_255:
    .long   0x3B808081
