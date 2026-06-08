.text
.align 4
.globl _astraldb_jit_invoke_i64
_astraldb_jit_invoke_i64:
	/* x0=entry, x1=values, x2=count */
	mov x16, x0
	mov x0, x1
	mov x1, x2
	blr x16
	ret

.globl _astraldb_jit_invoke_filter
_astraldb_jit_invoke_filter:
	/* x0=entry, x1=values, x2=count, x3=literal, x4=out */
	mov x16, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	blr x16
	ret
