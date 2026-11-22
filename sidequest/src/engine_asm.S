/*
 * Sidequest - the parts that must be assembly.
 *
 *   sq_setctx / sq_jumpctx   setjmp/longjmp for the callee-saved state
 *   sq_hook_entry            replaces scePower's call to the kernel's
 *                            "restore suspend context" function
 *   sq_launch                flush caches and jump to the unpack stub
 *   sq_stub_start..end       position-independent unpacker. It is copied
 *                            into extra RAM and run from there, because it
 *                            replaces all of lower RAM, this module included.
 *
 * Only the first four arguments ($4..$7) are relied on anywhere here.
 * Cache operations are the same ones the 6.6x kernel uses (see uOFW,
 * src/kd/sysmem/start.S): the Allegrex does not use the standard MIPS codes.
 */
    .set noreorder
    .set noat
    .text

/* SqCtx (u32 words): s0..s7, fp, sp, ra, gp, k1 */

    .global sq_setctx
    .ent    sq_setctx
sq_setctx:
    sw      $16,  0($4)
    sw      $17,  4($4)
    sw      $18,  8($4)
    sw      $19, 12($4)
    sw      $20, 16($4)
    sw      $21, 20($4)
    sw      $22, 24($4)
    sw      $23, 28($4)
    sw      $30, 32($4)
    sw      $29, 36($4)
    sw      $31, 40($4)
    sw      $28, 44($4)
    sw      $27, 48($4)
    jr      $31
    move    $2, $0
    .end    sq_setctx

    .global sq_jumpctx
    .ent    sq_jumpctx
sq_jumpctx:
    lw      $16,  0($4)
    lw      $17,  4($4)
    lw      $18,  8($4)
    lw      $19, 12($4)
    lw      $20, 16($4)
    lw      $21, 20($4)
    lw      $22, 24($4)
    lw      $23, 28($4)
    lw      $30, 32($4)
    lw      $29, 36($4)
    lw      $31, 40($4)
    lw      $28, 44($4)
    lw      $27, 48($4)
    jr      $31
    li      $2, 1
    .end    sq_jumpctx

/*
 * int sq_hook_entry(int a0)
 *
 * scePower calls this where it used to call sceSuspendForKernel_B2C9640B(1):
 * every driver is suspended, interrupts are off, and the kernel is about to
 * reload its saved context and resume. This is reached at the end of an
 * instant ("touch and go") suspend and also when waking from real sleep.
 *
 * Idle: jump straight to the original, touching nothing.
 * Armed: run on a private stack (scePower's thread stack is only 2KB),
 * do the capture or the swap, then continue to the original.
 *
 * The capture path saves a context with sq_setctx. A later restore unpacks
 * the snapshot and jumps back in through sq_jumpctx, so execution continues
 * at "second" inside the restored system.
 */
    .global sq_hook_entry
    .ent    sq_hook_entry
sq_hook_entry:
    lui     $8, %hi(g_phase)
    lw      $9, %lo(g_phase)($8)
    bnez    $9, 1f
    nop
    lui     $8, %hi(g_orig_resume)
    lw      $25, %lo(g_orig_resume)($8)
    jr      $25
    nop

1:  lui     $8, %hi(g_hk_a0)
    sw      $4, %lo(g_hk_a0)($8)
    lui     $8, %hi(g_hk_ra)
    sw      $31, %lo(g_hk_ra)($8)
    lui     $8, %hi(g_hk_sp)
    sw      $29, %lo(g_hk_sp)($8)
    lui     $29, %hi(g_hook_stack_top)
    lw      $29, %lo(g_hook_stack_top)($29)

    jal     hook_prepare            /* 0 = nothing, 1 = capture, 2 = restore */
    nop
    li      $8, 1
    beq     $2, $8, 2f
    li      $8, 2
    bne     $2, $8, 9f
    nop
    jal     hook_restore            /* returns only if the swap was refused */
    nop
    b       9f
    nop

2:  lui     $4, %hi(g_ctx)
    jal     sq_setctx
    addiu   $4, $4, %lo(g_ctx)
    bnez    $2, 3f
    nop
    jal     hook_capture
    nop
    b       9f
    nop

3:  jal     hook_restored           /* we are the restored system now */
    nop

9:  lui     $8, %hi(g_hk_a0)
    lw      $4, %lo(g_hk_a0)($8)
    lui     $8, %hi(g_hk_ra)
    lw      $31, %lo(g_hk_ra)($8)
    lui     $8, %hi(g_hk_sp)
    lw      $29, %lo(g_hk_sp)($8)
    lui     $8, %hi(g_orig_resume)
    lw      $25, %lo(g_orig_resume)($8)
    jr      $25
    nop
    .end    sq_hook_entry

/*
 * void sq_launch(const u32 args[4], void *stub)   -- never returns
 * args = { stream, ctx, jumpctx, flag }, handed to the stub in $4..$7.
 * Only two register arguments are used so this does not depend on how the
 * compiler passes a fifth one.
 */
    .global sq_launch
    .ent    sq_launch
sq_launch:
    lw      $16, 0($4)
    lw      $17, 4($4)
    lw      $18, 8($4)
    lw      $19, 12($4)
    move    $20, $5
    jal     sceKernelDcacheWritebackInvalidateAll
    nop
    jal     sceKernelIcacheInvalidateAll
    nop
    move    $4, $16
    move    $5, $17
    move    $6, $18
    move    $7, $19
    jr      $20
    nop
    .end    sq_launch

/*
 * Unpack stub.  $4 = packed stream, $5 = ctx, $6 = jumpctx, $7 = flag
 * No stack, no globals, relative branches only.
 * The stream has been validated and CRC-checked before we get here.
 */
    .global sq_stub_start
    .global sq_stub_end
    .ent    sq_stub_start
sq_stub_start:
    move    $12, $0                 /* write position */
10: lw      $8, 0($4)               /* record */
    addiu   $4, $4, 4
    srl     $9, $8, 30              /* tag */
    sll     $10, $8, 2
    beqz    $9, 30f                 /* 0: literal */
    srl     $10, $10, 2             /* count */
    li      $11, 2
    beq     $9, $11, 20f            /* 2: fill */
    li      $11, 1
    bne     $9, $11, 40f            /* 3: end */
    nop
    lw      $12, 0($4)              /* 1: seek */
    b       10b
    addiu   $4, $4, 4

20: lw      $13, 0($4)
    addiu   $4, $4, 4
21: sw      $13, 0($12)
    addiu   $10, $10, -1
    bnez    $10, 21b
    addiu   $12, $12, 4
    b       10b
    nop

30: lw      $13, 0($4)
    addiu   $4, $4, 4
    sw      $13, 0($12)
    addiu   $10, $10, -1
    bnez    $10, 30b
    addiu   $12, $12, 4
    b       10b
    nop

40: /* tell the restored system how it got here */
    lui     $13, 0x5253
    ori     $13, $13, 0x5444        /* SQ_RESTORED_MAGIC */
    sw      $13, 0($7)

    /* write everything to RAM and empty the data cache */
    mfc0    $8, $16
    li      $9, 2048
    ext     $8, $8, 6, 3
    sllv    $9, $9, $8
    move    $8, $0
41: cache   0x14, 0($8)
    addiu   $8, $8, 64
    bne     $8, $9, 41b
    cache   0x14, -64($8)
    sync

    /* empty the instruction cache: it still holds the old system's code */
    mfc0    $8, $16
    li      $9, 4096
    ext     $8, $8, 9, 3
    sllv    $9, $9, $8
    mtc0    $0, $28
    mtc0    $0, $29
    move    $8, $0
42: addiu   $8, $8, 64
    cache   0x1, -64($8)
    bne     $8, $9, 42b
    cache   0x3, -64($8)

    move    $4, $5
    jr      $6
    nop
sq_stub_end:
    .end    sq_stub_start
