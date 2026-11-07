! Hand-written SH-4 for the per-instance numbers of the batcher (batcher.c):
! an instance's world matrix and its model-view matrix.
!
!   void dc_world_matrix(const MATRIX *m, float out[12]);
!   void dc_model_view(const float view[16], const float model[16],
!                      float out[16], float *scale);
!
! dc_world_matrix: M_WorldMatrix. MATRIX is twelve int64 (rows of four); each
! becomes (float)(int32)(v >> 16) * 65536 + (float)(v & 0xFFFF), times
! 1 / (1 << W2V_SHIFT) (W2V_SHIFT is 14), as Output_MatrixToFloat does.
!
! dc_model_view: M_ModelView. out[c][r] = sum over k of view[k][r] * model[c][k]
! (GL's layout, column first), and *scale = the length of model's first three
! entries. Uses only fr0-fr11, and leaves XMTRX alone.

    .text
    .align 2
    .global _dc_world_matrix
    .global _dc_model_view

_dc_world_matrix:
    mov.l   .Lc65536,r0
    lds     r0,fpul
    fsts    fpul,fr2
    mov.l   .Lkw2v,r0
    lds     r0,fpul
    fsts    fpul,fr3
    mov     #12,r3
1:  mov.l   @r4+,r1                 ! the low word
    mov.l   @r4+,r2                 ! the high word
    shll16  r2
    mov     r1,r6
    shlr16  r6
    or      r6,r2                   ! (int32)(v >> 16)
    extu.w  r1,r1                   ! v & 0xFFFF
    lds     r2,fpul
    float   fpul,fr0
    fmul    fr2,fr0
    lds     r1,fpul
    float   fpul,fr1
    fadd    fr1,fr0
    fmul    fr3,fr0
    fmov.s  fr0,@r5
    add     #4,r5
    dt      r3
    bf      1b
    rts
    nop

_dc_model_view:
    mov     r5,r1
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fmov.s  @r1+,fr3
    fmul    fr1,fr1                 ! as the C does it: x*x, then fused y*y, z*z
    fmov    fr2,fr0
    fmac    fr0,fr2,fr1
    fmov    fr3,fr0
    fmac    fr0,fr3,fr1
    fsqrt   fr1
    fmov.s  fr1,@r7
    mov     #0,r2                   ! r: the byte offset of the row
2:  mov     r4,r1
    add     r2,r1                   ! &view[0][r]
    fmov.s  @r1,fr4
    add     #16,r1
    fmov.s  @r1,fr5
    add     #16,r1
    fmov.s  @r1,fr6
    add     #16,r1
    fmov.s  @r1,fr7
    mov     r5,r1                   ! the model's columns
    mov     r6,r3
    add     r2,r3                   ! &out[0][r]
    mov     #4,r7
3:  fmov.s  @r1+,fr8
    fmov.s  @r1+,fr9
    fmov.s  @r1+,fr10
    fmov.s  @r1+,fr11
    fmov    fr4,fr1                 ! the C's order: a0*b0, then fused a1*b1,
    fmul    fr8,fr1                 ! a2*b2, a3*b3
    fmov    fr5,fr0
    fmac    fr0,fr9,fr1
    fmov    fr6,fr0
    fmac    fr0,fr10,fr1
    fmov    fr7,fr0
    fmac    fr0,fr11,fr1
    fmov.s  fr1,@r3
    add     #16,r3
    dt      r7
    bf      3b
    add     #4,r2
    mov     #16,r0
    cmp/eq  r2,r0
    bf      2b
    rts
    nop

    .align 2
.Lc65536:
    .long   0x47800000
.Lkw2v:
    .long   0x38800000

! void dc_light_dir(const float *mv, const float *v, const float *k, float *out)
! void dc_light_dir_n(...)    the same, but v is scaled to length 1 first
!                             (when it has any length)
! void dc_light_dir_div(...)  the same as dc_light_dir, but divides by *k
!
! A light's direction from the view space into the object's: out[c] is the
! dot product of row c of the matrix (mv[c][0..2], rows of four floats) and v,
! times *k (batcher.c, M_ReadInstanceLight).
    .global _dc_light_dir
    .global _dc_light_dir_n
    .global _dc_light_dir_div
    .align 2
_dc_light_dir:
    bra     ld_common
    mov     #0,r3
_dc_light_dir_n:
    bra     ld_common
    mov     #1,r3
_dc_light_dir_div:
    mov     #2,r3
ld_common:
    mov     r5,r1
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    mov     r3,r0
    cmp/eq  #1,r0
    bf      2f
    fmov    fr4,fr11
    fmul    fr4,fr11                ! x*x, then fused y*y and z*z: the C's way
    fmov    fr5,fr0
    fmac    fr0,fr5,fr11
    fmov    fr6,fr0
    fmac    fr0,fr6,fr11            ! fr11 = the length squared
    fldi0   fr7
    fcmp/gt fr7,fr11
    bf      2f
    fsrra   fr11
    fmul    fr11,fr4
    fmul    fr11,fr5
    fmul    fr11,fr6
2:  fmov.s  @r6,fr7
    mov     #3,r2
3:  fmov.s  @r4+,fr8
    fmov.s  @r4+,fr9
    fmov.s  @r4+,fr10
    add     #4,r4
    fmov    fr8,fr11
    fmul    fr4,fr11                ! m0*v0, then fused m1*v1 and m2*v2
    fmov    fr9,fr0
    fmac    fr0,fr5,fr11
    fmov    fr10,fr0
    fmac    fr0,fr6,fr11
    mov     r3,r0
    cmp/eq  #2,r0
    bf      4f
    fdiv    fr7,fr11
    bra     5f
    nop
4:  fmul    fr7,fr11
5:  fmov.s  fr11,@r7
    add     #4,r7
    dt      r2
    bf      3b
    rts
    nop

! void dc_room_sphere(const float world[12], const float bounds[6], float out[4]);
!
! An instance's bounding sphere in the world (batcher.c, M_PrepareRoomLights):
! out[0..2] the middle of the box (lo, hi) taken through the world matrix (rows
! of four), out[3] the radius, sqrt(half squared * the matrix's x scale squared).
    .global _dc_room_sphere
    .align 2
_dc_room_sphere:
    mov     r5,r1
    fmov.s  @r1+,fr0
    fmov.s  @r1+,fr1
    fmov.s  @r1+,fr2
    fmov.s  @r1+,fr4
    fmov.s  @r1+,fr5
    fmov.s  @r1+,fr6
    mov.l   .Lhalf2,r0
    lds     r0,fpul
    fsts    fpul,fr7
    fsub    fr0,fr4
    fsub    fr1,fr5
    fsub    fr2,fr6
    fmul    fr7,fr4
    fmul    fr7,fr5
    fmul    fr7,fr6
    fadd    fr4,fr0
    fadd    fr5,fr1
    fadd    fr6,fr2
    fmul    fr4,fr4
    fmul    fr5,fr5
    fmul    fr6,fr6
    fadd    fr5,fr4
    fadd    fr6,fr4                 ! half squared
    fldi1   fr3
    mov     r4,r1
    mov     #3,r2
1:  fmov.s  @r1+,fr8
    fmov.s  @r1+,fr9
    fmov.s  @r1+,fr10
    fmov.s  @r1+,fr11
    fipr    fv0,fv8
    fmov.s  fr11,@r6
    add     #4,r6
    dt      r2
    bf      1b
    mov     r4,r1
    fmov.s  @r1,fr8
    add     #16,r1
    fmov.s  @r1,fr9
    add     #16,r1
    fmov.s  @r1,fr10
    fldi0   fr11
    fipr    fv8,fv8                 ! the x scale squared
    fmul    fr11,fr4
    fsqrt   fr4
    fmov.s  fr4,@r6
    rts
    nop
    .align 2
.Lhalf2:
    .long   0x3F000000

! void dc_scan_vertices(const OUTPUT_MESH_VERTEX *v, int32_t n, int32_t *out);
!
! What a mesh's vertices say as a whole (batcher.c, MeshBatcher_AddMesh), n at
! least 1. In: out[10] a mask, out[11] the value its flags must show for
! "plain". Out (words): 0 the flags ANDed  1 the flags ORed  2 1 if every colour
! is white (all four bytes 255)  3 1 if (flags & mask) == value for every vertex
! 4-6 unused  7-9 hi... as laid out: 4 lo x  5 lo y  6 lo z  7 hi x  8 hi y
! 9 hi z (whole units). The vertex is 28 bytes: colour at 4, x y z at 8 10 12
! (int16), flags at 22 (uint16).
    .global _dc_scan_vertices
    .align 2
_dc_scan_vertices:
    mov.w   @(8,r4),r0
    mov.l   r0,@(16,r6)
    mov.l   r0,@(28,r6)
    mov.w   @(10,r4),r0
    mov.l   r0,@(20,r6)
    mov.l   r0,@(32,r6)
    mov.w   @(12,r4),r0
    mov.l   r0,@(24,r6)
    mov.l   r0,@(36,r6)
    mov     #-1,r0
    extu.w  r0,r0
    mov.l   r0,@(0,r6)
    mov     #0,r0
    mov.l   r0,@(4,r6)
    mov     #1,r0
    mov.l   r0,@(8,r6)
    mov.l   r0,@(12,r6)
    .macro BND p, lo, hi
    mov.w   @(\p,r4),r0
    mov.l   @(\lo,r6),r2
    cmp/gt  r0,r2
    bf      1f
    mov.l   r0,@(\lo,r6)
1:  mov.l   @(\hi,r6),r2
    cmp/gt  r2,r0
    bf      1f
    mov.l   r0,@(\hi,r6)
1:
    .endm
sv_loop:
    mov.w   @(22,r4),r0
    extu.w  r0,r1
    mov.l   @(0,r6),r2
    and     r1,r2
    mov.l   r2,@(0,r6)
    mov.l   @(4,r6),r2
    or      r1,r2
    mov.l   r2,@(4,r6)
    mov.l   @(40,r6),r2
    and     r1,r2
    mov.l   @(44,r6),r3
    cmp/eq  r3,r2
    movt    r2
    mov.l   @(12,r6),r3
    and     r2,r3
    mov.l   r3,@(12,r6)
    mov.l   @(4,r4),r2
    add     #1,r2
    tst     r2,r2
    movt    r2
    mov.l   @(8,r6),r3
    and     r2,r3
    mov.l   r3,@(8,r6)
    BND     8, 16, 28
    BND     10, 20, 32
    BND     12, 24, 36
    add     #28,r4
    dt      r5
    bf      sv_loop
    rts
    nop
