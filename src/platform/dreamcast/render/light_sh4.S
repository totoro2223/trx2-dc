! Hand-written SH-4 for the batcher's vertex light (batcher.c, M_LightVertex):
! TR3's colour light (the object's three directions, the room's own ambient and
! the dynamic lights), TR1 and TR2's shade (own, object, room with passing
! lights and flicker), and the caustics. The generic transform calls it for the
! vertices it does not light inline. The C stays as M_LightVertex, which a
! build with DC_EMIT_VERIFY compares it with (xform_gen_sh4.S's verify).
!
!   void dc_light_slow(M_GENRUN *run, const OUTPUT_MESH_VERTEX *vertex);
!
! Writes run->rgba (120..132). Uses r0-r7 and fr0-fr11 only (a callee that
! keeps r8-r14 and fr12-fr15 untouched). r6 is run + 254, so that the fields
! from 224 on lie within a signed byte of it.
!
! M_GENRUN: 28 il  32 tint r g b a  72 lighting  76 min_shade  88 adder
!   92 has_direction  96 l[3]  120 rgba[4]  136 -  188 1/255  192 shade max
!   196 1/65536  200 2.0  204 1/shade neutral  212 255.0  220 shade neutral
!   224 caustic table (32 floats)  228 rgb lights  232 point lights
!   236 world matrix  240 tr3  244 caustics  248 room lights  252 num lights
!   256 num rgb lights  260 tr3 count  264 tr3 double  268 flicker  272 1/32
!   276 16.0  280 scratch (the doubling)  284, 288, 292 the caustic hash factors
! M_INSTANCE_LIGHT: 24 tr3_ambient  40 tr3_dir[3][3]  76 tr3_color[3][3]
! OUTPUT_MESH_VERTEX: 4 colour r g b a  8 x 10 y 12 z  16 nx 18 ny 20 nz
!   22 flags  24 shade  26 light_table_idx
! Flags: 0x08 no lighting  0x80 dynamic light  0x100 object light  0x200 own

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

    .macro RF fr, off
    .if \off > 127
    mov     #(\off-254),r0
    fmov.s  @(r0,r6),\fr
    .else
    mov     #\off,r0
    fmov.s  @(r0,r4),\fr
    .endif
    .endm

    .macro SF fr, off
    .if \off > 127
    mov     #(\off-254),r0
    fmov.s  \fr,@(r0,r6)
    .else
    mov     #\off,r0
    fmov.s  \fr,@(r0,r4)
    .endif
    .endm

    .macro XF fr, off
    mov     #(\off-254),r0
    fmov.s  @(r0,r6),\fr
    .endm

    .macro XS fr, off
    mov     #(\off-254),r0
    fmov.s  \fr,@(r0,r6)
    .endm

    .macro XL reg, off
    mov     #(\off-254),r0
    mov.l   @(r0,r6),\reg
    .endm

    .macro IF fr, off
    mov     #\off,r0
    fmov.s  @(r0,r7),\fr
    .endm

! A signed halfword of the vertex to a float.
    .macro I2F fr, disp
    mov.w   @(\disp,r5),r0
    exts.w  r0,r0
    lds     r0,fpul
    float   fpul,\fr
    .endm

! A byte of the vertex colour to a float.
    .macro B2F fr, disp
    mov.b   @(\disp,r5),r0
    extu.b  r0,r0
    lds     r0,fpul
    float   fpul,\fr
    .endm

! T set when the low flag byte has none of imm / the high byte has none.
    .macro VL imm
    mov.w   @(22,r5),r0
    tst     #\imm,r0
    .endm

    .macro VH imm
    mov.w   @(22,r5),r0
    shlr8   r0
    tst     #\imm,r0
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

! The vertex's place in the world, into fr9, fr10, fr11, from the matrix
! (three rows of four). Uses r1, fr0-fr5.
    .macro WORLD
    I2F     fr0, 8
    I2F     fr1, 10
    I2F     fr2, 12
    XL      r1, 236
    fmov.s  @r1+,fr4
    fmul    fr0,fr4
    fmov.s  @r1+,fr5
    fmul    fr1,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fmul    fr2,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fadd    fr5,fr4
    fmov    fr4,fr9
    fmov.s  @r1+,fr4
    fmul    fr0,fr4
    fmov.s  @r1+,fr5
    fmul    fr1,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fmul    fr2,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fadd    fr5,fr4
    fmov    fr4,fr10
    fmov.s  @r1+,fr4
    fmul    fr0,fr4
    fmov.s  @r1+,fr5
    fmul    fr1,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fmul    fr2,fr5
    fadd    fr5,fr4
    fmov.s  @r1+,fr5
    fadd    fr5,fr4
    fmov    fr4,fr11
    .endm

    .text
    .align 2
    .global _dc_light_slow
_dc_light_slow:
    mov     r4,r6
    add     #127,r6
    add     #127,r6
    mov     #28,r0
    mov.l   @(r0,r4),r7             ! il
    XL      r1, 240
    tst     r1,r1
    JT      ls_12

    ! ---- TR3: (colour + dynamic) * (object or own light) * doubling * tint
    fldi0   fr0
    SF      fr0, 120
    SF      fr0, 124
    SF      fr0, 128
    fldi1   fr0
    XS      fr0, 280                ! the doubling, 1 with the lighting off
    mov     #72,r0
    mov.l   @(r0,r4),r1
    tst     r1,r1
    JT      l3_one
    XF      fr0, 264
    XS      fr0, 280
    VL      0x08
    JF      l3_one
    ! the room's passing lights
    VL      0x80
    JT      l3_obj
    XL      r1, 256
    cmp/pl  r1
    JF      l3_obj
    XL      r1, 248
    tst     r1,r1
    JT      l3_obj
    WORLD
    XL      r2, 256
    XL      r1, 228
    fldi0   fr6
    fmov    fr6,fr7
    fmov    fr6,fr8
l3_dl:
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fsub    fr9,fr0
    fsub    fr10,fr1
    fsub    fr11,fr2
    fmul    fr0,fr0
    fmul    fr1,fr1
    fmul    fr2,fr2
    fadd    fr1,fr0
    fadd    fr2,fr0                 ! dist_sq
    fmov.s  @r1+,fr3                ! radius
    fmov.s  @r1+,fr4                ! 1 / radius
    fmov    fr3,fr5
    fmul    fr3,fr5
    fcmp/gt fr0,fr5
    JF      l3_dskip
    fldi0   fr1
    fcmp/gt fr1,fr0
    bf      l3_dz
    fmov    fr0,fr2
    fsrra   fr2
    fmul    fr0,fr2
    bra     l3_dd
    nop
l3_dz:
    fldi0   fr2
l3_dd:
    fsub    fr2,fr3
    fmul    fr4,fr3
    fmov.s  @r1+,fr0
    fmul    fr3,fr0
    fadd    fr0,fr6
    fmov.s  @r1+,fr0
    fmul    fr3,fr0
    fadd    fr0,fr7
    fmov.s  @r1+,fr0
    fmul    fr3,fr0
    fadd    fr0,fr8
    bra     l3_dn
    nop
l3_dskip:
    add     #12,r1
l3_dn:
    dt      r2
    JF      l3_dl
    SF      fr6, 120
    SF      fr7, 124
    SF      fr8, 128
l3_obj:
    fldi1   fr8
    fmov    fr8,fr9
    fmov    fr8,fr10
    VH      0x01
    JF      l3_object
    VH      0x02
    JF      l3_own
    bra     l3_out
    nop
l3_one:
    fldi1   fr8
    fmov    fr8,fr9
    fmov    fr8,fr10
    bra     l3_out
    nop

    ! the object's light: ambient plus each direction facing the normal
l3_object:
    IF      fr8, 24
    IF      fr9, 28
    IF      fr10, 32
    XL      r2, 260
    tst     r2,r2
    JT      l3_oc
    mov     r7,r1
    add     #40,r1
    mov     r7,r3
    add     #76,r3
    I2F     fr4, 16
    I2F     fr5, 18
    I2F     fr6, 20
l3_ol:
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi0   fr3
    fldi0   fr7
    fipr    fv0,fv4
    fldi0   fr0
    fcmp/gt fr0,fr7
    bf      l3_oskip
    fmov.s  @r3+,fr0
    fmul    fr7,fr0
    fadd    fr0,fr8
    fmov.s  @r3+,fr0
    fmul    fr7,fr0
    fadd    fr0,fr9
    fmov.s  @r3+,fr0
    fmul    fr7,fr0
    fadd    fr0,fr10
    bra     l3_on
    nop
l3_oskip:
    add     #12,r3
l3_on:
    dt      r2
    JF      l3_ol
l3_oc:
    fldi0   fr0
    fldi1   fr1
    CLAMP   fr8, fr0, fr1
    CLAMP   fr9, fr0, fr1
    CLAMP   fr10, fr0, fr1
    bra     l3_out
    nop

    ! the room's own light: the ambient by the vertex's step of 32
l3_own:
    mov.w   @(24,r5),r0
    exts.w  r0,r0
    lds     r0,fpul
    float   fpul,fr0
    RF      fr1, 192
    fsub    fr0,fr1
    XF      fr2, 272
    fmul    fr2,fr1                 ! (max - shade) / 32
    ftrc    fr1,fpul
    sts     fpul,r0
    lds     r0,fpul
    float   fpul,fr2
    fcmp/gt fr1,fr2
    bf      l3_fl
    fldi1   fr3
    fsub    fr3,fr2                 ! floor
l3_fl:
    fldi0   fr3
    fcmp/gt fr3,fr2
    bt      l3_s8
    RF      fr2, 212
l3_s8:
    RF      fr4, 212
    IF      fr8, 24
    fmul    fr2,fr8
    fdiv    fr4,fr8
    IF      fr9, 28
    fmul    fr2,fr9
    fdiv    fr4,fr9
    IF      fr10, 32
    fmul    fr2,fr10
    fdiv    fr4,fr10
    fldi0   fr0
    fldi1   fr1
    CLAMP   fr8, fr0, fr1
    CLAMP   fr9, fr0, fr1
    CLAMP   fr10, fr0, fr1

    ! out = (colour / 255 + add) * mul * doubling * tint
l3_out:
    XF      fr11, 280
    RF      fr1, 188
    B2F     fr0, 4
    fmul    fr1,fr0
    RF      fr2, 120
    fadd    fr2,fr0
    fmul    fr8,fr0
    fmul    fr11,fr0
    RF      fr2, 32
    fmul    fr2,fr0
    SF      fr0, 120
    B2F     fr0, 5
    fmul    fr1,fr0
    RF      fr2, 124
    fadd    fr2,fr0
    fmul    fr9,fr0
    fmul    fr11,fr0
    RF      fr2, 36
    fmul    fr2,fr0
    SF      fr0, 124
    B2F     fr0, 6
    fmul    fr1,fr0
    RF      fr2, 128
    fadd    fr2,fr0
    fmul    fr10,fr0
    fmul    fr11,fr0
    RF      fr2, 40
    fmul    fr2,fr0
    SF      fr0, 128
    B2F     fr0, 7
    fmul    fr1,fr0
    RF      fr2, 44
    fmul    fr2,fr0
    SF      fr0, 132
    rts
    nop

    ! ---- TR1 and TR2: the shade, its caustics, mul = 2 - shade / neutral
ls_12:
    RF      fr8, 220                ! neutral
    mov     #72,r0
    mov.l   @(r0,r4),r1
    tst     r1,r1
    JT      l12_mul
    VL      0x08
    JF      l12_mul
    fldi0   fr3
    RF      fr7, 192                ! the shade's top
    VH      0x02
    JF      l12_own
    VH      0x01
    JF      l12_obj
    ! the room's shade, with a light passing and flicker
    mov.w   @(24,r5),r0
    exts.w  r0,r0
    lds     r0,fpul
    float   fpul,fr8
    VL      0x80
    JT      l12_clamp
    XL      r1, 248
    tst     r1,r1
    JT      l12_clamp
    XL      r2, 252
    cmp/pl  r2
    bf      l12_flick
    WORLD
    XL      r1, 232
l12_dl:
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fsub    fr9,fr0
    fsub    fr10,fr1
    fsub    fr11,fr2
    fmul    fr0,fr0
    fmul    fr1,fr1
    fmul    fr2,fr2
    fadd    fr1,fr0
    fadd    fr2,fr0                 ! dist_sq
    fmov.s  @r1+,fr4                ! radius_sq
    fcmp/gt fr4,fr0
    bt      l12_dskip
    fmov.s  @r1+,fr5                ! max shade
    fmov.s  @r1+,fr6                ! 1 / divider
    fmul    fr6,fr0
    fsub    fr0,fr5
    fsub    fr5,fr8
    bra     l12_dn
    nop
l12_dskip:
    add     #8,r1
l12_dn:
    dt      r2
    JF      l12_dl
    fldi0   fr3
    fcmp/gt fr8,fr3
    bf      l12_flick
    fldi0   fr8
l12_flick:
    XF      fr0, 268
    fldi0   fr1
    fcmp/eq fr1,fr0
    bt      l12_clamp
    mov.w   @(26,r5),r0
    and     #31,r0
    lds     r0,fpul
    float   fpul,fr2
    XF      fr1, 276
    fsub    fr1,fr2
    fmul    fr0,fr2
    fadd    fr2,fr8
l12_clamp:
    fldi0   fr3
    CLAMP   fr8, fr3, fr7
    bra     l12_caus
    nop
l12_own:
    RF      fr8, 88
    mov.w   @(24,r5),r0
    exts.w  r0,r0
    lds     r0,fpul
    float   fpul,fr0
    fadd    fr0,fr8
    bra     l12_caus
    nop
l12_obj:
    RF      fr8, 88
    mov     #92,r0
    mov.l   @(r0,r4),r1
    tst     r1,r1
    JT      l12_caus
    RF      fr0, 96
    RF      fr1, 100
    RF      fr2, 104
    fldi0   fr3
    I2F     fr4, 16
    I2F     fr5, 18
    I2F     fr6, 20
    fldi0   fr7
    fipr    fv0,fv4
    RF      fr0, 196
    fmul    fr0,fr7
    fadd    fr7,fr8
    fldi0   fr3
    RF      fr7, 192
    CLAMP   fr8, fr3, fr7
l12_caus:
    XL      r1, 244
    tst     r1,r1
    JT      l12_mul
    mov.w   @(8,r5),r0
    exts.w  r0,r0
    XL      r2, 284
    mul.l   r2,r0
    sts     macl,r3
    mov.w   @(10,r5),r0
    exts.w  r0,r0
    XL      r2, 288
    mul.l   r2,r0
    sts     macl,r1
    xor     r1,r3
    mov.w   @(12,r5),r0
    exts.w  r0,r0
    XL      r2, 292
    mul.l   r2,r0
    sts     macl,r1
    xor     r1,r3                   ! the hash
    shlr8   r3
    shlr2   r3
    shlr    r3
    mov     #31,r0
    and     r0,r3                   ! the table's slot
    shll2   r3
    XL      r1, 224
    add     r1,r3
    fmov.s  @r3,fr0
    fadd    fr0,fr8
    fldi0   fr3
    RF      fr7, 192
    CLAMP   fr8, fr3, fr7
l12_mul:
    VL      0x08
    bt      l12_lit
    fldi1   fr8
    bra     l12_out
    nop
l12_lit:
    RF      fr1, 76
    fcmp/gt fr1,fr8
    bt      l12_keep
    fmov    fr1,fr8
l12_keep:
    RF      fr1, 204
    fmul    fr1,fr8
    RF      fr1, 200
    fsub    fr8,fr1
    fmov    fr1,fr8
l12_out:
    RF      fr1, 188
    fmul    fr1,fr8                 ! m = mul / 255
    B2F     fr0, 4
    fmul    fr8,fr0
    RF      fr2, 32
    fmul    fr2,fr0
    SF      fr0, 120
    B2F     fr0, 5
    fmul    fr8,fr0
    RF      fr2, 36
    fmul    fr2,fr0
    SF      fr0, 124
    B2F     fr0, 6
    fmul    fr8,fr0
    RF      fr2, 40
    fmul    fr2,fr0
    SF      fr0, 128
    B2F     fr0, 7
    fmul    fr1,fr0
    RF      fr2, 44
    fmul    fr2,fr0
    SF      fr0, 132
    rts
    nop
