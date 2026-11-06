! Hand-written SH-4 for the batcher's particles, quads and streaks (batcher.c,
! M_DrawParticles, M_DrawQuads, M_DrawStreaks).
!
!   void dc_draw_particles(DC_PRUN *run);
!   void dc_draw_quads(DC_PRUN *run);
!   void dc_draw_streaks(DC_PRUN *run);
!
! Each takes the staged items from run->index on, and writes the ones that show
! straight into the polygon list (the region DC_PVR_BeginRun gives, 128 bytes an
! item: four vertices): the world points through the view matrix in XMTRX
! (loaded by the caller), rejected before the near plane or outside the
! screen's rectangle, one quad a particle, a quad or a streak, as the C loops
! do. They stop, with the index of the item they stopped at, at an item of
! another texture page (stop 1, its page in run->stop_page: the caller sets the
! state and goes on), when the region has no room for another (stop 2) and at
! the end (stop 0).
!
! DC_PRUN (batcher.c; the integers first, within the 60 bytes @(disp,rn) reaches):
!    0 items  4 count  8 index (in, out)  12 blend  16 page (the page the state
!   is set to)  20 out  24 space  28 written (out)  32 polys (out)
!   36 stop_page (out)  40 stop (out)
!   48 near_z  52 center_x  56 center_y  60 proj_x  64 proj_y  68 lo_x  72 hi_x
!   76 lo_y  80 hi_y  84 sc (the screen scale)  88 depth_lo  92 depth_scale
! DC_PARTICLE (72 bytes): 0 pos 12 half 16 half_h 20 sn 24 cs 28 blend 32 argb
!   36 page 40 u[4] 56 v[4]
! DC_QUAD (112): 0 p[4][3] 48 u[4] 64 v[4] 80 argb[4] 96 page 100 blend 104 n
!   108 pull
! DC_STREAK (32): 0 a 12 b 24 argb_a 28 argb_b
!
! Registers (r8-r14 are the caller's, and are kept; the floating point ones
! used are fr0-fr11, which the C ABI lets a callee spend):
!   r5 index  r6 items  r7 run  r8 out  r9 space  r10 polys  r11 blend
!   r12 page  r13 count  r14 the item; r0-r4 scratch
!   fr8 near z   fr9 proj_x   fr10 the screen scale
! A vertex is written backwards from its end: oargb, argb, v, u, z, y, x,
! flags (the list's layout: flags 0, x 4, y 8, z 12, u 16, v 20, argb 24,
! oargb 28), r8 first moved to the vertex's end.

! A float of the run (at \off) into \dst (r0 is spent).
    .macro LDF off, dst
    mov     r7,r0
    add     #\off,r0
    fmov.s  @r0,\dst
    .endm

! What every entry does: the registers saved, the run's fields in theirs.
    .macro ENTER stride
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    sts.l   pr,@-r15
    mov     r4,r7
    mov.l   @(0,r7),r6
    mov.l   @(4,r7),r13
    mov.l   @(8,r7),r5
    mov.l   @(12,r7),r11
    mov.l   @(16,r7),r12
    mov.l   @(20,r7),r8
    mov.l   @(24,r7),r9
    mov     #0,r10
    mov     #0,r0
    mov.l   r0,@(40,r7)
    LDF     48, fr8
    LDF     60, fr9
    LDF     84, fr10
    mov     r5,r0
    mov     #\stride,r1
    mul.l   r1,r0
    sts     macl,r14
    add     r6,r14                  ! the item
    .endm

! What every entry ends with: the index, the polygons and bytes written, the
! registers back, and \frame bytes of the caller's stack given up.
    .macro LEAVE frame
    mov.l   r5,@(8,r7)
    mov.l   r10,@(32,r7)
    mov     r10,r0
    shll2   r0
    shll2   r0
    shll2   r0
    shll    r0                      ! 128 bytes a polygon
    mov.l   r0,@(28,r7)
    lds.l   @r15+,pr
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    add     #\frame,r15
    rts
    nop
    .endm

! M_Depth of fr3 (the 1/w): at most 1, at least the lowest, times the scale.
    .macro DEPTH
    fldi1   fr2
    fcmp/gt fr2,fr3
    bf      1f
    fmov    fr2,fr3
1:  LDF     88, fr2
    fcmp/gt fr3,fr2
    bf      1f
    fmov    fr2,fr3
1:  LDF     92, fr2
    fmul    fr2,fr3
    .endm

! The vertex flags: r2 for a vertex that is not the last, r3 for the last.
    .macro FLAGS
    mov     #-32,r2
    shll16  r2
    shll8   r2                      ! PVR_CMD_VERTEX 0xE0000000
    mov     #-16,r3
    shll16  r3
    shll8   r3                      ! PVR_CMD_VERTEX_EOL 0xF0000000
    .endm

! The world point at r1 (three floats, r1 moved past them) through XMTRX: the
! screen place and the depth of its 1/z, which the C loops find the same way:
! fr0 sx, fr1 sy, fr3 the depth, with fr4 the 1/z, or a branch to \skip when it
! lies before the near plane. fr11 is the divisor of the depth (1: none).
    .macro PROJECT skip
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi1   fr3
    ftrv    xmtrx,fv0
    fcmp/gt fr2,fr8                 ! before the near plane
    bf      7f
    bra     \skip
    nop
7:  fldi1   fr4
    fdiv    fr2,fr4                 ! 1/z
    fmul    fr9,fr0
    fmul    fr4,fr0
    LDF     52, fr5
    fadd    fr5,fr0                 ! sx
    LDF     64, fr5
    fmul    fr5,fr1
    fmul    fr4,fr1
    LDF     56, fr5
    fadd    fr5,fr1                 ! sy
    fmov    fr8,fr3
    fmul    fr4,fr3
    fdiv    fr11,fr3                ! near z / z (/ (1 - pull))
    DEPTH
    .endm

! One vertex of a particle at the picture's corner \c, the offsets at fr4 (a),
! fr5 (b), fr6 (c), fr7 (d), the signs of the corner's offsets in \sa..\sd:
! x = (sx +- a) +- c, y = (sy +- b) +- d, times the scale; the flags in \flag.
    .macro PVERT c, xa, xc, yb, yd, flag, adv
    add     #\adv,r8
    mov     #0,r0
    mov.l   r0,@-r8                 ! oargb
    mov.l   r4,@-r8                 ! argb
    mov     #(56+4*\c),r0
    fmov.s  @(r0,r14),fr2
    fmov.s  fr2,@-r8                ! v
    mov     #(40+4*\c),r0
    fmov.s  @(r0,r14),fr2
    fmov.s  fr2,@-r8                ! u
    fmov.s  fr3,@-r8                ! z
    fmov    fr1,fr2
    \yb     fr5,fr2
    \yd     fr7,fr2
    fmul    fr10,fr2
    fmov.s  fr2,@-r8                ! y
    fmov    fr0,fr2
    \xa     fr4,fr2
    \xc     fr6,fr2
    fmul    fr10,fr2
    fmov.s  fr2,@-r8                ! x
    mov.l   \flag,@-r8
    .endm

    .text
    .align 2
    .global _dc_draw_particles
    .global _dc_draw_quads
    .global _dc_draw_streaks

! ---------------------------------------------------------------- particles
_dc_draw_particles:
    ENTER   72
    bra     ploop
    nop
pskip:
    bra     pnext
    nop
ppage:
    mov.l   r0,@(36,r7)
    mov     #1,r0
    mov.l   r0,@(40,r7)
    bra     pdone
    nop
pspace:
    mov     #2,r0
    mov.l   r0,@(40,r7)
    bra     pdone
    nop
pnext:
    add     #1,r5
    add     #72,r14
ploop:
    cmp/ge  r13,r5
    bf      1f
    bra     pdone
    nop
1:  mov.l   @(28,r14),r0
    cmp/eq  r11,r0
    bf      pnext
    mov     r14,r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fldi1   fr3
    ftrv    xmtrx,fv0               ! fr0 x, fr1 y, fr2 z
    fcmp/gt fr2,fr8                 ! before the near plane
    bt      pskip
    fldi1   fr4
    fdiv    fr2,fr4                 ! 1/z
    fmul    fr9,fr0
    fmul    fr4,fr0
    LDF     52, fr5
    fadd    fr5,fr0                 ! sx
    LDF     64, fr5
    fmul    fr5,fr1
    fmul    fr4,fr1
    LDF     56, fr5
    fadd    fr5,fr1                 ! sy
    ! half sides in pixels, one at least
    fmov    fr9,fr5
    fabs    fr5
    mov     r14,r1
    add     #12,r1
    fmov.s  @r1,fr6
    fmul    fr6,fr5
    fmul    fr4,fr5
    fldi1   fr6
    fcmp/gt fr5,fr6
    bf      1f
    fmov    fr6,fr5                 ! hw
1:  LDF     64, fr6
    fabs    fr6
    mov     r14,r1
    add     #16,r1
    fmov.s  @r1,fr7
    fmul    fr7,fr6
    fmul    fr4,fr6
    fldi1   fr7
    fcmp/gt fr6,fr7
    bf      1f
    fmov    fr7,fr6                 ! hh
1:  fmov    fr5,fr7
    fcmp/gt fr7,fr6
    bf      1f
    fmov    fr6,fr7                 ! the larger
1:  mov     r14,r1
    add     #20,r1
    fmov.s  @r1,fr2                 ! sn
    fldi0   fr3
    fcmp/eq fr3,fr2
    bt      1f
    mov.l   .Lreach,r1
    lds     r1,fpul
    fsts    fpul,fr3
    fmul    fr3,fr7                 ! turned: 1.4143 times
1:  ! the rectangle of the screen
    fmov    fr0,fr3
    fadd    fr7,fr3
    LDF     68, fr2
    fcmp/gt fr3,fr2
    bt      pskip
    fmov    fr0,fr3
    fsub    fr7,fr3
    LDF     72, fr2
    fcmp/gt fr2,fr3
    bt      pskip
    fmov    fr1,fr3
    fadd    fr7,fr3
    LDF     76, fr2
    fcmp/gt fr3,fr2
    bt      pskip
    fmov    fr1,fr3
    fsub    fr7,fr3
    LDF     80, fr2
    fcmp/gt fr2,fr3
    bt      pskip
    mov.l   @(36,r14),r0            ! its page
    cmp/eq  r12,r0
    bt      1f
    bra     ppage
    nop
1:  mov     #64,r0
    shll    r0                      ! 128 bytes
    cmp/ge  r0,r9
    bt      1f
    bra     pspace
    nop
1:  ! the depth, and the corner offsets: a = hw cs, b = hw sn, c = hh sn, d = hh cs
    fmov    fr8,fr3
    fmul    fr4,fr3
    fldi1   fr11
    DEPTH                           ! fr3
    mov     r14,r1
    add     #24,r1
    fmov.s  @r1,fr2                 ! cs
    fmov    fr5,fr4
    fmul    fr2,fr4                 ! a
    fmov    fr6,fr7
    fmul    fr2,fr7                 ! d
    mov     r14,r1
    add     #20,r1
    fmov.s  @r1,fr2                 ! sn
    fmul    fr2,fr5                 ! b
    fmul    fr2,fr6                 ! c
    FLAGS
    mov.l   @(32,r14),r4            ! the colour
    ! The corners as the C loop writes them: the picture's 0, 3, 1, 2.
    PVERT   0, fsub, fadd, fsub, fsub, r2, 32
    PVERT   3, fsub, fsub, fsub, fadd, r2, 64
    PVERT   1, fadd, fadd, fadd, fsub, r2, 64
    PVERT   2, fadd, fsub, fadd, fadd, r3, 64
    add     #32,r8                  ! past the quad
    add     #-128,r9
    add     #1,r10
    bra     pnext
    nop
pdone:
    LEAVE   0
    .align 2
.Lreach:
    .long   0x3fb507c8

! ------------------------------------------------------------------- quads
! The corners' screen place and depth wait in the frame: 12 bytes each, from
! r15 + 32 (the registers ENTER keeps come first).
    .macro QVERT k, flag, adv
    add     #\adv,r8
    mov     #(80+4*\k),r0
    mov.l   @(r0,r14),r1
    mov     #0,r0
    mov.l   r0,@-r8                 ! oargb
    mov.l   r1,@-r8                 ! argb
    mov     #(64+4*\k),r0
    fmov.s  @(r0,r14),fr0
    fmov.s  fr0,@-r8                ! v
    mov     #(48+4*\k),r0
    fmov.s  @(r0,r14),fr0
    fmov.s  fr0,@-r8                ! u
    mov     #(40+12*\k),r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@-r8                ! z
    mov     #(36+12*\k),r0
    fmov.s  @(r0,r15),fr0
    fmul    fr10,fr0
    fmov.s  fr0,@-r8                ! y
    mov     #(32+12*\k),r0
    fmov.s  @(r0,r15),fr0
    fmul    fr10,fr0
    fmov.s  fr0,@-r8                ! x
    mov.l   \flag,@-r8
    .endm

_dc_draw_quads:
    add     #-48,r15
    ENTER   112
    bra     qloop
    nop
qskip:
    bra     qnext
    nop
qpage:
    mov.l   r0,@(36,r7)
    mov     #1,r0
    mov.l   r0,@(40,r7)
    bra     qdone
    nop
qspace:
    mov     #2,r0
    mov.l   r0,@(40,r7)
    bra     qdone
    nop
qnext:
    add     #1,r5
    add     #112,r14
qloop:
    cmp/ge  r13,r5
    bf      1f
    bra     qdone
    nop
1:  mov     #100,r0
    mov.l   @(r0,r14),r0
    cmp/eq  r11,r0
    bf      qnext
    mov     r14,r1
    add     #108,r1
    fmov.s  @r1,fr0
    fldi1   fr11
    fsub    fr0,fr11                ! 1 - pull
    mov     #104,r0
    mov.l   @(r0,r14),r4            ! n
    mov     r15,r2
    add     #32,r2                  ! the frame's place for the corners
    mov     r14,r1                  ! the corner's world point
    mov     r4,r0
    mov     r0,r3
qcorner:
    PROJECT qskip
    fmov.s  fr0,@r2
    add     #4,r2
    fmov.s  fr1,@r2
    add     #4,r2
    fmov.s  fr3,@r2
    add     #4,r2
    dt      r3
    bt      qbox0
    bra     qcorner
    nop
qbox0:
    ! The rectangle they cover, from the n corners kept in the frame.
    mov     r15,r2
    add     #32,r2
    mov.l   .Lbig,r0
    lds     r0,fpul
    fsts    fpul,fr4                ! min x
    fmov    fr4,fr6                 ! min y
    fmov    fr4,fr5
    fneg    fr5                     ! max x
    fmov    fr5,fr7                 ! max y
    mov     r4,r3
qbox:
    fmov.s  @r2+,fr0                ! sx
    fmov.s  @r2+,fr1                ! sy
    fmov.s  @r2+,fr2                ! depth
    fcmp/gt fr0,fr4
    bf      1f
    fmov    fr0,fr4
1:  fcmp/gt fr5,fr0
    bf      1f
    fmov    fr0,fr5
1:  fcmp/gt fr1,fr6
    bf      1f
    fmov    fr1,fr6
1:  fcmp/gt fr7,fr1
    bf      1f
    fmov    fr1,fr7
1:  dt      r3
    bf      qbox
    LDF     68, fr0
    fcmp/gt fr5,fr0                 ! lo x > max x
    bf      1f
    bra     qskip
    nop
1:  LDF     72, fr0
    fcmp/gt fr0,fr4                 ! min x > hi x
    bf      1f
    bra     qskip
    nop
1:  LDF     76, fr0
    fcmp/gt fr7,fr0
    bf      1f
    bra     qskip
    nop
1:  LDF     80, fr0
    fcmp/gt fr0,fr6
    bf      1f
    bra     qskip
    nop
1:  mov     #96,r0
    mov.l   @(r0,r14),r0            ! its page
    cmp/eq  r12,r0
    bt      1f
    bra     qpage
    nop
1:  mov     #64,r0
    shll    r0
    cmp/ge  r0,r9
    bt      1f
    bra     qspace
    nop
1:  FLAGS
    mov     #4,r0
    cmp/eq  r0,r4
    bt      1f
    ! the triangle goes 0, 2, 1, 1
    QVERT   0, r2, 32
    QVERT   2, r2, 64
    QVERT   1, r2, 64
    QVERT   1, r3, 64
    bra     qfin
    nop
1:  ! the quad goes 0, 3, 1, 2
    QVERT   0, r2, 32
    QVERT   3, r2, 64
    QVERT   1, r2, 64
    QVERT   2, r3, 64
qfin:
    add     #32,r8
    add     #-128,r9
    add     #1,r10
    bra     qnext
    nop
qdone:
    LEAVE   48
    .align 2
.Lbig:
    .long   0x7149f2ca

! ----------------------------------------------------------------- streaks
! The two ends' screen place and depth wait in the frame: 12 bytes each.
    .macro SVERT x, xs, y, ys, d, argb, flag, adv
    add     #\adv,r8
    mov     #0,r0
    mov.l   r0,@-r8                 ! oargb
    mov.l   \argb,@-r8              ! argb
    mov.l   r0,@-r8                 ! v
    mov.l   r0,@-r8                 ! u
    fmov.s  \d,@-r8                 ! z
    fmov    \y,fr7
    \ys     fr6,fr7
    fmul    fr10,fr7
    fmov.s  fr7,@-r8                ! y
    fmov    \x,fr7
    \xs     fr11,fr7
    fmul    fr10,fr7
    fmov.s  fr7,@-r8                ! x
    mov.l   \flag,@-r8
    .endm

_dc_draw_streaks:
    add     #-24,r15
    ENTER   32
    bra     sloop
    nop
sskip:
    bra     snext
    nop
sspace:
    mov     #2,r0
    mov.l   r0,@(40,r7)
    bra     sdone
    nop
snext:
    add     #1,r5
    add     #32,r14
    LDF     48, fr8                 ! the streak spends fr8 to fr10
    LDF     60, fr9
    LDF     84, fr10
sloop:
    cmp/ge  r13,r5
    bf      1f
    bra     sdone
    nop
1:  mov     #64,r0
    shll    r0
    cmp/ge  r0,r9
    bt      1f
    bra     sspace
    nop
1:  fldi1   fr11                    ! no divisor
    mov     r14,r1
    mov     r15,r2
    add     #32,r2
    mov     #2,r3
scorner:
    PROJECT sskip
    fmov.s  fr0,@r2
    add     #4,r2
    fmov.s  fr1,@r2
    add     #4,r2
    fmov.s  fr3,@r2
    add     #4,r2
    dt      r3
    bt      sends
    bra     scorner
    nop
sends:
    mov     r15,r2
    add     #32,r2
    fmov.s  @r2+,fr0                ! sx0
    fmov.s  @r2+,fr1                ! sy0
    fmov.s  @r2+,fr2                ! depth 0
    fmov.s  @r2+,fr3                ! sx1
    fmov.s  @r2+,fr4                ! sy1
    fmov.s  @r2+,fr5                ! depth 1
    ! both ends past the same side: gone
    LDF     68, fr6
    fcmp/gt fr0,fr6
    bf      1f
    fcmp/gt fr3,fr6
    bf      1f
    bra     sskip
    nop
1:  LDF     72, fr6
    fcmp/gt fr6,fr0
    bf      1f
    fcmp/gt fr6,fr3
    bf      1f
    bra     sskip
    nop
1:  LDF     76, fr6
    fcmp/gt fr1,fr6
    bf      1f
    fcmp/gt fr4,fr6
    bf      1f
    bra     sskip
    nop
1:  LDF     80, fr6
    fcmp/gt fr6,fr1
    bf      1f
    fcmp/gt fr6,fr4
    bf      1f
    bra     sskip
    nop
1:  fmov    fr3,fr6
    fsub    fr0,fr6                 ! dx
    fmov    fr4,fr7
    fsub    fr1,fr7                 ! dy
    fmov    fr6,fr8
    fmul    fr8,fr8
    fmov    fr7,fr9
    fmul    fr9,fr9
    fadd    fr9,fr8                 ! dx^2 + dy^2
    mov.l   .Ltiny,r1
    lds     r1,fpul
    fsts    fpul,fr9
    fcmp/gt fr8,fr9                 ! shorter than a hundredth of a pixel: gone
    bf      1f
    bra     sskip
    nop
1:  fsrra   fr8                     ! 1 / length
    mov.l   .Lp75,r1
    lds     r1,fpul
    fsts    fpul,fr9
    fmul    fr9,fr8                 ! k = 0.75 / length
    fmov    fr7,fr11
    fneg    fr11
    fmul    fr8,fr11                ! nx = -dy k
    fmul    fr8,fr6                 ! ny = dx k
    LDF     84, fr10
    FLAGS
    mov.l   @(24,r14),r4
    mov.l   @(28,r14),r1
    SVERT   fr0, fadd, fr1, fadd, fr2, r4, r2, 32
    SVERT   fr0, fsub, fr1, fsub, fr2, r4, r2, 64
    SVERT   fr3, fadd, fr4, fadd, fr5, r1, r2, 64
    SVERT   fr3, fsub, fr4, fsub, fr5, r1, r3, 64
    add     #32,r8
    add     #-128,r9
    add     #1,r10
    bra     snext
    nop
sdone:
    ! fr8 to fr10 are the caller's to spend; the run keeps what it needs.
    LEAVE   24

    .align 2
.Lp75:
    .long   0x3f400000
.Ltiny:
    .long   0x38d1b717
