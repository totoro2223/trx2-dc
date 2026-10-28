! Hand-written SH-4 for the batcher's face emission (batcher.c, M_EmitRunFast).
!
!   void dc_emit_run(DC_EMIT_RUN *run);
!
! Takes a run of faces that share a texture page, as M_EmitIndices walks them
! (quads as M_PairQuads laid them out, then triangles), and writes the ones
! that go out whole straight into the polygon list: projected, in front of the
! near plane, turned to the camera. A quad whose corners all lie past one side
! is left out, as M_TryEmitQuad does, and so is a triangle (M_EmitTriangle).
! It stops, with what it did so far, at the first face that is not so plain
! (one that crosses the near plane, or a quad folded so that one half faces
! away), at a change of page, and when there is no room left: the C code goes
! on from there, face by face, as ever. The result of every plain face is the
! one M_Project makes of it.
!
! Registers (the C ABI's: r8-r14 and fr12-fr15 are the caller's, and are
! kept; the others are free):
!   r2  PVR_CMD_VERTEX            r3  PVR_CMD_VERTEX_EOL
!   r6  indices (uint16_t *)      r7  indices left
!   r8  xverts                    r9  texture coordinates (uint16_t pairs)
!   r10 page of each corner       r11 where the next vertex goes
!   r12 room left, in bytes       r13 offset colours, or 0
!   r4, r5, r14, r0, r1: scratch
!   fr8  centre x     fr9  centre y   fr10 screen scale   fr11 near z
!   fr12 depth lo     fr13 depth scale fr14 1/65535       fr15 1.0
! The frame on the stack: 0 the run, 4 polygons made, 8 cull, 12 the page, 32 the
! cel colours, 36 the cel cursor, 40..52 the shaded colours of a face,
! 16..28 the four corners' xverts, advanced to their sx, 56 cel_dark, 60 the
! store queue's distance from the list (0: the cache).
!
! Struct layout, DC_EMIT_RUN (batcher.c):
!    0 centre_x 4 centre_y 8 screen_scale 12 near_z 16 depth_lo 20 depth_scale
!   24 uv_scale 28 one 32 indices 36 count 40 xverts 44 uv 48 page 52 oargb
!   56 out 60 space 64 page_id 68 cull 72 sargb 76 cel_out | 80 used
!   84 written 88 polys 92 cel_end | 96 cel_dark 100 sq_delta

    .text
    .align 2
    .global _dc_emit_run

! The xvert of the corner at \off in the indices, to its sx, into slot \slot.
    .macro LOAD_CORNER off, slot
    mov.w   @(\off,r6),r0
    extu.w  r0,r0
    shll2   r0
    shll2   r0
    shll    r0
    add     r8,r0
    add     #12,r0
    mov.l   r0,@(\slot,r15)
    .endm

! Folds the clip of the corner in slot \slot (at sx + 16) into r4 (and) and
! r14 (or); \first starts them.
    .macro CLIP_CORNER slot, first
    mov.l   @(\slot,r15),r5
    mov     #16,r0
    mov.b   @(r0,r5),r1
    .if \first
    mov     r1,r4
    mov     r1,r14
    .else
    and     r1,r4
    or      r1,r14
    .endif
    .endm

! One vertex: the corner at \off in the indices, its xverts in slot \slot,
! with the flag register \flag; r11 first moves by \adv (32 for the first of
! a face, 64 after, since the stores run backwards from its end) and ends on
! the vertex's start.
    .macro EMIT_VERTEX_BODY off, slot, flag, adv
    mov.w   @(\off,r6),r0
    extu.w  r0,r1                   ! r1 = corner
    mov     r1,r0
    shll2   r0                      ! r0 = corner * 4
    mov.w   @(r0,r9),r14
    extu.w  r14,r14
    lds     r14,fpul
    float   fpul,fr0
    fmul    fr14,fr0                ! u
    add     #2,r0
    mov.w   @(r0,r9),r14
    extu.w  r14,r14
    lds     r14,fpul
    float   fpul,fr1
    fmul    fr14,fr1                ! v
    ! the offset colour: the table's, or the fog's black where fogged
    tst     r13,r13
    bf      8f
    mov.l   @(\slot,r15),r5
    mov     #16,r0
    mov.b   @(r0,r5),r0
    tst     #0x40,r0
    movt    r14
    add     #-1,r14
    shll16  r14
    bra     9f
    shll8   r14
8:  mov     r1,r0
    shll2   r0
    mov.l   @(r0,r13),r14
9:
    mov.l   @(\slot,r15),r5
    fmov.s  @r5+,fr2                ! sx
    fmov.s  @r5+,fr3                ! sy
    fmov.s  @r5+,fr4                ! 1/z
    mov.l   @r5,r1                  ! the colour
    fmov    fr8,fr5
    fadd    fr2,fr5
    fmul    fr10,fr5                ! x = (centre_x + sx) * scale
    fmov    fr9,fr6
    fadd    fr3,fr6
    fmul    fr10,fr6                ! y
    fmov    fr11,fr7
    fmul    fr4,fr7                 ! z = M_Depth(near_z * 1/z)
    fcmp/gt fr15,fr7
    bf      6f
    fmov    fr15,fr7
6:  fcmp/gt fr7,fr12
    bf      7f
    fmov    fr12,fr7
7:  fmul    fr13,fr7
    add     #\adv,r11
    mov.l   r14,@-r11
    mov.l   r1,@-r11
    fmov.s  fr1,@-r11
    fmov.s  fr0,@-r11
    fmov.s  fr7,@-r11
    fmov.s  fr6,@-r11
    fmov.s  fr5,@-r11
    mov.l   \flag,@-r11
#if defined(TRX_DC_SQ_VERTICES)
    ! Through a store queue (r11 its address): the real line out of the cache,
    ! then the queue's 32 bytes to the RAM.
    mov.l   @(60,r15),r0
    tst     r0,r0
    bt      5f
    mov     r11,r5
    sub     r0,r5
    ocbi    @r5
    pref    @r11
5:
#endif
    .endm

! Whether the face of the corners in slots \a, \b, \c (as M_IsBackFacing) turns
! to the camera: movt into \into (1 for the front).
    .macro FRONT_FACING a, b, c, into
    mov.l   @(\a,r15),r5
    fmov.s  @r5+,fr0
    fmov.s  @r5+,fr1
    mov.l   @(\b,r15),r5
    fmov.s  @r5+,fr2
    fmov.s  @r5+,fr3
    mov.l   @(\c,r15),r5
    fmov.s  @r5+,fr4
    fmov.s  @r5+,fr5
    fsub    fr0,fr2                 ! b - a
    fsub    fr1,fr5                 ! c - a
    fmul    fr5,fr2
    fsub    fr0,fr4
    fsub    fr1,fr3
    fmul    fr3,fr4
    fsub    fr4,fr2
    fldi0   fr6
    fcmp/gt fr2,fr6                 ! 0 > cross: the front
    movt    \into
    .endm

! One vertex of a face just written, copied for the cel shading with its
! shaded colour (the slot \slot) and no offset colour; r1 reads it, r5 writes.
    .macro COPY_VERTEX slot
    mov.l   @(0,r1),r0
    mov.l   r0,@(0,r5)
    mov.l   @(4,r1),r0
    mov.l   r0,@(4,r5)
    mov.l   @(8,r1),r0
    mov.l   r0,@(8,r5)
    mov.l   @(12,r1),r0
    mov.l   r0,@(12,r5)
    mov.l   @(16,r1),r0
    mov.l   r0,@(16,r5)
    mov.l   @(20,r1),r0
    mov.l   r0,@(20,r5)
    mov.l   @(\slot,r15),r0
    mov.l   r0,@(24,r5)
    mov     #0,r0
    mov.l   r0,@(28,r5)
    add     #32,r1
    add     #32,r5
    .endm

! The cel shading's copy of the face of \n vertices just written (r11 is past
! them), for the corners at \o0.. in the indices: kept where the lit side of
! the face reaches half the alpha, as M_KeepCel does after M_EmitFront; with
! cel_dark (a room's), not where all its corners do (M_CelKeeps).
    .macro CEL_FACE n, o0, o1, o2, o3
    mov.l   @(36,r15),r5
    tst     r5,r5
    bt      9f
    mov.l   @(32,r15),r14
    mov     #0,r4
    mov.w   @(\o0,r6),r0
    extu.w  r0,r0
    shll2   r0
    mov.l   @(r0,r14),r1
    or      r1,r4
    mov.l   r1,@(40,r15)
    mov.w   @(\o1,r6),r0
    extu.w  r0,r0
    shll2   r0
    mov.l   @(r0,r14),r1
    or      r1,r4
    mov.l   r1,@(44,r15)
    mov.w   @(\o2,r6),r0
    extu.w  r0,r0
    shll2   r0
    mov.l   @(r0,r14),r1
    or      r1,r4
    mov.l   r1,@(48,r15)
    .if \n == 4
    mov.w   @(\o3,r6),r0
    extu.w  r0,r0
    shll2   r0
    mov.l   @(r0,r14),r1
    or      r1,r4
    mov.l   r1,@(52,r15)
    .endif
    cmp/pz  r4                      ! T = no alpha has its top bit
    bf      8f
    bra     9f
    nop
8:  mov.l   @(56,r15),r0
    tst     r0,r0
    bt      0f                      ! not a room's: kept
    mov.l   @(40,r15),r0
    mov.l   @(44,r15),r1
    and     r1,r0
    mov.l   @(48,r15),r1
    and     r1,r0
    .if \n == 4
    mov.l   @(52,r15),r1
    and     r1,r0
    .endif
    cmp/pz  r0                      ! T = some corner out of the shade
    bt      0f
    bra     9f                      ! all in it: gone out dark already
    nop
0:  mov     r11,r1
    add     #-(32 * \n),r1
    COPY_VERTEX 40
    COPY_VERTEX 44
    COPY_VERTEX 48
    .if \n == 4
    COPY_VERTEX 52
    .endif
    mov.l   r5,@(36,r15)
9:
    .endm

_dc_emit_run:
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
    add     #-64,r15
    mov.l   r4,@(0,r15)
    mov     r4,r5
    fmov.s  @r5+,fr8
    fmov.s  @r5+,fr9
    fmov.s  @r5+,fr10
    fmov.s  @r5+,fr11
    fmov.s  @r5+,fr12
    fmov.s  @r5+,fr13
    fmov.s  @r5+,fr14
    fmov.s  @r5+,fr15
    mov.l   @r5+,r6
    mov.l   @r5+,r7
    mov.l   @r5+,r8
    mov.l   @r5+,r9
    mov.l   @r5+,r10
    mov.l   @r5+,r13
    mov.l   @r5+,r11
    mov.l   @r5+,r12
    mov.l   @r5+,r1
    mov.l   r1,@(12,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(8,r15)
    mov.l   @r5+,r1
    mov.l   r1,@(32,r15)            ! the cel colours
    mov.l   @r5+,r1
    mov.l   r1,@(36,r15)            ! where the cel copies go, or 0
    mov.l   @(16,r5),r1             ! cel_dark, at 96
    mov.l   r1,@(56,r15)
    mov.l   @(20,r5),r1             ! sq_delta, at 100
    mov.l   r1,@(60,r15)
    mov     #-32,r2
    shll16  r2
    shll8   r2                      ! 0xE0000000
    mov     #-16,r3
    shll16  r3
    shll8   r3                      ! 0xF0000000
    mov     #0,r1
    mov.l   r1,@(4,r15)

loop:
    mov     #3,r0
    cmp/ge  r0,r7                   ! three corners left?
    bt      1f
    bra     done
    nop
1:  mov.w   @r6,r0
    extu.w  r0,r0
    mov.b   @(r0,r10),r1            ! the page of its first corner
    mov.l   @(12,r15),r5
    cmp/eq  r5,r1
    bt      2f
    bra     done                    ! the page changed
    nop
2:  mov     #6,r0
    cmp/ge  r0,r7
    bt      2f
    bra     tri                     ! under six corners: a triangle
    nop
2:  mov.w   @(6,r6),r0
    mov     r0,r1
    mov.w   @(4,r6),r0
    cmp/eq  r0,r1                   ! t[3] == t[2]
    bt      2f
    bra     tri
    nop
2:  mov.w   @(8,r6),r0
    mov     r0,r1
    mov.w   @(2,r6),r0
    cmp/eq  r0,r1                   ! t[4] == t[1]
    bt      2f
    bra     tri
    nop
2:  mov.w   @(6,r6),r0
    extu.w  r0,r0
    mov.b   @(r0,r10),r1
    mov.l   @(12,r15),r5
    cmp/eq  r5,r1                   ! the second half on the same page
    bt      2f
    bra     tri
    nop
2:
! ---- a quad: a, b, c, d = t0[0], t0[1], t0[2], t1[2]
    mov     #64,r0
    shll    r0                      ! 128 bytes
    cmp/ge  r0,r12
    bt      3f
    bra     done
    nop
3:  LOAD_CORNER 0, 16
    LOAD_CORNER 2, 20
    LOAD_CORNER 4, 24
    LOAD_CORNER 10, 28
    CLIP_CORNER 16, 1
    CLIP_CORNER 20, 0
    CLIP_CORNER 24, 0
    CLIP_CORNER 28, 0
    mov     r4,r0
    tst     #63,r0                  ! all past one side: out
    bt      4f
    bra     skip_quad
    nop
4:  mov     r14,r0
    tst     #2,r0                   ! one before the near plane: the C code
    bt      5f
    bra     done
    nop
5:  mov.l   @(8,r15),r1
    tst     r1,r1
    bt      emit_quad               ! no culling
    FRONT_FACING 16, 20, 24, r4     ! a, b, c
    FRONT_FACING 24, 20, 28, r14    ! c, b, d
    cmp/eq  r4,r14
    bt      6f
    bra     done                    ! folded: the C code
    nop
6:  tst     r4,r4
    bf      emit_quad
skip_quad:
    add     #12,r6
    add     #-6,r7
    bra     loop
    nop
emit_quad:
    EMIT_VERTEX_BODY 0, 16, r2, 32
    EMIT_VERTEX_BODY 2, 20, r2, 64
    EMIT_VERTEX_BODY 4, 24, r2, 64
    EMIT_VERTEX_BODY 10, 28, r3, 64
    add     #32,r11
    CEL_FACE 4, 0, 2, 4, 10
    add     #-128,r12
    mov.l   @(4,r15),r1
    add     #1,r1
    mov.l   r1,@(4,r15)
    add     #12,r6
    add     #-6,r7
    bra     loop
    nop

! ---- a triangle
tri:
    mov     #96,r0
    cmp/ge  r0,r12
    bt      7f
    bra     done
    nop
7:  LOAD_CORNER 0, 16
    LOAD_CORNER 2, 20
    LOAD_CORNER 4, 24
    CLIP_CORNER 16, 1
    CLIP_CORNER 20, 0
    CLIP_CORNER 24, 0
    mov     r4,r0
    tst     #63,r0
    bt      8f
    bra     skip_tri
    nop
8:  mov     r14,r0
    tst     #2,r0
    bt      9f
    bra     done
    nop
9:  mov.l   @(8,r15),r1
    tst     r1,r1
    bt      emit_tri
    FRONT_FACING 16, 20, 24, r4
    tst     r4,r4
    bf      emit_tri
skip_tri:
    add     #6,r6
    add     #-3,r7
    bra     loop
    nop
emit_tri:
    EMIT_VERTEX_BODY 0, 16, r2, 32
    EMIT_VERTEX_BODY 2, 20, r2, 64
    EMIT_VERTEX_BODY 4, 24, r3, 64
    add     #32,r11
    CEL_FACE 3, 0, 2, 4, 4
    add     #-96,r12
    mov.l   @(4,r15),r1
    add     #1,r1
    mov.l   r1,@(4,r15)
    add     #6,r6
    add     #-3,r7
    bra     loop
    nop

done:
    mov.l   @(0,r15),r4
    mov.l   @(36,r4),r1             ! indices at the start
    sub     r7,r1                   ! used: those at the start less those left
    mov.l   @(56,r4),r5
    sub     r5,r11                  ! written
    mov.l   @(4,r15),r0
    mov.l   @(36,r15),r5            ! where the cel copies end
    add     #80,r4
    mov.l   r1,@r4
    mov.l   r11,@(4,r4)
    mov.l   r0,@(8,r4)
    mov.l   r5,@(12,r4)
    add     #64,r15
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
