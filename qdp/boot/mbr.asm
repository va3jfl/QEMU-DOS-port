; QEMU DOS port - the master boot record its setup writes (and its setup disk has): the active partition's first
; sector read to 0000:7C00 - by INT 13h's extensions (LBA: a partition anywhere on a disk of up to 2 TB), else by its
; cylinder, head and sector - and started with DL its unit and DS:SI its partition entry, as every MBR has done since
; DOS 2.  Its own: written for the QEMU DOS port (no code of anyone else's).   nasm -f bin -o mbr.bin mbr.asm
        bits    16
        org     0x0600
start:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     es, ax
        mov     si, 0x7C00
        mov     di, 0x0600
        mov     cx, 256
        cld
        rep     movsw                       ; (to 0000:0600: the partition's own sector goes to 7C00)
        jmp     0:here
here:   sti
        mov     [drive], dl
        mov     si, ptable
        mov     cx, 4
.find:  test    byte [si], 0x80
        jnz     found
        add     si, 16
        loop    .find
        mov     si, msg_noactive
        jmp     fail
found:  mov     [entry], si
        mov     di, 5                       ; (tries: a floppy-era BIOS may want a reset between)
.try:   mov     ah, 0x41                    ; extensions?
        mov     bx, 0x55AA
        mov     dl, [drive]
        int     0x13
        jc      .chs
        cmp     bx, 0xAA55
        jne     .chs
        test    cl, 1
        jz      .chs
        mov     si, [entry]
        mov     eax, [si + 8]
        mov     [dap_lba], eax
        mov     si, dap
        mov     ah, 0x42
        mov     dl, [drive]
        int     0x13
        jnc     .read
        jmp     .again
.chs:   mov     si, [entry]
        mov     dh, [si + 1]
        mov     cx, [si + 2]
        mov     bx, 0x7C00
        mov     ax, 0x0201
        mov     dl, [drive]
        int     0x13
        jnc     .read
.again: xor     ax, ax                      ; (reset, and once more)
        mov     dl, [drive]
        int     0x13
        dec     di
        jnz     .try
        mov     si, msg_ioerr
        jmp     fail
.read:  cmp     word [0x7DFE], 0xAA55
        je      .go
        mov     si, msg_nosys
        jmp     fail
.go:    mov     dl, [drive]
        mov     si, [entry]
        jmp     0:0x7C00
fail:   lodsb                               ; (the message, then INT 18h: the BIOS's next boot device or its own message)
        or      al, al
        jz      .halt
        mov     ah, 0x0E
        mov     bx, 7
        int     0x10
        jmp     fail
.halt:  int     0x18
        cli
        hlt
        jmp     .halt

drive   db      0x80
entry   dw      0
        align   4
dap     db      16, 0
        dw      1                           ; one sector
        dw      0x7C00, 0                   ; to 0000:7C00
dap_lba dd      0, 0
msg_noactive db 'QEMU DOS port: no active partition on this disk', 13, 10, 0
msg_ioerr    db 'QEMU DOS port: the active partition cannot be read', 13, 10, 0
msg_nosys    db 'QEMU DOS port: the active partition has no system on it', 13, 10, 0

        times   0x1B8 - ($ - start) db 0
sig     dd      0                           ; (the disk's signature: the setup writes one)
        dw      0
ptable  times   64 db 0                     ; (the partitions: the setup writes them)
        dw      0xAA55
