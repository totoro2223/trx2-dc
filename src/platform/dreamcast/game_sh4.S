! Hand-written SH-4 for the game logic that is moved out of C (PLAN_ASM_TOTAL,
! phase 9). Each routine is a function of C, branch by branch; the C stays as
! the reference (a build with DC_EMIT_VERIFY runs both and compares) and as the
! other platforms' code.
!
!   void dc_clamp_step(int16_t *f, int32_t required, int32_t max_change,
!                      int32_t arc);
!     creature.c Creature_Head / Neck / Joint: change = required - *f, held to
!     +-max_change; *f += change; *f held to +-arc. int16 wraps as in C.
!   void dc_creature_tilt(int16_t *rot_z, int32_t angle, int32_t max);
!     Creature_Tilt: angle = angle * 4 - *rot_z, held to +-max; *rot_z += angle.
!   void dc_creature_turn_to(int16_t *rot_y, int32_t angle, int32_t max_turn);
!     Creature_TurnTo.

    .text
    .align 2
    .global _dc_clamp_step
    .global _dc_creature_tilt
    .global _dc_creature_turn_to

_dc_clamp_step:
    mov.w   @r4,r0
    mov     r5,r1
    sub     r0,r1
    exts.w  r1,r1                   ! change, as an int16
    neg     r6,r2                   ! the low bound
    cmp/gt  r1,r2
    bf      1f
    mov     r2,r1                   ! change < -max_change
    bra     2f
    nop
1:  cmp/gt  r6,r1
    bf      2f
    mov     r6,r1                   ! change > max_change
2:  add     r1,r0
    exts.w  r0,r0                   ! *f += change, as an int16
    neg     r7,r2
    cmp/gt  r0,r2
    bf      3f
    mov     r2,r0
    bra     4f
    nop
3:  cmp/gt  r7,r0
    bf      4f
    mov     r7,r0
4:  mov.w   r0,@r4
    rts
    nop

_dc_creature_tilt:
    mov.w   @r4,r0                  ! rot.z
    shll2   r5
    sub     r0,r5
    exts.w  r5,r5                   ! angle * 4 - rot.z, as an int16
    neg     r6,r2
    cmp/gt  r5,r2
    bf      1f
    mov     r2,r5
    bra     2f
    nop
1:  cmp/gt  r6,r5
    bf      2f
    mov     r6,r5
2:  add     r5,r0
    mov.w   r0,@r4
    rts
    nop

_dc_creature_turn_to:
    mov.w   @r4,r0                  ! rot.y
    cmp/gt  r6,r5                   ! angle > max_turn
    bf      1f
    add     r6,r0
    bra     3f
    nop
1:  neg     r6,r2
    cmp/gt  r5,r2                   ! angle < -max_turn
    bf      2f
    sub     r6,r0
    bra     3f
    nop
2:  add     r5,r0
3:  mov.w   r0,@r4
    rts
    nop

! SECTOR *dc_room_sector(const ROOM *rooms, int32_t x, int32_t z, int16_t *room_num);
!
! The first half of Room_GetSector (rooms/geometry.c): the sector at (x, z) of
! *room_num after the wall portals, *room_num following them. Returns the sector
! when it has no pit or sky portal (so the floor and ceiling tests of the C have
! nothing to do), else 0 (*room_num is then where the wall portals ended, and
! the C starts from there). The layout comes from g_DC_RoomOff (words): 0 ROOM
! stride  4 pos.x  8 pos.z  12 size.x  16 size.z  20 SECTOR size  24 sectors
! 28 portal_room.wall  32 .pit  36 .sky.
    .text
    .align 2
    .global _dc_room_sector
_dc_room_sector:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   .Lroomoff,r1
    mov     #-10,r11                ! WALL_SHIFT, as a shad count
    mov.w   @r7,r0                  ! the room
rs_room:
    mov.l   @r1,r2
    mul.l   r0,r2
    sts     macl,r2
    add     r4,r2                   ! the room
    mov.l   @(4,r1),r0
    mov.l   @(r0,r2),r3
    mov     r5,r8
    sub     r3,r8
    shad    r11,r8                  ! x_sector
    mov.l   @(8,r1),r0
    mov.l   @(r0,r2),r3
    mov     r6,r9
    sub     r3,r9
    shad    r11,r9                  ! z_sector
    mov.l   @(12,r1),r0
    mov.w   @(r0,r2),r10            ! size.x
    mov.l   @(16,r1),r0
    mov.w   @(r0,r2),r3             ! size.z
    cmp/pl  r9
    bt      rs_z1
    mov     #0,r9
    bra     rs_clampx
    nop
rs_z1:
    mov     r3,r0
    add     #-1,r0
    cmp/ge  r0,r9                   ! z_sector >= size.z - 1
    bf      rs_mid
    mov     r0,r9
rs_clampx:
    mov     #1,r0
    cmp/ge  r0,r8
    bf      rs_set1
    mov     r10,r0
    add     #-2,r0
    cmp/gt  r0,r8
    bf      rs_get
    mov     r0,r8
    bra     rs_get
    nop
rs_set1:
    mov     #1,r8
    bra     rs_get
    nop
rs_mid:
    cmp/pz  r8
    bt      rs_xhi
    mov     #0,r8
    bra     rs_get
    nop
rs_xhi:
    cmp/ge  r10,r8
    bf      rs_get
    mov     r10,r8
    add     #-1,r8
rs_get:
    mul.l   r3,r8
    sts     macl,r8
    add     r9,r8                   ! z + x * size.z
    mov.l   @(20,r1),r0
    mul.l   r0,r8
    sts     macl,r8
    mov.l   @(24,r1),r0
    mov.l   @(r0,r2),r3
    add     r3,r8                   ! the sector
    mov.l   @(28,r1),r0
    mov.w   @(r0,r8),r0             ! portal_room.wall
    cmp/eq  #-1,r0
    bt      rs_done
    mov.w   r0,@r7
    bra     rs_room
    nop
rs_done:
    mov.l   @(32,r1),r0
    mov.w   @(r0,r8),r2
    mov.l   @(36,r1),r0
    mov.w   @(r0,r8),r3
    and     r3,r2
    mov     #-1,r0
    cmp/eq  r0,r2
    mov     r8,r0
    bt      rs_out
    mov     #0,r0
rs_out:
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    rts
    nop
    .align 2
.Lroomoff:
    .long   _g_DC_RoomOff

! bool dc_flat_floor(const SECTOR *sector, int32_t abyss_min, int32_t *out);
!
! The floor height of a sector whose floor is level and whole (no tilt, no
! split), with no pit portal, no walkable and not an abyss: Room_GetHeightEx's
! plain case (rooms/geometry.c). Stores the height in *out and returns 1, else
! returns 0 (the C then does it). Layout words of g_DC_RoomOff: 8 pit  10 walkable
! 11 floor.height  12 floor.tilt  13 floor.is_split (a byte).
    .text
    .align 2
    .global _dc_flat_floor
_dc_flat_floor:
    mov.l   .Lroomoff2,r1
    mov     #-1,r3
    mov.l   @(32,r1),r0
    mov.w   @(r0,r4),r2
    cmp/eq  r3,r2
    bf      ff_fail
    mov.l   @(40,r1),r0
    mov.l   @(r0,r4),r2
    tst     r2,r2
    bf      ff_fail
    mov.l   @(52,r1),r0
    mov.b   @(r0,r4),r2
    tst     r2,r2
    bf      ff_fail
    mov.l   @(48,r1),r0
    mov.l   @(r0,r4),r2
    tst     r2,r2
    bf      ff_fail
    mov.l   @(44,r1),r0
    mov.l   @(r0,r4),r2
    tst     r5,r5
    bt      ff_ok
    cmp/ge  r5,r2
    bt      ff_fail
ff_ok:
    mov.l   r2,@r6
    rts
    mov     #1,r0
ff_fail:
    rts
    mov     #0,r0
    .align 2
.Lroomoff2:
    .long   _g_DC_RoomOff

! void dc_item_move(ITEM *item, const ANIM *anim);
!
! The end of Item_Animate (items/anim.c): the fall or the run of the animation's
! speed, then the move along the item's yaw and across it. Layout words of
! g_DC_ItemOff: 0 gravity (byte)  1 fall_speed  2 pos.y  3 speed  4 frame_num
! 5 anim frame_base  6 velocity  7 acceleration  8 lateral_velocity
! 9 lateral_acceleration  10 rot.y  11 pos.x  12 pos.z.
    .text
    .align 2
    .global _dc_item_move
_dc_item_move:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov     r4,r8                   ! item
    mov     r5,r9                   ! anim
    mov.l   .Litemoff,r10
    mov     #0,r12                  ! lateral speed
    mov.l   @r10,r0
    mov.b   @(r0,r8),r1
    tst     r1,r1
    bt      im_run
    mov.l   @(4,r10),r0
    mov.w   @(r0,r8),r2             ! fall_speed
    mov     #127,r1
    cmp/gt  r1,r2
    bt      im_fast
    bra     im_fall
    add     #6,r2
im_fast:
    add     #1,r2
im_fall:
    mov.w   r2,@(r0,r8)
    exts.w  r2,r2
    mov.l   @(8,r10),r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.l   r3,@(r0,r8)
    mov.l   @(12,r10),r0
    mov.w   @(r0,r8),r11            ! speed
    bra     im_move
    nop
im_run:
    mov.l   @(16,r10),r0
    mov.w   @(r0,r8),r2
    mov.l   @(20,r10),r0
    mov.w   @(r0,r9),r3
    sub     r3,r2                   ! frame_ofs
    mov.l   @(24,r10),r0
    mov.l   @(r0,r9),r3
    mov.l   @(28,r10),r0
    mov.l   @(r0,r9),r1
    mul.l   r1,r2
    sts     macl,r1
    add     r1,r3
    mov     #-16,r1
    shad    r1,r3
    mov.l   @(12,r10),r0
    mov.w   r3,@(r0,r8)
    exts.w  r3,r11                  ! speed, as the item keeps it
    mov.l   @(16,r10),r0
    mov.w   @(r0,r8),r2
    mov.l   @(20,r10),r0
    mov.w   @(r0,r9),r3
    sub     r3,r2
    mov.l   @(32,r10),r0
    mov.l   @(r0,r9),r3
    mov.l   @(36,r10),r0
    mov.l   @(r0,r9),r1
    mul.l   r1,r2
    sts     macl,r1
    add     r1,r3
    mov     #-16,r1
    shad    r1,r3
    mov     r3,r12                  ! lateral speed
im_move:
    mov.l   @(40,r10),r0
    mov.w   @(r0,r8),r13            ! yaw
    mov     r13,r4
    mov.l   .Lmsin,r1
    jsr     @r1
    nop
    mul.l   r11,r0
    sts     macl,r0
    mov     #-14,r1
    shad    r1,r0
    mov     r0,r2
    mov.l   @(44,r10),r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.l   r3,@(r0,r8)
    mov     r13,r4
    mov.l   .Lmcos,r1
    jsr     @r1
    nop
    mul.l   r11,r0
    sts     macl,r0
    mov     #-14,r1
    shad    r1,r0
    mov     r0,r2
    mov.l   @(48,r10),r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.l   r3,@(r0,r8)
    mov     #64,r1
    shll8   r1                      ! DEG_90
    add     r1,r13
    exts.w  r13,r13                 ! the yaw across the item
    mov     r13,r4
    mov.l   .Lmsin,r1
    jsr     @r1
    nop
    mul.l   r12,r0
    sts     macl,r0
    mov     #-14,r1
    shad    r1,r0
    mov     r0,r2
    mov.l   @(44,r10),r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.l   r3,@(r0,r8)
    mov     r13,r4
    mov.l   .Lmcos,r1
    jsr     @r1
    nop
    mul.l   r12,r0
    sts     macl,r0
    mov     #-14,r1
    shad    r1,r0
    mov     r0,r2
    mov.l   @(48,r10),r0
    mov.l   @(r0,r8),r3
    add     r2,r3
    mov.l   r3,@(r0,r8)
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Litemoff:
    .long   _g_DC_ItemOff
.Lmsin:
    .long   _Math_Sin
.Lmcos:
    .long   _Math_Cos

! bool dc_item_animate(ITEM *item, const ANIM *anim);
!
! Item_Animate (items/anim.c) for the plain frame: the animation has no
! commands, nothing to change to (or the item is where it is going), and the
! frame does not pass its end. Counts the frame, clears the hit and touch bits
! and moves the item (dc_item_move), returning 1; else changes nothing and
! returns 0. Layout words of g_DC_ItemOff: 4 frame_num  13 hit_status (byte)
! 14 touch_bits  15 prev_frame_num  16 current_anim_state  17 goal_anim_state
! 18 anim num_changes  19 num_commands  20 frame_end.
    .text
    .align 2
    .global _dc_item_animate
_dc_item_animate:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov     r4,r8
    mov     r5,r9
    mov.l   .Litemoff2,r10
    mov     r10,r11
    add     #64,r11                 ! words 16 and on
    mov.l   @(12,r11),r0
    mov.w   @(r0,r9),r1
    tst     r1,r1
    bf      ia_fail                 ! it has commands
    mov.l   @(16,r10),r0
    mov.w   @(r0,r8),r2
    add     #1,r2
    exts.w  r2,r2                   ! the next frame
    mov.l   @(16,r11),r0
    mov.w   @(r0,r9),r1
    cmp/gt  r1,r2
    bt      ia_fail                 ! it passes the end
    mov.l   @(8,r11),r0
    mov.w   @(r0,r9),r1
    cmp/pl  r1
    bf      ia_go                   ! no changes
    mov.l   @(0,r11),r0
    mov.w   @(r0,r8),r1
    mov.l   @(4,r11),r0
    mov.w   @(r0,r8),r3
    cmp/eq  r1,r3
    bf      ia_fail                 ! a change may be due
ia_go:
    mov     #0,r1
    mov.l   @(52,r10),r0
    mov.b   r1,@(r0,r8)             ! hit_status
    mov.l   @(56,r10),r0
    mov.l   r1,@(r0,r8)             ! touch_bits
    mov.l   @(16,r10),r0
    mov.w   @(r0,r8),r3
    mov.w   r2,@(r0,r8)             ! frame_num
    mov.l   @(60,r10),r0
    mov.w   r3,@(r0,r8)             ! prev_frame_num
    mov     r8,r4
    mov     r9,r5
    mov.l   .Limove,r1
    jsr     @r1
    nop
    bra     ia_out
    mov     #1,r0
ia_fail:
    mov     #0,r0
ia_out:
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Litemoff2:
    .long   _g_DC_ItemOff
.Limove:
    .long   _dc_item_move

! void dc_rot_x/y/z(MATRIX *m, int32_t s, int32_t c);   matrix.c M_RotXSC etc.
!
! A turn of the first three columns of a MATRIX (twelve int64, rows of four).
! For the pair (a, b) of each row: a' = (a*c + b*s) >> 14, b' = (b*c - a*s) >> 14,
! with 32-bit products and the entries taken as int32, as the C does. Y is the
! same with s negated. The result goes back sign-extended to int64.
    .macro ROTDEF name, oa, ob, neg
    .global _\name
    .align 2
_\name:
    .if \neg
    neg     r5,r5
    .endif
    mov     #3,r3
1:  mov.l   @(\oa,r4),r0
    mov.l   @(\ob,r4),r1
    mul.l   r0,r6
    sts     macl,r2                 ! a*c
    mul.l   r1,r5
    sts     macl,r7                 ! b*s
    add     r7,r2                   ! a'
    mul.l   r1,r6
    sts     macl,r1                 ! b*c
    mul.l   r0,r5
    sts     macl,r7                 ! a*s
    sub     r7,r1                   ! b'
    mov     #-14,r0
    shad    r0,r2                   ! >> W2V_SHIFT (14), arithmetic
    shad    r0,r1
    mov.l   r2,@(\oa,r4)
    mov     r2,r0
    shll    r0
    subc    r0,r0                   ! the sign, all ones or zero
    mov.l   r0,@(\oa+4,r4)
    mov.l   r1,@(\ob,r4)
    mov     r1,r0
    shll    r0
    subc    r0,r0
    mov.l   r0,@(\ob+4,r4)
    add     #32,r4
    dt      r3
    bf      1b
    rts
    nop
    .endm
    ROTDEF  dc_rot_x, 8, 16, 0
    ROTDEF  dc_rot_y, 0, 16, 1
    ROTDEF  dc_rot_z, 0, 8, 0

! void dc_translate_rel(MATRIX *m, int32_t x, int32_t y, int32_t z);
!
! m[r][3] += x*m[r][0] + y*m[r][1] + z*m[r][2] for the three rows, each product
! 32 x 32 -> 64 (the entries as int32), as M_TranslateRel does.
    .macro TROW
    mov.l   @(0,r4),r0
    dmuls.l r5,r0
    sts     macl,r1
    sts     mach,r2
    mov.l   @(8,r4),r0
    dmuls.l r6,r0
    sts     macl,r3
    clrt
    addc    r3,r1
    sts     mach,r3
    addc    r3,r2
    mov.l   @(16,r4),r0
    dmuls.l r7,r0
    sts     macl,r3
    clrt
    addc    r3,r1
    sts     mach,r3
    addc    r3,r2
    mov.l   @(24,r4),r0
    mov.l   @(28,r4),r3
    clrt
    addc    r1,r0
    addc    r2,r3
    mov.l   r0,@(24,r4)
    mov.l   r3,@(28,r4)
    add     #32,r4
    .endm
    .global _dc_translate_rel
    .align 2
_dc_translate_rel:
    TROW
    TROW
    TROW
    rts
    nop

! void dc_mat_copy(MATRIX *dst, const MATRIX *src);   96 bytes, matrix.c Matrix_Push
    .global _dc_mat_copy
    .align 2
_dc_mat_copy:
    mov     #6,r7
1:  mov.l   @r5+,r0
    mov.l   @r5+,r1
    mov.l   @r5+,r2
    mov.l   @r5+,r3
    mov.l   r0,@r4
    mov.l   r1,@(4,r4)
    mov.l   r2,@(8,r4)
    mov.l   r3,@(12,r4)
    add     #16,r4
    dt      r7
    bf      1b
    rts
    nop

! int dc_mul3x3(MATRIX *out, const MATRIX *lhs, const MATRIX *rhs);   matrix.c Matrix_Mul3x3_M
!
! out[r][c] = (sum over k of lhs[r][k] * rhs[k][c]) >> 14 for the 3x3 part, in
! 64 bits. Does it only when all eighteen entries read are int32 (so each product is
! one dmuls.l); otherwise returns 0 and touches nothing. out may be lhs: the nine
! results wait on the stack until all are found.
    .macro CHKENT off
    mov.l   @(\off,r2),r0
    mov.l   @(\off+4,r2),r1
    shad    r7,r0                   ! the sign of the low word, as a high word
    cmp/eq  r0,r1
    bf      m3_fail
    .endm
    .macro CHKM reg
    mov     \reg,r2
    mov     #3,r3
1:  CHKENT  0
    CHKENT  8
    CHKENT  16
    add     #32,r2
    dt      r3
    bf      1b
    .endm
    .global _dc_mul3x3
    .align 2
_dc_mul3x3:
    mov     #-31,r7
    CHKM    r5
    CHKM    r6
    bra     m3_ok
    nop
m3_fail:
    rts
    mov     #0,r0
m3_ok:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    add     #-72,r15
    mov     r15,r11
    mov     r5,r2                   ! the row of lhs
    mov     #3,r12
m3_row:
    mov     r6,r7                   ! the column of rhs
    mov     #3,r13
m3_col:
    mov.l   @(0,r2),r0
    mov.l   @(0,r7),r1
    dmuls.l r0,r1
    sts     macl,r8
    sts     mach,r9
    mov.l   @(8,r2),r0
    mov.l   @(32,r7),r1
    dmuls.l r0,r1
    sts     macl,r10
    clrt
    addc    r10,r8
    sts     mach,r10
    addc    r10,r9
    mov.l   @(16,r2),r0
    add     #32,r7
    mov.l   @(32,r7),r1
    dmuls.l r0,r1
    sts     macl,r10
    clrt
    addc    r10,r8
    sts     mach,r10
    addc    r10,r9
    add     #-32,r7
    mov     #-14,r0
    shld    r0,r8                   ! the 64-bit value >> 14, arithmetic
    mov     #18,r1
    mov     r9,r10
    shld    r1,r10
    or      r10,r8
    shad    r0,r9
    mov.l   r8,@r11
    mov.l   r9,@(4,r11)
    add     #8,r11
    add     #8,r7
    dt      r13
    bf      m3_col
    add     #32,r2
    dt      r12
    bf      m3_row
    mov     r15,r11                 ! the results into out
    mov     r4,r2
    mov     #3,r12
m3_put:
    mov.l   @r11+,r0
    mov.l   @r11+,r1
    mov.l   r0,@r2
    mov.l   r1,@(4,r2)
    mov.l   @r11+,r0
    mov.l   @r11+,r1
    mov.l   r0,@(8,r2)
    mov.l   r1,@(12,r2)
    mov.l   @r11+,r0
    mov.l   @r11+,r1
    mov.l   r0,@(16,r2)
    mov.l   r1,@(20,r2)
    add     #32,r2
    dt      r12
    bf      m3_put
    add     #72,r15
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    rts
    mov     #1,r0

! int dc_blend12(MATRIX *out, const MATRIX *m1, const MATRIX *m2, const int32_t *p);
!
! The original games' pose blend, M_InterpolateLinear: every one of the twelve
! entries goes frac/rate of the way from m1 to m2 (p: frac, rate, 2^32-1 / rate).
! The caller has the rate above 0 and frac < 32 or (rate 2, frac 1). The
! average case is (a+b)>>1 in 64 bits; the rest is a + (d*frac)/rate by the
! reciprocal, d = b-a. If some d lies outside +-(2^26-1) nothing is written and
! the result is 0 (the C divides in 64 bits there). out may be m1.
    .global _dc_blend12
    .align 2
_dc_blend12:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   @(0,r7),r8              ! frac
    mov.l   @(4,r7),r9              ! rate
    mov.l   @(8,r7),r10             ! 2^32-1 / rate
    mov     r9,r0
    cmp/eq  #2,r0
    bf      bl_gen
    mov     r8,r0
    cmp/eq  #1,r0
    bf      bl_gen
    mov     #12,r11
bl_avg:
    mov.l   @r5+,r0
    mov.l   @r5+,r1
    mov.l   @r6+,r2
    mov.l   @r6+,r3
    clrt
    addc    r2,r0
    addc    r3,r1
    shar    r1
    rotcr   r0
    mov.l   r0,@r4
    mov.l   r1,@(4,r4)
    add     #8,r4
    dt      r11
    bf      bl_avg
    bra     bl_ok
    nop
bl_gen:
    mov.l   .Lbk26,r12
    mov.l   .Lbk27,r13
    mov     #12,r11
bl_chk:
    mov.l   @r5+,r0
    mov.l   @r5+,r1
    mov.l   @r6+,r2
    mov.l   @r6+,r3
    clrt
    subc    r0,r2                   ! d, low word
    subc    r1,r3                   ! d, high word
    mov     r2,r1
    shll    r1
    subc    r1,r1                   ! the sign of the low word
    cmp/eq  r1,r3
    bf      bl_fail
    add     r12,r2
    cmp/hi  r13,r2
    bt      bl_fail
    dt      r11
    bf      bl_chk
    add     #-96,r5
    add     #-96,r6
    mov     #12,r11
bl_com:
    mov.l   @r5+,r0                 ! a
    mov.l   @r5+,r1
    mov.l   @r6+,r2
    add     #4,r6
    sub     r0,r2                   ! d
    mul.l   r2,r8
    sts     macl,r3                 ! n = d * frac
    mov     r3,r2
    cmp/pz  r2
    bt      bl_pos
    neg     r2,r2                   ! u = |n|
bl_pos:
    dmulu.l r2,r10
    sts     mach,r7                 ! q
    mul.l   r7,r9
    sts     macl,r12
    sub     r12,r2                  ! r = u - q * rate
bl_fix:
    cmp/hs  r9,r2
    bf      bl_done
    add     #1,r7
    bra     bl_fix
    sub     r9,r2
bl_done:
    mov     #0,r12
    cmp/pz  r3
    bf      bl_neg
    clrt
    addc    r7,r0
    addc    r12,r1
    bra     bl_put
    nop
bl_neg:
    clrt
    subc    r7,r0
    subc    r12,r1
bl_put:
    mov.l   r0,@r4
    mov.l   r1,@(4,r4)
    add     #8,r4
    dt      r11
    bf      bl_com
bl_ok:
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    rts
    mov     #1,r0
bl_fail:
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    rts
    mov     #0,r0
    .align 2
.Lbk26:
    .long   67108863
.Lbk27:
    .long   134217726

! void dc_rot_yxz_f(MATRIX *const *ms, int32_t n, const int32_t *p);
!
! M_RotYXZ_Float of matrix.c: the Y, X and Z turns of a bone worked out as one
! float matrix (each pair of entries as the C compiler fuses it: c*a + s*b,
! c*b - s*a, with fmac) and applied with ftrv to the three rows of each of the
! n matrices (the floor of the result, kept in the low word, sign-extended).
! p: 0 sin Y  1 cos Y  2 sin X  3 cos X  4 sin Z  5 cos Z (the table's integers)
! 6 a mask of the turns that are not zero (1 Y, 2 X, 4 Z).
    .macro ROTF bit, si, ci, ca, cb, negs
    mov     r13,r0
    tst     #\bit,r0
    bt      2f
    mov.l   @(\si*4,r6),r1
    lds     r1,fpul
    float   fpul,fr4
    fmul    fr8,fr4
    .if \negs
    fneg    fr4
    .endif
    mov.l   @(\ci*4,r6),r1
    lds     r1,fpul
    float   fpul,fr0
    fmul    fr8,fr0
    mov     r14,r2
    mov     #3,r3
1:  mov     r2,r1
    add     #\ca,r1
    fmov.s  @r1,fr6
    mov     r2,r1
    add     #\cb,r1
    fmov.s  @r1,fr7
    fmov    fr4,fr2
    fmul    fr7,fr2
    fmac    fr0,fr6,fr2
    fmov    fr4,fr3
    fmul    fr6,fr3
    fneg    fr3
    fmac    fr0,fr7,fr3
    mov     r2,r1
    add     #\ca,r1
    fmov.s  fr2,@r1
    mov     r2,r1
    add     #\cb,r1
    fmov.s  fr3,@r1
    add     #16,r2
    dt      r3
    bf      1b
2:
    .endm
    .global _dc_rot_yxz_f
    .align 2
_dc_rot_yxz_f:
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    add     #-72,r15
    mov     r15,r14
    add     #7,r14
    mov     #-8,r0
    and     r0,r14                  ! the 4x4 float matrix, 8-aligned
    mov.l   .Lrf_one,r1
    mov     #0,r0
    mov.l   r1,@(0,r14)
    mov.l   r0,@(4,r14)
    mov.l   r0,@(8,r14)
    mov.l   r0,@(12,r14)
    mov.l   r0,@(16,r14)
    mov.l   r1,@(20,r14)
    mov.l   r0,@(24,r14)
    mov.l   r0,@(28,r14)
    mov.l   r0,@(32,r14)
    mov.l   r0,@(36,r14)
    mov.l   r1,@(40,r14)
    mov.l   r0,@(44,r14)
    mov.l   r0,@(48,r14)
    mov.l   r0,@(52,r14)
    mov.l   r0,@(56,r14)
    mov.l   r1,@(60,r14)
    mov.l   @(24,r6),r13            ! the mask
    mov.l   .Lrf_k,r1
    lds     r1,fpul
    fsts    fpul,fr8                ! 1 / 16384
    ROTF    1, 0, 1, 0, 8, 1        ! Y
    ROTF    2, 2, 3, 4, 8, 0        ! X
    ROTF    4, 4, 5, 0, 4, 0        ! Z
    mov     r14,r1
    fschg
    frchg
    fmov.d  @r1+,dr0
    fmov.d  @r1+,dr2
    fmov.d  @r1+,dr4
    fmov.d  @r1+,dr6
    fmov.d  @r1+,dr8
    fmov.d  @r1+,dr10
    fmov.d  @r1+,dr12
    fmov.d  @r1+,dr14
    frchg
    fschg
    mov.l   .Lrf_bias,r1
    lds     r1,fpul
    fsts    fpul,fr9                ! 32768.0
    mov.l   .Lrf_m32768,r12
rf_next:
    mov.l   @r4+,r2                 ! the matrix
    mov     #3,r7
rf_row:
    mov.l   @r2,r1
    lds     r1,fpul
    float   fpul,fr0
    mov.l   @(8,r2),r1
    lds     r1,fpul
    float   fpul,fr1
    mov.l   @(16,r2),r1
    lds     r1,fpul
    float   fpul,fr2
    fldi0   fr3
    ftrv    xmtrx,fv0
    fadd    fr9,fr0
    fadd    fr9,fr1
    fadd    fr9,fr2
    ftrc    fr0,fpul
    sts     fpul,r1
    add     r12,r1
    mov.l   r1,@r2
    shll    r1
    subc    r3,r3
    mov.l   r3,@(4,r2)
    ftrc    fr1,fpul
    sts     fpul,r1
    add     r12,r1
    mov.l   r1,@(8,r2)
    shll    r1
    subc    r3,r3
    mov.l   r3,@(12,r2)
    ftrc    fr2,fpul
    sts     fpul,r1
    add     r12,r1
    mov.l   r1,@(16,r2)
    shll    r1
    subc    r3,r3
    mov.l   r3,@(20,r2)
    add     #32,r2
    dt      r7
    bf      rf_row
    dt      r5
    bf      rf_next
    add     #72,r15
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    rts
    mov.l   @r15+,r12
    .align 2
.Lrf_one:
    .long   0x3F800000
.Lrf_k:
    .long   0x38800000
.Lrf_bias:
    .long   0x47000000
.Lrf_m32768:
    .long   -32768

! void dc_bounds_lerp(int16_t *out, const int16_t *b1, const int16_t *b2, const int32_t *p);
!
! Item_GetBoundsAccurate (items/col.c): each of the six int16 of a BOUNDS_16 is
! b1 + (b2 - b1) * frac / rate, the division truncating toward zero, by the
! reciprocal (p: frac, rate > 0, 2^32-1 / rate) with the fix-up that makes it exact.
    .global _dc_bounds_lerp
    .align 2
_dc_bounds_lerp:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   @(0,r7),r8              ! frac
    mov.l   @(4,r7),r9              ! rate
    mov.l   @(8,r7),r10             ! 2^32-1 / rate
    mov     #6,r11
bl2_loop:
    mov.w   @r5+,r12                ! a
    mov.w   @r6+,r1
    sub     r12,r1                  ! d
    mul.l   r1,r8
    sts     macl,r2                 ! n = d * frac
    mov     r2,r1
    cmp/pz  r1
    bt      bl2_pos
    neg     r1,r1                   ! u = |n|
bl2_pos:
    dmulu.l r1,r10
    sts     mach,r3                 ! q
    mul.l   r3,r9
    sts     macl,r7
    sub     r7,r1                   ! r = u - q * rate
bl2_fix:
    cmp/hs  r9,r1
    bf      bl2_done
    add     #1,r3
    bra     bl2_fix
    sub     r9,r1
bl2_done:
    cmp/pz  r2
    bt      bl2_add
    neg     r3,r3
bl2_add:
    add     r3,r12
    mov.w   r12,@r4
    add     #2,r4
    dt      r11
    bf      bl2_loop
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! int32_t dc_get_frames(const int32_t *p, ANIM_FRAME **frames, int32_t *rate);
!
! M_GetFrames of items/anim.c without the call-backs: the two keyframes around
! the frame an item is showing and the blend between them. p: 0 frame_num
! 1 prev_frame_num 2 frame_base 3 frame_end 4 the keyframe span 5 frame_ptr
! 6 alpha (float bits) 7 the can_interpolate call-back: 0 none or yes, 2 no,
! 1 some other one (C does it). frames
! are 24 bytes apart. The division by the span is a multiply by
! g_DC_InvTab[span] (2^32-1 / span) with the fix-up. The float steps are the C
! compiler's: the position with fmac, the result with fmac. Returns the blend
! (>= 0), or -1 with nothing written when the C has to do it (a call-back to
! ask, span 0, a frame before the animation).
    .global _dc_get_frames
    .align 2
_dc_get_frames:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   @(8,r4),r3              ! base
    mov.l   @(0,r4),r8
    sub     r3,r8                   ! cur
    mov.l   @(12,r4),r7             ! end
    mov     r7,r9
    sub     r3,r9                   ! last
    mov.l   @(16,r4),r10            ! span
    tst     r10,r10
    bt      gf_fail
    mov     r8,r11                  ! interp
    fldi0   fr8                     ! sub
    mov.l   @(24,r4),r1
    lds     r1,fpul
    fsts    fpul,fr9                ! alpha
    fldi0   fr0
    fcmp/gt fr0,fr9
    bt      gf_a1
    fcmp/eq fr0,fr9
    bf      gf_noint
gf_a1:
    fldi1   fr1
    fcmp/gt fr9,fr1
    bt      gf_a2
    fcmp/eq fr9,fr1
    bf      gf_noint
gf_a2:
    mov.l   @(4,r4),r2              ! prev
    cmp/ge  r3,r2
    bf      gf_noint
    cmp/ge  r2,r7
    bf      gf_noint
    sub     r3,r2                   ! prev - base
    mov     r8,r1
    sub     r2,r1                   ! delta
    cmp/pl  r1
    bf      gf_noint
    mov.l   @(28,r4),r0
    cmp/eq  #2,r0
    bt      gf_noint                ! the call-back said no
    tst     r0,r0
    bf      gf_fail                 ! a call-back to ask
    lds     r1,fpul
    float   fpul,fr0                ! delta
    lds     r2,fpul
    float   fpul,fr1                ! prev - base
    fmac    fr0,fr9,fr1             ! the position
    lds     r9,fpul
    float   fpul,fr2
    fcmp/gt fr1,fr2                 ! last > position
    bf      gf_noint
    ftrc    fr1,fpul
    sts     fpul,r11                ! interp
    float   fpul,fr2
    fsub    fr2,fr1
    fmov    fr1,fr8                 ! the fraction of a frame
gf_noint:
    cmp/pz  r11
    bf      gf_fail
    mov.l   .Lgf_tab,r1
    mov     r10,r0
    shll2   r0
    mov.l   @(r0,r1),r12            ! 2^32-1 / span
    dmulu.l r11,r12
    sts     mach,r2                 ! q
    mul.l   r2,r10
    sts     macl,r1
    mov     r11,r7
    sub     r1,r7                   ! the shift
gf_fix:
    cmp/hs  r10,r7
    bf      gf_fixed
    add     #1,r2
    bra     gf_fix
    sub     r10,r7
gf_fixed:
    mov.l   @(20,r4),r13
    mov     r2,r1
    shll2   r1
    shll    r1                      ! q * 8
    mov     r2,r3
    shll2   r3
    shll2   r3                      ! q * 16
    add     r3,r1                   ! q * 24
    add     r1,r13
    mov.l   r13,@r5
    add     #24,r13
    mov.l   r13,@(4,r5)
    mov     r10,r12                 ! denominator
    tst     r7,r7
    bf      gf_cond
    fldi0   fr0
    fcmp/gt fr0,fr8
    bf      gf_nocond
gf_cond:
    mov     r2,r1
    add     #1,r1
    mul.l   r1,r10
    sts     macl,r1                 ! second key frame
    mov.l   @(12,r4),r3
    cmp/ge  r1,r3
    bt      gf_nocond
    sub     r1,r3
    add     r3,r12
gf_nocond:
    lds     r7,fpul
    float   fpul,fr0
    fadd    fr8,fr0                 ! numerator
    mov.l   .Lgf_k,r1
    lds     r1,fpul
    fsts    fpul,fr2                ! 1024.0
    mov.l   .Lgf_h,r1
    lds     r1,fpul
    fsts    fpul,fr1                ! 0.5
    fmac    fr0,fr2,fr1
    ftrc    fr1,fpul
    sts     fpul,r0
    shll8   r12
    shll2   r12
    mov.l   r12,@r6
    bra     gf_out
    nop
gf_fail:
    mov     #-1,r0
gf_out:
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .align 2
.Lgf_tab:
    .long   _g_DC_InvTab
.Lgf_k:
    .long   0x44800000
.Lgf_h:
    .long   0x3F000000

! int dc_scale_col(MATRIX *m, int32_t scale, int32_t coloff);   matrix.c M_ScaleX/Y/Z
!
! The three entries of one column (coloff 0, 8 or 16 bytes) times scale >> 14, in
! 64 bits: m[r][c] = (int64) m[r][c] * scale >> 14. Only when the three entries
! are int32 (one dmuls.l each); otherwise returns 0 and writes nothing.
    .global _dc_scale_col
    .align 2
_dc_scale_col:
    add     r6,r4
    mov     #-31,r7
    mov     r4,r2
    mov     #3,r3
sc_chk:
    mov.l   @(0,r2),r0
    mov.l   @(4,r2),r1
    shad    r7,r0
    cmp/eq  r0,r1
    bf      sc_fail
    add     #32,r2
    dt      r3
    bf      sc_chk
    mov     #3,r3
sc_go:
    mov.l   @(0,r4),r0
    dmuls.l r0,r5
    sts     macl,r1
    sts     mach,r2
    mov     #-14,r0
    shld    r0,r1
    mov     #18,r0
    mov     r2,r7
    shld    r0,r7
    or      r7,r1
    mov     #-14,r0
    shad    r0,r2
    mov.l   r1,@(0,r4)
    mov.l   r2,@(4,r4)
    add     #32,r4
    dt      r3
    bf      sc_go
    rts
    mov     #1,r0
sc_fail:
    rts
    mov     #0,r0

! int dc_arm_interp(MATRIX *m, const MATRIX *mi, const int32_t *p);   matrix.c M_InterpolateArm
!
! The arm of a pose: the 3x3 part of m[-2], and each translation moved
! (mi - m) * frac / rate toward mi's (truncating toward zero, by the reciprocal;
! p: frac, rate, 2^32-1 / rate). Only when frac < 32 and every step lies within
! +-(2^26-1); otherwise returns 0 and writes nothing.
    .global _dc_arm_interp
    .align 2
_dc_arm_interp:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   @(0,r6),r8              ! frac
    mov.l   @(4,r6),r9              ! rate
    mov.l   @(8,r6),r10             ! inv
    mov.l   .Lai_k26,r12
    mov.l   .Lai_k27,r13
    mov     r4,r2
    add     #24,r2
    mov     r5,r3
    add     #24,r3
    mov     #3,r11
ai_chk:
    mov.l   @r2,r0
    mov.l   @(4,r2),r1
    mov.l   @r3,r6
    mov.l   @(4,r3),r7
    clrt
    subc    r0,r6
    subc    r1,r7                   ! d
    mov     r6,r1
    shll    r1
    subc    r1,r1
    cmp/eq  r1,r7
    bf      ai_fail
    add     r12,r6
    cmp/hi  r13,r6
    bt      ai_fail
    add     #32,r2
    add     #32,r3
    dt      r11
    bf      ai_chk
    mov     r4,r2
    add     #24,r2
    mov     r5,r3
    add     #24,r3
    mov     #3,r11
ai_com:
    mov.l   @r2,r0                  ! the translation, low
    mov.l   @(4,r2),r1              ! high
    mov.l   @r3,r6
    sub     r0,r6                   ! d (low word is all there is)
    mul.l   r6,r8
    sts     macl,r7                 ! n
    mov     r7,r6
    cmp/pz  r6
    bt      ai_pos
    neg     r6,r6                   ! u
ai_pos:
    dmulu.l r6,r10
    sts     mach,r12                ! q
    mul.l   r12,r9
    sts     macl,r13
    sub     r13,r6                  ! r
ai_fix:
    cmp/hs  r9,r6
    bf      ai_done
    add     #1,r12
    bra     ai_fix
    sub     r9,r6
ai_done:
    mov     #0,r13
    cmp/pz  r7
    bf      ai_neg
    clrt
    addc    r12,r0
    addc    r13,r1
    bra     ai_put
    nop
ai_neg:
    clrt
    subc    r12,r0
    subc    r13,r1
ai_put:
    mov.l   r0,@r2
    mov.l   r1,@(4,r2)
    add     #32,r2
    add     #32,r3
    dt      r11
    bf      ai_com
    mov     r4,r3
    add     #-96,r3
    add     #-96,r3                 ! m[-2]
    mov     #3,r11
ai_rot:
    mov.l   @r3+,r0
    mov.l   @r3+,r1
    mov.l   @r3+,r2
    mov.l   @r3+,r6
    mov.l   @r3+,r7
    mov.l   @r3+,r12
    mov.l   r0,@(0,r4)
    mov.l   r1,@(4,r4)
    mov.l   r2,@(8,r4)
    mov.l   r6,@(12,r4)
    mov.l   r7,@(16,r4)
    mov.l   r12,@(20,r4)
    add     #8,r3                   ! past the translation
    add     #32,r4
    dt      r11
    bf      ai_rot
    mov     #1,r0
    bra     ai_out
    nop
ai_fail:
    mov     #0,r0
ai_out:
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .align 2
.Lai_k26:
    .long   67108863
.Lai_k27:
    .long   134217726

! int dc_translate_abs(MATRIX *m, const MATRIX *v, MATRIX *wm, const int32_t *xyz);
!
! Matrix_TranslateAbs: m[r][3] = x*v[r][0] + y*v[r][1] + z*v[r][2] + v[r][3] in
! 64 bits, and wm[r][3] = (r-th of x, y, z) << 14 (M_TranslateSet). Only when
! the nine entries of v are int32 (a dmuls.l each); otherwise returns 0 and
! writes nothing.
    .global _dc_translate_abs
    .align 2
_dc_translate_abs:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   @(0,r7),r8
    mov.l   @(4,r7),r9
    mov.l   @(8,r7),r10
    mov     #-31,r7
    mov     r5,r2
    mov     #3,r3
ta_chk:
    mov.l   @(0,r2),r0
    mov.l   @(4,r2),r1
    shad    r7,r0
    cmp/eq  r0,r1
    bf      ta_fail
    mov.l   @(8,r2),r0
    mov.l   @(12,r2),r1
    shad    r7,r0
    cmp/eq  r0,r1
    bf      ta_fail
    mov.l   @(16,r2),r0
    mov.l   @(20,r2),r1
    shad    r7,r0
    cmp/eq  r0,r1
    bf      ta_fail
    add     #32,r2
    dt      r3
    bf      ta_chk
    mov     #3,r3
ta_row:
    mov.l   @(0,r5),r0
    dmuls.l r8,r0
    sts     macl,r11
    sts     mach,r12
    mov.l   @(8,r5),r0
    dmuls.l r9,r0
    sts     macl,r1
    clrt
    addc    r1,r11
    sts     mach,r1
    addc    r1,r12
    mov.l   @(16,r5),r0
    dmuls.l r10,r0
    sts     macl,r1
    clrt
    addc    r1,r11
    sts     mach,r1
    addc    r1,r12
    mov.l   @(24,r5),r0
    mov.l   @(28,r5),r1
    clrt
    addc    r0,r11
    addc    r1,r12
    mov.l   r11,@(24,r4)
    mov.l   r12,@(28,r4)
    add     #32,r4
    add     #32,r5
    dt      r3
    bf      ta_row
    mov     r8,r1
    mov     #14,r0
    shld    r0,r1
    mov     r8,r2
    mov     #-18,r0
    shad    r0,r2
    mov.l   r1,@(24,r6)
    mov.l   r2,@(28,r6)
    mov     r9,r1
    mov     #14,r0
    shld    r0,r1
    mov     r9,r2
    mov     #-18,r0
    shad    r0,r2
    mov.l   r1,@(56,r6)
    mov.l   r2,@(60,r6)
    add     #64,r6
    mov     r10,r1
    mov     #14,r0
    shld    r0,r1
    mov     r10,r2
    mov     #-18,r0
    shad    r0,r2
    mov.l   r1,@(24,r6)
    mov.l   r2,@(28,r6)
    mov     #1,r0
    bra     ta_out
    nop
ta_fail:
    mov     #0,r0
ta_out:
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! uint32_t dc_sqrt32(uint32_t n);   core/math/func.c Math_Sqrt
!
! floor(sqrt(n)) exactly, as the C's fsqrt and integer fix-up return: the float
! root (n taken as unsigned), cut to 65535, then stepped down while r*r > n and
! up while (r+1)^2 <= n.
    .global _dc_sqrt32
    .align 2
_dc_sqrt32:
    tst     r4,r4
    bf      sq_nz
    rts
    mov     #0,r0
sq_nz:
    cmp/pz  r4
    bt      sq_small
    mov     r4,r1                   ! n >= 2^31: (n >> 1 | n & 1) * 2
    shlr    r1
    mov     #1,r0
    and     r4,r0
    or      r0,r1
    lds     r1,fpul
    float   fpul,fr0
    fadd    fr0,fr0
    bra     sq_root
    nop
sq_small:
    lds     r4,fpul
    float   fpul,fr0
sq_root:
    fsqrt   fr0
    ftrc    fr0,fpul
    sts     fpul,r0
    mov.l   .Lsq_max,r1             ! 65535
    cmp/gt  r1,r0
    bf      sq_dn
    mov     r1,r0
sq_dn:
    mov     r0,r2
    mul.l   r2,r2
    sts     macl,r3
    cmp/hi  r4,r3                   ! r*r > n
    bf      sq_up
    bra     sq_dn
    add     #-1,r0
sq_up:
    cmp/ge  r1,r0                   ! r >= 65535 : done
    bt      sq_done
    mov     r0,r2
    add     #1,r2
    mul.l   r2,r2
    sts     macl,r3
    cmp/hi  r4,r3                   ! (r+1)^2 > n : done
    bt      sq_done
    bra     sq_up
    add     #1,r0
sq_done:
    rts
    nop
    .align 2
.Lsq_max:
    .long   65535

! int32_t dc_sin(int32_t angle);   core/math/trig.c Math_Sin, the fine table
! int32_t dc_atan(int32_t x, int32_t y);   Math_Atan
!
! g_DC_Trig: 0 the sine table (int16, 0x402)  1 the atan base table (int32, 8)
! 2 the atan angle table (int16, 0x802).
    .global _dc_sin
    .global _dc_atan
    .align 2
_dc_sin:
    extu.w  r4,r1
    mov.l   .Ltr_7fff,r2
    and     r1,r2                   ! sector
    mov.l   .Ltr_4000,r3
    cmp/gt  r3,r2
    bf      sn_a
    mov.l   .Ltr_8000,r5
    sub     r2,r5
    mov     r5,r2
sn_a:
    shlr2   r2
    shlr2   r2
    shll    r2
    mov.l   .Ltr_tab,r3
    mov.l   @r3,r3                  ! the sine table
    mov     r2,r0
    mov.w   @(r0,r3),r0
    mov.l   .Ltr_8000,r5
    cmp/hs  r5,r1                   ! angle >= 180 degrees
    bf      sn_b
    neg     r0,r0
sn_b:
    rts
    nop
_dc_atan:
    tst     r4,r4
    bf      at_nz
    tst     r5,r5
    bf      at_nz
    rts
    mov     #0,r0
at_nz:
    mov     #0,r6                   ! base
    cmp/pz  r4
    bt      at_xp
    neg     r4,r4
    add     #4,r6
at_xp:
    cmp/pz  r5
    bt      at_yp
    neg     r5,r5
    add     #2,r6
at_yp:
    cmp/gt  r4,r5
    bf      at_ns
    add     #1,r6
    mov     r4,r1
    mov     r5,r4
    mov     r1,r5
at_ns:
    mov     r5,r2
    shll8   r2
    shll2   r2
    shll    r2                      ! num = 0x800 * y
    lds     r2,fpul
    float   fpul,fr0
    lds     r4,fpul
    float   fpul,fr1
    fdiv    fr1,fr0
    ftrc    fr0,fpul
    sts     fpul,r3                 ! q
    mul.l   r3,r4
    sts     macl,r1
    sub     r1,r2                   ! rem
    cmp/pz  r2
    bt      at_c1
    bra     at_c3
    add     #-1,r3
at_c1:
    cmp/ge  r4,r2
    bf      at_c3
    add     #1,r3
at_c3:
    mov.l   .Ltr_tab,r1
    mov.l   @(4,r1),r7              ! the base table
    mov.l   @(8,r1),r1              ! the angle table
    shll2   r6
    mov     r6,r0
    mov.l   @(r0,r7),r2
    mov     r3,r0
    shll    r0
    mov.w   @(r0,r1),r0
    add     r2,r0
    cmp/pz  r0
    bt      at_out
    neg     r0,r0
at_out:
    rts
    nop
    .align 2
.Ltr_7fff:
    .long   0x7FFF
.Ltr_4000:
    .long   0x4000
.Ltr_8000:
    .long   0x8000
.Ltr_tab:
    .long   _g_DC_Trig

! void dc_turn(int32_t *a, int32_t *b, int32_t s, int32_t c);   core/math/geom.c M_Turn
!
! a' = (a*c + b*s) >> 14, b' = (b*c - a*s) >> 14, the products in 64 bits and
! the low 32 bits of each result kept, as the C has them.
    .global _dc_turn
    .align 2
_dc_turn:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   @r4,r0                  ! a
    mov.l   @r5,r1                  ! b
    dmuls.l r0,r7
    sts     macl,r2
    sts     mach,r3
    dmuls.l r1,r6
    sts     macl,r8
    sts     mach,r9
    clrt
    addc    r8,r2
    addc    r9,r3
    mov     #-14,r8
    shld    r8,r2
    mov     #18,r8
    shld    r8,r3
    or      r3,r2
    mov.l   r2,@r4                  ! the new a
    dmuls.l r1,r7
    sts     macl,r2
    sts     mach,r3
    dmuls.l r0,r6
    sts     macl,r8
    sts     mach,r9
    clrt
    subc    r8,r2
    subc    r9,r3
    mov     #-14,r8
    shld    r8,r2
    mov     #18,r8
    shld    r8,r3
    or      r3,r2
    mov.l   r2,@r5                  ! the new b
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! uint32_t dc_sphere_touch(const SPHERE *a, int32_t na, const SPHERE *b, int32_t nb);
!
! Collide_TestCollision's pairs: bit i is set when sphere i of a (radius above
! 0) touches some sphere of b (radius above 0): d^2 < (r1 + r2)^2, in 32 bits.
! A SPHERE is x y z r, 16 bytes.
    .global _dc_sphere_touch
    .align 2
_dc_sphere_touch:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov     #0,r12                  ! the bits
    mov     #1,r13                  ! this sphere's bit
st_i:
    cmp/pl  r5
    bf      st_done
    mov.l   @(12,r4),r11
    cmp/pl  r11
    bf      st_next_i
    mov     r6,r9
    mov     r7,r10
st_j:
    cmp/pl  r10
    bf      st_next_i
    mov.l   @(12,r9),r1
    cmp/pl  r1
    bf      st_next_j
    add     r11,r1
    mul.l   r1,r1
    sts     macl,r1                 ! (r1 + r2)^2
    mov.l   @r9,r2
    mov.l   @r4,r3
    sub     r3,r2
    mul.l   r2,r2
    sts     macl,r0                 ! dx^2
    mov.l   @(4,r9),r2
    mov.l   @(4,r4),r3
    sub     r3,r2
    mul.l   r2,r2
    sts     macl,r2
    add     r2,r0
    mov.l   @(8,r9),r2
    mov.l   @(8,r4),r3
    sub     r3,r2
    mul.l   r2,r2
    sts     macl,r2
    add     r2,r0                   ! d^2
    cmp/gt  r0,r1
    bf      st_next_j
    or      r13,r12                 ! it touches: the next sphere
    bra     st_next_i
    nop
st_next_j:
    add     #16,r9
    add     #-1,r10
    bra     st_j
    nop
st_next_i:
    add     #16,r4
    add     #-1,r5
    shll    r13
    bra     st_i
    nop
st_done:
    mov     r12,r0
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! int dc_bounds_collide(const COLL_ITEM *src, const COLL_ITEM *dst, int32_t radius);
!
! Collide_TestBoundsCollide. COLL_ITEM (bytes): 0 min x y z, 6 max x y z (int16)
! 12 pos x y z (int32) 24 rot x y z (int16).
    .global _dc_bounds_collide
    .align 2
_dc_bounds_collide:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov     r4,r8                   ! src
    mov     r5,r9                   ! dst
    mov     r6,r10                  ! radius
    mov.l   @(16,r8),r1             ! src y
    mov.w   @(2,r8),r0
    add     r1,r0                   ! src.y + src.min.y
    mov.l   @(16,r9),r2             ! dst y
    mov     r0,r3
    mov     r9,r0
    add     #8,r0
    mov.w   @r0,r0                  ! dst.max.y
    add     r2,r0
    cmp/ge  r0,r3                   ! src.y + min.y >= dst.y + max.y
    bt      bc_no
    mov     r8,r0
    add     #8,r0
    mov.w   @r0,r0                  ! src.max.y
    add     r1,r0
    mov     r0,r3
    mov     r9,r0
    add     #2,r0
    mov.w   @r0,r0                  ! dst.min.y
    add     r2,r0
    cmp/ge  r3,r0                   ! src.y + max.y <= dst.y + min.y
    bt      bc_no
    mov.l   @(12,r9),r1
    mov.l   @(12,r8),r2
    sub     r2,r1                   ! dx
    mov.l   @(20,r9),r11
    mov.l   @(20,r8),r2
    sub     r2,r11                  ! dz
    mov     r8,r0
    add     #26,r0
    mov.w   @r0,r4                  ! yaw
    tst     r4,r4
    bt      bc_test
    add     #-16,r15
    mov.l   r1,@(4,r15)             ! dx
    mov.l   r11,@r15                ! dz
    mov.l   .Lbc_sin,r0
    jsr     @r0
    nop
    neg     r0,r0
    mov.l   r0,@(8,r15)             ! -sin
    mov     r8,r0
    add     #26,r0
    mov.w   @r0,r4
    mov.l   .Lbc_16384,r1
    add     r1,r4
    mov.l   .Lbc_sin,r0
    jsr     @r0
    nop
    mov     r0,r7                   ! cos
    mov.l   @(8,r15),r6
    mov     r15,r5
    mov     r5,r4
    add     #4,r4                   ! &dx
    mov.l   .Lbc_turn,r0
    jsr     @r0
    nop
    mov.l   @(4,r15),r1             ! local x
    mov.l   @r15,r11                ! local z
    add     #16,r15
bc_test:
    mov.w   @(0,r8),r0
    sub     r10,r0                  ! min.x - radius
    cmp/ge  r0,r1                   ! local x >= min.x - radius
    bf      bc_no
    mov     r8,r0
    add     #6,r0
    mov.w   @r0,r0
    add     r10,r0                  ! max.x + radius
    cmp/gt  r0,r1                   ! local x > max.x + radius
    bt      bc_no
    mov     r8,r0
    add     #4,r0
    mov.w   @r0,r0
    sub     r10,r0                  ! min.z - radius
    cmp/ge  r0,r11
    bf      bc_no
    mov     r8,r0
    add     #10,r0
    mov.w   @r0,r0
    add     r10,r0                  ! max.z + radius
    cmp/gt  r0,r11
    bt      bc_no
    mov     #1,r0
    bra     bc_out
    nop
bc_no:
    mov     #0,r0
bc_out:
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Lbc_sin:
    .long   _Math_Sin               ! (the coarse table too, when the mode has it)
.Lbc_turn:
    .long   _dc_turn
.Lbc_16384:
    .long   0x4000

! int32_t dc_sin_coarse(int32_t angle);   core/math/trig.c M_CoarseSin
!
! The coarse table (int16, 1025 entries, g_DC_Trig word 3): step = (angle >> 4)
! & 0xFFF; 4 * table[step], [0x800 - step], - [step - 0x800], - [0x1000 - step]
! by the quarter. (TR3's mode uses it.)
    .global _dc_sin_coarse
    .align 2
_dc_sin_coarse:
    extu.w  r4,r1
    shlr2   r1
    shlr2   r1                      ! step
    mov.l   .Lsc_tab,r3
    mov.l   @(12,r3),r3             ! the coarse table
    mov     #0,r5
    mov.l   .Lsc_0400,r2
    cmp/hs  r2,r1
    bt      sc_q1
    bra     sc_get
    nop
sc_q1:
    mov.l   .Lsc_0800,r2
    cmp/hs  r2,r1
    bt      sc_q2
    sub     r1,r2
    mov     r2,r1                   ! 0x800 - step
    bra     sc_get
    nop
sc_q2:
    mov.l   .Lsc_0c00,r2
    cmp/hs  r2,r1
    bt      sc_q3
    mov.l   .Lsc_0800,r2
    sub     r2,r1                   ! step - 0x800
    mov     #1,r5
    bra     sc_get
    nop
sc_q3:
    mov.l   .Lsc_1000,r2
    sub     r1,r2
    mov     r2,r1                   ! 0x1000 - step
    mov     #1,r5
sc_get:
    mov     r1,r0
    shll    r0
    mov.w   @(r0,r3),r0
    shll2   r0
    tst     r5,r5
    bt      sc_out
    neg     r0,r0
sc_out:
    rts
    nop
    .align 2
.Lsc_tab:
    .long   _g_DC_Trig
.Lsc_0400:
    .long   0x400
.Lsc_0800:
    .long   0x800
.Lsc_0c00:
    .long   0xC00
.Lsc_1000:
    .long   0x1000

! int dc_box_search(const int32_t *p);   pathing/box.c Box_SearchLOT (Box_SearchLOTC)
!
! The creatures' route search: it expands up to p[15] boxes from the head of the
! queue, marking the boxes it can reach from each with the search number and
! the way out, and queuing the new ones. Returns 1, or 0 when the queue ran
! empty; the head and tail go back to the LOT.
!
! p (words): 0 lot  1 node  2 zone  3 boxes  4 box count  5 overlaps
!  6 overlap count  7 fixed fly zone  8 box number mask  9 overlap flags mask
!  10 step  11 drop  12 block mask  13 can jump  14 can monkey  15 search zone
!  (from the first head)  16 expansion  17 offset of head in the LOT  18 offset
!  of tail  19 tail cleared when the queue runs dry (TR2 on)
! BOX_NODE (8 bytes): 0 exit box  2 search number  4 next expansion  6 box
! BOX_INFO (24 bytes): 16 height (int16)  20 overlap index
!
! r8 head  r9 tail  r10 nodes  r11 boxes  r12 p  r13 expansions left
! r14 the head's node;  stack: 0 the head's box  4 overlap index  8 done
    .global _dc_box_search
    .align 2
_dc_box_search:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    add     #-16,r15
    mov     r4,r12
    mov.l   @(0,r12),r1             ! lot
    mov     r12,r0
    add     #68,r0
    mov.l   @r0,r0                  ! the head's offset
    mov.w   @(r0,r1),r8             ! head
    mov     r12,r0
    add     #72,r0
    mov.l   @r0,r0                  ! the tail's offset
    mov.w   @(r0,r1),r9             ! tail
    mov.l   @(4,r12),r10
    mov.l   @(12,r12),r11
    mov     r12,r0
    add     #64,r0
    mov.l   @r0,r13                 ! expansion
    mov.l   @(8,r12),r1
    mov     r8,r0
    shll    r0
    mov.w   @(r0,r1),r0             ! zone[head], read as the C does
    mov.l   r0,@(60,r12)
bs_loop:
    cmp/pl  r13
    bf      bs_true
    mov     #-1,r0
    cmp/eq  r0,r8
    bf      bs_have
    ! the queue ran dry
    mov     r12,r0
    add     #76,r0
    mov.l   @r0,r0
    tst     r0,r0
    bt      bs_false
    mov     #-1,r9
bs_false:
    mov     #0,r0
    bra     bs_store
    nop
bs_have:
    mov     r8,r0
    shll2   r0
    shll    r0
    add     r10,r0
    mov     r0,r14                  ! node = nodes + head * 8
    mov     r8,r1
    shll2   r1
    shll    r1                      ! head * 8
    mov     r8,r2
    shll2   r2
    shll2   r2                      ! head * 16
    add     r2,r1
    add     r11,r1                  ! the head's box
    mov.l   r1,@r15
    mov.l   @(20,r1),r0
    mov.l   .Lbs_3fff,r2
    and     r2,r0                   ! overlap index
    mov.l   r0,@(4,r15)
    mov     #0,r0
    mov.l   r0,@(8,r15)             ! done = false
bs_inner:
    mov.l   @(8,r15),r0
    tst     r0,r0
    bf      bs_end                  ! while (!done)
    mov.l   @(4,r15),r1
    mov     r1,r2
    add     #1,r2
    mov.l   r2,@(4,r15)             ! index++
    mov.l   @(20,r12),r2            ! overlaps
    tst     r2,r2
    bt      bs_end
    cmp/pz  r1
    bf      bs_end
    mov.l   @(24,r12),r3
    cmp/ge  r3,r1
    bt      bs_end                  ! M_GetOverlap said NO_BOX
    mov     r1,r0
    shll    r0
    mov.w   @(r0,r2),r7             ! the overlap
    mov     #-1,r0
    cmp/eq  r0,r7
    bt      bs_end                  ! NO_BOX: break
    cmp/pz  r7
    bt      bs_notend
    mov     #1,r0
    mov.l   r0,@(8,r15)             ! BOX_END_BIT: done after this one
bs_notend:
    mov.l   @(36,r12),r0
    and     r7,r0
    mov     r0,r6                   ! the flags
    mov.l   @(32,r12),r0
    and     r7,r0
    mov     r0,r5                   ! the box number
    cmp/pz  r5
    bf      bs_inner
    mov.l   @(16,r12),r0
    cmp/ge  r0,r5
    bt      bs_inner                ! no such box
    mov     r5,r1
    shll2   r1
    shll    r1
    mov     r5,r2
    shll2   r2
    shll2   r2
    add     r2,r1
    add     r11,r1
    mov     r1,r4                   ! the box
    mov.l   @(28,r12),r0
    tst     r0,r0
    bf      bs_zone_ok              ! the fixed fly zone: no test
    mov.l   @(8,r12),r1
    mov     r5,r0
    shll    r0
    mov.w   @(r0,r1),r1             ! zone[box]
    mov.l   @(60,r12),r2
    cmp/eq  r2,r1
    bf      bs_inner                ! not the search zone
bs_zone_ok:
    mov     r4,r0
    add     #16,r0
    mov.w   @r0,r1                  ! the box's height
    mov.l   @r15,r2
    mov     r2,r0
    add     #16,r0
    mov.w   @r0,r2                  ! the head's height
    sub     r2,r1                   ! change
    mov.l   @(40,r12),r2
    cmp/gt  r2,r1                   ! change > step
    bt      bs_monkey
    mov.l   @(44,r12),r2
    cmp/gt  r1,r2                   ! drop > change
    bf      bs_height_ok
bs_monkey:
    mov.l   @(56,r12),r0
    tst     r0,r0
    bt      bs_inner                ! cannot swing: skip
    mov.l   .Lbs_2000,r1
    tst     r1,r6
    bt      bs_inner                ! the overlap is no monkey bar
bs_height_ok:
    mov.l   .Lbs_0800,r1
    tst     r1,r6
    bt      bs_nojump
    mov.l   @(52,r12),r0
    tst     r0,r0
    bt      bs_inner                ! a jump this one cannot make
bs_nojump:
    mov     r5,r0
    shll2   r0
    shll    r0
    add     r10,r0
    mov     r0,r3                   ! expand
    mov     r14,r0
    add     #2,r0
    mov.w   @r0,r1
    extu.w  r1,r1                   ! node->search_num
    mov     r3,r0
    add     #2,r0
    mov.w   @r0,r2
    extu.w  r2,r2                   ! expand->search_num
    mov.l   .Lbs_7fff,r0
    mov     r1,r7
    and     r0,r7                   ! node's number
    mov     r2,r6
    and     r0,r6                   ! expand's number
    cmp/gt  r7,r6
    bt      bs_inner                ! node < expand: the other is newer
    mov.l   .Lbs_8000,r0
    tst     r0,r1
    bt      bs_free                 ! the node is not blocked
    cmp/eq  r7,r6
    bt      bs_inner
    mov     r3,r0
    add     #2,r0
    mov.w   r1,@r0
    bra     bs_queue
    nop
bs_free:
    cmp/eq  r7,r6
    bf      bs_free2
    mov.l   .Lbs_8000,r0
    tst     r0,r2
    bt      bs_inner                ! same number and not blocked
bs_free2:
    mov.l   @(20,r4),r0
    mov.l   @(48,r12),r2
    tst     r2,r0
    bt      bs_open                 ! (overlap & block mask) == 0
    mov.l   .Lbs_8000,r0
    or      r1,r0
    mov     r3,r2
    add     #2,r2
    mov.w   r0,@r2                  ! the box is blocked from here
    bra     bs_queue
    nop
bs_open:
    mov     r3,r0
    add     #2,r0
    mov.w   r1,@r0
    mov.w   r8,@r3                  ! exit box = the head
bs_queue:
    mov     r3,r0
    add     #4,r0
    mov.w   @r0,r1
    mov     #-1,r2
    cmp/eq  r2,r1
    bf      bs_inner
    cmp/eq  r9,r5
    bt      bs_inner
    mov     r9,r0
    shll2   r0
    shll    r0
    add     r10,r0
    add     #4,r0
    mov.w   r5,@r0                  ! node[tail].next expansion = box
    mov     r5,r9
    bra     bs_inner
    nop
bs_end:
    mov     r14,r0
    add     #4,r0
    mov.w   @r0,r8                  ! head = node->next expansion
    mov     #-1,r1
    mov.w   r1,@r0
    add     #-1,r13
    bra     bs_loop
    nop
bs_true:
    mov     #1,r0
bs_store:
    mov     r0,r7                   ! the result
    mov.l   @(0,r12),r1             ! lot
    mov     r12,r0
    add     #68,r0
    mov.l   @r0,r0
    mov.w   r8,@(r0,r1)
    mov     r12,r0
    add     #72,r0
    mov.l   @r0,r0
    mov.w   r9,@(r0,r1)
    mov     r7,r0
    add     #16,r15
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .align 2
.Lbs_3fff:
    .long   0x3FFFFFFF
.Lbs_2000:
    .long   0x2000
.Lbs_0800:
    .long   0x0800
.Lbs_7fff:
    .long   0x7FFF
.Lbs_8000:
    .long   0x8000

! int32_t dc_creature_turn(const int32_t *p);   creature/common.c Creature_Turn
!
! The turn a creature walking somewhere makes this tick: the angle to its
! target less the way it faces (cut to 16 bits), the turn eased to half when
! the target lies behind and is nearer than the speed asks, and the angle kept
! within +-max_turn. p: 0 dx  1 dz  2 rot.y  3 speed  4 max_turn (not 0).
! Returns the angle; the caller adds it to rot.y.
    .global _dc_creature_turn
    .align 2
_dc_creature_turn:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   @(0,r4),r8              ! dx
    mov.l   @(4,r4),r9              ! dz
    mov.l   @(8,r4),r10             ! rot.y
    mov.l   @(12,r4),r11            ! speed
    mov.l   @(16,r4),r12            ! max_turn
    mov     r9,r4
    mov     r8,r5
    mov.l   .Lct_atan,r0
    jsr     @r0                     ! Math_Atan(dz, dx)
    nop
    sub     r10,r0
    exts.w  r0,r13                  ! angle
    mov.l   .Lct_arc,r1             ! FRONT_ARC
    cmp/gt  r1,r13
    bt      ct_range
    neg     r1,r1
    cmp/gt  r13,r1                  ! angle < -FRONT_ARC
    bf      ct_clamp
ct_range:
    mov     r11,r4
    shll8   r4
    shll2   r4
    shll2   r4
    shll2   r4                      ! speed << 14
    mov     r12,r5
    mov.l   .Lct_div,r0
    jsr     @r0                     ! / max_turn
    nop
    mul.l   r0,r0
    sts     macl,r0                 ! range^2
    mul.l   r8,r8
    sts     macl,r1
    mul.l   r9,r9
    sts     macl,r2
    add     r2,r1                   ! dx^2 + dz^2
    cmp/gt  r1,r0                   ! d^2 < range^2
    bf      ct_clamp
    cmp/pz  r12
    bt      ct_half
    add     #1,r12                  ! toward zero
ct_half:
    shar    r12
    exts.w  r12,r12
ct_clamp:
    neg     r12,r1                  ! -max_turn
    cmp/gt  r13,r1                  ! angle < -max_turn
    bf      ct_hi
    mov     r1,r13
    bra     ct_out
    nop
ct_hi:
    cmp/gt  r12,r13                 ! angle > max_turn
    bf      ct_out
    mov     r12,r13
ct_out:
    mov     r13,r0
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Lct_atan:
    .long   _Math_Atan
.Lct_div:
    .long   ___sdivsi3_i4i
.Lct_arc:
    .long   0x4000

! int dc_box_target(int32_t *p);   pathing/box.c Box_CalculateTarget's walk (M_TargetWalkC)
!
! The walk along the route from the box the creature stands in towards the
! box it is to reach, taking its target (x y z) along the sides of the boxes
! that lie between. Returns 1 when the target box was reached (TARGET_PRIMARY),
! 2 when a secondary target was set (TARGET_SECONDARY), or 0 when the walk ran
! out, with the last box and prime_free left in p for the C to finish.
!
! p (bytes): 0 pos x  4 pos z  8 pair clip  12 fly  16 block mask  20 target
! box  24 lot target x  28 y  32 z  36 boxes  40 (box count)  44 nodes
! 48 target (XYZ_32*)  52 left  56 right  60 top  64 bottom  68 first box
! 72 out: last box  76 out: prime_free
! BOX_INFO (24 bytes): 0 left 4 right 8 top 12 bottom 16 height (int16) 20 overlap
! BOX_NODE (8 bytes): 0 exit box
!
! r8 target  r9 box  r10 box number  r11 prime_free  r12 p  r13 p + 64
    .macro CLAMPL off, base, val
    mov.l   @(\off,\base),r1
    cmp/gt  r1,\val
    bf      9f
    mov.l   \val,@(\off,\base)
9:
    .endm
    .macro CLAMPG off, base, val
    mov.l   @(\off,\base),r1
    cmp/gt  \val,r1
    bf      9f
    mov.l   \val,@(\off,\base)
9:
    .endm
    .global _dc_box_target
    .align 2
_dc_box_target:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   r14,@-r15
    mov     r4,r12
    mov     r12,r13
    add     #64,r13
    mov.l   @(48,r12),r8
    mov.l   @(4,r13),r10
    mov     #15,r11
bt_loop:
    mov.l   @(36,r12),r9
    mov     r10,r1
    shll2   r1
    shll    r1                      ! * 8
    mov     r10,r0
    shll2   r0
    shll2   r0                      ! * 16
    add     r0,r1
    add     r1,r9                   ! the box
    mov     r9,r0
    add     #16,r0
    mov.w   @r0,r2                  ! its height
    mov.l   @(12,r12),r0
    tst     r0,r0
    bt      bt_ground
    mov.l   .Lbt_wall,r1
    sub     r1,r2
bt_ground:
    CLAMPG  4, r8, r2               ! target.y
    mov.l   @(4,r12),r3             ! pos z
    mov.l   @(0,r12),r4             ! pos x
    mov.l   @(0,r9),r5              ! left
    mov.l   @(4,r9),r6              ! right
    mov.l   @(8,r9),r7              ! top
    mov.l   @(12,r9),r2             ! bottom
    cmp/ge  r5,r3
    bf      bt_beside
    cmp/gt  r6,r3
    bt      bt_beside
    cmp/ge  r7,r4
    bf      bt_beside
    cmp/gt  r2,r4
    bt      bt_beside
    mov.l   r5,@(52,r12)
    mov.l   r6,@(56,r12)
    mov.l   r7,@(60,r12)
    mov.l   r2,@(0,r13)
    bra     bt_check
    nop
bt_beside:
    ! the z side
    cmp/ge  r5,r3
    bt      bt_not_left
    mov     r11,r0
    tst     #1,r0
    bt      bt_zl_else
    cmp/ge  r7,r4
    bf      bt_zl_else
    cmp/gt  r2,r4
    bt      bt_zl_else
    mov.l   .Lbt_biff,r0
    add     r5,r0
    CLAMPL  8, r8, r0               ! target.z at least left + BIFF
    mov     r11,r0
    tst     #16,r0
    bf      bt_secondary
    CLAMPL  60, r12, r7
    CLAMPG  0, r13, r2
    mov     #1,r11
    bra     bt_xside
    nop
bt_zl_else:
    mov     #1,r0
    cmp/eq  r0,r11
    bt      bt_xside
    mov.l   @(8,r12),r0
    tst     r0,r0
    bt      bt_zl_do
    mov     #2,r0
    cmp/eq  r0,r11
    bt      bt_xside
bt_zl_do:
    mov.l   @(56,r12),r0
    mov.l   .Lbt_biff,r1
    sub     r1,r0
    mov.l   r0,@(8,r8)              ! target.z = right - BIFF
    mov     #15,r0
    cmp/eq  r0,r11
    bf      bt_secondary
    mov     r11,r0
    or      #16,r0
    mov     r0,r11
    bra     bt_xside
    nop
bt_not_left:
    cmp/gt  r6,r3
    bf      bt_xside
    mov     r11,r0
    tst     #2,r0
    bt      bt_zr_else
    cmp/ge  r7,r4
    bf      bt_zr_else
    cmp/gt  r2,r4
    bt      bt_zr_else
    mov     r6,r0
    mov.l   .Lbt_biff,r1
    sub     r1,r0
    CLAMPG  8, r8, r0               ! target.z at most right - BIFF
    mov     r11,r0
    tst     #16,r0
    bf      bt_secondary
    CLAMPL  60, r12, r7
    CLAMPG  0, r13, r2
    mov     #2,r11
    bra     bt_xside
    nop
bt_zr_else:
    mov     #2,r0
    cmp/eq  r0,r11
    bt      bt_xside
    mov.l   @(8,r12),r0
    tst     r0,r0
    bt      bt_zr_do
    mov     #1,r0
    cmp/eq  r0,r11
    bt      bt_xside
bt_zr_do:
    mov.l   @(52,r12),r0
    mov.l   .Lbt_biff,r1
    add     r1,r0
    mov.l   r0,@(8,r8)              ! target.z = left + BIFF
    mov     #15,r0
    cmp/eq  r0,r11
    bf      bt_secondary
    mov     r11,r0
    or      #16,r0
    mov     r0,r11
bt_xside:
    cmp/ge  r7,r4
    bt      bt_not_top
    mov     r11,r0
    tst     #4,r0
    bt      bt_xt_else
    cmp/ge  r5,r3
    bf      bt_xt_else
    cmp/gt  r6,r3
    bt      bt_xt_else
    mov.l   .Lbt_biff,r0
    add     r7,r0
    CLAMPL  0, r8, r0               ! target.x at least top + BIFF
    mov     r11,r0
    tst     #16,r0
    bf      bt_secondary
    CLAMPL  52, r12, r5
    CLAMPG  56, r12, r6
    mov     #4,r11
    bra     bt_check
    nop
bt_xt_else:
    mov     #4,r0
    cmp/eq  r0,r11
    bt      bt_check
    mov.l   @(8,r12),r0
    tst     r0,r0
    bt      bt_xt_do
    mov     #8,r0
    cmp/eq  r0,r11
    bt      bt_check
bt_xt_do:
    mov.l   @(0,r13),r0
    mov.l   .Lbt_biff,r1
    sub     r1,r0
    mov.l   r0,@(0,r8)              ! target.x = bottom - BIFF
    mov     #15,r0
    cmp/eq  r0,r11
    bf      bt_secondary
    mov     r11,r0
    or      #16,r0
    mov     r0,r11
    bra     bt_check
    nop
bt_not_top:
    cmp/gt  r2,r4
    bf      bt_check
    mov     r11,r0
    tst     #8,r0
    bt      bt_xb_else
    cmp/ge  r5,r3
    bf      bt_xb_else
    cmp/gt  r6,r3
    bt      bt_xb_else
    mov     r2,r0
    mov.l   .Lbt_biff,r1
    sub     r1,r0
    CLAMPG  0, r8, r0               ! target.x at most bottom - BIFF
    mov     r11,r0
    tst     #16,r0
    bf      bt_secondary
    CLAMPL  52, r12, r5
    CLAMPG  56, r12, r6
    mov     #8,r11
    bra     bt_check
    nop
bt_xb_else:
    mov     #8,r0
    cmp/eq  r0,r11
    bt      bt_check
    mov.l   @(8,r12),r0
    tst     r0,r0
    bt      bt_xb_do
    mov     #4,r0
    cmp/eq  r0,r11
    bt      bt_check
bt_xb_do:
    mov.l   @(60,r12),r0
    mov.l   .Lbt_biff,r1
    add     r1,r0
    mov.l   r0,@(0,r8)              ! target.x = top + BIFF
    mov     #15,r0
    cmp/eq  r0,r11
    bf      bt_secondary
    mov     r11,r0
    or      #16,r0
    mov     r0,r11
bt_check:
    mov.l   @(20,r12),r0
    cmp/eq  r0,r10
    bf      bt_next
    ! the target box: the target, clamped inside it
    mov     r11,r0
    tst     #3,r0
    bt      bt_pz
    mov.l   @(32,r12),r0
    mov.l   r0,@(8,r8)
    bra     bt_px
    nop
bt_pz:
    mov     r11,r0
    tst     #16,r0
    bf      bt_px
    mov.l   .Lbt_biff,r1
    mov     r5,r0
    add     r1,r0                   ! left + BIFF
    mov.l   @(8,r8),r3
    cmp/gt  r3,r0
    bf      bt_pz_hi
    mov     r0,r3
    bra     bt_pz_st
    nop
bt_pz_hi:
    mov     r6,r4
    sub     r1,r4                   ! right - BIFF
    cmp/gt  r4,r3
    bf      bt_pz_st
    mov     r4,r3
bt_pz_st:
    mov.l   r3,@(8,r8)
bt_px:
    mov     r11,r0
    tst     #12,r0
    bt      bt_pxc
    mov.l   @(24,r12),r0
    mov.l   r0,@(0,r8)
    bra     bt_py
    nop
bt_pxc:
    mov     r11,r0
    tst     #16,r0
    bf      bt_py
    mov.l   .Lbt_biff,r1
    mov     r7,r0
    add     r1,r0                   ! top + BIFF
    mov.l   @(0,r8),r3
    cmp/gt  r3,r0
    bf      bt_px_hi
    mov     r0,r3
    bra     bt_px_st
    nop
bt_px_hi:
    mov     r2,r4
    sub     r1,r4                   ! bottom - BIFF
    cmp/gt  r4,r3
    bf      bt_px_st
    mov     r4,r3
bt_px_st:
    mov.l   r3,@(0,r8)
bt_py:
    mov.l   @(28,r12),r0
    mov.l   r0,@(4,r8)
    mov     #1,r0
    bra     bt_out
    nop
bt_next:
    mov.l   @(44,r12),r1
    mov     r10,r0
    shll2   r0
    shll    r0
    add     r0,r1
    mov.w   @r1,r10                 ! the exit box
    mov     #-1,r0
    cmp/eq  r0,r10
    bt      bt_ran_out
    mov.l   @(36,r12),r1
    mov     r10,r2
    shll2   r2
    shll    r2
    mov     r10,r3
    shll2   r3
    shll2   r3
    add     r3,r2
    add     r2,r1
    mov.l   @(20,r1),r0
    mov.l   @(16,r12),r1
    tst     r1,r0
    bt      bt_loop                 ! not blocked: the next box
bt_ran_out:
    mov.l   r9,@(8,r13)
    mov.l   r11,@(12,r13)
    mov     #0,r0
    bra     bt_out
    nop
bt_secondary:
    mov     #2,r0
bt_out:
    mov.l   @r15+,r14
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .align 2
.Lbt_wall:
    .long   1024
.Lbt_biff:
    .long   512

! int dc_anim_change(const int32_t *p);   items/anim.c Item_GetAnimChange
!
! Whether the item's animation moves on to another (the goal state differs and
! a change of the animation lists a range its frame lies in), and then to
! which animation and frame: written to the item. p: 0 item  4 num_changes
! 8 the animation's first ANIM_CHANGE (6 bytes: goal state, number of ranges,
! first range)  12 the ranges (ANIM_RANGE, 8 bytes: start, end, link animation,
! link frame)  16 offset of current_anim_state in the item  20 of
! goal_anim_state  24 of frame_num  28 of anim_num (all int16).
    .global _dc_anim_change
    .align 2
_dc_anim_change:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   @(0,r4),r8              ! item
    mov.l   @(16,r4),r0
    mov.w   @(r0,r8),r1             ! current state
    mov.l   @(20,r4),r0
    mov.w   @(r0,r8),r2             ! goal state
    cmp/eq  r1,r2
    bt      ac_no
    mov.l   @(24,r4),r0
    mov.w   @(r0,r8),r3             ! frame number
    mov.l   @(4,r4),r5              ! changes left
    mov.l   @(8,r4),r6              ! the change
    mov.l   @(12,r4),r9             ! the ranges
ac_change:
    cmp/pl  r5
    bf      ac_no
    mov.w   @r6,r0                  ! goal state of the change
    cmp/eq  r0,r2
    bf      ac_next
    mov     r6,r1
    add     #2,r1
    mov.w   @r1,r7                  ! number of ranges
    add     #2,r1
    mov.w   @r1,r0                  ! first range
    shll2   r0
    shll    r0
    add     r9,r0                   ! the range
    mov     r0,r1
ac_range:
    cmp/pl  r7
    bf      ac_next
    mov.w   @r1,r0                  ! start
    cmp/ge  r0,r3                   ! frame >= start
    bf      ac_nrange
    mov     r1,r0
    add     #2,r0
    mov.w   @r0,r0                  ! end
    cmp/gt  r0,r3                   ! frame > end
    bt      ac_nrange
    mov     r1,r0
    add     #4,r0
    mov.w   @r0,r2                  ! link animation
    mov     r1,r0
    add     #6,r0
    mov.w   @r0,r3                  ! link frame
    mov.l   @(28,r4),r0
    mov.w   r2,@(r0,r8)             ! anim_num
    mov.l   @(24,r4),r0
    mov.w   r3,@(r0,r8)             ! frame_num
    mov     #1,r0
    bra     ac_out
    nop
ac_nrange:
    add     #8,r1
    add     #-1,r7
    bra     ac_range
    nop
ac_next:
    add     #6,r6
    add     #-1,r5
    bra     ac_change
    nop
ac_no:
    mov     #0,r0
ac_out:
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8

! void dc_ai_angles(const int32_t *p);   creature/common.c Creature_AIInfo
!
! The angles a creature works out about its enemy. p in: 0 x  1 z  2 y
! (as the C has them)  3 item rot.y  4 enemy rot.y. p out: 5 angle (to the
! enemy, less the way it faces)  6 enemy_facing  7 x_angle (all 16 bits).
    .global _dc_ai_angles
    .align 2
_dc_ai_angles:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov     r4,r8
    mov.l   @(0,r8),r9              ! x
    mov.l   @(4,r8),r10             ! z
    mov.l   @(8,r8),r11             ! y
    mov     r10,r4
    mov     r9,r5
    mov.l   .Lai_atan,r12
    jsr     @r12                    ! Math_Atan(z, x)
    nop
    exts.w  r0,r1                   ! angle (16 bits)
    mov.l   @(12,r8),r2
    mov     r1,r3
    sub     r2,r3
    exts.w  r3,r3
    mov.l   r3,@(20,r8)             ! angle - item rot.y
    mov.l   @(16,r8),r2
    mov     r1,r3
    sub     r2,r3
    mov.l   .Lai_180,r2
    add     r2,r3
    exts.w  r3,r3
    mov.l   r3,@(24,r8)             ! angle - enemy rot.y + 180 degrees
    cmp/pz  r9
    bt      ai_xp
    neg     r9,r9
ai_xp:
    cmp/pz  r10
    bt      ai_zp
    neg     r10,r10
ai_zp:
    cmp/gt  r10,r9                  ! x > z
    bf      ai_zx
    mov     r10,r2
    shar    r2
    add     r9,r2                   ! x + (z >> 1)
    bra     ai_at
    nop
ai_zx:
    mov     r9,r2
    shar    r2
    add     r10,r2                  ! z + (x >> 1)
ai_at:
    mov     r2,r4
    mov     r11,r5
    jsr     @r12                    ! Math_Atan(.., y)
    nop
    exts.w  r0,r0
    mov.l   r0,@(28,r8)
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Lai_atan:
    .long   _Math_Atan
.Lai_180:
    .long   0x8000

! int32_t dc_surface_height(const SURFACE *s, int32_t x, int32_t z, const int32_t *p);
!   rooms/geometry.c M_GetSurfaceHeight (M_GetSurfaceHeightC)
!
! The height of a floor or ceiling at (x, z): flat, tilted, or split in two
! triangles. The Dreamcast's narrow SURFACE: 0 height  4 tilt x, z (int16) or
! the split's address  8 type  9 is_split; SPLIT: 0 h1  2 h2 (int16)  4 tilts
! (int8 x4)  8 type. p: 0 fix_tilts  4 camera is chunky  8 &m_HeightType
! 12 NO_HEIGHT. Sets the height type as the C does.
!
! r8 height  r9 is_split  r10 is a ceiling  r11 shift  r12 split / slope type
! r13 &height type  r1 tilt x  r2 tilt z  r5 x  r6 z
    .macro TNEG t, h
    mov.l   .Lsf_mask,r0
    and     \h,r0
    mul.l   \t,r0
    sts     macl,r0
    shar    r0
    shar    r0
    .endm
    .macro TPOS t, h
    mov.l   .Lsf_mask,r3
    sub     \h,r3
    mov.l   .Lsf_mask,r0
    and     r3,r0
    mul.l   \t,r0
    sts     macl,r0
    shar    r0
    shar    r0
    .endm
    .macro LDT n, reg
    mov     r12,r0
    add     #4+\n,r0
    mov.b   @r0,r0
    mov     r0,\reg
    .endm
    .global _dc_surface_height
    .align 2
_dc_surface_height:
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   r10,@-r15
    mov.l   r11,@-r15
    mov.l   r12,@-r15
    mov.l   r13,@-r15
    mov.l   @r4,r8                  ! height
    mov     r4,r0
    add     #9,r0
    mov.b   @r0,r9                  ! is_split
    mov     r4,r0
    add     #8,r0
    mov.b   @r0,r10                 ! type (0 floor, 1 ceiling)
    mov.l   @(8,r7),r13
    mov.l   @(12,r7),r1
    cmp/eq  r1,r8
    bf      sf_have
    tst     r9,r9
    bf      sf_ret                  ! NO_HEIGHT stays
    mov.l   @r7,r1
    tst     r1,r1
    bf      sf_ret
sf_have:
    mov     #0,r11                  ! the shift
    tst     r9,r9
    bf      sf_split
    ! --- not split
    mov     r4,r0
    add     #4,r0
    mov.w   @r0+,r1                 ! tilt x
    mov.w   @r0,r2                  ! tilt z
    mov     r1,r3
    or      r2,r3
    tst     r3,r3
    bt      sf_ret                  ! no tilt: the height as is
    mov     #1,r12                  ! small slope, unless a tilt is above 2
    mov     r2,r3
    cmp/pz  r3
    bt      sf_a1
    neg     r3,r3
sf_a1:
    mov     #2,r0
    cmp/gt  r0,r3
    bt      sf_big
    mov     r1,r3
    cmp/pz  r3
    bt      sf_a2
    neg     r3,r3
sf_a2:
    mov     #2,r0
    cmp/gt  r0,r3
    bf      sf_tilt
sf_big:
    mov     #2,r12                  ! big slope
    mov.l   @(4,r7),r0
    tst     r0,r0
    bf      sf_ret                  ! chunky camera: the height as is
    bra     sf_tilt
    nop
    ! --- split in two triangles
sf_split:
    mov.l   @(4,r4),r12             ! the split
    mov.l   @(4,r7),r0
    tst     r0,r0
    bt      sf_split_go
    mov.w   @(2,r12),r0             ! chunky: the extreme of the two triangles
    add     r8,r0                   ! height + h2
    mov     r0,r1
    mov.w   @r12,r0
    add     r8,r0                   ! height + h1
    tst     r10,r10
    bf      sf_ch_ceil
    cmp/gt  r1,r0                   ! floor: MIN
    bt      sf_ch_r1
    mov     r0,r8
    bra     sf_ret
    nop
sf_ch_ceil:
    cmp/gt  r0,r1                   ! ceiling: MAX
    bt      sf_ch_r1
    mov     r0,r8
    bra     sf_ret
    nop
sf_ch_r1:
    mov     r1,r8
    bra     sf_ret
    nop
sf_split_go:
    tst     r10,r10
    bf      sf_sg1
    mov     #4,r0
    mov.l   r0,@r13                 ! HT_SPLIT_TRI
sf_sg1:
    mov.l   .Lsf_mask,r0
    and     r0,r5                   ! dx
    and     r0,r6                   ! dz
    mov     r12,r0
    add     #8,r0
    mov.b   @r0,r0
    extu.b  r0,r4                   ! the split's type
    LDT     0, r1
    LDT     1, r2
    sub     r2,r1                   ! t0 - t1
    LDT     3, r3
    LDT     2, r2
    sub     r2,r3                   ! t3 - t2
    mov     #1,r0
    cmp/eq  r0,r4
    bt      sf_nwse
    mov     #3,r0
    cmp/eq  r0,r4
    bt      sf_nwse
    mov     #4,r0
    cmp/eq  r0,r4
    bt      sf_nwse
    cmp/gt  r6,r5                   ! dx > dz
    bt      sf_p1b
    bra     sf_p2a
    nop
sf_nwse:
    mov.l   .Lsf_wall,r0
    sub     r6,r0
    cmp/gt  r0,r5                   ! dx > WALL_L - dz
    bt      sf_p1a
    bra     sf_p2b
    nop
sf_p1a:                             ! tilt x: ceiling t0 - t1, floor t3 - t2
    tst     r10,r10
    bt      sf_p1a_f
    bra     sf_p1
    nop
sf_p1a_f:
    mov     r3,r1
    bra     sf_p1
    nop
sf_p1b:                             ! tilt x: ceiling t3 - t2, floor t0 - t1
    tst     r10,r10
    bt      sf_p1
    mov     r3,r1
sf_p1:
    LDT     3, r2
    LDT     0, r4
    sub     r4,r2                   ! tilt z = t3 - t0
    mov.w   @r12,r0
    mov     r0,r11                  ! shift = h1
    bra     sf_stilt
    nop
sf_p2a:                             ! tilt x: ceiling t0 - t1, floor t3 - t2
    tst     r10,r10
    bf      sf_p2
    mov     r3,r1
    bra     sf_p2
    nop
sf_p2b:                             ! tilt x: ceiling t3 - t2, floor t0 - t1
    tst     r10,r10
    bt      sf_p2
    mov     r3,r1
sf_p2:
    LDT     2, r2
    LDT     1, r4
    sub     r4,r2                   ! tilt z = t2 - t1
    mov.w   @(2,r12),r0
    mov     r0,r11                  ! shift = h2
sf_stilt:
    ! the height type of a floor is told after the shift (r12 not needed)
    ! --- the tilt's shift (r1 tilt x, r2 tilt z, r5 x, r6 z)
sf_tilt:
    tst     r10,r10
    bf      sf_ceil
    cmp/pz  r2                      ! floor: tilt z
    bt      sf_fz_pos
    TNEG    r2, r6
    sub     r0,r11
    bra     sf_fx
    nop
sf_fz_pos:
    TPOS    r2, r6
    add     r0,r11
sf_fx:
    cmp/pz  r1
    bt      sf_fx_pos
    TNEG    r1, r5
    sub     r0,r11
    bra     sf_shifted
    nop
sf_fx_pos:
    TPOS    r1, r5
    add     r0,r11
    bra     sf_shifted
    nop
sf_ceil:
    cmp/pz  r2
    bt      sf_cz_pos
    TNEG    r2, r6
    add     r0,r11
    bra     sf_cx
    nop
sf_cz_pos:
    TPOS    r2, r6
    sub     r0,r11
sf_cx:
    cmp/pz  r1
    bt      sf_cx_pos
    TPOS    r1, r5
    add     r0,r11
    bra     sf_shifted
    nop
sf_cx_pos:
    TNEG    r1, r5
    sub     r0,r11
sf_shifted:
    add     r11,r8                  ! height += shift
    tst     r10,r10
    bf      sf_ret                  ! a ceiling: no height type
    tst     r9,r9
    bt      sf_unsplit_ht
    ! split floor: HT_DIAGONAL for a steep tilt, else the small slope
    mov     r1,r3
    cmp/pz  r3
    bt      sf_b1
    neg     r3,r3
sf_b1:
    mov     #2,r0
    cmp/gt  r0,r3
    bt      sf_diag
    mov     r2,r3
    cmp/pz  r3
    bt      sf_b2
    neg     r3,r3
sf_b2:
    mov     #2,r0
    cmp/gt  r0,r3
    bt      sf_diag
    mov.l   @r13,r0
    mov     #4,r3
    cmp/eq  r3,r0
    bt      sf_ret                  ! still HT_SPLIT_TRI
    mov     #1,r0
    mov.l   r0,@r13
    bra     sf_ret
    nop
sf_diag:
    mov     #3,r0
    mov.l   r0,@r13
    bra     sf_ret
    nop
sf_unsplit_ht:
    mov.l   r12,@r13                ! the slope type
sf_ret:
    mov     r8,r0
    mov.l   @r15+,r13
    mov.l   @r15+,r12
    mov.l   @r15+,r11
    mov.l   @r15+,r10
    mov.l   @r15+,r9
    rts
    mov.l   @r15+,r8
    .align 2
.Lsf_mask:
    .long   1023
.Lsf_wall:
    .long   1024

! int32_t dc_distance(const XYZ_32 *a, const XYZ_32 *b);   core/math/geom.c XYZ_32_GetDistance
!
! The distance between two points, when each of the three differences is within
! +-32767 (the sum of the squares then fits 32 bits: floor(sqrt) by dc_sqrt32).
! Returns -1 when it does not (the C works in 64 bits there).
    .global _dc_distance
    .align 2
_dc_distance:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    mov.l   r9,@-r15
    mov.l   .Ldd_65534,r6
    mov.l   .Ldd_32767,r7
    mov     #0,r8                   ! the sum of squares
    mov     #3,r2                   ! axes left
dd_axis:
    mov.l   @r4+,r1                 ! a
    mov.l   @r5+,r3                 ! b
    mov     r1,r9                   ! a
    mov     r1,r0
    xor     r3,r0                   ! a ^ b
    sub     r3,r1                   ! d = a - b (32 bits)
    xor     r1,r9                   ! a ^ d
    and     r9,r0                   ! (a ^ b) & (a ^ d): the sign says an overflow
    cmp/pz  r0
    bf      dd_fail
    mov     r1,r3
    add     r7,r3
    cmp/hi  r6,r3                   ! d + 32767 > 65534
    bt      dd_fail
    mul.l   r1,r1
    sts     macl,r3
    add     r3,r8
    add     #-1,r2
    cmp/pl  r2
    bt      dd_axis
    mov     r8,r4
    mov.l   .Ldd_sqrt,r0
    jsr     @r0
    nop
    bra     dd_out
    nop
dd_fail:
    mov     #-1,r0
dd_out:
    mov.l   @r15+,r9
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
    .align 2
.Ldd_65534:
    .long   65534
.Ldd_32767:
    .long   32767
.Ldd_sqrt:
    .long   _dc_sqrt32
