/*
 * precalc_bridge.S -- manual ASM bridge for the System::preCalc entry hook.
 *
 * The loader installs a non-linking branch at 0x02C57E4C to this label. On
 * entry, all of preCalc's incoming state is live:
 *   r3   = this (frm::System*)
 *   r4.. = further arguments (preCalc reads r4 at 0x02C57E58)
 *   LR   = caller's return address (the entry branch does not touch LR)
 *
 * We must:
 *   1. Save the live argument registers and LR.
 *   2. Call the no-arg C observer (which may clobber volatiles).
 *   3. Restore the argument registers and LR exactly.
 *   4. Tail-call the original trampoline, which replays the overwritten
 *      `mflr r0` and jumps to 0x02C57E50 -- so the real preCalc runs with the
 *      original register state and LR, and returns straight to preCalc's caller.
 *
 * Numeric register operands (LLVM PPC integrated assembler), not r0/r1 (spec 5.1).
 */

	.globl guest_precalc_bridge
	.type  guest_precalc_bridge, @function
guest_precalc_bridge:
	/* Establish a frame. Save LR and the volatile arg registers we must
	   preserve across the observer call. */
	mflr    0
	stwu    1, -64(1)
	stw     0, 68(1)          /* saved LR (in caller frame slot above our frame) */
	stw     3, 8(1)
	stw     4, 12(1)
	stw     5, 16(1)
	stw     6, 20(1)
	stw     7, 24(1)
	stw     8, 28(1)
	stw     9, 32(1)
	stw     10, 36(1)

	/* Call the observer (no arguments). */
	bl      precalc_observe

	/* Restore argument registers and LR. */
	lwz     3, 8(1)
	lwz     4, 12(1)
	lwz     5, 16(1)
	lwz     6, 20(1)
	lwz     7, 24(1)
	lwz     8, 28(1)
	lwz     9, 32(1)
	lwz     10, 36(1)
	lwz     0, 68(1)
	addi    1, 1, 64
	mtlr    0

	/* Tail-call the original trampoline. It replays `mflr r0` (re-reading the
	   now-restored LR) and branches to 0x02C57E50, so preCalc runs normally and
	   ultimately returns to the original caller via LR. Use a non-linking
	   branch so we do not push our own return address. */
	b       uking_System_preCalc_original
	.size guest_precalc_bridge, .-guest_precalc_bridge
