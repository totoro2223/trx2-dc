! Hand-written SH-4 for the sparks' tick (sparks/manager.c, M_ControlC): every
! spark that is on ages, fades, turns, moves and shrinks as the C code does.
!
!   void dc_spark_control(M_SPARKCTL *ctl);
!
! M_SPARKCTL (manager.c):
!    0 sparks  4 count  8 the dynamic lights (6 bytes each, "on" first)
!   12 the explosion sprites' base (-1: none)  16 the smoke wind x  20 its z
!   24 the C function that gives an attached spark's world position
!   28 the one that sets off a spark's explosion  32 the one that detaches a
!   spark from its node  36 the sparks' sprite object
!
! SPARK (sparks/types.h): 0 on, 1 s_life, 2 life, 4 pos, 16 prev_pos, 28
! prev_world_pos, 40 vel, 52 src_size, 54 dst_size, 56 size, 58 prev_size, 60
! src_color, 63 dst_color, 66 color, 69 prev_color, 72 scalar, 73
! col_fade_speed, 74 fade_to_black, 76 gravity, 78 max_y_vel, 79 friction, 80
! flags, 84 room_num, 85 node_num, 86 extras, 87 dynamic, 88 rot_angle, 90
! prev_rot_angle, 92 rot_add, 96 sprite_idx, 100 sprite_obj_id; 112 bytes.
!
! The frame: 0 lived, 4 flags.
! r8 the control  r9 the spark  r10 sparks left  r12 the spark + 52
! r13 the spark + 56  r14 the spark + 72 (all kept by the C functions called)

! One colour or size channel from \sb/\so (source) to \db/\do (destination)
! towards \tb/\to with the fade in fr0: src + (dst - src) * fade, as an integer
! byte into \ob/\oo.
    .macro LERP_BYTE sb, so, db, do, ob, oo
    mov.b   @(\so,\sb),r0
    extu.b  r0,r1
    mov.b   @(\do,\db),r0
    extu.b  r0,r2
    sub     r1,r2
    lds     r2,fpul
    float   fpul,fr1
    fmul    fr0,fr1
    lds     r1,fpul
    float   fpul,fr2
    fadd    fr2,fr1
    ftrc    fr1,fpul
    sts     fpul,r0
    mov.b   r0,@(\oo,\ob)
    .endm

! The same towards black: dst * fade.
    .macro SCALE_BYTE db, do, ob, oo
    mov.b   @(\do,\db),r0
    extu.b  r0,r1
    lds     r1,fpul
    float   fpul,fr1
    fmul    fr0,fr1
    ftrc    fr1,fpul
    sts     fpul,r0
    mov.b   r0,@(\oo,\ob)
    .endm

! A byte moved.
    .macro MOVE_BYTE sb, so, db, do
    mov.b   @(\so,\sb),r0
    mov.b   r0,@(\do,\db)
    .endm

! A word of the spark moved.
    .macro MOVE_WORD from, to
    mov.l   @(\from,r9),r0
    mov.l   r0,@(\to,r9)
    .endm

! One axis of the velocity slowed by 1 / 2^r2 (r2 negative: the count of a
! right shift): vel -= vel >> n.
    .macro SLOW off
    mov.l   @(\off,r9),r3
    mov     r3,r4
    shad    r2,r4
    sub     r4,r3
    mov.l   r3,@(\off,r9)
    .endm

! One axis of the position moved by vel >> 5.
    .macro MOVE_AXIS pos, vel
    mov.l   @(\vel,r9),r3
    shad    r2,r3
    mov.l   @(\pos,r9),r4
    add     r3,r4
    mov.l   r4,@(\pos,r9)
    .endm

! Whether the colour (r2, r3, r4) is under the limit in all channels: T clear
! (bt \no) where it is not.
    .macro UNDER limit, no
    mov     #\limit,r5
    cmp/hs  r5,r2
    bt      \no
    cmp/hs  r5,r3
    bt      \no
    cmp/hs  r5,r4
    bt      \no
    .endm

    .text
    .align 2
    .global _dc_spark_control
_dc_spark_control:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    add     #-16,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10

sc_loop:
    tst     r10,r10
    bf      1f
    bra     sc_done
    nop
1:  mov.b   @r9,r0                      ! on
    tst     r0,r0
    bf      2f
    bra     sc_next
    nop
2:  mov     r9,r12
    add     #52,r12
    mov     r9,r13
    add     #56,r13
    mov     r9,r14
    add     #72,r14
    mov.w   @(8,r14),r0                 ! flags
    extu.w  r0,r0
    mov.l   r0,@(4,r15)
    shlr8   r0                          ! life is cut by one unless the spark is
    tst     #4,r0                       ! attached to a position (SPARK_F_ATTACHED_POS)
    bt      3f                          ! and young
    mov.b   @(2,r9),r0
    extu.b  r0,r0
    mov     #16,r1
    cmp/gt  r1,r0
    bf      4f
3:  mov.b   @(2,r9),r0
    extu.b  r0,r0
    tst     r0,r0
    bt      4f
    add     #-1,r0
    mov.b   r0,@(2,r9)
4:  mov.b   @(2,r9),r0
    extu.b  r0,r0
    tst     r0,r0
    bf      5f
    mov.b   @(15,r14),r0                ! life out: its dynamic light goes
    cmp/eq  #-1,r0
    bt      6f
    extu.b  r0,r0
    mov     r0,r1
    shll    r1
    shll2   r0
    add     r1,r0                       ! * 6
    mov.l   @(8,r8),r1
    add     r1,r0
    mov     #0,r1
    mov.b   r1,@r0                      ! on = false
    mov     #-1,r0
    mov.b   r0,@(15,r14)
6:  mov     #0,r0
    mov.b   r0,@r9
    bra     sc_next
    nop
5:  mov.b   @(1,r9),r0
    extu.b  r0,r1
    mov.b   @(2,r9),r0
    extu.b  r0,r0
    sub     r0,r1
    mov.l   r1,@(0,r15)                 ! lived
    MOVE_WORD 4, 16                     ! prev_pos = pos
    MOVE_WORD 8, 20
    MOVE_WORD 12, 24
    mov.l   @(4,r15),r0
    and     #0xC0,r0                    ! SPARK_F_FX | SPARK_F_ITEM
    tst     r0,r0
    bf      sc_att
    mov.l   @(4,r15),r0
    shlr8   r0
    tst     #0x10,r0                    ! SPARK_F_ATTACHED_NODE
    bf      sc_att
    MOVE_WORD 4, 28                     ! a free spark: its world position is its own
    MOVE_WORD 8, 32
    MOVE_WORD 12, 36
    bra     sc_pw
    nop
sc_att:
    mov     r9,r4
    mov     r9,r5
    add     #28,r5
    mov.l   @(24,r8),r1
    jsr     @r1
    nop
sc_pw:
    MOVE_BYTE r13, 10, r13, 13          ! prev_color = color
    MOVE_BYTE r13, 11, r13, 14
    MOVE_BYTE r13, 12, r13, 15
    MOVE_BYTE r13, 0, r13, 2            ! prev_size = size
    MOVE_BYTE r13, 1, r13, 3
    mov.w   @(16,r14),r0                ! prev_rot_angle = rot_angle
    mov.w   r0,@(18,r14)

    ! the colour: src -> dst while it fades in, then to black
    mov.b   @(1,r14),r0
    extu.b  r0,r1                       ! col_fade_speed
    mov.l   @(0,r15),r2
    tst     r1,r1
    bt      cf_black
    cmp/ge  r1,r2
    bt      cf_black
    lds     r2,fpul
    float   fpul,fr0
    lds     r1,fpul
    float   fpul,fr1
    fdiv    fr1,fr0                     ! lived / col_fade_speed
    LERP_BYTE r13, 4, r13, 7, r13, 10
    LERP_BYTE r13, 5, r13, 8, r13, 11
    LERP_BYTE r13, 6, r13, 9, r13, 12
    bra     cf_done
    nop
cf_black:
    mov.b   @(2,r14),r0
    extu.b  r0,r1                       ! fade_to_black
    mov.b   @(2,r9),r0
    extu.b  r0,r2                       ! life
    tst     r1,r1
    bt      cf_dst
    cmp/ge  r1,r2
    bt      cf_dst
    lds     r2,fpul
    float   fpul,fr0
    lds     r1,fpul
    float   fpul,fr1
    fdiv    fr1,fr0                     ! life / fade_to_black
    SCALE_BYTE r13, 7, r13, 10
    SCALE_BYTE r13, 8, r13, 11
    SCALE_BYTE r13, 9, r13, 12
    bra     cf_done
    nop
cf_dst:
    MOVE_BYTE r13, 7, r13, 10
    MOVE_BYTE r13, 8, r13, 11
    MOVE_BYTE r13, 9, r13, 12
cf_done:
    mov.b   @(2,r9),r0                  ! underwater: the size it fades to shrinks
    extu.b  r0,r2                       ! at the moment the black begins
    mov.b   @(2,r14),r0
    extu.b  r0,r1
    cmp/eq  r1,r2
    bf      uw_done
    mov.l   @(4,r15),r0
    shlr8   r0
    tst     #8,r0                       ! SPARK_F_UNDERWATER
    bt      uw_done
    mov.b   @(2,r12),r0
    extu.b  r0,r0
    shlr2   r0
    mov.b   r0,@(2,r12)
    mov.b   @(3,r12),r0
    extu.b  r0,r0
    shlr2   r0
    mov.b   r0,@(3,r12)
uw_done:
    mov.l   @(4,r15),r0
    tst     #0x10,r0                    ! SPARK_F_ROTATE
    bt      rot_done
    mov.w   @(16,r14),r0
    extu.w  r0,r1
    mov     #20,r0
    mov.b   @(r0,r14),r0                ! rot_add, signed
    add     r0,r1
    shll16  r1                          ! & 0xFFF
    shll2   r1
    shll2   r1
    shlr16  r1
    shlr2   r1
    shlr2   r1
    mov     r1,r0
    mov.w   r0,@(16,r14)
rot_done:
    mov.l   @(4,r15),r0
    shlr8   r0
    tst     #2,r0                       ! SPARK_F_ALT_SPRITE
    bt      as_done
    mov.l   @(12,r8),r1                 ! the explosion sprites' base
    mov     r1,r6
    add     #1,r6
    tst     r6,r6
    bt      as_done                     ! none (-1)
    mov.l   @(36,r8),r0
    mov.l   r0,@(28,r14)                ! sprite_obj_id
    mov.b   @(10,r13),r0
    extu.b  r0,r2
    mov.b   @(11,r13),r0
    extu.b  r0,r3
    mov.b   @(12,r13),r0
    extu.b  r0,r4
    UNDER   16, as_64
    add     #3,r1
    bra     as_set
    nop
as_64:
    UNDER   64, as_96
    add     #2,r1
    bra     as_set
    nop
as_96:
    UNDER   96, as_set
    add     #1,r1
as_set:
    mov.l   r1,@(24,r14)                ! sprite_idx
as_done:
    mov.b   @(14,r14),r0                ! extras: the explosion at its time
    extu.b  r0,r1
    mov     r1,r0
    and     #7,r0
    tst     r0,r0
    bt      ex_done
    shlr2   r1
    shlr    r1
    mov.l   @(0,r15),r2
    cmp/eq  r1,r2
    bf      ex_done
    mov     r9,r4
    mov.l   @(28,r8),r1
    jsr     @r1
    nop
ex_done:
    mov.w   @(4,r14),r0                 ! vel.y += gravity
    mov.l   @(44,r9),r1
    add     r0,r1
    mov.l   r1,@(44,r9)
    mov.b   @(6,r14),r0                 ! max_y_vel
    tst     r0,r0
    bt      my_done
    shll2   r0
    shll2   r0
    shll    r0
    mov     r0,r2                       ! the limit: max_y_vel * 32
    cmp/pl  r1
    bt      my_pos
    cmp/pz  r1
    bt      my_done                     ! nought
    cmp/gt  r1,r2                       ! vy < 0 and vy < limit
    bf      my_done
    bra     my_set
    nop
my_pos:
    cmp/gt  r2,r1                       ! vy > 0 and vy > limit
    bf      my_done
my_set:
    mov.l   r2,@(44,r9)
my_done:
    mov.b   @(7,r14),r0
    extu.b  r0,r1                       ! friction
    mov     r1,r0
    and     #15,r0
    tst     r0,r0
    bt      fr1_done
    neg     r0,r2
    SLOW    40
    SLOW    48
fr1_done:
    mov     r1,r0
    and     #0xF0,r0
    tst     r0,r0
    bt      fr2_done
    shlr2   r1
    shlr2   r1
    neg     r1,r2
    SLOW    44
fr2_done:
    mov     #-5,r2
    MOVE_AXIS 4, 40                     ! pos += vel >> 5
    MOVE_AXIS 8, 44
    MOVE_AXIS 12, 48
    mov.l   @(4,r15),r0
    shlr8   r0
    tst     #1,r0                       ! SPARK_F_OUTSIDE
    bt      ow_done
    mov.l   @(16,r8),r3
    shar    r3
    mov.l   @(4,r9),r4
    add     r3,r4
    mov.l   r4,@(4,r9)
    mov.l   @(20,r8),r3
    shar    r3
    mov.l   @(12,r9),r4
    add     r3,r4
    mov.l   r4,@(12,r9)
ow_done:
    mov.b   @(1,r9),r0                  ! the size across the life
    extu.b  r0,r1
    tst     r1,r1
    bt      sz_zero
    mov.l   @(0,r15),r2
    lds     r2,fpul
    float   fpul,fr0
    lds     r1,fpul
    float   fpul,fr1
    fdiv    fr1,fr0                     ! lived / s_life
    LERP_BYTE r12, 0, r12, 2, r13, 0
    LERP_BYTE r12, 1, r12, 3, r13, 1
    bra     sz_done
    nop
sz_zero:
    MOVE_BYTE r12, 0, r13, 0
    MOVE_BYTE r12, 1, r13, 1
sz_done:
    mov.l   @(4,r15),r0                 ! hanging from a node (an item's):
    and     #0x80,r0                    ! SPARK_F_ITEM
    tst     r0,r0
    bt      dt_done
    mov.l   @(4,r15),r0
    shlr8   r0
    tst     #0x10,r0                    ! SPARK_F_ATTACHED_NODE
    bt      dt_done
    mov.b   @(13,r14),r0
    extu.b  r0,r1                       ! node_num
    mov     #2,r2
    cmp/eq  r2,r1
    bt      dt_call
    mov     #3,r2
    cmp/eq  r2,r1
    bf      dt_done
dt_call:
    mov     r9,r4
    mov.l   @(0,r15),r5
    mov.l   @(32,r8),r1
    jsr     @r1
    nop
dt_done:
sc_next:
    mov     #112,r0                     ! sizeof(SPARK)
    add     r0,r9
    add     #-1,r10
    bra     sc_loop
    nop
sc_done:
    add     #16,r15
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop

! ---- the dynamic light pass (manager.c, M_LightsC)
!
!   void dc_spark_lights(M_SPARKCTL *ctl);
!
! Every spark that is on and has a dynamic light that is on lights its place:
! the position takes a random shift, the colour and the light's falloff follow
! the spark's age (the same draws from the control generator, ctl + 40, as
! Random_GetControl). Each light goes to the function at ctl + 44 as seven
! words: x, y, z, falloff, r, g, b.
!
! The frame: 0..24 the light, 28 the draw.
! r8 the control  r9 the spark  r10 sparks left  r12 the spark's light
! r14 the spark + 72

    .global _dc_spark_lights
_dc_spark_lights:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r12,@-r15
    mov.l   r14,@-r15
    add     #-32,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10

sl_loop:
    tst     r10,r10
    bf      1f
    bra     sl_done
    nop
1:  mov.b   @r9,r0                      ! on
    tst     r0,r0
    bf      2f
    bra     sl_next
    nop
2:  mov     r9,r14
    add     #72,r14
    mov.b   @(15,r14),r0                ! dynamic
    cmp/eq  #-1,r0
    bf      3f
    bra     sl_next
    nop
3:  extu.b  r0,r0
    mov     r0,r1
    shll    r1
    shll2   r0
    add     r1,r0                       ! * 6
    mov.l   @(8,r8),r1
    add     r1,r0
    mov     r0,r12                      ! its light
    mov.b   @r12,r0
    tst     r0,r0                       ! on?
    bf      4f
    bra     sl_next
    nop
4:  mov.w   @(8,r14),r0                 ! flags
    extu.w  r0,r2
    mov     r2,r0
    and     #0xC0,r0
    tst     r0,r0
    bf      sl_att
    mov     r2,r0
    shlr8   r0
    tst     #0x10,r0
    bf      sl_att
    mov.l   @(4,r9),r0
    mov.l   r0,@(0,r15)
    mov.l   @(8,r9),r0
    mov.l   r0,@(4,r15)
    mov.l   @(12,r9),r0
    mov.l   r0,@(8,r15)
    bra     sl_rng
    nop
sl_att:
    mov     r9,r4
    mov     r15,r5
    mov.l   @(24,r8),r1
    jsr     @r1
    nop
sl_rng:
    mov.l   @(40,r8),r1                 ! rnd: state = state * 0x41C64E6D + 0x3039
    mov.l   @r1,r2
    mov.l   .Lmul,r3
    mul.l   r3,r2
    sts     macl,r2
    mov.l   .Ladd,r3
    add     r3,r2
    mov.l   r2,@r1
    shlr8   r2
    shlr2   r2                          ! >> 10
    extu.w  r2,r2
    shll    r2
    shlr    r2                          ! & 0x7FFF
    mov.l   r2,@(28,r15)
    mov     r2,r0
    and     #15,r0
    shll2   r0
    shll2   r0                          ! (rnd & 0xF) << 4
    mov.l   @(0,r15),r1
    add     r0,r1
    mov.l   r1,@(0,r15)
    mov     r2,r0
    and     #0xF0,r0
    mov.l   @(4,r15),r1
    add     r0,r1
    mov.l   r1,@(4,r15)
    mov     r2,r0
    shlr2   r0
    shlr2   r0
    and     #0xF0,r0                    ! (rnd >> 4) & 0xF0
    mov.l   @(8,r15),r1
    add     r0,r1
    mov.l   r1,@(8,r15)
    mov.b   @(1,r9),r0                  ! the age
    extu.b  r0,r1
    mov.b   @(2,r9),r0
    extu.b  r0,r0
    sub     r0,r1
    add     #-1,r1                      ! falloff = s_life - life - 1
    mov     r2,r0
    and     #0x1F,r0
    mov     r0,r5                       ! rnd & 0x1F
    mov     #2,r0
    cmp/ge  r0,r1
    bt      sl_f2
    mov.b   @(1,r12),r0                 ! the light's own falloff rises
    extu.b  r0,r3
    mov     #28,r0
    cmp/hs  r0,r3
    bt      5f
    add     #6,r3
    mov     r3,r0
    mov.b   r0,@(1,r12)
5:  mov     #-1,r6
    extu.b  r6,r6                       ! 255
    sub     r5,r6
    mov     r1,r0
    shll2   r0
    shll    r0
    sub     r0,r6                       ! r = 255 - (rnd & 0x1F) - (f << 3)
    mov     #-1,r7
    extu.b  r7,r7
    sub     r5,r7
    mov     r1,r0
    shll2   r0
    shll2   r0
    sub     r0,r7                       ! g = 255 - (rnd & 0x1F) - (f << 4)
    mov     #-1,r4
    extu.b  r4,r4
    sub     r5,r4
    mov     r1,r0
    shll2   r0
    shll2   r0
    shll2   r0
    sub     r0,r4                       ! b = 255 - (rnd & 0x1F) - (f << 6)
    bra     sl_col
    nop
sl_f2:
    mov     #4,r0
    cmp/ge  r0,r1
    bt      sl_f4
    mov.b   @(1,r12),r0
    extu.b  r0,r3
    mov     #28,r0
    cmp/hs  r0,r3
    bt      6f
    add     #6,r3
    mov     r3,r0
    mov.b   r0,@(1,r12)
6:  mov     #-1,r6
    extu.b  r6,r6
    sub     r5,r6
    mov     r1,r0
    shll2   r0
    shll    r0
    sub     r0,r6                       ! r = 255 - (rnd & 0x1F) - (f << 3)
    mov     #-128,r7
    neg     r7,r7                       ! 128
    mov     r1,r0
    shll2   r0
    shll    r0
    sub     r0,r7                       ! g = 128 - (f << 3)
    mov     #4,r4
    sub     r1,r4
    shll2   r4                          ! b = (4 - f) << 2, then << 3
    cmp/pz  r4
    bt      7f
    mov     #0,r4
    bra     sl_col
    nop
7:  shll2   r4
    shll    r4
    bra     sl_col
    nop
sl_f4:
    mov.b   @(1,r12),r0                 ! and falls
    extu.b  r0,r3
    tst     r3,r3
    bt      8f
    add     #-1,r3
    mov     r3,r0
    mov.b   r0,@(1,r12)
8:  mov     r5,r6
    mov     #-32,r0
    extu.b  r0,r0                       ! 224
    add     r0,r6                       ! r = (rnd & 0x1F) + 224
    mov     r2,r0
    shlr2   r0
    shlr2   r0
    and     #0x1F,r0
    mov     r0,r7
    mov     #-128,r0
    extu.b  r0,r0                       ! 128
    add     r0,r7                       ! g = ((rnd >> 4) & 0x1F) + 128
    mov     r2,r0
    shlr8   r0
    and     #0x3F,r0
    mov     r0,r4                       ! b = (rnd >> 8) & 0x3F
sl_col:
    mov.b   @(1,r12),r0                 ! the falloff, held to 31
    extu.b  r0,r1
    mov     #31,r3
    cmp/gt  r3,r1
    bf      9f
    mov     r3,r1
9:  mov.l   r1,@(12,r15)
    mov.w   @(8,r14),r0
    extu.w  r0,r0
    shlr8   r0
    tst     #0x20,r0                    ! SPARK_F_GREEN
    bt      sl_rgb
    mov.l   r4,@(16,r15)                ! green: b, r, g
    mov.l   r6,@(20,r15)
    mov.l   r7,@(24,r15)
    bra     sl_emit
    nop
sl_rgb:
    mov.l   r6,@(16,r15)
    mov.l   r7,@(20,r15)
    mov.l   r4,@(24,r15)
sl_emit:
    mov     r15,r4
    mov.l   @(44,r8),r1
    jsr     @r1
    nop
sl_next:
    mov     #112,r0
    add     r0,r9
    add     #-1,r10
    bra     sl_loop
    nop
sl_done:
    add     #32,r15
    mov.l   @r15+,r14
    mov.l   @r15+,r12
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Lmul:
    .long   0x41C64E6D
.Ladd:
    .long   0x3039
