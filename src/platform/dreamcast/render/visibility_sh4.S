! Hand-written SH-4 for the room visibility tests of rooms/draw.c.
!
!   int dc_portal_faces(const int32_t *room_pos, const PORTAL *portal,
!                       const int32_t *view_pos);
!
! M_PortalFacesCamera: sum over x, y, z of normal * (room_pos + vertex[0] -
! view_pos), in 32-bit integers as C does it, less than 0. PORTAL: room_num
! int16 at 0, normal x y z (int16) at 2 4 6, vertex[4] (x y z, int16) from 8.
    .text
    .align 2
    .global _dc_portal_faces
_dc_portal_faces:
    mov     #0,r3
    mov.l   @r4,r1                  ! room x
    mov.w   @(8,r5),r0
    add     r0,r1
    mov.l   @r6,r2
    sub     r2,r1
    mov.w   @(2,r5),r0
    mul.l   r0,r1
    sts     macl,r1
    add     r1,r3
    mov.l   @(4,r4),r1              ! y
    mov.w   @(10,r5),r0
    add     r0,r1
    mov.l   @(4,r6),r2
    sub     r2,r1
    mov.w   @(4,r5),r0
    mul.l   r0,r1
    sts     macl,r1
    add     r1,r3
    mov.l   @(8,r4),r1              ! z
    mov.w   @(12,r5),r0
    add     r0,r1
    mov.l   @(8,r6),r2
    sub     r2,r1
    mov.w   @(6,r5),r0
    mul.l   r0,r1
    sts     macl,r1
    add     r1,r3
    cmp/pz  r3
    movt    r0
    xor     #1,r0                   ! 1 when the sum is below 0
    rts
    nop
