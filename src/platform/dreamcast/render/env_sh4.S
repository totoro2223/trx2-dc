! Hand-written SH-4 for the environment reflection of one vertex
! (batcher.c, M_EnvVertex).
!
!   void dc_env_vertex(M_ENVRUN *run);
!
! The normal is turned to eye space and to face the camera, the view direction
! is reflected about it, and the sphere map's (u, v) plus the reflection's
! colour (packed ARGB) are written. Two reciprocal roots (fsrra) in place of
! the divisions.
!
! M_ENVRUN (words): 0 env_uv (2 floats out)  4 eargb (word out)
!   8 nx 12 ny 16 nz  20 m0 (3) 32 m1 (3) 44 m2 (3)  56 x 60 y 64 z
!   68 keep  72 lit (3)  84 strength  88 env_x 92 env_y  96 0.5 100 2.0 104 255
!
! r8 run  fr8 nx  fr9 ny  fr10 nz  fr11 x  fr12 y  fr13 z  fr14 keep
! fr15 strength
! frame: 0 n0 4 n1 8 n2 12 d_n 16 d0 20 d1 24 d2 28 eu 32 ev

    .macro ENVCHAN litoff, shift
    mov     #\litoff,r0
    fmov.s  @(r0,r8),fr1    ! lit
    mov     #96,r0
    fmov.s  @(r0,r8),fr2    ! 0.5
    fmul    fr2,fr1         ! lit * 0.5
    fadd    fr2,fr1         ! 0.5 + 0.5 * lit
    fmul    fr15,fr1        ! * strength
    fmul    fr14,fr1        ! * keep = e
    fldi0   fr2
    fcmp/gt fr2,fr1         ! e > 0
    bf      1f
    fldi1   fr2
    fcmp/gt fr1,fr2         ! 1 > e
    bt      2f
    fmov    fr2,fr1         ! e = 1
    bra     2f
    nop
1:  fldi0   fr1             ! e = 0
2:  mov     #104,r0
    fmov.s  @(r0,r8),fr2    ! 255
    fmul    fr2,fr1
    ftrc    fr1,fpul
    sts     fpul,r0
    .if \shift == 16
    shll16  r0
    .elseif \shift == 8
    shll8   r0
    .endif
    or      r0,r6
    .endm

    .global _dc_env_vertex
_dc_env_vertex:
    sts.l   pr,@-r15
    mov.l   r8,@-r15
    fmov.s  fr12,@-r15
    fmov.s  fr13,@-r15
    fmov.s  fr14,@-r15
    fmov.s  fr15,@-r15
    add     #-48,r15
    mov     r4,r8
    mov     #8,r0
    fmov.s  @(r0,r8),fr8    ! nx
    mov     #12,r0
    fmov.s  @(r0,r8),fr9    ! ny
    mov     #16,r0
    fmov.s  @(r0,r8),fr10   ! nz
    mov     #56,r0
    fmov.s  @(r0,r8),fr11   ! x
    mov     #60,r0
    fmov.s  @(r0,r8),fr12   ! y
    mov     #64,r0
    fmov.s  @(r0,r8),fr13   ! z
    mov     #68,r0
    fmov.s  @(r0,r8),fr14   ! keep
    mov     #84,r0
    fmov.s  @(r0,r8),fr15   ! strength

    ! the normal in eye space: n[k] = m[k] . (nx, ny, nz), w = 0
    fmov    fr8,fr4
    fmov    fr9,fr5
    fmov    fr10,fr6
    fldi0   fr7             ! w
    mov     #20,r0
    fmov.s  @(r0,r8),fr0
    mov     #24,r0
    fmov.s  @(r0,r8),fr1
    mov     #28,r0
    fmov.s  @(r0,r8),fr2
    fipr    fv4,fv0         ! fr3 = n0
    fmov.s  fr3,@r15
    mov     #32,r0
    fmov.s  @(r0,r8),fr0
    mov     #36,r0
    fmov.s  @(r0,r8),fr1
    mov     #40,r0
    fmov.s  @(r0,r8),fr2
    fipr    fv4,fv0         ! fr3 = n1
    mov     #4,r0
    fmov.s  fr3,@(r0,r15)
    mov     #44,r0
    fmov.s  @(r0,r8),fr0
    mov     #48,r0
    fmov.s  @(r0,r8),fr1
    mov     #52,r0
    fmov.s  @(r0,r8),fr2
    fipr    fv4,fv0         ! fr3 = n2
    mov     #8,r0
    fmov.s  fr3,@(r0,r15)

    ! d_len_sq, n_len_sq
    fmov    fr11,fr0
    fmul    fr11,fr0
    fmov    fr12,fr1
    fmul    fr12,fr1
    fadd    fr1,fr0
    fmov    fr13,fr1
    fmul    fr13,fr1
    fadd    fr1,fr0         ! d_len_sq in fr0
    fmov.s  @r15,fr1
    fmul    fr1,fr1
    mov     #4,r0
    fmov.s  @(r0,r15),fr2
    fmul    fr2,fr2
    fadd    fr2,fr1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2
    fmul    fr2,fr2
    fadd    fr2,fr1         ! n_len_sq in fr1

    fldi0   fr2
    fcmp/gt fr2,fr0         ! d_len_sq > 0
    bf      env_noreflect
    fcmp/gt fr2,fr1         ! n_len_sq > 0
    bf      env_noreflect

    ! d = (x, y, z) / |d| ; n /= |n|
    fsrra   fr0             ! 1/sqrt(d_len_sq)
    fmov    fr11,fr3
    fmul    fr0,fr3         ! d0
    fmov    fr12,fr4
    fmul    fr0,fr4         ! d1
    fmov    fr13,fr5
    fmul    fr0,fr5         ! d2
    mov     #16,r0
    fmov.s  fr3,@(r0,r15)
    mov     #20,r0
    fmov.s  fr4,@(r0,r15)
    mov     #24,r0
    fmov.s  fr5,@(r0,r15)
    fsrra   fr1             ! 1/sqrt(n_len_sq)
    fmov.s  @r15,fr6
    fmul    fr1,fr6
    fmov.s  fr6,@r15
    mov     #4,r0
    fmov.s  @(r0,r15),fr6
    fmul    fr1,fr6
    fmov.s  fr6,@(r0,r15)
    mov     #8,r0
    fmov.s  @(r0,r15),fr6
    fmul    fr1,fr6
    fmov.s  fr6,@(r0,r15)

    ! d_n = d . n
    fmov.s  @r15,fr0        ! n0
    fmul    fr3,fr0         ! d0*n0
    mov     #4,r0
    fmov.s  @(r0,r15),fr1   ! n1
    fmul    fr4,fr1
    fadd    fr1,fr0
    mov     #8,r0
    fmov.s  @(r0,r15),fr1   ! n2
    fmul    fr5,fr1
    fadd    fr1,fr0         ! d_n in fr0

    fldi0   fr1
    fcmp/gt fr1,fr0         ! d_n > 0
    bf      1f
    fneg    fr0             ! d_n = -d_n
    fmov.s  @r15,fr1
    fneg    fr1
    fmov.s  fr1,@r15
    mov     #4,r0
    fmov.s  @(r0,r15),fr1
    fneg    fr1
    fmov.s  fr1,@(r0,r15)
    mov     #8,r0
    fmov.s  @(r0,r15),fr1
    fneg    fr1
    fmov.s  fr1,@(r0,r15)
1:  mov     #12,r0
    fmov.s  fr0,@(r0,r15)   ! d_n

    ! r = d - 2 * d_n * n
    mov     #100,r0
    fmov.s  @(r0,r8),fr0    ! 2.0
    mov     #12,r0
    fmov.s  @(r0,r15),fr1   ! d_n
    fmul    fr0,fr1         ! 2 * d_n
    fmov.s  @r15,fr2        ! n0
    fmul    fr1,fr2
    fsub    fr2,fr3         ! r0
    mov     #4,r0
    fmov.s  @(r0,r15),fr2   ! n1
    fmul    fr1,fr2
    fsub    fr2,fr4         ! r1
    mov     #8,r0
    fmov.s  @(r0,r15),fr2   ! n2
    fmul    fr1,fr2
    fsub    fr2,fr5         ! r2

    ! m2 = r0^2 + r1^2 + (r2 - 1)^2
    fmov    fr3,fr0
    fmul    fr3,fr0
    fmov    fr4,fr1
    fmul    fr4,fr1
    fadd    fr1,fr0
    fldi1   fr1
    fsub    fr1,fr5         ! r2 - 1
    fmul    fr5,fr5
    fadd    fr5,fr0         ! m2

    fldi0   fr1
    fcmp/gt fr1,fr0         ! m2 > 0
    bf      env_default     ! no: eu = ev = 0.5
    fsrra   fr0             ! 1/sqrt(m2)
    mov     #96,r0
    fmov.s  @(r0,r8),fr1    ! 0.5
    fmul    fr1,fr0         ! inv_m
    mov     #88,r0
    fmov.s  @(r0,r8),fr1    ! env_x
    fmul    fr3,fr1         ! env_x * r0
    fmul    fr0,fr1
    mov     #96,r0
    fmov.s  @(r0,r8),fr2    ! 0.5
    fadd    fr2,fr1         ! eu
    mov     #28,r0
    fmov.s  fr1,@(r0,r15)
    mov     #92,r0
    fmov.s  @(r0,r8),fr1    ! env_y
    fmul    fr4,fr1         ! env_y * r1
    fmul    fr0,fr1
    fadd    fr2,fr1         ! ev
    mov     #32,r0
    fmov.s  fr1,@(r0,r15)
    bra     env_store_uv
    nop
env_default:
    mov     #96,r0
    fmov.s  @(r0,r8),fr1    ! 0.5
    mov     #28,r0
    fmov.s  fr1,@(r0,r15)
    mov     #32,r0
    fmov.s  fr1,@(r0,r15)
    bra     env_store_uv
    nop
env_noreflect:
    mov     #96,r0
    fmov.s  @(r0,r8),fr1    ! 0.5
    mov     #28,r0
    fmov.s  fr1,@(r0,r15)
    mov     #32,r0
    fmov.s  fr1,@(r0,r15)
env_store_uv:
    mov.l   @(0,r8),r1      ! env_uv
    mov     #28,r0
    fmov.s  @(r0,r15),fr0
    fmov.s  fr0,@r1
    mov     #32,r0
    fmov.s  @(r0,r15),fr0
    mov     #4,r0
    fmov.s  fr0,@(r0,r1)

    ! the reflection's colour, packed: alpha 255, r g b from the three channels
    mov     #-1,r6
    shll16  r6
    shll8   r6              ! 0xFF000000
    ENVCHAN 72, 16
    ENVCHAN 76, 8
    ENVCHAN 80, 0
    mov.l   @(4,r8),r1      ! eargb
    mov.l   r6,@r1

    add     #48,r15
    fmov.s  @r15+,fr15
    fmov.s  @r15+,fr14
    fmov.s  @r15+,fr13
    fmov.s  @r15+,fr12
    mov.l   @r15+,r8
    lds.l   @r15+,pr
    rts
    nop
