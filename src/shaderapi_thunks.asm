; Vtable thunks for the shader API shim (shaderapi_shim.cpp). A wrapper object is
;   { vtable, real interface, InterfaceTrace* }
; and InterfaceTrace holds a 32-bit call counter per slot at offset 8. Thunk N counts the call, then jumps to slot N
; of the real interface with rcx replaced; while gmodpp_trace_active is set it first passes the call to
; gmodpp_TraceCall(wrapper, slot, saved rcx/rdx/r8/r9 then xmm0-3, caller's stack pointer).

THUNK_COUNT EQU 320

EXTERN gmodpp_TraceCall:PROC

.data
PUBLIC gmodpp_trace_active
gmodpp_trace_active BYTE 0

.code
TraceAndForward PROC FRAME
    sub rsp, 136
    .allocstack 136
    .endprolog
    mov [rsp + 32], rcx
    mov [rsp + 40], rdx
    mov [rsp + 48], r8
    mov [rsp + 56], r9
    movdqa [rsp + 64], xmm0
    movdqa [rsp + 80], xmm1
    movdqa [rsp + 96], xmm2
    movdqa [rsp + 112], xmm3
    mov [rsp + 128], rax
    mov edx, eax
    lea r8, [rsp + 32]
    lea r9, [rsp + 136]
    call gmodpp_TraceCall
    mov rdx, [rsp + 40]
    mov r8, [rsp + 48]
    mov r9, [rsp + 56]
    movdqa xmm0, [rsp + 64]
    movdqa xmm1, [rsp + 80]
    movdqa xmm2, [rsp + 96]
    movdqa xmm3, [rsp + 112]
    mov rax, [rsp + 128]
    mov rcx, [rsp + 32]
    mov rcx, [rcx + 8]
    mov r10, [rcx]
    mov r10, [r10 + rax * 8]
    add rsp, 136
    jmp r10
TraceAndForward ENDP

THUNK MACRO index
gmodpp_thunk_&index& PROC
    mov rax, [rcx + 16]
    inc DWORD PTR [rax + 8 + index * 4]
    cmp gmodpp_trace_active, 0
    jne @F
    mov rcx, [rcx + 8]
    mov rax, [rcx]
    jmp QWORD PTR [rax + index * 8]
@@:
    mov eax, index
    jmp TraceAndForward
gmodpp_thunk_&index& ENDP
ENDM

THUNK_ADDRESS MACRO index
    QWORD gmodpp_thunk_&index&
ENDM

slot = 0
REPT THUNK_COUNT
    THUNK %slot
    slot = slot + 1
ENDM

.const
PUBLIC gmodpp_thunk_count
gmodpp_thunk_count DWORD THUNK_COUNT
ALIGN 8
PUBLIC gmodpp_thunks
gmodpp_thunks LABEL QWORD
slot = 0
REPT THUNK_COUNT
    THUNK_ADDRESS %slot
    slot = slot + 1
ENDM

END
