! Hand-written SH-4 for the work the batcher does on a room once its vertices
! are placed (batcher.c): the coloured fog's share of each vertex.
!
!   void dc_fog_colours(FOGRUN *run);
!
! FOGRUN: 0 the vertices (M_XVERT, 32 bytes: x y z at 0, clip at 28)  4 the
! offset colours (one word each)  8 the ranges (start, count pairs)  12 how many
! ranges  16 fog_start squared  20 fog_start  24 fog_scale  28, 32, 36 the fog
! colour times the tint (r, g, b)  40 255.0
!
! A vertex marked fogged (clip bit 0x40) takes the colour of the fog scaled by
! how deep in it the vertex is (M_Fog in C: a squared distance by fipr, an
! fsrra, the share clamped to 0..1); any other, and one with no share, takes 0.

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
! One channel of the fog colour: min(1, max(0, c * fog)) * 255, shifted into r6.
    .macro FCH frc, shift
    fmov    \frc,fr5
    fmul    fr3,fr5
    fldi0   fr6
    fcmp/gt fr5,fr6
    bf      9f
    fmov    fr6,fr5
9:  fldi1   fr6
    fcmp/gt fr6,fr5
    bf      9f
    fmov    fr6,fr5
9:  fmul    fr11,fr5
    ftrc    fr5,fpul
    sts     fpul,r0
    .if \shift == 16
    shll16  r0
    .endif
    .if \shift == 8
    shll8   r0
    .endif
    or      r0,r6
    .endm

    .text
    .align 2
    .global _dc_fog_colours
_dc_fog_colours:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r7
    mov     r8,r6
    add     #16,r6
    fmov.s  @r6+,fr8                    ! fog_start squared
    fmov.s  @r6+,fr9                    ! fog_start
    fmov.s  @r6+,fr10                   ! fog_scale
    fmov.s  @r6+,fr12                   ! r
    fmov.s  @r6+,fr13                   ! g
    fmov.s  @r6+,fr14                   ! b
    fmov.s  @r6+,fr11                   ! 255
fc_range:
    tst     r7,r7
    bf      1f
    bra     fc_done
    nop
1:  mov.l   @r11+,r1                    ! the range's start
    mov.l   @r11+,r2                    ! how many
    add     #-1,r7
    mov     r1,r4
    shll2   r4
    shll2   r4
    shll    r4
    add     r9,r4                       ! its first vertex
    mov     r1,r5
    shll2   r5
    add     r10,r5                      ! its first offset colour
fc_loop:
    tst     r2,r2
    bf      1f
    bra     fc_range
    nop
1:  mov     #28,r0
    mov.b   @(r0,r4),r0
    tst     #0x40,r0
    JT      fc_zero_t                   ! no fog on it: cleared
    mov     r4,r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi0   fr3
    fipr    fv0,fv0                     ! the squared distance, in fr3
    fcmp/gt fr8,fr3
    JF      fc_zero_t                   ! not past where the fog starts
    fmov    fr3,fr4
    fsrra   fr4
    fmul    fr4,fr3
    fsub    fr9,fr3
    fmul    fr10,fr3                    ! the share
    fldi0   fr5
    fcmp/gt fr3,fr5
    bf      1f
    fmov    fr5,fr3
1:  fldi1   fr5
    fcmp/gt fr5,fr3
    bf      1f
    fmov    fr5,fr3
1:  fldi0   fr5
    fcmp/gt fr5,fr3
    JF      fc_zero_t                   ! no share
    mov     #-1,r6
    shll16  r6
    shll8   r6                          ! alpha 255
    FCH     fr12, 16
    FCH     fr13, 8
    FCH     fr14, 0
    bra     fc_store
    nop
fc_zero_t:
    mov     #0,r6
fc_store:
    tst     r10,r10                     ! no oargb: a native batch's, into
    bf      2f                          ! the vertex (emit_sh4.S DC_EMIT_MAP)
    mov     #28,r0
    mov.b   @(r0,r4),r0
    and     #0x7F,r0                    ! its clip
    tst     r6,r6
    bt      3f
    or      #0x80,r0                    ! fogged
    shll8   r6                          ! the colour above the clip byte
    extu.b  r0,r0
    or      r6,r0
3:  mov.l   r0,@(28,r4)
    bra     4f
    nop
2:  mov.l   r6,@r5
4:  add     #32,r4
    add     #4,r5
    add     #-1,r2
    bra     fc_loop
    nop
fc_done:
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    rts
    nop

! ---- the portals' windows (rooms/draw.c, M_GetBounds and M_SetBounds)
!
!   void dc_get_bounds(BOUNDRUN *run);
!
! Walks the queue of rooms the camera sees through the portals, narrowing the
! window of each by the portals that face the camera. Only the low 32 bits of
! the matrix products matter (the C code stores them in int32), so the words of
! the 64-bit matrix entries are used. BOUNDRUN (words):
!    0 the rooms  4 sizeof(ROOM)  8 the room binds  12 sizeof(bind)  16 the
!   queue (128 words)  20 start  24 end  28 outside  32 left  36 top  40 right
!   44 bottom of the outside window  48 the matrix g_MatrixPtr  52 the view
!   matrix  56 the view position  60 far_z  64 g_PhdPersp  68 centre x  72
!   centre y  76 g_PhdLeft  80 right  84 top  88 bottom  92 min x  96 max x
!   100 min y  104 max y  108 the C function that marks a room to be drawn
!   112 the offset of ROOM.pos  116 of .portals  120 of .flags.outside  124 of
!   .flags.inside
!
! OUTPUT_ROOM_BIND: 0 active, 1 drawn, 4 bound left, 6 right, 8 top, 10 bottom,
! 12 test left, 14 right, 16 top, 18 bottom.   PORTAL: 0 room_num, 2 normal,
! 8 the four vertices (6 bytes each).
!
! The frame: 0, 4, 8 the room's translation  12.. the four vertices' xv yv zv
! (12 bytes each)  60 left  64 right  68 top  72 bottom  76 too near  80 too far
! 84 the portal's room bind  88 the room number  92 portals left  96 the portal
! r8 the run  r9 the room  r10 its bind  r11 the portal  r14 the matrix

    .macro CTXL2 off, reg
    mov     #\off,r0
    mov.l   @(r0,r8),\reg
    .endm

! r4 / r5 as signed 32-bit numbers, the quotient truncated towards zero, in r0.
! The estimate is a single-precision division, brought exact in integers.
! Clobbers r1-r6, fr0, fr1.
sdiv32:
    mov     r4,r6
    xor     r5,r6                       ! the sign of the result
    cmp/pz  r4
    bt      1f
    neg     r4,r4
1:  cmp/pz  r5
    bt      2f
    neg     r5,r5
2:  lds     r4,fpul
    float   fpul,fr0
    lds     r5,fpul
    float   fpul,fr1
    fdiv    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r0                     ! q
    mov     r0,r1
    mul.l   r1,r5
    sts     macl,r2
    sub     r2,r4                       ! r = a - q * b
    lds     r4,fpul
    float   fpul,fr0
    fdiv    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r1                     ! the rest of the quotient
    add     r1,r0
    mul.l   r1,r5
    sts     macl,r2
    sub     r2,r4
3:  cmp/pz  r4
    bt      4f
    add     #-1,r0
    add     r5,r4
    bra     3b
    nop
4:  cmp/ge  r5,r4
    bf      5f
    add     #1,r0
    sub     r5,r4
    bra     4b
    nop
5:  cmp/pz  r6
    bt      6f
    neg     r0,r0
6:  rts
    nop

    .macro TROW base, off, slot
    mov.l   @(\off,\base),r6
    mul.l   r6,r1
    sts     macl,r7
    mov.l   @(\off+8,\base),r6
    mul.l   r6,r2
    sts     macl,r6
    add     r6,r7
    mov.l   @(\off+16,\base),r6
    mul.l   r6,r3
    sts     macl,r6
    add     r6,r7
    mov.l   @(\off+24,\base),r6
    add     r6,r7
    mov.l   r7,@(\slot,r15)
    .endm
! One coordinate of a portal vertex: row . (x, y, z) + the room's translation,
! low word; x y z are in r1 r2 r3, the row's matrix base in \base.
    .macro VROW base, off, slot, t
    mov.l   @(\off,\base),r6
    mul.l   r6,r1
    sts     macl,r7
    mov.l   @(\off+8,\base),r6
    mul.l   r6,r2
    sts     macl,r6
    add     r6,r7
    mov.l   @(\off+16,\base),r6
    mul.l   r6,r3
    sts     macl,r6
    add     r6,r7
    mov.l   @(\t,r15),r6
    add     r6,r7
    mov.l   r7,@(\slot,r15)
    .endm

    .global _dc_get_bounds
_dc_get_bounds:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    add     #-128,r15
    mov     r15,r12
    add     #64,r12
    mov     r4,r8
    mov.l   @(48,r8),r14                ! the matrix
    mov     r14,r13
    add     #64,r13
gb_loop:
    mov.l   @(20,r8),r1
    mov.l   @(24,r8),r2
    cmp/eq  r1,r2
    bf      1f
    bra     gb_done
    nop
1:  mov     r1,r3                       ! room_num = queue[start % 128]
    mov     #127,r0
    and     r0,r3
    shll2   r3
    mov.l   @(16,r8),r4
    add     r4,r3
    mov.l   @r3,r3
    mov.l   r3,@(24,r12)
    add     #1,r1
    mov.l   r1,@(20,r8)
    mov.l   @(0,r8),r9                  ! room = rooms + room_num * stride
    mov.l   @(4,r8),r4
    mul.l   r3,r4
    sts     macl,r4
    add     r4,r9
    mov.l   @(8,r8),r10                 ! bind
    mov.l   @(12,r8),r4
    mul.l   r3,r4
    sts     macl,r4
    add     r4,r10
    mov     #0,r0
    mov.b   r0,@r10                     ! active = false
    mov.w   @(4,r10),r0                 ! bound left = min(bound left, test left)
    mov     r0,r1
    mov.w   @(12,r10),r0
    cmp/gt  r0,r1
    bf      1f
    mov.w   r0,@(4,r10)
1:  mov.w   @(8,r10),r0                 ! top
    mov     r0,r1
    mov.w   @(16,r10),r0
    cmp/gt  r0,r1
    bf      1f
    mov.w   r0,@(8,r10)
1:  mov.w   @(6,r10),r0                 ! right = max
    mov     r0,r1
    mov.w   @(14,r10),r0
    cmp/gt  r1,r0
    bf      1f
    mov.w   r0,@(6,r10)
1:  mov.w   @(10,r10),r0                ! bottom
    mov     r0,r1
    mov.w   @(18,r10),r0
    cmp/gt  r1,r0
    bf      1f
    mov.w   r0,@(10,r10)
1:  CTXL2   120,r2                      ! the offset of flags.outside
    mov     r2,r0
    mov.b   @(r0,r9),r0
    mov.l   r0,@(52,r12)                ! outside
    mov.b   @(1,r10),r0                 ! drawn
    tst     r0,r0
    bf      gb_drawn
    mov.l   @(24,r12),r4                ! not yet: marked to be drawn
    CTXL2   108,r1
    jsr     @r1
    nop
    mov     #1,r0
    mov.b   r0,@(1,r10)
    mov.l   @(52,r12),r0
    tst     r0,r0
    bt      gb_drawn
    mov     #1,r1
    mov.l   r1,@(28,r8)
gb_drawn:
    CTXL2   124,r2                      ! flags.inside
    mov     r2,r0
    mov.b   @(r0,r9),r0
    mov     r0,r1
    mov.l   @(52,r12),r0
    tst     r0,r0
    bf      gb_out                      ! outside
    tst     r1,r1
    bf      gb_noout                    ! inside and not outside
gb_out:
    mov.w   @(4,r10),r0                 ! left
    mov.l   @(32,r8),r1
    cmp/gt  r0,r1
    bf      1f
    mov.l   r0,@(32,r8)
1:  mov.w   @(8,r10),r0                 ! top
    mov.l   @(36,r8),r1
    cmp/gt  r0,r1
    bf      1f
    mov.l   r0,@(36,r8)
1:  mov.w   @(6,r10),r0                 ! right
    mov.l   @(40,r8),r1
    cmp/gt  r1,r0
    bf      1f
    mov.l   r0,@(40,r8)
1:  mov.w   @(10,r10),r0                ! bottom
    mov.l   @(44,r8),r1
    cmp/gt  r1,r0
    bf      1f
    mov.l   r0,@(44,r8)
1:
gb_noout:
    CTXL2   116,r2                      ! the portals
    mov     r2,r0
    mov.l   @(r0,r9),r11
    tst     r11,r11
    bf      1f
    bra     gb_loop
    nop
1:  mov.w   @r11,r0                     ! count
    extu.w  r0,r0
    mov.l   r0,@(48,r12)
    add     #2,r11                      ! the first portal
    tst     r0,r0
    bf      1f
    bra     gb_loop
    nop
1:  CTXL2   112,r2                      ! the room's translation
    mov     r2,r0
    add     r9,r0
    mov.l   @(0,r0),r1
    mov.l   @(4,r0),r2
    mov.l   @(8,r0),r3
    mov.l   @(52,r8),r4
    mov     r4,r5
    add     #64,r5
    TROW    r4, 0, 0
    TROW    r4, 32, 4
    TROW    r5, 0, 8
gb_portal:
    ! does the portal face the camera: normal . (room + vertex 0 - eye) < 0
    mov.l   @(56,r8),r7                 ! the eye
    mov     r11,r5
    add     #2,r5                       ! the normal
    mov     r11,r4
    add     #8,r4                       ! vertex 0
    CTXL2   112,r2
    mov     r2,r3
    add     r9,r3                       ! &room.pos
    mov.w   @r5+,r0
    mov     r0,r1                       ! nx
    mov.w   @r4+,r0
    mov.l   @(0,r3),r2
    add     r0,r2
    mov.l   @(0,r7),r0
    sub     r0,r2
    mul.l   r1,r2
    sts     macl,r6
    mov.w   @r5+,r0
    mov     r0,r1
    mov.w   @r4+,r0
    mov.l   @(4,r3),r2
    add     r0,r2
    mov.l   @(4,r7),r0
    sub     r0,r2
    mul.l   r1,r2
    sts     macl,r2
    add     r2,r6
    mov.w   @r5+,r0
    mov     r0,r1
    mov.w   @r4+,r0
    mov.l   @(8,r3),r2
    add     r0,r2
    mov.l   @(8,r7),r0
    sub     r0,r2
    mul.l   r1,r2
    sts     macl,r2
    add     r2,r6
    cmp/pz  r6
    bf      1f
    bra     gb_next
    nop
1:  bsr     sb_window
    nop
gb_next:
    mov     #56,r0
    add     r0,r11                      ! sizeof(PORTAL)
    mov.l   @(48,r12),r1
    add     #-1,r1
    mov.l   r1,@(48,r12)
    tst     r1,r1
    bt      1f
    bra     gb_portal
    nop
1:  bra     gb_loop
    nop
gb_done:
    add     #127,r15                    ! (an immediate holds -128..127)
    add     #1,r15
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

! The window of one portal (r11) of the room r9 (bind r10), the vertices
! placed by the matrix and the room's translation; narrows the bind of the
! room it leads to, or queues it. Returns with nothing to say.
    .macro WUPD
    mov.l   @(36,r12),r1               ! xs
    mov.l   @(60,r15),r2
    add     #-1,r1
    cmp/gt  r1,r2
    bf      1f
    mov.l   r1,@(60,r15)
1:  mov.l   @(36,r12),r1
    mov.l   @(0,r12),r2
    add     #1,r1
    cmp/gt  r2,r1
    bf      1f
    mov.l   r1,@(0,r12)
1:  mov.l   @(40,r12),r1               ! ys
    mov.l   @(4,r12),r2
    add     #-1,r1
    cmp/gt  r1,r2
    bf      1f
    mov.l   r1,@(4,r12)
1:  mov.l   @(40,r12),r1
    mov.l   @(8,r12),r2
    add     #1,r1
    cmp/gt  r2,r1
    bf      1f
    mov.l   r1,@(8,r12)
1:
    .endm
    .macro VERT i
    mov.w   @(8+6*\i,r11),r0
    mov     r0,r1
    mov.w   @(10+6*\i,r11),r0
    mov     r0,r2
    mov.w   @(12+6*\i,r11),r0
    mov     r0,r3
    VROW    r14, 0, 12+12*\i, 0
    VROW    r14, 32, 16+12*\i, 4
    VROW    r13, 0, 20+12*\i, 8
    mov.l   @(20+12*\i,r15),r1          ! zv
    mov.l   @(60,r8),r2
    cmp/ge  r2,r1
    bf      1f
    mov.l   @(16,r12),r2
    add     #1,r2
    mov.l   r2,@(16,r12)
1:  cmp/pl  r1
    bt      2f
    mov.l   @(12,r12),r2
    add     #1,r2
    mov.l   r2,@(12,r12)
    bra     9f
    nop
2:  mov     r1,r4                       ! zp = zv / persp
    mov     #64,r0
    mov.l   @(r0,r8),r5
    bsr     sdiv32
    nop
    mov     r0,r7
    tst     r7,r7
    bt      3f
    mov.l   @(12+12*\i,r15),r4
    mov     r7,r5
    bsr     sdiv32
    nop
    mov     r0,r2
    mov     #68,r0
    mov.l   @(r0,r8),r1
    add     r2,r1
    mov.l   r1,@(36,r12)
    mov.l   @(16+12*\i,r15),r4
    mov     r7,r5
    bsr     sdiv32
    nop
    mov     r0,r2
    mov     #72,r0
    mov.l   @(r0,r8),r1
    add     r2,r1
    mov.l   r1,@(40,r12)
    bra     4f
    nop
3:  mov.l   @(12+12*\i,r15),r1          ! right on the eye: the window's side
    mov     #76,r0
    mov.l   @(r0,r8),r2
    cmp/pz  r1
    bf      5f
    mov     #80,r0
    mov.l   @(r0,r8),r2
5:  mov.l   r2,@(36,r12)
    mov.l   @(16+12*\i,r15),r1
    mov     #84,r0
    mov.l   @(r0,r8),r2
    cmp/pz  r1
    bf      5f
    mov     #88,r0
    mov.l   @(r0,r8),r2
5:  mov.l   r2,@(40,r12)
4:  WUPD
9:
    .endm
! Where a portal's edge crosses the near plane: the sides it opens to.
    .macro EDGE d, l
    mov.l   @(20+12*\d,r15),r1
    mov.l   @(20+12*\l,r15),r2
    cmp/pl  r1
    movt    r3
    cmp/pl  r2
    movt    r4
    cmp/eq  r3,r4
    bf      1f
    bra     9f
    nop
1:  mov.l   @(12+12*\d,r15),r1          ! x
    mov.l   @(12+12*\l,r15),r2
    cmp/pz  r1
    bt      2f
    cmp/pz  r2
    bt      2f
    mov     #92,r0
    mov.l   @(r0,r8),r5                 ! both left of the eye: left = min x
    mov.l   r5,@(60,r15)
    bra     6f
    nop
2:  cmp/pl  r1
    bf      3f
    cmp/pl  r2
    bf      3f
    mov     #96,r0
    mov.l   @(r0,r8),r5                 ! both right: right = max x
    mov.l   r5,@(0,r12)
    bra     6f
    nop
3:
    mov     #92,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(60,r15)
    mov     #96,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(0,r12)
6:  mov.l   @(16+12*\d,r15),r1          ! y
    mov.l   @(16+12*\l,r15),r2
    cmp/pz  r1
    bt      2f
    cmp/pz  r2
    bt      2f
    mov     #100,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(4,r12)
    bra     9f
    nop
2:  cmp/pl  r1
    bf      3f
    cmp/pl  r2
    bf      3f
    mov     #104,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(8,r12)
    bra     9f
    nop
3:
    mov     #100,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(4,r12)
    mov     #104,r0
    mov.l   @(r0,r8),r5
    mov.l   r5,@(8,r12)
9:
    .endm

sb_window:
    sts     pr,r0
    mov.l   r0,@(44,r12)
    mov.w   @r11,r0                     ! the room it leads to
    mov     r0,r1
    mov.l   r1,@(28,r12)
    mov.l   @(12,r8),r2
    mul.l   r1,r2
    sts     macl,r2
    mov.l   @(8,r8),r3
    add     r2,r3                       ! its bind
    mov.l   r3,@(20,r12)
    mov.w   @(4,r3),r0                  ! already as wide as the parent's window?
    mov     r0,r4
    mov.w   @(12,r10),r0
    cmp/gt  r0,r4
    bt      sb_go
    mov.w   @(8,r3),r0
    mov     r0,r4
    mov.w   @(16,r10),r0
    cmp/gt  r0,r4
    bt      sb_go
    mov.w   @(6,r3),r0
    mov     r0,r4
    mov.w   @(14,r10),r0
    cmp/gt  r4,r0
    bt      sb_go
    mov.w   @(10,r3),r0
    mov     r0,r4
    mov.w   @(18,r10),r0
    cmp/gt  r4,r0
    bt      sb_go
    mov.l   @(44,r12),r0
    lds     r0,pr
    rts
    nop
sb_go:
    mov.w   @(14,r10),r0                ! left = parent right ... (inverted start)
    mov.l   r0,@(60,r15)
    mov.w   @(12,r10),r0
    mov.l   r0,@(0,r12)
    mov.w   @(18,r10),r0
    mov.l   r0,@(4,r12)
    mov.w   @(16,r10),r0
    mov.l   r0,@(8,r12)
    mov     #0,r1
    mov.l   r1,@(12,r12)
    mov.l   r1,@(16,r12)
    VERT    0
    VERT    1
    VERT    2
    VERT    3
    mov.l   @(12,r12),r1
    mov     #4,r2
    cmp/eq  r2,r1
    bf      1f
    bra     sb_ret
    nop
1:  mov.l   @(16,r12),r1
    cmp/eq  r2,r1
    bf      1f
    bra     sb_ret
    nop
1:  mov.l   @(12,r12),r1
    cmp/pl  r1
    JF      sb_noclip
    EDGE    0, 3
    EDGE    1, 0
    EDGE    2, 1
    EDGE    3, 2
sb_noclip:
    mov.w   @(12,r10),r0                ! kept inside the parent's window
    mov.l   @(60,r15),r1
    cmp/ge  r0,r1
    bt      1f
    mov.l   r0,@(60,r15)
1:  mov.w   @(14,r10),r0
    mov.l   @(0,r12),r1
    cmp/gt  r0,r1
    bf      1f
    mov.l   r0,@(0,r12)
1:  mov.w   @(16,r10),r0
    mov.l   @(4,r12),r1
    cmp/ge  r0,r1
    bt      1f
    mov.l   r0,@(4,r12)
1:  mov.w   @(18,r10),r0
    mov.l   @(8,r12),r1
    cmp/gt  r0,r1
    bf      1f
    mov.l   r0,@(8,r12)
1:  mov.l   @(60,r15),r1
    mov.l   @(0,r12),r2
    cmp/ge  r2,r1
    bf      1f
    bra     sb_ret
    nop
1:  mov.l   @(4,r12),r1
    mov.l   @(8,r12),r2
    cmp/ge  r2,r1
    bf      1f
    bra     sb_ret
    nop
1:  mov.l   @(20,r12),r3                ! the bind
    mov.b   @r3,r0
    tst     r0,r0
    bt      sb_queue
    mov.w   @(12,r3),r0                 ! active: the window grows
    mov.l   @(60,r15),r1
    cmp/gt  r1,r0
    bf      1f
    mov     r1,r0
1:  mov.w   r0,@(12,r3)
    mov.w   @(16,r3),r0
    mov.l   @(4,r12),r1
    cmp/gt  r1,r0
    bf      1f
    mov     r1,r0
1:  mov.w   r0,@(16,r3)
    mov.w   @(14,r3),r0
    mov.l   @(0,r12),r1
    cmp/ge  r1,r0
    bt      1f
    mov     r1,r0
1:  mov.w   r0,@(14,r3)
    mov.w   @(18,r3),r0
    mov.l   @(8,r12),r1
    cmp/ge  r1,r0
    bt      1f
    mov     r1,r0
1:  mov.w   r0,@(18,r3)
    bra     sb_ret
    nop
sb_queue:
    mov.l   @(24,r8),r1                 ! queue[end % 128] = room
    mov     r1,r2
    mov     #127,r0
    and     r0,r2
    shll2   r2
    mov.l   @(16,r8),r4
    add     r4,r2
    mov.l   @(28,r12),r5
    mov.l   r5,@r2
    add     #1,r1
    mov.l   r1,@(24,r8)
    mov     #1,r0
    mov.b   r0,@r3
    mov.l   @(60,r15),r0
    mov.w   r0,@(12,r3)
    mov.l   @(4,r12),r0
    mov.w   r0,@(16,r3)
    mov.l   @(0,r12),r0
    mov.w   r0,@(14,r3)
    mov.l   @(8,r12),r0
    mov.w   r0,@(18,r3)
sb_ret:
    mov.l   @(44,r12),r0
    lds     r0,pr
    rts
    nop

! ---- the walls between an eye and a point (batcher.c, M_SightBlocked)
!
!   int dc_sight_blocked(SIGHT *s);
!
! The ray from the eye to the point, walked sector by sector across the room's
! grid, stops at the first wall solid from floor to ceiling, or at the first
! block whose floor lies wholly below the eye's path. SIGHT (words):
!    0 sectors across x  4 across z  8 the sectors  12 sizeof(SECTOR)
!   16 the offset of floor.height  20 of portal_room.wall  24 of portal_room.pit
!   28 of floor.is_split (a byte)  32 of floor.tilt.x  36 of floor.tilt.z (words
!   of 16 bits)  40 room.pos.y  44 see_through (bytes)  48 wall_top (floats)
!   52 eye x  56 eye y  60 eye z  64 y_lo  68 y_hi  72 to x  76 to y  80 to z
!   (floats)  84, 88, 92, 96 the footprint x0 z0 x1 z1 (words)
!   100 1/1024  104 FLT_MAX  108 256.0  112 384.0  116 -3251200 (NO_HEIGHT)
!   120 the steps at most
! Returns 1 when the ray is stopped.
!
! r8 the block  r9 ix  r10 iz  r11 ex  r12 ez  r13 steps left  r14 the sector
! The frame: 0 sx  4 sz  8 the sector's index  12 y_in (float)  16 y_out
! fr8 next x  fr9 next z  fr10 step x  fr11 step z  fr12 dy  fr13 eye y
! fr14 1.0  fr15 (kept)

! r1 = floor(fr) for the float in \f (clobbers fr7, fpul).
    .macro FLOORI f
    ftrc    \f,fpul
    sts     fpul,r1
    lds     r1,fpul
    float   fpul,fr0
    fcmp/gt \f,fr0                      ! (float)trunc > f: it was negative
    bf      9f
    add     #-1,r1
9:
    .endm

    .global _dc_sight_blocked
_dc_sight_blocked:
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
    add     #-24,r15
    mov     r4,r8
    mov     #100,r0
    fmov.s  @(r0,r8),fr15               ! inv = 1/1024
    mov     #52,r0
    fmov.s  @(r0,r8),fr0                ! eye x
    fmul    fr15,fr0                    ! x0
    mov     #72,r0
    fmov.s  @(r0,r8),fr1                ! to x
    fmul    fr15,fr1                    ! to x * inv
    fmov    fr1,fr2
    fsub    fr0,fr2                     ! dx
    mov     #60,r0
    fmov.s  @(r0,r8),fr3                ! eye z
    fmul    fr15,fr3                    ! z0
    mov     #80,r0
    fmov.s  @(r0,r8),fr4
    fmul    fr15,fr4                    ! to z * inv
    fmov    fr4,fr5
    fsub    fr3,fr5                     ! dz
    mov     #76,r0
    fmov.s  @(r0,r8),fr12               ! to y
    mov     #56,r0
    fmov.s  @(r0,r8),fr13               ! eye y
    fsub    fr13,fr12                   ! dy
    FLOORI  fr1
    mov     r1,r11                      ! ex
    FLOORI  fr4
    mov     r1,r12                      ! ez
    FLOORI  fr0
    mov     r1,r9                       ! ix
    FLOORI  fr3
    mov     r1,r10                      ! iz
    fldi1   fr14
    mov     #104,r0
    fmov.s  @(r0,r8),fr1                ! FLT_MAX
    fldi0   fr4
    ! x axis: sx, step_x, next_x
    fcmp/gt fr4,fr2                     ! dx > 0
    bf      sbx_nopos
    mov     #1,r1
    mov.l   r1,@(0,r15)
    fmov    fr14,fr10
    fdiv    fr2,fr10                    ! 1 / |dx|
    lds     r9,fpul
    float   fpul,fr8
    fadd    fr14,fr8
    fsub    fr0,fr8
    fmul    fr10,fr8
    bra     sbx_done
    nop
sbx_nopos:
    mov     #-1,r1
    mov.l   r1,@(0,r15)
    fcmp/gt fr2,fr4                     ! dx < 0
    bf      sbx_zero
    fmov    fr2,fr10
    fneg    fr10
    fmov    fr14,fr6
    fdiv    fr10,fr6
    fmov    fr6,fr10                    ! 1 / |dx|
    lds     r9,fpul
    float   fpul,fr8
    fsub    fr8,fr0                     ! x0 - ix
    fmov    fr0,fr8
    fmul    fr10,fr8
    bra     sbx_done
    nop
sbx_zero:
    fmov    fr1,fr10
    fmov    fr1,fr8
sbx_done:
    ! z axis
    fcmp/gt fr4,fr5
    bf      sbz_nopos
    mov     #1,r1
    mov.l   r1,@(4,r15)
    fmov    fr14,fr11
    fdiv    fr5,fr11
    lds     r10,fpul
    float   fpul,fr9
    fadd    fr14,fr9
    fsub    fr3,fr9
    fmul    fr11,fr9
    bra     sbz_done
    nop
sbz_nopos:
    mov     #-1,r1
    mov.l   r1,@(4,r15)
    fcmp/gt fr5,fr4
    bf      sbz_zero
    fmov    fr5,fr11
    fneg    fr11
    fmov    fr14,fr6
    fdiv    fr11,fr6
    fmov    fr6,fr11
    lds     r10,fpul
    float   fpul,fr9
    fsub    fr9,fr3
    fmov    fr3,fr9
    fmul    fr11,fr9
    bra     sbz_done
    nop
sbz_zero:
    fmov    fr1,fr11
    fmov    fr1,fr9
sbz_done:
    mov     #120,r0
    mov.l   @(r0,r8),r13
sb_step:
    tst     r13,r13
    bf      1f
    bra     sb_free
    nop
1:  add     #-1,r13
    cmp/eq  r11,r9
    bf      1f
    cmp/eq  r12,r10
    bf      1f
    bra     sb_free
    nop
1:  fmov    fr8,fr0                     ! enter = min(next x, next z)
    fcmp/gt fr9,fr0
    bf      1f
    fmov    fr9,fr0
1:  fcmp/gt fr0,fr14                    ! 1 > enter
    bt      1f
    bra     sb_free
    nop
1:  fmov    fr0,fr6                     ! enter, kept
    fcmp/gt fr8,fr9                     ! next x < next z
    bf      sb_stepz
    mov.l   @(0,r15),r1
    add     r1,r9
    fadd    fr10,fr8
    bra     sb_stepd
    nop
sb_stepz:
    mov.l   @(4,r15),r1
    add     r1,r10
    fadd    fr11,fr9
sb_stepd:
    mov     #84,r0                      ! inside the footprint, or outside the room
    mov.l   @(r0,r8),r1
    cmp/ge  r1,r9
    bf      sb_chk
    mov     #92,r0
    mov.l   @(r0,r8),r1
    cmp/gt  r1,r9
    bt      sb_chk
    mov     #88,r0
    mov.l   @(r0,r8),r1
    cmp/ge  r1,r10
    bf      sb_chk
    mov     #96,r0
    mov.l   @(r0,r8),r1
    cmp/gt  r1,r10
    bt      sb_chk
    bra     sb_step
    nop
sb_chk:
    cmp/pz  r9
    bt      1f
    bra     sb_step
    nop
1:  cmp/pz  r10
    bt      1f
    bra     sb_step
    nop
1:  mov.l   @(0,r8),r1
    cmp/ge  r1,r9
    bf      1f
    bra     sb_step
    nop
1:  mov.l   @(4,r8),r2
    cmp/ge  r2,r10
    bf      1f
    bra     sb_step
    nop
1:  mul.l   r9,r2                       ! at = iz + ix * size z
    sts     macl,r2
    add     r10,r2
    mov.l   r2,@(8,r15)
    mov.l   @(12,r8),r3
    mul.l   r2,r3
    sts     macl,r3
    mov.l   @(8,r8),r14
    add     r3,r14                      ! the sector
    ! y_in = eye y + dy * enter ; y_out = eye y + dy * min(next x, next z)
    fmov    fr12,fr0
    fmul    fr6,fr0
    fadd    fr13,fr0
    mov     #12,r0
    fmov.s  fr0,@(r0,r15)
    fmov    fr8,fr0
    fcmp/gt fr9,fr0
    bf      1f
    fmov    fr9,fr0
1:  fmul    fr12,fr0
    fadd    fr13,fr0
    mov     #16,r0
    fmov.s  fr0,@(r0,r15)
    mov     #16,r0
    mov.l   @(r0,r8),r0                 ! offset of floor.height
    mov.l   @(r0,r14),r1
    mov     #116,r0
    mov.l   @(r0,r8),r2                 ! NO_HEIGHT
    cmp/eq  r2,r1
    bt      sb_wall
    mov.l   @(8,r15),r2                 ! see_through & 2: passes
    mov     #44,r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.b   @r3,r0
    tst     #2,r0
    bt      1f
    bra     sb_step
    nop
1:  mov     #24,r0
    mov.l   @(r0,r8),r0                 ! portal_room.pit
    mov.w   @(r0,r14),r0
    cmp/eq  #-1,r0
    bt      1f
    bra     sb_step
    nop
1:  mov     #28,r0
    mov.l   @(r0,r8),r0                 ! floor.is_split
    mov.b   @(r0,r14),r0
    tst     r0,r0
    bt      1f
    bra     sb_step
    nop
1:  mov     #40,r0
    mov.l   @(r0,r8),r2                 ! room.pos.y
    sub     r2,r1                       ! floor height - pos.y
    lds     r1,fpul
    float   fpul,fr0
    mov     #32,r0
    mov.l   @(r0,r8),r0
    mov.w   @(r0,r14),r0                ! tilt.x
    mov     r0,r3
    cmp/pz  r3
    bt      1f
    neg     r3,r3
1:  mov     #36,r0
    mov.l   @(r0,r8),r0
    mov.w   @(r0,r14),r0                ! tilt.z
    cmp/pz  r0
    bt      1f
    neg     r0,r0
1:  add     r0,r3
    lds     r3,fpul
    float   fpul,fr1
    mov     #108,r0
    fmov.s  @(r0,r8),fr2
    fmul    fr2,fr1
    fadd    fr1,fr0                     ! lowest
    mov     #12,r0
    fmov.s  @(r0,r15),fr1
    fcmp/gt fr0,fr1                     ! y_in > lowest
    bt      1f
    bra     sb_step
    nop
1:  mov     #16,r0
    fmov.s  @(r0,r15),fr1
    fcmp/gt fr0,fr1                     ! y_out > lowest
    bt      1f
    bra     sb_step
    nop
1:  bra     sb_hit
    nop
sb_wall:
    ! a wall: from the highest point of its faces down to the floor
    mov     #20,r0
    mov.l   @(r0,r8),r0                 ! portal_room.wall
    mov.w   @(r0,r14),r0
    cmp/eq  #-1,r0
    bt      1f
    bra     sb_step
    nop
1:  mov.l   @(8,r15),r2
    mov     #44,r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.b   @r3,r0
    tst     #1,r0
    bt      1f
    bra     sb_step
    nop
1:  shll2   r2                          ! top = max(y_lo, wall_top[at] + 384)
    mov     #48,r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    fmov.s  @r3,fr0
    mov     #112,r0
    fmov.s  @(r0,r8),fr1
    fadd    fr1,fr0
    mov     #64,r0
    fmov.s  @(r0,r8),fr1                ! y_lo
    fcmp/gt fr1,fr0
    bt      1f
    fmov    fr1,fr0
1:  mov     #68,r0
    fmov.s  @(r0,r8),fr2                ! y_hi
    mov     #12,r0
    fmov.s  @(r0,r15),fr3               ! y_in
    fcmp/gt fr3,fr0                     ! top > y_in: above the wall
    bf      1f
    bra     sb_step
    nop
1:  fcmp/gt fr2,fr3
    bf      1f
    bra     sb_step
    nop
1:  mov     #16,r0
    fmov.s  @(r0,r15),fr3               ! y_out
    fcmp/gt fr3,fr0
    bf      1f
    bra     sb_step
    nop
1:  fcmp/gt fr2,fr3
    bf      1f
    bra     sb_step
    nop
1:
sb_hit:
    mov     #1,r0
    bra     sb_out
    nop
sb_free:
    mov     #0,r0
sb_out:
    add     #24,r15
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
    mov.l   @r15+,r8
    rts
    nop

! ---- the walls hiding a chunk (batcher.c, M_ChunkHidden)
!
!   int dc_chunk_hidden(M_SIGHTRUN *run, const M_CHUNK *chunk);
!
! The shared fields of run (sizes, sectors, offsets, eye, y_lo, y_hi, the
! constants, steps, k05, k16) are already filled by the caller; this fills to[]
! and foot[] for each of the ten rays (two heights, five points: the middle and
! four corners) and stops at the first that sees the chunk (dc_sight_blocked
! returns 0). Returns 1 when every ray is stopped.
!
! r8 the run  r9 the chunk  fr12 mid_x  fr13 mid_z  fr14 0.5  fr15 16.0
! The frame: 0 xs1  4 xs2  8 zs1  12 zs2  16 ys0  20 ys1

    .macro CHRAY
    mov     #72,r0
    fmov.s  fr0,@(r0,r8)      ! to x
    mov     #76,r0
    fmov.s  fr1,@(r0,r8)      ! to y
    mov     #80,r0
    fmov.s  fr2,@(r0,r8)      ! to z
    mov     r8,r4
    bsr     _dc_sight_blocked
    nop
    tst     r0,r0
    JT      ch_visible
    .endm

    .global _dc_chunk_hidden
_dc_chunk_hidden:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    add     #-24,r15
    mov     r4,r8
    mov     r5,r9
    mov     #100,r0
    fmov.s  @(r0,r8),fr8      ! inv
    mov     #124,r0
    fmov.s  @(r0,r8),fr14     ! 0.5
    mov     #128,r0
    fmov.s  @(r0,r8),fr15     ! 16.0

    ! foot: floor(lo/hi * inv), stored at 84, 88, 92, 96
    mov     #16,r0
    fmov.s  @(r0,r9),fr1      ! lo.x
    fmul    fr8,fr1
    FLOORI  fr1
    mov     #84,r0
    mov.l   r1,@(r0,r8)
    mov     #24,r0
    fmov.s  @(r0,r9),fr1      ! lo.z
    fmul    fr8,fr1
    FLOORI  fr1
    mov     #88,r0
    mov.l   r1,@(r0,r8)
    mov     #28,r0
    fmov.s  @(r0,r9),fr1      ! hi.x
    fmul    fr8,fr1
    FLOORI  fr1
    mov     #92,r0
    mov.l   r1,@(r0,r8)
    mov     #36,r0
    fmov.s  @(r0,r9),fr1      ! hi.z
    fmul    fr8,fr1
    FLOORI  fr1
    mov     #96,r0
    mov.l   r1,@(r0,r8)

    ! mid_x, mid_z
    mov     #16,r0
    fmov.s  @(r0,r9),fr0
    mov     #28,r0
    fmov.s  @(r0,r9),fr1
    fadd    fr1,fr0
    fmul    fr14,fr0
    fmov    fr0,fr12          ! mid_x
    mov     #24,r0
    fmov.s  @(r0,r9),fr0
    mov     #36,r0
    fmov.s  @(r0,r9),fr1
    fadd    fr1,fr0
    fmul    fr14,fr0
    fmov    fr0,fr13          ! mid_z

    ! xs1 = min(lo.x + 16, mid_x)
    mov     #16,r0
    fmov.s  @(r0,r9),fr0
    fadd    fr15,fr0
    fcmp/gt fr12,fr0          ! (lo.x+16) > mid_x ?
    bf      1f
    fmov    fr12,fr0
1:  mov     #0,r0
    fmov.s  fr0,@(r0,r15)
    ! xs2 = max(hi.x - 16, mid_x)
    mov     #28,r0
    fmov.s  @(r0,r9),fr0
    fsub    fr15,fr0
    fcmp/gt fr12,fr0          ! (hi.x-16) > mid_x ?
    bf      1f
    bra     2f
    nop
1:  fmov    fr12,fr0
2:  mov     #4,r0
    fmov.s  fr0,@(r0,r15)
    ! zs1 = min(lo.z + 16, mid_z)
    mov     #24,r0
    fmov.s  @(r0,r9),fr0
    fadd    fr15,fr0
    fcmp/gt fr13,fr0
    bf      1f
    fmov    fr13,fr0
1:  mov     #8,r0
    fmov.s  fr0,@(r0,r15)
    ! zs2 = max(hi.z - 16, mid_z)
    mov     #36,r0
    fmov.s  @(r0,r9),fr0
    fsub    fr15,fr0
    fcmp/gt fr13,fr0
    bf      1f
    bra     2f
    nop
1:  fmov    fr13,fr0
2:  mov     #12,r0
    fmov.s  fr0,@(r0,r15)
    ! ys0 = (lo.y + hi.y) * 0.5, ys1 = lo.y
    mov     #20,r0
    fmov.s  @(r0,r9),fr0
    mov     #32,r0
    fmov.s  @(r0,r9),fr1
    fadd    fr1,fr0
    fmul    fr14,fr0
    mov     #16,r0
    fmov.s  fr0,@(r0,r15)
    mov     #20,r0
    fmov.s  @(r0,r9),fr0
    mov     #20,r0
    fmov.s  fr0,@(r0,r15)

    ! h = 0 (y = ys0): the middle then the four corners
    fmov    fr12,fr0
    mov     #16,r0
    fmov.s  @(r0,r15),fr1
    fmov    fr13,fr2
    CHRAY
    mov     #0,r0
    fmov.s  @(r0,r15),fr0
    mov     #16,r0
    fmov.s  @(r0,r15),fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #0,r0
    fmov.s  @(r0,r15),fr0
    mov     #16,r0
    fmov.s  @(r0,r15),fr1
    mov     #12,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #4,r0
    fmov.s  @(r0,r15),fr0
    mov     #16,r0
    fmov.s  @(r0,r15),fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #4,r0
    fmov.s  @(r0,r15),fr0
    mov     #16,r0
    fmov.s  @(r0,r15),fr1
    mov     #12,r0
    fmov.s  @(r0,r15),fr2
    CHRAY

    ! h = 1 (y = ys1)
    fmov    fr12,fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    fmov    fr13,fr2
    CHRAY
    mov     #0,r0
    fmov.s  @(r0,r15),fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #0,r0
    fmov.s  @(r0,r15),fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    mov     #12,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #4,r0
    fmov.s  @(r0,r15),fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    CHRAY
    mov     #4,r0
    fmov.s  @(r0,r15),fr0
    mov     #20,r0
    fmov.s  @(r0,r15),fr1
    mov     #12,r0
    fmov.s  @(r0,r15),fr2
    CHRAY

    mov     #1,r0
    bra     ch_out
    nop
ch_visible:
    mov     #0,r0
ch_out:
    add     #24,r15
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
