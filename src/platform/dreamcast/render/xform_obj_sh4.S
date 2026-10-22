! Hand-written SH-4 for the batcher's object vertices (batcher.c, M_XformObject).
!
!   void dc_xform_object(DC_XFORM_OBJECT *run);
!
! The plain case of M_XformObject: a TR1/TR2 object, white, with no reflection
! and no per-vertex fog flags, in front of a perspective projection. Per
! vertex it does what the C loop does: a twin of an earlier vertex is copied
! (with its cel colour); the rest go through the matrix in XMTRX (loaded by
! the caller), M_Place's near and side tests, the shade from n.l, the fog and
! the white colour table, and the cel colour when asked.
!
! Registers (r8-r14 and fr12-fr15 are the caller's, and are kept):
!   r13 c + 48 (the M_XFORM flags)  r8 xverts  r9 vertices  r10 vref
!   r11 twin  r12 white table  r7 vertex  r6 count  r14 its xvert
!   r5 its clip  r3 its colour factor  r0-r4 scratch
!   fr0-3 the vertex  fr4-7 scratch  fr8 proj_x  fr9 proj_y  fr10 near z
!   fr11 nearest 1/z  fr12 fog start squared  fr13 fog start  fr14 fog scale
!   fr15 1.0
! The frame on the stack: 0 the run, 4 n.l of the vertex.
!
! Struct layout, DC_XFORM_OBJECT (batcher.c):
!    0 c  4 xverts  8 vertices  12 vref  16 twin  20 count  24 min_shade
!   28 white  32 sargb (0: no cel)  36 adder  40 l0  44 l1  48 l2 (0 with no
!   direction)  52 cel_scale  56 dark
!
! M_XFORM and M_XVERT: as xform_sh4.S. OUTPUT_MESH_VERTEX (28 bytes): 4 color
!   8 x 10 y 12 z (int16) 16 nx 18 ny 20 nz (int16) 24 shade

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

! One of TR3's lights, number \n, if the object has that many (count in r2,
! the light in r4, the normal in fv4): its colour times n.l, if n.l is over
! 0, added to the colour in the frame at 20, 24, 28.
    .macro LIGHT n
    .if \n == 0
    tst     r2,r2
    bf      1f
    .else
    mov     #\n,r1
    cmp/gt  r1,r2
    bt      1f
    .endif
    bra     lights_done
    nop
1:  mov     r4,r1
    add     #(40 + 12 * \n),r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi0   fr3
    fipr    fv0,fv4                 ! fr7 = n.l
    fldi0   fr0
    fcmp/gt fr0,fr7
    bt      1f
    bra     2f
    nop
1:  mov     r4,r1
    add     #(76 + 12 * \n),r1
    fmov.s  @r1+,fr0
    fmul    fr7,fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    fadd    fr0,fr1
    fmov.s  fr1,@(r0,r15)
    fmov.s  @r1+,fr0
    fmul    fr7,fr0
    mov     #24,r0
    fmov.s  @(r0,r15),fr1
    fadd    fr0,fr1
    fmov.s  fr1,@(r0,r15)
    fmov.s  @r1+,fr0
    fmul    fr7,fr0
    mov     #28,r0
    fmov.s  @(r0,r15),fr1
    fadd    fr0,fr1
    fmov.s  fr1,@(r0,r15)
2:
    .endm

! A channel of TR3's light (the frame at \slot), kept from 0 to 1 and times
! kk (fr4), whole, in r3.
    .macro TR3K slot
    mov     #\slot,r0
    fmov.s  @(r0,r15),fr1
    fldi0   fr0
    fcmp/gt fr1,fr0
    bf      1f
    fmov    fr0,fr1
1:  fcmp/gt fr15,fr1
    bf      1f
    fmov    fr15,fr1
1:  fmul    fr4,fr1
    ftrc    fr1,fpul
    sts     fpul,r3
    .endm

    .text
    .align 2
    .global _dc_xform_object

_dc_xform_object:
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
    add     #-36,r15
    mov.l   r4,@(0,r15)
    mov     r4,r1
    add     #76,r1
    mov.l   @r1,r1
    mov.l   r1,@(16,r15)            ! TR3's light (0: not TR3)
    mov.l   @(0,r4),r13
    mov.l   @(4,r4),r8
    mov.l   @(8,r4),r9
    mov.l   @(12,r4),r10
    mov.l   @(16,r4),r11
    mov.l   @(20,r4),r6
    mov.l   @(28,r4),r12
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

obj_loop:
    cmp/ge  r6,r7                   ! T = past the end
    bf      1f
    bra     obj_finish
    nop
1:  mov     r7,r14
    shll2   r14
    shll2   r14
    shll    r14
    add     r8,r14                  ! its xvert
    tst     r11,r11
    bt      untwinned
    mov     r7,r0
    shll    r0
    mov.w   @(r0,r11),r1
    extu.w  r1,r1                   ! its twin
    cmp/eq  r7,r1
    bt      untwinned
    mov     r1,r3
    shll2   r3
    shll2   r3
    shll    r3
    add     r8,r3
    mov     r14,r4
    add     #32,r4
    fschg                           ! 64-bit moves: the xvert is 32 aligned
    fmov.d  @r3+,dr0
    fmov.d  @r3+,dr2
    fmov.d  @r3+,dr4
    fmov.d  @r3+,dr6
    fmov.d  dr6,@-r4
    fmov.d  dr4,@-r4
    fmov.d  dr2,@-r4
    fmov.d  dr0,@-r4
    fschg
    mov.l   @(0,r15),r4
    mov.l   @(32,r4),r2
    tst     r2,r2
    bt      twinned
    mov     r1,r0
    shll2   r0
    mov.l   @(r0,r2),r3
    mov     r7,r0
    shll2   r0
    mov.l   r3,@(r0,r2)             ! its cel colour
twinned:
    add     #1,r7
    bra     obj_loop
    nop

untwinned:
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
    mov.l   @(4,r2),r1
    mov     r15,r3
    add     #8,r3
    mov.l   r1,@r3                  ! its colour, for a tinted instance
    mov.l   r2,@(12,r15)            ! the vertex, for TR3's normal
    mov     r2,r3
    add     #16,r3
    mov.w   @r3+,r1
    lds     r1,fpul
    float   fpul,fr4
    mov.w   @r3+,r1
    lds     r1,fpul
    float   fpul,fr5
    mov.w   @r3+,r1
    lds     r1,fpul
    float   fpul,fr6                ! the normal
    fldi0   fr7
    mov.l   @(0,r15),r4
    mov     r4,r1
    add     #40,r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi0   fr3                     ! the light
    fipr    fv0,fv4                 ! fr7 = n.l
    mov     r15,r1
    add     #4,r1
    fmov.s  fr7,@r1                 ! kept for the cel colour
    mov     r4,r1
    add     #36,r1
    fmov.s  @r1,fr4
    mov.l   .Lone_65536a,r1
    lds     r1,fpul
    fsts    fpul,fr5
    fmul    fr7,fr5
    fadd    fr5,fr4                 ! adder + n.l / 65536
    ftrc    fr4,fpul
    sts     fpul,r3
    mov.l   .Lshade_maxa,r1
    cmp/gt  r1,r3
    bf      1f
    mov     r1,r3
1:  mov.l   @(24,r4),r1
    cmp/gt  r3,r1
    bf      1f
    mov     r1,r3
1:  shlr2   r3
    shlr2   r3
    mov     #2,r1
    shll8   r1
    sub     r3,r1
    mov     r1,r3                   ! k = 512 - shade / 16
    mov.l   @(16,r15),r1
    tst     r1,r1
    bt      1f
    mov     #1,r3
    shll8   r3                      ! TR3: 256, the light is per channel
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
.Lone_65536a:
    .long   0x37800000
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
    mov.l   @(16,r15),r4
    tst     r4,r4
    bt      1f
    bra     tr3col
    nop
1:  tst     r12,r12
    bf      1f
    mov     r15,r4
    add     #8,r4
    mov.l   @r4,r4                  ! its colour
    mov     r14,r2
    add     #24,r2
    CHANNEL 16, 72, 0, 1
    CHANNEL 8, 68, 1, 1
    CHANNEL 0, 64, 2, 1
    CHANNEL 24, 60, 3, 0
    mov.l   @(24,r14),r1            ! the tinted colour
    bra     coloured
    nop
1:  mov     r3,r0
    shll2   r0
    mov.l   @(r0,r12),r1            ! the white of that factor
    mov     r14,r4
    add     #24,r4
    mov.l   r1,@r4
coloured:
    mov     r14,r4
    add     #28,r4
    mov.b   r5,@r4
    mov.l   @(0,r15),r4
    mov.l   @(32,r4),r3
    tst     r3,r3
    bt      celled                  ! no cel colour
    mov     r15,r2
    add     #4,r2
    fmov.s  @r2,fr4                 ! n.l
    mov.l   @(52,r4),r2
    lds     r2,fpul
    fsts    fpul,fr5
    fmul    fr5,fr4                 ! n.l * cel_scale
    mov.l   .Lhalf,r2
    lds     r2,fpul
    fsts    fpul,fr6
    fmul    fr6,fr4
    fadd    fr6,fr4                 ! the edge: 0.5 + 0.5 * (n.l * cel_scale)
    fldi0   fr5
    fcmp/gt fr4,fr5
    bf      1f
    fmov    fr5,fr4                 ! not under 0
1:  fcmp/gt fr15,fr4
    bf      1f
    fmov    fr15,fr4                ! not over 1
1:  mov.l   .Lf255,r2
    lds     r2,fpul
    fsts    fpul,fr5
    fmul    fr5,fr4
    ftrc    fr4,fpul
    sts     fpul,r2                 ! the alpha
    shll16  r2
    shll8   r2                      ! the alpha, in place
    mov.l   @(56,r4),r3             ! dark
    mov     r1,r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    or      r0,r2                   ! blue, scaled
    mov     r1,r0
    shlr8   r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    shll8   r0
    or      r0,r2                   ! green
    mov     r1,r0
    shlr16  r0
    extu.b  r0,r0
    mul.l   r3,r0
    sts     macl,r0
    shlr8   r0
    shll16  r0
    or      r0,r2                   ! red
    mov.l   @(32,r4),r3
    mov     r7,r0
    shll2   r0
    mov.l   r2,@(r0,r3)
celled:
    add     #1,r7
    bra     obj_loop
    nop

! TR3: the light is a colour per channel, from the object's light (r4) and the
! vertex's normal; the channel factor goes in r3 for CHANNEL.
tr3col:
    mov.l   @(12,r15),r2
    add     #16,r2
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr4
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr5
    mov.w   @r2+,r1
    lds     r1,fpul
    float   fpul,fr6
    fldi0   fr7                     ! the normal
    lds     r3,fpul
    float   fpul,fr0
    mov     r4,r1
    add     #20,r1
    fmov.s  @r1,fr1
    fmul    fr1,fr0                 ! kk = k * tr3_double
    mov     #32,r0
    fmov.s  fr0,@(r0,r15)
    mov     r4,r1
    add     #24,r1
    mov.l   @r1+,r0
    mov.l   r0,@(20,r15)
    mov.l   @r1+,r0
    mov.l   r0,@(24,r15)
    mov.l   @r1+,r0
    mov.l   r0,@(28,r15)            ! the ambient colour
    mov     r4,r1
    add     #36,r1
    mov.l   @r1,r2                  ! how many lights
    LIGHT   0
    LIGHT   1
    LIGHT   2
lights_done:
    mov     #32,r0
    fmov.s  @(r0,r15),fr4           ! kk
    mov     r15,r4
    add     #8,r4
    mov.l   @r4,r4                  ! the vertex colour
    mov     r14,r2
    add     #24,r2
    TR3K    28
    CHANNEL 16, 72, 0, 1
    TR3K    24
    CHANNEL 8, 68, 1, 1
    TR3K    20
    CHANNEL 0, 64, 2, 1
    CHANNEL 24, 60, 3, 0
    mov.l   @(24,r14),r1
    bra     coloured
    nop

obj_finish:
    mov     r13,r5
    add     #28,r5
    fmov.s  fr11,@r5                ! the nearest 1/z
    add     #36,r15
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
.Lone_65536:
    .long   0x37800000
.Lhalf:
    .long   0x3F000000
.Lf255:
    .long   0x437F0000
