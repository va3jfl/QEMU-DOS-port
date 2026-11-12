/* DOS95 kernel - the hard disks read by the kernel itself: 32-bit disk access (v41, #412)
 *
 * Until v41 every sector came from the BIOS: INT 13h in the system VM (or a box), the BIOS's
 * own code in V86 mode driving the ATA controller a port at a time - each IN and OUT a trap, a
 * sector some 13,000 instructions and sixty traps.  The 7-Zip File Manager's first start read
 * 28,000 sectors so and spent two of its seven seconds there (at 125 million instructions a
 * second); SeaMonkey's and FileZilla's as much.  Windows 3.1's 32-bit disk access (WDCTRL) and
 * Windows 95's port driver did what is done here: the kernel gives the controller the command
 * itself and takes the sectors with one REP INSW each.
 *
 * Which disk is which: a hard disk is read here only when the BIOS itself says where it is -
 * its EDD table (INT 13h AH=48h) with the device's ports (the DPTE: the first ATA channel,
 * master or slave, not an ATAPI device), the device answers IDENTIFY with the size the BIOS
 * gives, and sectors read both ways are the same.  Anything else - no EDD table, the second
 * channel (a box's CD-ROM driver drives it outside INT 13h), a SCSI or USB disk, a size or a
 * sector that differs - stays the BIOS's, as before.  So does every call but the extended read
 * and (v41, #426) the extended write (dcache.c): a write through the BIOS was as dear as a read,
 * the BIOS's own polling of the controller trap after trap - two thousand traps a sector.
 *
 * When: a BIOS disk call runs to its end before anything else touches the controller (a VM
 * inside INT 13h is not switched from, vm->crit), and a command given here runs to its end
 * before any other thread runs (ata_busy: sched.c does not switch while it is set).  While the
 * disk is busy the interrupts are let in (the mouse, the sound card's DMA, the clock); the
 * device's own interrupt is off for the kernel's commands (nIEN) and taken away at their end.
 *
 * [Disk] Direct= in DOS95.INI: 1 (default) as above; 0 every disk through the BIOS, as in v40;
 * 2 the check - every read the kernel makes for a VM's INT 13h is made through the BIOS as
 * well and the two compared ("disk: WRONG" in the log).  A command that fails puts that disk
 * back to the BIOS for good, with a line in the log. */
#include "kernel.h"
#include "vmmem.h"
#include "pci.h"
#include "kdisk.h"

#define ATA_IO   0x1F0
#define ATA_CTL  0x3F6
#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_DF    0x20
#define ST_BSY   0x80

int ata_mode = 1;                           /* [Disk] Direct= (ini.c) */
volatile int ata_busy;                      /* a command of the kernel's in flight: no thread switch (sched.c) - v42: a count
                                             * (ahci.c, nvme.c: a command inside the taking of a controller) */
volatile int hd_bios_busy;                  /* VMs inside the BIOS for a hard disk now (dcache.c) */
uint32_t ata_cmds, ata_sectors, ata_checked, ata_wrong, ata_wsectors;
uint32_t ata_polls;                         /* the looks at the status while the disk was busy: its time, under an emulator */

struct atadev { uint8_t present, lba48, mult; uint64_t sectors; char model[41]; };   /* mult: sectors a block (READ MULTIPLE), 0 one at a time */
static struct atadev adev[2];
static int8_t unit_dev[16];                 /* BIOS unit 80h+i: the device read here (0 master, 1 slave), -1 the BIOS's */
static int ata_state;                       /* 0 not set up, 1 set up, -1 nothing here */
/* v42 (#469 #470): disks on AHCI and NVMe controllers (ahci.c, nvme.c): unit_dev 2 + k is kd[k] */
static struct kdisk *kd[KDISK_MAX];
static int nkd;
/* what the BIOS answered for a unit before the kernel took its disk - INT 13h AH=08h, 15h, 41h, 48h - given
 * again by the kernel for it (dcache.c kd_int13): no machine calls the BIOS's disk code for it any more */
static struct ubios ub[16];
/* v42 (#488): a GPT disk - its own sector 0 a protective MBR (an entry of type EEh).  Under the BIOS layer (CSMWrap's
 * SeaBIOS: tools/csmwrap block.c gptmbr_setup) DOS is given, for sector 0, an MBR of the disk's FAT partitions (its boot
 * code says "DOS95GPT" at 1B0h) - or, with none, the protective MBR as it is; with a BIOS of its own, the protective MBR.
 * The sector DOS is given is taken when the disk is matched to its unit and given here for sector 0 as well; and a write
 * is refused (INT 13h: AH=03h, write protected) unless it lies wholly in a partition of it - as the BIOS layer does:
 * nothing of a GPT disk but the partitions DOS is shown is written (FDISK would spoil the GPT, or the partitions DOS
 * is not shown - NTFS, exFAT, Linux - writing there) */
static uint8_t *unit_gpt0[16];
static int is_prot(const uint8_t *s0)
{
    if (s0[510] != 0x55 || s0[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) if (s0[0x1BE + i * 16 + 4] == 0xEE) return 1;
    return 0;
}
static int is_vmbr(const uint8_t *s0) { return !memcmp(s0 + 0x1B0, "DOS95GPT", 8) && s0[510] == 0x55 && s0[511] == 0xAA; }
static int gpt_may_write(const uint8_t *m, uint64_t lba, int count)
{
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = m + 0x1BE + i * 16;
        uint32_t start = *(const uint32_t *)(e + 8), size = *(const uint32_t *)(e + 12);
        if (e[4] && e[4] != 0xEE && lba >= start && lba + (uint64_t)count <= (uint64_t)start + size) return 1;
    }
    return 0;
}
static void gpt_keep(int u, const uint8_t *bios0)
{
    if (!unit_gpt0[u]) unit_gpt0[u] = kmalloc(512);
    if (!unit_gpt0[u]) return;
    memcpy(unit_gpt0[u], bios0, 512);
    if (!is_vmbr(bios0)) { kprintf("disk: unit %02x is a GPT disk - DOS is given its protective MBR, no partition of it: none of it is written\n", 0x80 + u); return; }
    kprintf("disk: unit %02x is a GPT disk - DOS is given the MBR its BIOS made of its FAT partitions:", 0x80 + u);
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = bios0 + 0x1BE + i * 16;
        if (e[4]) kprintf(" %s%02x at %u (%u MB)", e[0] & 0x80 ? "*" : "", e[4], *(const uint32_t *)(e + 8), *(const uint32_t *)(e + 12) / 2048);
    }
    kprintf(" - nothing else of the disk is written\n");
}

static void delay400(void) { inb(ATA_CTL); inb(ATA_CTL); inb(ATA_CTL); inb(ATA_CTL); }
/* the status (alternate: reading it does not take the device's interrupt request away) until
 * (st & mask) == want - the status, or -1: not in time / nothing there.  After a few looks the
 * interrupts are let in between them (a real disk's seek is milliseconds) */
/* v41 (#415): ... and the processor's clock (its TSC) while it waited, which under an emulator counting instructions is
 * the disk's time in the machine's own nanoseconds - what the starts' measuring (speed41, vstart.py) leaves out of a
 * program's start: a count of looks times what one costs was only near it */
uint64_t ata_wait_tsc;
static inline uint64_t ata_tsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static int ata_poll(uint8_t mask, uint8_t want)
{
    extern int cpu_level;
    uint64_t t0 = cpu_level >= 5 ? ata_tsc() : 0;
    int r = -1;
    for (uint32_t i = 0; i < 30000000u; i++) {
        uint8_t st = inb(ATA_CTL);
        if (st == 0xFF) break;                              /* (no device: the bus floats) */
        if ((st & mask) == want) { r = st; break; }
        ata_polls++;
        if (i >= 64) { sti(); __asm__ volatile("nop; nop; nop; nop"); cli(); }
    }
    if (cpu_level >= 5) ata_wait_tsc += ata_tsc() - t0;
    return r;
}
static int ata_begin(void)
{
    if (hd_bios_busy) return 0;                             /* a VM is inside the BIOS for a hard disk */
    ata_busy++;
    int st = inb(ATA_CTL);
    if (st == 0xFF || (st & (ST_BSY | ST_DRQ))) { ata_busy--; return 0; }   /* (not idle: someone else's command) */
    outb(ATA_CTL, 0x0A);                                    /* nIEN: no interrupt for the kernel's commands */
    return 1;
}
static void ata_end(void)
{
    inb(ATA_IO + 7);                                        /* (the status: the device's request is gone) */
    outb(ATA_CTL, 0x08);                                    /* nIEN off again, as the BIOS keeps it */
    ata_busy--;
}
static void read_words(uint8_t *buf, int n)
{
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(n) : "d"(ATA_IO) : "memory");
}
/* v41 (#448): every page a read goes into made present - and writable - before the command is given.  A REP INSW
 * whose store meets a page not there yet (a file's buffer is memory committed as it is touched: kmalloc_pages, vmem.c)
 * faults in the middle, and the word it had taken from the port is lost on a processor that reads the port before it
 * looks at the page: PCem's and 86Box's do (QEMU's and the real ones look first) - the transfer goes on one word late
 * from every new page, its last words what the device never sent.  On PCem every library past its first page read so:
 * "D95POSIX.DLL: no symbol table", and the cache under INT 13h kept those sectors for DOS.  (Windows 95's port
 * driver reads into locked pages too.) */
static void touch_pages(uint8_t *buf, uint32_t bytes)
{
    if (!bytes) return;
    uint8_t *end = buf + bytes;
    for (uint8_t *p = buf; p < end; p = (uint8_t *)(((uint32_t)p & ~0xFFFu) + 4096))
        *(volatile uint8_t *)p = *(volatile uint8_t *)p;
    *(volatile uint8_t *)(end - 1) = *(volatile uint8_t *)(end - 1);
}
static void write_words(const uint8_t *buf, int n)
{
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(n) : "d"(ATA_IO) : "memory");
}
/* IDENTIFY DEVICE: 0 and the 256 words, or -1 (nothing, or not an ATA disk - an ATAPI device aborts it) */
static int ata_identify(int dev, uint16_t *id)
{
    int ok = -1;
    uint32_t f = save_flags_cli();
    if (!ata_begin()) { restore_flags(f); return -1; }
    outb(ATA_IO + 6, 0xA0 | (dev << 4)); delay400();
    if (ata_poll(ST_BSY | ST_DRQ, 0) >= 0) {
        outb(ATA_IO + 2, 0); outb(ATA_IO + 3, 0); outb(ATA_IO + 4, 0); outb(ATA_IO + 5, 0);
        outb(ATA_IO + 7, 0xEC); delay400();
        uint8_t st = inb(ATA_IO + 7);
        if (st && st != 0xFF) {
            int s = ata_poll(ST_BSY, 0);
            if (s >= 0 && !(s & (ST_ERR | ST_DF)) && !inb(ATA_IO + 4) && !inb(ATA_IO + 5) &&
                ata_poll(ST_BSY | ST_DRQ, ST_DRQ) >= 0) { read_words((uint8_t*)id, 256); ok = 0; }
        }
    }
    ata_end();
    restore_flags(f);
    return ok;
}
/* count (1..128) sectors from lba of the device into buf - a block of d->mult sectors at a time
 * (READ MULTIPLE: one handshake a block, not a sector), or one at a time (mult 0): 0; -1 the
 * command failed; -2 not given (the controller busy with another's, or a request it cannot take) */
static int ata_pio_read1(int dev, uint64_t lba, int count, uint8_t *buf, int mult)
{
    struct atadev *d = &adev[dev];
    int ext = lba + (uint64_t)count > 0x0FFFFFFFull;
    if ((ext && !d->lba48) || lba + (uint64_t)count > d->sectors || count < 1 || count > 128) return -2;
    touch_pages(buf, (uint32_t)count * 512);         /* (#448: before the command - a fault inside REP INSW loses a word) */
    uint32_t f = save_flags_cli();
    if (!ata_begin()) { restore_flags(f); return -2; }
    int ok = -1, blk = mult >= 2 ? mult : 1;
    outb(ATA_IO + 6, (ext ? 0x40 : 0xE0 | (uint8_t)((lba >> 24) & 0x0F)) | (dev << 4)); delay400();
    if (ata_poll(ST_BSY | ST_DRQ, 0) >= 0) {
        if (ext) { outb(ATA_IO + 1, 0); outb(ATA_IO + 2, 0); outb(ATA_IO + 3, (uint8_t)(lba >> 24)); outb(ATA_IO + 4, (uint8_t)(lba >> 32)); outb(ATA_IO + 5, (uint8_t)(lba >> 40)); }
        outb(ATA_IO + 1, 0); outb(ATA_IO + 2, (uint8_t)count);
        outb(ATA_IO + 3, (uint8_t)lba); outb(ATA_IO + 4, (uint8_t)(lba >> 8)); outb(ATA_IO + 5, (uint8_t)(lba >> 16));
        outb(ATA_IO + 7, blk > 1 ? (ext ? 0x29 : 0xC4) : (ext ? 0x24 : 0x20));
        int i;
        for (i = 0; i < count; ) {
            delay400();
            int st = ata_poll(ST_BSY, 0);
            if (st < 0 || (st & (ST_ERR | ST_DF)) || !(st & ST_DRQ)) break;
            int k = count - i < blk ? count - i : blk;
            read_words(buf + i * 512, 256 * k);
            i += k;
            /* v44 (#526): the interrupts let in between blocks too (the clock, the pointer) - a device whose data is ready at
             * once (an emulator's, a fast disk) never made ata_poll wait: 128 sectors with interrupts off, 50 ms under QEMU */
            if (i < count) { sti(); __asm__ volatile("nop; nop; nop; nop"); cli(); }
        }
        if (i == count) { ok = 0; ata_cmds++; ata_sectors += (uint32_t)count; }
    }
    ata_end();
    restore_flags(f);
    return ok;
}
static int ata_pio_read(int dev, uint64_t lba, int count, uint8_t *buf)
{
    struct atadev *d = &adev[dev];
    int r = ata_pio_read1(dev, lba, count, buf, d->mult);
    if (r == -1 && d->mult) {
        /* (a READ MULTIPLE refused: the device's block size gone - a reset by the BIOS - so one sector at a time from now on) */
        r = ata_pio_read1(dev, lba, count, buf, 0);
        if (r == 0) { kprintf("disk: the first ATA channel's %s refused a block of %d sectors - read a sector at a time from now on\n", dev ? "slave" : "master", d->mult); d->mult = 0; }
    }
    return r;
}
/* v41 (#426): and written - WRITE SECTORS (WRITE MULTIPLE: a block of d->mult sectors a handshake), each block with
 * one REP OUTSW, the device's status looked at after each and at the end: 0; -1 the command failed; -2 not given */
static int ata_pio_write1(int dev, uint64_t lba, int count, const uint8_t *buf, int mult)
{
    struct atadev *d = &adev[dev];
    int ext = lba + (uint64_t)count > 0x0FFFFFFFull;
    if ((ext && !d->lba48) || lba + (uint64_t)count > d->sectors || count < 1 || count > 128) return -2;
    uint32_t f = save_flags_cli();
    if (!ata_begin()) { restore_flags(f); return -2; }
    int ok = -1, blk = mult >= 2 ? mult : 1;
    outb(ATA_IO + 6, (ext ? 0x40 : 0xE0 | (uint8_t)((lba >> 24) & 0x0F)) | (dev << 4)); delay400();
    if (ata_poll(ST_BSY | ST_DRQ, 0) >= 0) {
        if (ext) { outb(ATA_IO + 1, 0); outb(ATA_IO + 2, 0); outb(ATA_IO + 3, (uint8_t)(lba >> 24)); outb(ATA_IO + 4, (uint8_t)(lba >> 32)); outb(ATA_IO + 5, (uint8_t)(lba >> 40)); }
        outb(ATA_IO + 1, 0); outb(ATA_IO + 2, (uint8_t)count);
        outb(ATA_IO + 3, (uint8_t)lba); outb(ATA_IO + 4, (uint8_t)(lba >> 8)); outb(ATA_IO + 5, (uint8_t)(lba >> 16));
        outb(ATA_IO + 7, blk > 1 ? (ext ? 0x39 : 0xC5) : (ext ? 0x34 : 0x30));
        int i;
        for (i = 0; i < count; ) {
            delay400();
            int st = ata_poll(ST_BSY, 0);
            if (st < 0 || (st & (ST_ERR | ST_DF)) || !(st & ST_DRQ)) break;
            int k = count - i < blk ? count - i : blk;
            write_words(buf + i * 512, 256 * k);
            i += k;
            if (i < count) { sti(); __asm__ volatile("nop; nop; nop; nop"); cli(); }   /* (v44, #526: as the read's) */
        }
        if (i == count) {                                   /* the last block taken: the device done with it, no error */
            delay400();
            int st = ata_poll(ST_BSY, 0);
            if (st >= 0 && !(st & (ST_ERR | ST_DF | ST_DRQ))) { ok = 0; ata_cmds++; ata_sectors += (uint32_t)count; ata_wsectors += (uint32_t)count; }
        }
    }
    ata_end();
    restore_flags(f);
    return ok;
}
static int ata_pio_write(int dev, uint64_t lba, int count, const uint8_t *buf)
{
    struct atadev *d = &adev[dev];
    int r = ata_pio_write1(dev, lba, count, buf, d->mult);
    if (r == -1 && d->mult) {
        r = ata_pio_write1(dev, lba, count, buf, 0);
        if (r == 0) { kprintf("disk: the first ATA channel's %s refused a block of %d sectors - written a sector at a time from now on\n", dev ? "slave" : "master", d->mult); d->mult = 0; }
    }
    return r;
}
/* SET MULTIPLE MODE: the block size the device keeps from now on - 0, or -1 */
static int ata_set_mult(int dev, int n)
{
    uint32_t f = save_flags_cli();
    if (!ata_begin()) { restore_flags(f); return -1; }
    int ok = -1;
    outb(ATA_IO + 6, 0xA0 | (dev << 4)); delay400();
    if (ata_poll(ST_BSY | ST_DRQ, 0) >= 0) {
        outb(ATA_IO + 2, (uint8_t)n); outb(ATA_IO + 7, 0xC6); delay400();
        int st = ata_poll(ST_BSY, 0);
        if (st >= 0 && !(st & (ST_ERR | ST_DF))) ok = 0;
    }
    ata_end();
    restore_flags(f);
    return ok;
}

/* the BIOS's word on a unit, and a sector read through it (dossvc.c, in the system VM) */
extern int bios_edd(int unit, uint8_t *out, int n);
extern int bios_read_raw(int unit, uint64_t lba, uint8_t *buf);
extern uint8_t *vm_lowptr(struct vm *vm, uint32_t lin);
static int unit_check(int u, char *why, int wn)
{
    uint8_t e[0x42];
    int n = bios_edd(u, e, sizeof e);
    if (n < 0) { strlcpy(why, "no EDD from its BIOS", wn); return -1; }
    if (n < 0x1E || *(uint16_t*)(e + 0x18) != 512) { strlcpy(why, "its BIOS gives no table of its ports (EDD 1.x)", wn); return -1; }
    uint16_t doff = *(uint16_t*)(e + 0x1A), dseg = *(uint16_t*)(e + 0x1C);
    if (doff == 0xFFFF && dseg == 0xFFFF) { strlcpy(why, "its BIOS gives no table of its ports", wn); return -1; }
    const uint8_t *dp = vm_lowptr(vms[0], LIN(dseg, doff));
    uint8_t sum = 0; for (int i = 0; i < 16; i++) sum = (uint8_t)(sum + dp[i]);
    if (dp[14] >= 0x11 && sum) { strlcpy(why, "the table of its ports does not add up", wn); return -1; }
    if (*(uint16_t*)dp != ATA_IO) { ksnprintf(why, wn, "not on the first ATA channel (ports %03x)", *(uint16_t*)dp); return -1; }
    if (dp[10] & 0x60) { strlcpy(why, dp[10] & 0x40 ? "an ATAPI device" : "a removable disk", wn); return -1; }
    int dev = (dp[4] >> 4) & 1;
    if (!adev[dev].present) { ksnprintf(why, wn, "the %s does not answer IDENTIFY as an ATA disk", dev ? "slave" : "master"); return -1; }
    uint64_t bios = *(uint64_t*)(e + 0x10), mine = adev[dev].sectors;
    if (bios > mine || mine - bios > 16384) { ksnprintf(why, wn, "its BIOS says %u sectors, the device %u", (uint32_t)bios, (uint32_t)mine); return -1; }
    /* the same sectors both ways: the first, one in the middle, one near the end */
    static uint8_t a[512], b[512], v[512];
    uint64_t at[3] = { 0, bios / 2 + 1, bios > 64 ? bios - 63 : 0 };
    int gpt = 0;
    for (int k = 0; k < 3; k++) {
        if (bios_read_raw(u, at[k], a) < 0) { ksnprintf(why, wn, "its BIOS would not read sector %u", (uint32_t)at[k]); return -1; }
        if (ata_pio_read(dev, at[k], 1, b) != 0) { ksnprintf(why, wn, "the device would not read sector %u", (uint32_t)at[k]); return -1; }
        if (!k) { gpt = is_prot(b); memcpy(v, a, 512); }                     /* (v42, #488: a GPT disk) */
        if (memcmp(a, b, 512) && !(!k && gpt && is_vmbr(a))) { ksnprintf(why, wn, "sector %u is not the same both ways", (uint32_t)at[k]); return -1; }
    }
    if (gpt) gpt_keep(u - 0x80, v);
    return dev;
}
/* v42 (#469 #470): the units no ATA channel answered for, on a SATA (AHCI) or NVMe controller - today's PCs.
 * What the BIOS says of each such unit, and three of its sectors as the BIOS reads them, are taken first (through
 * the system VM, before any controller is touched); then, with no thread switch and no machine inside the BIOS
 * for a hard disk, the controllers are taken and each disk is matched to the unit whose size and sectors it
 * has.  A disk that matches no unit, or more than one, is given back to the BIOS (an AHCI port; an NVMe
 * controller cannot be - it was reset to be taken, and the log says so). */
extern int bios_int13(struct dosregs *r);
static void bios_answers(int u, struct ubios *b)
{
    struct dosregs r;
    memset(b, 0, sizeof *b);
    memset(&r, 0, sizeof r); r.ax = 0x0800; r.dx = (uint16_t)(0x80 + u);
    if (!bios_int13(&r)) { b->ok08 = 1; b->cx08 = r.cx; b->dx08 = r.dx; }
    memset(&r, 0, sizeof r); r.ax = 0x1500; r.dx = (uint16_t)(0x80 + u);
    if (!bios_int13(&r)) { b->ok15 = 1; b->ah15 = (uint8_t)(r.ax >> 8); b->cx15 = r.cx; b->dx15 = r.dx; }
    memset(&r, 0, sizeof r); r.ax = 0x4100; r.bx = 0x55AA; r.dx = (uint16_t)(0x80 + u);
    if (!bios_int13(&r) && r.bx == 0xAA55) { b->ok41 = 1; b->ah41 = (uint8_t)(r.ax >> 8); b->cx41 = r.cx; }
    int n = b->ok41 ? bios_edd(0x80 + u, b->edd, sizeof b->edd) : -1;
    b->eddlen = n > 0 ? (n < (int)sizeof b->edd ? n : (int)sizeof b->edd) : 0;
    if (b->eddlen >= 0x18) b->sectors = *(uint64_t *)(b->edd + 0x10);
    if (!b->sectors && b->ok08) {                       /* (no EDD: what its CHS reaches) */
        uint32_t cyl = (b->cx08 >> 8) | ((uint32_t)(b->cx08 & 0xC0) << 2), hd = (b->dx08 >> 8) + 1u, spt = b->cx08 & 0x3Fu;
        b->sectors = (uint64_t)(cyl + 1) * hd * spt;
    }
}
/* v42 (#487): USB disks - usbstor.c's, as each is ready.  During kd_setup's USB pass they are gathered here and
 * matched like the others; after DOS95's start, a disk pulled out and put back is its unit's again (its vendor,
 * product, serial and size), any other has no unit (DOS gave its letters at its start) */
extern int usb_take_early(void);
extern void exfat_disk_added(struct kdisk *d), exfat_disk_gone(struct kdisk *d);
/* v42 (#490): the disks the kernel drives that are no unit of the BIOS's (an NVMe namespace or a USB disk its BIOS did not
 * have) - DOS has no letter on them, but an exFAT volume on one gets one (exfat.c) */
static struct kdisk *nounit[KDISK_MAX];
static int n_nounit;
static struct kdisk *usb_pend[8];
static int n_usb_pend, usb_collect;
void ata_usb_disk(struct kdisk *d)
{
    if (usb_collect) { if (n_usb_pend < 8) usb_pend[n_usb_pend++] = d; return; }
    for (int k = 0; k < nkd; k++) {
        struct kdisk *o = kd[k];
        if (o && o != d && o->gone && o->ident[0] && !strcmp(o->ident, d->ident) && o->sectors == d->sectors) {
            uint32_t f = save_flags_cli(); kd[k] = d; restore_flags(f);
            /* (what the kernel and DOS keep of it may be stale - it may have been written elsewhere meanwhile) */
            extern void dcache_drop_unit(uint8_t unit);
            extern volatile uint32_t disk_write_gen_other;
            for (int u = 0; u < 16; u++) if (unit_dev[u] == 2 + k) { dcache_drop_unit((uint8_t)(0x80 + u)); kprintf("disk: unit %02x: its USB disk (%s) is back\n", 0x80 + u, d->model); }
            disk_write_gen_other++;
            exfat_disk_added(d);                        /* (v42, #490: its exFAT volumes again) */
            return;
        }
    }
    kprintf("disk: the USB disk at %s (%s, %u MB): plugged in after DOS95 started - no unit of DOS's for it (an exFAT volume on it gets a drive letter)\n", d->where, d->model, (uint32_t)(d->sectors / 2048));
    exfat_disk_added(d);                                /* (v42, #490) */
}
void ata_usb_gone(struct kdisk *d)
{
    exfat_disk_gone(d);                                 /* (v42, #490: its exFAT volumes' drive letters taken away) */
    for (int k = 0; k < nkd; k++)
        if (kd[k] == d) for (int u = 0; u < 16; u++) if (unit_dev[u] == 2 + k) kprintf("disk: unit %02x: its USB disk pulled out - \"not ready\" until it is back\n", 0x80 + u);
}
static int has_xhci(void)
{
    for (int i = 0; ; i++) { struct pci_dev *p = pci_find_class(0x0C, 0x03, i); if (!p) return 0; if (p->prog_if == 0x30) return 1; }
}
/* v42 (#494): the loader's look at each hard disk (loader.asm probe_disks): what the BIOS said, three of its sectors -
 * read in real mode.  A BIOS whose code for a USB, SATA (AHCI) or NVMe disk is 32-bit (SeaBIOS: QEMU's, coreboot's)
 * cannot run it in V86 mode - the kernel's own sector reads there stopped the PC at its start (an invalid opcode
 * inside the BIOS, a USB stick plugged in).  Copied at the kernel's start, before the memory it is in can be used */
static uint8_t *probe; static int nprobe;
void ata_probe_take(void)
{
    if (boot->probe_magic != 0x31425250 || !boot->probe_n || !boot->probe_seg) return;
    int n = boot->probe_n > 8 ? 8 : boot->probe_n;
    probe = kmalloc((uint32_t)n * 2048u);
    if (!probe) return;
    memcpy(probe, PHYSLOW((uint32_t)boot->probe_seg << 4), (uint32_t)n * 2048u);
    nprobe = n;
}
/* a unit's answers and samples from the loader's look: 1, or 0 (none for it: the BIOS is asked) */
static int probe_unit(int u, struct ubios *b, uint8_t smp[3][512], uint64_t at[3])
{
    if (u >= nprobe) return 0;
    const uint8_t *p = probe + (uint32_t)u * 2048u;
    uint8_t fl = p[0];
    memset(b, 0, sizeof *b);
    if (fl & 1) { b->ok08 = 1; b->cx08 = *(const uint16_t *)(p + 4); b->dx08 = *(const uint16_t *)(p + 6); }
    if (fl & 2) { b->ok15 = 1; b->ah15 = p[1]; b->cx15 = *(const uint16_t *)(p + 8); b->dx15 = *(const uint16_t *)(p + 10); }
    if (fl & 4) { b->ok41 = 1; b->ah41 = p[2]; b->cx41 = *(const uint16_t *)(p + 12); }
    if (fl & 8) { int got = *(const uint16_t *)(p + 14); b->eddlen = got > 0 ? (got < (int)sizeof b->edd ? got : (int)sizeof b->edd) : 0; memcpy(b->edd, p + 16, sizeof b->edd); }
    if (b->eddlen >= 0x18) b->sectors = *(const uint64_t *)(b->edd + 0x10);
    if (!b->sectors && b->ok08) {
        uint32_t cyl = (b->cx08 >> 8) | ((uint32_t)(b->cx08 & 0xC0) << 2), hd = (b->dx08 >> 8) + 1u, spt = b->cx08 & 0x3Fu;
        b->sectors = (uint64_t)(cyl + 1) * hd * spt;
    }
    uint64_t n = b->sectors;
    at[0] = 0; at[1] = n / 2 + 1; at[2] = n > 64 ? n - 63 : 0;
    if (!(fl & 0x10) || !n) return -1;                  /* (its sectors not read: no samples - not matched) */
    for (int k = 0; k < 3; k++) memcpy(smp[k], p + 128 + k * 512, 512);
    return 1;
}
static void kd_setup(int nhd)
{
    if (ata_mode == 2) return;                          /* ([Disk] Direct=2 compares with the BIOS: it keeps them) */
    int left = 0;
    for (int u = 0; u < nhd; u++) if (unit_dev[u] < 0) left++;
    if (!left) return;
    { extern void pci_init(void); pci_init(); }         /* (once: it keeps its table) */
    int has_ahci = pci_find_class(0x01, 0x06, 0) != NULL, has_nvme = pci_find_class(0x01, 0x08, 0) != NULL, has_usb = has_xhci();
    if (!has_ahci && !has_nvme && !has_usb) return;
    uint8_t (*smp)[3][512] = kmalloc(16 * 3 * 512);
    if (!smp) return;
    uint64_t at[16][3];
    int have[16];
    for (int u = 0; u < nhd; u++) {
        have[u] = 0;
        if (unit_dev[u] >= 0) continue;
        int pr = probe_unit(u, &ub[u], smp[u], at[u]);  /* (v42, #494: the loader's look, made in real mode) */
        if (pr) { have[u] = pr > 0; continue; }
        bios_answers(u, &ub[u]);
        uint64_t n = ub[u].sectors;
        if (!n) continue;
        at[u][0] = 0; at[u][1] = n / 2 + 1; at[u][2] = n > 64 ? n - 63 : 0;
        have[u] = 1;
        for (int k = 0; k < 3 && have[u]; k++) if (bios_read_raw(0x80 + u, at[u][k], smp[u][k]) < 0) have[u] = 0;
    }
    /* (no machine inside the BIOS for a hard disk: none of its commands half done on a port taken now) */
    for (int t = 0; t < 3000; t++) { uint32_t f = save_flags_cli(); if (!hd_bios_busy) { ata_busy++; restore_flags(f); goto taken; } restore_flags(f); thread_sleep_ms(10); }
    kprintf("disk: a machine inside the BIOS for a hard disk for 30 s - the AHCI and NVMe controllers left to the BIOS\n");
    kfree(smp);
    return;
  taken:;
    for (int pass = 0; pass < 3; pass++) {
        int from = nkd;
        if (pass == 0 && has_ahci) nkd += ahci_probe(kd + nkd, KDISK_MAX - nkd);
        if (pass >= 1) {
            int still = 0;
            for (int u = 0; u < nhd; u++) if (unit_dev[u] < 0 && have[u]) still++;
            if (!still) break;                          /* (no unit left: the NVMe and USB controllers the BIOS's for now) */
        }
        if (pass == 1 && has_nvme) nkd += nvme_probe(kd + nkd, KDISK_MAX - nkd);
        if (pass == 2 && has_usb) {                     /* v42 (#487): a unit left may be a USB disk - the BIOS's USB code
                                                         * reads it: the xHCI controllers taken now, before DOS95 reads a
                                                         * sector of its own, and each USB disk matched as the others */
            usb_collect = 1;
            usb_take_early();
            usb_collect = 0;
            for (int i = 0; i < n_usb_pend && nkd < KDISK_MAX; i++) kd[nkd++] = usb_pend[i];
            n_usb_pend = 0;
        }
        static uint8_t b[512];
        for (int k = from; k < nkd; k++) {
            struct kdisk *d = kd[k];
            int hit = -1, hits = 0, hitgpt = 0;
            for (int u = 0; u < nhd; u++) {
                if (unit_dev[u] >= 0 || !have[u] || ub[u].sectors > d->sectors || d->sectors - ub[u].sectors > 16384) continue;
                int same = 1, gpt = 0;
                for (int j = 0; j < 3 && same; j++) {
                    if (d->rw(d, at[u][j], 1, b, 0)) { same = 0; break; }
                    if (j == 0) gpt = is_prot(b);                           /* (v42, #488: a GPT disk) */
                    if (memcmp(b, smp[u][j], 512) && !(j == 0 && gpt && is_vmbr(smp[u][0]))) same = 0;
                }
                if (same) { hit = u; hits++; hitgpt = gpt; }
            }
            if (hits == 1) {
                if (hitgpt) gpt_keep(hit, smp[hit][0]);
                unit_dev[hit] = (int8_t)(2 + k);
                kprintf("disk: unit %02x is the %s disk at %s (%s, %u MB) - its size and sectors agree with its BIOS's: read and written by the kernel itself, every INT 13h call for it answered by the kernel\n",
                        0x80 + hit, d->kind, d->where, d->model, (uint32_t)(d->sectors / 2048));
            } else {
                kprintf("disk: the %s disk at %s (%s): %s\n", d->kind, d->where, d->model,
                        hits ? "two units of the BIOS have its sectors - left to the BIOS" :
                        d->give_back ? "no unit of the BIOS has its sectors - left to the BIOS" :
                        pass == 2 ? "no unit of the BIOS has its sectors - DOS has no letter for it" :
                                       "no unit of the BIOS has its sectors - and its BIOS driver was reset taking it: the BIOS cannot read it until the PC restarts");
                if (d->give_back) d->give_back(d);
                else if (n_nounit < KDISK_MAX) nounit[n_nounit++] = d;   /* (v42, #490: the kernel's still - exFAT) */
                kd[k] = NULL;
            }
        }
    }
    ata_busy--;
    kfree(smp);
}
/* once, from a kernel thread, before the first sector the kernel itself asks for */
void ata_setup(void)
{
    if (ata_state) return;
    ata_state = -1;
    for (int i = 0; i < 16; i++) unit_dev[i] = -1;
    if (!ata_mode) { kprintf("disk: [Disk] Direct=0 - every disk through the BIOS\n"); return; }
    static uint16_t id[256];
    int any = 0;
    for (int dev = 0; dev < 2; dev++) {
        struct atadev *d = &adev[dev];
        if (ata_identify(dev, id) < 0 || (id[0] & 0x8000) || !(id[49] & 0x200)) continue;   /* (an ATA disk with LBA) */
        d->lba48 = (id[83] & 0x400) != 0;
        d->sectors = d->lba48 ? *(uint64_t*)(id + 100) : (uint64_t)(id[60] | ((uint32_t)id[61] << 16));
        if (!d->sectors) continue;
        for (int i = 0; i < 20; i++) { d->model[2 * i] = (char)(id[27 + i] >> 8); d->model[2 * i + 1] = (char)id[27 + i]; }
        d->model[40] = 0; for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
        d->present = 1; any = 1;
        /* its block mode: the one the BIOS set, kept as it is; none set - the most it takes (16 at most) */
        int cur = (id[59] & 0x100) ? id[59] & 0xFF : 0, most = id[47] & 0xFF;
        if (cur >= 2) d->mult = (uint8_t)cur;
        else if (most >= 2 && ata_set_mult(dev, most > 16 ? 16 : most) == 0) d->mult = (uint8_t)(most > 16 ? 16 : most);
    }
    int nhd = *vm_lowptr(vms[0], 0x475);
    if (nhd > 16) nhd = 16;
    if (!any) kprintf("disk: no ATA disk on the first channel\n");
    else for (int u = 0; u < nhd; u++) {
        char why[96] = "";
        int dev = unit_check(0x80 + u, why, sizeof why);
        unit_dev[u] = (int8_t)dev;
        if (dev >= 0) kprintf("disk: unit %02x is the first ATA channel's %s (%s, %u MB) - its BIOS's own table says so and its sectors agree: read by the kernel itself, %d sector%s a block%s\n",
                              0x80 + u, dev ? "slave" : "master", adev[dev].model, (uint32_t)(adev[dev].sectors / 2048), adev[dev].mult ? adev[dev].mult : 1, adev[dev].mult ? "s" : "",
                              ata_mode == 2 ? " (and by the BIOS too, compared: [Disk] Direct=2)" : "");
        else kprintf("disk: unit %02x not on the first ATA channel - %s\n", 0x80 + u, why);
    }
    kd_setup(nhd);
    for (int u = 0; u < nhd; u++) if (unit_dev[u] < 0) kprintf("disk: unit %02x through its BIOS\n", 0x80 + u);
    ata_state = 1;
    /* v42 (#490): exFAT volumes on the disks the kernel drives - each given a drive letter after DOS's own */
    for (int k = 0; k < nkd; k++) if (kd[k]) exfat_disk_added(kd[k]);
    for (int k = 0; k < n_nounit; k++) exfat_disk_added(nounit[k]);
}
/* the device a unit is read from here, or -1 */
int ata_unit(uint8_t unit)
{
    if (ata_state != 1 || !ata_mode || unit < 0x80 || unit > 0x8F) return -1;
    return unit_dev[unit - 0x80];
}
/* count sectors of a unit from lba into buf, in commands of 128 at most: 0, or -1 (the unit is
 * the BIOS's from now on) */
int ata_read(uint8_t unit, uint64_t lba, int count, uint8_t *buf)
{
    int dev = ata_unit(unit);
    if (dev < 0) return -1;
    if (dev >= 2) {                                             /* (v42: AHCI, NVMe - the BIOS cannot take it over) */
        struct kdisk *d = kd[dev - 2];
        if (lba + (uint64_t)count > d->sectors || d->rw(d, lba, count, buf, 0)) return -1;
        if (!lba && count > 0 && unit_gpt0[unit - 0x80]) memcpy(buf, unit_gpt0[unit - 0x80], 512);    /* (#488) */
        return 0;
    }
    for (int i = 0; i < count; ) {
        int k = count - i > 128 ? 128 : count - i;
        int r = ata_pio_read(dev, lba + (uint64_t)i, k, buf + (uint32_t)i * 512);
        if (r == -2) return -1;                                 /* (busy, not broken: the BIOS this time) */
        if (r < 0) {
            unit_dev[unit - 0x80] = -1;
            kprintf("disk: unit %02x: the kernel's own read of sector %u failed (status %02x, error %02x) - through its BIOS from now on\n",
                    unit, (uint32_t)(lba + (uint64_t)i), inb(ATA_CTL), inb(ATA_IO + 1));
            return -1;
        }
        i += k;
    }
    if (!lba && count > 0 && unit_gpt0[unit - 0x80]) memcpy(buf, unit_gpt0[unit - 0x80], 512);        /* (#488) */
    return 0;
}
/* v41 (#426): count sectors of buf to a unit from lba, the same way: 0, or -1 (not given now - the BIOS writes them;
 * or a command failed: the unit is the BIOS's from now on, for reading too); v42 (#488): -3 - refused, outside the
 * partitions DOS is given of a GPT disk (write protected) */
int ata_write(uint8_t unit, uint64_t lba, int count, const uint8_t *buf)
{
    int dev = ata_unit(unit);
    if (dev < 0) return -1;
    if (unit_gpt0[unit - 0x80] && !gpt_may_write(unit_gpt0[unit - 0x80], lba, count)) {
        static uint32_t told;
        if (told++ < 8) kprintf("disk: unit %02x: a write to sector %u (%d) refused - outside the partitions DOS is given of this GPT disk\n", unit, (uint32_t)lba, count);
        return -3;
    }
    if (dev >= 2) {
        struct kdisk *d = kd[dev - 2];
        return lba + (uint64_t)count <= d->sectors && d->rw(d, lba, count, (uint8_t *)buf, 1) == 0 ? 0 : -1;
    }
    for (int i = 0; i < count; ) {
        int k = count - i > 128 ? 128 : count - i;
        int r = ata_pio_write(dev, lba + (uint64_t)i, k, buf + (uint32_t)i * 512);
        if (r == -2) { if (i) break; return -1; }               /* (busy before the first: the BIOS this time) */
        if (r < 0) break;
        i += k;
        if (i == count) return 0;
    }
    /* a command failed (or the controller was taken half way): through the BIOS from now on, and this write again by it */
    unit_dev[unit - 0x80] = -1;
    kprintf("disk: unit %02x: the kernel's own write from sector %u failed (status %02x, error %02x) - through its BIOS from now on\n",
            unit, (uint32_t)lba, inb(ATA_CTL), inb(ATA_IO + 1));
    return -1;
}
/* v42 (#490): the BIOS unit (80h...) a disk the kernel drives is, or -1 */
int ata_kdisk_unit(struct kdisk *d)
{
    for (int k = 0; k < nkd; k++) if (kd[k] == d) for (int u = 0; u < 16; u++) if (unit_dev[u] == 2 + k) return 0x80 + u;
    return -1;
}
/* v42: a unit every INT 13h call of which the kernel answers (dcache.c kd_int13) - its disk on AHCI or NVMe */
int ata_unit_all(uint8_t unit) { return ata_unit(unit) >= 2; }
/* v42 (#487): its USB disk pulled out - INT 13h says "not ready" */
int ata_unit_gone(uint8_t unit) { int d = ata_unit(unit); return d >= 2 && kd[d - 2] && kd[d - 2]->gone; }
const struct ubios *ata_ubios(uint8_t unit) { return ata_unit_all(unit) ? &ub[unit - 0x80] : NULL; }
uint64_t ata_unit_sectors(uint8_t unit) { int d = ata_unit(unit); return d >= 2 ? kd[d - 2]->sectors : d >= 0 ? adev[d].sectors : 0; }
/* v42: at DOS95's end - every disk's write cache to its medium, the AHCI ports the BIOS's again.  1: a disk's
 * controller could not be given back (NVMe: reset to be taken) - DOS cannot read that disk after DOS95, and
 * the PC is restarted instead (main.c kernel_shutdown) */
/* v42 (#480): 1 - a disk taken from its BIOS for good (NVMe: its controller reset to be taken): DOS could not
 * read it after DOS95 */
int ata_bios_lost(void)
{
    for (int k = 0; k < nkd; k++) if (kd[k] && !kd[k]->give_back && kd[k]->bios_lost) return 1;
    return 0;
}
int ata_shutdown(void)
{
    int lost = 0;
    for (int k = 0; k < nkd; k++) if (kd[k]) kd[k]->flush(kd[k]);
    for (int k = 0; k < nkd; k++) {
        if (!kd[k]) continue;
        if (kd[k]->give_back) kd[k]->give_back(kd[k]);
        else if (kd[k]->bios_lost) lost = 1;
    }
    return lost;
}
#ifdef QDP
/* the QEMU DOS port's setup: what a unit's disk is ("SATA (AHCI) port 0, Samsung SSD 860"), when the kernel drives it -
 * and a disk the setup erases whole: its GPT no more shown as an MBR, nor its sectors outside the partitions refused */
const char *ata_unit_desc(uint8_t unit, char *out, int n)
{
    int dev = ata_unit(unit);
    out[0] = 0;
    if (dev >= 2 && kd[dev - 2]) ksnprintf(out, n, "%s %s, %s", kd[dev - 2]->kind, kd[dev - 2]->where, kd[dev - 2]->model);
    else if (dev == 0 || dev == 1) ksnprintf(out, n, "IDE %s, %s", dev ? "slave" : "master", adev[dev].model);
    return out;
}
int ata_unit_is_gpt(uint8_t unit) { return unit >= 0x80 && unit <= 0x8F && unit_gpt0[unit - 0x80] != NULL; }
void ata_unit_erase_ok(uint8_t unit)
{
    if (unit < 0x80 || unit > 0x8F || !unit_gpt0[unit - 0x80]) return;
    unit_gpt0[unit - 0x80] = NULL;
    kprintf("disk: unit %02x: its GPT is to be erased - written as it is from now on\n", unit);
}
#endif
