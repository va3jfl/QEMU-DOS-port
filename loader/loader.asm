; DOS95.COM - real-mode loader for the DOS95 32-bit multitasking kernel
;
;  * checks CPU (386+), refuses to run under EMM386/Windows (already in V86 mode)
;  * collects DOS internals (InDOS flag, Swappable Data Area, List of Lists)
;  * acquires extended memory (XMS if present, else raw INT 15h memory)
;  * loads DOS95.KRN into extended memory
;  * installs real-mode hook stubs used by the kernel's V86 monitor
;  * enters 32-bit protected mode and jumps to the kernel
;  * provides the return-to-DOS path (exit32/exit16/real_exit)
;
; The whole program stays resident while the kernel runs; every DOS virtual
; machine gets a private copy of this segment, so the stubs exist in every VM.

        cpu 386
        bits 16
        org 100h

HC_STUB_SIZE    equ 16
XFER_SIZE       equ 16384
STACK_SIZE      equ 4096

start:
        jmp     main

; ======================================================================
;  Resident data
; ======================================================================
        align 4
bootinfo:
bi_magic        dd 42353944h            ; 'D95B'
bi_psp          dw 0
bi_res_paras    dw 0
bi_indos        dd 0
bi_sda          dd 0
bi_sda_idle     dw 0
bi_sda_busy     dw 0
bi_lol          dd 0
bi_ext_base     dd 0
bi_ext_size     dd 0
bi_kernel_phys  dd 0
bi_kernel_size  dd 0
bi_stub_seg     dw 0
bi_stub_off     dw stubs
bi_xfer_off     dw xfer_buf
bi_xfer_size    dw XFER_SIZE
bi_req_off      dw svc_req
bi_job_off      dw vm_job
bi_oldivt_off   dw old_ivt
bi_exit32_off   dw exit32
bi_dos_ver      dw 0
bi_flags        dw 0
bi_xms_handle   dw 0
bi_cpu          dw 3
bi_fpu          dw 0
bi_stack_off    dw 0
bi_vmboot_off   dw vm_boot
bi_svcloop_off  dw svc_loop
bi_env_off      dw vm_env               ; VM environment block (paragraph aligned)
bi_comspec      times 80 db 0
bi_exedir       times 80 db 0
bi_cmdline      times 128 db 0
bi_pktbuf_off   dw pkt_buf              ; 2 x 1536 bytes: TX / RX frame buffers for pktuser
bi_pmret_off    dw pmret_frame          ; 32 bytes: ring-3 return frame landing area (kernel/intr.c)
; v41 (#336, M6): Windows' startup broadcast (INT 2Fh AX=1605h) - what it gave back: the Startup
; Info Structures' chain (ES:BX), a handler's refusal (CX), a V86 switcher (DS:SI)
bi_sis          dd 0
bi_sis_cx       dw 0
bi_sis_sw       dd 0
; v41 (#437): what the kernel had to say when DOS95 ends before its desktop - no VESA mode, say - put
; here by the kernel ('$'-ended) and printed after "DOS95 session ended.": the text screen is cleared
; by the mode set at the exit, and the kernel's lines on it went with it
bi_exitmsg_off  dw exit_msg
; v42 (#491): the other blocks of XMS memory - a UEFI firmware leaves RAM in pieces (OVMF: 1-8 MB and 9 MB up) and
; the largest alone was taken: each other block of 1 MB or more, allocated and locked (more_xms), for the kernel's
; pool; their handles freed at the exit.  bi_ext_magic says the list is there (an older loader has none)
EXT_MAX         equ 8
bi_ext_magic    dd 4B4C4258h            ; 'XBLK'
bi_ext_n        dw 0
bi_ext_list     times EXT_MAX*8 db 0    ; each: dd physical base, dd bytes
bi_ext_handles  times EXT_MAX dw 0
; v42 (#494): what the BIOS says of each hard disk, and three of its sectors, read in real mode (probe_disks): their
; segment and count; bi_probe_magic says they are there
bi_probe_magic  dd 31425250h            ; 'PRB1'
bi_probe_seg    dw 0
bi_probe_n      dw 0

; service request block (see struct svcreq in kernel.h)
        align 2
svc_req:
rq_ax   dw 0
rq_bx   dw 0
rq_cx   dw 0
rq_dx   dw 0
rq_si   dw 0
rq_di   dw 0
rq_ds   dw 0
rq_es   dw 0
rq_bp   dw 0
rq_flags dw 0
rq_int  db 0
rq_mode db 0

; VM job block (see struct vmjob in kernel.h)
        align 2
vm_job:
jb_path     times 128 db 0
jb_cmdlen   db 0
jb_cmdtail  times 128 db 0
jb_workdir  times 128 db 0
jb_exitcode dw 0
jb_error    dw 0
jb_envseg   dw 0
jb_kill     dw 0
jb_parm     times 7 dw 0

; misc resident state
saved_ss    dw 0
saved_sp    dw 0
umb_link    dw 0
mcb_fix_cnt dw 0
mcb_fix_list times 64 dw 0
xms_entry   dd 0
have_xms    db 0
xms3        db 0                        ; (v42) XMS 3.0: 88h/89h
verbose     db 0
tmp_ax      dw 0
tmp_eax     dd 0

; EMS (LIM 4.0) device-driver header.  Paragraph-aligned so its segment base
; sits at offset 0, which is what programs test: they read INT 67h's vector,
; take the SEGMENT, and check for the 8-byte name "EMMXXXX0" at offset 0Ah.
; The kernel's V86 monitor intercepts the INT 67h instruction itself, so the
; entry point here is only a safe fallback (an IRET) that never runs in
; practice.  Every VM gets a private copy of this segment (like the stubs).
        align 16
emm_hdr:
        dd  0FFFFFFFFh          ; +00 next-device pointer (none)
        dw  0C000h             ; +04 attributes: char device, IOCTL
        dw  emm_ret - emm_hdr  ; +06 strategy entry (unused; points at RETF)
        dw  emm_ret - emm_hdr  ; +08 interrupt entry (unused; points at RETF)
        db  'EMMXXXX0'         ; +0A device name (8 bytes) -> ends at +12
emm_entry:                      ; +12 INT 67h fallback (monitor handles it)
        iret
emm_ret:                        ; +13 device strategy/interrupt fallback
        retf

; VCPI device header - Windows opens "VCPIXXX0" to decide a VCPI server is present.
        align 16
vcpi_hdr:
        dd  0FFFFFFFFh          ; +00 next-device pointer (patched into the chain)
        dw  0C000h             ; +04 attributes: char device, IOCTL (no open/close call)
        dw  vcpi_ret - vcpi_hdr ; +06 strategy entry (RETF)
        dw  vcpi_ret - vcpi_hdr ; +08 interrupt entry (RETF)
        db  'VCPIXXX0'         ; +0A device name
vcpi_ret:                       ; +12
        retf

; GDT used for the PM switch (and for the return path)
        align 8
gdt:
        dq 0
        dw 0FFFFh, 0, 9A00h, 00CFh      ; 08: code32 base 0 limit 4G
        dw 0FFFFh, 0, 9200h, 00CFh      ; 10: data32 base 0 limit 4G
        dw 0FFFFh, 0, 9A00h, 0000h      ; 18: code16 base = psp*16 (patched)
        dw 0FFFFh, 0, 9200h, 0000h      ; 20: data16 base = psp*16 (patched)
gdt_end:
gdtr:   dw gdt_end - gdt - 1
        dd 0                            ; linear base (patched)
real_idtr:
        dw 3FFh
        dd 0

; ----------------------------------------------------------------------
;  Hypercall stubs.  Each is HC_STUB_SIZE bytes.  The kernel recognises a
;  hypercall by the CS:IP of the HLT instruction.  Layout of an interrupt
;  hook stub:  +0 hlt   +1 jmp far [cs:old_ivt+n*4]   +6 iret
; ----------------------------------------------------------------------
%macro HOOKSTUB 1
        hlt
        jmp far [cs:old_ivt + (%1)*4]
        iret
        times HC_STUB_SIZE - ($ - %%s) db 90h
%%s:
%endmacro

        align 16
stubs:
stub_int10:
        hlt
        jmp far [cs:old_ivt + 10h*4]
        iret
        times 16 - ($ - stub_int10) db 90h
stub_int13:                             ; +0 enter crit, +7 leave crit
        hlt
        pushf
        call far [cs:old_ivt + 13h*4]
        hlt
        times 16 - ($ - stub_int13) db 90h
stub_int15:
        hlt
        jmp far [cs:old_ivt + 15h*4]
        iret
        times 16 - ($ - stub_int15) db 90h
stub_int16:
        hlt
        jmp far [cs:old_ivt + 16h*4]
        iret
        times 16 - ($ - stub_int16) db 90h
stub_int23:
        hlt
        jmp far [cs:old_ivt + 23h*4]
        iret
        times 16 - ($ - stub_int23) db 90h
stub_int24:
        hlt
        jmp far [cs:old_ivt + 24h*4]
        iret
        times 16 - ($ - stub_int24) db 90h
stub_int28:
        hlt
        jmp far [cs:old_ivt + 28h*4]
        iret
        times 16 - ($ - stub_int28) db 90h
stub_int2f:
        hlt
        jmp far [cs:old_ivt + 2Fh*4]
        iret
        times 16 - ($ - stub_int2f) db 90h
stub_int33:
        hlt
        jmp far [cs:old_ivt + 33h*4]
        iret
        times 16 - ($ - stub_int33) db 90h
stub_xms:                               ; XMS far entry point.  The XMS spec requires the
        jmp     short .go               ; entry to begin "jmp short / nop / nop / nop": clients
        nop                             ; (Windows' kernel) hook it by patching those 5 bytes
        nop                             ; and restore a jmp there on exit.
        nop
.go:    hlt
        retf
        times 16 - ($ - stub_xms) db 90h
stub_vmboot:                            ; "VM finished" hypercall (never returns)
        hlt
        ret
        times 16 - ($ - stub_vmboot) db 90h
stub_vmboot2:                           ; "VM starting" hypercall (returns)
        hlt
        ret
        times 16 - ($ - stub_vmboot2) db 90h
stub_svcloop:                           ; VM0: wait for a service request
        hlt
        ret
        times 16 - ($ - stub_svcloop) db 90h
stub_svcdone:                           ; VM0: request completed
        hlt
        ret
        times 16 - ($ - stub_svcdone) db 90h
stub_int13b:                            ; reserved
        hlt
        iret
        times 16 - ($ - stub_int13b) db 90h
stub_kill:                              ; kernel points a VM here to terminate the current program
        hlt                             ; +0 (v41, #451): v86.c STUB_KILL - a machine being ended: its next DOS call
        int     21h                     ; +1   (AX, BX set) - a file closed, a program made current, ended
        jmp     short stub_kill         ; +3   (DOS comes back here: an ended program's way back is this stub)
        int     21h                     ; +5 a program's own end (AX=4CFFh: a DPMI program's, dpmi_exit) - DOS does
        hlt                             ; +7 not come back; it did: refused - the machine ended without DOS
        times 16 - ($ - stub_kill) db 90h
stub_dpmi:                              ; DPMI mode-switch far entry point (hlt; retf)
        hlt
        retf
        times 16 - ($ - stub_dpmi) db 90h
stub_dpmiraw:                           ; DPMI raw real->protected mode switch (far JMP target)
        hlt
        retf
        times 16 - ($ - stub_dpmiraw) db 90h
stub_int74:                             ; IRQ12 (PS/2 mouse) BIOS ISR: +0 kernel far-calls the
        hlt                             ;   device handler (return lands on +1), +1 kernel pops
        hlt                             ;   the packet + EOIs, +2 iret
        iret
        times 16 - ($ - stub_int74) db 90h
stub_pkt:                               ; INT 60h packet driver (FTP Software spec): the
        jmp     short .go               ;   signature must sit at entry+3
        nop
        db      'PKT DRVR', 0
.go:    hlt                             ;   +11: kernel/pktdrv.c handles the call
        iret                            ;   +12
        times 16 - ($ - stub_pkt) db 90h
stub_pktrx:                             ; IRQ10 (INT 72h) receive ISR for the packet driver:
        hlt                             ;   +0 kernel starts the receiver upcall (AX=0)
        hlt                             ;   +1 upcall 1 returned ES:DI, kernel copies + upcall 2
        hlt                             ;   +2 upcall 2 returned: next packet or EOI
        iret                            ;   +3
        times 16 - ($ - stub_pktrx) db 90h
stub_pktstat:                           ; packet driver statistics block (7 dwords, data only)
        times 32 db 0
stub_pktusr:                            ; receiver we register with the user's own DOS packet
        hlt                             ;   driver (VM0): far-called with AX=0 (want buffer)
        retf                            ;   and AX=1 (packet copied) -> kernel/pktuser.c
        times 16 - ($ - stub_pktusr) db 90h
stub_int17:                             ; v41 (#251): the BIOS's printer services - a port with a
        hlt                             ;   printer set up is spooled (kernel/lpt.c), any other
        jmp far [cs:old_ivt + 17h*4]    ;   goes to the BIOS
        iret
        times 16 - ($ - stub_int17) db 90h
stub_int21:                             ; v41 (#336, F24): a DOS box's INT 21h (the kernel puts it in
        hlt                             ;   the boxes' vector, not the system VM's) - +0 the kernel
        jmp far [cs:old_ivt + 21h*4]    ;   looks at it: on to DOS (+1), or a write past FAT32's
        pushf                           ;   4 GB - 1 cut to what fits (+6: DOS called with that,
        call far [cs:old_ivt + 21h*4]   ;   back at +12: the count asked for put back, DOS's
        hlt                             ;   flags to the caller), or none fits (+13: 0 written)
        iret
        times 16 - ($ - stub_int21) db 90h
stub_int08:                             ; v42 (#471): the BIOS's timer tick, where the kernel took the USB
        hlt                             ;   controller from a BIOS that polls it at each tick (SeaBIOS: under
        int     1Ch                     ;   a BIOS layer its USB code runs on another processor through a
        hlt                             ;   mailbox a DOS box's copy of the BIOS never reaches).  The BIOS's
        iret                            ;   own entry (F000:FEA5) jumps here: +0 the kernel counts the tick
        times 16 - ($ - stub_int08) db 90h ; (40:6Ch) and the floppy motor's time, +1 the user's tick, +3 EOI
stub_rmcb:                              ; v43 (#503): a DPMI client's real-mode callbacks (INT 31h 0303h) - each at
        hlt                             ;   its own CS:IP to this one byte (CS one paragraph lower, IP 16 higher,
        times 16 - ($ - stub_rmcb) db 90h ;   for each): the kernel takes it to the client's procedure (dpmi.c)
stubs_end:

; ----------------------------------------------------------------------
;  VM boot code: executed by every new VM (private copy) to run its program
; ----------------------------------------------------------------------
vm_boot:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, stack_top
        sti
        cld
        call    stub_vmboot2            ; tell kernel we're alive (it may fill job)
        ; change drive / directory if requested
        cmp     byte [jb_workdir], 0
        je      .nocd
        cmp     byte [jb_workdir+1], ':'
        jne     .cd
        mov     dl, [jb_workdir]
        and     dl, 0DFh
        sub     dl, 'A'
        mov     ah, 0Eh
        int     21h
.cd:    mov     dx, jb_workdir
        mov     ah, 3Bh
        int     21h
.nocd:
        ; EXEC parameter block
        mov     ax, [jb_envseg]
        mov     [jb_parm+0], ax
        mov     word [jb_parm+2], jb_cmdlen
        mov     [jb_parm+4], cs
        mov     word [jb_parm+6], 5Ch
        mov     [jb_parm+8], cs
        mov     word [jb_parm+10], 6Ch
        mov     [jb_parm+12], cs
        mov     dx, jb_path
        mov     bx, jb_parm
        mov     [saved_ss], ss
        mov     [saved_sp], sp
        mov     ax, 4B00h
        int     21h
        cli
        mov     bx, cs
        mov     ss, bx
        mov     sp, [cs:saved_sp]
        mov     ds, bx
        mov     es, bx
        sti
        jc      .fail
        mov     ah, 4Dh
        int     21h
        mov     [jb_exitcode], ax
        mov     word [jb_error], 0
        jmp     short .done
.fail:  mov     [jb_error], ax
        mov     word [jb_exitcode], 0FFFFh
.done:  call    stub_vmboot             ; never returns
        jmp     short .done

; ----------------------------------------------------------------------
;  VM0 service loop: executes DOS/BIOS calls on behalf of the kernel
; ----------------------------------------------------------------------
svc_loop:
        cli
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, stack_top
        sti
        cld
.loop:
        call    stub_svcloop            ; blocks until a request is posted
        cmp     byte [cs:rq_mode], 2
        je      .absdisk
        cmp     byte [cs:rq_mode], 0
        jne     .orig
        mov     al, [cs:rq_int]
        mov     [cs:.intins+1], al
        call    .loadregs
.intins:
        int     21h
        jmp     .store
.absdisk:                               ; int 25h/26h: absolute disk read/write
        mov     al, [cs:rq_int]
        mov     [cs:.absins+1], al
        call    .loadregs
.absins:
        int     25h                     ; self-modified to 25h/26h; leaves flags on stack
        pop     bp                      ; discard the leftover flags word (pop preserves CF)
        jmp     .store
.orig:
        mov     bl, [cs:rq_int]
        xor     bh, bh
        shl     bx, 2
        add     bx, old_ivt
        pushf
        push    cs
        push    word .store
        push    word [cs:bx+2]
        push    word [cs:bx]
        call    .loadregs
        retf
.store:
        pushf
        push    ds
        mov     [cs:tmp_ax], ax
        mov     ax, cs
        mov     ds, ax
        pop     word [rq_ds]
        pop     word [rq_flags]
        mov     [rq_bx], bx
        mov     [rq_cx], cx
        mov     [rq_dx], dx
        mov     [rq_si], si
        mov     [rq_di], di
        mov     [rq_es], es
        mov     [rq_bp], bp
        mov     ax, [tmp_ax]
        mov     [rq_ax], ax
        mov     ax, cs
        mov     es, ax
        call    stub_svcdone
        jmp     .loop
.loadregs:
        mov     bx, [cs:rq_bx]
        mov     cx, [cs:rq_cx]
        mov     dx, [cs:rq_dx]
        mov     si, [cs:rq_si]
        mov     di, [cs:rq_di]
        mov     bp, [cs:rq_bp]
        mov     es, [cs:rq_es]
        mov     ax, [cs:rq_ax]
        mov     ds, [cs:rq_ds]
        ret

; ----------------------------------------------------------------------
;  Return-to-DOS path.  The kernel jumps to exit32 (physical address) with
;  paging still enabled but low memory identity-mapped, CS = flat code32.
; ----------------------------------------------------------------------
        bits 32
exit32:                                 ; EBP = linear base of this segment
        cli
        mov     eax, cr0
        and     eax, 7FFFFFFFh          ; paging off
        mov     cr0, eax
        xor     eax, eax
        mov     cr3, eax
        lgdt    [ds:ebp + gdtr]
        jmp     far [ds:ebp + exit16_ptr] ; jmp 18h:exit16
        align 4
exit16_ptr      dd exit16               ; 32-bit code: far pointer is dword offset + word selector
                dw 18h

        bits 16
exit16:
        mov     ax, 20h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     sp, stack_top
        lidt    [real_idtr]
        mov     eax, cr0
        and     al, 0FEh
        mov     cr0, eax
        jmp     far [cs:real_ptr]
real_ptr        dw real_exit
real_seg        dw 0

real_exit:
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, stack_top
        ; restore interrupt vectors
        push    es
        xor     ax, ax
        mov     es, ax
        xor     di, di
        mov     si, old_ivt
        mov     cx, 512
        cld
        rep     movsw
        pop     es
        sti
        ; text mode
        mov     ax, 0003h
        int     10h
        call    restore_dosmem
        ; free XMS block
        cmp     byte [have_xms], 0
        je      .noxms
        mov     dx, [bi_xms_handle]
        mov     ah, 0Dh                 ; unlock
        call    far [xms_entry]
        mov     dx, [bi_xms_handle]
        mov     ah, 0Ah                 ; free
        call    far [xms_entry]
        xor     si, si                  ; (v42, #491) the other blocks too
.freex: cmp     si, [bi_ext_n]
        jae     .noxms
        push    si
        shl     si, 1
        mov     dx, [bi_ext_handles + si]
        mov     ah, 0Dh
        call    far [xms_entry]
        pop     si
        push    si
        shl     si, 1
        mov     dx, [bi_ext_handles + si]
        mov     ah, 0Ah
        call    far [xms_entry]
        pop     si
        inc     si
        jmp     .freex
.noxms:
        ; (v41, #458) DOS's device chain as it was: NUL's next device the one it had before main put the EMS and VCPI
        ; devices in front of it (vcpi_hdr's next holds it) - their headers are in this program's memory, which DOS
        ; takes back at its exit: the next program loaded there was DOS's chain, and DOS walked it for ever at the
        ; first file a program opened after DOS95 (Doom: nothing, the machine stopped; DIR and VER went on)
        push    es
        mov     ah, 52h
        int     21h                     ; ES:BX -> List of Lists
        mov     ax, [vcpi_hdr]
        mov     [es:bx+22h], ax
        mov     ax, [vcpi_hdr+2]
        mov     [es:bx+24h], ax
        pop     es
        mov     dx, msg_bye
        mov     ah, 9
        int     21h
        mov     dx, exit_msg            ; (v41, #437: the kernel's last word, if it had one)
        mov     ah, 9
        int     21h
        mov     ax, 4C00h
        int     21h

; ======================================================================
;  Non-resident-ish init code (kept resident anyway, it is small)
; ======================================================================
main:
        cld
        mov     [bi_psp], cs
        mov     [bi_stub_seg], cs
        mov     [real_seg], cs
        mov     word [bi_stack_off], stack_top

        ; ---- command line -------------------------------------------------
        movzx   cx, byte [80h]
        mov     si, 81h
        mov     di, bi_cmdline
        cmp     cx, 127
        jbe     .cl_ok
        mov     cx, 127
.cl_ok: rep     movsb
        mov     byte [di], 0
        call    parse_args

        mov     dx, msg_banner
        call    print

        ; ---- CPU check ----------------------------------------------------
        call    check_cpu
        jnc     .cpu_ok
        mov     dx, msg_cpu
        jmp     fatal
.cpu_ok:
        smsw    ax
        test    al, 1
        jz      .not_v86
        mov     dx, msg_v86
        jmp     fatal
.not_v86:
        mov     ax, 1600h
        int     2Fh
        cmp     al, 0
        je      .nowin
        cmp     al, 80h
        je      .nowin
        mov     dx, msg_win
        jmp     fatal
.nowin:
        call    check_fpu

        ; ---- (v42, #480) Restart in MS-DOS mode ------------------------------------------------------
        ; DOS95 restarted the PC for DOS - Shut Down, Return to MS-DOS, on a PC whose NVMe disk or USB
        ; controller its BIOS could not have back: DOSMODE.FLG in DOS95's folder, written then.  This
        ; start (AUTOEXEC.BAT's) is DOS's, as after Windows 95's "Restart in MS-DOS mode": the flag
        ; deleted, a line said, DOS left at its prompt.  A flag not of the last five minutes was not
        ; this restart's: deleted, and DOS95 starts.
        call    get_env_info
        call    dosmode_check

        ; ---- DOS internals ------------------------------------------------
        mov     ah, 30h
        int     21h
        mov     [bi_dos_ver], ax
        cmp     al, 3
        jae     .dos_ok
        mov     dx, msg_dos
        jmp     fatal
.dos_ok:
        push    ds
        push    es
        mov     ah, 34h
        int     21h                     ; ES:BX -> InDOS
        mov     ax, es
        movzx   eax, ax
        shl     eax, 4
        movzx   ebx, bx
        add     eax, ebx
        pop     es
        pop     ds
        mov     [bi_indos], eax

        push    ds
        xor     si, si
        mov     ax, 5D06h
        int     21h                     ; DS:SI -> SDA, CX = size while in DOS, DX = size always
        mov     ax, ds
        pop     ds
        mov     [bi_sda_idle], dx
        mov     [bi_sda_busy], cx
        movzx   eax, ax
        shl     eax, 4
        movzx   esi, si
        add     eax, esi
        mov     [bi_sda], eax
        test    eax, eax
        jnz     .sda_ok
        mov     dx, msg_sda
        jmp     fatal
.sda_ok:
        cmp     dx, 0
        je      .sda_bad
        cmp     cx, dx
        jae     .sda_ok2
.sda_bad:
        mov     dx, msg_sda
        jmp     fatal
.sda_ok2:
        push    es
        mov     ah, 52h
        int     21h                     ; ES:BX -> List of Lists
        ; --- splice the EMS device header into the DOS device chain, so programs
        ;     (e.g. Windows' DOSX) can open the "EMMXXXX0" device and detect us ---
        push    ax
        push    cx
        push    dx
        ; chain: NUL -> emm_hdr -> vcpi_hdr -> (old NUL.next)
        mov     ax, cs
        mov     cx, vcpi_hdr
        shr     cx, 4
        add     ax, cx                  ; ax = vcpi_hdr segment
        mov     dx, [es:bx+22h]         ; old NUL.next offset
        mov     [cs:vcpi_hdr], dx       ; vcpi_hdr.next = old NUL.next
        mov     dx, [es:bx+24h]
        mov     [cs:vcpi_hdr+2], dx
        mov     word [cs:emm_hdr], 0    ; emm_hdr.next = vcpi_hdr (seg=ax, off=0)
        mov     [cs:emm_hdr+2], ax
        mov     ax, cs
        mov     cx, emm_hdr
        shr     cx, 4
        add     ax, cx                  ; ax = emm_hdr segment
        mov     word [es:bx+22h], 0     ; NUL.next = emm_hdr
        mov     [es:bx+24h], ax
        pop     dx
        pop     cx
        pop     ax
        mov     ax, es
        pop     es
        movzx   eax, ax
        shl     eax, 4
        movzx   ebx, bx
        add     eax, ebx
        mov     [bi_lol], eax

        ; LFN available?
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     dx, root_path
        mov     di, xfer_buf
        mov     cx, 64
        stc
        mov     ax, 71A0h
        int     21h
        pop     es
        pop     ds
        jc      .nolfn
        cmp     ax, 7100h
        je      .nolfn
        or      word [bi_flags], 2
.nolfn:
        call    get_env_info
        call    build_vm_env

        ; ---- Windows' startup broadcast (v41, #336, M6) ----------------------
        ; As Windows 3.1's enhanced mode and Windows 95's VMM start: DOS and every TSR loaded
        ; before DOS95 chain a Startup Info Structure - the data each virtual machine must have a
        ; copy of its own (the kernel swaps it with the machine, vm.c).  As Windows 3.10 enhanced
        ; mode asks (DX=0, DI=030Ah); then at once its exit broadcast (1606h) - DOS95 is not
        ; Windows: DOS would think Windows' DOSMGR there (MS-DOS 7.1 sets its Win386 flags at
        ; 1605h: INT 2Ah critical sections, no machine number in its file table), and a TSR
        ; would wait for a virtual device of its own.  The lists stay where they are.
        call    sis_start

        ; ---- extended memory -----------------------------------------------
        call    get_ext_memory
        jnc     .mem_ok
        mov     dx, msg_mem
        jmp     fatal
.mem_ok:
        call    more_xms                ; (v42, #491)
        mov     dx, msg_loading
        call    print
        call    load_kernel
        jnc     .load_ok
        mov     ax, [fail_step]
        call    print_hex16
        mov     ax, [fail_ax]
        call    print_hex16
        mov     dx, msg_load
        jmp     fatal2
.load_ok:
        ; ---- DOS memory fix-ups ----------------------------------------------
        call    fix_dosmem
        call    probe_disks             ; (v42, #494: after the shrink - into what DOS95 does not keep)

        ; ---- hooks ---------------------------------------------------------------
        cli
        push    ds
        push    es
        xor     ax, ax
        mov     ds, ax
        mov     ax, cs
        mov     es, ax
        xor     si, si
        mov     di, old_ivt
        mov     cx, 512
        rep     movsw
        pop     es
        pop     ds
        call    install_hooks
        ; (v42, #497) no STI here: interrupts stay off until the kernel has its own.  The stubs just put in
        ; the vectors begin with HLT - the kernel's trap in V86 mode; in real mode, inside an interrupt
        ; handler, it stops the PC for good.  A timer tick let in here ran the BIOS's own handlers through
        ; them: on a PC with UEFI only the BIOS layer's video BIOS blinks its cursor from the timer
        ; (SeaVGABIOS swcursor: INT 15h AH=87h to the frame buffer) - the PC stopped after "Loading
        ; kernel..." (uefi42 B, once).

        ; ---- GDT / PM entry ----------------------------------------------------
        movzx   eax, word [bi_psp]
        shl     eax, 4                  ; eax = linear base of our segment
        mov     ebx, eax
        add     ebx, gdt
        mov     [gdtr+2], ebx
        ; patch 16-bit descriptors base = eax
        mov     [gdt+18h+2], ax
        mov     [gdt+20h+2], ax
        shr     eax, 16
        mov     [gdt+18h+4], al
        mov     [gdt+20h+4], al
        ; eax = bootinfo phys, ebx = kernel phys
        movzx   eax, word [bi_psp]
        shl     eax, 4
        mov     ebx, eax
        add     ebx, bootinfo
        mov     edx, [bi_kernel_phys]
        add     eax, stack_top
        mov     [pm_esp], eax
        mov     [pm_ebx], ebx
        mov     [pm_entry], edx
        movzx   eax, word [bi_psp]
        shl     eax, 4
        add     eax, pm32
        mov     [pm32_ptr], eax

        cli
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     far dword [pm32_ptr]
pm32_ptr        dd 0
                dw 08h
pm_esp          dd 0
pm_ebx          dd 0
pm_entry        dd 0

        bits 32
pm32:
        mov     ax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        ; addresses below are relative to our segment; CS base is 0 so
        ; compute absolute: we stored absolute values already
        mov     esp, [ebx + (pm_esp - bootinfo)]
        mov     eax, [ebx + (pm_entry - bootinfo)]
        mov     ebx, [ebx + (pm_ebx - bootinfo)]
        jmp     eax
        bits 16

; ----------------------------------------------------------------------
fatal2: call    restore_dosmem
fatal:  call    print
        mov     dx, msg_crlf
        call    print
        mov     ax, 4C01h
        int     21h

; v41 (#336, M6): INT 2Fh AX=1605h - Windows' startup broadcast; ES:BX, CX, DS:SI to bootinfo
sis_start:
        push    ds
        push    es
        pushad
        xor     bx, bx
        mov     es, bx
        xor     si, si
        xor     cx, cx
        xor     dx, dx                  ; enhanced mode
        mov     di, 030Ah               ; Windows 3.10
        mov     ax, 1605h
        push    ds
        mov     ds, bx
        int     2Fh
        mov     ax, ds
        pop     ds
        mov     [bi_sis], bx
        mov     [bi_sis+2], es
        mov     [bi_sis_cx], cx
        mov     [bi_sis_sw], si
        mov     [bi_sis_sw+2], ax
        mov     byte [sis_sent], 1
        popad
        pop     es
        pop     ds
        jmp     sis_exit                ; (Windows gone again at once)
; INT 2Fh AX=1606h - Windows' exit broadcast, once (after the 1605h)
sis_exit:
        cmp     byte [cs:sis_sent], 0
        je      .r
        mov     byte [cs:sis_sent], 0
        push    ds
        push    es
        pushad
        xor     dx, dx                  ; enhanced mode
        mov     ax, 1606h
        int     2Fh
        popad
        pop     es
        pop     ds
.r:     ret
sis_sent        db 0

print:  mov     ah, 9
        int     21h
        ret

; print AX as hex (debug)
print_hex16:
        push    cx
        mov     cx, 4
.l:     rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 7
.d:     mov     dl, al
        mov     ah, 2
        int     21h
        pop     ax
        loop    .l
        pop     cx
        ret

; ----------------------------------------------------------------------
parse_args:
        mov     si, bi_cmdline
.next:  lodsb
        test    al, al
        jz      .done
        cmp     al, '/'
        je      .opt
        cmp     al, '-'
        jne     .next
.opt:   lodsb
        and     al, 0DFh
        cmp     al, 'V'
        jne     .next
        mov     byte [verbose], 1
        or      word [bi_flags], 4
        jmp     .next
.done:  ret

; ----------------------------------------------------------------------
; CPU check: CF set if < 386
check_cpu:
        pushf
        pop     ax
        mov     cx, ax
        xor     ax, 4000h               ; toggle bit 14 (NT)
        push    ax
        popf
        pushf
        pop     ax
        push    cx
        popf
        xor     ax, cx
        test    ax, 4000h
        jz      .old                    ; couldn't toggle -> 8086/286
        ; 386 or better.  Try toggling AC (bit 18) to detect 486+
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 40000h
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax, ecx
        test    eax, 40000h
        jz      .is386
        mov     word [bi_cpu], 4
        ; ID flag -> CPUID capable (pentium+)
        mov     eax, ecx
        xor     eax, 200000h
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax, ecx
        test    eax, 200000h
        jz      .ok
        mov     word [bi_cpu], 5
.ok:    clc
        ret
.is386: mov     word [bi_cpu], 3
        clc
        ret
.old:   stc
        ret

check_fpu:
        fninit
        mov     word [tmp_ax], 5A5Ah
        fnstsw  [tmp_ax]
        cmp     byte [tmp_ax], 0
        jne     .none
        mov     word [bi_fpu], 1
.none:  ret

; ----------------------------------------------------------------------
; VM environment block: a copy of our own environment strings plus the
; Sound Blaster resource variable (the virtual card every DOS box sees).
; Programs and drivers (Creative's Windows drivers among them) locate the
; card through it.  Too big to fit -> left empty (VMs inherit ours).
VMENV_SIZE equ 1536
build_vm_env:
        push    ds
        push    es
        mov     ax, [2Ch]
        mov     ds, ax                  ; DS:SI = our environment
        mov     ax, cs
        mov     es, ax
        xor     si, si
        mov     di, vm_env
        mov     cx, VMENV_SIZE - 64
.copy:  lodsb
        stosb
        test    al, al
        jnz     .cnt
        cmp     byte [si], 0            ; double NUL: end of strings
        je      .strs_done
.cnt:   loop    .copy
        ; did not fit: leave it empty
        mov     byte [es:vm_env], 0
        mov     byte [es:vm_env+1], 0
        jmp     .out
.strs_done:
        ; already has BLASTER= ?  (scan the copy)
        push    ds
        mov     ax, cs
        mov     ds, ax                  ; DS = CS
        mov     si, vm_env
.scan:  cmp     byte [si], 0
        je      .add
        cmp     dword [si], 'BLAS'
        jne     .nxt
        cmp     dword [si+4], 'TER='
        je      .have
.nxt:   lodsb
        test    al, al
        jnz     .nxt
        jmp     .scan
.add:   mov     si, str_blaster
.cpb:   lodsb
        stosb
        test    al, al
        jnz     .cpb
.have:  xor     al, al
        stosb                           ; terminating double NUL
        pop     ds
.out:   pop     es
        pop     ds
        ret
str_blaster db 'BLASTER=A220 I5 D1 H5 P330 T6',0

; ----------------------------------------------------------------------
; environment: COMSPEC and program directory
get_env_info:
        push    ds
        push    es
        mov     ax, [2Ch]
        mov     es, ax
        mov     ax, cs
        mov     ds, ax
        xor     di, di
        ; scan environment strings
.envloop:
        cmp     byte [es:di], 0
        je      .envend
        ; compare with "COMSPEC="
        mov     si, str_comspec
        mov     bx, di
.cmp:   mov     al, [si]
        test    al, al
        jz      .match
        cmp     al, [es:bx]
        jne     .nomatch
        inc     si
        inc     bx
        jmp     .cmp
.match: ; copy value
        mov     si, bi_comspec
        mov     cx, 79
.cp:    mov     al, [es:bx]
        mov     [si], al
        inc     bx
        inc     si
        test    al, al
        jz      .skip
        loop    .cp
        mov     byte [si], 0
.nomatch:
.skip:  ; advance to next string
        mov     al, [es:di]
        inc     di
        test    al, al
        jnz     .skip
        jmp     .envloop
.envend:
        inc     di                      ; skip final 0
        add     di, 2                   ; word count
        ; ES:DI -> full program path
        mov     si, bi_exedir
        mov     bx, si                  ; bx = last backslash pos +1
        mov     cx, 79
.cp2:   mov     al, [es:di]
        inc     di
        test    al, al
        jz      .cp2end
        mov     [si], al
        inc     si
        cmp     al, '\'
        jne     .nb
        mov     bx, si
.nb:    loop    .cp2
.cp2end:
        mov     byte [bx], 0            ; cut after last backslash
        cmp     byte [bi_exedir], 0
        jne     .have
        ; DOS 2.x or missing: use current directory
        mov     byte [bi_exedir], 0
.have:  pop     es
        pop     ds
        ret

; ----------------------------------------------------------------------
; (v42, #491) the other XMS blocks of 1 MB or more, the largest first: allocated, locked, in bi_ext_list
more_xms:
        cmp     byte [have_xms], 0
        je      .done
.next:  cmp     word [bi_ext_n], EXT_MAX
        jae     .done
        cmp     byte [xms3], 0
        je      .x2
        mov     ah, 88h
        call    far [xms_entry]         ; EAX = the largest free block, KB
        cmp     eax, 1024
        jb      .done
        mov     [tmp_eax], eax
        mov     edx, eax
        mov     ah, 89h
        call    far [xms_entry]         ; DX = its handle
        cmp     ax, 1
        jne     .done
        jmp     .lock
.x2:    mov     ah, 08h
        call    far [xms_entry]         ; AX = the largest free block, KB
        cmp     ax, 1024
        jb      .done
        movzx   eax, ax
        mov     [tmp_eax], eax
        mov     dx, ax
        mov     ah, 09h
        call    far [xms_entry]
        cmp     ax, 1
        jne     .done
.lock:  mov     si, [bi_ext_n]
        shl     si, 1
        mov     [bi_ext_handles + si], dx
        push    dx
        mov     ah, 0Ch
        call    far [xms_entry]         ; DX:BX = its physical address
        pop     cx
        cmp     ax, 1
        je      .locked
        mov     dx, cx                  ; (not lockable: given back)
        mov     ah, 0Ah
        call    far [xms_entry]
        jmp     .done
.locked:
        mov     si, [bi_ext_n]
        shl     si, 3
        mov     [bi_ext_list + si], bx
        mov     [bi_ext_list + si + 2], dx
        mov     eax, [tmp_eax]
        shl     eax, 10
        mov     [bi_ext_list + si + 4], eax
        inc     word [bi_ext_n]
        jmp     .next
.done:  ret

; ----------------------------------------------------------------------
; Extended memory: sets bi_ext_base / bi_ext_size.  CF on failure.
get_ext_memory:
        mov     ax, 4300h
        int     2Fh
        cmp     al, 80h
        jne     .noxms
        push    es
        mov     ax, 4310h
        int     2Fh
        mov     [xms_entry], bx
        mov     [xms_entry+2], es
        pop     es
        mov     byte [have_xms], 1
        or      word [bi_flags], 1
        ; enable A20 (global)
        mov     ah, 03h
        call    far [xms_entry]
        ; XMS 3.0 (HIMEM with >64 MB): 88h/89h take 32-bit KB counts
        mov     ah, 00h
        call    far [xms_entry]         ; AH = version (BCD major)
        cmp     ah, 3
        jb      .xms2
        mov     byte [xms3], 1
        mov     ah, 88h
        call    far [xms_entry]         ; EAX = largest free KB, EDX = total KB
        cmp     eax, 1536
        jb      .xms2
        sub     eax, 256
        mov     [tmp_eax], eax
        mov     edx, eax
        mov     ah, 89h
        call    far [xms_entry]         ; DX = handle
        cmp     ax, 1
        jne     .xms2
        mov     [bi_xms_handle], dx
        mov     ah, 0Ch                 ; lock -> DX:BX = phys
        call    far [xms_entry]
        cmp     ax, 1
        jne     .fail
        mov     [bi_ext_base], bx
        mov     [bi_ext_base+2], dx
        mov     eax, [tmp_eax]
        shl     eax, 10
        mov     [bi_ext_size], eax
        clc
        ret
.xms2:
        ; largest free block
        mov     ah, 08h
        call    far [xms_entry]         ; AX = largest KB, DX = total KB
        cmp     ax, 1536
        jb      .fail
        ; leave 256K for other XMS users
        sub     ax, 256
        mov     dx, ax
        mov     [tmp_ax], ax
        mov     ah, 09h
        call    far [xms_entry]         ; DX = handle
        cmp     ax, 1
        jne     .fail
        mov     [bi_xms_handle], dx
        mov     ah, 0Ch                 ; lock -> DX:BX = phys
        call    far [xms_entry]
        cmp     ax, 1
        jne     .fail
        mov     [bi_ext_base], bx
        mov     [bi_ext_base+2], dx
        movzx   eax, word [tmp_ax]
        shl     eax, 10
        mov     [bi_ext_size], eax
        clc
        ret
.noxms:
        ; raw extended memory: INT 15h E801h, fall back to 88h
        mov     ax, 0E801h
        xor     cx, cx
        xor     dx, dx
        int     15h
        jc      .try88
        test    ax, ax
        jnz     .e801
        mov     ax, cx
        mov     bx, dx
.e801:  movzx   eax, ax                 ; KB between 1M and 16M
        ; v41 (#336): and the memory above 16 MB (BX: 64 KB blocks) - until v41 a machine without
        ; an XMS driver ran DOS95 in its first 15 MB, however much it had
        movzx   ebx, bx
        shl     ebx, 6                  ; KB above 16 MB
        cmp     eax, 3C00h              ; all of 1-16 MB there: one run from 1 MB up
        jb      .e801hole
        add     eax, ebx
        jmp     .have_kb
.e801hole:                              ; a hole below 16 MB: the bigger of the two runs
        cmp     ebx, eax
        jbe     .have_kb
        cmp     ebx, 1600
        jb      .have_kb
        cmp     ebx, 3FBC00h            ; (16 MB up: at most 4 GB - 16 MB)
        jbe     .hiok
        mov     ebx, 3FBC00h
.hiok:  shl     ebx, 10
        mov     [bi_ext_size], ebx
        mov     dword [bi_ext_base], 1000000h
        call    enable_a20
        jc      .fail
        clc
        ret
.try88: mov     ah, 88h
        int     15h
        jc      .fail
        movzx   eax, ax
.have_kb:
        cmp     eax, 1600
        jb      .fail
        cmp     eax, 3FFC00h            ; (4 GB in KB would not fit in bytes: the kernel
        jbe     .kbok                   ;  takes what 32-bit addresses can - mm.c)
        mov     eax, 3FFC00h
.kbok:  shl     eax, 10                 ; bytes above 1M
        sub     eax, 10000h             ; skip HMA
        mov     [bi_ext_size], eax
        mov     dword [bi_ext_base], 110000h
        call    enable_a20
        jc      .fail
        clc
        ret
.fail:  stc
        ret

; enable A20 without XMS: fast A20 then keyboard controller
enable_a20:
        call    test_a20
        jnc     .done
        in      al, 92h
        or      al, 2
        and     al, 0FEh
        out     92h, al
        call    test_a20
        jnc     .done
        call    kbc_wait
        mov     al, 0D1h
        out     64h, al
        call    kbc_wait
        mov     al, 0DFh
        out     60h, al
        call    kbc_wait
        mov     cx, 0FFFFh
.w:     call    test_a20
        jnc     .done
        loop    .w
        stc
        ret
.done:  clc
        ret
kbc_wait:
        in      al, 64h
        test    al, 2
        jnz     kbc_wait
        ret
; CF set if A20 disabled (memory wraps)
test_a20:
        push    ds
        push    es
        xor     ax, ax
        mov     ds, ax
        mov     ax, 0FFFFh
        mov     es, ax
        mov     ax, [ds:0]
        mov     bx, [es:10h]
        cmp     ax, bx
        jne     .on
        not     word [ds:0]
        mov     ax, [ds:0]
        mov     bx, [es:10h]
        not     word [ds:0]
        cmp     ax, bx
        jne     .on
        stc
        jmp     .x
.on:    clc
.x:     pop     es
        pop     ds
        ret

; ----------------------------------------------------------------------
; (v42, #480) DOSMODE.FLG in DOS95's folder, of the last five minutes: DOS95 restarted the PC into
; MS-DOS - the flag deleted, a line said, back to DOS's prompt.  Returns when there is none.
dosmode_check:
        mov     si, bi_exedir
        mov     di, path_buf
.c1:    lodsb
        test    al, al
        jz      .c1e
        stosb
        jmp     .c1
.c1e:   mov     si, str_dosmode
.c2:    lodsb
        stosb
        test    al, al
        jnz     .c2
        mov     dx, path_buf
        mov     ax, 3D00h               ; there?
        int     21h
        jc      .none
        mov     bx, ax
        mov     ax, 5700h               ; its time (CX) and date (DX)
        int     21h
        pushf
        push    cx
        push    dx
        mov     ah, 3Eh
        int     21h
        pop     dx
        pop     cx
        popf
        jc      .stale
        mov     [dm_fdate], dx
        mov     ax, cx
        shr     ax, 11                  ; its hour
        mov     bl, 60
        mul     bl
        mov     bx, cx
        shr     bx, 5
        and     bx, 3Fh                 ; its minute
        add     ax, bx
        mov     [dm_fmin], ax
        mov     ah, 2Ah
        int     21h                     ; today: CX year, DH month, DL day
        sub     cx, 1980
        shl     cx, 9
        movzx   ax, dh
        shl     ax, 5
        or      cx, ax
        movzx   ax, dl
        or      cx, ax
        cmp     cx, [dm_fdate]
        jne     .stale
        mov     ah, 2Ch
        int     21h                     ; now: CH hour, CL minute
        movzx   ax, ch
        mov     bl, 60
        mul     bl
        movzx   bx, cl
        add     ax, bx
        sub     ax, [dm_fmin]
        jb      .stale                  ; (written later than now: not this restart's)
        cmp     ax, 5
        ja      .stale
        mov     dx, path_buf
        mov     ah, 41h
        int     21h
        mov     dx, msg_dosmode
        call    print
        mov     ax, 4C00h
        int     21h
.stale: mov     dx, path_buf
        mov     ah, 41h
        int     21h
.none:  ret

; ----------------------------------------------------------------------
; Load DOS95.KRN into extended memory at bi_ext_base (page aligned).
load_kernel:
        ; build path: exedir + "DOS95.KRN"
        mov     si, bi_exedir
        mov     di, path_buf
.c1:    lodsb
        test    al, al
        jz      .c1e
        stosb
        jmp     .c1
.c1e:   mov     si, str_krn
.c2:    lodsb
        stosb
        test    al, al
        jnz     .c2
        cmp     byte [verbose], 0
        je      .nov
        mov     dx, path_buf
        mov     ah, 9
        push    dx
        mov     dx, msg_crlf
        int     21h
        pop     dx
        ; path is ASCIIZ: print char by char
        mov     si, path_buf
.pv:    lodsb
        test    al, al
        jz      .nov
        mov     dl, al
        mov     ah, 2
        int     21h
        jmp     .pv
.nov:   mov     dx, path_buf
        mov     ax, 3D00h
        int     21h
        mov     word [fail_step], 1
        jc      .fail
        mov     [file_h], ax
        ; size
        mov     bx, ax
        mov     ax, 4202h
        xor     cx, cx
        xor     dx, dx
        int     21h                     ; DX:AX = size
        mov     [bi_kernel_size], ax
        mov     [bi_kernel_size+2], dx
        mov     ax, 4200h
        xor     cx, cx
        xor     dx, dx
        int     21h
        ; kernel phys = ext_base rounded up to 4K
        mov     eax, [bi_ext_base]
        add     eax, 0FFFh
        and     eax, 0FFFFF000h
        mov     [bi_kernel_phys], eax
        mov     [cur_phys], eax
        ; check it fits (kernel + 1MB margin)
        mov     ebx, [bi_ext_base]
        add     ebx, [bi_ext_size]
        sub     ebx, eax
        mov     ecx, [bi_kernel_size]
        add     ecx, 100000h
        mov     word [fail_step], 2
        cmp     ebx, ecx
        jb      .fail
.rd:    mov     bx, [file_h]
        mov     cx, XFER_SIZE
        mov     dx, xfer_buf
        mov     ah, 3Fh
        int     21h
        mov     word [fail_step], 3
        jc      .fail
        test    ax, ax
        jz      .eof
        mov     [chunk_len], ax
        call    copy_chunk
        mov     word [fail_step], 4
        jc      .fail
        movzx   eax, word [chunk_len]
        add     [cur_phys], eax
        cmp     ax, XFER_SIZE
        je      .rd
.eof:   mov     bx, [file_h]
        mov     ah, 3Eh
        int     21h
        ; print dot progress? no.
        clc
        ret
.fail:  mov     bx, [file_h]
        test    bx, bx
        jz      .f2
        mov     ah, 3Eh
        int     21h
.f2:    stc
        ret

; copy chunk_len bytes from xfer_buf to phys [cur_phys]
copy_chunk:
        cmp     byte [have_xms], 0
        je      .raw
        ; XMS move (length must be even)
        movzx   eax, word [chunk_len]
        inc     eax
        and     eax, 0FFFFFFFEh
        mov     [xm_len], eax
        mov     word [xm_shandle], 0
        mov     word [xm_soff], xfer_buf
        mov     [xm_soff+2], cs
        mov     ax, [bi_xms_handle]
        mov     [xm_dhandle], ax
        mov     eax, [cur_phys]
        sub     eax, [bi_ext_base]
        mov     [xm_doff], eax
        mov     si, xms_move
        mov     ah, 0Bh
        push    ds
        call    far [xms_entry]
        pop     ds
        cmp     ax, 1
        jne     .err
        clc
        ret
.raw:   ; unreal mode copy: load flat limits into ES then reload real values
        cli
        push    ds
        push    es
        movzx   eax, word [bi_psp]
        shl     eax, 4
        add     eax, gdt
        mov     [gdtr+2], eax
        lgdt    [gdtr]
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax
        jmp     short $+2
        mov     ax, 10h
        mov     es, ax
        mov     ds, ax
        mov     eax, cr0
        and     al, 0FEh
        mov     cr0, eax
        jmp     short $+2
        xor     ax, ax
        mov     es, ax
        mov     ds, ax                  ; ES/DS now flat with 4G limits
        movzx   esi, word [cs:bi_psp]
        shl     esi, 4
        add     esi, xfer_buf
        mov     edi, [cs:cur_phys]
        movzx   ecx, word [cs:chunk_len]
        a32 rep movsb
        pop     es
        pop     ds
        sti
        clc
        ret
.err:   mov     [fail_ax], bx
        stc
        ret

; ----------------------------------------------------------------------
; DOS memory fix-ups: mark free blocks below us as used, disable UMB link,
; shrink our own block to the resident size.
fix_dosmem:
        ; UMB link state
        mov     ax, 5802h
        int     21h
        jc      .noumb
        movzx   ax, al
        mov     [umb_link], ax
        mov     ax, 5803h
        xor     bx, bx
        int     21h
.noumb:
        ; shrink ourselves
        mov     bx, resident_end + 15
        shr     bx, 4
        mov     [bi_res_paras], bx
        mov     ah, 4Ah
        int     21h
        ; walk MCB chain
        push    es
        mov     ah, 52h
        int     21h
        mov     ax, [es:bx-2]           ; first MCB segment
        pop     es
        mov     word [mcb_fix_cnt], 0
.walk:  push    es
        mov     es, ax
        mov     cx, [es:3]              ; size in paras
        cmp     byte [es:0], 'M'
        je      .m
        cmp     byte [es:0], 'Z'
        jne     .stop
.m:     cmp     word [es:1], 0
        jne     .nxt
        cmp     ax, [bi_psp]
        jae     .nxt
        ; free block below us -> claim it
        mov     word [es:1], 8
        mov     bx, [mcb_fix_cnt]
        cmp     bx, 64
        jae     .nxt
        shl     bx, 1
        mov     [mcb_fix_list+bx], ax
        inc     word [mcb_fix_cnt]
.nxt:   cmp     byte [es:0], 'Z'
        je      .stop
        pop     es
        add     ax, cx
        inc     ax
        jmp     .walk
.stop:  pop     es
        ret

restore_dosmem:
        push    es
        mov     cx, [mcb_fix_cnt]
        xor     bx, bx
.l:     cmp     bx, cx
        jae     .d
        push    bx
        shl     bx, 1
        mov     ax, [mcb_fix_list+bx]
        mov     es, ax
        mov     word [es:1], 0
        pop     bx
        inc     bx
        jmp     .l
.d:     pop     es
        mov     bx, [umb_link]
        mov     ax, 5803h
        int     21h
        ret

; ----------------------------------------------------------------------
; v42 (#494): what the BIOS says of each hard disk and three of its sectors - the first, one in the middle, one near
; the end - read here in real mode for the kernel, which matches each disk it drives itself (SATA, NVMe, USB) to
; its BIOS unit by them (kernel/ata.c kd_setup).  A BIOS whose code for such a disk is 32-bit - SeaBIOS's for USB,
; AHCI, NVMe: QEMU's own BIOS, coreboot's - cannot run it in V86 mode: the kernel's reads of a sector there stopped
; the PC at its start (QEMU, a USB stick plugged in: an invalid opcode inside the BIOS).  Into memory DOS95 does not
; keep - the paragraphs after this program's block once it has shrunk itself, past the free block's header; the
; kernel copies them before anything can use that memory (kernel/main.c).  A unit's 2048 bytes:
;   +0 flags (1: AH=08h answered, 2: AH=15h, 4: AH=41h, 8: AH=48h, 10h: the three sectors read), +1 AH of 15h,
;   +2 AH of 41h, +4 CX, DX of 08h, +8 CX, DX of 15h, +12 CX of 41h, +14 the bytes AH=48h filled, +16 its 42h bytes,
;   +128 the three sectors (at 0, sectors/2 + 1, sectors - 63) - as kernel/ata.c bios_answers asks them
PROBE_MAX       equ 8
PROBE_STRIDE    equ 2048
probe_disks:
        pushad
        push    es
        mov     ax, cs
        add     ax, [bi_res_paras]
        inc     ax                      ; (past the header of the free block after ours)
        mov     [bi_probe_seg], ax
        mov     es, ax
        xor     di, di
        xor     ax, ax
        mov     cx, PROBE_MAX*PROBE_STRIDE/2
        rep     stosw
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     al, [0475h]             ; (the BIOS's count of hard disks)
        pop     ds
        cmp     ax, PROBE_MAX
        jbe     .cnt
        mov     ax, PROBE_MAX
.cnt:   mov     [bi_probe_n], ax
        xor     bp, bp
.unit:  cmp     bp, [bi_probe_n]
        jae     .done
        mov     di, bp
        shl     di, 11
        mov     [pr_base], di
        mov     ax, bp
        add     al, 80h
        mov     [pr_unit], al
        ; AH=08h: its geometry
        push    es
        mov     ah, 08h
        mov     dl, [pr_unit]
        xor     di, di
        int     13h
        pop     es
        mov     di, [pr_base]
        jc      .no08
        or      byte [es:di], 1
        mov     [es:di+4], cx
        mov     [es:di+6], dx
.no08:  mov     ah, 15h
        mov     dl, [pr_unit]
        int     13h
        mov     di, [pr_base]
        jc      .no15
        or      byte [es:di], 2
        mov     [es:di+1], ah
        mov     [es:di+8], cx
        mov     [es:di+10], dx
.no15:  mov     ah, 41h
        mov     bx, 55AAh
        mov     dl, [pr_unit]
        int     13h
        mov     di, [pr_base]
        jc      .no41
        cmp     bx, 0AA55h
        jne     .no41
        or      byte [es:di], 4
        mov     [es:di+2], ah
        mov     [es:di+12], cx
        test    cl, 1
        jz      .no41
        ; AH=48h: its EDD, room for 42h bytes
        push    es
        push    cs
        pop     es
        mov     di, pr_edd
        xor     ax, ax
        mov     cx, 42h/2
        rep     stosw
        pop     es
        mov     word [pr_edd], 42h
        mov     ah, 48h
        mov     dl, [pr_unit]
        mov     si, pr_edd
        int     13h
        mov     di, [pr_base]
        jc      .no41
        or      byte [es:di], 8
        mov     ax, [pr_edd]
        mov     [es:di+14], ax
        push    di
        add     di, 16
        mov     si, pr_edd
        mov     cx, 42h
        rep     movsb
        pop     di
.no41:
        ; its sectors: EDD's count (when it gave 18h bytes or more), or its geometry's
        xor     eax, eax
        xor     edx, edx
        test    byte [es:di], 8
        jz      .chs
        cmp     word [es:di+14], 18h
        jb      .chs
        mov     eax, [es:di+16+10h]
        mov     edx, [es:di+16+14h]
.chs:   mov     ebx, eax
        or      ebx, edx
        jnz     .gotn
        test    byte [es:di], 1
        jz      .next                   ; (no count: no sectors read - the kernel asks the BIOS then)
        movzx   eax, word [es:di+4]     ; cylinders: CH, CL's top two bits
        mov     ebx, eax
        shr     eax, 8
        and     ebx, 0C0h
        shl     ebx, 2
        or      eax, ebx
        inc     eax
        movzx   ebx, byte [es:di+7]     ; heads: DH + 1
        inc     ebx
        mul     ebx
        movzx   ebx, byte [es:di+4]
        and     ebx, 3Fh                ; sectors a track
        mul     ebx
.gotn:  mov     [pr_n], eax
        mov     [pr_n+4], edx
        ; the first sector
        xor     eax, eax
        xor     edx, edx
        mov     cx, 128
        call    .read
        jc      .next
        ; one in the middle: n / 2 + 1
        mov     eax, [pr_n]
        mov     edx, [pr_n+4]
        shrd    eax, edx, 1
        shr     edx, 1
        add     eax, 1
        adc     edx, 0
        mov     cx, 128+512
        call    .read
        jc      .next
        ; one near the end: n - 63 (0 for a disk of 64 sectors or fewer)
        mov     eax, [pr_n]
        mov     edx, [pr_n+4]
        test    edx, edx
        jnz     .big
        cmp     eax, 64
        ja      .big
        xor     eax, eax
        jmp     .last
.big:   sub     eax, 63
        sbb     edx, 0
.last:  mov     cx, 128+1024
        call    .read
        jc      .next
        or      byte [es:di], 10h
.next:  inc     bp
        jmp     .unit
.done:  pop     es
        popad
        ret
; (EDX:EAX the sector, CX where in this unit's bytes: read with AH=42h - CF on failure)
.read:  mov     [pr_dap+8], eax
        mov     [pr_dap+12], edx
        mov     ax, di
        add     ax, cx
        mov     [pr_dap+4], ax
        mov     [pr_dap+6], es
        mov     word [pr_dap+2], 1
        mov     ah, 42h
        mov     dl, [pr_unit]
        mov     si, pr_dap
        int     13h
        mov     di, [pr_base]
        ret
pr_base dw 0
pr_unit db 0, 0
pr_n    dd 0, 0
pr_dap  db 16, 0
        dw 1, 0, 0
        dd 0, 0
pr_edd  times 42h db 0

; ----------------------------------------------------------------------
install_hooks:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, cs
        mov     word [es:10h*4], stub_int10
        mov     [es:10h*4+2], ax
        mov     word [es:13h*4], stub_int13
        mov     [es:13h*4+2], ax
        mov     word [es:15h*4], stub_int15
        mov     [es:15h*4+2], ax
        mov     word [es:16h*4], stub_int16
        mov     [es:16h*4+2], ax
        mov     word [es:17h*4], stub_int17     ; v41 (#251)
        mov     [es:17h*4+2], ax
        mov     word [es:23h*4], stub_int23
        mov     [es:23h*4+2], ax
        mov     word [es:24h*4], stub_int24
        mov     [es:24h*4+2], ax
        mov     word [es:28h*4], stub_int28
        mov     [es:28h*4+2], ax
        mov     word [es:2Fh*4], stub_int2f
        mov     [es:2Fh*4+2], ax
        mov     word [es:33h*4], stub_int33
        mov     [es:33h*4+2], ax
        mov     word [es:74h*4], stub_int74
        mov     [es:74h*4+2], ax
        ; INT 67h -> EMS.  Point the vector at the device header's segment
        ; (base at emm_hdr, so its offset 0 lands on the header) with the
        ; fallback IRET as the offset.  ax still = cs here.  emm_hdr is
        ; paragraph-aligned, so emm_hdr/16 is exact.
        mov     bx, emm_hdr
        shr     bx, 4                    ; paragraph of emm_hdr within this segment
        add     bx, ax                   ; bx = cs + emm_hdr/16  (segment of the header)
        mov     word [es:67h*4], emm_entry - emm_hdr
        mov     [es:67h*4+2], bx
        pop     es
        ret

; ----------------------------------------------------------------------
str_comspec db 'COMSPEC=',0
%ifdef QDP
; the QEMU DOS port (Makefile.qdp: nasm -DQDP): its own kernel and names - the loader is DOS95's
str_krn     db 'QEMUDOS.KRN',0
root_path   db 'C:\',0
msg_banner  db 'QEMU DOS port loader 1.0',13,10,'$'
msg_cpu     db 'The QEMU DOS port requires an 80386 or better.$'
msg_v86     db 'The CPU is already in V86 mode (EMM386/JEMM/QEMM loaded?).',13,10
            db 'Please boot without a memory manager other than HIMEM.$'
msg_win     db 'The QEMU DOS port cannot run under Windows.$'
msg_dos     db 'DOS 3.1 or later is required.$'
msg_sda     db 'This DOS does not support the Swappable Data Area (INT 21h/5D06h).$'
msg_mem     db 'Not enough extended memory (need at least 1.5 MB free).$'
msg_load    db 'Cannot load QEMUDOS.KRN (must be in the same directory as QEMUDOS.COM).$'
msg_loading db 'Loading kernel...',13,10,'$'
msg_crlf    db 13,10,'$'
msg_bye     db 13,10,'QEMU DOS port ended.',13,10,'$'
str_dosmode db 'DOSMODE.FLG',0
msg_dosmode db 'The QEMU DOS port restarted the computer into MS-DOS (Exit to MS-DOS).',13,10
            db 'Type QEMUDOS to start it again.',13,10,'$'
%else
str_krn     db 'DOS95.KRN',0
root_path   db 'C:\',0
msg_banner  db 'DOS95 loader 1.0',13,10,'$'
msg_cpu     db 'DOS95 requires an 80386 or better.$'
msg_v86     db 'The CPU is already in V86 mode (EMM386/JEMM/QEMM loaded?).',13,10
            db 'Please boot without a memory manager other than HIMEM.$'
msg_win     db 'DOS95 cannot run under Windows.$'
msg_dos     db 'DOS 3.1 or later is required.$'
msg_sda     db 'This DOS does not support the Swappable Data Area (INT 21h/5D06h).$'
msg_mem     db 'Not enough extended memory (need at least 1.5 MB free).$'
msg_load    db 'Cannot load DOS95.KRN (must be in the same directory as DOS95.COM).$'
msg_loading db 'Loading kernel...',13,10,'$'
msg_crlf    db 13,10,'$'
msg_bye     db 13,10,'DOS95 session ended.',13,10,'$'
str_dosmode db 'DOSMODE.FLG',0
msg_dosmode db 'DOS95 restarted the computer into MS-DOS (Shut Down, Return to MS-DOS).',13,10
            db 'Type DOS95 to start it again.',13,10,'$'
%endif
dm_fdate    dw 0
dm_fmin     dw 0
exit_msg    times 480 db '$'            ; (v41, #437: bi_exitmsg_off)

file_h      dw 0
fail_step   dw 0
fail_ax     dw 0
chunk_len   dw 0
cur_phys    dd 0
        align 4
xms_move:
xm_len      dd 0
xm_shandle  dw 0
xm_soff     dd 0
xm_dhandle  dw 0
xm_doff     dd 0
path_buf    times 160 db 0

; ----------------------------------------------------------------------
;  Uninitialised resident areas
; ----------------------------------------------------------------------
        align 16
old_ivt:    times 1024 db 0
        align 16
xfer_buf:   times XFER_SIZE db 0
        align 16
vm_env:     times VMENV_SIZE db 0       ; per-VM environment: ours + BLASTER=
        align 16
pkt_buf:    times 2*1536 db 0           ; user packet driver: TX frame, RX frame
        align 16
pmret_frame: times 32 db 0              ; the kernel's IRET frame for a protected-mode client return
        align 16
stack_bot:  times STACK_SIZE db 0
stack_top:
resident_end:
