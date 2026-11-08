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

! int dc_soft_faces(M_SOFTFACES *run);   batcher.c, M_SoftFaces (M_SoftFacesC)
!
! Which faces of an instance a soft shadow falls on, and how much: for each
! face of the ranges, from the cursor on, the three corners' off bits, clip
! bits, which way it turns on the screen, its normal against the light and
! (for the world) how it grazes the light. A face that takes the shadow goes
! to the list, as its three indices and its strength. Returns 1 when the
! ranges are done, or 0 with the cursor at the face that did not fit the list.
!
! M_SOFTFACES (words): 0 indices  4 ranges  8 range_count  12 xverts  16 soft
!   20 soft_off  24 cur_r  28 cur_i  32 out  36 out_cap  40 out_count  44 world
!   48 strength  52 axes0 x y z  64 axes_sq  68 graze_min  72 inv_graze
! M_XVERT (32 bytes): 0 x 4 y 8 z 12 sx 16 sy 28 clip
!
! r8 run  r9 indices  r10 soft_off  r11 xverts  r13 i  r14 end  fr12-14 axes0
    .global _dc_soft_faces
    .align 2
_dc_soft_faces:
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
    mov     r4,r8
    mov.l   @(0,r8),r9
    mov.l   @(20,r8),r10
    mov.l   @(12,r8),r11
    mov     r8,r0
    add     #52,r0
    fmov.s  @r0+,fr12
    fmov.s  @r0+,fr13
    fmov.s  @r0,fr14
sf_range:
    mov.l   @(24,r8),r1             ! cur_r
    mov.l   @(8,r8),r2              ! range_count
    cmp/ge  r2,r1
    bt      sf_all
    mov     r1,r0
    shll2   r0
    shll2   r0                      ! * 16
    mov.l   @(4,r8),r3
    add     r0,r3                   ! the range
    mov.l   @r3,r13                 ! start
    mov.l   @(4,r3),r14
    add     r13,r14                 ! end
    mov.l   @(28,r8),r1             ! cur_i
    cmp/pz  r1
    bf      sf_face
    mov     r1,r13                  ! resume
sf_face:
    mov     r13,r0
    add     #2,r0
    cmp/ge  r14,r0                  ! i + 2 >= end
    bt      sf_next_range
    mov     r13,r0
    shll    r0
    mov.w   @(r0,r9),r1
    extu.w  r1,r1                   ! idx a
    mov     r13,r0
    add     #1,r0
    shll    r0
    mov.w   @(r0,r9),r2
    extu.w  r2,r2                   ! idx b
    mov     r13,r0
    add     #2,r0
    shll    r0
    mov.w   @(r0,r9),r3
    extu.w  r3,r3                   ! idx c
    mov     r1,r0
    mov.b   @(r0,r10),r5
    mov     r2,r0
    mov.b   @(r0,r10),r6
    and     r6,r5
    mov     r3,r0
    mov.b   @(r0,r10),r6
    and     r6,r5
    tst     r5,r5
    bf      sf_skip                 ! off the cell on one same side
    ! the corners: r5 r6 r7 = a b c  (idx * 32)
    mov     r1,r5
    shll2   r5
    shll2   r5
    shll    r5
    add     r11,r5
    mov     r2,r6
    shll2   r6
    shll2   r6
    shll    r6
    add     r11,r6
    mov     r3,r7
    shll2   r7
    shll2   r7
    shll    r7
    add     r11,r7
    ! the clip bits
    mov     r5,r4
    add     #28,r4
    mov.b   @r4,r12                 ! a
    mov     r6,r4
    add     #28,r4
    mov.b   @r4,r4                  ! b
    mov     r12,r0
    and     r4,r0                   ! a & b
    or      r4,r12                  ! a | b
    mov     r7,r4
    add     #28,r4
    mov.b   @r4,r4                  ! c
    and     r4,r0                   ! all three
    or      r4,r12                  ! any
    and     #0x3F,r0
    tst     r0,r0
    bf      sf_skip                 ! M_CLIP_CULL
    mov     r12,r0
    tst     #2,r0
    bf      sf_skip                 ! M_CLIP_NEAR
    ! which way it turns on the screen
    mov     r5,r4
    add     #12,r4
    fmov.s  @r4+,fr0                ! a sx
    fmov.s  @r4,fr1                 ! a sy
    mov     r6,r4
    add     #12,r4
    fmov.s  @r4+,fr2                ! b sx
    fmov.s  @r4,fr3                 ! b sy
    mov     r7,r4
    add     #12,r4
    fmov.s  @r4+,fr4                ! c sx
    fmov.s  @r4,fr5                 ! c sy
    fsub    fr0,fr2
    fsub    fr1,fr3
    fsub    fr0,fr4
    fsub    fr1,fr5
    fmul    fr5,fr2                 ! (b.sx - a.sx) * (c.sy - a.sy)
    fmul    fr3,fr4                 ! (c.sx - a.sx) * (b.sy - a.sy)
    fsub    fr4,fr2
    fldi0   fr0
    fcmp/gt fr2,fr0                 ! 0 > d : turns the way that is drawn
    bf      sf_skip
    ! the normal
    fmov.s  @r5+,fr0
    fmov.s  @r5+,fr1
    fmov.s  @r5,fr2                 ! a
    fmov.s  @r6+,fr3
    fmov.s  @r6+,fr4
    fmov.s  @r6,fr5                 ! b
    fmov.s  @r7+,fr6
    fmov.s  @r7+,fr7
    fmov.s  @r7,fr8                 ! c
    fsub    fr0,fr3                 ! e1
    fsub    fr1,fr4
    fsub    fr2,fr5
    fsub    fr0,fr6                 ! e2
    fsub    fr1,fr7
    fsub    fr2,fr8
    fmov    fr4,fr9
    fmul    fr8,fr9
    fmov    fr5,fr10
    fmul    fr7,fr10
    fsub    fr10,fr9                ! nx = e1y * e2z - e1z * e2y
    fmov    fr5,fr10
    fmul    fr6,fr10
    fmov    fr3,fr11
    fmul    fr8,fr11
    fsub    fr11,fr10               ! ny = e1z * e2x - e1x * e2z
    fmul    fr7,fr3
    fmul    fr6,fr4
    fsub    fr4,fr3                 ! nz = e1x * e2y - e1y * e2x
    fmov    fr9,fr4
    fmul    fr12,fr4
    fmov    fr10,fr5
    fmul    fr13,fr5
    fadd    fr5,fr4
    fmov    fr3,fr5
    fmul    fr14,fr5
    fadd    fr5,fr4                 ! fr4 = n . light
    fmov    fr9,fr5
    fmul    fr0,fr5
    fmov    fr10,fr6
    fmul    fr1,fr6
    fadd    fr6,fr5
    fmov    fr3,fr6
    fmul    fr2,fr6
    fadd    fr6,fr5                 ! fr5 = n . p0
    fldi0   fr6
    fcmp/gt fr6,fr5                 ! n . p0 > 0
    movt    r0
    fcmp/gt fr4,fr6                 ! n . light < 0
    movt    r12
    cmp/eq  r0,r12
    bt      sf_skip
    ! the strength
    mov     r8,r0
    add     #48,r0
    fmov.s  @r0,fr8                 ! strength
    mov.l   @(44,r8),r0
    tst     r0,r0
    bt      sf_store                ! not the world: the strength as is
    fmov    fr9,fr5
    fmul    fr9,fr5
    fmov    fr10,fr6
    fmul    fr10,fr6
    fadd    fr6,fr5
    fmov    fr3,fr6
    fmul    fr3,fr6
    fadd    fr6,fr5                 ! n . n
    mov     r8,r0
    add     #64,r0
    fmov.s  @r0,fr6
    fmul    fr6,fr5                 ! len_sq
    fldi0   fr6
    fcmp/gt fr6,fr5                 ! len_sq > 0
    bf      sf_cos0
    fsrra   fr5
    fabs    fr4
    fmul    fr5,fr4                 ! cos_l
    bra     sf_cos
    nop
sf_cos0:
    fldi0   fr4
sf_cos:
    mov     r8,r0
    add     #68,r0
    fmov.s  @r0+,fr6                ! graze_min
    fmov.s  @r0,fr7                 ! inv_graze
    fsub    fr6,fr4
    fmul    fr7,fr4
    fldi0   fr6
    fcmp/gt fr4,fr6                 ! x < 0
    bf      sf_c1
    fldi0   fr4
    bra     sf_c2
    nop
sf_c1:
    fldi1   fr7
    fcmp/gt fr7,fr4                 ! x > 1
    bf      sf_c2
    fldi1   fr4
sf_c2:
    fmul    fr4,fr8                 ! strength * clamp(...)
    fldi0   fr6
    fcmp/gt fr6,fr8                 ! > 0
    bf      sf_skip
sf_store:
    mov.l   @(40,r8),r4             ! out_count
    mov.l   @(36,r8),r0
    cmp/ge  r0,r4
    bf      sf_room
    mov.l   r13,@(28,r8)            ! the list is full: stop at this face
    bra     sf_out
    mov     #0,r0
sf_room:
    mov     r4,r0
    shll2   r0
    shll2   r0                      ! * 16
    mov.l   @(32,r8),r5
    add     r0,r5
    mov.l   r1,@r5
    mov.l   r2,@(4,r5)
    mov.l   r3,@(8,r5)
    add     #12,r5
    fmov.s  fr8,@r5
    add     #1,r4
    mov.l   r4,@(40,r8)
sf_skip:
    add     #3,r13
    bra     sf_face
    nop
sf_next_range:
    mov.l   @(24,r8),r1
    add     #1,r1
    mov.l   r1,@(24,r8)
    mov     #-1,r1
    mov.l   r1,@(28,r8)
    bra     sf_range
    nop
sf_all:
    mov     #1,r0
sf_out:
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

! void dc_sil_project(M_SILRUN *run);   batcher.c, M_SilProject (M_SilProjectC)
!
! Where each vertex of a caster falls on its cell of the shadow map (x, y in
! texels), for the silhouettes: the vertex run through two rows of the
! caster-to-cell matrix, brought towards the middle by the perspective of a
! light close by, kept inside the cell, times its size. A vertex that is
! another's twin takes the twin's.
!
! M_SILRUN (words): 0 vertices  4 vref (or 0)  8 twin (or 0)  12 count
!   16 xy  20 persp  24 row x (4 floats)  40 row y (4)  56 depth row (4)
!   72 dist  76 size  80 0.5  84 1/3
! OUTPUT_MESH_VERTEX (28 bytes): x y z (int16) at 8, 10, 12
!
! r8 run  r9 vertices  r10 vref  r11 twin  r12 count  r13 j  r14 xy (j)
! fr4-7 row x  fr8-11 row y  fr0-2 the vertex  fr12 acc  fr13 x  fr14 y  fr15 tmp
    .global _dc_sil_project
    .align 2
_dc_sil_project:
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
    mov.l   @(16,r8),r14
    mov     r8,r0
    add     #24,r0
    fmov.s  @r0+,fr4
    fmov.s  @r0+,fr5
    fmov.s  @r0+,fr6
    fmov.s  @r0+,fr7
    fmov.s  @r0+,fr8
    fmov.s  @r0+,fr9
    fmov.s  @r0+,fr10
    fmov.s  @r0+,fr11
    mov     #0,r13
si_next:
    cmp/ge  r12,r13
    bt      si_out
    tst     r11,r11
    bt      si_vertex               ! no twins
    mov     r13,r0
    shll    r0
    mov.w   @(r0,r11),r1
    extu.w  r1,r1                   ! twin[j]
    cmp/eq  r13,r1
    bt      si_vertex
    mov     r1,r2
    shll2   r2
    shll    r2                      ! * 8
    mov.l   @(16,r8),r3
    add     r2,r3
    fmov.s  @r3+,fr0
    fmov.s  @r3,fr1
    fmov.s  fr0,@r14
    mov     r14,r0
    add     #4,r0
    fmov.s  fr1,@r0
    bra     si_step
    nop
si_vertex:
    mov     r13,r1
    tst     r10,r10
    bt      si_noref
    mov     r13,r0
    shll    r0
    mov.w   @(r0,r10),r1
    extu.w  r1,r1                   ! vref[j]
si_noref:
    mov     r1,r2
    shll2   r2                      ! 4i
    mov     r2,r3
    shll2   r3
    shll    r3                      ! 32i
    sub     r2,r3                   ! 28i
    add     r9,r3
    add     #8,r3
    mov.w   @r3+,r1
    lds     r1,fpul
    float   fpul,fr0                ! px
    mov.w   @r3+,r1
    lds     r1,fpul
    float   fpul,fr1                ! py
    mov.w   @r3,r1
    lds     r1,fpul
    float   fpul,fr2                ! pz
    fmov    fr4,fr13
    fmul    fr0,fr13
    fmov    fr5,fr12
    fmul    fr1,fr12
    fadd    fr12,fr13
    fmov    fr6,fr12
    fmul    fr2,fr12
    fadd    fr12,fr13
    fadd    fr7,fr13                ! x
    fmov    fr8,fr14
    fmul    fr0,fr14
    fmov    fr9,fr12
    fmul    fr1,fr12
    fadd    fr12,fr14
    fmov    fr10,fr12
    fmul    fr2,fr12
    fadd    fr12,fr14
    fadd    fr11,fr14               ! y
    mov.l   @(20,r8),r0
    tst     r0,r0
    bt      si_clamp                ! parallel
    mov     r8,r0
    add     #56,r0
    fmov.s  @r0+,fr15
    fmul    fr0,fr15
    fmov.s  @r0+,fr12
    fmul    fr1,fr12
    fadd    fr12,fr15
    fmov.s  @r0+,fr12
    fmul    fr2,fr12
    fadd    fr12,fr15
    fmov.s  @r0,fr12
    fadd    fr12,fr15               ! z
    mov     r8,r0
    add     #72,r0
    fmov.s  @r0,fr12                ! dist
    fadd    fr12,fr15               ! depth = dist + z
    mov     r8,r0
    add     #84,r0
    fmov.s  @r0,fr0                 ! 1/3
    fmul    fr12,fr0                ! least = dist / 3
    fcmp/gt fr0,fr15                ! depth > least
    bt      si_depth
    fmov    fr0,fr15
si_depth:
    fsrra   fr15
    fmul    fr15,fr12
    fmul    fr15,fr12               ! f = dist * r * r
    mov     r8,r0
    add     #80,r0
    fmov.s  @r0,fr0                 ! 0.5
    fsub    fr0,fr13
    fmul    fr12,fr13
    fadd    fr0,fr13
    fsub    fr0,fr14
    fmul    fr12,fr14
    fadd    fr0,fr14
si_clamp:
    fldi0   fr0
    fldi1   fr1
    fcmp/gt fr13,fr0                ! x < 0
    bf      si_x1
    fldi0   fr13
    bra     si_x2
    nop
si_x1:
    fcmp/gt fr1,fr13                ! x > 1
    bf      si_x2
    fldi1   fr13
si_x2:
    fcmp/gt fr14,fr0                ! y < 0
    bf      si_y1
    fldi0   fr14
    bra     si_y2
    nop
si_y1:
    fcmp/gt fr1,fr14                ! y > 1
    bf      si_y2
    fldi1   fr14
si_y2:
    mov     r8,r0
    add     #76,r0
    fmov.s  @r0,fr0                 ! size
    fmul    fr0,fr13
    fmul    fr0,fr14
    fmov.s  fr13,@r14
    mov     r14,r0
    add     #4,r0
    fmov.s  fr14,@r0
si_step:
    add     #8,r14
    add     #1,r13
    bra     si_next
    nop
si_out:
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

! void dc_casters_center(const M_CASTER *casters, int32_t count, float out[3]);
!
! The middle of the casters' bounding boxes in the world, as batcher.c
! M_CastersCenter has it: out += (world * the box's middle) / count, each.
! M_CASTER (bytes): 4 bounds lo x y z, hi x y z  28 world, 3 rows of 4 floats.
! The record is 88 bytes.
    .global _dc_casters_center
    .align 2
_dc_casters_center:
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    fldi0   fr8
    fldi0   fr9
    fldi0   fr10
    mov     r5,r1
    lds     r1,fpul
    float   fpul,fr15               ! count
    mov.l   .Lcc_half,r0
    lds     r0,fpul
    fsts    fpul,fr14               ! 0.5
cc_next:
    cmp/pl  r5
    bf      cc_out
    mov     r4,r0
    add     #4,r0
    fmov.s  @r0+,fr0                ! lo x
    fmov.s  @r0+,fr1
    fmov.s  @r0+,fr2
    fmov.s  @r0+,fr3                ! hi x
    fmov.s  @r0+,fr4
    fmov.s  @r0+,fr5
    fadd    fr3,fr0
    fadd    fr4,fr1
    fadd    fr5,fr2
    fmul    fr14,fr0
    fmul    fr14,fr1
    fmul    fr14,fr2                ! the middle
    mov     r4,r0
    add     #28,r0
    ! row 0
    fmov.s  @r0+,fr3
    fmul    fr0,fr3
    fmov.s  @r0+,fr4
    fmul    fr1,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fmul    fr2,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fadd    fr4,fr3
    fdiv    fr15,fr3
    fadd    fr3,fr8
    ! row 1
    fmov.s  @r0+,fr3
    fmul    fr0,fr3
    fmov.s  @r0+,fr4
    fmul    fr1,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fmul    fr2,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fadd    fr4,fr3
    fdiv    fr15,fr3
    fadd    fr3,fr9
    ! row 2
    fmov.s  @r0+,fr3
    fmul    fr0,fr3
    fmov.s  @r0+,fr4
    fmul    fr1,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fmul    fr2,fr4
    fadd    fr4,fr3
    fmov.s  @r0+,fr4
    fadd    fr4,fr3
    fdiv    fr15,fr3
    fadd    fr3,fr10
    mov.l   .Lcc_size,r0
    add     r0,r4
    add     #-1,r5
    bra     cc_next
    nop
cc_out:
    fmov.s  fr8,@r6
    mov     r6,r0
    add     #4,r0
    fmov.s  fr9,@r0
    add     #4,r0
    fmov.s  fr10,@r0
    fmov.s  @r15+,fr15
    rts
    fmov.s  @r15+,fr14
    .align 2
.Lcc_half:
    .long   0x3F000000
.Lcc_size:
    .long   88
