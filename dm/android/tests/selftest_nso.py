# A synthetic Switch module for the Android port's end-to-end check, without game files: an NSO named "main" whose nnMain exercises
# what the rewriter and the host change on Android, then reports through svc OutputDebugString (shown with DM_DBG=1):
#   the guest's own x18 (rewritten to thread slot K18), msr / mrs tpidr_el0 (KTLS), mrs tpidrro_el0 (KTLR, the thread-local region),
#   mrs cntpct_el0 and svc (the host gate), a call through the GOT to an import the host resolves (strlen, bionic's), and a second guest thread
#   (CreateThread / StartThread / WaitSynchronization) doing the same in its own x18 slot block.
# The SDK start-up entry points main.c calls before nnMain are defined as plain returns.
# Usage: selftest_nso.py <NDK clang> <NDK llvm-objcopy> <output .nso>
import os, struct, subprocess, sys, tempfile

ASM = r'''
    .text
    .globl _start
_start:
    b nnMain
    .word mod0 - _start

    .p2align 2
init_ret:                                  // _init_libc0/1/2, nnosInitialize, nninitInitializeSdkModule, nninitStartup, SetHostArgc/Argv
    ret

nnMain:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x18, #0x1234                       // the guest's x18: rewritten to slot K18
    add x18, x18, #1
    mov x19, x18
    adr x0, tlsbuf
    msr tpidr_el0, x0                      // KTLS
    mrs x20, tpidr_el0
    mrs x21, tpidrro_el0                   // KTLR
    mrs x22, cntpct_el0                    // gate
    mov x1, #0x1235
    cmp x19, x1
    b.ne fail_x18
    adr x0, tlsbuf
    cmp x20, x0
    b.ne fail_tls
    cbz x21, fail_tlr
    cbz x22, fail_cnt
    adr x0, msg_hello                      // import: strlen("hello") through the GOT
    ldr x9, got_strlen
    blr x9
    cmp x0, #5
    b.ne fail_import
    mov x0, #0x1234                        // the guest's x18 survived the call into host code
    add x0, x0, #1
    cmp x18, x0
    b.ne fail_x18_call
    adr x1, thread_entry                   // CreateThread(entry, arg, stack top, priority, core)
    mov x2, #0x77
    adr x3, tstack_top
    mov w4, #44
    mov w5, #0
    svc #0x08
    cbnz w0, fail_thread
    mov w23, w1
    mov w0, w23                            // StartThread
    svc #0x09
    cbnz w0, fail_thread
    adr x1, hbuf                           // WaitSynchronization(handles, 1, forever)
    str w23, [x1]
    mov w2, #1
    mov x3, #-1
    svc #0x18
    cbnz w0, fail_thread
    adr x0, thr_result
    ldr x1, [x0]
    mov x2, #0x78
    cmp x1, x2
    b.ne fail_thread_body
    adr x0, msg_ok
    mov x1, #(msg_ok_end - msg_ok)
    svc #0x27
    b out
fail_x18:         adr x0, msg_f_x18
                  mov x1, #(msg_f_x18_end - msg_f_x18)
                  b report
fail_tls:         adr x0, msg_f_tls
                  mov x1, #(msg_f_tls_end - msg_f_tls)
                  b report
fail_tlr:         adr x0, msg_f_tlr
                  mov x1, #(msg_f_tlr_end - msg_f_tlr)
                  b report
fail_cnt:         adr x0, msg_f_cnt
                  mov x1, #(msg_f_cnt_end - msg_f_cnt)
                  b report
fail_import:      adr x0, msg_f_imp
                  mov x1, #(msg_f_imp_end - msg_f_imp)
                  b report
fail_x18_call:    adr x0, msg_f_x18c
                  mov x1, #(msg_f_x18c_end - msg_f_x18c)
                  b report
fail_thread:      adr x0, msg_f_thr
                  mov x1, #(msg_f_thr_end - msg_f_thr)
                  b report
fail_thread_body: adr x0, msg_f_thb
                  mov x1, #(msg_f_thb_end - msg_f_thb)
report:
    svc #0x27
out:
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret

thread_entry:                              // x0 = 0x77: stores 0x78 (through x18) when its thread-local region is there, then ExitThread
    mov x18, x0
    add x18, x18, #1
    mov x1, x18
    mrs x2, tpidrro_el0
    cbnz x2, 1f
    mov x1, #0
1:  adr x3, thr_result
    str x1, [x3]
    svc #0x0A
    brk #0

    .p2align 3
got_strlen_lit_pad: .quad 0

    // ---------------- read-only segment ----------------
    .org 0x1000
mod0:
    .ascii "MOD0"
    .word dynamic - mod0, bss_start - mod0, bss_end - mod0, ehhdr - mod0, ehhdr_end - mod0, modobj - mod0
ehhdr:                                     // eh_frame_hdr with no FDEs: the rewriter scans the text linearly
    .byte 1, 0x1b, 0x03, 0x3b
    .word ehhdr_end - .
    .word 0
ehhdr_end:
    .p2align 3
symtab:
    .quad 0, 0, 0
    SYM name_init0, init_ret
    SYM name_init1, init_ret
    SYM name_init2, init_ret
    SYM name_osinit, init_ret
    SYM name_sdkinit, init_ret
    SYM name_startup, init_ret
    SYM name_argc, init_ret
    SYM name_argv, init_ret
    SYM name_main, nnMain
    .word name_strlen - strtab             // undefined import
    .byte 0x12, 0
    .hword 0
    .quad 0, 0
strtab:
    .byte 0
name_init0:   .asciz "_init_libc0"
name_init1:   .asciz "_init_libc1"
name_init2:   .asciz "_init_libc2"
name_osinit:  .asciz "nnosInitialize"
name_sdkinit: .asciz "nninitInitializeSdkModule"
name_startup: .asciz "nninitStartup"
name_argc:    .asciz "_ZN2nn2os11SetHostArgcEi"
name_argv:    .asciz "_ZN2nn2os11SetHostArgvEPPc"
name_main:    .asciz "nnMain"
name_strlen:  .asciz "strlen"
strtab_end:
msg_hello: .asciz "hello"
msg_ok: .ascii "nxnative-android selftest: PASS (x18 slot, TLS, TLR, gate, import, guest thread)"
msg_ok_end:
msg_f_x18: .ascii "nxnative-android selftest: FAIL x18 slot"
msg_f_x18_end:
msg_f_tls: .ascii "nxnative-android selftest: FAIL tpidr_el0 slot"
msg_f_tls_end:
msg_f_tlr: .ascii "nxnative-android selftest: FAIL tpidrro_el0 slot"
msg_f_tlr_end:
msg_f_cnt: .ascii "nxnative-android selftest: FAIL cntpct_el0 gate"
msg_f_cnt_end:
msg_f_imp: .ascii "nxnative-android selftest: FAIL import call"
msg_f_imp_end:
msg_f_x18c: .ascii "nxnative-android selftest: FAIL x18 slot after host call"
msg_f_x18c_end:
msg_f_thr: .ascii "nxnative-android selftest: FAIL thread svc"
msg_f_thr_end:
msg_f_thb: .ascii "nxnative-android selftest: FAIL thread body"
msg_f_thb_end:

    // ---------------- data segment ----------------
    .org 0x2000
dynamic:
    .quad 6, symtab - _start               // DT_SYMTAB
    .quad 5, strtab - _start               // DT_STRTAB
    .quad 10, strtab_end - strtab          // DT_STRSZ
    .quad 7, rela - _start                 // DT_RELA
    .quad 8, rela_end - rela               // DT_RELASZ
    .quad 0, 0
rela:
    .quad got_strlen - _start, (10 << 32) | 1025, 0   // R_AARCH64_GLOB_DAT strlen (symbol 10)
rela_end:
    .p2align 3
got_strlen: .quad 0
thr_result: .quad 0
hbuf: .quad 0
tlsbuf: .space 64
modobj: .space 64
bss_start:
bss_end:
    .p2align 4
tstack: .space 0x4000
tstack_top:
    .p2align 12
'''
MACROS = r'''
.macro SYM name, at
    .word \name - strtab
    .byte 0x12, 0
    .hword 1
    .quad \at - _start, 0
.endm
'''

def build(clang, objcopy, out):
    with tempfile.TemporaryDirectory() as d:
        s, o, b = (os.path.join(d, n) for n in ('t.s', 't.o', 't.bin'))
        with open(s, 'w') as f: f.write(MACROS + ASM)
        subprocess.check_call([clang, '--target=aarch64-linux-android29', '-c', s, '-o', o])
        subprocess.check_call([objcopy, '-O', 'binary', '-j', '.text', o, b])
        img = open(b, 'rb').read()
    assert len(img) % 0x1000 == 0 and len(img) >= 0x3000, hex(len(img))
    text, ro, data = img[:0x1000], img[0x1000:0x2000], img[0x2000:]
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off = 0x100
    for i, (seg, mem) in enumerate(((text, 0), (ro, 0x1000), (data, 0x2000))):
        struct.pack_into('<III', hdr, 0x10 + 0x10 * i, off, mem, len(seg)); off += len(seg)
    struct.pack_into('<I', hdr, 0x3c, 0)                          # bss
    hdr[0x40:0x60] = bytes.fromhex('a11d0000' * 4) + bytes(16)    # build ID
    struct.pack_into('<III', hdr, 0x60, len(text), len(ro), len(data))
    with open(out, 'wb') as f: f.write(bytes(hdr) + text + ro + data)

if __name__ == '__main__':
    build(*sys.argv[1:4])
