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
! store queue's distance from the list (0: the cache), then past what a
! 60-byte reach gives (through r0): 64 the face lights, 68 how many, 72 the
! colours without them (eargb), 76 how far this face turns from them
! (faceaway, 0 to 256). With face lights the faces go the *_l ways below.
!
! Struct layout, DC_EMIT_RUN (batcher.c):
!    0 centre_x 4 centre_y 8 screen_scale 12 near_z 16 depth_lo 20 depth_scale
!   24 uv_scale 28 one 32 indices 36 count 40 xverts 44 uv 48 page 52 oargb
!   56 out 60 space 64 page_id 68 cull 72 sargb 76 cel_out | 80 used
!   84 written 88 polys 92 cel_end | 96 cel_dark 100 sq_delta
!  104 face_lights 108 face_light_count 112 eargb

! Probe builds count the faces left out (g_DC_EmitCull[\idx]); r0, r1 are free.
    .macro COUNT idx, pool
#if defined(TRX_DC_PROBES)
    mov.l   \pool,r0
    mov.l   @(\idx*4,r0),r1
    add     #1,r1
    mov.l   r1,@(\idx*4,r0)
#endif
    .endm

! DC_EMIT_MAP (emit_map_sh4.S, the native rooms' batches, batcher.c
! M_DrawNative): xverts is per corner the byte offset of its vertex's sx in the
! batch buffer, whose address comes in sargb (stack slot 32); there are no cel
! copies, and the fog colour lies in the vertex (bit 7 of its clip byte says
! fogged, the colour in the three bytes above), oargb only saying there is one.
#if defined(DC_EMIT_MAP)
#define EMIT_ENTRY _dc_emit_run_map
#else
#define EMIT_ENTRY _dc_emit_run
#endif

    .text
    .align 2
    .global EMIT_ENTRY

! The xvert of the corner at \off in the indices, to its sx, into slot \slot.
    .macro LOAD_CORNER off, slot
    mov.w   @(\off,r6),r0
    extu.w  r0,r0
    shll2   r0
    shll2   r0
    shll    r0
    add     r8,r0                   ! r8 is on 12: the vertex's sx
    mov.l   r0,@(\slot,r15)
    .endm

! LOAD_CORNER and CLIP_CORNER in one: the xvert is still in r0, so the clip
! byte is read from it with no trip back through the slot.
    .macro LOAD_CLIP off, slot, first
    mov.w   @(\off,r6),r0
    extu.w  r0,r0
#if defined(DC_EMIT_MAP)
    add     r0,r0
    mov.w   @(r0,r8),r0             ! the offset of its sx in the batch
    extu.w  r0,r0
    mov.l   @(32,r15),r1
    add     r1,r0
#else
    shll2   r0
    shll2   r0
    shll    r0
    add     r8,r0
#endif
    mov.l   r0,@(\slot,r15)
    mov     r0,r5
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
    .macro EMIT_VERTEX_BODY off, slot, flag, adv, lit=0
    mov.w   @(\off,r6),r0
    extu.w  r0,r1                   ! r1 = corner
    mov     r1,r0
    shll2   r0                      ! r0 = corner * 4
    mov.l   @(r0,r9),r4             ! u in the low half, v in the high one
    extu.w  r4,r14
    lds     r14,fpul
    float   fpul,fr0
    fmul    fr14,fr0                ! u
    shlr16  r4
    lds     r4,fpul
    float   fpul,fr1
    fmul    fr14,fr1                ! v
    ! the offset colour: the table's, or the fog's black where fogged
    tst     r13,r13
    bf      8f
#if defined(DC_EMIT_MAP)
    ! A native batch with no fog colour has no vertex in the fog either (the
    ! PowerVR's table fogs it, or nothing does): none.
    bra     9f
    mov     #0,r14
#endif
    mov.l   @(\slot,r15),r5
    mov     #16,r0
    mov.b   @(r0,r5),r0
    tst     #0x40,r0
    movt    r14
    add     #-1,r14
    shll16  r14
    bra     9f
    shll8   r14
8:
#if defined(DC_EMIT_MAP)
    mov.l   @(\slot,r15),r5
    mov     #16,r0
    mov.l   @(r0,r5),r14            ! clip, then the fog colour above it
    mov     r14,r0
    tst     #0x80,r0                ! T: not fogged
    shlr8   r14
    movt    r0
    add     #-1,r0                  ! 0 not fogged, -1 fogged
    and     r0,r14
    shll16  r0
    shll8   r0
    or      r0,r14                  ! alpha 255 where fogged
#else
    mov     r1,r0
    shll2   r0
    mov.l   @(r0,r13),r14
#endif
9:
    mov.l   @(\slot,r15),r5
    fmov.s  @r5+,fr2                ! sx
    fmov.s  @r5+,fr3                ! sy
    fmov.s  @r5+,fr4                ! 1/z
    mov.l   @r5,r1                  ! the colour
    .if \lit
    ! a face turned from the lights: its colour towards the one without them
    mov     #76,r0
    mov.l   @(r0,r15),r5
    tst     r5,r5
    bt      4f
    mov     #72,r0
    mov.l   @(r0,r15),r4
    mov.w   @(\off,r6),r0
    extu.w  r0,r0
    shll2   r0
    mov.l   @(r0,r4),r4
    bsr     lerp_argb
    nop
4:
    .endif
#if defined(DC_EMIT_MAP)
    ! On the screen already, once per vertex (nat_sh4.S, dc_nat_screen):
    ! stored as they are.
#define EMIT_X fr2
#define EMIT_Y fr3
#define EMIT_Z fr4
#else
#define EMIT_X fr5
#define EMIT_Y fr6
#define EMIT_Z fr7
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
#endif
    add     #\adv,r11
    mov.l   r14,@-r11
    mov.l   r1,@-r11
    fmov.s  fr1,@-r11
    fmov.s  fr0,@-r11
    fmov.s  EMIT_Z,@-r11
    fmov.s  EMIT_Y,@-r11
    fmov.s  EMIT_X,@-r11
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
    .macro CEL_FACE n, o0, o1, o2, o3, lit=0
    mov.l   @(36,r15),r5
    tst     r5,r5
    bt      9f
    .if \lit
    mov     #76,r0
    mov.l   @(r0,r15),r5
    tst     r5,r5
    bt      7f
    bra     9f
    nop
7:
    .endif
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

EMIT_ENTRY:
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
    fmov.s  fr8,@-r15
    fmov.s  fr9,@-r15
    fmov.s  fr10,@-r15
    fmov.s  fr11,@-r15
    add     #-80,r15
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
#if !defined(DC_EMIT_MAP)
    add     #12,r8                  ! the xverts, at their sx
#endif
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
    mov.l   @(24,r5),r1             ! the face lights, at 104
    mov     #64,r0
    mov.l   r1,@(r0,r15)
    mov.l   @(28,r5),r1             ! how many
    mov     #68,r0
    mov.l   r1,@(r0,r15)
    mov.l   @(32,r5),r1             ! eargb
    mov     #72,r0
    mov.l   r1,@(r0,r15)
    mov     #0,r1
    mov     #76,r0
    mov.l   r1,@(r0,r15)
    mov     #-32,r2
    shll16  r2
    shll8   r2                      ! 0xE0000000
    mov     #-16,r3
    shll16  r3
    shll8   r3                      ! 0xF0000000
    mov     #0,r1
    mov.l   r1,@(4,r15)

    bra     loop                    ! the setup's end; the exits below are
    nop                             ! short hops for the not-taken tests
done_t:
    bra     done
    nop
tri_t:
    bra     tri
    nop
loop:
    mov     #3,r0
    cmp/ge  r0,r7                   ! three corners left?
    bf      done_t
    mov.w   @r6,r0
    extu.w  r0,r0
    mov.b   @(r0,r10),r1            ! the page of its first corner
    mov.l   @(12,r15),r5
    cmp/eq  r5,r1
    bf      done_t                  ! the page changed
    mov     #6,r0
    cmp/ge  r0,r7
    bf      tri_t                   ! under six corners: a triangle
    mov.w   @(6,r6),r0
    mov     r0,r1
    mov.w   @(4,r6),r0
    cmp/eq  r0,r1                   ! t[3] == t[2]
    bf      tri_t
    mov.w   @(8,r6),r0
    mov     r0,r1
    mov.w   @(2,r6),r0
    cmp/eq  r0,r1                   ! t[4] == t[1]
    bf      tri_t
    mov.w   @(6,r6),r0
    extu.w  r0,r0
    mov.b   @(r0,r10),r1
    mov.l   @(12,r15),r5
    cmp/eq  r5,r1                   ! the second half on the same page
    bf      tri_t
! ---- a quad: a, b, c, d = t0[0], t0[1], t0[2], t1[2]
    mov     #64,r0
    shll    r0                      ! 128 bytes
    cmp/ge  r0,r12
    bf      done_t
    LOAD_CLIP 0, 16, 1
    LOAD_CLIP 2, 20, 0
    LOAD_CLIP 4, 24, 0
    LOAD_CLIP 10, 28, 0
    mov     r4,r0
    tst     #63,r0                  ! all past one side: out
    bf      cull_clip_q
    mov     r14,r0
    tst     #2,r0                   ! one before the near plane: the C code
    bf      done_t
    mov.l   @(8,r15),r1
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
    COUNT   1, .Lcull_q
skip_quad:
    add     #12,r6
    add     #-6,r7
    bra     loop
    nop
cull_clip_q:
    COUNT   0, .Lcull_q
    bra     skip_quad
    nop
emit_quad:
    mov     #68,r0
    mov.l   @(r0,r15),r1
    tst     r1,r1
    bt      1f
    bra     emit_quad_l
    nop
1:
    EMIT_VERTEX_BODY 0, 16, r2, 32
    EMIT_VERTEX_BODY 2, 20, r2, 64
    EMIT_VERTEX_BODY 4, 24, r2, 64
    EMIT_VERTEX_BODY 10, 28, r3, 64
    add     #32,r11
    CEL_FACE 4, 0, 2, 4, 10
quad_tail:
    add     #-128,r12
    mov.l   @(4,r15),r1
    add     #1,r1
    mov.l   r1,@(4,r15)
    add     #12,r6
    add     #-6,r7
    bra     loop
    nop

    .align 2
.Lcull_q:
    .long   _g_DC_EmitCull
done_t2:
    bra     done
    nop
! ---- a triangle
tri:
    mov     #96,r0
    cmp/ge  r0,r12
    bf      done_t2
    LOAD_CLIP 0, 16, 1
    LOAD_CLIP 2, 20, 0
    LOAD_CLIP 4, 24, 0
    mov     r4,r0
    tst     #63,r0
    bf      cull_clip_t
    mov     r14,r0
    tst     #2,r0
    bf      done_t2
    mov.l   @(8,r15),r1
    tst     r1,r1
    bt      emit_tri
    FRONT_FACING 16, 20, 24, r4
    tst     r4,r4
    bf      emit_tri
    COUNT   1, .Lcull_t
skip_tri:
    add     #6,r6
    add     #-3,r7
    bra     loop
    nop
cull_clip_t:
    COUNT   0, .Lcull_t
    bra     skip_tri
    nop
emit_tri:
    mov     #68,r0
    mov.l   @(r0,r15),r1
    tst     r1,r1
    bt      1f
    bra     emit_tri_l
    nop
1:
    EMIT_VERTEX_BODY 0, 16, r2, 32
    EMIT_VERTEX_BODY 2, 20, r2, 64
    EMIT_VERTEX_BODY 4, 24, r3, 64
    add     #32,r11
    CEL_FACE 3, 0, 2, 4, 4
tri_tail:
    add     #-96,r12
    mov.l   @(4,r15),r1
    add     #1,r1
    mov.l   r1,@(4,r15)
    add     #6,r6
    add     #-3,r7
    bra     loop
    nop

    .align 2
.Lcull_t:
    .long   _g_DC_EmitCull
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
    add     #80,r15
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
    mov.l   @r15+,r8
    rts
    lds.l   @r15+,pr

! ---- the faces with face lights (a room's cel shading, M_FaceTurnedAway)
emit_quad_l:
    bsr     faceaway
    nop
    mov     r0,r1
    mov     #76,r0
    mov.l   r1,@(r0,r15)
    EMIT_VERTEX_BODY 0, 16, r2, 32, 1
    EMIT_VERTEX_BODY 2, 20, r2, 64, 1
    EMIT_VERTEX_BODY 4, 24, r2, 64, 1
    EMIT_VERTEX_BODY 10, 28, r3, 64, 1
    add     #32,r11
    CEL_FACE 4, 0, 2, 4, 10, 1
    bra     quad_tail
    nop
emit_tri_l:
    bsr     faceaway
    nop
    mov     r0,r1
    mov     #76,r0
    mov.l   r1,@(r0,r15)
    EMIT_VERTEX_BODY 0, 16, r2, 32, 1
    EMIT_VERTEX_BODY 2, 20, r2, 64, 1
    EMIT_VERTEX_BODY 4, 24, r3, 64, 1
    add     #32,r11
    CEL_FACE 3, 0, 2, 4, 4, 1
    bra     tri_tail
    nop

! r1 (a colour) towards r4 by the face's away (slot 76 of the caller's frame,
! 0 to 256), a channel at a time as M_LerpARGB: a + (((b - a) * t + 128) >> 8).
! The result is in r1; r0, r4, r5 are lost, r2, r3, r14 kept.
    .macro LERP_CHANNEL shift
    mov     r1,r2
    mov     r4,r3
    .if \shift >= 16
    shlr16  r2
    shlr16  r3
    .endif
    .if (\shift & 8)
    shlr8   r2
    shlr8   r3
    .endif
    extu.b  r2,r2                   ! a
    extu.b  r3,r3                   ! b
    sub     r2,r3
    muls.w  r5,r3                   ! (b - a) * t
    sts     macl,r3
    add     #127,r3
    add     #1,r3                   ! + 128
    mov     #-8,r0
    shad    r0,r3                   ! >> 8, arithmetic
    add     r2,r3
    .if \shift >= 16
    shll16  r3
    .endif
    .if (\shift & 8)
    shll8   r3
    .endif
    or      r3,r14
    .endm

lerp_argb:
    mov.l   r2,@-r15
    mov.l   r3,@-r15
    mov.l   r14,@-r15
    mov     #88,r0                  ! slot 76, and the three pushed
    mov.l   @(r0,r15),r5
    mov     #0,r14
    LERP_CHANNEL 0
    LERP_CHANNEL 8
    LERP_CHANNEL 16
    LERP_CHANNEL 24
    mov     r14,r1
    mov.l   @r15+,r14
    mov.l   @r15+,r3
    rts
    mov.l   @r15+,r2

! The squared distance of the light (fr8..fr10) from the vertex at \base (its
! x, y, z) into \into; fr11, fr7 and r0 are lost.
    .macro DIST_SQ base, into
    fmov.s  @\base,fr7
    fmov    fr8,\into
    fsub    fr7,\into
    fmul    \into,\into
    mov     #4,r0
    fmov.s  @(r0,\base),fr7
    fmov    fr9,fr11
    fsub    fr7,fr11
    fmul    fr11,fr11
    fadd    fr11,\into
    mov     #8,r0
    fmov.s  @(r0,\base),fr7
    fmov    fr10,fr11
    fsub    fr7,fr11
    fmul    fr11,fr11
    fadd    fr11,\into
    .endm

! How far the face in slots 16, 20, 24 of the caller's frame turns from the
! face lights, 0 to 256, in r0: M_FaceTurnedAway. All but r2, r3 are free.
faceaway:
    mov.l   @(16,r15),r4            ! a, at its sx
    mov.l   @(20,r15),r5            ! b
    mov.l   @(24,r15),r14           ! c
    add     #-12,r4                 ! at their x
    add     #-12,r5
    add     #-12,r14
    mov     r4,r1
    fmov.s  fr8,@-r15
    fmov.s  fr9,@-r15
    fmov.s  fr10,@-r15
    fmov.s  fr11,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    add     #-16,r15                ! 0 away, 4 1/|n|, 8 the distance squared
    fmov.s  @r1+,fr0                ! a
    fmov.s  @r1+,fr1
    fmov.s  @r1,fr2
    mov     r5,r1
    fmov.s  @r1+,fr3
    fmov.s  @r1+,fr4
    fmov.s  @r1,fr5
    fsub    fr0,fr3                 ! e1 = b - a
    fsub    fr1,fr4
    fsub    fr2,fr5
    mov     r14,r1
    fmov.s  @r1+,fr6
    fmov.s  @r1+,fr7
    fmov.s  @r1,fr8
    fsub    fr0,fr6                 ! e2 = c - a
    fsub    fr1,fr7
    fsub    fr2,fr8
    fmov    fr4,fr9                 ! n0 = e1y * e2z - e1z * e2y
    fmul    fr8,fr9
    fmov    fr5,fr10
    fmul    fr7,fr10
    fsub    fr10,fr9
    fmov    fr5,fr10                ! n1 = e1z * e2x - e1x * e2z
    fmul    fr6,fr10
    fmov    fr3,fr11
    fmul    fr8,fr11
    fsub    fr11,fr10
    fmov    fr3,fr11                ! n2 = e1x * e2y - e1y * e2x
    fmul    fr7,fr11
    fmov    fr4,fr12
    fmul    fr6,fr12
    fsub    fr12,fr11
    fmov    fr9,fr3                 ! n
    fmov    fr10,fr4
    fmov    fr11,fr5
    fmov    fr3,fr6                 ! n . a (eye is its minus)
    fmul    fr0,fr6
    fmov    fr4,fr7
    fmul    fr1,fr7
    fadd    fr7,fr6
    fmov    fr5,fr7
    fmul    fr2,fr7
    fadd    fr7,fr6
    fmov    fr3,fr7                 ! |n|^2
    fmul    fr3,fr7
    fmov    fr4,fr8
    fmul    fr4,fr8
    fadd    fr8,fr7
    fmov    fr5,fr8
    fmul    fr5,fr8
    fadd    fr8,fr7
    fldi0   fr8
    fcmp/gt fr8,fr7
    bt      1f
    bra     fa_none
    nop
1:  fsrra   fr7                     ! 1 / |n|
    fcmp/gt fr8,fr6                 ! eye < 0: the other way round
    bf      2f
    fneg    fr7
2:  mov     #4,r0
    fmov.s  fr7,@(r0,r15)
    fldi1   fr6
    fadd    fr6,fr6                 ! away = 2: no light yet
    mov     #0,r0
    fmov.s  fr6,@(r0,r15)
    mov     #112,r0                 ! the caller's slot 64, under 48 more
    mov.l   @(r0,r15),r4            ! the lights
    mov     #116,r0
    mov.l   @(r0,r15),r1            ! how many
fa_loop:
    fmov.s  @r4+,fr8                ! the light, in view space
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    fmov    fr8,fr12                ! d = l - a
    fsub    fr0,fr12
    fmov    fr9,fr13
    fsub    fr1,fr13
    fmov    fr10,fr14
    fsub    fr2,fr14
    fmov    fr12,fr15               ! d . d
    fmul    fr12,fr15
    fmov    fr13,fr11
    fmul    fr13,fr11
    fadd    fr11,fr15
    fmov    fr14,fr11
    fmul    fr14,fr11
    fadd    fr11,fr15
    mov     #8,r0
    fmov.s  fr15,@(r0,r15)          ! kept for the cosine
    DIST_SQ r5, fr6
    fcmp/gt fr6,fr15
    bf      3f
    fmov    fr6,fr15
3:  DIST_SQ r14, fr6
    fcmp/gt fr6,fr15
    bf      3f
    fmov    fr6,fr15
3:  fmov.s  @r4,fr7                 ! the radius squared
    fcmp/gt fr7,fr15
    bf      4f
    bra     fa_next
    nop
4:  mov     #8,r0
    fmov.s  @(r0,r15),fr15
    fldi0   fr7
    fcmp/gt fr7,fr15
    bt      5f
    fldi1   fr6                     ! on the face: it faces the light
    bra     fa_cos
    nop
5:  fmov    fr3,fr6                 ! n . d
    fmul    fr12,fr6
    fmov    fr4,fr7
    fmul    fr13,fr7
    fadd    fr7,fr6
    fmov    fr5,fr7
    fmul    fr14,fr7
    fadd    fr7,fr6
    mov     #4,r0
    fmov.s  @(r0,r15),fr7
    fmul    fr7,fr6                 ! * 1/|n|
    fmov    fr15,fr7
    fsrra   fr7
    fmul    fr7,fr6                 ! * 1/|d|: the cosine
fa_cos:
    mov     #63,r0                  ! 0.5 - the cosine, held to 0..1
    shll16  r0
    shll8   r0
    lds     r0,fpul
    fsts    fpul,fr7
    fsub    fr6,fr7
    fldi0   fr6
    fcmp/gt fr7,fr6
    bf      6f
    fmov    fr6,fr7
    bra     7f
    nop
6:  fldi1   fr6
    fcmp/gt fr6,fr7
    bf      7f
    fmov    fr6,fr7
7:  mov     #0,r0
    fmov.s  @(r0,r15),fr6
    fcmp/gt fr7,fr6                 ! away = min(away, it)
    bf      fa_next
    fmov.s  fr7,@(r0,r15)
fa_next:
    add     #4,r4
    dt      r1
    bt      8f
    bra     fa_loop
    nop
8:  mov     #0,r0
    fmov.s  @(r0,r15),fr6
    fldi1   fr7
    fcmp/gt fr7,fr6                 ! still 2: no light reaches the face
    bf      9f
fa_none:
    mov     #0,r0
    bra     fa_out
    nop
9:  fadd    fr7,fr7                 ! 256
    fadd    fr7,fr7
    fadd    fr7,fr7
    fadd    fr7,fr7
    fadd    fr7,fr7
    fadd    fr7,fr7
    fadd    fr7,fr7
    fadd    fr7,fr7
    fmul    fr7,fr6
    ftrc    fr6,fpul
    sts     fpul,r0
fa_out:
    add     #16,r15
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    fmov.s  @r15+,fr11
    fmov.s  @r15+,fr10
    fmov.s  @r15+,fr9
    fmov.s  @r15+,fr8
    rts
    nop
