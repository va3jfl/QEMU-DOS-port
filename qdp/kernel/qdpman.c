/* QEMU DOS port - the QEMU manager: the one window of the QEMU DOS port, maximized over the whole screen.
 *
 * Its machines are .VM files (INI) in <QEMUDOS>\MACHINES - DOS95's QEMU Launcher's files, read as they are, with the
 * keys the QEMU DOS port adds (Cpus, Hdd, FloppyB, Pcspk, AutoStart).  A machine runs over the whole screen
 * (kernel/vpc.c); Ctrl+Alt+Home brings this window back while it runs, and the machine is shown, paused, reset, turned
 * off, given another CD from here.  QEMU 0.14's state is global: one machine runs at a time.
 *
 * The autoboot (AUTOBOOT.INI beside QEMUDOS.COM): a machine started when the QEMU DOS port starts, after a countdown
 * Esc stops, and what is done when it shuts down - the manager, the machine again, MS-DOS, or the computer restarted.
 * The same from DOS: QEMUDOS /M "name" starts that machine at once (AUTOEXEC.BAT, a batch file of one's own). */
#include "shell.h"
#include "dosfs.h"
#include "app.h"
#include "net.h"
#include "font.h"
#include "qdp.h"

extern int cpu_level;
extern int ncpus;

#define MAX_MEM_MB   2047          /* QEMU 0.14's own limit on a 32-bit host (vl.c: "at most 2047 MB RAM can be simulated") */
#define MAX_CPUS     8             /* (QEMU 0.14's TCG runs them in turn on one host thread: the guest sees them all) */
/* QEMU's own memory beside the machine's: its translated code (64 MB at most, -tb-size), the table of its blocks,
 * the video card's memory and the rest - measured: a 2047 MB machine took 2047 + 118 MB */
#define QEMU_OVERHEAD_MB 128

static char machines_dir[200];
static const char *mdir(void)
{
    if (!machines_dir[0]) path_join(machines_dir, sizeof machines_dir, qdp_dir(), "MACHINES");
    return machines_dir;
}

/* ---- a machine ---------------------------------------------------------------------------------------------------- */
struct machine {
    char file[260];
    char name[256];
    int memory, cpus;
    char cpu[64];
    char hda[260], hdb[260], hdd[260], cdrom[260], floppy[260], floppyb[260];
    char boot[8];
    char vga[32];
    int sound, pcspk, tablet, net, localtime, acpi, autostart;
    char mac[20];
    char *extra;
};
static const char *mextra(const struct machine *m) { return m->extra ? m->extra : ""; }

static const char *net_ids[] = { "", "pcnet", "e1000", "rtl8139", "ne2k_pci", "ne2k_isa" };
static const char *net_names[] = { "(none)", "AMD PCnet-PCI II (Am79C970A) - Windows 95..XP, Linux", "Intel PRO/1000 MT (82540EM) - Windows XP and later, Linux",
                                   "Realtek RTL8139 - Windows 98..XP, Linux", "NE2000 PCI (Realtek 8029)", "NE2000 ISA (port 300h, IRQ 9) - DOS packet drivers" };
#define N_NET 6
static const char *cpu_names[] = { "QEMU virtual CPU (default)", "486", "Pentium", "Pentium II", "Pentium III", "Core Duo", "Athlon", "Atom N270", "qemu32" };
static const char *cpu_ids[]   = { "", "486", "pentium", "pentium2", "pentium3", "coreduo", "athlon", "n270", "qemu32" };
#define N_CPU 9
static const char *vga_names[] = { "Cirrus Logic GD5446 (Windows 95 and later have its driver)", "Standard VGA with VESA VBE (Bochs)", "VMware SVGA II" };
static const char *vga_ids[]   = { "cirrus", "std", "vmware" };
#define N_VGA 3
static const char *boot_names[] = { "Hard disk", "CD-ROM", "Floppy", "CD-ROM, then the hard disk", "Floppy, then the hard disk" };
static const char *boot_ids[]   = { "c", "d", "a", "dc", "ac" };
#define N_BOOT 5

/* a machine's own MAC address: locally administered (52:54:D9), its last three bytes from this PC's network card's
 * address, the date and time, the timer's count and the processor's cycle count - krand alone gives every PC the same
 * address after it starts, and two machines of two QEMU DOS port PCs on one network must not share one */
static void new_mac(char *out, int n)
{
    extern uint32_t master_ticks_now(void);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    fs_get_datetime(&y, &mo, &d, NULL, &h, &mi, &sec);
    uint32_t x = (uint32_t)((((y * 12 + mo) * 31 + d) * 24 + h) * 3600 + mi * 60 + sec);
    x = x * 2654435761u ^ master_ticks_now() * 2246822519u ^ rdtsc_lo() * 3266489917u ^ krand();
    struct netdev *nd = net_device();
    if (nd) for (int i = 0; i < 6; i++) x = (x ^ nd->mac[i]) * 16777619u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    ksnprintf(out, n, "52:54:d9:%02x:%02x:%02x", (x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);
}
static void machine_defaults(struct machine *m)
{
    memset(m, 0, sizeof *m);
    strcpy(m->name, "New Machine"); m->memory = 64; m->cpus = 1; strcpy(m->boot, "c"); strcpy(m->vga, "cirrus");
    m->sound = 1; m->pcspk = 1; m->tablet = 0; m->net = net_device() ? 1 : 0; m->localtime = 1; m->acpi = 1;
}
static const char *ini_find(const char *buf, const char *key, int *len)
{
    const char *p = buf; int kl = strlen(key);
    while (*p) {
        const char *eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (!strnicmp(p, key, kl) && p[kl] == '=') {
            int vl = eol - (p + kl + 1);
            while (vl > 0 && (p[kl + vl] == ' ' || p[kl + vl] == '\t')) vl--;
            *len = vl; return p + kl + 1;
        }
        p = eol; while (*p == '\n' || *p == '\r') p++;
    }
    return NULL;
}
static const char *ini_val(const char *buf, const char *key, char *out, int n)
{
    int vl; const char *v = ini_find(buf, key, &vl);
    if (!v) { out[0] = 0; return NULL; }
    if (vl > n - 1) vl = n - 1;
    memcpy(out, v, vl); out[vl] = 0;
    return out;
}
static void machine_free(struct machine *m) { kfree(m->extra); m->extra = NULL; }
static int machine_load(struct machine *m, const char *path)
{
    int n; char *buf = kread_text(path, &n);
    if (!buf) return -1;
    machine_defaults(m);
    m->net = 1;
    strlcpy(m->file, path, sizeof m->file);
    char v[256];
    if (ini_val(buf, "Name", v, sizeof v)) strlcpy(m->name, v, sizeof m->name);
    if (ini_val(buf, "Memory", v, sizeof v)) m->memory = atoi(v);
    if (ini_val(buf, "Cpus", v, sizeof v)) m->cpus = atoi(v);
    ini_val(buf, "Cpu", m->cpu, sizeof m->cpu);
    ini_val(buf, "Hda", m->hda, sizeof m->hda); ini_val(buf, "Hdb", m->hdb, sizeof m->hdb); ini_val(buf, "Hdd", m->hdd, sizeof m->hdd);
    ini_val(buf, "Cdrom", m->cdrom, sizeof m->cdrom); ini_val(buf, "Floppy", m->floppy, sizeof m->floppy); ini_val(buf, "FloppyB", m->floppyb, sizeof m->floppyb);
    if (ini_val(buf, "Boot", v, sizeof v) && v[0]) strlcpy(m->boot, v, sizeof m->boot);
    if (ini_val(buf, "Vga", v, sizeof v) && v[0]) strlcpy(m->vga, v, sizeof m->vga);
    if (ini_val(buf, "Sound", v, sizeof v)) m->sound = atoi(v);
    if (ini_val(buf, "Pcspk", v, sizeof v)) m->pcspk = atoi(v); else m->pcspk = 0;
    if (ini_val(buf, "UsbTablet", v, sizeof v)) m->tablet = atoi(v);
    if (ini_val(buf, "Net", v, sizeof v)) m->net = atoi(v);
    ini_val(buf, "Mac", m->mac, sizeof m->mac);
    if (m->net < 0 || m->net >= N_NET) m->net = 0;
    if (ini_val(buf, "LocalTime", v, sizeof v)) m->localtime = atoi(v);
    if (ini_val(buf, "Acpi", v, sizeof v)) m->acpi = atoi(v);
    if (ini_val(buf, "AutoStart", v, sizeof v)) m->autostart = atoi(v);
    { int vl; const char *x = ini_find(buf, "Extra", &vl);
      if (x && vl > 0 && (m->extra = kmalloc((uint32_t)vl + 1))) { memcpy(m->extra, x, vl); m->extra[vl] = 0; } }
    kfree(buf);
    if (m->memory < 4) m->memory = 4; if (m->memory > MAX_MEM_MB) m->memory = MAX_MEM_MB;
    if (m->cpus < 1) m->cpus = 1; if (m->cpus > MAX_CPUS) m->cpus = MAX_CPUS;
    return 0;
}
static int machine_save(struct machine *m)
{
    char *buf = kaprintf(
        "; QEMU DOS port machine (DOS95's QEMU Launcher reads it too)\r\n[Machine]\r\nName=%s\r\nMemory=%d\r\nCpus=%d\r\nCpu=%s\r\n"
        "Hda=%s\r\nHdb=%s\r\nHdd=%s\r\nCdrom=%s\r\nFloppy=%s\r\nFloppyB=%s\r\nBoot=%s\r\nVga=%s\r\nSound=%d\r\nPcspk=%d\r\nUsbTablet=%d\r\n"
        "Net=%d\r\nMac=%s\r\nLocalTime=%d\r\nAcpi=%d\r\nAutoStart=%d\r\nExtra=%s\r\n",
        m->name, m->memory, m->cpus, m->cpu, m->hda, m->hdb, m->hdd, m->cdrom, m->floppy, m->floppyb, m->boot, m->vga, m->sound, m->pcspk,
        m->tablet, m->net, m->mac, m->localtime, m->acpi, m->autostart, mextra(m));
    if (!buf) return -1;
    int r = fs_write_file(m->file, buf, strlen(buf)) < 0 ? -1 : 0;
    kfree(buf);
    return r;
}
static char *cat(char *s, const char *fmt, ...)
{
    if (!s) return NULL;
    va_list ap; va_start(ap, fmt);
    char one[1]; int n = kvsnprintf(one, 0, fmt, ap);
    va_end(ap);
    int l = strlen(s);
    char *t = kmalloc((uint32_t)l + (uint32_t)n + 1);
    if (!t) { kfree(s); return NULL; }
    memcpy(t, s, l); kfree(s);
    va_start(ap, fmt); kvsnprintf(t + l, (size_t)n + 1, fmt, ap); va_end(ap);
    return t;
}
static char *machine_cmdline(struct machine *m)
{
    /* (-tb-size: QEMU's translated code at 64 MB at most - by itself it takes a quarter of the machine's memory, 512 MB
     * of code and as much again in its blocks' table for 2047 MB, memory the machine itself does not have) */
    char *out = kaprintf("-m %d -tb-size %d", m->memory, m->memory / 4 < 64 ? (m->memory / 4 < 16 ? 16 : m->memory / 4) : 64);
    if (m->cpus > 1) out = cat(out, " -smp %d", m->cpus);
    if (m->cpu[0]) out = cat(out, " -cpu %s", m->cpu);
    if (m->hda[0]) out = cat(out, " -hda \"%s\"", m->hda);
    if (m->hdb[0]) out = cat(out, " -hdb \"%s\"", m->hdb);
    if (m->hdd[0]) out = cat(out, " -hdd \"%s\"", m->hdd);
    if (m->cdrom[0]) out = cat(out, " -cdrom \"%s\"", m->cdrom);
    else out = cat(out, " -drive if=ide,index=2,media=cdrom");        /* (an empty drive: a CD can be put in while it runs) */
    if (m->floppy[0]) out = cat(out, " -fda \"%s\"", m->floppy);
    if (m->floppyb[0]) out = cat(out, " -fdb \"%s\"", m->floppyb);
    out = cat(out, " -boot %s -vga %s", m->boot, m->vga);
    /* (a Sound Blaster 16 has its OPL3 FM synthesizer: adlib, qemu/hw/adlib-dos95.c - the DOS games' music) */
    if (m->sound || m->pcspk) out = cat(out, " -soundhw %s%s%s", m->sound ? "sb16,adlib" : "", m->sound && m->pcspk ? "," : "", m->pcspk ? "pcspk" : "");
    if (m->tablet) out = cat(out, " -usb -usbdevice tablet");
    if (!m->net) out = cat(out, " -net none");
    else {
        if (!m->mac[0]) new_mac(m->mac, sizeof m->mac);
        out = cat(out, " -net nic,model=%s,macaddr=%s -net %s", net_ids[m->net], m->mac, net_device() ? "dos95" : "none");
    }
    if (m->localtime) out = cat(out, " -localtime");
    if (!m->acpi) out = cat(out, " -no-acpi");
    if (m->extra && m->extra[0]) out = cat(out, " %s", m->extra);
    return out;
}

/* ---- before a start: the processor, the memory, the images ---------------------------------------------------- */
static int image_ok(struct window *w, const char *what, const char *path)
{
    if (!path[0] || fs_exists(path)) return 1;
    char *msg = kaprintf("The %s image %s is not there.\n\nChange it in Machine > Settings.", what, path);
    msgbox(w, QDP_NAME, msg ? msg : "An image of the machine is not there.", MB_OK | MB_ICONERROR);
    kfree(msg);
    return 0;
}
static int host_check(struct window *w, struct machine *m)
{
    if (cpu_level < 5) {
        msgbox(w, QDP_NAME, "QEMU needs a Pentium-class processor or better.\n\nThis computer's is a 386 or 486, which cannot run its translator.", MB_OK | MB_ICONERROR);
        return 0;
    }
    uint32_t need = (uint32_t)m->memory * 256 + QEMU_OVERHEAD_MB * 256;   /* the guest's memory + QEMU's own (its translated code, video) */
    if (mm_free_pages() < need) {
        char *msg = kaprintf("Not enough free memory to start \"%s\".\n\nIt needs about %u MB (%d MB for the machine and %d MB for QEMU); %u MB is free.\n"
                             "Give the machine less memory in Machine > Settings.", m->name, need / 256, m->memory, QEMU_OVERHEAD_MB, mm_free_pages() / 256);
        msgbox(w, QDP_NAME, msg ? msg : "Not enough free memory to start the machine.", MB_OK | MB_ICONERROR);
        kfree(msg);
        return 0;
    }
    if (!m->hda[0] && !m->hdb[0] && !m->hdd[0] && !m->cdrom[0] && !m->floppy[0]) {
        msgbox(w, QDP_NAME, "This machine has no disk, CD-ROM or floppy image to start from.\n\nGive it one in Machine > Settings.", MB_OK | MB_ICONWARN);
        return 0;
    }
    return image_ok(w, "hard disk", m->hda) && image_ok(w, "second hard disk", m->hdb) && image_ok(w, "third hard disk", m->hdd) &&
           image_ok(w, "CD-ROM", m->cdrom) && image_ok(w, "floppy", m->floppy) && image_ok(w, "second floppy", m->floppyb);
}

/* ---- the autoboot (AUTOBOOT.INI) -------------------------------------------------------------------------------- */
static struct autoboot { char machine[260]; int delay, when_off; } ab = { "", 5, 0 };
static const char *const when_off_names[] = { "Show the QEMU manager", "Start the machine again", "Exit to MS-DOS", "Restart the computer" };
static void ab_path(char *out, int n) { path_join(out, n, qdp_dir(), "AUTOBOOT.INI"); }
static void ab_load(void)
{
    char p[200]; ab_path(p, sizeof p);
    int n; char *buf = kread_text(p, &n);
    ab.machine[0] = 0; ab.delay = 5; ab.when_off = 0;
    if (!buf) return;
    char v[260];
    ini_val(buf, "Machine", ab.machine, sizeof ab.machine);
    if (ini_val(buf, "Delay", v, sizeof v)) ab.delay = atoi(v);
    if (ini_val(buf, "WhenOff", v, sizeof v)) ab.when_off = atoi(v);
    if (ab.delay < 0) ab.delay = 0; if (ab.delay > 600) ab.delay = 600;
    if (ab.when_off < 0 || ab.when_off > 3) ab.when_off = 0;
    kfree(buf);
}
static int ab_save(void)
{
    char p[200]; ab_path(p, sizeof p);
    char *buf = kaprintf("; QEMU DOS port - the machine started when it starts (QEMUDOS /NOAUTO skips it)\r\n"
                         "; Machine= its .VM file (none: the manager)   Delay= seconds to wait, Esc stops it\r\n"
                         "; WhenOff= when it shuts down: 0 the manager, 1 start it again, 2 exit to MS-DOS, 3 restart the computer\r\n"
                         "[Autoboot]\r\nMachine=%s\r\nDelay=%d\r\nWhenOff=%d\r\n", ab.machine, ab.delay, ab.when_off);
    if (!buf) return -1;
    int r = fs_write_file(p, buf, strlen(buf)) < 0 ? -1 : 0;
    kfree(buf);
    return r;
}

/* ---- the manager's window -------------------------------------------------------------------------------------- */
enum { M_NEW = 300, M_OPEN, M_SAVEAS, M_REMOVE, M_NEWDISK, M_AUTOBOOT, M_RESTART, M_EXIT,
       M_START, M_SHOW, M_PAUSE, M_RESET, M_POWERBTN, M_POWEROFF, M_CAD, M_SETTINGS,
       M_CD_CHANGE, M_CD_EJECT, M_FDA_CHANGE, M_FDA_EJECT, M_FDB_CHANGE, M_FDB_EJECT,
       M_FIT, M_STRETCH, M_ONE, M_REFRESH, M_KEYS, M_ABOUT, M_SCREEN,
       C_LIST = 1, C_INFO, B_NEW, B_SETTINGS, B_START, B_SHOW, B_PAUSE, B_RESET, B_OFF, B_DISK, B_AUTO };

struct manager {
    struct window *w;
    struct control *lv, *info, *btn[9];
    struct menu *mfile, *mmachine, *mmedia, *mview, *mhelp;
    struct menubar_item bar[5];
    struct machine *list; int n, cap;
    uint32_t *thumb; int tw, th;           /* the running machine's picture, small */
    int last_state;
};
static struct manager *man;
static struct app qdpman_app;

struct window *qdpman_window(void) { return man ? man->w : NULL; }

static struct machine *selected(void)
{
    if (!man) return NULL;
    int s = man->lv->sel; return (s >= 0 && s < man->n) ? &man->list[s] : NULL;
}
static struct machine *running_machine(void)
{
    if (!man || vpc_state() == VPC_NONE) return NULL;
    const char *f = vpc_machine_file();
    for (int i = 0; i < man->n; i++) if (!stricmp(man->list[i].file, f)) return &man->list[i];
    return NULL;
}
static int is_running(const struct machine *m) { return m && vpc_state() != VPC_NONE && !stricmp(m->file, vpc_machine_file()); }
static const char *state_text(const struct machine *m)
{
    if (!is_running(m)) return m && m->autostart ? "Off - starts by itself" : "Off";
    switch (vpc_state()) {
    case VPC_STARTING: return "Starting";
    case VPC_PAUSED: return "Paused";
    case VPC_STOPPING: return "Shutting down";
    default: return "Running";
    }
}
static void list_label(const struct machine *m, char *out, int n)
{
    if (is_running(m)) ksnprintf(out, n, "%s  [%s]", m->name, state_text(m));
    else strlcpy(out, m->name, n);
}
static void update_buttons(void)
{
    if (!man) return;
    struct machine *m = selected();
    int st = vpc_state(), run = is_running(m), any = st != VPC_NONE;
    ctl_set_enabled(man->btn[1], m != NULL);
    ctl_set_enabled(man->btn[2], m != NULL && !run);
    ctl_set_enabled(man->btn[3], any);
    ctl_set_enabled(man->btn[4], any && st != VPC_STOPPING);
    ctl_set_text(man->btn[4], st == VPC_PAUSED ? "&Resume" : "&Pause");
    ctl_set_enabled(man->btn[5], any && st != VPC_STOPPING);
    ctl_set_enabled(man->btn[6], any && st != VPC_STOPPING);
    for (int i = 0; i < man->n; i++) {
        char t[300]; list_label(&man->list[i], t, sizeof t);
        if (strcmp(man->lv->items[i].text, t)) strlcpy(man->lv->items[i].text, t, sizeof man->lv->items[i].text);
    }
    ctl_invalidate(man->lv);
    ctl_invalidate(man->info);
    char st_line[200];
    struct net_status ns; net_get_status(&ns);
    ksnprintf(st_line, sizeof st_line, "%s engine - %u MB free - %d processor%s - network: %s%s%s", QDP_ENGINE, mm_free_pages() / 256, ncpus, ncpus == 1 ? "" : "s",
              ns.have_nic ? ns.nic : "none", ns.have_nic && ns.bound ? ", " : "", ns.have_nic && ns.bound ? ns.ip_s : "");
    wm_set_status(man->w, st_line);
}
static void refresh_list(void)
{
    if (!man) return;
    char keep[260] = ""; struct machine *s = selected(); if (s) strlcpy(keep, s->file, sizeof keep);
    ctl_lv_clear(man->lv);
    for (int i = 0; i < man->n; i++) machine_free(&man->list[i]);
    man->n = 0;
    fs_mkdir(mdir());
    char pat[220]; path_join(pat, sizeof pat, mdir(), "*.VM");
    struct fs_dirent e; struct fs_find fh;
    if (fs_findfirst(pat, 0x27, &e, &fh) == 0) {
        do {
            if (e.attr & FA_DIR) continue;
            if (man->n >= man->cap) { int nc = man->cap ? man->cap * 2 : 16; struct machine *nl = kmalloc(nc * sizeof *nl); if (!nl) break; if (man->list) { memcpy(nl, man->list, man->n * sizeof *nl); kfree(man->list); } man->list = nl; man->cap = nc; }
            char p[260]; path_join(p, sizeof p, mdir(), e.name);
            if (machine_load(&man->list[man->n], p) == 0) { char t[300]; list_label(&man->list[man->n], t, sizeof t); ctl_lv_add(man->lv, t, ICO_COMPUTER, NULL); man->n++; }
        } while (fs_findnext(&fh, &e) == 0);
        fs_findclose(&fh);
    }
    int sel = man->n ? 0 : -1;
    for (int i = 0; i < man->n; i++) if (keep[0] && !stricmp(man->list[i].file, keep)) sel = i;
    if (sel >= 0) ctl_lv_select(man->lv, sel, 1);
    update_buttons();
}
static int find_machine(const char *file)
{
    for (int i = 0; man && i < man->n; i++) if (!stricmp(man->list[i].file, file)) return i;
    return -1;
}

/* ---- the info panel: the machine's name, its state, its picture while it runs, its parts ------------------------- */
static int info_paint(struct control *c)
{
    struct window *w = c->win;
    int x = w->cx + c->r.x, y = w->cy + c->r.y;
    draw_sunken_field(x, y, c->r.w, c->r.h);
    gfx_fill(x + 2, y + 2, c->r.w - 4, c->r.h - 4, C_WINDOW);
    struct machine *m = selected();
    int tx = x + 16, ty = y + 14, cw = c->r.w - 32;
    if (!m) {
        qdp_draw_big_text(tx, ty, 2, C_WINTEXT, QDP_NAME);
        gfx_text_wrap(ui_font, tx, ty + bmf_height(&font_ui_bold) * 2 + 16, cw, C_WINTEXT,
            "There is no machine yet.\n\nMake one with New: give it memory, a hard disk image (New Disk makes an empty one of any size) and the CD "
            "or floppy image of the system to install, then press Start.  It runs over the whole screen; Ctrl+Alt+Home brings this window back.\n\n"
            QDP_ENGINE " runs DOS, Windows 3.1, 95, 98, ME, NT 4, 2000 and XP and Linux of their time on this computer's processor with its translator.",
            ui_font->height + 3);
        return 1;
    }
    gfx_icon(ICO_COMPUTER, 32, tx, ty);
    gfx_text(ui_font_bold, tx + 44, ty + 2, C_WINTEXT, m->name);
    const char *st = state_text(m);
    uint32_t stc = is_running(m) ? (vpc_state() == VPC_PAUSED ? 0x806000 : 0x008000) : 0x606060;
    gfx_text(ui_font, tx + 44, ty + 18, stc, st);
    ty += 48;
    /* the picture of the machine running: as large as half the panel's width, its shape kept */
    if (is_running(m) && man) {
        int sw, sh; vpc_screen_size(&sw, &sh);
        int pw = MIN(cw, MAX(160, cw / 2)), ph = sw > 0 ? pw * sh / sw : pw * 3 / 4;
        if (ph > c->r.h / 2) { ph = c->r.h / 2; pw = sh > 0 ? ph * sw / sh : ph * 4 / 3; }
        if (pw > 0 && ph > 0) {
            if (man->tw != pw || man->th != ph) { kfree(man->thumb); man->thumb = kmalloc((uint32_t)pw * ph * 4); man->tw = man->thumb ? pw : 0; man->th = man->thumb ? ph : 0; }
            draw_sunken_field(tx - 2, ty - 2, pw + 4, ph + 4);
            if (man->thumb && vpc_thumbnail(man->thumb, pw, ph)) gfx_blit(tx, ty, pw, ph, man->thumb, pw);
            else gfx_fill(tx, ty, pw, ph, C_BLACK);
            gfx_text(&font_small, tx, ty + ph + 6, 0x404040, "Show (Ctrl+Alt+Home) gives it the screen and the keyboard.");
            ty += ph + 6 + font_small.height + 12;
        }
    }
    char mem[32], cpu[96], boot[64], vga[96], netd[96], snd[64], disk[3][300];
    ksnprintf(mem, sizeof mem, "%d MB", m->memory);
    { int i; for (i = 0; i < N_CPU; i++) if (!stricmp(cpu_ids[i], m->cpu)) break; ksnprintf(cpu, sizeof cpu, "%s%s%s", m->cpus > 1 ? "" : "", i < N_CPU ? cpu_names[i] : m->cpu, ""); }
    if (m->cpus > 1) { char t[96]; ksnprintf(t, sizeof t, "%d x %s", m->cpus, cpu); strlcpy(cpu, t, sizeof cpu); }
    { int i; for (i = 0; i < N_BOOT; i++) if (!stricmp(boot_ids[i], m->boot)) break; strlcpy(boot, i < N_BOOT ? boot_names[i] : m->boot, sizeof boot); }
    { int i; for (i = 0; i < N_VGA; i++) if (!stricmp(vga_ids[i], m->vga)) break; strlcpy(vga, i < N_VGA ? vga_names[i] : m->vga, sizeof vga); }
    { const char *nn = net_names[m->net]; const char *dash = strstr(nn, " - "); int l = dash ? (int)(dash - nn) : (int)strlen(nn); if (l > 80) l = 80; memcpy(netd, nn, l); netd[l] = 0;
      if (m->net) strlcat(netd, net_device() ? " (bridged to this computer's network)" : " (this computer has no network)", sizeof netd); }
    ksnprintf(snd, sizeof snd, "%s%s%s", m->sound ? "Sound Blaster 16 with OPL3 FM" : "", m->sound && m->pcspk ? ", " : "", m->pcspk ? "PC speaker" : "");
    if (!snd[0]) strcpy(snd, "(none)");
    const char *hd[3] = { m->hda, m->hdb, m->hdd };
    for (int k = 0; k < 3; k++) strlcpy(disk[k], hd[k][0] ? hd[k] : "(none)", sizeof disk[k]);
    const char *rows[][2] = {
        { "Memory", mem }, { "Processor", cpu }, { "Hard disk", disk[0] }, { "Second disk", disk[1] }, { "Third disk", disk[2] },
        { "CD-ROM", m->cdrom[0] ? m->cdrom : "(empty drive)" }, { "Floppy A:", m->floppy[0] ? m->floppy : "(none)" }, { "Floppy B:", m->floppyb[0] ? m->floppyb : "(none)" },
        { "Starts from", boot }, { "Video", vga }, { "Sound", snd }, { "Mouse", m->tablet ? "USB tablet (absolute: no grab)" : "PS/2 (a click gives it the mouse, Ctrl+Alt takes it back)" },
        { "Network", netd }, { "Starts by itself", m->autostart ? "yes - when the QEMU DOS port starts" : "no" }, { "File", m->file } };
    int lw = 110;
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (ty + ui_font->height > y + c->r.h - 6) break;
        gfx_text(ui_font_bold, tx, ty, C_WINTEXT, rows[i][0]);
        gfx_text_clip(ui_font, tx + lw, ty, cw - lw, C_WINTEXT, rows[i][1]);
        ty += ui_font->height + 5;
    }
    return 1;
}

/* ---- the machine's settings ------------------------------------------------------------------------------------ */
enum { S_TABS = 90, S_NAME = 100, S_MEM, S_MEMLBL, S_CPUS, S_CPU, S_BOOT, S_AUTO,
       S_HDA, S_HDA_B, S_HDA_N, S_HDB, S_HDB_B, S_HDB_N, S_HDD, S_HDD_B, S_HDD_N, S_CD, S_CD_B, S_FD, S_FD_B, S_FDB, S_FDB_B,
       S_VGA, S_SOUND, S_PCSPK, S_TABLET, S_NET, S_MAC, S_MAC_NEW, S_LOCALTIME, S_ACPI, S_EXTRA };
static int create_disk_image(struct window *owner, char *path, int n);
static int settings_proc(struct window *d, struct wevent *e)
{
    if (e->type == WE_COMMAND) {
        int id = e->id;
        if (id == S_HDA_B || id == S_HDB_B || id == S_HDD_B || id == S_CD_B || id == S_FD_B || id == S_FDB_B) {
            int t = id - 1;
            const char *filter = t == S_CD ? "CD-ROM images (*.iso)|*.iso|All Files (*.*)|*.*" : (t == S_FD || t == S_FDB) ? "Floppy images (*.img;*.ima;*.vfd)|*.img;*.ima;*.vfd|All Files (*.*)|*.*"
                                 : "Disk images (*.img;*.qcow2;*.qcow;*.vmdk;*.vhd;*.vdi)|*.img;*.qcow2;*.qcow;*.vmdk;*.vhd;*.vdi|All Files (*.*)|*.*";
            struct control *c = ctl_find(d, t); char p[260]; strlcpy(p, c->text, sizeof p);
            if (!p[0]) strlcpy(p, qdp_dir(), sizeof p);
            if (file_dialog(d, "Select Image", 0, filter, p, sizeof p)) ctl_set_text(c, p);
            return 1;
        }
        if (id == S_HDA_N || id == S_HDB_N || id == S_HDD_N) {
            char p[260] = ""; if (create_disk_image(d, p, sizeof p)) ctl_set_text(ctl_find(d, id - 2), p);
            return 1;
        }
        if (id == S_MAC_NEW) { char t[24]; new_mac(t, sizeof t); ctl_set_text(ctl_find(d, S_MAC), t); return 1; }
    }
    if (e->type == WE_NOTIFY && e->id == S_MEM && e->notify == NT_CHANGE) {
        char t[32]; ksnprintf(t, sizeof t, "%d MB", ((struct control*)e->ptr)->value); ctl_set_text(ctl_find(d, S_MEMLBL), t); return 1;
    }
    return dialog_std_proc(d, e);
}
static void combo_fill(struct control *cb, const char *const *names, const char *const *ids, int n, const char *cur)
{
    cb->sel = -1;
    for (int i = 0; i < n; i++) { ctl_combo_add(cb, names[i]); if (!stricmp(ids[i], cur)) cb->sel = i; }
    if (cb->sel < 0) { ctl_combo_add(cb, cur); cb->sel = n; }     /* (a value the list has not: kept as one more entry) */
}
static int settings_dialog(struct window *owner, struct machine *m)
{
    int W = 480, H = 410;
    struct window *d = wm_create("Machine Settings", 0, 0, W, H, WF_DLG | WF_MODAL, settings_proc, m);
    d->owner = owner; if (owner) owner->enabled = 0; wm_center(d, owner);
    struct control *tabs = ctl_add(d, CT_TABS, S_TABS, 8, 8, W - 16, H - 90, "", 0);
    const char *tn[5] = { "General", "Disks", "Display/Sound", "Network", "Advanced" };
    ctl_tabs_set(tabs, tn, 5);
    struct control *c;
    int R = W - 40;
#define PAGE(g, ctl) do { c = (ctl); c->group = (g); } while (0)
    /* General */
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 44, 80, 16, "&Name:", 0));
    PAGE(1, ctl_add(d, CT_EDIT, S_NAME, 110, 40, R - 110, 21, m->name, 0)); c->maxlen = 255;
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 76, 80, 16, "&Memory:", 0));
    { int maxmb = MAX_MEM_MB, freemb = (int)(mm_free_pages() / 256) - QEMU_OVERHEAD_MB; if (freemb < maxmb && freemb > 64) maxmb = freemb; if (m->memory > maxmb) maxmb = m->memory;
      PAGE(1, ctl_add(d, CT_SLIDER, S_MEM, 110, 70, R - 190, 28, "", 0)); c->min = 4; c->max = maxmb; c->value = m->memory; c->page = 64; }
    { char t[32]; ksnprintf(t, sizeof t, "%d MB", m->memory); PAGE(1, ctl_add(d, CT_LABEL, S_MEMLBL, R - 72, 76, 72, 16, t, 0)); }
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 108, 90, 16, "&Processors:", 0));
    PAGE(1, ctl_add(d, CT_COMBO, S_CPUS, 110, 104, 60, 21, "", CS_DROPLIST));
    { struct control *cb = c; for (int i = 1; i <= MAX_CPUS; i++) { char t[8]; ksnprintf(t, sizeof t, "%d", i); ctl_combo_add(cb, t); } cb->sel = m->cpus - 1; }
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 140, 90, 16, "Processor &type:", 0));
    PAGE(1, ctl_add(d, CT_COMBO, S_CPU, 110, 136, R - 110, 21, "", CS_DROPLIST)); combo_fill(c, cpu_names, cpu_ids, N_CPU, m->cpu);
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 172, 90, 16, "&Starts from:", 0));
    PAGE(1, ctl_add(d, CT_COMBO, S_BOOT, 110, 168, R - 110, 21, "", CS_DROPLIST)); combo_fill(c, boot_names, boot_ids, N_BOOT, m->boot);
    PAGE(1, ctl_add(d, CT_CHECK, S_AUTO, 20, 206, R - 20, 16, "Start this machine by &itself when the QEMU DOS port starts", 0)); c->checked = m->autostart;
    PAGE(1, ctl_add(d, CT_LABEL, 0, 20, 232, R - 20, 60, "The machine is an Intel 440FX PC: PS/2 keyboard and mouse, IDE disks and CD-ROM, floppies, PCI video and sound. "
                    "Up to 2047 MB of memory (QEMU 0.14's most on this 32-bit system) and 8 processors, which its translator runs in turn.", 0));
    /* Disks */
    const struct { int id; const char *label; const char *val; int create; } dk[] = {
        { S_HDA, "Hard disk (IDE primary &master):", m->hda, 1 }, { S_HDB, "Second hard disk (IDE primary s&lave):", m->hdb, 1 },
        { S_HDD, "Third hard disk (IDE secondary slave):", m->hdd, 1 }, { S_CD, "&CD-ROM (IDE secondary master) - an ISO image, or empty:", m->cdrom, 0 },
        { S_FD, "Floppy &A:", m->floppy, 0 }, { S_FDB, "Floppy &B:", m->floppyb, 0 } };
    for (int i = 0; i < 6; i++) {
        int yy = 40 + i * 44;
        PAGE(2, ctl_add(d, CT_LABEL, 0, 20, yy, R - 20, 16, dk[i].label, 0));
        PAGE(2, ctl_add(d, CT_EDIT, dk[i].id, 20, yy + 17, dk[i].create ? R - 160 : R - 90, 21, dk[i].val, 0)); c->maxlen = 255;
        PAGE(2, ctl_add(d, CT_BUTTON, dk[i].id + 1, dk[i].create ? R - 134 : R - 64, yy + 16, 64, 23, "Browse", 0));
        if (dk[i].create) PAGE(2, ctl_add(d, CT_BUTTON, dk[i].id + 2, R - 64, yy + 16, 64, 23, "New...", 0));
    }
    /* Display / Sound */
    PAGE(3, ctl_add(d, CT_LABEL, 0, 20, 44, 200, 16, "&Video card:", 0));
    PAGE(3, ctl_add(d, CT_COMBO, S_VGA, 20, 62, R - 20, 21, "", CS_DROPLIST)); combo_fill(c, vga_names, vga_ids, N_VGA, m->vga);
    PAGE(3, ctl_add(d, CT_LABEL, 0, 20, 90, R - 20, 32, "Cirrus: up to 1600 x 1200 in 16, 24 and 32 bits.  The machine's screen is fitted to this one's (Ctrl+Alt+S: stretched, pixel for pixel).", 0));
    PAGE(3, ctl_add(d, CT_CHECK, S_SOUND, 20, 134, R - 20, 16, "Sound &Blaster 16 with its OPL3 FM synthesizer", 0)); c->checked = m->sound;
    PAGE(3, ctl_add(d, CT_CHECK, S_PCSPK, 20, 158, R - 20, 16, "&PC speaker (the beeps and tunes of DOS games)", 0)); c->checked = m->pcspk;
    PAGE(3, ctl_add(d, CT_CHECK, S_TABLET, 20, 182, R - 20, 16, "&USB tablet: an absolute mouse (Windows 98 and later, Linux)", 0)); c->checked = m->tablet;
    PAGE(3, ctl_add(d, CT_LABEL, 0, 20, 210, R - 20, 64, "The machine's sound comes out of this computer's sound card.  The tablet's mouse is never grabbed; without it the machine has a PS/2 mouse: a click on its screen gives it the mouse, Ctrl+Alt takes it back.", 0));
    /* Network */
    PAGE(4, ctl_add(d, CT_LABEL, 0, 20, 44, R - 20, 16, "Network &card (bridged: the machine has an address of its own):", 0));
    PAGE(4, ctl_add(d, CT_COMBO, S_NET, 20, 62, R - 20, 21, "", CS_DROPLIST));
    { struct control *cb = c; for (int i = 0; i < N_NET; i++) ctl_combo_add(cb, net_names[i]); cb->sel = m->net; }
    PAGE(4, ctl_add(d, CT_LABEL, 0, 20, 100, 100, 16, "&MAC address:", 0));
    PAGE(4, ctl_add(d, CT_EDIT, S_MAC, 120, 96, 150, 21, m->mac, 0)); c->maxlen = 17;
    PAGE(4, ctl_add(d, CT_BUTTON, S_MAC_NEW, 278, 95, 80, 23, "New", 0));
    PAGE(4, ctl_add(d, CT_LABEL, 0, 20, 134, R - 20, 64, net_device() ? "The machine is on this computer's network as a computer of its own: it asks the network's DHCP server for its address."
                                                                      : "This computer has no network card the QEMU DOS port drives: the machine's card sees no network.", 0));
    /* Advanced */
    PAGE(5, ctl_add(d, CT_CHECK, S_LOCALTIME, 20, 44, R - 20, 16, "The machine's clock in &local time (DOS, Windows) - not UTC (Linux)", 0)); c->checked = m->localtime;
    PAGE(5, ctl_add(d, CT_CHECK, S_ACPI, 20, 68, R - 20, 16, "&ACPI (off for Windows 95 and very old Linux)", 0)); c->checked = m->acpi;
    PAGE(5, ctl_add(d, CT_LABEL, 0, 20, 100, R - 20, 16, "E&xtra QEMU command-line options:", 0));
    PAGE(5, ctl_add(d, CT_EDIT, S_EXTRA, 20, 118, R - 20, 21, mextra(m), 0)); c->maxlen = 255;
    char extra0[256]; strlcpy(extra0, c->text, sizeof extra0);
    PAGE(5, ctl_add(d, CT_LABEL, 0, 20, 148, R - 20, 60, "Given to " QDP_ENGINE " as typed, for example:  -k de  (the machine's keyboard layout)   -rtc base=1999-12-31   -hdc image.img", 0));
#undef PAGE
    for (struct control *x = d->controls; x; x = x->next) if (x->group) ctl_set_visible(x, x->group == 1);
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 81, H - 24 - 34, 75, 23, "OK", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 81, H - 24 - 34, 75, 23, "Cancel", 0);
    d->focus = ctl_find(d, S_NAME); ctl_edit_select_all(d->focus);
    wm_show(d, 1);
    wm_run_modal(d);
    int r = d->result == ID_OK;
    if (r) {
        strlcpy(m->name, ctl_find(d, S_NAME)->text, sizeof m->name); if (!m->name[0]) strcpy(m->name, "Machine");
        m->memory = ctl_find(d, S_MEM)->value;
        m->cpus = ctl_find(d, S_CPUS)->sel + 1; if (m->cpus < 1) m->cpus = 1;
        int s = ctl_find(d, S_CPU)->sel; if (s >= 0 && s < N_CPU) strlcpy(m->cpu, cpu_ids[s], sizeof m->cpu);
        s = ctl_find(d, S_BOOT)->sel; if (s >= 0 && s < N_BOOT) strlcpy(m->boot, boot_ids[s], sizeof m->boot);
        m->autostart = ctl_find(d, S_AUTO)->checked;
        strlcpy(m->hda, ctl_find(d, S_HDA)->text, sizeof m->hda); strlcpy(m->hdb, ctl_find(d, S_HDB)->text, sizeof m->hdb); strlcpy(m->hdd, ctl_find(d, S_HDD)->text, sizeof m->hdd);
        strlcpy(m->cdrom, ctl_find(d, S_CD)->text, sizeof m->cdrom); strlcpy(m->floppy, ctl_find(d, S_FD)->text, sizeof m->floppy); strlcpy(m->floppyb, ctl_find(d, S_FDB)->text, sizeof m->floppyb);
        s = ctl_find(d, S_VGA)->sel; if (s >= 0 && s < N_VGA) strlcpy(m->vga, vga_ids[s], sizeof m->vga);
        m->sound = ctl_find(d, S_SOUND)->checked; m->pcspk = ctl_find(d, S_PCSPK)->checked; m->tablet = ctl_find(d, S_TABLET)->checked;
        { int ns = ctl_find(d, S_NET)->sel; m->net = ns >= 0 && ns < N_NET ? ns : 0; }
        strlcpy(m->mac, ctl_find(d, S_MAC)->text, sizeof m->mac);
        m->localtime = ctl_find(d, S_LOCALTIME)->checked; m->acpi = ctl_find(d, S_ACPI)->checked;
        const char *x = ctl_find(d, S_EXTRA)->text;
        if (strcmp(x, extra0)) { char *nx = x[0] ? kstrdup(x) : NULL; if (nx || !x[0]) { kfree(m->extra); m->extra = nx; } }
    }
    wm_destroy(d);
    if (owner) { owner->enabled = 1; wm_activate(owner); }
    return r;
}

/* ---- a new disk image: one file of a fixed size (to 4095 MB, FAT32's largest file), or a disk that grows as it is
 * written, of any size to 2 TB (pieces of 512 MB beside a small file that names them - qemu/dos95/raw-dos95.c) ------- */
enum { D_SIZE = 101, D_UNIT, D_PATH, D_BROWSE, D_KIND };
static int newdisk_proc(struct window *d, struct wevent *e)
{
    if (e->type == WE_COMMAND && e->id == D_BROWSE) {
        char p[260]; strlcpy(p, ctl_find(d, D_PATH)->text, sizeof p);
        if (file_dialog(d, "New Disk Image", 1, "Disk images (*.img)|*.img|All Files (*.*)|*.*", p, sizeof p)) ctl_set_text(ctl_find(d, D_PATH), p);
        return 1;
    }
    return dialog_std_proc(d, e);
}
#define SPLIT_PIECE (512u * 1024 * 1024)
static int write_split_descriptor(const char *path, uint64_t bytes)
{
    char num[24];                                         /* (the size in decimal - kvsnprintf has no 64-bit conversion) */
    if (bytes >= 1000000000u) ksnprintf(num, sizeof num, "%u%09u", (uint32_t)(bytes / 1000000000u), (uint32_t)(bytes % 1000000000u));
    else ksnprintf(num, sizeof num, "%u", (uint32_t)bytes);
    char *buf = kaprintf("QEMU DOS port split disk image\r\nsize=%s\r\npiece=%u\r\n"
                         "; the disk's bytes are in the files beside this one named as it is with the extensions .000, .001 ... (hex), each\r\n"
                         "; %u MB - a piece not there yet reads as zeros, and is made when the machine first writes to it\r\n",
                         num, SPLIT_PIECE, SPLIT_PIECE >> 20);
    if (!buf) return -1;
    int r = fs_write_file(path, buf, strlen(buf)) < 0 ? -1 : 0;
    kfree(buf);
    return r;
}
static int create_disk_image(struct window *owner, char *path, int n)
{
    int W = 440, H = 270;
    struct window *d = wm_create("New Disk Image", 0, 0, W, H, WF_DLG | WF_MODAL, newdisk_proc, NULL);
    d->owner = owner; if (owner) owner->enabled = 0; wm_center(d, owner);
    char def[260]; path_join(def, sizeof def, qdp_dir(), "DISKS\\DISK.IMG");
    ctl_add(d, CT_LABEL, 0, 12, 12, 200, 16, "Image &file:", 0);
    struct control *pe = ctl_add(d, CT_EDIT, D_PATH, 12, 30, W - 110, 21, path[0] ? path : def, 0); pe->maxlen = 255;
    ctl_add(d, CT_BUTTON, D_BROWSE, W - 90, 29, 76, 23, "Browse", 0);
    ctl_add(d, CT_LABEL, 0, 12, 64, 100, 16, "&Size:", 0);
    struct control *se = ctl_add(d, CT_EDIT, D_SIZE, 12, 82, 90, 21, "8", CS_NUMBER); se->maxlen = 7;
    struct control *un = ctl_add(d, CT_COMBO, D_UNIT, 108, 82, 60, 21, "", CS_DROPLIST); ctl_combo_add(un, "GB"); ctl_combo_add(un, "MB"); un->sel = 0;
    ctl_add(d, CT_LABEL, 0, 12, 116, 100, 16, "&Kind:", 0);
    struct control *kd = ctl_add(d, CT_COMBO, D_KIND, 12, 134, W - 26, 21, "", CS_DROPLIST);
    ctl_combo_add(kd, "Grows as it is written - any size up to 2 TB (pieces of 512 MB)");
    ctl_combo_add(kd, "One file of a fixed size, written now - up to 4095 MB");
    kd->sel = 0;
    ctl_add(d, CT_LABEL, 0, 12, 166, W - 26, 48, "A disk that grows takes only the room the machine has written (in steps of 512 MB) and is made at once; "
            "the fixed one is the size asked at once, every byte zero, the plain .img any QEMU or emulator reads.", 0);
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 81, H - 24 - 34, 75, 23, "Create", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 81, H - 24 - 34, 75, 23, "Cancel", 0);
    d->focus = se; ctl_edit_select_all(se);
    wm_show(d, 1);
    int ok = 0;
    for (;;) {
        wm_run_modal(d);
        if (d->result != ID_OK) break;
        d->result = 0;
        uint32_t num = (uint32_t)atoi(ctl_find(d, D_SIZE)->text);
        int gb = ctl_find(d, D_UNIT)->sel == 0, grows = ctl_find(d, D_KIND)->sel == 0;
        uint64_t mb = gb ? (uint64_t)num * 1024 : num;
        const char *p = ctl_find(d, D_PATH)->text;
        if (!p[0]) { msgbox(d, "New Disk Image", "Give the image a file name.", MB_OK | MB_ICONWARN); continue; }
        if (mb < 1 || mb > (grows ? 2u * 1024 * 1024 : 4095u)) {
            msgbox(d, "New Disk Image", grows ? "A disk that grows may be 1 MB to 2 TB (2048 GB)." : "A fixed disk may be 1 to 4095 MB (FAT32's largest file).\nFor a bigger one take the kind that grows.", MB_OK | MB_ICONWARN);
            continue;
        }
        if (fs_getattr(p) >= 0 && msgbox(d, "New Disk Image", "There is a file of that name. Replace it?", MB_YESNO | MB_ICONQUESTION) != ID_YES) continue;
        { char dir[260]; strlcpy(dir, p, sizeof dir); char *bs = strrchr(dir, '\\'); if (bs && bs > dir + 2) { *bs = 0; fs_mkdir(dir); } }
        if (grows) {
            /* (the pieces of a disk of that name made before go: a new disk reads as zeros) */
            char base[260]; strlcpy(base, p, sizeof base); char *dot = strrchr(base, '.'), *bs = strrchr(base, '\\'); if (dot && (!bs || dot > bs)) *dot = 0;
            for (int k = 0; k < 4096; k++) { char q[270]; ksnprintf(q, sizeof q, "%s.%03X", base, k); if (fs_unlink(q) < 0 && k > 8) { if (fs_getattr(q) < 0) break; } }
            if (write_split_descriptor(p, mb * 1024 * 1024) < 0) { msgbox(d, "New Disk Image", "The image could not be written.", MB_OK | MB_ICONERROR); continue; }
            strlcpy(path, p, n); ok = 1; break;
        }
        int h = fs_create(p);
        if (h < 0) { msgbox(d, "New Disk Image", "The file could not be made.", MB_OK | MB_ICONERROR); continue; }
        struct window *pg = progress_dialog("New Disk Image", "Writing the image...");
        static uint8_t zero[65536]; int failed = 0; uint32_t total = (uint32_t)mb * 16;
        for (uint32_t i = 0; i < total; i++) {
            if (fs_write(h, zero, sizeof zero) != (int)sizeof zero) { failed = 1; break; }
            if ((i & 15) == 0) { char t[64]; ksnprintf(t, sizeof t, "Writing the image... %u of %u MB", i / 16, (uint32_t)mb); if (progress_update(pg, t, (int)((uint64_t)i * 100 / total))) { failed = 2; break; } }
        }
        fs_close(h);
        progress_close(pg);
        if (failed) { fs_unlink(p); if (failed == 1) msgbox(d, "New Disk Image", "The disk is full, or the file could not be written.", MB_OK | MB_ICONERROR); continue; }
        strlcpy(path, p, n); ok = 1; break;
    }
    wm_destroy(d);
    if (owner) { owner->enabled = 1; wm_activate(owner); }
    return ok;
}

/* ---- the autoboot dialog ---------------------------------------------------------------------------------------- */
enum { A_MACHINE = 101, A_DELAY, A_WHEN };
static void autoboot_dialog(struct window *owner)
{
    ab_load();
    int W = 440, H = 250;
    struct window *d = wm_create("Autoboot", 0, 0, W, H, WF_DLG | WF_MODAL, dialog_std_proc, NULL);
    d->owner = owner; if (owner) owner->enabled = 0; wm_center(d, owner);
    ctl_add(d, CT_LABEL, 0, 12, 12, W - 24, 32, "The machine started when the QEMU DOS port starts - after a countdown, which Esc or a click stops (the manager then).", 0);
    ctl_add(d, CT_LABEL, 0, 12, 52, 120, 16, "&Machine:", 0);
    struct control *mc = ctl_add(d, CT_COMBO, A_MACHINE, 140, 48, W - 152, 21, "", CS_DROPLIST);
    ctl_combo_add(mc, "(none - the QEMU manager)"); mc->sel = 0;
    for (int i = 0; man && i < man->n; i++) { ctl_combo_add(mc, man->list[i].name); if (ab.machine[0] && !stricmp(man->list[i].file, ab.machine)) mc->sel = i + 1; }
    ctl_add(d, CT_LABEL, 0, 12, 86, 120, 16, "&Countdown (s):", 0);
    char t[16]; ksnprintf(t, sizeof t, "%d", ab.delay);
    struct control *de = ctl_add(d, CT_EDIT, A_DELAY, 140, 82, 60, 21, t, CS_NUMBER); de->maxlen = 3;
    ctl_add(d, CT_LABEL, 0, 12, 120, 120, 16, "&When it shuts down:", 0);
    struct control *wc = ctl_add(d, CT_COMBO, A_WHEN, 140, 116, W - 152, 21, "", CS_DROPLIST);
    for (int i = 0; i < 4; i++) ctl_combo_add(wc, when_off_names[i]);
    wc->sel = ab.when_off;
    ctl_add(d, CT_LABEL, 0, 12, 150, W - 24, 32, "From DOS too: QEMUDOS /M \"name\" starts a machine at once, QEMUDOS /NOAUTO skips this.", 0);
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 81, H - 24 - 34, 75, 23, "OK", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 81, H - 24 - 34, 75, 23, "Cancel", 0);
    d->focus = mc;
    wm_show(d, 1);
    wm_run_modal(d);
    if (d->result == ID_OK) {
        int s = mc->sel;
        strlcpy(ab.machine, s > 0 && man && s - 1 < man->n ? man->list[s - 1].file : "", sizeof ab.machine);
        ab.delay = atoi(de->text); if (ab.delay < 0) ab.delay = 0; if (ab.delay > 600) ab.delay = 600;
        ab.when_off = wc->sel >= 0 && wc->sel < 4 ? wc->sel : 0;
        /* (the machine's own "starts by itself" follows: one machine starts by itself) */
        for (int i = 0; man && i < man->n; i++) {
            int want = ab.machine[0] && !stricmp(man->list[i].file, ab.machine);
            if (man->list[i].autostart != want) { man->list[i].autostart = want; machine_save(&man->list[i]); }
        }
        if (ab_save() < 0) msgbox(d, "Autoboot", "AUTOBOOT.INI could not be written.", MB_OK | MB_ICONERROR);
    }
    wm_destroy(d);
    if (owner) { owner->enabled = 1; wm_activate(owner); }
    update_buttons();
}

/* ---- start, stop -------------------------------------------------------------------------------------------------- */
static int start_machine(struct machine *m, int quiet)
{
    if (!man || !m) return -1;
    if (is_running(m)) { vpc_show(); return 0; }
    if (vpc_state() != VPC_NONE) {
        struct machine *r = running_machine();
        char *msg = kaprintf("\"%s\" is running, and one machine runs at a time.\n\nTurn it off (as pulling its plug) and start \"%s\"?", r ? r->name : "A machine", m->name);
        int ans = msgbox(man->w, QDP_NAME, msg ? msg : "A machine is running. Turn it off?", MB_YESNO | MB_ICONQUESTION);
        kfree(msg);
        if (ans != ID_YES) return -1;
        vpc_power_off();
        for (int i = 0; i < 1000 && vpc_state() != VPC_NONE; i++) { wm_pump(); thread_sleep_ms(10); }
        if (vpc_state() != VPC_NONE) { msgbox(man->w, QDP_NAME, "The machine running did not stop.", MB_OK | MB_ICONERROR); return -1; }
    }
    if (!host_check(man->w, m)) return -1;
    char *cmd = machine_cmdline(m), *title = kstrdup(m->name);
    if (!cmd || !title) { msgbox(man->w, QDP_NAME, "There is not enough memory to start the machine.", MB_OK | MB_ICONERROR); kfree(cmd); kfree(title); return -1; }
    if (!m->file[0] || machine_save(m) < 0) { }          /* (a MAC address made now kept) */
    kprintf("qdp: start \"%s\": %s\n", m->name, cmd);
    int r = vpc_launch_machine(cmd, title, m->file);
    kfree(cmd); kfree(title);
    update_buttons();
    return r;
}
int qdpman_start_by_name(const char *name_or_file, int quiet)
{
    if (!man) return -1;
    for (int i = 0; i < man->n; i++) {
        const char *base = path_basename(man->list[i].file);
        char stem[260]; strlcpy(stem, base, sizeof stem); char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
        if (!stricmp(man->list[i].name, name_or_file) || !stricmp(man->list[i].file, name_or_file) || !stricmp(base, name_or_file) || !stricmp(stem, name_or_file)) {
            ctl_lv_select(man->lv, i, 1);
            return start_machine(&man->list[i], quiet);
        }
    }
    char *msg = kaprintf("There is no machine \"%s\" in %s.", name_or_file, mdir());
    msgbox(man->w, QDP_NAME, msg ? msg : "There is no such machine.", MB_OK | MB_ICONWARN);
    kfree(msg);
    return -1;
}
void qdpman_vm_changed(void) { update_buttons(); }
void qdpman_vm_stopped(const char *vmfile, int exit_code)
{
    kprintf("qdp: the machine %s stopped (exit code %d) - %u KB free\n", vmfile, exit_code, mm_free_pages() * 4);
    if (!man) return;
    if (exit_code != 0) {
        /* QEMU stopped by itself with an error - at its start, mostly (a file it needs, an option it does not know): what
         * it said is shown, and an autoboot's "start it again" does not start it again (it would fail again, and again) */
        int i = find_machine(vmfile);
        const char *said = vpc_qemu_last_said();
        char *msg = kaprintf("\"%s\" stopped - QEMU could not run it:\n\n%s", i >= 0 ? man->list[i].name : vmfile,
                             said[0] ? said : "(QEMU gave no reason.)");
        qdpman_show();
        update_buttons();
        msgbox(man->w, QDP_NAME, msg ? msg : "The machine stopped: QEMU could not run it.", MB_OK | MB_ICONERROR);
        kfree(msg);
        return;
    }
    ab_load();
    if (vmfile[0] && ab.machine[0] && !stricmp(vmfile, ab.machine) && ab.when_off) {
        int i = find_machine(vmfile);
        switch (ab.when_off) {
        case 1: if (i >= 0) { qdpman_show(); start_machine(&man->list[i], 1); return; } break;
        case 2: qdp_exit_to_dos(man->w, 0); return;
        case 3: qdp_restart(man->w, 0); return;
        }
    }
    qdpman_show();
    update_buttons();
}

/* the autoboot at the start: a countdown in the info panel's place, Esc (or a click, a key) stops it */
static int countdown_left, countdown_on;
static char countdown_file[260];
static int countdown_proc(struct window *d, struct wevent *e)
{
    if (e->type == WE_TIMER) {
        if (--countdown_left <= 0) { d->result = ID_OK; return 1; }
        char t[200]; struct machine *m = NULL; int i = find_machine(countdown_file); if (i >= 0) m = &man->list[i];
        ksnprintf(t, sizeof t, "\"%s\" starts in %d second%s.", m ? m->name : "The machine", countdown_left, countdown_left == 1 ? "" : "s");
        ctl_set_text(ctl_find(d, 2), t);
        return 1;
    }
    if (e->type == WE_KEYDOWN || e->type == WE_MOUSEDOWN) { d->result = (e->type == WE_KEYDOWN && e->key == 0x1C) ? ID_OK : ID_CANCEL; return 1; }
    return dialog_std_proc(d, e);
}
void qdpman_autoboot(void)
{
    if (!man) return;
    ab_load();
    int i = ab.machine[0] ? find_machine(ab.machine) : -1;
    if (i < 0) for (int k = 0; k < man->n; k++) if (man->list[k].autostart) { i = k; break; }   /* (a machine set to start by itself in its settings) */
    if (i < 0) return;
    ctl_lv_select(man->lv, i, 1); update_buttons();
    strlcpy(countdown_file, man->list[i].file, sizeof countdown_file);
    int ok = 1;
    if (ab.delay > 0) {
        int W = 380, H = 130;
        struct window *d = wm_create("Autoboot", 0, 0, W, H, WF_DLG | WF_MODAL, countdown_proc, NULL);
        d->owner = man->w; man->w->enabled = 0; wm_center(d, man->w);
        char t[200]; ksnprintf(t, sizeof t, "\"%s\" starts in %d seconds.", man->list[i].name, ab.delay);
        ctl_add(d, CT_ICON, 1, 14, 14, 32, 32, "", 0)->icon = ICO_COMPUTER;
        ctl_add(d, CT_LABEL, 2, 60, 18, W - 72, 16, t, 0);
        ctl_add(d, CT_LABEL, 3, 60, 40, W - 72, 16, "Enter starts it now; Esc, another key or a click: the manager.", 0);
        ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 91, H - 24 - 34, 85, 23, "Start now", CS_DEFAULT);
        ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 91, H - 24 - 34, 85, 23, "Manager", 0);
        countdown_left = ab.delay; countdown_on = 1;
        wm_set_timer(d, 1000);
        wm_show(d, 1);
        wm_run_modal(d);
        ok = d->result == ID_OK;
        countdown_on = 0;
        wm_destroy(d);
        man->w->enabled = 1; wm_activate(man->w);
    }
    i = find_machine(countdown_file);
    if (ok && i >= 0) start_machine(&man->list[i], 1);
}

/* ---- media: another CD or floppy while the machine runs --------------------------------------------------------- */
static void change_media(int drive, int eject)
{
    if (vpc_state() == VPC_NONE) { msgbox(man->w, QDP_NAME, "No machine is running.  A machine's images are changed in Machine > Settings.", MB_OK | MB_ICONINFO); return; }
    char p[260] = "";
    if (!eject) {
        strlcpy(p, qdp_dir(), sizeof p);
        const char *filter = drive == 0 ? "CD-ROM images (*.iso)|*.iso|All Files (*.*)|*.*" : "Floppy images (*.img;*.ima;*.vfd)|*.img;*.ima;*.vfd|All Files (*.*)|*.*";
        if (!file_dialog(man->w, drive == 0 ? "Put a CD in" : "Put a floppy in", 0, filter, p, sizeof p)) return;
    }
    if (vpc_change_media(drive, eject ? NULL : p) < 0) { msgbox(man->w, QDP_NAME, "The medium could not be changed.", MB_OK | MB_ICONERROR); return; }
    /* (kept in the machine's settings too, for its next start) */
    struct machine *m = running_machine();
    if (m) { char *f = drive == 0 ? m->cdrom : drive == 1 ? m->floppy : m->floppyb; strlcpy(f, eject ? "" : p, 260); machine_save(m); }
    update_buttons();
}

static void new_machine(void)
{
    struct machine m; machine_defaults(&m);
    new_mac(m.mac, sizeof m.mac);                       /* (its own address from the start - the settings show it) */
    for (int k = 1; ; k++) { int clash = 0; for (int i = 0; i < man->n; i++) if (!stricmp(man->list[i].name, m.name)) clash = 1; if (!clash) break; ksnprintf(m.name, sizeof m.name, "New Machine %d", k + 1); }
    if (!settings_dialog(man->w, &m)) { machine_free(&m); return; }
    char fn[256]; int j = 0; for (const char *p = m.name; *p && j < (int)sizeof fn - 1; p++) if (isalpha(*p) || isdigit(*p) || *p == '_' || *p == '-' || *p == ' ') fn[j++] = *p == ' ' ? '_' : *p; fn[j] = 0; if (!j) strcpy(fn, "MACHINE");
    if (j > 240) fn[240] = 0;
    char first[260], pre[260], first83[16], stem83[9];
    ksnprintf(first, sizeof first, "%s.VM", fn); ksnprintf(pre, sizeof pre, "%s (", fn);
    { char s8[9]; strlcpy(s8, fn, sizeof s8); ksnprintf(first83, sizeof first83, "%s.VM", s8); }
    strlcpy(stem83, fn, sizeof stem83);
    fs_mkdir(mdir());
    if (fileops_unique_name(mdir(), first, pre, ").VM", first83, stem83, ".VM", m.file, sizeof m.file) < 0 || machine_save(&m) < 0) {
        msgbox(man->w, QDP_NAME, "The machine's file could not be written.", MB_OK | MB_ICONERROR); machine_free(&m); return;
    }
    if (m.autostart) { ab_load(); strlcpy(ab.machine, m.file, sizeof ab.machine); ab_save(); }
    machine_free(&m);
    refresh_list();
    int i = find_machine(m.file); if (i >= 0) ctl_lv_select(man->lv, i, 1);
    update_buttons();
}

/* ---- the screen: its resolution, as the card's VESA BIOS lists them - a new one kept only when said so within 15 s
 * (a monitor that cannot show it shows nothing: the old one comes back by itself) -------------------------------------- */
extern int gui_set_mode(int w, int h, int bpp);
extern int display_modes(int *out, int max);
extern int display_bpp(void);
static int keep_left;
static int keep_proc(struct window *d, struct wevent *e)
{
    if (e->type == WE_TIMER) {
        if (--keep_left <= 0) { d->result = ID_NO; return 1; }
        char t[80]; ksnprintf(t, sizeof t, "The old resolution comes back in %d second%s.", keep_left, keep_left == 1 ? "" : "s");
        ctl_set_text(ctl_find(d, 2), t);
        return 1;
    }
    return dialog_std_proc(d, e);
}
static void screen_dialog(struct window *owner)
{
    int n = display_modes(NULL, 0);
    if (n <= 0) { msgbox(owner, QDP_NAME, "The graphics card lists no other resolution.", MB_OK | MB_ICONINFO); return; }
    int *m = kmalloc(sizeof(int) * 3 * (uint32_t)n);
    if (!m) return;
    display_modes(m, n);
    /* (largest first, each size once with its most colours) */
    int *ord = kmalloc(sizeof(int) * (uint32_t)n); int no = 0;
    if (!ord) { kfree(m); return; }
    for (int i = 0; i < n; i++) {
        if (m[3 * i + 2] < 15) continue;
        int dup = -1;
        for (int k = 0; k < no; k++) if (m[3 * ord[k]] == m[3 * i] && m[3 * ord[k] + 1] == m[3 * i + 1]) dup = k;
        if (dup >= 0) { if (m[3 * i + 2] > m[3 * ord[dup] + 2]) ord[dup] = i; continue; }
        ord[no++] = i;
    }
    for (int a = 0; a < no; a++) for (int b = a + 1; b < no; b++) {
        int ia = ord[a], ib = ord[b];
        if (m[3 * ib] * m[3 * ib + 1] > m[3 * ia] * m[3 * ia + 1]) { ord[a] = ib; ord[b] = ia; }
    }
    int W = 340, H = 330;
    struct window *d = wm_create("Screen Resolution", 0, 0, W, H, WF_DLG | WF_MODAL, dialog_std_proc, NULL);
    d->owner = owner; owner->enabled = 0; wm_center(d, owner);
    ctl_add(d, CT_LABEL, 0, 12, 12, W - 24, 16, "This screen's resolution and colours:", 0);
    struct control *lb = ctl_add(d, CT_LISTBOX, 1, 12, 32, W - 24, H - 110, "", 0);
    int cur = 0;
    for (int k = 0; k < no; k++) {
        int i = ord[k]; char t[48]; ksnprintf(t, sizeof t, "%d x %d, %d bits", m[3 * i], m[3 * i + 1], m[3 * i + 2]);
        ctl_lv_add(lb, t, -1, NULL);
        if (m[3 * i] == scr_w && m[3 * i + 1] == scr_h) cur = k;
    }
    ctl_lv_select(lb, cur, 1);
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 81, H - 24 - 34, 75, 23, "OK", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 81, H - 24 - 34, 75, 23, "Cancel", 0);
    d->focus = lb;
    wm_show(d, 1);
    wm_run_modal(d);
    int s = d->result == ID_OK ? lb->sel : -1;
    wm_destroy(d);
    owner->enabled = 1; wm_activate(owner);
    if (s >= 0 && s < no) {
        int i = ord[s], ow = scr_w, oh = scr_h, ob = display_bpp();
        if ((m[3 * i] != ow || m[3 * i + 1] != oh || m[3 * i + 2] != ob) && gui_set_mode(m[3 * i], m[3 * i + 1], m[3 * i + 2]) == 0) {
            int KW = 360, KH = 130;
            struct window *k = wm_create("Screen Resolution", 0, 0, KW, KH, WF_DLG | WF_MODAL, keep_proc, NULL);
            wm_center(k, NULL);
            ctl_add(k, CT_LABEL, 1, 12, 14, KW - 24, 16, "Keep this resolution?", 0);
            ctl_add(k, CT_LABEL, 2, 12, 36, KW - 24, 16, "The old resolution comes back in 15 seconds.", 0);
            ctl_add(k, CT_BUTTON, ID_YES, KW - 6 - 2 * 81, KH - 24 - 34, 75, 23, "Keep", CS_DEFAULT);
            ctl_add(k, CT_BUTTON, ID_NO, KW - 6 - 81, KH - 24 - 34, 75, 23, "Go back", 0);
            keep_left = 15; wm_set_timer(k, 1000);
            wm_show(k, 1); wm_run_modal(k);
            int keep = k->result == ID_YES;
            wm_destroy(k);
            if (keep) { cfg.scr_w = scr_w; cfg.scr_h = scr_h; cfg.scr_bpp = display_bpp(); config_save(); }
            else gui_set_mode(ow, oh, ob);
        } else if (m[3 * i] != ow || m[3 * i + 1] != oh || m[3 * i + 2] != ob) msgbox(owner, QDP_NAME, "The card could not be set to that resolution.", MB_OK | MB_ICONERROR);
    }
    kfree(ord); kfree(m);
}

static void layout(void)
{
    struct window *w = man->w;
    int lw = MAX(220, w->cw / 4), top = 40;
    man->lv->r = (struct rect){ 6, top, lw, w->ch - top - 6 };
    man->info->r = (struct rect){ 12 + lw, top, w->cw - 18 - lw, w->ch - top - 6 };
    wm_invalidate(w);
}
static void show_keys(void)
{
    msgbox(man->w, "Keys",
        "Ctrl+Alt+Home\tthe QEMU manager in front - and from it, the machine again\n"
        "Ctrl+Alt+End\tCtrl+Alt+Del to the machine\n"
        "Ctrl+Alt\tthe mouse back from the machine\n"
        "Ctrl+Alt+S\tthe machine's picture fitted, stretched, pixel for pixel\n"
        "Alt+Tab\t(in the manager) the machine again\n\n"
        "Every other key is the machine's while it has the screen - Ctrl+Alt+Del, Alt+Tab and Alt+F4 too.", MB_OK | MB_ICONINFO);
}
static int man_proc(struct window *w, struct wevent *e)
{
    switch (e->type) {
    case WE_SIZE: layout(); return 1;
    case WE_TIMER: {
        int st = vpc_state();
        if (st != man->last_state) { man->last_state = st; update_buttons(); }
        else if (st != VPC_NONE && running_machine() == selected()) ctl_invalidate(man->info);   /* (its picture) */
        return 1; }
    case WE_NOTIFY:
        if (e->ptr == man->lv) {
            if (e->notify == NT_SELECT) { update_buttons(); return 1; }
            if (e->notify == NT_DBLCLICK) { start_machine(selected(), 0); return 1; }
            if (e->notify == NT_CONTEXT) { int id = menu_popup(man->mmachine, w->cx + e->x, w->cy + e->y, w); if (id) wm_post_command(w, id); return 1; }
        }
        return 0;
    case WE_MENUSELECT: {
        int has = selected() != NULL, st = vpc_state(), any = st != VPC_NONE;
        menu_set_flags(man->mmachine, M_START, MI_DISABLED, !has); menu_set_flags(man->mmachine, M_SETTINGS, MI_DISABLED, !has);
        menu_set_flags(man->mfile, M_REMOVE, MI_DISABLED, !has); menu_set_flags(man->mfile, M_SAVEAS, MI_DISABLED, !has);
        for (int id = M_SHOW; id <= M_CAD; id++) menu_set_flags(man->mmachine, id, MI_DISABLED, !any);
        for (int id = M_CD_CHANGE; id <= M_FDB_EJECT; id++) menu_set_flags(man->mmedia, id, MI_DISABLED, !any);
        menu_set_flags(man->mview, M_FIT, MI_CHECKED, vpc_scale_mode() == 0); menu_set_flags(man->mview, M_STRETCH, MI_CHECKED, vpc_scale_mode() == 1);
        menu_set_flags(man->mview, M_ONE, MI_CHECKED, vpc_scale_mode() == 2);
        return 1; }
    case WE_KEYDOWN:
        if (e->key == 0x3F) { refresh_list(); return 1; }                                    /* F5 */
        if (e->key == 0x1C && selected()) { start_machine(selected(), 0); return 1; }        /* Enter */
        if (e->key == 0x31 && (e->mods & MOD_CTRL)) { new_machine(); return 1; }             /* Ctrl+N */
        return 0;
    case WE_COMMAND:
        switch (e->id) {
        case M_NEW: case B_NEW: new_machine(); return 1;
        case M_OPEN: {
            char p[260]; strlcpy(p, mdir(), sizeof p);
            if (file_dialog(w, "Open Machine File", 0, "QEMU machines (*.vm)|*.vm|All Files (*.*)|*.*", p, sizeof p)) {
                struct machine m; if (machine_load(&m, p) < 0) { msgbox(w, QDP_NAME, "This is not a machine file.", MB_OK | MB_ICONERROR); return 1; }
                char dst[260]; path_join(dst, sizeof dst, mdir(), path_basename(p));
                if (stricmp(dst, p)) { strlcpy(m.file, dst, sizeof m.file); machine_save(&m); }
                machine_free(&m);
                refresh_list();
                int i = find_machine(dst); if (i >= 0) ctl_lv_select(man->lv, i, 1);
                update_buttons();
            }
            return 1; }
        case M_SAVEAS: {
            struct machine *m = selected(); if (!m) return 1;
            char p[260]; strlcpy(p, m->file, sizeof p);
            if (file_dialog(w, "Save Machine As", 1, "QEMU machines (*.vm)|*.vm|All Files (*.*)|*.*", p, sizeof p)) { struct machine c = *m; strlcpy(c.file, p, sizeof c.file); machine_save(&c); refresh_list(); }
            return 1; }
        case M_REMOVE: {
            struct machine *m = selected(); if (!m) return 1;
            if (is_running(m)) { msgbox(w, QDP_NAME, "The machine is running: turn it off first.", MB_OK | MB_ICONWARN); return 1; }
            char *msg = kaprintf("Remove \"%s\"?\n\nIts machine file is deleted; its disk images are kept.", m->name);
            if (msgbox(w, QDP_NAME, msg ? msg : "Remove this machine?", MB_YESNO | MB_ICONQUESTION) == ID_YES) { fs_unlink(m->file); refresh_list(); }
            kfree(msg);
            return 1; }
        case M_NEWDISK: case B_DISK: { char p[260] = ""; if (create_disk_image(w, p, sizeof p)) { char *msg = kaprintf("%s was made.\n\nGive it to a machine in Machine > Settings > Disks.", p); msgbox(w, QDP_NAME, msg ? msg : "The disk image was made.", MB_OK | MB_ICONINFO); kfree(msg); } return 1; }
        case M_AUTOBOOT: case B_AUTO: autoboot_dialog(w); return 1;
        case M_RESTART: qdp_restart(w, 1); return 1;
        case M_EXIT: qdp_exit_to_dos(w, 1); return 1;
        case M_START: case B_START: start_machine(selected(), 0); return 1;
        case M_SHOW: case B_SHOW: vpc_show(); return 1;
        case M_PAUSE: case B_PAUSE: vpc_pause(vpc_state() != VPC_PAUSED); update_buttons(); return 1;
        case M_RESET: case B_RESET:
            if (msgbox(w, QDP_NAME, "Reset the machine?  It starts again at once, as with its reset button: what it has not saved is lost.", MB_YESNO | MB_ICONQUESTION) == ID_YES) { vpc_reset(); vpc_show(); }
            return 1;
        case M_POWERBTN: vpc_power_button(); vpc_show(); return 1;
        case M_POWEROFF: case B_OFF:
            if (msgbox(w, QDP_NAME, "Turn the machine off at once?\n\nAs pulling its plug: what it has not saved is lost.  (Machine > Power Button lets a system with ACPI shut itself down.)", MB_YESNO | MB_ICONWARN) == ID_YES) vpc_power_off();
            return 1;
        case M_CAD: vpc_send_cad(); vpc_show(); return 1;
        case M_SETTINGS: case B_SETTINGS: {
            struct machine *m = selected(); if (!m) return 1;
            struct machine c = *m;
            if (c.extra && !(c.extra = kstrdup(c.extra))) return 1;
            if (settings_dialog(w, &c)) {
                kfree(m->extra); *m = c; machine_save(m);
                if (m->autostart) { ab_load(); if (stricmp(ab.machine, m->file)) { strlcpy(ab.machine, m->file, sizeof ab.machine); ab_save(); } }
                else { ab_load(); if (!stricmp(ab.machine, m->file)) { ab.machine[0] = 0; ab_save(); } }
                if (is_running(m)) msgbox(w, QDP_NAME, "The machine is running: the new settings are its at its next start.", MB_OK | MB_ICONINFO);
                update_buttons();
            } else kfree(c.extra);
            return 1; }
        case M_CD_CHANGE: change_media(0, 0); return 1;
        case M_CD_EJECT: change_media(0, 1); return 1;
        case M_FDA_CHANGE: change_media(1, 0); return 1;
        case M_FDA_EJECT: change_media(1, 1); return 1;
        case M_FDB_CHANGE: change_media(2, 0); return 1;
        case M_FDB_EJECT: change_media(2, 1); return 1;
        case M_FIT: vpc_set_scale_mode(0); return 1;
        case M_STRETCH: vpc_set_scale_mode(1); return 1;
        case M_ONE: vpc_set_scale_mode(2); return 1;
        case M_REFRESH: refresh_list(); return 1;
        case M_SCREEN: screen_dialog(w); return 1;
        case M_KEYS: show_keys(); return 1;
        case M_ABOUT:
            msgbox(w, "About " QDP_NAME, QDP_NAME " " QDP_VERSION "\n\nA PC emulator that is the computer: MS-DOS 7.1 starts it, and its 32-bit kernel - DOS95's, with its drivers for "
                   "today's disks, USB, sound and network cards - runs " QDP_ENGINE " (the i386 system emulator with its TCG translator) over the whole screen.\n\n"
                   "QEMU is free software under the GNU GPL v2 (qemu\\COPYING in the source).", MB_OK | MB_ICONINFO);
            return 1;
        }
        return 0;
    case WE_CLOSE: qdp_exit_to_dos(w, 1); return 1;
    case WE_DESTROY:
        menu_free(man->mfile); menu_free(man->mmachine); menu_free(man->mmedia); menu_free(man->mview); menu_free(man->mhelp);
        for (int i = 0; i < man->n; i++) machine_free(&man->list[i]);
        kfree(man->list); kfree(man->thumb);
        if (qdpman_app.inst == w) qdpman_app.inst = NULL;
        kfree(man); man = NULL;
        return 1;
    }
    return 0;
}
void qdpman_show(void)
{
    if (!man) { app_launch("qemu"); return; }
    wm_show(man->w, 1);
    if (man->w->state != WS_MAXIMIZED) wm_set_state(man->w, WS_MAXIMIZED);
    wm_activate(man->w);
    update_buttons();
}
static void qdpman_launch(void)
{
    if (man) { qdpman_show(); return; }
    man = kzalloc(sizeof *man);
    if (!man) return;
    struct window *w = wm_create(QDP_NAME, 0, 0, MIN(scr_w, 800), MIN(scr_h, 600), WF_TITLE | WF_RESIZABLE | WF_MAXBOX | WF_CLOSEBOX | WF_SYSMENU | WF_MENUBAR | WF_STATUSBAR, man_proc, NULL);
    if (!w) { kfree(man); man = NULL; return; }
    man->w = w; w->icon = ICO_COMPUTER; w->minw = 500; w->minh = 340; qdpman_app.inst = w;
    man->mfile = menu_create();
    menu_add(man->mfile, "&New Machine...\tCtrl+N", M_NEW, 0); menu_add(man->mfile, "&Open Machine File...", M_OPEN, 0);
    menu_add(man->mfile, "Save Machine &As...", M_SAVEAS, 0); menu_add(man->mfile, "&Remove Machine", M_REMOVE, 0);
    menu_add_sep(man->mfile); menu_add(man->mfile, "New &Disk Image...", M_NEWDISK, 0); menu_add(man->mfile, "A&utoboot...", M_AUTOBOOT, 0);
    menu_add_sep(man->mfile); menu_add(man->mfile, "Res&tart the Computer", M_RESTART, 0); menu_add(man->mfile, "E&xit to MS-DOS", M_EXIT, 0);
    man->mmachine = menu_create();
    menu_add(man->mmachine, "&Start\tEnter", M_START, MI_DEFAULT); menu_add(man->mmachine, "S&how\tCtrl+Alt+Home", M_SHOW, 0);
    menu_add(man->mmachine, "&Pause / Resume", M_PAUSE, 0); menu_add(man->mmachine, "&Reset", M_RESET, 0);
    menu_add(man->mmachine, "Power &Button (ACPI shutdown)", M_POWERBTN, 0); menu_add(man->mmachine, "Turn &Off", M_POWEROFF, 0);
    menu_add(man->mmachine, "Send Ctrl+Alt+&Del\tCtrl+Alt+End", M_CAD, 0);
    menu_add_sep(man->mmachine); menu_add(man->mmachine, "S&ettings...", M_SETTINGS, 0);
    man->mmedia = menu_create();
    menu_add(man->mmedia, "Put a &CD in...", M_CD_CHANGE, 0); menu_add(man->mmedia, "Take the CD &out", M_CD_EJECT, 0);
    menu_add_sep(man->mmedia); menu_add(man->mmedia, "Put a floppy in &A:...", M_FDA_CHANGE, 0); menu_add(man->mmedia, "Take the floppy out of A:", M_FDA_EJECT, 0);
    menu_add(man->mmedia, "Put a floppy in &B:...", M_FDB_CHANGE, 0); menu_add(man->mmedia, "Take the floppy out of B:", M_FDB_EJECT, 0);
    man->mview = menu_create();
    menu_add(man->mview, "The machine's picture &fitted (its shape kept)", M_FIT, 0); menu_add(man->mview, "The machine's picture &stretched", M_STRETCH, 0);
    menu_add(man->mview, "The machine's picture &pixel for pixel", M_ONE, 0);
    menu_add_sep(man->mview); menu_add(man->mview, "S&creen Resolution...", M_SCREEN, 0);
    menu_add_sep(man->mview); menu_add(man->mview, "&Refresh\tF5", M_REFRESH, 0);
    man->mhelp = menu_create();
    menu_add(man->mhelp, "&Keys", M_KEYS, 0); menu_add_sep(man->mhelp); menu_add(man->mhelp, "&About " QDP_NAME, M_ABOUT, 0);
    man->bar[0].text = "&File"; man->bar[0].menu = man->mfile;
    man->bar[1].text = "&Machine"; man->bar[1].menu = man->mmachine;
    man->bar[2].text = "M&edia"; man->bar[2].menu = man->mmedia;
    man->bar[3].text = "&View"; man->bar[3].menu = man->mview;
    man->bar[4].text = "&Help"; man->bar[4].menu = man->mhelp;
    wm_set_menubar(w, man->bar, 5);
    static const struct { int id, w; const char *t; } tb[9] = {
        { B_NEW, 64, "&New" }, { B_SETTINGS, 76, "Se&ttings" }, { B_START, 64, "&Start" }, { B_SHOW, 64, "Sho&w" }, { B_PAUSE, 70, "&Pause" },
        { B_RESET, 64, "&Reset" }, { B_OFF, 74, "Turn &Off" }, { B_DISK, 90, "New &Disk..." }, { B_AUTO, 86, "&Autoboot..." } };
    int bx = 6;
    for (int i = 0; i < 9; i++) { man->btn[i] = ctl_add(w, CT_BUTTON, tb[i].id, bx, 6, tb[i].w, 26, tb[i].t, 0); bx += tb[i].w + 4 + (i == 1 || i == 6 ? 10 : 0); }
    man->lv = ctl_add(w, CT_LISTVIEW, C_LIST, 6, 40, 220, w->ch - 46, "", CS_LISTVIEW | CS_NORENAME);
    man->info = ctl_add(w, CT_CUSTOM, C_INFO, 232, 40, w->cw - 238, w->ch - 46, "", 0); man->info->custom_paint = info_paint;
    man->last_state = -1;
    refresh_list();
    w->focus = man->lv;
    wm_set_timer(w, 1000);
    wm_show(w, 1);
    wm_set_state(w, WS_MAXIMIZED);
    layout();
    update_buttons();
}
/* (app.c apps_init: in the QEMU DOS port this is the manager - DOS95's QEMU Launcher, kernel/qlaunch.c, is not in its kernel) */
void qlaunch_register(void)
{
    qdpman_app.name = "qemu"; qdpman_app.title = QDP_NAME; qdpman_app.icon = ICO_COMPUTER; qdpman_app.single = 1;
    qdpman_app.launch = qdpman_launch;
    app_register(&qdpman_app);
}
