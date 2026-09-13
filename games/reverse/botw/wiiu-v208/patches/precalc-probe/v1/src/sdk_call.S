/*
 * SDK dispatcher call shim (spec 8.1 register protocol).
 *
 * C signature:
 *   unsigned int cemu_sdk_call(exportId, contextHandle, request, requestSize,
 *                              response, responseCapacity)
 *
 * The eabi C ABI passes these six 32-bit args in r3..r8, which is exactly the
 * dispatcher's register protocol:
 *   r3=export_id r4=context r5=request r6=request_size r7=response r8=capacity
 * The loader binds cemu_sdk_gateway to the real dispatcher VA; it returns Status
 * in r3, which is our return value.
 *
 * NOTE: LLVM's PPC integrated assembler uses NUMERIC register operands (0, 1,
 * ...), not the r0/r1 syntax used by Cemu's own patch_*.asm (spec 5.1).
 */

	.globl cemu_sdk_call
	.type  cemu_sdk_call, @function
cemu_sdk_call:
	/* r3..r8 already hold export_id, context, request, size, response, cap. */
	mflr    0
	stwu    1, -16(1)
	stw     0, 20(1)
	bl      cemu_sdk_gateway
	lwz     0, 20(1)
	addi    1, 1, 16
	mtlr    0
	blr
	.size cemu_sdk_call, .-cemu_sdk_call
