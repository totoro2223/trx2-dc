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
