! Hand-written SH-4 for the soft shadow's per-vertex projection
! (batcher.c, M_SoftProject).
!
!   void dc_soft_project(M_SOFTRUN *run);
!
! Each vertex of the instance's ranges, already in eye space, is run through
! the shadow's matrix (three dot products, w = 1), its perspective brought
! towards the middle of the cell, and the sides it lies past set as bits in
! soft_off. soft[][] gets where the vertex falls (u, v) and how far along the
! light (z).
!
! M_SOFTRUN (words): 0 xverts  4 soft  8 soft_off  12 the ranges  16 how many
!   24 the matrix (4 rows of 4 floats)  88 persp  92 dist  96 world
!   100 reach  104 behind  108 0.5  112 1/3
! M_XVERT (32 bytes): 0 x  4 y  8 z
!
! r8 run  r9 xverts  r10 soft  r11 soft_off  r12 ranges  r13 ranges left
! r14 vertex  r3 the range's end  r2 the off byte  fr8 x  fr9 y  fr10 z
! fr11 dist  fr12 reach  fr13 0.5  fr14 1/3  fr15 1.0  fr4-7 the vertex
! (x y z 1)  fr0-3 a matrix row

    ! mat_load puts the matrix in XMTRX column by column (ftrv: x' = m[0]x +
    ! m[4]y + m[8]z + m[12]w), so the row of x', y', z' is read with a stride
    ! of 16 bytes, not as four floats in a row.
    .macro SROW off, dst
    mov     r8,r0
    add     #\off,r0
    fmov.s  @r0,fr0
    add     #16,r0
    fmov.s  @r0,fr1
    add     #16,r0
    fmov.s  @r0,fr2
    add     #16,r0
    fmov.s  @r0,fr3
    fipr    fv4,fv0
    fmov    fr3,\dst
    .endm

    .global _dc_soft_project
_dc_soft_project:
    sts.l   pr,@-r15
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
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(4,r8),r10
    mov.l   @(8,r8),r11
    mov.l   @(12,r8),r12
    mov.l   @(16,r8),r13
    mov     #92,r0
    fmov.s  @(r0,r8),fr11    ! dist
    mov     #100,r0
    fmov.s  @(r0,r8),fr12    ! reach
    mov     #108,r0
    fmov.s  @(r0,r8),fr13    ! 0.5
    mov     #112,r0
    fmov.s  @(r0,r8),fr14    ! 1/3
    fldi1   fr15
sp_range:
    tst     r13,r13
    bf      1f
    bra     sp_done
    nop
1:  mov.l   @r12+,r14        ! start
    mov.l   @r12+,r3         ! count
    add     #-1,r13
    add     r14,r3           ! end = start + count
sp_loop:
    cmp/ge  r3,r14
    bt      sp_range
    ! the vertex in eye space
    mov     r14,r1
    shll2   r1
    shll2   r1
    shll    r1               ! * 32
    add     r9,r1
    fmov.s  @r1+,fr4         ! x
    fmov.s  @r1+,fr5         ! y
    fmov.s  @r1,fr6          ! z
    fldi1   fr7              ! w
    SROW    24, fr8          ! x' = m[0] m[4] m[8] m[12]
    SROW    28, fr9          ! y' = m[1] m[5] m[9] m[13]
    SROW    32, fr10         ! z' = m[2] m[6] m[10] m[14]
    mov     #88,r0
    mov.l   @(r0,r8),r1
    tst     r1,r1
    bt      sp_nopersp
    ! f = dist / max(dist + z, dist / 3), by the SH-4's own reciprocal root
    fmov    fr11,fr0
    fadd    fr10,fr0         ! depth
    fmov    fr11,fr1
    fmul    fr14,fr1         ! least
    fcmp/gt fr0,fr1          ! least > depth
    bf      1f
    fmov    fr1,fr0
1:  fsrra   fr0              ! 1/sqrt(denom)
    fmul    fr0,fr0
    fmul    fr11,fr0         ! f
    fmov    fr8,fr1
    fsub    fr13,fr1
    fmul    fr0,fr1
    fadd    fr13,fr1
    fmov    fr1,fr8          ! x
    fmov    fr9,fr1
    fsub    fr13,fr1
    fmul    fr0,fr1
    fadd    fr13,fr1
    fmov    fr1,fr9          ! y
sp_nopersp:
    ! soft[i] = (x, y, z)
    mov     r14,r1
    shll2   r1
    mov     r1,r2
    shll    r1
    add     r2,r1            ! i * 12
    add     r10,r1
    mov     #0,r0
    fmov.s  fr8,@(r0,r1)
    mov     #4,r0
    fmov.s  fr9,@(r0,r1)
    mov     #8,r0
    fmov.s  fr10,@(r0,r1)
    ! the sides it lies past, as bits
    mov     #0,r2
    fldi0   fr0
    fcmp/gt fr8,fr0          ! x < 0
    nop
    movt    r0
    or      r0,r2
    fcmp/gt fr15,fr8         ! x > 1
    nop
    movt    r0
    shll    r0
    or      r0,r2
    fcmp/gt fr9,fr0          ! y < 0
    nop
    movt    r0
    shll2   r0
    or      r0,r2
    fcmp/gt fr15,fr9         ! y > 1
    nop
    movt    r0
    shll2   r0
    shll    r0
    or      r0,r2
    mov     #96,r0
    mov.l   @(r0,r8),r1
    tst     r1,r1
    bt      1f
    fcmp/gt fr12,fr10        ! z > reach
    nop
    movt    r0
    shll2   r0
    shll2   r0
    or      r0,r2
    mov     #104,r0
    fmov.s  @(r0,r8),fr0     ! behind
    fcmp/gt fr10,fr0         ! z < behind
    nop
    movt    r0
    shll2   r0
    shll2   r0
    shll    r0
    or      r0,r2
1:  mov     r14,r0
    mov.b   r2,@(r0,r11)
    add     #1,r14
    bra     sp_loop
    nop
sp_done:
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
    lds.l   @r15+,pr
    rts
    nop
