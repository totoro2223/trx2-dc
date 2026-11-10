! Hand-written SH-4 for the native rooms' batches with their colours baked
! (batcher.c, M_DrawNative; PLAN_60_FIJO.md): what the PSX does with a room's
! static light, copied, not worked out. Per vertex only the place: the matrix
! in XMTRX (loaded by the caller), the near plane, the projection and the
! sides, exactly as xform_sh4.S places a vertex (the same instructions, so
! the batch comes out bit for bit the same), and the colour from the table.
!
!   void dc_nat_place(DC_NAT_PLACE *run);
!
! Struct layout, DC_NAT_PLACE (batcher.c, M_NATPLACE):
!    0 vertices (OUTPUT_MESH_VERTEX, 28 bytes: x y z int16 at 8)
!    4 argb (one per vertex)   8 out (M_XVERT, 32 bytes)   12 count
!   16 c (M_XFORM: 0 left 4 right 8 top 12 bottom 49 sides 50 near
!      52 proj_x 56 proj_y 60 near_z 76 max_inv_z)
!
! Registers: r4 vertex (at its x), r5 colours, r6 out, r7 left, r8 c,
!   r1 r2 r3 r0 scratch, r9 the clip; fr8 proj_x fr9 proj_y fr10 near z
!   fr11 nearest 1/z, fr12-15 left right top bottom.

    .text
    .align 2
    .global _dc_nat_place

_dc_nat_place:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    mov.l   @(4,r4),r5
    mov.l   @(8,r4),r6
    mov.l   @(12,r4),r7
    mov.l   @(16,r4),r8
    mov.l   @(0,r4),r4
    add     #8,r4                   ! the first vertex's x
    mov     r8,r1
    fmov.s  @r1+,fr12               ! left
    fmov.s  @r1+,fr13               ! right
    fmov.s  @r1+,fr14               ! top
    fmov.s  @r1+,fr15               ! bottom
    mov     r8,r1
    add     #52,r1
    fmov.s  @r1+,fr8                ! proj_x
    fmov.s  @r1+,fr9                ! proj_y
    fmov.s  @r1+,fr10               ! near z
    mov     r8,r1
    add     #76,r1
    fmov.s  @r1,fr11                ! nearest 1/z so far
    tst     r7,r7
    bt      done

loop:
    mov.w   @r4,r0
    lds     r0,fpul
    float   fpul,fr0
    mov.w   @(2,r4),r0
    lds     r0,fpul
    float   fpul,fr1
    mov.w   @(4,r4),r0
    lds     r0,fpul
    float   fpul,fr2
    add     #28,r4
    fldi1   fr3
    ftrv    xmtrx,fv0
    mov     r6,r1
    add     #12,r1
    fmov.s  fr2,@-r1
    fmov.s  fr1,@-r1
    fmov.s  fr0,@-r1
    mov     #0,r9                   ! the clip
    mov     #50,r0
    mov.b   @(r0,r8),r0
    tst     r0,r0
    bt      place
    fcmp/gt fr2,fr10                ! near z > z
    bf      place
    mov     #2,r9                   ! before the near plane
    mov     r6,r1
    add     #24,r1
    mov     #0,r0
    mov.l   r0,@-r1                 ! 1/z, sy, sx: nothing, but written
    mov.l   r0,@-r1
    mov.l   r0,@-r1
    bra     placed
    nop
place:
    fmov    fr2,fr4
    fmul    fr2,fr4
    fsrra   fr4                     ! 1/z
    fmov    fr8,fr5
    fmul    fr0,fr5
    fmul    fr4,fr5                 ! sx
    fmov    fr9,fr6
    fmul    fr1,fr6
    fmul    fr4,fr6                 ! sy
    mov     r6,r1
    add     #24,r1
    fmov.s  fr4,@-r1
    fmov.s  fr6,@-r1
    fmov.s  fr5,@-r1
    fcmp/gt fr11,fr4
    bf      1f
    fmov    fr4,fr11
1:  mov     #49,r0
    mov.b   @(r0,r8),r0
    tst     r0,r0
    bt      placed
    fcmp/gt fr5,fr12                ! left > sx
    bf      1f
    mov     #4,r1
    or      r1,r9
1:  fcmp/gt fr13,fr5                ! sx > right
    bf      1f
    mov     #8,r1
    or      r1,r9
1:  fcmp/gt fr6,fr14                ! top > sy
    bf      1f
    mov     #16,r1
    or      r1,r9
1:  fcmp/gt fr15,fr6                ! sy > bottom
    bf      placed
    mov     #32,r1
    or      r1,r9
placed:
    mov.l   @r5+,r0
    mov.l   r0,@(24,r6)             ! the colour, baked
    mov.l   r9,@(28,r6)             ! the clip, nothing above it
    dt      r7
    bf/s    loop
    add     #32,r6

done:
    mov     r8,r1
    add     #76,r1
    fmov.s  fr11,@r1
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! The batch's vertices to the screen, once each, as emit_sh4.S does it per
! corner (the same instructions): x = (centre_x + sx) * scale, y likewise,
! z = M_Depth(near_z * 1/z). The map emission (DC_EMIT_MAP) then copies them.
!
!   void dc_nat_screen(DC_NAT_SCREEN *run);
!
! Struct layout, DC_NAT_SCREEN (batcher.c, M_NATSCREEN): 0 xverts 4 count
!   8 centre_x 12 centre_y 16 screen_scale 20 near_z 24 depth_lo
!   28 depth_scale
    .global _dc_nat_screen
_dc_nat_screen:
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr15,@-r15
    mov.l   @(0,r4),r5
    add     #12,r5                  ! the first xvert's sx
    mov.l   @(4,r4),r6
    add     #8,r4
    fmov.s  @r4+,fr8                ! centre x
    fmov.s  @r4+,fr9                ! centre y
    fmov.s  @r4+,fr10               ! screen scale
    fmov.s  @r4+,fr11               ! near z
    fmov.s  @r4+,fr12               ! depth lo
    fmov.s  @r4+,fr13               ! depth scale
    fldi1   fr15
    tst     r6,r6
    bt      9f
1:  fmov.s  @r5+,fr2                ! sx
    fmov.s  @r5+,fr3                ! sy
    fmov.s  @r5+,fr4                ! 1/z
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
    fmov.s  fr7,@-r5
    fmov.s  fr6,@-r5
    fmov.s  fr5,@-r5
    dt      r6
    bf/s    1b
    add     #32,r5
9:  fmov.s  @r15+,fr15
    fmov.s  @r15+,fr13
    rts
    fmov.s  @r15+,fr12

! An object's corners with their colours kept from a frame before (batcher.c,
! M_ObjCached): as dc_nat_place, but a corner with a twin before it (the
! first corner like it) is copied from that one, as dc_xform_object does.
!
!   void dc_obj_place(DC_OBJ_PLACE *run);
!
! Struct layout: as DC_NAT_PLACE, and 20 twin (uint16_t per corner).
! Registers as dc_nat_place's, and r10 twin, r11 the corner, r12 out's start.
    .global _dc_obj_place
_dc_obj_place:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    mov.l   @(4,r4),r5
    mov.l   @(8,r4),r6
    mov.l   @(12,r4),r7
    mov.l   @(16,r4),r8
    mov.l   @(20,r4),r10
    mov.l   @(0,r4),r4
    add     #8,r4                   ! the first vertex's x
    mov     r6,r12
    mov     #0,r11
    mov     r8,r1
    fmov.s  @r1+,fr12               ! left
    fmov.s  @r1+,fr13               ! right
    fmov.s  @r1+,fr14               ! top
    fmov.s  @r1+,fr15               ! bottom
    mov     r8,r1
    add     #52,r1
    fmov.s  @r1+,fr8                ! proj_x
    fmov.s  @r1+,fr9                ! proj_y
    fmov.s  @r1+,fr10               ! near z
    mov     r8,r1
    add     #76,r1
    fmov.s  @r1,fr11                ! nearest 1/z so far
    tst     r7,r7
    bt      odone

oloop:
    mov.w   @r10+,r0
    extu.w  r0,r0
    cmp/eq  r0,r11
    bt      oown
    ! A twin: the earlier corner's xvert, whole.
    shll2   r0
    shll2   r0
    shll    r0
    add     r12,r0
    mov.l   @r0+,r1
    mov.l   r1,@(0,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(4,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(8,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(12,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(16,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(20,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(24,r6)
    mov.l   @r0+,r1
    mov.l   r1,@(28,r6)
    add     #28,r4
    bra     onext
    add     #4,r5

oown:
    mov.w   @r4,r0
    lds     r0,fpul
    float   fpul,fr0
    mov.w   @(2,r4),r0
    lds     r0,fpul
    float   fpul,fr1
    mov.w   @(4,r4),r0
    lds     r0,fpul
    float   fpul,fr2
    add     #28,r4
    fldi1   fr3
    ftrv    xmtrx,fv0
    mov     r6,r1
    add     #12,r1
    fmov.s  fr2,@-r1
    fmov.s  fr1,@-r1
    fmov.s  fr0,@-r1
    mov     #0,r9                   ! the clip
    mov     #50,r0
    mov.b   @(r0,r8),r0
    tst     r0,r0
    bt      oplace
    fcmp/gt fr2,fr10                ! near z > z
    bf      oplace
    mov     #2,r9                   ! before the near plane
    mov     r6,r1
    add     #24,r1
    mov     #0,r0
    mov.l   r0,@-r1
    mov.l   r0,@-r1
    mov.l   r0,@-r1
    bra     oplaced
    nop
oplace:
    fmov    fr2,fr4
    fmul    fr2,fr4
    fsrra   fr4                     ! 1/z
    fmov    fr8,fr5
    fmul    fr0,fr5
    fmul    fr4,fr5                 ! sx
    fmov    fr9,fr6
    fmul    fr1,fr6
    fmul    fr4,fr6                 ! sy
    mov     r6,r1
    add     #24,r1
    fmov.s  fr4,@-r1
    fmov.s  fr6,@-r1
    fmov.s  fr5,@-r1
    fcmp/gt fr11,fr4
    bf      1f
    fmov    fr4,fr11
1:  mov     #49,r0
    mov.b   @(r0,r8),r0
    tst     r0,r0
    bt      oplaced
    fcmp/gt fr5,fr12                ! left > sx
    bf      1f
    mov     #4,r1
    or      r1,r9
1:  fcmp/gt fr13,fr5                ! sx > right
    bf      1f
    mov     #8,r1
    or      r1,r9
1:  fcmp/gt fr6,fr14                ! top > sy
    bf      1f
    mov     #16,r1
    or      r1,r9
1:  fcmp/gt fr15,fr6                ! sy > bottom
    bf      oplaced
    mov     #32,r1
    or      r1,r9
oplaced:
    mov.l   @r5+,r0
    mov.l   r0,@(24,r6)             ! the colour, kept
    mov.l   r9,@(28,r6)             ! the clip, nothing above it
onext:
    add     #1,r11
    dt      r7
    bf/s    oloop
    add     #32,r6

odone:
    mov     r8,r1
    add     #76,r1
    fmov.s  fr11,@r1
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
