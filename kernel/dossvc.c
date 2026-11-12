/* DOS95 kernel - DOS services executed by the system VM (VM0) on behalf of the GUI */
#include "kernel.h"
#include "vmmem.h"
#include "dosfs.h"
#include "fsnames.h"      /* fs_* here are the unlocked *_i bodies; kernel/fslock.c wraps them */
void fs_findclose(struct fs_find *h);   /* (its _i body, below: fs_findfirst uses it first) */

static struct svcreq *req;
static uint8_t *xfer;
static uint16_t xfer_segv, xfer_offv, xfer_sizev;
static volatile int req_pending, req_done;
static struct thread *waiter;
static struct thread *svc_lock_owner;
int lfn_available;      /* long names available to the DOS95 shell/UI (native or DOS 71xx) */
int lfn_dosapi;         /* the underlying DOS itself exposes the int 21h/71xx LFN API      */

uint8_t *xfer_buf(void) { return xfer; }
/* (v41, #402: the scheduler's - who has DOS now, and the thread that runs it) */
struct thread *dos_lock_holder(void) { return svc_lock_owner; }
struct thread *dos_vm_thread(void) { return vms[0] ? vms[0]->thr : NULL; }
uint16_t xfer_seg(void) { return xfer_segv; }
uint16_t xfer_off(void) { return xfer_offv; }
uint16_t xfer_size(void) { return xfer_sizev; }

void dossvc_init(void)
{
    req = (struct svcreq*)PHYSLOW(LIN(boot->psp_seg, boot->req_off));
    xfer = (uint8_t*)PHYSLOW(LIN(boot->psp_seg, boot->xfer_off));
    xfer_segv = boot->psp_seg; xfer_offv = boot->xfer_off; xfer_sizev = boot->xfer_size;
    lfn_dosapi = (boot->flags & BIF_LFN) ? 1 : 0;
    /* DOS95 provides long names itself (native VFAT) whenever C: is a readable
     * FAT volume, so the shell offers long names even on bare MS-DOS. */
    lfn_available = lfn_dosapi || vfat_usable();
}

/* VM0 hypercall: wait for a request */
void dossvc_vm0_wait(struct vm *vm, struct frame *f)
{
    extern int vpic_pending(struct vm *vm);
    extern uint16_t stub_addr(int idx);
    cli();
    while (!req_pending) {
        if (vpic_pending(vm)) {
            /* a hardware IRQ routed to the system session (e.g. the user's packet driver's
             * card): go back to the HLT so the interrupt is delivered; the ISR's IRET
             * lands on the HLT again and we resume waiting */
            f->eip = stub_addr(STUB_SVCLOOP);
            return;
        }
        vm->in_hcall_wait = 3;
        thread_block();
        vm->in_hcall_wait = 0;
    }
    req_pending = 0;
    vm_dos_gate(vm);            /* (v41, #461: the system session's DOS call waits while a box is inside INT 13h) */
}

void dossvc_vm0_done(struct vm *vm, struct frame *f)
{
    (void)f; (void)vm;
    cli();
    req_done = 1;
    if (waiter) thread_wake(waiter);
}

/* ---- what DOS (VM0) holds that is not on the disk, and what it holds that is stale (v37)
 * FreeDOS keeps disk sectors in its buffers.  The kernel reads the disk itself too
 * (vfat.c), so before such a read DOS had to write what it changed - int 21h AH=0Dh -
 * and that call also throws every buffer away, so DOS read everything again after it.
 * vfat.c did it before every listing: 800 times for one Apache listing.  Now the flush
 * is made only when DOS may have changed something since the last one (dos_dirty: every
 * call that is not known to only read, and the close of a handle that could write), and
 * DOS's buffers are thrown away when another VM wrote to a disk since (they would be
 * stale - a DOS box writes through a DOS of its own). */
static int dos_dirty = 1;
static uint8_t h_wr[256];                       /* VM0's DOS handles open with write access */
extern volatile uint32_t disk_write_gen_other;  /* ports.c: writes by any VM but VM0 */
static uint32_t other_seen;
static int svc_dirties(int intno, const struct dosregs *r)
{
    if (intno == 0x25) return 0;                /* absolute read */
    if (intno != 0x21) return 1;
    uint8_t ah = r->ax >> 8, al = r->ax & 0xFF;
    switch (ah) {
    case 0x0D: case 0x0E: case 0x19: case 0x1A: case 0x2A: case 0x2C: case 0x2F: case 0x30:
    case 0x36: case 0x3B: case 0x3D: case 0x3F: case 0x42: case 0x47: case 0x4E: case 0x4F:
    case 0x59: case 0x60:
        return 0;                               /* flush, drive, DTA, date, time, version, free space,
                                                   chdir, open, read, seek, getcwd, find, error, truename */
    case 0x43: case 0x57: return al != 0;      /* get attributes / get a file's time: reads */
    case 0x6C: return (r->dx & 0x12) != 0;     /* v41 (#421): extended open - only one that may create or truncate writes
                                                  (every open made DOS's buffers "dirty", and the next look at a folder of
                                                  the kernel's own flushed them: a DOS call more an open) */
    case 0x3E: return (r->bx & 0xFF00) || h_wr[r->bx & 0xFF];
    case 0x71:
        switch (al) {
        case 0x3B: case 0x47: case 0x4E: case 0x4F: case 0x60: case 0xA0: case 0xA1: return 0;
        case 0x43: return (r->bx & 0xFF) != 0;                          /* BL 0: get attributes */
        case 0x6C: return (r->dx & 0x12) != 0;                          /* truncate / create */
        }
        return 1;
    }
    return 1;
}
/* after a call that succeeded: which handles may write */
static void svc_track(int intno, const struct dosregs *in, const struct dosregs *out)
{
    if (intno != 0x21 || (out->flags & EFL_CF)) return;
    uint8_t ah = in->ax >> 8, al = in->ax & 0xFF;
    int h = out->ax & 0xFF;
    if ((out->ax & 0xFF00) && (ah == 0x3C || ah == 0x3D || ah == 0x45 || ah == 0x5A || ah == 0x5B || (ah == 0x71 && al == 0x6C))) return;
    if (ah == 0x3C || ah == 0x5A || ah == 0x5B) h_wr[h] = 1;
    else if (ah == 0x3D) h_wr[h] = (al & 3) != 0;
    else if (ah == 0x71 && al == 0x6C) h_wr[h] = (in->bx & 3) != 0 || (in->dx & 0x12) != 0;
    else if (ah == 0x45) h_wr[h] = h_wr[in->bx & 0xFF];
    else if (ah == 0x46) h_wr[in->cx & 0xFF] = h_wr[in->bx & 0xFF];
    else if (ah == 0x3E) h_wr[in->bx & 0xFF] = 0;
}
static void svc_issue(int intno, int mode, struct dosregs *r);
void dos_disk_flush(void);
/* before the kernel reads the disk itself: what DOS changed is on it */
void dos_disk_sync(void)
{
    if (dos_dirty || other_seen != disk_write_gen_other) dos_disk_flush();
}

static int dos_call_ex(int intno, int mode, struct dosregs *r)
{
    if (cur_thread->vm) kpanic("dos_call from VM context");
    /* serialise callers */
    uint32_t fl = save_flags_cli();
    while (svc_lock_owner && svc_lock_owner != cur_thread) { thread_sleep_ms(2); }
    svc_lock_owner = cur_thread;
    /* another VM wrote to a disk: what DOS holds may be stale - throw it away first */
    if (other_seen != disk_write_gen_other && !(intno == 0x21 && (r->ax >> 8) == 0x0D)) {
        uint32_t g = disk_write_gen_other;
        struct dosregs fr = {0}; fr.ax = 0x0D00;
        svc_issue(0x21, 0, &fr);
        other_seen = g; dos_dirty = 0;
    }
    uint32_t g_other = disk_write_gen_other;
    struct dosregs in = *r;
    if (svc_dirties(intno, r)) dos_dirty = 1;
    svc_issue(intno, mode, r);
    svc_track(intno, &in, r);
    if (intno == 0x21 && (in.ax >> 8) == 0x0D) { dos_dirty = 0; other_seen = g_other; }
    svc_lock_owner = NULL;
    restore_flags(fl);
    return (r->flags & EFL_CF) ? 1 : 0;
}
/* one request to VM0, the service lock held */
static void svc_issue(int intno, int mode, struct dosregs *r)
{
    req->ax = r->ax; req->bx = r->bx; req->cx = r->cx; req->dx = r->dx;
    req->si = r->si; req->di = r->di; req->ds = r->ds; req->es = r->es; req->bp = r->bp;
    req->intno = intno; req->mode = mode;
    req_done = 0;
    req_pending = 1;
    waiter = cur_thread;
    extern int gui_in_dos_call;
    if (cur_thread == gui_thread) gui_in_dos_call = 1;
    struct vm *v0 = vms[0];
    if (v0->thr->state == T_BLOCKED && v0->in_hcall_wait == 3) thread_wake(v0->thr);
    while (!req_done) thread_block();
    if (cur_thread == gui_thread) gui_in_dos_call = 0;
    waiter = NULL;
    r->ax = req->ax; r->bx = req->bx; r->cx = req->cx; r->dx = req->dx;
    r->si = req->si; r->di = req->di; r->ds = req->ds; r->es = req->es; r->bp = req->bp;
    r->flags = req->flags;
}

int dos_call(int intno, struct dosregs *r) { return dos_call_ex(intno, 0, r); }
/* v41: DOS's active code page (INT 21h AX=6601h: BX) - the one its 8.3 names are in
 * (kernel/vfat.c); 437 when DOS does not say */
int dos_codepage(void)
{
    struct dosregs r = {0};
    r.ax = 0x6601;
    if (dos_call(0x21, &r) || !r.bx) return 437;
    return r.bx;
}
/* v41 (#332): how many files DOS95 can have open.  Every file the kernel and its programs
 * open is a handle of one DOS process - DOS95.COM's, in the system session (kernel/fslock.c)
 * - and a DOS process starts with 20 handles, 5 of them DOS's own (CON, AUX, PRN): 15 files
 * open at once in the whole system, the 16th refused (DOS's "too many open files") - seven
 * threads of one program, each reading a file with the files it includes (libxml2's
 * runtest), were refused.  And every open file is an entry of DOS's file table, whose size
 * CONFIG.SYS says (FILES=, 40 on DOS95's own disk, 8 if it says nothing) - shared with every
 * DOS box.  So:
 *  - DOS's own Set Handle Count (INT 21h AH=67h, MS-DOS 3.3 and later) gives DOS95.COM a
 *    handle table of 255: a table entry is a byte (0FFh: none), and an odd count (the first
 *    MS-DOS 3.30 takes 64 KB for an even one);
 *  - DOS95 has a file table of its own, 255 entries, in the system session's memory (DOS's
 *    memory of DOS95.COM's, INT 21h AH=48h), made as DOS makes one (a table's header: the
 *    next table, FFFFh:FFFFh for none, and its count).  The List of Lists +04h - where DOS
 *    finds its first table - is part of each machine's DOS state, as the swappable data area
 *    is (kernel/vm.c vm_sda_save): in the system session it names DOS95's table, in each DOS
 *    box DOS's.  DOS95's programs have 250 files open at once whatever CONFIG.SYS says, and
 *    the DOS boxes keep all of FILES= to themselves.  The handles DOS95.COM had from DOS (its
 *    standard ones) keep their entries: copied to the same places in DOS95's table.
 * (Past 250: a file system of DOS95's own, not DOS's handles - as Windows 95's VFAT.)
 * Made once, before the first file is opened (gui_main); taken back before DOS95 ends
 * (dos_handles_fini, kernel_shutdown: dos_view_shared), and DOS frees the memory. */
extern uint32_t sft_shared_head;                /* DOS's file tables (vm.c) */
static uint32_t sft_own;                        /* DOS95's: seg:0000, 0 none */
static uint8_t jft_dos[20];                     /* DOS95.COM's handles as DOS gave them */
static int sft_esize(void) { return (boot->dos_ver & 0xFF) >= 4 ? 0x3B : 0x35; }   /* DOS 4+ / 3.1-3.3 */
static uint8_t *sft_entry(uint32_t head, int idx)
{
    uint16_t off = head & 0xFFFF, seg = head >> 16;
    for (int k = 0; k < 64 && off != 0xFFFF; k++) {
        uint8_t *t = PHYSLOW(LIN(seg, off));
        int n = *(uint16_t*)(t + 4);
        if (idx < n) return t + 6 + idx * sft_esize();
        idx -= n;
        off = *(uint16_t*)t; seg = *(uint16_t*)(t + 2);
    }
    return NULL;
}
static int sft_count(uint32_t head)             /* a chain's entries (DOS's: FILES=) */
{
    uint16_t off = head & 0xFFFF, seg = head >> 16;
    int n = 0;
    for (int k = 0; k < 64 && off != 0xFFFF; k++) {
        uint8_t *t = PHYSLOW(LIN(seg, off));
        n += *(uint16_t*)(t + 4);
        off = *(uint16_t*)t; seg = *(uint16_t*)(t + 2);
    }
    return n;
}
static uint8_t *dos95_jft(int *count)           /* DOS95.COM's handle table (its PSP +32h, +34h) */
{
    uint8_t *psp = PHYSLOW(LIN(boot->psp_seg, 0));
    uint32_t p = *(uint32_t*)(psp + 0x34);
    if (count) *count = *(uint16_t*)(psp + 0x32);
    return PHYSLOW(LIN(p >> 16, p & 0xFFFF));
}
void dos_handles_init(void)
{
    int n0, files = sft_count(sft_shared_head);
    uint8_t *jft = dos95_jft(&n0);
    for (int j = 0; j < 20; j++) jft_dos[j] = j < n0 ? jft[j] : 0xFF;
    struct dosregs r = {0};
    r.ax = 0x6700; r.bx = 255;
    if (dos_call(0x21, &r)) { kprintf("dos: Set Handle Count refused (error %u): 15 files open at once\n", r.ax); return; }
    uint32_t bytes = 6 + 255 * (uint32_t)sft_esize();
    memset(&r, 0, sizeof r);
    r.ax = 0x4800; r.bx = (uint16_t)((bytes + 15) >> 4);
    if (dos_call(0x21, &r)) {
        kprintf("dos: no memory for DOS95's file table (error %u): DOS's, %d (FILES=), shared with the DOS boxes\n", r.ax, files);
        return;
    }
    uint16_t seg = r.ax;
    uint8_t *t = PHYSLOW(LIN(seg, 0));
    memset(t, 0, bytes);                        /* every entry unused (no references) */
    *(uint16_t*)(t + 0) = 0xFFFF; *(uint16_t*)(t + 2) = 0xFFFF; *(uint16_t*)(t + 4) = 255;
    jft = dos95_jft(&n0);                       /* (the new one) */
    for (int j = 0; j < n0; j++) {
        uint8_t *e;
        if (jft[j] != 0xFF && (e = sft_entry(sft_shared_head, jft[j]))) memcpy(t + 6 + jft[j] * sft_esize(), e, sft_esize());
    }
    uint32_t fl = save_flags_cli();
    sft_own = (uint32_t)seg << 16;
    vms[0]->sft_head = sft_own;
    if (vm_current == vms[0]) *(uint32_t*)PHYSLOW(boot->lol_lin + 4) = sft_own;
    restore_flags(fl);
    kprintf("dos: DOS95's programs have 250 files: a handle table of 255 and a file table of their own at %04x (DOS's, %d: FILES=, the DOS boxes')\n", seg, files);
}
/* Before DOS95 ends: the system session back on DOS's tables, with the handles DOS gave
 * DOS95.COM and no other - any left open (a program's, the kernel's) would name entries of
 * DOS95's table, which DOS's are not.  Interrupts off, the system session's DOS state live
 * (kernel_shutdown); no DOS call (dos_handles_fini closes them first, with DOS). */
void dos_view_shared(void)
{
    if (!sft_own) return;
    int n;
    uint8_t *jft = dos95_jft(&n);
    for (int j = 0; j < n; j++) jft[j] = j < 20 ? jft_dos[j] : 0xFF;
    vms[0]->sft_head = sft_shared_head;
    if (vm_current == vms[0]) *(uint32_t*)PHYSLOW(boot->lol_lin + 4) = sft_shared_head;
    sft_own = 0;
}
/* DOS95 shutting down (gui_main, after every program has ended): the files still open closed
 * by DOS - written ones get their directory entries - then DOS's tables back */
void dos_handles_fini(void)
{
    if (!sft_own) return;
    int n;
    uint8_t *jft = dos95_jft(&n);
    for (int j = 0; j < n; j++) {
        if (jft[j] == 0xFF || (j < 20 && jft[j] == jft_dos[j])) continue;
        struct dosregs r = {0};
        r.ax = 0x3E00; r.bx = (uint16_t)j;
        dos_call(0x21, &r);
    }
    uint32_t fl = save_flags_cli();
    dos_view_shared();
    restore_flags(fl);
}
/* the thread with a DOS call in progress (kernel/sched.c: sched_stop_app) */
struct thread *svc_lock_holder(void) { return svc_lock_owner; }
int dos_call_orig(int intno, struct dosregs *r) { return dos_call_ex(intno, 1, r); }

/* ------------------------------------------------------------------ */
/* file system API                                                      */
#define XP_PATH   0        /* 260 bytes: path argument (v41, #336: MAX_PATH - they were 256) */
#define XP_PATH2  260      /* 260 bytes: second path                  */
#define XP_FIND   520      /* find data (LFN 318 bytes / DTA 43)      */
#define XP_DATA   1024     /* bulk data                               */
#define XP_DATA_MAX (16384 - 1024)

/* a path for DOS: whole, or "|" - which DOS refuses - when it does not fit (v41, #336: it was cut
 * at 255 bytes, and DOS acted on the shorter path) */
static void put_path(int off, const char *p) { strlcpy((char*)xfer + off, strlen(p) < 260 ? p : "|", 260); }

/* Absolute sector read (int 25h) via VM0. mode 2 = the loader loop cleans the
 * flags-on-stack quirk. drive 0=A,1=B,2=C...; lsn logical sector; count -> buf. */
/* int 25h/26h: DOS 4+ "big partition" form -- CX=FFFFh and DS:BX -> a packet
 * {dword sector, word count, dword far buffer}.  Mandatory for volumes with more
 * than 65535 sectors (> 32 MB); the word-sector form silently wraps there.  The
 * packet lives in the transfer area just below the data. */
#define XP_DPKT   (XP_DATA - 16)
static void abs_packet(uint32_t lsn, int count)
{
    uint8_t *pk = xfer + XP_DPKT;
    *(uint32_t*)(pk + 0) = lsn;
    *(uint16_t*)(pk + 4) = (uint16_t)count;
    *(uint16_t*)(pk + 6) = xfer_offv + XP_DATA;
    *(uint16_t*)(pk + 8) = xfer_segv;
}
/* v41: MS-DOS 7.1's extended absolute read/write, INT 21h AX=7305h - the only one it takes
 * for a FAT32 drive (its INT 25h/26h refuse one): CX=FFFFh, DL the drive (1 = A:), DS:BX the
 * same packet, SI bit 0 write.  0 done, else CF (DOS before 7.1 has no such call). */
static int ext_abs_io(int drive, int write)
{
    struct dosregs r = {0};
    r.ax = 0x7305; r.cx = 0xFFFF; r.dx = (uint16_t)(drive + 1);
    r.si = write ? 1 : 0;
    r.ds = xfer_segv; r.bx = xfer_offv + XP_DPKT;
    return dos_call(0x21, &r);
}
/* ---- the sector cache under the kernel's own reads of the disks (v37) ----------------
 * vfat.c reads the directories and the FAT itself, a sector at a time, through DOS's
 * int 25h - and it did that for every name it looked up: each stat, open or listing
 * walked every folder on the way down from the disk again.  Measured: a listing of
 * Apache's /icons/ (236 files) made 7 000 int 25h calls, each a DOS call and a BIOS
 * disk read, and took 5 seconds; opening the Start menu 1 800 of them, 2.5 seconds;
 * the fonts wxWidgets opens as a program starts, tens of seconds.  The sectors read are
 * kept here, and stay good while nothing is written to any disk: every write, whoever
 * makes it (DOS for the kernel or a program, a DOS box, a driver of its own), gives an
 * ATA controller a command, and ports.c counts each one that may write
 * (disk_write_gen).  A read that a write overtook is not kept. */
volatile uint32_t disk_write_gen;
/* v41 (#336): as big as the machine spares - a sixteenth of the memory free when it is first
 * used, in a power of two of sets (it stopped at 2 MB) - and emptied at a write by its
 * generation, not by clearing every slot (the clearing grew with the cache) */
#define SC_WAYS 4
struct scslot { uint64_t key; uint32_t gen; uint8_t age; };   /* key ((drive + 1) << 32) | sector */
static struct scslot *sc_slot;                  /* sc_sets * SC_WAYS */
static uint8_t *sc_data;                        /* sc_sets * SC_WAYS * 512 bytes */
static uint32_t sc_sets;                        /* sets in use */
static int sc_state;                            /* 0 not yet, 1 in use, -1 no memory for it */
static uint32_t sc_set(int drive, uint32_t lsn) { return (lsn + (uint32_t)drive * 7919u) & (sc_sets - 1); }
static int sc_ready(void)
{
    if (sc_state) return sc_state > 0;
    uint32_t freep = mm_free_pages(), sets = 32;
    while ((uint64_t)sets * 2 * SC_WAYS * 512 / 4096 <= freep / 16 && sets < 0x10000000u) sets <<= 1;
    uint32_t pages, p; struct scslot *sl;
    for (;; sets >>= 1) {                       /* (a smaller one when that much is not in one piece) */
        pages = sets * SC_WAYS * 512 / 4096;
        p = freep > pages * 4 ? page_alloc((int)pages) : 0;
        sl = p ? kzalloc(sets * SC_WAYS * sizeof *sl) : NULL;
        if (p && sl) break;
        if (p) page_free(p, (int)pages);
        if (sets <= 32) { sc_state = -1; return 0; }
    }
    sc_data = (uint8_t*)p; sc_slot = sl; sc_sets = sets; sc_state = 1;
    kprintf("disk: sector cache %u KB\n", (unsigned)(sets * SC_WAYS / 2));
    return 1;
}
static int sc_get(int drive, uint32_t lsn, void *buf)
{
    uint64_t k = ((uint64_t)(drive + 1) << 32) | lsn;
    uint32_t f = save_flags_cli();
    uint32_t s = sc_set(drive, lsn), gen = disk_write_gen;
    struct scslot *set = sc_slot + s * SC_WAYS;
    for (int w = 0; w < SC_WAYS; w++) if (set[w].key == k && set[w].gen == gen) {
        memcpy(buf, sc_data + (s * SC_WAYS + w) * 512, 512);
        for (int o = 0; o < SC_WAYS; o++) if (set[o].age < 255) set[o].age++;
        set[w].age = 0;
        restore_flags(f);
        return 1;
    }
    restore_flags(f);
    return 0;
}
static void sc_put(int drive, uint32_t lsn, const void *buf, uint32_t gen)
{
    uint64_t k = ((uint64_t)(drive + 1) << 32) | lsn;
    uint32_t f = save_flags_cli();
    if (disk_write_gen != gen) { restore_flags(f); return; }    /* a write came in the meantime */
    uint32_t s = sc_set(drive, lsn);
    struct scslot *set = sc_slot + s * SC_WAYS;
    int v = 0;
    for (int w = 0; w < SC_WAYS; w++) {
        if ((set[w].key == k && set[w].gen == gen) || set[w].gen != gen || !set[w].key) { v = w; break; }
        if (set[w].age > set[v].age) v = w;
    }
    set[v].key = k; set[v].gen = gen; set[v].age = 0;
    memcpy(sc_data + (s * SC_WAYS + v) * 512, buf, 512);
    restore_flags(f);
}

/* ---- v41 (#392): a hard disk's sectors without a trip to DOS ---------------------------------
 * The cache above is emptied by any write to any disk, and the long-name layer then read every
 * folder on the way to a file again - each sector an INT 25h, a full trip into DOS and back,
 * though the cache under INT 13h (dcache.c) had it and would have answered DOS without the BIOS:
 * a warm start of Notepad under Wine was 746 calls to DOS, 291 of them these.  So a read asks
 * that cache first.  Which of the disk's sectors a drive's sector is, is learnt from DOS itself:
 * the one read its disk driver made for an INT 25h of this drive (the BIOS unit, and the
 * partition's start as the difference) - checked against what DOS returned, and against every
 * later INT 25h of the drive (one that disagrees: the drive is not asked for here again). */
static struct { uint8_t state, unit; uint64_t start; } dmap[32];   /* state 0 not known, 1 known, 2 never */
extern uint32_t dcache_sys_seq(void);
extern int dcache_sys_last(uint32_t since, int count, uint8_t *unit, uint64_t *lba);
extern int dcache_peek(uint8_t unit, uint64_t lba, int count, void *buf);
extern int dcache_read_direct(uint8_t unit, uint64_t lba, int count, void *buf);
/* v41 (#412): where a drive is on its disk, as learnt (vfat.c keeps folders by it): 1, or 0 not known */
int dos_drive_where(int drive, uint8_t *unit, uint64_t *base)
{
    if (drive < 2 || drive >= 32 || dmap[drive].state != 1) return 0;
    *unit = dmap[drive].unit; *base = dmap[drive].start;
    return 1;
}
static int dmap_read(int drive, uint32_t lsn, int count, void *buf)
{
    if (drive < 2 || drive >= 32 || dmap[drive].state != 1) return 0;
    if (dcache_peek(dmap[drive].unit, dmap[drive].start + lsn, count, buf)) return 1;
    /* v41 (#412): not there - read by the kernel itself (ata.c), no trip to DOS and the BIOS */
    return dcache_read_direct(dmap[drive].unit, dmap[drive].start + lsn, count, buf);
}
/* an INT 25h of count sectors from lsn has just returned `data`; seq0: dcache_sys_seq() before it */
static void dmap_learn(int drive, uint32_t lsn, int count, uint32_t seq0, const uint8_t *data)
{
    uint8_t unit, tmp[512]; uint64_t lba;
    if (drive < 2 || drive >= 32 || dmap[drive].state == 2) return;
    if (!dcache_sys_last(seq0, count, &unit, &lba) || lba < lsn) return;
    if (!dcache_peek(unit, lba, 1, tmp) || memcmp(tmp, data, 512)) return;   /* (not what DOS gave: not that read) */
    if (dmap[drive].state == 1) {
        if (dmap[drive].unit == unit && dmap[drive].start == lba - lsn) return;
        dmap[drive].state = 2;
        kprintf("disk: %c: is not where it was on its disk (unit %02x sector %u, now unit %02x sector %u) - its sectors are read through DOS\n",
                'A' + drive, dmap[drive].unit, (uint32_t)dmap[drive].start, unit, (uint32_t)(lba - lsn));
        return;
    }
    dmap[drive].unit = unit; dmap[drive].start = lba - lsn; dmap[drive].state = 1;
    kprintf("disk: %c: is BIOS unit %02x from sector %u - its sectors read from the cache under INT 13h when it has them\n",
            'A' + drive, unit, (uint32_t)(lba - lsn));
}

static int phantom_drive(int d);
/* v41 (#336): any count of sectors - read and written through DOS's transfer area (15 KB) a part
 * at a time; until v41 a count over 30 ran past it (no caller asked for one yet) */
#define XP_SECTORS (XP_DATA_MAX / 512)
static int dos_absread_part(int drive, uint32_t lsn, int count, void *buf);
static int dos_abswrite_part(int drive, uint32_t lsn, int count, const void *buf);
int dos_absread(int drive, uint32_t lsn, int count, void *buf)
{
    for (int i = 0; i < count; i += XP_SECTORS) {
        int k = count - i < XP_SECTORS ? count - i : XP_SECTORS;
        int r = dos_absread_part(drive, lsn + (uint32_t)i, k, (uint8_t *)buf + (size_t)i * 512);
        if (r) return r;
    }
    return 0;
}
/* v41 (#336, M8): MS-DOS 7.x halts the machine at an absolute write to a hard disk whose volume is
 * not locked - its INT 26h: the drive's lock flag clear (DOS's data 1308h + the drive, bit 7), then
 * mode 3, its message, the disks flushed, HLT for ever (the system session did that under DOS95:
 * DOS95's long-name writer's first directory write - Apache's programs' first folder - and the
 * system VM's video cleared over the IVT, a kernel panic).  Windows 95's rule for direct disk
 * access, kept by its real-mode DOS: a disk utility locks the volume (INT 21h AX=440Dh, CX=084Ah,
 * BL the drive, BH level 0) for its writes and unlocks it after (CX=086Ah).  DOS 7's real mode only
 * sets and clears that flag; FreeDOS takes the calls too.  Floppies need none (DOS exempts A: B:) */
static void vol_lock(int drive, int on)
{
    extern int vol_box_locks[32];
    if (drive < 2 || drive >= 32 || vol_box_locks[drive] > 0) return;   /* (a box holds it: locked already, and left so) */
    struct dosregs r = {0};
    r.ax = 0x440D; r.bx = (uint16_t)(drive + 1);         /* BH 0: level 0 */
    r.cx = on ? 0x084A : 0x086A; r.dx = 0;
    dos_call(0x21, &r);                                  /* (a DOS without the call: its error, nothing to lock) */
}
int dos_abswrite(int drive, uint32_t lsn, int count, const void *buf)
{
    int r = 0;
    vol_lock(drive, 1);
    for (int i = 0; i < count; i += XP_SECTORS) {
        int k = count - i < XP_SECTORS ? count - i : XP_SECTORS;
        r = dos_abswrite_part(drive, lsn + (uint32_t)i, k, (const uint8_t *)buf + (size_t)i * 512);
        if (r) break;
    }
    vol_lock(drive, 0);
    return r;
}
static int dos_absread_part(int drive, uint32_t lsn, int count, void *buf)
{
    if (phantom_drive(drive)) return -1;                 /* v40 (#171): no B: on one floppy drive */
    { extern void ata_setup(void); ata_setup(); }        /* v41 (#412): (once) which disks the kernel reads itself */
    int cached = sc_ready();
    if (cached) {
        int i = 0;
        while (i < count && sc_get(drive, lsn + (uint32_t)i, (uint8_t*)buf + i * 512)) i++;
        if (i == count) return 0;
    }
    uint32_t gen = disk_write_gen;
    if (dmap_read(drive, lsn, count, buf)) {             /* v41 (#392): the cache under INT 13h has them */
        if (cached) for (int i = 0; i < count; i++) sc_put(drive, lsn + (uint32_t)i, (uint8_t*)buf + i * 512, gen);
        return 0;
    }
    uint32_t seq0 = dcache_sys_seq();
    struct dosregs r = {0};
    abs_packet(lsn, count);
    r.ax = drive & 0xFF; r.cx = 0xFFFF; r.dx = 0;
    r.ds = xfer_segv; r.bx = xfer_offv + XP_DPKT;
    int bad = dos_call_ex(0x25, 2, &r);
    if (bad) { seq0 = dcache_sys_seq(); bad = ext_abs_io(drive, 0); }
    if (bad) {
        if (lsn > 0xFFFF) return -1;
        /* DOS 3.x has no packet form: fall back to the word-sector call */
        struct dosregs r2 = {0};
        r2.ax = drive & 0xFF; r2.cx = count; r2.dx = (uint16_t)lsn;
        r2.ds = xfer_segv; r2.bx = xfer_offv + XP_DATA;
        seq0 = dcache_sys_seq();
        if (dos_call_ex(0x25, 2, &r2)) return -1;
    }
    memcpy(buf, xfer + XP_DATA, count * 512);
    dmap_learn(drive, lsn, count, seq0, buf);
    if (cached) for (int i = 0; i < count; i++) sc_put(drive, lsn + (uint32_t)i, (uint8_t*)buf + i * 512, gen);
    return 0;
}

uint32_t fs_drives(void);
int fs_drive_type(int drive);
/* ---- v40 (#185): a disk or a volume as a program's device file - read, never written -----
 * A Windows program opens \\.\C: (a volume) or \\.\PhysicalDrive0 (a disk) as a file and
 * reads it sector by sector: 7-Zip's File Manager lists them under \\.\, copies a drive to
 * an image and opens one as an archive.  dev: 0..25 a DOS drive (A: = 0), read with DOS's
 * INT 25h; 0x80.. a BIOS hard disk (PhysicalDrive0 = 0x80), with INT 13h's extended read.
 * Whatever DOS changed is written first (dos_disk_sync), so what is read is the disk now. */
/* out[8]: bytes per sector; sectors (low, high); cylinders, heads, sectors per track;
 * 1 removable (a floppy) / 0 fixed; hidden sectors (where a volume starts on its disk) */
int disk_info_i(int dev, uint32_t *out)
{
    for (int i = 0; i < 8; i++) out[i] = 0;
    if (dev >= 0 && dev < 26) {
        if (phantom_drive(dev)) return -15;
        if (!(fs_drives() & (1u << dev))) return -15;
        if (fs_drive_type(dev) == 3) return -19;             /* a network drive or a CD: not DOS's sectors */
        uint8_t bs[512];
        dos_disk_sync();
        if (dos_absread(dev, 0, 1, bs)) return -21;         /* not ready: no disk in it */
        uint16_t bps = *(uint16_t*)(bs + 11), ts16 = *(uint16_t*)(bs + 19);
        if (bps != 512) return -21;                          /* (no BPB: not a FAT volume) */
        uint32_t tot = ts16 ? ts16 : *(uint32_t*)(bs + 32);
        out[0] = bps; out[1] = tot;
        out[4] = *(uint16_t*)(bs + 26); out[5] = *(uint16_t*)(bs + 24);
        out[3] = (out[4] && out[5]) ? tot / (out[4] * out[5]) : 0;
        out[6] = fs_drive_type(dev) == 1;
        out[7] = *(uint32_t*)(bs + 28);
        return 0;
    }
    if (dev >= 0x80 && dev <= 0xFF) {             /* (v41, #336: every BIOS disk unit - it was 80h-87h) */
        struct dosregs r = {0};
        uint8_t *p = xfer + XP_DATA;
        memset(p, 0, 0x1E); *(uint16_t*)p = 0x1E;
        r.ax = 0x4800; r.dx = (uint16_t)dev; r.ds = xfer_segv; r.si = xfer_offv + XP_DATA;
        dos_disk_sync();
        int cf = dos_call(0x13, &r);
        if (!cf) {
            out[3] = *(uint32_t*)(p + 4); out[4] = *(uint32_t*)(p + 8); out[5] = *(uint32_t*)(p + 12);
            out[1] = *(uint32_t*)(p + 16); out[2] = *(uint32_t*)(p + 20); out[0] = *(uint16_t*)(p + 24);
        }
        if (cf || !out[1]) {                                  /* no extensions: the old geometry */
            memset(&r, 0, sizeof r); r.ax = 0x0800; r.dx = (uint16_t)dev;
            if (dos_call(0x13, &r) || !(r.cx & 0x3F)) return -19;
            out[4] = (r.dx >> 8) + 1; out[5] = r.cx & 0x3F; out[3] = ((r.cx >> 8) | ((r.cx & 0xC0) << 2)) + 1;
            out[1] = out[3] * out[4] * out[5]; out[2] = 0;
        }
        if (!out[0]) out[0] = 512;
        return 0;
    }
    return -19;                                                /* ENODEV */
}
/* v41 (#412, ata.c): a hard disk unit's EDD - INT 13h AH=41h, then AH=48h with room for EDD 3.0's
 * 42h bytes: how much of it the BIOS filled (out: n bytes of it), or -1 (no extensions) */
int bios_edd(int unit, uint8_t *out, int n)
{
    struct dosregs r = {0};
    r.ax = 0x4100; r.bx = 0x55AA; r.dx = (uint16_t)unit;
    if (dos_call(0x13, &r) || r.bx != 0xAA55 || !(r.cx & 1)) return -1;
    uint8_t *p = xfer + XP_DATA;
    memset(p, 0, 0x42); *(uint16_t*)p = 0x42;
    memset(&r, 0, sizeof r);
    r.ax = 0x4800; r.dx = (uint16_t)unit; r.ds = xfer_segv; r.si = xfer_offv + XP_DATA;
    if (dos_call(0x13, &r)) return -1;
    int got = *(uint16_t*)p;
    memcpy(out, p, n < 0x42 ? n : 0x42);
    return got;
}
/* ... and one sector of it as its BIOS reads it - the cache under INT 13h not asked, nothing kept */
int bios_read_raw(int unit, uint64_t lba, uint8_t *buf)
{
    extern int dcache_bios_only;
    uint8_t *pk = xfer + XP_DPKT;
    pk[0] = 16; pk[1] = 0; *(uint16_t*)(pk + 2) = 1;
    *(uint16_t*)(pk + 4) = xfer_offv + XP_DATA; *(uint16_t*)(pk + 6) = xfer_segv;
    *(uint64_t*)(pk + 8) = lba;
    struct dosregs r = {0};
    r.ax = 0x4200; r.dx = (uint16_t)unit; r.ds = xfer_segv; r.si = xfer_offv + XP_DPKT;
    dcache_bios_only = 1;
    int bad = dos_call(0x13, &r);
    dcache_bios_only = 0;
    if (bad) return -1;
    memcpy(buf, xfer + XP_DATA, 512);
    return 0;
}
/* v42 (#469): an INT 13h call that takes and gives only registers, made to the BIOS itself (the cache under
 * INT 13h not asked) - ata.c's look at a unit before the kernel takes its disk: CF */
int bios_int13(struct dosregs *r)
{
    extern int dcache_bios_only;
    dcache_bios_only = 1;
    int bad = dos_call(0x13, r);
    dcache_bios_only = 0;
    return bad;
}
/* n sectors (up to 28 a call: the transfer buffer) from sector lba of dev into buf; the
 * count read, or < 0 */
int disk_read_i(int dev, uint32_t lba_lo, uint32_t lba_hi, int n, void *buf)
{
    if (n <= 0) return 0;
    if (n > 28) n = 28;
    if (dev >= 0 && dev < 26) {
        if (phantom_drive(dev)) return -15;
        if (lba_hi) return -22;
        dos_disk_sync();
        if (dos_absread(dev, lba_lo, n, buf)) return -5;
        return n;
    }
    if (dev >= 0x80 && dev <= 0xFF) {             /* (v41, #336: every BIOS disk unit - it was 80h-87h) */
        uint8_t *pk = xfer + XP_DPKT;
        pk[0] = 16; pk[1] = 0; *(uint16_t*)(pk + 2) = (uint16_t)n;
        *(uint16_t*)(pk + 4) = xfer_offv + XP_DATA; *(uint16_t*)(pk + 6) = xfer_segv;
        *(uint32_t*)(pk + 8) = lba_lo; *(uint32_t*)(pk + 12) = lba_hi;
        struct dosregs r = {0};
        r.ax = 0x4200; r.dx = (uint16_t)dev; r.ds = xfer_segv; r.si = xfer_offv + XP_DPKT;
        dos_disk_sync();
        if (dos_call(0x13, &r)) {
            /* no extensions: CHS, for a disk small enough to have an address that way */
            uint32_t g[8];
            if (lba_hi || disk_info_i(dev, g) < 0 || !g[4] || !g[5]) return -5;
            uint32_t c = lba_lo / (g[4] * g[5]), h = (lba_lo / g[5]) % g[4], sct = lba_lo % g[5] + 1;
            if (c > 1023) return -5;
            if (n > (int)(g[5] - sct + 1)) n = (int)(g[5] - sct + 1);
            memset(&r, 0, sizeof r);
            r.ax = 0x0200 | (uint16_t)n; r.cx = (uint16_t)(((c & 0xFF) << 8) | ((c >> 2) & 0xC0) | sct);
            r.dx = (uint16_t)((h << 8) | (uint32_t)dev); r.es = xfer_segv; r.bx = xfer_offv + XP_DATA;
            if (dos_call(0x13, &r)) return -5;
        }
        memcpy(buf, xfer + XP_DATA, (size_t)n * 512);
        return n;
    }
    return -19;
}

/* Absolute sector write (int 26h) via VM0. Same mode-2 flags-on-stack cleanup. */
static int dos_abswrite_part(int drive, uint32_t lsn, int count, const void *buf)
{
    if (phantom_drive(drive)) return -1;
    struct dosregs r = {0};
    memcpy(xfer + XP_DATA, buf, count * 512);
    abs_packet(lsn, count);
    r.ax = drive & 0xFF; r.cx = 0xFFFF; r.dx = 0;
    r.ds = xfer_segv; r.bx = xfer_offv + XP_DPKT;
    if (dos_call_ex(0x26, 2, &r) && ext_abs_io(drive, 1)) {
        if (lsn > 0xFFFF) return -1;
        struct dosregs r2 = {0};
        r2.ax = drive & 0xFF; r2.cx = count; r2.dx = (uint16_t)lsn;
        r2.ds = xfer_segv; r2.bx = xfer_offv + XP_DATA;
        if (dos_call_ex(0x26, 2, &r2)) return -1;
    }
    return 0;
}

static int dos_err(struct dosregs *r, int cf) { return cf ? -(int)(r->ax ? r->ax : 1) : 0; }

/* ---- v40 (#171): B: on a machine with one floppy drive ------------------------------------
 * DOS makes B: a second name of that drive, and the first touch of B: (a program asking
 * what B: is - a Windows program's GetDriveType loop, a game looking for its CD) made DOS
 * say "Insert diskette for drive B: and press any key" in the system session, where nobody
 * sees it and no key comes: the call never returned, and every program's file work waited
 * behind it.  Here there is no B: - as Windows 95's Explorer shows none on such a machine -
 * so nothing asks DOS about it: "invalid drive" (15), at once, and A: stays A:. */
static int one_floppy(void)
{
    uint16_t equip = *(uint16_t*)PHYSLOW(BDA_EQUIP);
    return (equip & 1) && ((equip >> 6) & 3) == 0;
}
static int phantom_drive(int d)                         /* d: 0 = A: */
{
    return d == 1 && one_floppy();
}
int fs_getdrive(void);
static int phantom_path(const char *p)
{
    if (!p || !p[0] || p[1] != ':') return phantom_drive(fs_getdrive());
    char c = p[0]; if (c >= 'a' && c <= 'z') c -= 32;
    return phantom_drive(c - 'A');
}
/* v40: and a path longer than MAX_PATH (v41, #336: 259 bytes - it was 255) is refused ("path not
 * found"), never cut short - a cut path names another file (a long UTF-8 name reaches it sooner) */
#define NO_PHANTOM(p) do { if (phantom_path(p)) return -15; if (strlen(p) > 259) return -3; } while (0)

/* Flush DOS's disk buffers to disk (int 21h AH=0Dh). Needed for coherency with
 * our raw int 25h/26h directory writes, which bypass the DOS buffer cache. */
void dos_disk_flush(void)
{
    struct dosregs r = {0};
    r.ax = 0x0D00;
    dos_call(0x21, &r);
}

/* DOS date/time -> components */
void fs_decode_datetime(uint16_t date, uint16_t time, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    if (y) *y = (date >> 9) + 1980; if (mo) *mo = (date >> 5) & 15; if (d) *d = date & 31;
    if (h) *h = time >> 11; if (mi) *mi = (time >> 5) & 63; if (s) *s = (time & 31) * 2;
}

static void fill_from_dta(struct fs_dirent *e)
{
    uint8_t *d = xfer + XP_FIND;
    e->attr = d[0x15];
    e->time = *(uint16_t*)(d + 0x16);
    e->date = *(uint16_t*)(d + 0x18);
    e->size = *(uint32_t*)(d + 0x1A);
    strlcpy(e->shortname, (char*)d + 0x1E, sizeof e->shortname);
    strlcpy(e->name, e->shortname, sizeof e->name);
}

static void fill_from_lfn(struct fs_dirent *e)
{
    uint8_t *d = xfer + XP_FIND;
    e->attr = *(uint32_t*)(d + 0) & 0xFF;
    /* DOS-format date/time requested (CL=1) : write time at +0x14, +0x16 */
    e->time = *(uint16_t*)(d + 0x14);
    e->date = *(uint16_t*)(d + 0x16);
    e->size = *(uint32_t*)(d + 0x20);
    strlcpy(e->name, (char*)d + 0x2C, sizeof e->name);
    strlcpy(e->shortname, (char*)d + 0x130, sizeof e->shortname);
    if (!e->shortname[0]) strlcpy(e->shortname, e->name, sizeof e->shortname);
}

int fs_findfirst(const char *pattern, uint8_t attr, struct fs_dirent *e, struct fs_find *h)
{
    if (phantom_path(pattern) || strlen(pattern) > 259) { h->lfn = 2; h->nat = 0; h->nat_n = h->nat_i = 0; return phantom_path(pattern) ? -15 : -3; }
    /* Native VFAT long-name listing: DOS95 reads the directory itself, so long
     * names work on any DOS with no 71xx provider underneath. */
    char dir[260]; strlcpy(dir, pattern, sizeof dir);
    char *base = dir; for (char *q = dir; *q; q++) if (*q == '\\' || *q == '/') base = q + 1;
    char pat[260]; strlcpy(pat, base, sizeof pat);     /* filename pattern (may be *.* ); v40: a whole
                                                          long name (it was cut at 63 bytes) */
    *base = 0;                                          /* dir keeps its trailing slash  */
    struct fs_dirent *arr;
    int n = vfat_list(dir, pat, &arr);
    if (n >= 0) {
        h->lfn = 2; h->nat = arr; h->nat_n = n; h->nat_i = 0;
        if (n == 0) { fs_findclose(h); return -18; }   /* no matching files */
        *e = arr[h->nat_i++];
        return 0;
    }
    /* fallback: plain DOS 8.3 for drives we can't read raw (network / CD) - v41 (#316): not for
     * a folder with a long name DOS95's FAT reader did not find, which DOS would cut to 8.3 and
     * list another folder by: that folder is not there */
    if (!lfn_dosapi && !vfat_dos_safe(dir)) { h->lfn = 2; h->nat = 0; h->nat_n = h->nat_i = 0; return -3; }
    struct dosregs r = {0};
    h->lfn = 0;
    put_path(XP_PATH, pattern);
    r.ax = 0x1A00; r.ds = xfer_segv; r.dx = xfer_offv + XP_FIND;
    dos_call(0x21, &r);
    memset(&r, 0, sizeof r);
    r.ax = 0x4E00; r.cx = attr | 0x10; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    memcpy(h->dta, xfer + XP_FIND, 43);
    fill_from_dta(e);
    return 0;
}

int fs_findnext(struct fs_find *h, struct fs_dirent *e)
{
    if (h->lfn == 2) {                         /* native VFAT listing */
        if (h->nat_i >= h->nat_n) return -18;
        *e = h->nat[h->nat_i++];
        return 0;
    }
    struct dosregs r = {0};
    memcpy(xfer + XP_FIND, h->dta, 43);
    r.ax = 0x1A00; r.ds = xfer_segv; r.dx = xfer_offv + XP_FIND;
    dos_call(0x21, &r);
    memset(&r, 0, sizeof r);
    r.ax = 0x4F00;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    memcpy(h->dta, xfer + XP_FIND, 43);
    fill_from_dta(e);
    return 0;
}

void fs_findclose(struct fs_find *h)
{
    if (h->lfn == 2) { if (h->nat) { kfree(h->nat); h->nat = 0; } return; }
    if (h->lfn && h->handle) {
        struct dosregs r = {0};
        r.ax = 0x71A1; r.bx = h->handle;
        dos_call(0x21, &r);
        h->handle = 0;
    }
}

/* Put `path` into an xfer slot, first translating any long names to 8.3 so the
 * underlying DOS can act on it. Falls back to the raw path (non-FAT volume).
 * v41 (#316): -1, and nothing put, for a long name DOS95's FAT reader does not find - the raw
 * path would go to DOS, which cuts a long name to 8.3 and finds another file by it; the name
 * is not there (DOS's answer 2, file not found; 3 when a folder on the way is not). */
static int put_path_short(int slot, const char *path)
{
    char sp[260];
    if (vfat_shortpath(path, sp, sizeof sp) == 0) { put_path(slot, sp); return 0; }
    if (!lfn_dosapi && !vfat_dos_safe(path)) return -1;
    put_path(slot, path);
    return 0;
}
/* the DOS error for a name put_path_short refused: 2 when its folder is there, else 3 */
static int missing_err(const char *path)
{
    char parent[260], sp[260];
    strlcpy(parent, path, sizeof parent);
    char *base = parent;
    for (char *q = parent; *q; q++) if (*q == '\\' || *q == '/') base = q;
    if (base == parent) return -2;
    *base = 0;
    if (!parent[0] || (parent[1] == ':' && !parent[2])) return -2;        /* a root's file */
    return vfat_shortpath(parent, sp, sizeof sp) == 0 ? -2 : -3;
}

/* v41 (#336, F24): a file past 2 GB - 1 is asked for by its open, as MS-DOS 7.1 has it - FAT32's
 * EXTENDED_SIZE (INT 21h AX=6C00h or 716Ch, BX bit 12): without it a handle stops at 2 GB - 1.
 * Every open and create DOS95 makes asks for it on a DOS of 7.10 or later (the version with
 * FAT32) - nothing of a DOS's own beyond MS-DOS 7.1's is relied on (FreeDOS lets any handle grow
 * past 2 GB); an earlier DOS is given its own calls (3Dh, 3Ch) as before */
#define OPEN_EXTENDED_SIZE 0x1000
static int dos_extended_size(void)
{
    int major = boot->dos_ver & 0xFF, minor = boot->dos_ver >> 8;
    return major > 7 || (major == 7 && minor >= 10);
}
int fs_open(const char *path, int mode)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    if (put_path_short(XP_PATH, path)) return missing_err(path);
    uint16_t big = dos_extended_size() ? OPEN_EXTENDED_SIZE : 0;
    if (lfn_dosapi || big) {
        r.ax = lfn_dosapi ? 0x716C : 0x6C00; r.bx = (mode & 0x7F) | big; r.cx = 0; r.dx = 1;   /* open existing */
        r.ds = xfer_segv; r.si = xfer_offv + XP_PATH;
    } else {
        r.ax = 0x3D00 | (mode & 0x7F); r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
    }
    int cf = dos_call(0x21, &r);
    return cf ? dos_err(&r, cf) : r.ax;
}

/* v41: DOS95's temporary names in a folder (8.3, of its own: ~D95xxxx) */
/* v41 (#336): the names one after another from a starting point - all 65536 of them are tried
 * before giving up (64 were) */
static uint16_t tmp_seq;
static void tmp_name_at(char *out, const char *ext, uint16_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    out[0] = '~'; out[1] = 'D'; out[2] = '9'; out[3] = '5';
    for (int i = 0; i < 4; i++) out[4 + i] = hx[(v >> (12 - 4 * i)) & 15];
    out[8] = 0;
    if (ext) { out[8] = '.'; strlcpy(out + 9, ext, 4); }
}
/* v41: DOS grows folder `dosdir` (its 8.3 path): k files made in it (INT 21h AH=5Bh, create
 * new), then deleted - DOS fills the folder's free slots and gives it its next cluster, its
 * FAT and free count kept by itself (kernel/vfat.c dir_put).  0, or -1 (and nothing left). */
int dos_grow_folder(const char *dosdir, int k)
{
    if (k < 1) k = 1;
    if (k > 65536) return -1;                           /* (more than a FAT folder holds) */
    char (*made)[13] = kmalloc((uint32_t)k * 13);       /* (v41, #336: as many as it takes - it gave up past 1024) */
    if (!made) return -1;
    int n = 0, bad = 0;
    uint16_t v0 = (uint16_t)(tmp_seq + (uint16_t)jiffies);
    for (uint32_t t = 0; n < k && t < 65536; t++) {
        char nm[13], p[300];
        tmp_name_at(nm, "TMP", (uint16_t)(v0 + t)); tmp_seq++;
        ksnprintf(p, sizeof p, "%s%s%s", dosdir, dosdir[0] && dosdir[strlen(dosdir) - 1] == '\\' ? "" : "\\", nm);
        struct dosregs r = {0};
        put_path(XP_PATH, p);
        r.ax = 0x5B00; r.cx = 0; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
        if (dos_call(0x21, &r)) {
            if (r.ax == 80) continue;                   /* that name is there: another one */
            bad = 1; break;                             /* the folder cannot grow (disk full, root full) */
        }
        struct dosregs c = {0}; c.ax = 0x3E00; c.bx = r.ax; dos_call(0x21, &c);
        strlcpy(made[n++], nm, 13);
    }
    for (int i = 0; i < n; i++) {
        char p[300];
        ksnprintf(p, sizeof p, "%s%s%s", dosdir, dosdir[0] && dosdir[strlen(dosdir) - 1] == '\\' ? "" : "\\", made[i]);
        struct dosregs r = {0};
        put_path(XP_PATH, p);
        r.ax = 0x4100; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
        dos_call(0x21, &r);
    }
    kfree(made);
    dos_disk_flush();
    return (bad || n < k) ? -1 : 0;
}
/* v41: DOS makes a folder in `dosdir` under a name of DOS95's; its name in `made` (13 bytes).
 * The next name is tried only when this one is there already - asked first (INT 21h AX=4300h):
 * DOS answers "access denied" for a name that is there, and for a disk or a root folder that is
 * full, and for a folder it made and whose cluster it could not write.  Taken for "there
 * already" each time, a full disk was 65536 calls of DOS before the answer, and a disk that
 * cannot be written where the folder came to lie got a folder a try: 106 of them, ~D95DFD5 to
 * ~D95E03E, in C:\TEMP of the regression's disk on MS-DOS 7.1 (its last 6 MB out of the BIOS's
 * reach - tools/mkms71img.sh), each a cluster of what a deleted file had left there. */
int dos_mkdir_temp(const char *dosdir, char *made)
{
    uint16_t v0 = (uint16_t)(tmp_seq + (uint16_t)jiffies);
    for (uint32_t t = 0; t < 65536; t++) {
        char nm[13], p[300];
        tmp_name_at(nm, NULL, (uint16_t)(v0 + t)); tmp_seq++;
        ksnprintf(p, sizeof p, "%s%s%s", dosdir, dosdir[0] && dosdir[strlen(dosdir) - 1] == '\\' ? "" : "\\", nm);
        struct dosregs a = {0};
        put_path(XP_PATH, p);
        a.ax = 0x4300; a.ds = xfer_segv; a.dx = xfer_offv + XP_PATH;
        if (!dos_call(0x21, &a)) continue;              /* there already: the next name */
        struct dosregs r = {0};
        put_path(XP_PATH, p);
        r.ax = 0x3900; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
        if (!dos_call(0x21, &r)) { strlcpy(made, nm, 13); dos_disk_flush(); return 0; }
        /* it cannot be made.  A folder DOS made and could not write is not left behind (if DOS
         * cannot take it away either - it reads the folder first - it is the disk checker's) */
        struct dosregs d = {0};
        put_path(XP_PATH, p);
        d.ax = 0x3A00; d.ds = xfer_segv; d.dx = xfer_offv + XP_PATH;
        if (!dos_call(0x21, &d)) kprintf("vfat: %s: made by DOS and not written (error %u) - taken away\n", p, r.ax);
        dos_disk_flush();
        return -1;
    }
    return -1;
}
/* v41 (#388): a name of DOS95's (~D95xxxx) that nothing in `dosdir` has; 0, or -1 */
int dos_tmp_free(const char *dosdir, char *name)
{
    uint16_t v0 = (uint16_t)(tmp_seq + (uint16_t)jiffies);
    for (uint32_t t = 0; t < 65536; t++) {
        char nm[13], p[300];
        tmp_name_at(nm, NULL, (uint16_t)(v0 + t)); tmp_seq++;
        ksnprintf(p, sizeof p, "%s%s%s", dosdir, dosdir[0] && dosdir[strlen(dosdir) - 1] == '\\' ? "" : "\\", nm);
        struct dosregs a = {0};
        put_path(XP_PATH, p);
        a.ax = 0x4300; a.ds = xfer_segv; a.dx = xfer_offv + XP_PATH;
        if (dos_call(0x21, &a)) { strlcpy(name, nm, 13); return 0; }   /* not there */
    }
    return -1;
}
/* v41: a folder DOS95 made and could not name removed (INT 21h AH=3Ah, its DOS path) */
int dos_rmdir_raw(const char *dospath)
{
    struct dosregs r = {0};
    put_path(XP_PATH, dospath);
    r.ax = 0x3A00; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
    return dos_call(0x21, &r) ? -1 : 0;
}

int fs_create(const char *path)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    /* Native path: DOS95 writes the long-name directory entry itself (with an
     * 8.3 alias), then opens that alias through DOS to get a writable handle.
     * Works on bare MS-DOS with no 71xx provider. */
    int vc = lfn_dosapi ? -1 : vfat_create(path);
    if (vc == -3) return -3;                        /* v40 (#223): a name FAT forbids */
    if (vc == 0) {
        char sp[260];
        /* v41: the entry made is opened by its 8.3 name, or the call fails - it went on to
         * DOS with the long name, and DOS made a file of another name */
        if (vfat_shortpath(path, sp, sizeof sp)) return -3;
        dos_disk_flush();                           /* let DOS see our new entry */
        put_path(XP_PATH, sp);
        if (dos_extended_size()) {                  /* (F24: a handle to 4 GB - 1) */
            r.ax = 0x6C00; r.bx = 2 | OPEN_EXTENDED_SIZE; r.cx = 0; r.dx = 0x12;      /* create or truncate */
            r.ds = xfer_segv; r.si = xfer_offv + XP_PATH;
        } else { r.ax = 0x3C00; r.cx = 0; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }  /* truncate */
        int cf = dos_call(0x21, &r);
        return cf ? dos_err(&r, cf) : r.ax;
    }
    memset(&r, 0, sizeof r);
    if (!lfn_dosapi) {                              /* v41: DOS given its 8.3 name, or no call */
        char dn[260];
        if (vfat_dosname(path, dn, sizeof dn)) return -3;
        put_path(XP_PATH, dn);
    } else put_path(XP_PATH, path);
    if (lfn_dosapi || dos_extended_size()) {
        r.ax = lfn_dosapi ? 0x716C : 0x6C00; r.bx = 2 | (dos_extended_size() ? OPEN_EXTENDED_SIZE : 0); r.cx = 0; r.dx = 0x12;   /* create or truncate */
        r.ds = xfer_segv; r.si = xfer_offv + XP_PATH;
    } else {
        r.ax = 0x3C00; r.cx = 0; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
    }
    int cf = dos_call(0x21, &r);
    return cf ? dos_err(&r, cf) : r.ax;
}

int fs_close(int h)
{
    struct dosregs r = {0};
    r.ax = 0x3E00; r.bx = h;
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}

/* a second DOS handle on an open file (INT 21h AH=45h): its position is shared */
int fs_dup(int h)
{
    struct dosregs r = {0};
    r.ax = 0x4500; r.bx = h;
    int cf = dos_call(0x21, &r);
    return cf ? dos_err(&r, cf) : r.ax;
}

/* make h2 a second handle on h's open file, closing what h2 was (INT 21h AH=46h) -
 * fslock.c, when a file open only for reading is opened to write as well */
int fs_forcedup_i(int h, int h2)
{
    struct dosregs r = {0};
    r.ax = 0x4600; r.bx = h; r.cx = h2;
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}

int fs_read(int h, void *buf, int n)
{
    int total = 0;
    while (n > 0) {
        int chunk = n > XP_DATA_MAX ? XP_DATA_MAX : n;
        struct dosregs r = {0};
        r.ax = 0x3F00; r.bx = h; r.cx = chunk; r.ds = xfer_segv; r.dx = xfer_offv + XP_DATA;
        int cf = dos_call(0x21, &r);
        if (cf) return total ? total : dos_err(&r, cf);
        if (r.ax == 0) break;
        memcpy((uint8_t*)buf + total, xfer + XP_DATA, r.ax);
        total += r.ax; n -= r.ax;
        if (r.ax < chunk) break;
    }
    return total;
}

/* v41 (#336, F24): what a write may still put in the file of DOS95's handle h - FAT32's files
 * end at 4 GB - 1 and DOS's place in one is 32 bits: DOS (FreeDOS) went on past it at 0 - the
 * byte at 4 GB - 1 "written" into the last cluster's slack, the place wrapped round to the file's
 * start, where the next write went over the file.  From the handle's file table entry (its place,
 * +15h; a device, +5 bit 7, has none) - no DOS call; 0xFFFFFFFF when it is not a file's.  (A DOS
 * box's write: v86.c dos_write_room) */
static uint32_t write_room(int h)
{
    int n; uint8_t *jft = dos95_jft(&n);
    if (h < 0 || h >= n || jft[h] == 0xFF) return 0xFFFFFFFFu;     /* (DOS answers it) */
    uint8_t *e = sft_entry(vms[0]->sft_head, jft[h]);
    if (!e || (*(uint16_t*)(e + 5) & 0x80)) return 0xFFFFFFFFu;     /* a device */
    return 0xFFFFFFFFu - *(uint32_t*)(e + 0x15);
}
int fs_write(int h, const void *buf, int n)
{
    int total = 0;
    if (n > 0) {                                        /* (v41, F24) cut to what fits; none: 0 written, */
        uint32_t room = write_room(h);                  /* as on a full disk - no DOS call (one of 0 bytes */
        if ((uint32_t)n > room) n = (int)room;          /* cuts the file there) */
        if (n == 0) return 0;
    }
    do {
        int chunk = n > XP_DATA_MAX ? XP_DATA_MAX : n;
        memcpy(xfer + XP_DATA, (const uint8_t*)buf + total, chunk);
        struct dosregs r = {0};
        r.ax = 0x4000; r.bx = h; r.cx = chunk; r.ds = xfer_segv; r.dx = xfer_offv + XP_DATA;
        int cf = dos_call(0x21, &r);
        if (cf) return total ? total : dos_err(&r, cf);
        total += r.ax; n -= r.ax;
        if (r.ax < chunk) break;
    } while (n > 0);
    return total;
}

/* v41 (#336, F24): a place in a file to FAT32's 4 GB - 1.  DOS's LSEEK (INT 21h AH=42h) has 32
 * bits each way - CX:DX the offset (signed from where the file is or its end, from the start the
 * place itself), DX:AX the place (MS-DOS 7.1: unsigned, to 4 GB - 1).  The place is worked out
 * here and DOS told it from the start: none before the start or past 4 GB - 1 - -25 (DOS's seek
 * error) and the file left where it was */
int64_t fs_seek64(int h, int64_t off, int whence)
{
    struct dosregs r;
    int64_t base = 0, here = 0;
    int cf;
    if (whence < 0 || whence > 2) return -1;            /* DOS: function not valid */
    if (whence) {
        memset(&r, 0, sizeof r); r.ax = 0x4201; r.bx = h;          /* where it is (CX:DX 0) */
        if ((cf = dos_call(0x21, &r))) return dos_err(&r, cf);
        here = base = (int64_t)(((uint32_t)r.dx << 16) | r.ax);
        if (whence == 2) {
            memset(&r, 0, sizeof r); r.ax = 0x4202; r.bx = h;      /* its end */
            if ((cf = dos_call(0x21, &r))) return dos_err(&r, cf);
            base = (int64_t)(((uint32_t)r.dx << 16) | r.ax);
        }
    }
    int64_t np = base + off;
    if (np < 0 || np > 0xFFFFFFFFLL) {
        if (whence == 2) {                                  /* back where it was */
            memset(&r, 0, sizeof r); r.ax = 0x4200; r.bx = h; r.cx = (uint16_t)((uint32_t)here >> 16); r.dx = (uint16_t)here;
            dos_call(0x21, &r);
        }
        return -25;
    }
    if (whence && off == 0) return np;                      /* (there already) */
    memset(&r, 0, sizeof r);
    r.ax = 0x4200; r.bx = h; r.cx = (uint16_t)((uint32_t)np >> 16); r.dx = (uint16_t)np;
    if ((cf = dos_call(0x21, &r))) return dos_err(&r, cf);
    return (int64_t)(((uint32_t)r.dx << 16) | r.ax);
}
/* the 32-bit call (v40's programs): DOS's own seek, moving as DOS moves - a program that appends
 * with fs_seek(h, 0, 2) and does not read the answer goes on finding the end - and the place said
 * when it fits in 31 bits; past 2 GB - 1, -25 (it came back negative: a nonsense error number) */
int32_t fs_seek(int h, int32_t off, int whence)
{
    struct dosregs r = {0};
    r.ax = 0x4200 | (whence & 3); r.bx = h; r.cx = (uint32_t)off >> 16; r.dx = off & 0xFFFF;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    uint32_t p = ((uint32_t)r.dx << 16) | r.ax;
    return p > 0x7FFFFFFF ? -25 : (int32_t)p;
}

int64_t fs_filesize64(int h)
{
    int64_t cur = fs_seek64(h, 0, 1);
    if (cur < 0) return cur;
    int64_t end = fs_seek64(h, 0, 2);
    fs_seek64(h, cur, 0);
    return end;
}
/* ... and the 32-bit one: a file of 2 GB or more is -25 (not its size cut to 31 bits) */
int32_t fs_filesize(int h) { int64_t s = fs_filesize64(h); return s > 0x7FFFFFFF ? -25 : (int32_t)s; }

static int path_op(uint16_t ax, uint16_t lfn_ax, const char *path, int unlink_fn)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    if (put_path_short(XP_PATH, path)) return missing_err(path);
    if (lfn_dosapi && lfn_ax) { r.ax = lfn_ax; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; r.si = 0; }
    else { r.ax = ax; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }
    (void)unlink_fn;
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}

int fs_unlink(const char *path) { return path_op(0x4100, 0x7141, path, 1); }
int fs_mkdir(const char *path)
{
    NO_PHANTOM(path);
    int v = vfat_mkdir(path);
    if (v == 0) return 0;
    if (v == -3) return -3;                         /* v40 (#223): a name FAT forbids - DOS not asked */
    if (v == -2) return -5;                         /* v41: there already - DOS's own answer */
    if (v == -5) return -5;                         /* v41: DOS could not make the folder (a full disk): its answer */
    if (!lfn_dosapi) {                              /* v41: DOS given its 8.3 name, or no call */
        char dn[260];
        if (vfat_dosname(path, dn, sizeof dn)) return -3;
        return path_op(0x3900, 0x7139, dn, 0);
    }
    return path_op(0x3900, 0x7139, path, 0);
}
int fs_rmdir(const char *path)
{
    /* v41 (#388): a folder past the length of path DOS removes one at (vfat.c) */
    extern int vfat_rmdir_deep(const char *path);
    int v = vfat_rmdir_deep(path);
    if (v == 0) return 0;
    if (v == -5) return -5;
    return path_op(0x3A00, 0x713A, path, 0);
}
int fs_chdir(const char *path)  { return path_op(0x3B00, 0x713B, path, 0); }

int fs_rename(const char *from, const char *to)
{
    NO_PHANTOM(from); NO_PHANTOM(to);
    /* Native: rewrite the directory entry with the new long name (+ fresh 8.3
     * alias), preserving the file's data. Handles long names on bare MS-DOS. */
    if (!lfn_dosapi) {
        int v = vfat_rename(from, to);
        if (v == 0) return 0;
        if (v == -2) return -5;          /* the target exists: DOS's own answer, access denied */
        if (v == -4) return -5;          /* v41 (#317): a folder into itself or under itself - the same */
        if (v == -3) return -3;          /* v40 (#223): a name FAT forbids - DOS not asked */
    }

    struct dosregs r = {0};
    if (put_path_short(XP_PATH, from)) return missing_err(from);
    if (!lfn_dosapi) {                   /* v41: DOS given the new name's 8.3 form, or no call */
        char dn[260];
        if (vfat_dosname(to, dn, sizeof dn)) return -3;
        put_path(XP_PATH2, dn);
    } else put_path(XP_PATH2, to);
    r.ax = lfn_dosapi ? 0x7156 : 0x5600;
    r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; r.es = xfer_segv; r.di = xfer_offv + XP_PATH2;
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}

int fs_getattr(const char *path)
{
    NO_PHANTOM(path);
    { extern int vfat_stat(const char *path, uint8_t *attr, uint32_t *size, uint32_t *stamp);   /* v41 (#421): from its folder, no DOS */
      extern int fs_drive_type(int drive);
      uint8_t a;
      if (path[0] && path[1] == ':' && fs_drive_type((path[0] | 0x20) - 'a') == 2) {
          int v = vfat_stat(path, &a, NULL, NULL);
          if (v == 0) return a;
          if (v == -2 || v == -3) return v;
      } }
    struct dosregs r = {0};
    if (put_path_short(XP_PATH, path)) return missing_err(path);
    if (lfn_dosapi) { r.ax = 0x7143; r.bx = 0; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }
    else { r.ax = 0x4300; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }
    int cf = dos_call(0x21, &r);
    return cf ? dos_err(&r, cf) : r.cx;
}

int fs_setattr(const char *path, int attr)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    if (put_path_short(XP_PATH, path)) return missing_err(path);
    if (lfn_dosapi) { r.ax = 0x7143; r.bx = 1; r.cx = attr; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }
    else { r.ax = 0x4301; r.cx = attr; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; }
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}

/* v38: an open file's date and time, DOS's (INT 21h AX=5700h/5701h - the directory
 * entry is written with it when the handle is closed): utime(), Wine's SetFileTime */
int fs_get_filetime(int h, uint16_t *date, uint16_t *time)
{
    struct dosregs r = {0};
    r.ax = 0x5700; r.bx = h;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    if (date) *date = (uint16_t)r.dx;
    if (time) *time = (uint16_t)r.cx;
    return 0;
}
int fs_set_filetime(int h, uint16_t date, uint16_t time)
{
    struct dosregs r = {0};
    r.ax = 0x5701; r.bx = h; r.cx = time; r.dx = date;
    int cf = dos_call(0x21, &r);
    return dos_err(&r, cf);
}
/* v41 (#190): a file's or a folder's date and time set by its name - the folders 7-Zip extracts
 * (SetFileTime on a folder), Wine's, utime()'s.  Where DOS has the long-name API, MS-DOS 7's own
 * call for it (INT 21h AX=7143h BL=03h); else DOS95's VFAT writer puts them in the folder's
 * entry, as it writes long names (DOS has no call that dates a folder: it opens none); a file
 * on a drive DOS95 does not read itself (a network drive) is dated by a handle, as
 * fs_set_filetime */
int fs_set_pathtime(const char *path, uint16_t date, uint16_t time)
{
    NO_PHANTOM(path);
    if (lfn_dosapi) {
        struct dosregs r = {0};
        put_path(XP_PATH, path);
        r.ax = 0x7143; r.bx = 3; r.cx = time; r.di = date; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH;
        int cf = dos_call(0x21, &r);
        return dos_err(&r, cf);
    }
    extern int vfat_set_time(const char *path, uint16_t date, uint16_t time);
    if (vfat_set_time(path, date, time) == 0) return 0;
    int a = fs_getattr(path);
    if (a < 0) return a;
    if (a & FA_DIR) return -5;                          /* (access denied: nothing can date it) */
    int h = fs_open(path, 0);
    if (h < 0) return h;
    int r = fs_set_filetime(h, date, time);
    fs_close(h);
    return r;
}

int fs_getdrive(void)
{
    struct dosregs r = {0};
    r.ax = 0x1900;
    dos_call(0x21, &r);
    return r.ax & 0xFF;
}

int fs_setdrive(int d)
{
    if (phantom_drive(d)) return -15;
    struct dosregs r = {0};
    r.ax = 0x0E00; r.dx = d;
    dos_call(0x21, &r);
    return 0;
}

int fs_getcwd(int drive, char *buf, int n)
{
    if (drive && phantom_drive(drive - 1)) return -15;
    struct dosregs r = {0};
    r.ax = lfn_dosapi ? 0x7147 : 0x4700; r.dx = drive; r.ds = xfer_segv; r.si = xfer_offv + XP_PATH;
    xfer[XP_PATH] = 0;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    int d = drive ? drive : fs_getdrive() + 1;
    ksnprintf(buf, n, "%c:\\%s", 'A' + d - 1, (char*)xfer + XP_PATH);
    return 0;
}

/* bitmap of valid drives via the CDS */
uint32_t fs_drives(void)
{
    uint8_t *lol = PHYSLOW(boot->lol_lin);
    uint16_t cds_off = *(uint16_t*)(lol + 0x16), cds_seg = *(uint16_t*)(lol + 0x18);
    uint8_t lastdrive = lol[0x21];
    int entry = ((boot->dos_ver & 0xFF) >= 4) ? 0x58 : 0x51;
    uint32_t mask = 0;
    for (int i = 0; i < lastdrive && i < 26; i++) {
        uint8_t *c = (uint8_t*)PHYSLOW(LIN(cds_seg, cds_off) + i * entry);
        uint16_t flags = *(uint16_t*)(c + 0x43);
        if (flags & 0xC000) mask |= 1u << i;
    }
    if (one_floppy()) mask &= ~2u;                      /* v40 (#171): no B: - the one drive is A: */
    return mask;
}

int fs_drive_type(int drive)   /* 0 = unknown, 1 = floppy, 2 = hard, 3 = cdrom/net, 4 = ram */
{
    uint8_t *lol = PHYSLOW(boot->lol_lin);
    uint16_t cds_off = *(uint16_t*)(lol + 0x16), cds_seg = *(uint16_t*)(lol + 0x18);
    int entry = ((boot->dos_ver & 0xFF) >= 4) ? 0x58 : 0x51;
    uint8_t *c = (uint8_t*)PHYSLOW(LIN(cds_seg, cds_off) + drive * entry);
    uint16_t flags = *(uint16_t*)(c + 0x43);
    { extern int exfat_is_drive(int drive); if (exfat_is_drive(drive)) return 2; }   /* (v42, #490: an exFAT volume - a disk) */
    if (flags & 0x8000) return 3;           /* network / CD-ROM redirector */
    if (drive < 2) {
        uint16_t equip = *(uint16_t*)PHYSLOW(BDA_EQUIP);
        int nfl = (equip & 1) ? ((equip >> 6) & 3) + 1 : 0;
        if (drive < nfl) return 1;
        if (drive == 1 && nfl == 1) return 1;   /* B: single floppy alias */
    }
    return 2;
}

/* Is `drive` (0-based) an MSCDEX/SHSUCDX CD-ROM? (int 2Fh AX=150Bh drive check) */
int fs_is_cdrom(int drive)
{
    struct dosregs r = {0};
    r.ax = 0x150B; r.cx = drive; r.bx = 0;
    dos_call(0x2F, &r);
    return r.bx == 0xADAD && (r.ax & 0xFFFF) != 0;
}

int fs_diskfree(int drive, uint32_t *total_kb, uint32_t *free_kb)
{
    if (drive && phantom_drive(drive - 1)) return -15;
    {   /* v42 (#490): an exFAT volume's own counts (DOS's are 16 bits: 2 GB at most) */
        extern int exfat_free(int drive, uint64_t *freeb, uint64_t *total);
        uint64_t fb, tb;
        if (exfat_free(drive ? drive - 1 : fs_getdrive(), &fb, &tb) == 0) {
            *total_kb = tb / 1024 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(tb / 1024);
            *free_kb = fb / 1024 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(fb / 1024);
            return 0;
        }
    }
    struct dosregs r = {0};
    /* v41 (#336): a drive's whole size and free space - MS-DOS 7.1's and FreeDOS's INT 21h AX=7303h
     * (Windows 95 OSR2's FAT32 call) first: AH=36h's 16-bit counts stop at 2 GB */
    {
        char root[4] = "?:\\"; root[0] = (char)('A' + (drive ? drive - 1 : fs_getdrive()));
        put_path(XP_PATH, root);
        uint8_t *b = xfer + XP_FIND;
        memset(b, 0, 48); *(uint16_t*)b = 44;
        r.ax = 0x7303; r.ds = xfer_segv; r.dx = xfer_offv + XP_PATH; r.es = xfer_segv; r.di = xfer_offv + XP_FIND; r.cx = 44;
        if (!dos_call(0x21, &r) && *(uint32_t*)(b + 4) && *(uint32_t*)(b + 8)) {
            uint64_t clus = (uint64_t)*(uint32_t*)(b + 4) * *(uint32_t*)(b + 8);     /* bytes a cluster */
            uint64_t t = clus * *(uint32_t*)(b + 16) / 1024, f = clus * *(uint32_t*)(b + 12) / 1024;
            *total_kb = t > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)t;
            *free_kb = f > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)f;
            return 0;
        }
        memset(&r, 0, sizeof r);
    }
    r.ax = 0x3600; r.dx = drive;   /* 0 = default, 1 = A: */
    dos_call(0x21, &r);
    if (r.ax == 0xFFFF) return -15;
    uint32_t clus = (uint32_t)r.ax * r.cx;   /* bytes per cluster */
    *total_kb = (clus / 1024) ? (clus / 1024) * r.dx : (clus * r.dx) / 1024;
    *free_kb  = (clus / 1024) ? (clus / 1024) * r.bx : (clus * r.bx) / 1024;
    return 0;
}

int fs_truename(const char *path, char *out, int n)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    if (lfn_dosapi) {
        put_path(XP_PATH, path);
        r.ax = 0x7160; r.cx = 2;    /* long name */
        r.ds = xfer_segv; r.si = xfer_offv + XP_PATH; r.es = xfer_segv; r.di = xfer_offv + XP_PATH2;
        int cf = dos_call(0x21, &r);
        if (!cf) { strlcpy(out, (char*)xfer + XP_PATH2, n); return 0; }
    }
    /* Native: an absolute long path is already canonical — keep its long names
     * instead of letting DOS 0x6000 collapse them to 8.3. */
    if (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        strlcpy(out, path, n);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
        return 0;
    }
    memset(&r, 0, sizeof r);
    put_path(XP_PATH, path);
    r.ax = 0x6000; r.ds = xfer_segv; r.si = xfer_offv + XP_PATH; r.es = xfer_segv; r.di = xfer_offv + XP_PATH2;
    int cf = dos_call(0x21, &r);
    if (cf) return dos_err(&r, cf);
    strlcpy(out, (char*)xfer + XP_PATH2, n);
    return 0;
}

int fs_shortname(const char *path, char *out, int n)
{
    NO_PHANTOM(path);
    struct dosregs r = {0};
    if (lfn_dosapi) {
        put_path(XP_PATH, path);
        r.ax = 0x7160; r.cx = 1;
        r.ds = xfer_segv; r.si = xfer_offv + XP_PATH; r.es = xfer_segv; r.di = xfer_offv + XP_PATH2;
        int cf = dos_call(0x21, &r);
        if (!cf) { strlcpy(out, (char*)xfer + XP_PATH2, n); return 0; }
    }
    /* Native: resolve the true on-disk 8.3 alias ourselves. */
    char sp[260];
    if (vfat_shortpath(path, sp, sizeof sp) == 0) { strlcpy(out, sp, n); return 0; }
    strlcpy(out, path, n);
    return 0;
}

/* v41 (#466): the date and the time of day the kernel's own, as Windows 95's VTD keeps them - DOS asked for the date once,
 * and again when a clock has been set (clock_gen: a box's DATE, Date/Time Properties); the time of day the kernel's count of
 * the BIOS's ticks (master_ticks), which DOS's own CLOCK$ reads (every machine's BIOS count is set from it).  Until v41 each
 * look was two DOS calls through the system session - the desktop's clock, every second, on the desktop's thread - and the
 * desktop stood still while the system session waited (for a box's DOS call on a floppy: seconds, vm.c vm_dos_gate) */
static volatile uint32_t dt_gen = 0xFFFFFFFFu;          /* clock_gen when DOS was asked (none yet) */
static uint32_t dt_day0;                                /* ... the kernel's day count then */
static int dt_y = 1980, dt_mo = 1, dt_d = 1;            /* ... and DOS's date */
static uint32_t dt_days(int y, int m, int d)            /* days since 1 January 1980 */
{
    static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (y < 1980 || m < 1 || m > 12) return 0;
    uint32_t n = (uint32_t)(y - 1980) * 365 + (uint32_t)(y - 1977) / 4 + before[m - 1] + (uint32_t)d - 1;
    if (m > 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) n++;
    return n - ((y > 2100) ? (uint32_t)((y - 2001) / 100 - (y - 2001) / 400) : 0);   /* (2100, 2200, 2300: no 29 February) */
}
static void dt_civil(uint32_t n, int *y, int *m, int *d)
{
    int yy = 1980;
    for (;;) { int len = (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365; if (n < (uint32_t)len) break; n -= len; yy++; }
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int mm = 0;
    for (; mm < 11; mm++) { int l = ml[mm] + (mm == 1 && (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0))); if (n < (uint32_t)l) break; n -= l; }
    *y = yy; *m = mm + 1; *d = (int)n + 1;
}
/* (fslock.c: before its lock) the kept date and time, 0 when DOS must be asked */
int fs_datetime_kept(int *y, int *mo, int *d, int *dow, int *h, int *mi, int *s)
{
    extern volatile uint32_t clock_gen;
    extern uint32_t master_ticks_now(void);
    uint32_t fl = save_flags_cli();
    if (dt_gen != clock_gen) { restore_flags(fl); return 0; }
    uint32_t mt = master_ticks_now(), day0 = dt_day0;
    int yy = dt_y, mm = dt_mo, dd = dt_d;
    restore_flags(fl);
    uint32_t n = dt_days(yy, mm, dd) + (mt / BDA_TICKS_DAY - day0);
    uint32_t sec = (uint32_t)((uint64_t)(mt % BDA_TICKS_DAY) * 65536u / 1193182u);
    if (y || mo || d) { int a, b, c; dt_civil(n, &a, &b, &c); if (y) *y = a; if (mo) *mo = b; if (d) *d = c; }
    if (dow) *dow = (int)((n + 2) % 7);                 /* (1 January 1980: a Tuesday) */
    if (h) *h = (int)(sec / 3600); if (mi) *mi = (int)(sec / 60 % 60); if (s) *s = (int)(sec % 60);
    return 1;
}
void fs_get_datetime(int *y, int *mo, int *d, int *dow, int *h, int *mi, int *s)
{
    extern volatile uint32_t clock_gen;
    extern uint32_t master_ticks_now(void);
    if (fs_datetime_kept(y, mo, d, dow, h, mi, s)) return;
    uint32_t g = clock_gen, day = master_ticks_now() / BDA_TICKS_DAY;
    struct dosregs r = {0};
    r.ax = 0x2A00; dos_call(0x21, &r);
    if (y) *y = r.cx; if (mo) *mo = r.dx >> 8; if (d) *d = r.dx & 0xFF; if (dow) *dow = r.ax & 0xFF;
    if (r.cx >= 1980 && (r.dx >> 8) >= 1 && (r.dx >> 8) <= 12 && master_ticks_now() / BDA_TICKS_DAY == day) {
        uint32_t fl = save_flags_cli();
        dt_y = r.cx; dt_mo = r.dx >> 8; dt_d = r.dx & 0xFF; dt_day0 = day; dt_gen = g;
        restore_flags(fl);
    }
    memset(&r, 0, sizeof r);
    r.ax = 0x2C00; dos_call(0x21, &r);
    if (h) *h = r.cx >> 8; if (mi) *mi = r.cx & 0xFF; if (s) *s = r.dx >> 8;
}

int fs_set_date(int y, int mo, int d)
{
    struct dosregs r = {0};
    r.ax = 0x2B00; r.cx = y; r.dx = (mo << 8) | d;
    dos_call(0x21, &r);
    if (r.ax & 0xFF) return -1;
    extern volatile uint32_t clock_gen;
    clock_gen++;                                         /* v41 (#299): programs read the clock again */
    return 0;
}

int fs_set_time(int h, int mi, int s)
{
    struct dosregs r = {0};
    r.ax = 0x2D00; r.cx = (h << 8) | mi; r.dx = s << 8;
    dos_call(0x21, &r);
    if (r.ax & 0xFF) return -1;
    /* v41 (#298): the kernel's count of BIOS ticks moved with it - every machine's tick count is
     * set from it at its timer interrupt (v86.c), the system session's too: until v41 the clock
     * set here went back to where it was at the next tick, in every machine but for an instant */
    extern void clock_ticks_set(uint32_t t);
    uint32_t t = (uint32_t)(((uint64_t)(h * 3600u + mi * 60u + s) * 1193182u + 32768u) / 65536u);
    clock_ticks_set(t < BDA_TICKS_DAY ? t : BDA_TICKS_DAY - 1);
    return 0;
}

/* whole-file helpers */
int fs_read_file(const char *path, void *buf, int max)
{
    int h = fs_open(path, 0);
    if (h < 0) return h;
    int n = fs_read(h, buf, max);
    fs_close(h);
    return n;
}

int fs_write_file(const char *path, const void *buf, int n)
{
    int h = fs_create(path);
    if (h < 0) return h;
    int w = fs_write(h, buf, n);
    fs_close(h);
    return w;
}

int fs_exists(const char *path) { return fs_getattr(path) >= 0; }

/* copy a file; returns 0 or DOS error. progress callback may abort by returning nonzero */
int fs_copy_file(const char *from, const char *to, int (*progress)(uint32_t done, uint32_t total, void *ctx), void *ctx)
{
    int src = fs_open(from, 0);
    if (src < 0) return src;
    int64_t total = fs_filesize64(src);                 /* (v41, F24: to 4 GB - 1) */
    uint8_t *buf = kmalloc(XP_DATA_MAX);
    if (!buf) { fs_close(src); return -8; }          /* (v41, #336: no memory - said, not a copy into address 0) */
    int dst = fs_create(to);
    if (dst < 0) { kfree(buf); fs_close(src); return dst; }
    uint32_t done = 0; int err = 0;
    for (;;) {
        int n = fs_read(src, buf, XP_DATA_MAX);
        if (n < 0) { err = n; break; }
        if (n == 0) break;
        int w = fs_write(dst, buf, n);
        if (w < n) { err = w < 0 ? w : -39; break; }   /* disk full */
        done += n;
        if (progress && progress(done, total < 0 ? 0 : (uint32_t)total, ctx)) { err = -1000; break; }
    }
    kfree(buf);
    fs_close(dst); fs_close(src);
    if (err) fs_unlink(to);
    else {
        /* copy attributes and timestamp */
        int a = fs_getattr(from);
        if (a >= 0) fs_setattr(to, a & 0x27);
    }
    return err;
}

const char *fs_strerror(int err)
{
    switch (-err) {
    case 1: return "Invalid function";
    case 2: return "File not found";
    case 3: return "Path not found";
    case 4: return "Too many open files";
    case 5: return "Access denied";
    case 6: return "Invalid handle";
    case 8: return "Not enough memory";
    case 15: return "Invalid drive";
    case 16: return "Cannot remove current directory";
    case 17: return "Not same device";
    case 18: return "No more files";
    case 19: return "Disk is write-protected";
    case 21: return "Drive not ready";
    case 32: return "Sharing violation";
    case 39: return "Disk full";
    case 80: return "File already exists";
    case 206: return "The file name or extension is too long";   /* v41 (#336): Windows' ERROR_FILENAME_EXCED_RANGE */
    case 1000: return "Cancelled";
    default: return "DOS error";
    }
}
#ifdef QDP
/* the QEMU DOS port's setup (qdp/kernel/qdpsetup.c): count sectors of a BIOS hard disk unit (80h ...) from lba into buf
 * (write 0) or from buf to the disk, by INT 13h AH=42h/43h - as DOS reads and writes it (for a disk the kernel drives,
 * the kernel answers: dcache.c kd_int13), in pieces of the transfer buffer: 0, or -1 (the BIOS said no) */
int qdp_int13_rw(int unit, uint64_t lba, int count, void *buf, int write)
{
    extern void fs_lock(void), fs_unlock(void);
    uint8_t *p = buf;
    while (count > 0) {
        int n = count > XP_DATA_MAX / 512 ? XP_DATA_MAX / 512 : count;
        fs_lock();
        uint8_t *pk = xfer + XP_DPKT;
        pk[0] = 16; pk[1] = 0; *(uint16_t*)(pk + 2) = (uint16_t)n;
        *(uint16_t*)(pk + 4) = xfer_offv + XP_DATA; *(uint16_t*)(pk + 6) = xfer_segv;
        *(uint64_t*)(pk + 8) = lba;
        if (write) memcpy(xfer + XP_DATA, p, (uint32_t)n * 512);
        struct dosregs r = {0};
        r.ax = write ? 0x4300 : 0x4200; r.dx = (uint16_t)unit; r.ds = xfer_segv; r.si = xfer_offv + XP_DPKT;
        int bad = dos_call(0x13, &r);
        if (!bad && !write) memcpy(p, xfer + XP_DATA, (uint32_t)n * 512);
        fs_unlock();
        if (bad) return -1;
        p += (uint32_t)n * 512; lba += (uint64_t)n; count -= n;
    }
    return 0;
}
/* how many hard disk units the BIOS has (INT 13h AH=08h's DL), and a unit's size in sectors (EDD AH=48h, else its CHS):
 * 0, or -1 (no such unit) */
int qdp_int13_units(void)
{
    struct dosregs r = {0};
    r.ax = 0x0800; r.dx = 0x80;
    if (dos_call(0x13, &r)) return 0;
    return r.dx & 0xFF;
}
int qdp_int13_size(int unit, uint64_t *sectors)
{
    uint8_t e[0x42];
    int n = bios_edd(unit, e, sizeof e);
    if (n >= 0x18 && *(uint64_t*)(e + 0x10)) { *sectors = *(uint64_t*)(e + 0x10); return 0; }
    struct dosregs r = {0};
    r.ax = 0x0800; r.dx = (uint16_t)unit;
    if (dos_call(0x13, &r) || !(r.cx & 0x3F)) return -1;
    uint32_t cyl = (r.cx >> 8) | ((uint32_t)(r.cx & 0xC0) << 2), hd = (r.dx >> 8) + 1u, spt = r.cx & 0x3Fu;
    *sectors = (uint64_t)(cyl + 1) * hd * spt;
    return 0;
}
#endif
