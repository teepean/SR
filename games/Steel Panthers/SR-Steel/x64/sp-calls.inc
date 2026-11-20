;; Steel Panthers runtime: calls from recompiled code into C (System V AMD64 ABI).
;;
;; At entry the recompiled code has pushed its return address on the 32-bit guest stack (r11d)
;; and [rsp] holds the _stack * of the current context. ecx, edx, esi, edi are saved on the
;; guest stack (Watcom code expects every register except eax to survive a call); ebx and ebp
;; are callee-saved in the host ABI. The C result in eax becomes the guest eax.

%macro SP_PROLOGUE 0
        sub r11d, byte 4*4
        mov [r11d], ecx
        mov [r11d+4], edx
        mov [r11d+2*4], esi
        mov [r11d+3*4], edi
        mov r10, [rsp]          ; _stack *
        mov [r10], r11d         ; store 32-bit guest stack pointer
%endmacro

%macro SP_EPILOGUE 0
        mov r10, [rsp]
        mov r11d, [r10]
        mov ecx, [r11d]
        mov edx, [r11d+4]
        mov esi, [r11d+2*4]
        mov edi, [r11d+3*4]
        mov r8d, [r11d+4*4]     ; guest return address
        add r11d, byte 5*4
        jmp r8
%endmacro

; cdecl guest function with %2 stack arguments (0..6); caller cleans the guest stack
%macro SP_CALL_STACK 2
        SP_PROLOGUE
  %if %2 >= 1
        mov edi, [r11d+5*4]
  %endif
  %if %2 >= 2
        mov esi, [r11d+6*4]
  %endif
  %if %2 >= 3
        mov edx, [r11d+7*4]
  %endif
  %if %2 >= 4
        mov ecx, [r11d+8*4]
  %endif
  %if %2 >= 5
        mov r8d, [r11d+9*4]
  %endif
  %if %2 >= 6
        mov r9d, [r11d+10*4]
  %endif
        call %1
        SP_EPILOGUE
%endmacro

; Watcom register convention: eax, edx, ebx, ecx
%macro SP_CALL_REG4 1
        SP_PROLOGUE
        mov edi, eax
        mov esi, edx
        mov edx, ebx
        ; ecx is already the 4th argument
        call %1
        SP_EPILOGUE
%endmacro
