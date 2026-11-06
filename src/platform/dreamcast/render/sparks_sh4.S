! Hand-written SH-4 for the sparks' staging (sprites.c, M_StageSpark): every
! spark that is on goes to a particle of the batcher's list, as the C code
! makes it.
!
!   void dc_stage_sparks(DC_SPARKRUN *run);
!
! DC_SPARKRUN (sprites.c):
!    0 sparks  4 count  8 out  12 room  16 written (out)  20, 24, 28 particles
!   added, blended (out)  32..44 the view's depth row  48 near  52 far  56 the
!   interpolation rate  60 mesh_count  64 ps1 blending  68 the picture corners
!   of the sprite last staged  72 that sprite's index (a pointer)  76 the C
!   function that gives the world position of an attached spark  80 the one
!   that gives a turning spark's sin and cos  84 the one that looks a sprite's
!   corners up  88 the gain of a sprite's colour  92 255.0  96 381.0  100 4.0
!   104 2.0  108 1 / 16384  112 0.5
!
! SPARK (sparks/types.h): 0 on, 1 s_life, 2 life, 4 pos, 16 prev_pos, 56 size
! (w, h), 66 color (r, g, b), 72 scalar, 80 flags, 96 sprite_idx, 104
! draw_type, 108 context; 112 bytes.
!
! The frame: 0 x, 4 y, 8 z (the position), 12 vpos_z, 16 sw, 20 sh, 24 w, 28 h,
! 32 the alpha is half, 36 sin, 40 cos, 44 flags, 48 sprite_idx, 52 draw_type,
! 56 argb.
! r8 the run  r9 the spark  r10 sparks left  r11 where the next particle goes
! r12 room left  r13 the spark + 56  r14 the spark + 72 (all kept by the C
! functions it calls)

    .macro CTXF off, reg
    mov     #\off,r0
    fmov.s  @(r0,r8),\reg
    .endm
    .macro CTXL off, reg
    mov     #\off,r0
    mov.l   @(r0,r8),\reg
    .endm



! One axis of a free spark between two ticks: prev + (pos - prev) * rate.
    .macro INTERP cur, prev, dst
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

! One colour channel: min(255, byte / 255 * 255 * gain * blend_k) as an integer
! shifted by \shift into r3's accumulator (r14 is not free: r3 accumulates).
! The byte is at @(\off,r13); the gain is in fr9, the blend factor in fr10.
    .macro CHANNEL off, shift
    mov.b   @(\off,r13),r0
    extu.b  r0,r0
    lds     r0,fpul
    float   fpul,fr0
    CTXF    92,fr1
    fdiv    fr1,fr0
    fmul    fr1,fr0
    fmul    fr9,fr0
    fmul    fr10,fr0
    fcmp/gt fr1,fr0                 ! 255 < v: 255
    bf      1f
    fmov    fr1,fr0
1:  ftrc    fr0,fpul
    sts     fpul,r0
    .if \shift == 16
    shll16  r0
    .endif
    .if \shift == 8
    shll8   r0
    .endif
    or      r0,r3
    .endm

    .text
    .align 2
    .global _dc_stage_sparks
_dc_stage_sparks:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    add     #-64,r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov     #0,r0
    mov.l   r0,@(16,r8)
    mov.l   r0,@(20,r8)
    mov.l   r0,@(24,r8)
    mov.l   r0,@(28,r8)

sp_loop:
    tst     r10,r10
    bf      1f
    bra     sp_done
    nop
1:  mov.b   @r9,r0                      ! on
    tst     r0,r0
    bf      2f
    bra     sp_next
    nop
2:  tst     r12,r12                     ! the list is full
    bf      3f
    bra     sp_done
    nop
3:  mov     r9,r13
    add     #56,r13
    mov     r9,r14
    add     #72,r14
    mov.w   @(8,r14),r0
    extu.w  r0,r0
    mov.l   r0,@(44,r15)                ! flags
    mov     r0,r2
    and     #0xC0,r0                    ! SPARK_F_FX | SPARK_F_ITEM
    tst     r0,r0
    bf      sp_attached
    mov     r2,r0
    shlr8   r0
    tst     #0x10,r0                    ! SPARK_F_ATTACHED_NODE (0x1000)
    bf      sp_attached
    mov.b   @(1,r9),r0                  ! a free spark: between two ticks
    extu.b  r0,r1
    mov.b   @(2,r9),r0
    extu.b  r0,r0
    sub     r0,r1
    mov     #1,r0
    cmp/gt  r0,r1
    bt      sp_interp
    mov.l   @(4,r9),r0
    mov.l   r0,@(0,r15)
    mov.l   @(8,r9),r0
    mov.l   r0,@(4,r15)
    mov.l   @(12,r9),r0
    mov.l   r0,@(8,r15)
    bra     sp_pos
    nop
sp_interp:
    mov     #56,r0
    fmov.s  @(r0,r8),fr4
    INTERP  4, 16, 0
    INTERP  8, 20, 4
    INTERP  12, 24, 8
    bra     sp_pos
    nop
sp_attached:
    mov     r9,r4
    mov     r15,r5
    CTXL    76,r1
    jsr     @r1
    nop
sp_pos:
    mov.l   @(0,r15),r0
    lds     r0,fpul
    float   fpul,fr0
    mov.l   @(4,r15),r0
    lds     r0,fpul
    float   fpul,fr1
    mov.l   @(8,r15),r0
    lds     r0,fpul
    float   fpul,fr2
    mov     #32,r0
    fmov.s  @(r0,r8),fr3                ! zv = depth . pos + depth3
    fmul    fr0,fr3
    mov     #36,r0
    fmov.s  @(r0,r8),fr4
    fmul    fr1,fr4
    fadd    fr4,fr3
    mov     #40,r0
    fmov.s  @(r0,r8),fr4
    fmul    fr2,fr4
    fadd    fr4,fr3
    mov     #44,r0
    fmov.s  @(r0,r8),fr4
    fadd    fr4,fr3
    mov     #48,r0
    fmov.s  @(r0,r8),fr4                ! zv <= near: out
    fcmp/gt fr4,fr3
    bt      4f
    bra     sp_next
    nop
4:  mov     #52,r0
    fmov.s  @(r0,r8),fr5                ! zv >= far: out
    fcmp/gt fr3,fr5
    bt      5f
    bra     sp_next
    nop
5:  CTXF    108,fr4
    fmul    fr4,fr3
    ftrc    fr3,fpul
    sts     fpul,r1                     ! vpos_z
    tst     r1,r1
    bf      6f
    mov     #1,r1
6:  mov.l   r1,@(12,r15)
    lds     r1,fpul
    float   fpul,fr6                    ! vpos_z
    mov.b   @(0,r13),r0
    extu.b  r0,r4                       ! sw
    mov.b   @(1,r13),r0
    extu.b  r0,r5                       ! sh
    mov.l   @(44,r15),r0
    tst     #2,r0                       ! SPARK_F_SCALE
    bt      sp_noscale
    mov.b   @(0,r14),r0                 ! scalar
    extu.b  r0,r1
    mov     #1,r2
    shld    r1,r2                       ! 1 << scalar
    lds     r2,fpul
    float   fpul,fr7
    CTXF    96,fr8
    fmul    fr8,fr7                     ! 381 * (1 << scalar)
    fdiv    fr6,fr7                     ! / vpos_z: k
    lds     r4,fpul
    float   fpul,fr0
    fmul    fr7,fr0
    ftrc    fr0,fpul
    sts     fpul,r4
    lds     r5,fpul
    float   fpul,fr0
    fmul    fr7,fr0
    ftrc    fr0,fpul
    sts     fpul,r5
sp_noscale:
    mov.l   r4,@(16,r15)
    mov.l   r5,@(20,r15)
    mov     r4,r1                       ! half = (sw + sh) / 4 * vpos_z / 381
    add     r5,r1
    lds     r1,fpul
    float   fpul,fr0
    CTXF    100,fr1
    fdiv    fr1,fr0
    fmul    fr6,fr0
    CTXF    96,fr1
    fdiv    fr1,fr0
    fldi1   fr1
    fcmp/gt fr0,fr1                     ! 1 > half: too small
    bf      7f
    bra     sp_next
    nop
7:  mov.l   @(44,r15),r0
    mov.l   @(24,r14),r5                ! sprite_idx
    mov.l   @(32,r14),r6                ! draw_type
    mov     #0,r7                       ! the alpha is half
    tst     #8,r0                       ! SPARK_F_SPRITE
    bf      sp_sprite
    mov     #-1,r5                      ! a flat spark: untextured, blended
    mov     #2,r1
    cmp/eq  r1,r6                       ! DRAW_BLEND_ADD
    bt      8f
    mov     #9,r1
    cmp/eq  r1,r6                       ! DRAW_REFLECTIVE_BLEND_ADD
    bt      8f
    mov.l   @(36,r14),r1                ! context
    mov     #1,r2
    cmp/eq  r2,r1                       ! SPARK_CONTEXT_BLOOD
    bf      9f
    CTXL    64,r1
    tst     r1,r1                       ! the PlayStation's blending
    bt      9f
8:  mov     #1,r7
9:  mov     #1,r6                       ! DRAW_BLEND
    bra     sp_typed
    nop
sp_sprite:
    cmp/pz  r5
    bt      1f
    bra     sp_next
    nop
1:  CTXL    60,r1                       ! mesh_count
    cmp/ge  r1,r5
    bf      sp_typed
    bra     sp_next
    nop
sp_typed:
    cmp/pz  r5                          ! flat and half: added
    bt      1f
    tst     r7,r7
    bt      1f
    mov     #2,r6
1:  mov.l   r5,@(48,r15)
    mov.l   r7,@(32,r15)
    mov     #2,r1                       ! the particle's blend: 0 added, 1
    cmp/eq  r1,r6                       ! alpha, 2 subtracted
    bt      2f
    mov     #9,r1
    cmp/eq  r1,r6
    bt      2f
    mov     #3,r1
    cmp/eq  r1,r6
    bt      3f
    mov     #1,r2
    bra     4f
    nop
2:  mov     #0,r2
    bra     4f
    nop
3:  mov     #2,r2
4:  mov.l   r2,@(52,r15)
    mov.l   @(16,r15),r1                ! w = (sw / 2) * vpos_z / 381
    lds     r1,fpul
    float   fpul,fr0
    CTXF    104,fr2
    fdiv    fr2,fr0
    mov.l   @(12,r15),r1
    lds     r1,fpul
    float   fpul,fr6
    fmul    fr6,fr0
    CTXF    96,fr1
    fdiv    fr1,fr0
    mov     #24,r0
    fmov.s  fr0,@(r0,r15)
    mov.l   @(20,r15),r1                ! h
    lds     r1,fpul
    float   fpul,fr0
    fdiv    fr2,fr0
    fmul    fr6,fr0
    fdiv    fr1,fr0
    mov     #28,r0
    fmov.s  fr0,@(r0,r15)
    fldi1   fr9                         ! the colour's gain
    mov.l   @(48,r15),r1
    cmp/pz  r1
    bf      5f
    CTXF    88,fr9
5:  fldi1   fr10                        ! the blend factor
    mov.l   @(32,r15),r1
    tst     r1,r1
    bt      6f
    CTXF    112,fr10
6:  mov     #-1,r3
    shll16  r3
    shll8   r3                          ! 0xFF000000
    CHANNEL 10, 16
    CHANNEL 11, 8
    CHANNEL 12, 0
    mov.l   r3,@(56,r15)
    mov.l   @(44,r15),r0
    tst     #0x10,r0                    ! SPARK_F_ROTATE
    bt      sp_norot
    mov     r9,r4
    mov     r15,r5
    add     #36,r5
    CTXL    80,r1
    jsr     @r1
    nop
    bra     sp_uv
    nop
sp_norot:
    mov     #0,r0
    mov.l   r0,@(36,r15)
    fldi1   fr0
    mov     #40,r0
    fmov.s  fr0,@(r0,r15)
sp_uv:
    mov.l   @(48,r15),r5
    cmp/pz  r5
    bt      1f
    mov     #-1,r4                      ! a flat one: no picture
    bra     sp_write
    nop
1:  CTXL    72,r1
    mov.l   @r1,r2
    cmp/eq  r2,r5
    bt      2f
    mov     r5,r4
    CTXL    84,r1
    jsr     @r1
    nop
2:  CTXL    68,r1
    mov     #32,r0
    mov.b   @(r0,r1),r4                 ! the page
sp_write:
    mov     #72,r0
    add     r0,r11                      ! the particle, from its end
    mov.l   @(48,r15),r0
    cmp/pz  r0
    bf      sp_flatuv
    CTXL    68,r1
    fmov.s  @r1+,fr0                    ! u0..3, v0..3, written from the end
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fmov.s  @r1+,fr3
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    fmov.s  @r1+,fr7
    fmov.s  fr7,@-r11
    fmov.s  fr6,@-r11
    fmov.s  fr5,@-r11
    fmov.s  fr4,@-r11
    fmov.s  fr3,@-r11
    fmov.s  fr2,@-r11
    fmov.s  fr1,@-r11
    fmov.s  fr0,@-r11
    bra     sp_fields
    nop
sp_flatuv:
    fldi0   fr0
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr0,@-r11
sp_fields:
    mov.l   r4,@-r11                    ! page
    mov.l   @(56,r15),r0
    mov.l   r0,@-r11                    ! argb
    mov.l   @(52,r15),r0
    mov.l   r0,@-r11                    ! blend
    mov     #40,r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@-r11                   ! cs
    mov     #36,r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@-r11                   ! sn
    mov     #28,r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@-r11                   ! half_h
    mov     #24,r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@-r11                   ! half
    mov.l   @(8,r15),r0
    lds     r0,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11                   ! z
    mov.l   @(4,r15),r0
    lds     r0,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11                   ! y
    mov.l   @(0,r15),r0
    lds     r0,fpul
    float   fpul,fr0
    fmov.s  fr0,@-r11                   ! x
    mov     #72,r0
    add     r0,r11                      ! the next particle
sp_count:
    mov.l   @(52,r15),r1
    shll2   r1
    add     #20,r1
    add     r8,r1
    mov.l   @r1,r2
    add     #1,r2
    mov.l   r2,@r1
    mov.l   @(16,r8),r1
    add     #1,r1
    mov.l   r1,@(16,r8)
    add     #-1,r12
sp_next:
    mov     #112,r0                     ! sizeof(SPARK)
    add     r0,r9
    add     #-1,r10
    bra     sp_loop
    nop
sp_done:
    add     #64,r15
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
