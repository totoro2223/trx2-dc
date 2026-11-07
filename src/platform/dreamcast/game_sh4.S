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
