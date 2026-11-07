! Hand-written SH-4 for the batcher's generic transform (batcher.c,
! M_XformGeneric): sprites, meshes lit by their own light or by none, coloured
! fog, reflections, the UI's models; any flags, a vertex at a time.
!
!   void dc_xform_generic(M_GENRUN *run);
!
! Per vertex: the model-view matrix (in XMTRX, loaded by the caller), the
! billboard offset, M_Place (near and side tests, the screen position), the
! fog, the lighting (inline for the shade of a mesh with its own light, the
! object's light and a room's own, without a passing light or flicker; M_LightVertex
! itself, through run->light, for TR3, the caustics and the rest), the colour with
! the fog, the cel shade's colour, the fog colour, and the reflection (run->env,
! M_EnvVertex). The C stays as M_XformGenericC for a build with DC_EMIT_VERIFY
! to compare.
!
! Registers (r8-r14, fr12-fr15 are kept):
!   r8 run + 128 (so that every field lies within a signed byte of it)
!   r9 vertices left  r10 xvert  r11 oargb (0: none)  r12 sargb (0: none)
!   r13 vertex  r14 vref cursor (0: the vertices in order)
!   fr12 255.0  fr13 1.0  fr14 0.0  fr15 0.5
! The frame (32 bytes from r15): 0 clip, 4 fog, 8 keep.
!
! Struct M_GENRUN (batcher.c):
!    0 vertices  4 vref  8 xverts  12 oargb  16 sargb  20 count  24 c  28 il
!   32 tint r  36 g  40 b  44 a  48 scale  52 cel_scale  56 cel_shade
!   60 fog_color r  64 g  68 b  72 lighting  76 min_shade  80 env  84 slow
!   88 adder  92 has_direction  96 l[3]  108 dyn  112 light  116 env
!   120 rgba[4]  136 -  140 cur_i  144 cur_vertex  148 x  152 y  156 z
!   160 keep  164 lit[3]  176 batcher  180 inst  184 mv
!   188 1/255  192 shade max  196 1/65536  200 2.0  204 1/shade neutral
!   208 cel edge  212 255.0  216 0.5  220 shade neutral
! M_XFORM: 0 left  4 right  8 top  12 bottom  48 ortho  49 sides  50 near
!   51 fog  52 proj_x  56 proj_y  60 near_z  64 fog_start  68 fog_start_sq
!   72 fog_scale  76 max_inv_z
! M_XVERT: 0 x  4 y  8 z  12 sx  16 sy  20 inv_z  24 argb  28 clip
! OUTPUT_MESH_VERTEX (28 bytes): 4 colour r g b a  8 x 10 y 12 z (int16)
!   16 nx 18 ny 20 nz (int16)  22 flags  24 shade (int16)

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

    .macro RF fr, x
    mov     #(\x-128),r0
    fmov.s  @(r0,r8),\fr
    .endm

    .macro RS fr, x
    mov     #(\x-128),r0
    fmov.s  \fr,@(r0,r8)
    .endm

    .macro RL reg, x
    mov     #(\x-128),r0
    mov.l   @(r0,r8),\reg
    .endm

    .macro FL fr, off
    mov     #\off,r0
    fmov.s  @(r0,r15),\fr
    .endm

    .macro FS fr, off
    mov     #\off,r0
    fmov.s  \fr,@(r0,r15)
    .endm

! A halfword of the vertex (signed) to a float.
    .macro I2F fr, disp
    mov.w   @(\disp,r13),r0
    exts.w  r0,r0
    lds     r0,fpul
    float   fpul,\fr
    .endm

! A byte of the vertex (unsigned) to a float.
    .macro B2F fr, disp
    mov.b   @(\disp,r13),r0
    extu.b  r0,r0
    lds     r0,fpul
    float   fpul,\fr
    .endm

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

    .text
    .align 2
    .global _dc_xform_generic
_dc_xform_generic:
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
    add     #-32,r15
    mov     r4,r8
    add     #127,r8
    add     #1,r8
    RL      r9, 20
    tst     r9,r9
    JT      gx_exit
    RL      r10, 8
    RL      r11, 12
    RL      r12, 16
    RL      r14, 4
    RL      r13, 0
    tst     r14,r14
    bf      gx_init
    add     #-28,r13
gx_init:
    fldi1   fr13
    fldi0   fr14
    RF      fr12, 212
    RF      fr15, 216

gx_loop:
    ! the vertex
    tst     r14,r14
    bt      gx_seq
    mov.w   @r14+,r0
    extu.w  r0,r0
    mov     r0,r2
    shll2   r2
    shll2   r2
    shll    r2
    mov     r0,r3
    shll2   r3
    sub     r3,r2                   ! 28 * index
    RL      r13, 0
    add     r2,r13
    bra     gx_have
    nop
gx_seq:
    add     #28,r13
gx_have:
    I2F     fr0, 8
    I2F     fr1, 10
    I2F     fr2, 12
    fldi1   fr3
    ftrv    xmtrx,fv0               ! x y z in eye space
    mov.w   @(22,r13),r0
    extu.w  r0,r0
    tst     #0x30,r0
    bt      gx_nobill
    I2F     fr4, 16
    RF      fr6, 48
    fmul    fr6,fr4
    fadd    fr4,fr0
    I2F     fr5, 18
    fmul    fr6,fr5
    fadd    fr5,fr1
gx_nobill:
    RS      fr0, 148
    RS      fr1, 152
    RS      fr2, 156

    ! M_Place
    mov     r10,r1
    fmov.s  fr0,@r1
    add     #4,r1
    fmov.s  fr1,@r1
    add     #4,r1
    fmov.s  fr2,@r1
    RL      r2, 24
    mov     r2,r4
    add     #48,r4
    mov.b   @r4+,r5                 ! ortho
    mov.b   @r4+,r6                 ! sides
    mov.b   @r4+,r7                 ! near
    mov     #0,r3                   ! clip
    tst     r5,r5
    JF      gx_placed
    tst     r7,r7
    bt      gx_nonear
    mov     #60,r0
    fmov.s  @(r0,r2),fr3
    fcmp/gt fr2,fr3
    bf      gx_nonear
    mov     #2,r3
    bra     gx_placed
    nop
gx_nonear:
    fmov    fr2,fr3
    fmul    fr2,fr3
    fsrra   fr3                     ! inv_z
    mov     #52,r0
    fmov.s  @(r0,r2),fr4
    fmul    fr0,fr4
    fmul    fr3,fr4                 ! sx
    mov     #56,r0
    fmov.s  @(r0,r2),fr5
    fmul    fr1,fr5
    fmul    fr3,fr5                 ! sy
    mov     r10,r1
    add     #12,r1
    fmov.s  fr4,@r1
    add     #4,r1
    fmov.s  fr5,@r1
    add     #4,r1
    fmov.s  fr3,@r1
    mov     #76,r0
    fmov.s  @(r0,r2),fr6
    fcmp/gt fr6,fr3
    bf      gx_nomax
    fmov.s  fr3,@(r0,r2)
gx_nomax:
    tst     r6,r6
    bt      gx_placed
    fmov.s  @r2,fr6
    fcmp/gt fr4,fr6
    bf      gx_nol
    mov     #4,r0
    or      r0,r3
gx_nol:
    mov     #4,r0
    fmov.s  @(r0,r2),fr6
    fcmp/gt fr6,fr4
    bf      gx_nor
    mov     #8,r0
    or      r0,r3
gx_nor:
    mov     #8,r0
    fmov.s  @(r0,r2),fr6
    fcmp/gt fr5,fr6
    bf      gx_not
    mov     #16,r0
    or      r0,r3
gx_not:
    mov     #12,r0
    fmov.s  @(r0,r2),fr6
    fcmp/gt fr6,fr5
    bf      gx_placed
    mov     #32,r0
    or      r0,r3
gx_placed:

    ! the fog
    mov     #4,r0
    fmov.s  fr14,@(r0,r15)
    mov.w   @(22,r13),r0
    extu.w  r0,r0
    mov     r0,r1
    tst     #8,r0
    JF      gx_nofog
    shlr8   r1
    mov     r1,r0
    tst     #0x80,r0
    JF      gx_nofog
    RL      r2, 24
    mov     r2,r4
    add     #51,r4
    mov.b   @r4,r5
    tst     r5,r5
    JT      gx_nofog
    fldi0   fr3
    fipr    fv0,fv0                 ! the square of the distance, in fr3
    mov     #68,r0
    fmov.s  @(r0,r2),fr4
    fcmp/gt fr4,fr3
    JF      gx_nofog
    fmov    fr3,fr5
    fsrra   fr5
    fmul    fr3,fr5                 ! the distance
    mov     #64,r0
    fmov.s  @(r0,r2),fr6
    fsub    fr6,fr5
    mov     #72,r0
    fmov.s  @(r0,r2),fr6
    fmul    fr6,fr5
    CLAMP   fr5, fr14, fr13
    FS      fr5, 4
    fcmp/gt fr5,fr13                ! 1 > fog
    bt      gx_nofog
    mov     #1,r0
    or      r0,r3
gx_nofog:
    mov.l   r3,@r15

    ! the lighting
    mov.w   @(22,r13),r0
    extu.w  r0,r0
    mov     r0,r7                   ! flags
    RL      r1, 84
    tst     r1,r1
    JF      gx_cb
    RF      fr4, 220                ! the neutral shade
    RL      r1, 72
    tst     r1,r1
    JT      gx_mul
    mov     r7,r0
    tst     #8,r0
    JF      gx_mul
    mov     r7,r1
    shlr8   r1
    mov     r1,r0
    tst     #2,r0
    bt      gx_notown
    I2F     fr4, 24
    RF      fr5, 88
    fadd    fr5,fr4
    bra     gx_mul
    nop
gx_notown:
    tst     #1,r0
    bt      gx_room
    RL      r1, 92
    tst     r1,r1
    bf      gx_dir
    RF      fr4, 88
    bra     gx_mul
    nop
gx_dir:
    RF      fr0, 96
    RF      fr1, 100
    RF      fr2, 104
    fldi0   fr3
    I2F     fr4, 16
    I2F     fr5, 18
    I2F     fr6, 20
    fldi0   fr7
    fipr    fv0,fv4
    RF      fr5, 196
    fmul    fr5,fr7
    RF      fr4, 88
    fadd    fr7,fr4
    RF      fr5, 192
    CLAMP   fr4, fr14, fr5
    bra     gx_mul
    nop
gx_room:
    I2F     fr4, 24
    mov     r7,r0
    tst     #0x80,r0
    bt      gx_roomc
    RL      r1, 108
    tst     r1,r1
    JF      gx_cb
gx_roomc:
    RF      fr5, 192
    CLAMP   fr4, fr14, fr5
gx_mul:
    mov     r7,r0
    tst     #8,r0
    bf      gx_mul1
    RF      fr5, 76
    fcmp/gt fr5,fr4
    bt      gx_smin
    fmov    fr5,fr4
gx_smin:
    RF      fr5, 204
    fmul    fr5,fr4
    RF      fr5, 200
    fsub    fr4,fr5
    fmov    fr5,fr4
    bra     gx_rgba
    nop
gx_mul1:
    fmov    fr13,fr4
gx_rgba:
    RF      fr5, 188
    fmul    fr5,fr4                 ! mul / 255
    B2F     fr5, 4
    fmul    fr4,fr5
    RF      fr6, 32
    fmul    fr6,fr5
    RS      fr5, 120
    B2F     fr5, 5
    fmul    fr4,fr5
    RF      fr6, 36
    fmul    fr6,fr5
    RS      fr5, 124
    B2F     fr5, 6
    fmul    fr4,fr5
    RF      fr6, 40
    fmul    fr6,fr5
    RS      fr5, 128
    B2F     fr5, 7
    RF      fr6, 188
    fmul    fr6,fr5
    RF      fr6, 44
    fmul    fr6,fr5
    RS      fr5, 132
    bra     gx_lit
    nop
gx_cb:
    mov     r8,r4
    add     #-128,r4
    mov     r13,r5
    RL      r1, 112
    jsr     @r1
    nop
gx_lit:

    ! the colour, with the fog taken off
    FL      fr8, 4
    fmov    fr13,fr9
    fsub    fr8,fr9                 ! keep = 1 - fog
    FS      fr9, 8
    RF      fr4, 132
    RF      fr5, 120
    fmul    fr9,fr5
    RF      fr6, 124
    fmul    fr9,fr6
    RF      fr7, 128
    fmul    fr9,fr7
    bsr     gx_pack
    nop
    mov     r10,r1
    add     #24,r1
    mov.l   r0,@r1
    mov.l   @r15,r3
    fcmp/gt fr14,fr8                ! fog > 0
    bf      gx_nofogged
    mov     #64,r0
    or      r0,r3
gx_nofogged:
    tst     r11,r11
    JT      gx_noo
    fcmp/gt fr14,fr8
    JF      gx_ozero
    fmov    fr13,fr4
    RF      fr5, 60
    fmul    fr8,fr5
    RF      fr6, 32
    fmul    fr6,fr5
    RF      fr6, 64
    fmul    fr8,fr6
    RF      fr7, 36
    fmul    fr7,fr6
    RF      fr7, 68
    fmul    fr8,fr7
    RF      fr9, 40
    fmul    fr9,fr7
    bsr     gx_pack
    nop
    mov.l   r0,@r11
    bra     gx_noo
    nop
gx_ozero:
    mov     #0,r0
    mov.l   r0,@r11
gx_noo:
    mov     r10,r1
    add     #28,r1
    mov.b   r3,@r1

    ! the cel shade's colour
    tst     r12,r12
    JT      gx_nocel
    FL      fr9, 8
    fldi0   fr4
    mov.w   @(22,r13),r0
    extu.w  r0,r0
    mov     r0,r7
    mov     r7,r1
    shlr8   r1
    mov     r1,r0
    tst     #1,r0
    bt      gx_celflat
    mov     r7,r0
    tst     #8,r0
    bf      gx_celflat
    RF      fr0, 96
    RF      fr1, 100
    RF      fr2, 104
    fldi0   fr3
    I2F     fr4, 16
    I2F     fr5, 18
    I2F     fr6, 20
    fldi0   fr7
    fipr    fv0,fv4
    RF      fr6, 52
    fmul    fr6,fr7
    RF      fr6, 208
    fsub    fr6,fr7
    fmul    fr15,fr7
    fadd    fr15,fr7
    fmov    fr7,fr4
gx_celflat:
    RF      fr8, 56
    fmul    fr9,fr8                 ! the dark
    RF      fr5, 120
    fmul    fr8,fr5
    RF      fr6, 124
    fmul    fr8,fr6
    RF      fr7, 128
    fmul    fr8,fr7
    bsr     gx_pack
    nop
    mov.l   r0,@r12
gx_nocel:

    ! the reflection
    RL      r1, 80
    tst     r1,r1
    JT      gx_noenv
    mov.w   @(22,r13),r0
    tst     #4,r0
    JT      gx_noenv
    RF      fr4, 120
    CLAMP   fr4, fr14, fr13
    RS      fr4, 164
    RF      fr4, 124
    CLAMP   fr4, fr14, fr13
    RS      fr4, 168
    RF      fr4, 128
    CLAMP   fr4, fr14, fr13
    RS      fr4, 172
    RL      r1, 20
    sub     r9,r1
    mov     #(140-128),r0
    mov.l   r1,@(r0,r8)
    mov     #(144-128),r0
    mov.l   r13,@(r0,r8)
    FL      fr4, 8
    RS      fr4, 160
    mov     r8,r4
    add     #-128,r4
    RL      r1, 116
    jsr     @r1
    nop
gx_noenv:

    add     #32,r10
    tst     r11,r11
    bt      gx_n1
    add     #4,r11
gx_n1:
    tst     r12,r12
    bt      gx_n2
    add     #4,r12
gx_n2:
    dt      r9
    JF      gx_loop

gx_exit:
    add     #32,r15
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

! M_PackARGB: alpha in fr4, red in fr5, green in fr6, blue in fr7 to r0.
! Destroys r1 and fr4-fr7.
gx_pack:
    CLAMP   fr4, fr14, fr13
    fmul    fr12,fr4
    ftrc    fr4,fpul
    sts     fpul,r0
    shll16  r0
    shll8   r0
    CLAMP   fr5, fr14, fr13
    fmul    fr12,fr5
    ftrc    fr5,fpul
    sts     fpul,r1
    shll16  r1
    or      r1,r0
    CLAMP   fr6, fr14, fr13
    fmul    fr12,fr6
    ftrc    fr6,fpul
    sts     fpul,r1
    shll8   r1
    or      r1,r0
    CLAMP   fr7, fr14, fr13
    fmul    fr12,fr7
    ftrc    fr7,fpul
    sts     fpul,r1
    or      r1,r0
    rts
    nop
