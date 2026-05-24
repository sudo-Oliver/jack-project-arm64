;; GOAL Runtime assembly functions. These exist only in the arm64 version of GOAL.
;; - https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms#Pass-arguments-to-functions-correctly
;; - https://en.wikipedia.org/wiki/Calling_convention#ARM_(A64)
;; - https://student.cs.uwaterloo.ca/~cs452/docs/rpi4b/aapcs64.pdf
;; - s16–s31 (d8–d15, q4–q7) must be preserved
;; - s0–s15 (d0–d7, q0–q3) and d16–d31 (q8–q15) do not need to be preserved
;; - https://devblogs.microsoft.com/oldnewthing/20220728-00/?p=106912
;; - ;; - https://courses.cs.washington.edu/courses/cse469/19wi/arm64.pdf

.text

;; Call C++ code on arm64 systems, from GOAL.
;; Following the macOS documentation which mostly aligns with standard arm64
;;
;; Entry state (set by EE-memory stub via movz/movk + br):
;;   x16 = C function pointer (IP0 scratch — does not clobber GOAL frame pointer x29)
;;   x30 = GOAL return address (br x8 in stub does not update x30)
;;   x0-x7 = GOAL arguments passed through unchanged
;; Debug helper: increment call counter and print every 50 calls
.global _arg_call_arm64
.align 4
_arg_call_arm64:
  ;; Standard prologue: save GOAL frame-pointer and link reg (= GOAL lr).
  stp  x29, x30, [sp, #-16]!
  mov  x29, sp

  ;; Save callee-saved SIMD registers below the frame.
  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  ;; Call the C function. x0-x7 hold GOAL arguments; x16 = C function pointer.
  ;; No pthread_jit_write_protect_np toggle here: call_goal_on_stack enters exec
  ;; mode before calling GOAL, and C functions invoked from GOAL (method_set,
  ;; new_type, format, ...) only access EE data memory (regular mmap, not
  ;; MAP_JIT). Code-writing paths in klink.cpp have their own explicit toggles.
  blr x16

  ;; Restore callee-saved SIMD registers.
  ldp q9, q8, [sp], #32
  ldp q10, q11, [sp], #32
  ldp q12, q13, [sp], #32
  ldp q14, q15, [sp], #32

  ;; Restore frame pointer and GOAL return address, then return to GOAL code.
  ldp  x29, x30, [sp], #16
  ret


;; Call C++ code on arm64 systems, from GOAL.
;;
;; Passes all 8 GOAL argument registers as an array; the C function receives
;; x0 = pointer to that array.
;;
;; Entry state (set by EE-memory stub):
;;   x16 = C function pointer (IP0 scratch — does not clobber GOAL frame pointer x29)
;;   x30 = GOAL return address
;;   x0-x7 = GOAL arguments
.global _stack_call_arm64
.align 4
_stack_call_arm64:
  ;; Standard prologue: save GOAL frame-pointer and GOAL lr.
  stp  x29, x30, [sp, #-16]!
  mov  x29, sp

  ;; Save callee-saved SIMD registers.
  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  ;; Push all 8 GOAL argument registers onto the stack as a contiguous array.
  ;; STP stores Xt1 at [base] and Xt2 at [base+8], so store even-indexed reg first
  ;; to get args[0]=x0, args[1]=x1, args[2]=x2, ... in ascending address order.
  stp x6, x7, [sp, #-16]!
  stp x4, x5, [sp, #-16]!
  stp x2, x3, [sp, #-16]!
  stp x0, x1, [sp, #-16]!

  ;; x0 = pointer to the argument array (first C argument).
  mov x0, sp

  ;; No pthread_jit_write_protect_np: same rationale as _arg_call_arm64.
  blr x16

  ;; Discard the argument array from the stack (8 regs * 8 bytes = 64 bytes).
  add sp, sp, #64

  ;; Restore callee-saved SIMD registers.
  ldp q9, q8, [sp], #32
  ldp q10, q11, [sp], #32
  ldp q12, q13, [sp], #32
  ldp q14, q15, [sp], #32

  ;; Restore frame pointer and GOAL return address, then return to GOAL code.
  ldp  x29, x30, [sp], #16
  ret

;; Call c++ code through mips2c.
;; GOAL will call a dynamically generated trampoline.
;; The trampoline will have pushed the exec function and stack offset onto the stack
.global _mips2c_call_arm64
.align 4
_mips2c_call_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; TODO - this is really weird using half an XMM, this makes the arm assembly
  ;; more difficult - this probably isn't required for arm?
  ;; grab the address to call and put it in xmm0
  ;; TODO - this stack pointer manipulation might be a problem for ARM64 which requires 16byte alignment
  ;; sub sp, 8
  ldr q0, [sp, #+16]
  ;; grab the stack offset
  ldr x0, [sp, #+8]

  ;; first, save quadword registers
  stp q15, q14, [sp, #-32]!
  stp q13, q12, [sp, #-32]!
  stp q11, q10, [sp, #-32]!
  stp q9, q8, [sp, #-32]!

  ; NOTE - in x86 the 2 special registers are saved (R10 and R11)
  ; we don't need to do that in ARM64, there are plenty of registers to work with

  ;; oof
  sub sp, sp, 1280
  str x0, [sp, #+64] ; arg 0 (RDI in x86) and 
  str x1, [sp, #+80] ; arg 1 (RSI in x86)
  str x2, [sp, #+96] ; arg 2 (RDX in x86) and arg 3 (RCX in x86)
  str x3, [sp, #+112] ; arg 2 (RDX in x86) and arg 3 (RCX in x86)
  str x4, [sp, #+128] ; arg 4 (R8 in x86) and arg 5 (R8 in x86)
  str x5, [sp, #+144] ; arg 4 (R8 in x86) and arg 5 (R8 in x86)
  str x6, [sp, #+160] ; arg 6 (R10 in x86) and arg 7 (R11 in x86)
  str x7, [sp, #+176] ; arg 6 (R10 in x86) and arg 7 (R11 in x86)
  str x20, [sp, #+352] ;; s6 (pp) (R13 in x86) and s7 (st) (R14 in x86)
  str x21, [sp, #+368] ;; s6 (pp) (R13 in x86) and s7 (st) (R14 in x86)

  mov x0, sp ; move the stack pointer to arg 0
  sub x0, x0, x22 ; R15 is a "special" offset TODO - whats special about it?
  str x0, [sp, #+464] ;; mip2c code's MIPS stack

  mov x0, sp ;; move the stack pointer to the new position

  sub sp, sp, x8 ;; allocate space on the stack for GOAL fake stack
  stp x8, x8, [sp, #-16]! ;; and remember this so we can find our way back

  ;; TODO - this used to be a movq rax, xmm0
  ;; TODO - not sure why an `xmm` was used because that movq only uses the lower 64bits anyway
  mov x0, v0.d[0] ; represents the lower 64 bits of q0
  blr x8 ;; call!

  ;; unallocate
  ldp x8, x8, [sp], #16
  add sp, sp, x8

  ldr x8, [sp, #+32]

  add sp, sp, 1280 ; reset the stackpointer back

  ldp q9, q8, [sp], #32
  ldp q10, q11, [sp], #32
  ldp q12, q13, [sp], #32
  ldp q14, q15, [sp], #32

  add sp, sp, 24 ;; 16 for the stuff pushed by trampoline
  ldp	x29, x30, [sp], #16
  ret

;; The _call_goal_asm function is used to call a GOAL function from C.
;; It calls on the parent stack, which is a bad idea if your stack is not already a GOAL stack.
;; It supports up to 3 arguments and a return value.
;; This should be called with the arguments:
;; - first goal arg
;; - second goal arg
;; - third goal arg
;; - address of function to call
;; - address of the symbol table
;; - GOAL memory space offset
.global _call_goal_asm_arm64
.align 4
_call_goal_asm_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; saved registers we need to modify for GOAL should be preserved
  ; ARM64 requires 16-byte stack pointer alignment
  stp x20, x21, [sp, #-16]!
  str x22, [sp, #-16]!

  ;; x0 - first arg
  ;; x1 - second arg
  ;; x2 - third arg
  ;; x3 - function pointer
  ;; x4 - st (goes in x20 and x21)
  ;; x5 - off (goes in x22)

  ;; set GOAL process
  mov x20, x4
  ;; symbol table
  mov x21, x4
  ;; offset
  mov x22, x5
  ;; call GOAL by function pointer
  blr x3

  ;; restore saved registers.
  ldr x22, [sp], #16
  ldp x20, x21, [sp], #16
  ldp	x29, x30, [sp], #16
  ret

.global _call_goal8_asm_arm64
.align 4
_call_goal8_asm_arm64:
  stp	x29, x30, [sp, #-16]!
  mov	x29, sp
  ;; saved registers we need to modify for GOAL should be preserved
  ; ARM64 requires 16-byte stack pointer alignment
  stp x20, x21, [sp, #-16]!
  str x22, [sp, #-16]!

  ;; x0 - first arg (func)
  ;; x1 - second arg (arg array)
  ;; x2 - third arg  (0)
  ;; x3 - pp (goes in r13)
  ;; x4  - st (goes in r14)
  ;; x5  - off (goes in r15)

  ;; set GOAL function pointer
  mov x20, x3
  ;; st
  mov x21, x4
  ;; offset
  mov x22, x5
  ;; move function to temp
  mov x8, x0
  ;; extract arguments
  ldr x0, [x1]  ;; 0
  ldr x2, [x1, #+16] ;; 2
  ldr x3, [x1, #+24] ;; 3
  ldr x4, [x1, #+32]  ;; 4
  ldr x5, [x1, #+40]  ;; 5
  ldr x6, [x1, #+48] ;; 6
  ldr x7, [x1, #+56]  ;; 7
  ldr x1, [x1, #+8] ;; 1 (do this last)
  ;; call GOAL by function pointer
  blr x8

  ;; retore registers.
  ldr x22, [sp], #16
  ldp x20, x21, [sp], #16
  ldp	x29, x30, [sp], #16
  ret

;; Call goal, but switch stacks.
;; Mirror x86 pattern: save old SP onto GOAL stack so it survives across the call.
.global _call_goal_on_stack_asm_arm64
.align 4
_call_goal_on_stack_asm_arm64:
  ;; x0 - new stack pointer (GOAL stack)
  ;; x1 - unused
  ;; x2 - unused
  ;; x3 - function pointer
  ;; x4 - st (goes in x20 and x21)
  ;; x5 - offset (goes in x22)

  ;; Save callee-saved registers and old host SP on the HOST stack before switching.
  ;; We cannot push onto the GOAL stack because GOAL stack starts at top of EE memory
  ;; with no headroom (goal_stack = g_ee_main_mem + EE_MAIN_MEM_SIZE - 8).
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  stp x20, x21, [sp, #-16]!
  stp x22, x23, [sp, #-16]!
  stp x24, x25, [sp, #-16]!
  stp x26, x27, [sp, #-16]!
  str x28,      [sp, #-16]!
  ;; Save old host SP into x23 (callee-saved, preserved by GOAL ABI via saved-reg frame above)
  ;; GOAL functions preserve x20, x21, x22 but MAY use x23 — however we've already saved x23
  ;; on the host stack, and x23 is callee-saved per ARM64 ABI so a well-behaved callee restores it.
  ;; The GOAL runtime sets x20/x21/x22 as GOAL reserved registers and must not clobber x23.
  ;; throw-dispatch restores GOAL register context (including R12=X26, R13=X24, R14=X25, etc.)
  ;; from the catch frame — those writes must not leak into the C++ caller's register state.
  mov x23, sp

  ;; Switch to GOAL stack, aligning to 16 bytes (ARM64 ABI requirement).
  ;; Reserve headroom below the stack top for compiler-generated prologues/spills.
  ;; The C++ side passes stack near EE top; without this margin, early pushes can fault.
  and x0, x0, #0xfffffffffffffff0
  sub x0, x0, #0x4000
  mov sp, x0

  ;; Set GOAL registers
  mov x20, x4  ;; process pointer (pp)
  mov x21, x4  ;; symbol table (st)
  mov x22, x5  ;; offset (g_ee_main_mem)

  ;; Call GOAL function
  blr x3

  ;; Switch back to old host stack (x23 holds it; GOAL code preserves x23 since it's not
  ;; a GOAL reserved register and ARM64 ABI designates x23 as callee-saved)
  mov sp, x23

  ;; Restore callee-saved registers (reverse push order)
  ldr x28,      [sp], #16
  ldp x26, x27, [sp], #16
  ldp x24, x25, [sp], #16
  ldp x22, x23, [sp], #16
  ldp x20, x21, [sp], #16
  ldp x29, x30, [sp], #16
  ret
