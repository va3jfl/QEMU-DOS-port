/* DOS95 GUI - configuration file (DOS95.INI), paths, shortcuts */
#ifdef QDP
#define CFG_INI_NAME "QEMUDOS.INI"                      /* (the QEMU DOS port's settings, beside QEMUDOS.COM) */
#else
#define CFG_INI_NAME "DOS95.INI"
#endif
#include "shell.h"

struct config cfg;

static char *ini_buf;
static int ini_len;

/* REGISTRY.INI - what programs register with the OS.
 *
 * Kept apart from DOS95.INI because config_save() rewrites that file from the
 * cfg struct, so anything the OS does not itself model is destroyed the next
 * time a setting changes.  The OS only ever READS this file; an installer
 * writes it, and removing a program's lines is all an uninstall has to do. */
static char *reg_buf;
/* v41 (#336): these files read whole, however long (REGISTRY.INI was cut at 64 KB - 16 KB before
 * v40, when 7-Zip registered forty-odd kinds of archive - DOS95.INI at 16 KB), a value as long as
 * it is (it was cut at 255): NULL no memory, *len its length */
/* a whole text file, NUL-terminated (kmalloc'd) - NULL, *len the error, if it cannot be read
 * (v41: also fileops.c's Recycle Bin list) */
char *kread_text(const char *path, int *len)
{
    int h = fs_open(path, 0);
    if (h < 0) { *len = h; return NULL; }             /* (not there, or not to be opened now) */
    int n = (int)fs_filesize(h);
    fs_close(h);
    if (n < 0) { *len = n; return NULL; }
    char *b = kmalloc((uint32_t)n + 1);
    if (!b) { *len = -12; return NULL; }
    int r = n ? fs_read_file(path, b, n) : 0;
    if (r < 0) { kfree(b); *len = r; return NULL; }
    b[r] = 0;
    *len = r;
    return b;
}

static const char *buf_get(const char *buf, const char *section, const char *key)
{
    if (!buf) return NULL;
    static char *val; static int valcap;    /* (the last value found: callers copy it at once) */
    const char *p = buf;
    int insec = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            insec = e && (int)strlen(section) == e - p - 1 && !strnicmp(p + 1, section, e - p - 1);
        } else if (insec && *p != ';' && *p != '#') {
            const char *eq = p; while (eq < eol && *eq != '=') eq++;
            if (eq < eol) {
                int kl = eq - p; while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) kl--;
                if (kl == (int)strlen(key) && !strnicmp(p, key, kl)) {
                    const char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
                    int vl = eol - v;
                    while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                    if (vl + 1 > valcap) {
                        char *nv = kmalloc((uint32_t)vl + 256);
                        if (!nv) return NULL;
                        kfree(val); val = nv; valcap = vl + 256;
                    }
                    memcpy(val, v, vl); val[vl] = 0;
                    return val;
                }
            }
        }
        p = eol; while (*p == '\n' || *p == '\r') p++;
    }
    return NULL;
}
/* Every key name in a section of the registry, in file order.  Used where the
 * keys themselves are data - the extensions a program claims, the Control Panel
 * icons it adds. */
int reg_keys(const char *section, char keys[][32], int max, int keylen)
{
    if (!reg_buf) return 0;
    const char *p = reg_buf;
    int insec = 0, n = 0;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        const char *eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (insec) break;                       /* the section ended */
            insec = e && (int)strlen(section) == e - p - 1 && !strnicmp(p + 1, section, e - p - 1);
        } else if (insec && *p != ';' && *p != '#') {
            const char *eq = p; while (eq < eol && *eq != '=') eq++;
            if (eq < eol) {
                int kl = eq - p; while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) kl--;
                if (kl > 0) { if (kl > keylen - 1) kl = keylen - 1;
                              memcpy(keys[n], p, kl); keys[n][kl] = 0; n++; }
            }
        }
        p = eol; while (*p == '\n' || *p == '\r') p++;
    }
    return n;
}

/* v41 (#336): every key name of a section, as many and as long as they are - a list of its own
 * (each name and the list the caller's to free); NULL no memory (or no registry) */
char **reg_keys_all(const char *section, int *count)
{
    *count = 0;
    if (!reg_buf) return NULL;
    int cap = 16, n = 0;
    char **keys = kmalloc(sizeof(char *) * (uint32_t)cap);
    if (!keys) return NULL;
    const char *p = reg_buf;
    int insec = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (insec) break;                       /* the section ended */
            insec = e && (int)strlen(section) == e - p - 1 && !strnicmp(p + 1, section, e - p - 1);
        } else if (insec && *p != ';' && *p != '#') {
            const char *eq = p; while (eq < eol && *eq != '=') eq++;
            if (eq < eol) {
                int kl = eq - p; while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) kl--;
                if (kl > 0) {
                    if (n == cap) {
                        char **t = kmalloc(sizeof(char *) * (uint32_t)cap * 2);
                        if (!t) break;
                        memcpy(t, keys, sizeof(char *) * (uint32_t)n); kfree(keys); keys = t; cap *= 2;
                    }
                    char *k = kmalloc((uint32_t)kl + 1);
                    if (!k) break;
                    memcpy(k, p, kl); k[kl] = 0; keys[n++] = k;
                }
            }
        }
        p = eol; while (*p == '\n' || *p == '\r') p++;
    }
    *count = n;
    return keys;
}

static const char *ini_get(const char *s, const char *k) { return buf_get(ini_buf, s, k); }
const char *reg_value(const char *s, const char *k)       { return buf_get(reg_buf, s, k); }
/* v41 (#198): a value as DOS95.INI keeps it now - the file, not cfg (a wallpaper a program set
 * for the session only is not in it); def when it has none */
void config_kept(const char *sec, const char *key, char *out, int n, const char *def)
{
    char path[160]; path_join(path, sizeof path, cfg.dos95dir, CFG_INI_NAME);
    int len;
    char *b = kread_text(path, &len);
    const char *v = b ? buf_get(b, sec, key) : NULL;
    strlcpy(out, v ? v : def, n);
    kfree(b);
}
/* v38: read REGISTRY.INI again - an installer or an applet (Wine's) that changed it
 * does not need a restart for the shell to see the change */
int reg_reload(void)
{
    char path[160]; path_join(path, sizeof path, cfg.dos95dir, "REGISTRY.INI");
    int rl;
    char *nb = kread_text(path, &rl);
    if (!nb) return rl;
    static char *older;                           /* freed one reload later: a reader may still be in it */
    uint32_t f = save_flags_cli();
    char *old = reg_buf; reg_buf = nb;
    restore_flags(f);
    kfree(older); older = old;
    return rl;
}

static int ini_int(const char *sec, const char *key, int def) { const char *v = ini_get(sec, key); return v ? (int)strtol(v, NULL, 0) : def; }

/* ---- v41 (#336): the most-recently-used lists (shell.h) -------------------------------------- */
static const struct { const char *sect, *key; int max; } mru_def[3] = {
    { "RunMRU", "Run", 26 }, { "FindMRU", "Find", 26 }, { "Recent", "File", 15 },
};
static char *mru_e[3][26];
static int mru_n[3];
static void mru_sync_cfg(int list)                         /* the first 8 into cfg, as before */
{
    for (int i = 0; i < 8; i++) {
        const char *v = i < mru_n[list] ? mru_e[list][i] : "";
        if (list == MRU_RUN) strlcpy(cfg.run_history[i], v, sizeof cfg.run_history[i]);
        else if (list == MRU_FIND) strlcpy(cfg.find_history[i], v, sizeof cfg.find_history[i]);
        else cfg_set_path(cfg.recent[i], sizeof cfg.recent[i], v);
    }
}
int mru_count(int list) { return list >= 0 && list < 3 ? mru_n[list] : 0; }
const char *mru_at(int list, int i) { return list >= 0 && list < 3 && i >= 0 && i < mru_n[list] ? mru_e[list][i] : NULL; }
void mru_add(int list, const char *s)
{
    if (list < 0 || list > 2 || !s || !s[0]) return;
    int k;
    for (k = 0; k < mru_n[list]; k++) if (!stricmp(mru_e[list][k], s)) break;
    char *v;
    if (k < mru_n[list]) v = mru_e[list][k];               /* already there: to the front */
    else {
        if (!(v = kstrdup(s))) return;
        if (mru_n[list] == mru_def[list].max) kfree(mru_e[list][--mru_n[list]]);   /* the oldest goes */
        k = mru_n[list]++;
    }
    memmove(&mru_e[list][1], &mru_e[list][0], k * sizeof mru_e[list][0]);
    mru_e[list][0] = v;
    mru_sync_cfg(list);
}
void mru_clear(int list)
{
    if (list < 0 || list > 2) return;
    for (int i = 0; i < mru_n[list]; i++) kfree(mru_e[list][i]);
    mru_n[list] = 0;
    mru_sync_cfg(list);
}
void cfg_set_path(char *field, int n, const char *path)
{
    if ((int)strlen(path) < n || fs_shortname(path, field, n) < 0) strlcpy(field, path, n);
}
static void ini_str(const char *sec, const char *key, char *out, int n, const char *def) { const char *v = ini_get(sec, key); strlcpy(out, v ? v : def, n); }

/* v41 (#336): DNS as Windows 95's DNS Configuration has it - the name servers past the first
 * (cfg.net_dns: its 16 bytes are the cfg ABI's), the domain and the domain suffix search order,
 * as many as are listed: [Network] DNS2=, DNS3=, ...  Domain=  Suffix1=, Suffix2=, ... */
char **cfg_dns_more; int cfg_dns_more_n;
char cfg_dns_domain[256];
char **cfg_dns_suffix; int cfg_dns_suffix_n;
/* a list set anew from items (copied): 0, or -1 with no memory (the old list kept) */
int cfg_list_set(char ***list, int *n, char *const *items, int count)
{
    char **t = count ? kzalloc(count * sizeof *t) : NULL;
    if (count && !t) return -1;
    for (int i = 0; i < count; i++)
        if (!(t[i] = kstrdup(items[i]))) { while (i--) kfree(t[i]); kfree(t); return -1; }
    for (int i = 0; i < *n; i++) kfree((*list)[i]);
    if (*list) kfree(*list);
    *list = t; *n = count;
    return 0;
}
/* key<first>, key<first+1>, ... while they are there (each value copied at once: ini_get's
 * buffer is the next call's) */
static void ini_list(const char *sec, const char *key, int first, char ***list, int *n)
{
    int count = 0, cap = 0; char **items = NULL;
    for (int k = first; ; k++) {
        char name[32]; ksnprintf(name, sizeof name, "%s%d", key, k);
        const char *v = ini_get(sec, name);
        if (!v) break;
        if (!v[0]) continue;
        if (count == cap) { int nc = cap ? cap * 2 : 8; char **t = kmalloc(nc * sizeof *t); if (!t) break; if (items) { memcpy(t, items, count * sizeof *t); kfree(items); } items = t; cap = nc; }
        if (!(items[count] = kstrdup(v))) break;
        count++;
    }
    for (int i = 0; i < *n; i++) kfree((*list)[i]);
    if (*list) kfree(*list);
    *list = items; *n = count;
}

void config_load(void)
{
    memset(&cfg, 0, sizeof cfg);
    strlcpy(cfg.dos95dir, boot->exedir, sizeof cfg.dos95dir);
    if (!cfg.dos95dir[0]) strcpy(cfg.dos95dir, "C:\\DOS95\\");
    path_join(cfg.desktop_dir, sizeof cfg.desktop_dir, cfg.dos95dir, "DESKTOP");
    path_join(cfg.startmenu_dir, sizeof cfg.startmenu_dir, cfg.dos95dir, "STARTMNU");
    char path[160]; path_join(path, sizeof path, cfg.dos95dir, CFG_INI_NAME);
    ini_buf = kread_text(path, &ini_len);
    if (!ini_buf) { ini_buf = kzalloc(1); ini_len = 0; }
    path_join(path, sizeof path, cfg.dos95dir, "REGISTRY.INI");
    int rl;
    reg_buf = kread_text(path, &rl);

    /* v41 (#402): how a library's pages are kept (kernel/kmod.c, "a unit's pages"): 0 in memory until
     * the memory is wanted; 1 only while a program uses them; 2 as 1, each built again and compared */
    { extern int kmod_pg_mode; kmod_pg_mode = ini_int("Memory", "Pages", 0); }
    { extern int vstep_mode; vstep_mode = ini_int("Test", "Step", 1);                        /* v41 (#412): vcpi.c vstep_run */
      if (vstep_mode != 1) kprintf("vcpi: [Test] Step=%d - %s\n", vstep_mode, vstep_mode == 2 ? "a box's code with its interrupts off is stepped on the processor and compared with what the kernel would have made of it (a test: vstep lines)" : "a box's code with its interrupts off is stepped on the processor, every instruction a trap, as before v41"); }
    { extern int ata_mode; ata_mode = ini_int("Disk", "Direct", 1);                          /* v41 (#412): kernel/ata.c */
      if (ata_mode != 1) kprintf("disk: [Disk] Direct=%d - %s\n", ata_mode, ata_mode == 2 ? "every read the kernel makes for a VM's INT 13h made through the BIOS too and compared (a test: disk: WRONG lines)" : "every disk through the BIOS"); }
    { extern int fs_auto_graphics; fs_auto_graphics = ini_int("DOS", "FullScreenGraphics", 0);   /* v41 (#455): vgafs.c - 0: a DOS
                                                              program's graphics stay in its window (Alt+Enter takes it full screen) */
      if (fs_auto_graphics) kprintf("vgafs: [DOS] FullScreenGraphics=1 - a DOS program setting a graphics mode is taken full screen\n"); }
    { extern int fs_scaled_pref; fs_scaled_pref = ini_int("DOS", "FullScreenScaled", 0);   /* v43 (#508): vgafs.c - Alt+Enter scales
                                                                                     * the session's picture over the display */
      if (fs_scaled_pref) kprintf("vgafs: [DOS] FullScreenScaled=1 - Alt+Enter scales a session's picture over the display\n"); }
    { extern int tm_speed_ms, tm_cpu_graphs, tm_kernel_times, tm_on_top, tm_page;               /* v44 (M6): Task Manager's choices */
      int v = ini_int("TaskManager", "UpdateSpeed", 1000); tm_speed_ms = v == 500 || v == 4000 || v == 0 ? v : 1000;
      tm_cpu_graphs = ini_int("TaskManager", "CpuGraphs", 1) != 0; tm_kernel_times = ini_int("TaskManager", "KernelTimes", 0) != 0;
      tm_on_top = ini_int("TaskManager", "AlwaysOnTop", 1) != 0; v = ini_int("TaskManager", "Page", 0); tm_page = v >= 0 && v < 3 ? v : 0; }
    { extern int smp_sched_on, smp_on; int p = ini_int("System", "Processors", 0);            /* v44 (item 3a, M7): sched.c smp_cpu_ok -
                                                              0 every processor, 1 the first only, as before (a PC whose others misbehave) */
      smp_sched_on = p != 1;
      if (p == 1 && smp_on) kprintf("smp: [System] Processors=1 - threads on the first processor only (the others idle)\n");
      { extern void smp_mode_text(char *b, int n); char m[80]; smp_mode_text(m, sizeof m); kprintf("smp: %s\n", m); } }   /* (what System Properties says) */
    { extern int aspace_shr_strict; aspace_shr_strict = ini_int("Memory", "Strict", 0);      /* v41 (#403): a test switch (aspace.c) */
      if (aspace_shr_strict) kprintf("mm: [Memory] Strict=1 - no program's libraries in the kernel's view between two programs' work (a test switch: KEPT POINTER lines)\n"); }
    { extern int midi_synth; char v[16]; ini_str("MIDI", "Synth", v, sizeof v, "GM");   /* v43 (#505): kernel/midi.c */
      midi_synth = !stricmp(v, "MT32") ? 1 : !stricmp(v, "FM") ? 2 : !stricmp(v, "MPU401") ? 3 : 0;
      extern char midi_soundfont[128]; ini_str("MIDI", "SoundFont", midi_soundfont, sizeof midi_soundfont, ""); }
#ifdef QDP
    /* (the QEMU DOS port: a machine's screen in its own colours - 32 bits - on 1024 x 768 the first time, a PC of today's
     * size; View > Screen in the manager sets another, kept here) */
    cfg.scr_w = ini_int("Display", "Width", 1024); cfg.scr_h = ini_int("Display", "Height", 768); cfg.scr_bpp = ini_int("Display", "Bpp", 32);
#else
    cfg.scr_w = ini_int("Display", "Width", 800);
    cfg.scr_h = ini_int("Display", "Height", 600);
    cfg.scr_bpp = ini_int("Display", "Bpp", 0);
#endif
    cfg.desktop_color = ini_int("Display", "Background", 0x008080);
    syscol = color_schemes[0].c; syscol.desktop = cfg.desktop_color;
    ini_str("Display", "Wallpaper", cfg.wallpaper, sizeof cfg.wallpaper, "");
    cfg.wallpaper_mode = ini_int("Display", "WallpaperMode", 0);
    cfg.pattern = ini_int("Display", "Pattern", 0);
    cfg.color_scheme = ini_int("Display", "ColorScheme", 0);
    cfg.custom_colors = ini_int("Display", "CustomColors", 0);
    for (int i = 0; i < CE_COUNT; i++) { char k[16]; ksnprintf(k, sizeof k, "Color%d", i); cfg.colors[i] = ini_int("Colors", k, 0); }
    cfg.screensaver = ini_int("Display", "ScreenSaver", 0);
    cfg.screensaver_min = ini_int("Display", "ScreenSaverWait", 15);
    /* v44 (item 3a, E1): the IceWM theme worn; with no Theme= at all, the owner's choice - Silver XP on a PC of several
     * processors, DOS95's own look on one */
    { char v[64]; ini_str("Display", "Theme", v, sizeof v, "\x01");
      if (v[0] == 1) strlcpy(cfg.theme, ncpus > 1 ? "SILVERXP" : "", sizeof cfg.theme); else strlcpy(cfg.theme, v, sizeof cfg.theme);
      cfg.theme_bg = ini_int("Display", "ThemeBackground", 1) != 0; }
    /* v44 (item 3a, E2): the effects, all on (each turns itself off on a PC too slow for it: kernel/wm.c) */
    cfg.fx_minanim = ini_int("Display", "MinAnimate", 1) != 0;
    cfg.fx_menuanim = ini_int("Display", "MenuAnimation", 1) != 0;
    cfg.fx_menufade = ini_int("Display", "MenuFade", 1) != 0;
    cfg.fx_dragfull = ini_int("Display", "DragFullWindows", 1) != 0;
    cfg.small_startmenu = ini_int("Taskbar", "SmallIcons", 0);
    cfg.desktop_autoarrange = ini_int("Desktop", "AutoArrange", 1);
    cfg.desktop_sort = ini_int("Desktop", "Sort", 0);
    cfg.net_enabled = ini_int("Network", "Enabled", 1);
    cfg.net_adapter = ini_int("Network", "Adapter", 0);
    cfg.net_isa_port = ini_int("Network", "IsaPort", 0x300);
    cfg.net_isa_irq = ini_int("Network", "IsaIrq", 9);
    cfg.net_dhcp = ini_int("Network", "Dhcp", 1);
    ini_str("Network", "IP", cfg.net_ip, sizeof cfg.net_ip, "");
    ini_str("Network", "Mask", cfg.net_mask, sizeof cfg.net_mask, "");
    ini_str("Network", "Gateway", cfg.net_gw, sizeof cfg.net_gw, "");
    ini_str("Network", "DNS", cfg.net_dns, sizeof cfg.net_dns, "");
    ini_str("Network", "Hostname", cfg.net_hostname, sizeof cfg.net_hostname, "dos95");
    ini_str("Network", "PacketDriver", cfg.net_pktdrv, sizeof cfg.net_pktdrv, "");
    ini_list("Network", "DNS", 2, &cfg_dns_more, &cfg_dns_more_n);
    ini_str("Network", "Domain", cfg_dns_domain, sizeof cfg_dns_domain, "");
    ini_list("Network", "Suffix", 1, &cfg_dns_suffix, &cfg_dns_suffix_n);
    cfg.always_on_top = ini_int("Taskbar", "AlwaysOnTop", 1);
    cfg.snd_scheme = ini_int("Sounds", "Scheme", 1);
    { extern const char *const sound_event_keys[SE_COUNT], *const sound_scheme_default[SE_COUNT];
      for (int i = 0; i < SE_COUNT; i++) ini_str("Sounds", sound_event_keys[i], cfg.snd_event[i], sizeof cfg.snd_event[i], cfg.snd_scheme == 1 ? sound_scheme_default[i] : ""); }
    cfg.mix_master = ini_int("Mixer", "Master", -1); cfg.mix_wave = ini_int("Mixer", "Wave", -1); cfg.mix_midi = ini_int("Mixer", "Midi", -1);
    cfg.mix_cd = ini_int("Mixer", "CD", -1); cfg.mix_line = ini_int("Mixer", "Line", -1); cfg.mix_mic = ini_int("Mixer", "Mic", -1);
    cfg.mix_spk = ini_int("Mixer", "PCSpeaker", -1); cfg.mix_bass = ini_int("Mixer", "Bass", -1); cfg.mix_treble = ini_int("Mixer", "Treble", -1);
    cfg.mix_mute = ini_int("Mixer", "Mute", 0);
    { extern void sndmix_prefer(int usb); sndmix_prefer(ini_int("Mixer", "PreferUSB", 1)); }   /* v43 (#510): Multimedia > Audio */
    cfg.java_heap_mb = ini_int("Java", "HeapMB", 48); cfg.java_console = ini_int("Java", "Console", 0); cfg.java_verbose = ini_int("Java", "Verbose", 0);
    ini_str("Java", "ClassPath", cfg.java_classpath, sizeof cfg.java_classpath, ""); ini_str("Java", "Options", cfg.java_options, sizeof cfg.java_options, "");
    /* (the MRU lists: at the end) */
    colors_apply_config();
    if (cfg.custom_colors) cfg.desktop_color = syscol.desktop;
    cfg.mouse_speed = ini_int("Mouse", "Speed", 4);
    cfg.dblclick_ms = ini_int("Mouse", "DoubleClick", 500);   /* (v41, #443: Windows' own, 500 ms - 400 was short for a hand) */
    cfg.mouse_swap = ini_int("Mouse", "SwapButtons", 0);
    cfg.kbd_repeat = ini_int("Keyboard", "RepeatRate", 8);
    cfg.kbd_delay = ini_int("Keyboard", "Delay", 1);
    /* v41 (#252): the game port's joysticks (kernel/joy.c; a calibration left 0 is the default) */
    cfg.joy_type[0] = ini_int("Joystick", "Joystick1", 0);
    cfg.joy_type[1] = ini_int("Joystick", "Joystick2", 0);
    for (int i = 0; i < 4; i++) { static const char *const w[3] = { "Least", "Centre", "Most" };
        for (int j = 0; j < 3; j++) { char k[24]; ksnprintf(k, sizeof k, "Axis%d%s", i + 1, w[j]); cfg.joy_cal[i][j] = ini_int("Joystick", k, 0); } }
    /* v41 (#251): DOS programs' printing spooled on these LPTs (kernel/lpt.c) - "LPT1,LPT3" */
    { char v[40]; ini_str("Printers", "CapturePorts", v, sizeof v, ""); cfg.lpt_capture = 0;
      for (char *q = v; *q; q++) if ((q[0] == 'L' || q[0] == 'l') && (q[1] == 'P' || q[1] == 'p') && (q[2] == 'T' || q[2] == 't') && q[3] >= '1' && q[3] <= '3') cfg.lpt_capture |= 1 << (q[3] - '1'); }
    cfg.lpt_timeout = ini_int("Printers", "DosTimeout", 15);
    if (cfg.lpt_timeout < 1) cfg.lpt_timeout = 15;     /* (v41, #336: any number of seconds - past 3600 it was 15) */
    /* v41 (#298): the clock's time zone and DST adjustment */
    ini_str("DateTime", "TimeZone", cfg.tz_name, sizeof cfg.tz_name, "");
    cfg.tz_autodst = ini_int("DateTime", "AutoDST", 1);
    cfg.show_clock = ini_int("Taskbar", "ShowClock", 1);
    cfg.taskbar_autohide = ini_int("Taskbar", "AutoHide", 0);
    cfg.close_on_exit = ini_int("Programs", "CloseOnExit", 1);
    cfg.view_mode = ini_int("Explorer", "ViewMode", 0);
    cfg.show_hidden = ini_int("Explorer", "ShowHidden", 0);
    cfg.beep = ini_int("Sounds", "Beep", 1);
    for (int l = 0; l < 3; l++) {                          /* v41 (#336): the MRU lists, whole */
        for (int i = 0; i < mru_n[l]; i++) kfree(mru_e[l][i]);
        mru_n[l] = 0;
        for (int i = 0; i < mru_def[l].max; i++) {
            char k[16]; ksnprintf(k, sizeof k, "%s%d", mru_def[l].key, i);
            const char *v = ini_get(mru_def[l].sect, k);
            if (v && v[0] && (mru_e[l][mru_n[l]] = kstrdup(v))) mru_n[l]++;
        }
        mru_sync_cfg(l);
    }
    kfree(ini_buf); ini_buf = NULL;
}

/* v41 (#336): DOS95.INI written from a buffer that grows (it was 16 KB, each line cut there) */
struct sbuf { char *b; int n, cap, bad; };
static void sb_printf(struct sbuf *s, const char *fmt, ...)
{
    for (;;) {
        if (s->bad) return;
        int room = s->cap - s->n;
        if (room > 1) {
            va_list ap; va_start(ap, fmt);
            int k = kvsnprintf(s->b + s->n, room, fmt, ap);
            va_end(ap);
            if (k >= 0 && k < room - 1) { s->n += k; return; }   /* (it fitted, with room to spare) */
        }
        int cap = s->cap ? s->cap * 2 : 16384;
        char *nb = kmalloc(cap);
        if (!nb) { s->bad = 1; return; }
        if (s->b) { memcpy(nb, s->b, s->n); kfree(s->b); }
        s->b = nb; s->cap = cap;
    }
}
void config_save(void)
{
    struct sbuf sb = { 0 }, *S = &sb;
    sb_printf(S, "; DOS95 configuration\r\n[Display]\r\nWidth=%d\r\nHeight=%d\r\nBpp=%d\r\nBackground=0x%06x\r\nWallpaper=%s\r\nWallpaperMode=%d\r\nPattern=%d\r\nColorScheme=%d\r\n",
                   cfg.scr_w, cfg.scr_h, cfg.scr_bpp, cfg.desktop_color, cfg.wallpaper, cfg.wallpaper_mode, cfg.pattern, cfg.color_scheme);
    sb_printf(S, "ScreenSaver=%d\r\nScreenSaverWait=%d\r\nCustomColors=%d\r\n", cfg.screensaver, cfg.screensaver_min, cfg.custom_colors);
    sb_printf(S, "; Theme: an IceWM theme's folder in C:\\DOS95\\THEMES (none: DOS95's own look); ThemeBackground=1: the desktop is the theme's\r\n"
                 "Theme=%s\r\nThemeBackground=%d\r\n", cfg.theme, cfg.theme_bg);   /* v44 (E1) */
    sb_printf(S, "; Display Properties > Effects: the caption's animation (minimize, maximize, restore), menus' (MenuFade=1 a fade, 0 a scroll), windows moved with their contents\r\n"
                 "MinAnimate=%d\r\nMenuAnimation=%d\r\nMenuFade=%d\r\nDragFullWindows=%d\r\n", cfg.fx_minanim, cfg.fx_menuanim, cfg.fx_menufade, cfg.fx_dragfull);   /* v44 (E2) */
    sb_printf(S, "\r\n[Desktop]\r\nAutoArrange=%d\r\nSort=%d\r\n", cfg.desktop_autoarrange, cfg.desktop_sort);
    sb_printf(S, "\r\n[Network]\r\n; Adapter: 0 auto-detect, 1 NE2000 ISA, 2 AMD PCnet, 3 Realtek 8139, 4 Intel E1000, 5 NE2000 PCI, 6 DOS packet driver\r\n"
                   "Enabled=%d\r\nAdapter=%d\r\nIsaPort=0x%x\r\nIsaIrq=%d\r\nDhcp=%d\r\nIP=%s\r\nMask=%s\r\nGateway=%s\r\nDNS=%s\r\nHostname=%s\r\nPacketDriver=%s\r\n",
                   cfg.net_enabled, cfg.net_adapter, cfg.net_isa_port, cfg.net_isa_irq, cfg.net_dhcp, cfg.net_ip, cfg.net_mask, cfg.net_gw, cfg.net_dns, cfg.net_hostname, cfg.net_pktdrv);
    for (int i = 0; i < cfg_dns_more_n; i++) sb_printf(S, "DNS%d=%s\r\n", i + 2, cfg_dns_more[i]);
    sb_printf(S, "Domain=%s\r\n", cfg_dns_domain);
    for (int i = 0; i < cfg_dns_suffix_n; i++) sb_printf(S, "Suffix%d=%s\r\n", i + 1, cfg_dns_suffix[i]);
    sb_printf(S, "\r\n[Colors]\r\n");
    for (int i = 0; i < CE_COUNT; i++) sb_printf(S, "Color%d=0x%06x\r\n", i, cfg.colors[i]);
    for (int l = 0; l < 2; l++) {                          /* v41 (#336): the MRU lists, whole */
        sb_printf(S, "\r\n[%s]\r\n", mru_def[l].sect);
        for (int i = 0; i < mru_n[l]; i++) sb_printf(S, "%s%d=%s\r\n", mru_def[l].key, i, mru_e[l][i]);
    }
    sb_printf(S, "\r\n[Mouse]\r\nSpeed=%d\r\nDoubleClick=%d\r\nSwapButtons=%d\r\n", cfg.mouse_speed, cfg.dblclick_ms, cfg.mouse_swap);
    sb_printf(S, "\r\n[Keyboard]\r\nRepeatRate=%d\r\nDelay=%d\r\n", cfg.kbd_repeat, cfg.kbd_delay);
    sb_printf(S, "\r\n[Taskbar]\r\nShowClock=%d\r\nAutoHide=%d\r\nSmallIcons=%d\r\nAlwaysOnTop=%d\r\n", cfg.show_clock, cfg.taskbar_autohide, cfg.small_startmenu, cfg.always_on_top);
    sb_printf(S, "\r\n[Programs]\r\nCloseOnExit=%d\r\n", cfg.close_on_exit);
    sb_printf(S, "\r\n[Explorer]\r\nViewMode=%d\r\nShowHidden=%d\r\n", cfg.view_mode, cfg.show_hidden);
    sb_printf(S, "\r\n[Sounds]\r\nBeep=%d\r\nScheme=%d\r\n", cfg.beep, cfg.snd_scheme);
    { extern const char *const sound_event_keys[SE_COUNT]; for (int i = 0; i < SE_COUNT; i++) sb_printf(S, "%s=%s\r\n", sound_event_keys[i], cfg.snd_event[i]); }
    sb_printf(S, "\r\n[Mixer]\r\nMaster=%d\r\nWave=%d\r\nMidi=%d\r\nCD=%d\r\nLine=%d\r\nMic=%d\r\nPCSpeaker=%d\r\nBass=%d\r\nTreble=%d\r\nMute=%d\r\n",
                   cfg.mix_master, cfg.mix_wave, cfg.mix_midi, cfg.mix_cd, cfg.mix_line, cfg.mix_mic, cfg.mix_spk, cfg.mix_bass, cfg.mix_treble, cfg.mix_mute);
    { extern int sndmix_prefer_usb; sb_printf(S, "PreferUSB=%d\r\n", sndmix_prefer_usb); }   /* v43 (#510) */
    sb_printf(S, "\r\n[Java]\r\nHeapMB=%d\r\nConsole=%d\r\nVerbose=%d\r\nClassPath=%s\r\nOptions=%s\r\n", cfg.java_heap_mb, cfg.java_console, cfg.java_verbose, cfg.java_classpath, cfg.java_options);
    sb_printf(S, "\r\n[%s]\r\n", mru_def[MRU_DOCS].sect);
    for (int i = 0; i < mru_n[MRU_DOCS]; i++) sb_printf(S, "%s%d=%s\r\n", mru_def[MRU_DOCS].key, i, mru_e[MRU_DOCS][i]);
    /* v41 (#252): the joysticks - the types 0 none, 1 two axes and two buttons, 2 two and four, 3
     * three and two, 4 three and four, 5 four and four; each axis's calibration in PIT counts */
    sb_printf(S, "\r\n[Joystick]\r\nJoystick1=%d\r\nJoystick2=%d\r\n", cfg.joy_type[0], cfg.joy_type[1]);
    for (int i = 0; i < 4; i++)
        sb_printf(S, "Axis%dLeast=%d\r\nAxis%dCentre=%d\r\nAxis%dMost=%d\r\n", i + 1, cfg.joy_cal[i][0], i + 1, cfg.joy_cal[i][1], i + 1, cfg.joy_cal[i][2]);
    /* v41 (#251): the LPTs whose DOS printing is spooled, and the time a job ends after */
    sb_printf(S, "\r\n[Printers]\r\nCapturePorts=%s%s%s%s%s\r\nDosTimeout=%d\r\n",
                   cfg.lpt_capture & 1 ? "LPT1" : "", (cfg.lpt_capture & 1) && (cfg.lpt_capture & 6) ? "," : "",
                   cfg.lpt_capture & 2 ? "LPT2" : "", (cfg.lpt_capture & 2) && (cfg.lpt_capture & 4) ? "," : "",
                   cfg.lpt_capture & 4 ? "LPT3" : "", cfg.lpt_timeout);
    sb_printf(S, "\r\n[DateTime]\r\nTimeZone=%s\r\nAutoDST=%d\r\n", cfg.tz_name, cfg.tz_autodst);   /* v41 (#298) */
    { extern int midi_synth; extern char midi_soundfont[128];                                    /* v43 (#505): where MIDI is played */
      sb_printf(S, "\r\n[MIDI]\r\n; GM: General MIDI on a SoundFont (MIDISYN.D95; SoundFont= its file, none: C:\\DOS95\\SYSTEM\\GM.SF2,\r\n"
                   "; GeneralUser GS), MT32: a Roland MT-32 from the ROMs in C:\\DOS95\\MT32, FM: the FM chip,\r\n"
                   "; MPU401: the PC's own MIDI port (an MPU-401 at 330h) and what is connected to it\r\nSynth=%s\r\n",
                midi_synth == 1 ? "MT32" : midi_synth == 2 ? "FM" : midi_synth == 3 ? "MPU401" : "GM");
      if (midi_soundfont[0]) sb_printf(S, "SoundFont=%s\r\n", midi_soundfont); }
    { extern int tm_speed_ms, tm_cpu_graphs, tm_kernel_times, tm_on_top, tm_page;               /* v44 (M6) */
      sb_printf(S, "\r\n[TaskManager]\r\n; UpdateSpeed: 500 High, 1000 Normal, 4000 Low, 0 Paused; CpuGraphs: 1 one per processor\r\n"
                   "UpdateSpeed=%d\r\nCpuGraphs=%d\r\nKernelTimes=%d\r\nAlwaysOnTop=%d\r\nPage=%d\r\n", tm_speed_ms, tm_cpu_graphs, tm_kernel_times, tm_on_top, tm_page); }
    { extern int smp_sched_on;                                                                   /* v44 (M7): kept when it is not the default */
      if (!smp_sched_on) sb_printf(S, "\r\n[System]\r\n; Processors=0: every processor; 1: the first only\r\nProcessors=1\r\n"); }
    { extern int kmod_pg_mode, aspace_shr_strict;                                                /* v41 (#402, #403): kept when they are set */
      if (kmod_pg_mode || aspace_shr_strict) sb_printf(S, "\r\n[Memory]\r\n");
      if (kmod_pg_mode) sb_printf(S, "Pages=%d\r\n", kmod_pg_mode);
      if (aspace_shr_strict) sb_printf(S, "Strict=%d\r\n", aspace_shr_strict); }
    { extern int vstep_mode;                                                                     /* v41 (#412): kept when it is not the default */
      if (vstep_mode != 1) sb_printf(S, "\r\n[Test]\r\nStep=%d\r\n", vstep_mode); }
    { extern int ata_mode;                                                                       /* v41 (#412): kept when it is not the default */
      if (ata_mode != 1) sb_printf(S, "\r\n[Disk]\r\nDirect=%d\r\n", ata_mode); }
    char path[160]; path_join(path, sizeof path, cfg.dos95dir, CFG_INI_NAME);
    if (!sb.bad && sb.b) fs_write_file(path, sb.b, sb.n);
    else kprintf("config: DOS95.INI not written - no memory\n");
    if (sb.b) kfree(sb.b);
}

void config_add_recent(const char *path) { mru_add(MRU_DOCS, path); }

/* ---- paths -------------------------------------------------------------------- */
/* v41 (#336): 0, or -1 when dir and name do not fit in n bytes - out is then "|", a path no file
 * has (DOS and the long-name layer refuse the character): until v41 the path was cut short, and
 * a delete, copy or rename past MAX_PATH acted on whatever file had the shortened name */
int path_join(char *out, int n, const char *dir, const char *name)
{
    if (n <= 0) return -1;
    int dl = strlen(dir), r;
    if (dl && dir[dl - 1] == '\\') r = ksnprintf(out, n, "%s%s", dir, name);
    else r = ksnprintf(out, n, "%s\\%s", dir, name);
    if (r < 0 || r >= n) { strlcpy(out, "|", n); return -1; }
    return 0;
}
const char *path_basename(const char *p)
{
    const char *b = strrchr(p, '\\');
    if (!b) b = strrchr(p, ':');
    return b ? b + 1 : p;
}
void path_dirname(const char *p, char *out, int n)
{
    strlcpy(out, p, n);
    char *b = strrchr(out, '\\');
    if (!b) { if (out[1] == ':') out[2] = 0; else out[0] = 0; return; }
    if (b == out + 2 && out[1] == ':') b[1] = 0;   /* keep "C:\" */
    else *b = 0;
}
const char *path_ext(const char *p)
{
    const char *b = path_basename(p);
    const char *e = strrchr(b, '.');
    return e ? e + 1 : "";
}
int path_is_root(const char *p) { int l = strlen(p); return (l == 3 && p[1] == ':' && p[2] == '\\') || (l == 2 && p[1] == ':'); }

/* ---- file types ----------------------------------------------------------------- */
int shell_file_type(const char *name, int attr)
{
    if (attr & FA_DIR) return FT_DIR;
    const char *e = path_ext(name);
    if (!stricmp(e, "EXE") || !stricmp(e, "COM")) return FT_EXE;
    if (!stricmp(e, "BAT")) return FT_BAT;
    if (!stricmp(e, "JAR")) return FT_JAR;
    if (!stricmp(e, "D95")) return FT_D95;
    if (!stricmp(e, "DSL")) return FT_SHORTCUT;
    if (*e && reg_value("Associations", e)) return FT_DOC;   /* claimed by an installed program */
    if (!stricmp(e, "TXT") || !stricmp(e, "ME") || !stricmp(e, "1ST") || !stricmp(e, "NFO") || !stricmp(e, "DIZ") || !stricmp(e, "LOG") || !stricmp(e, "C") || !stricmp(e, "H") || !stricmp(e, "ASM") || !stricmp(e, "PAS") || !stricmp(e, "BAS")) return FT_TEXT;
    if (!stricmp(e, "BMP") || !stricmp(e, "PCX") || !stricmp(e, "GIF")) return FT_IMAGE;
    if (!stricmp(e, "INI") || !stricmp(e, "SYS") || !stricmp(e, "CFG")) return FT_INI;
    return FT_OTHER;
}

/* Which program opens a document.
 *
 * The OS holds no list of applications.  An installed program claims its own
 * extensions in REGISTRY.INI:
 *
 *     [Associations]
 *     RTF=WORDPAD.D95
 *     DOC=WORDPAD.D95
 *     BMP=PAINT.D95
 *
 * and names its document type in [FileTypes] for Explorer's Type column:
 *
 *     [FileTypes]
 *     RTF=Rich Text Document
 *
 * An extension nobody claims falls back to the built-in editor, and a claim
 * whose program is not installed falls back the same way, so an entry left
 * behind by a removed program is harmless.
 */
/* A registry value that names a program, resolved to a full path.  Relative names
 * are taken as living in the DOS95 directory.  NULL if nothing is registered or the
 * program named is not installed, so a stale entry falls back instead of failing. */
const char *reg_program(const char *section, const char *key)
{
    static char path[260];                          /* (v41, #336: MAX_PATH - a path past 199 bytes was "not installed") */
    const char *prog = reg_value(section, key);
    if (!prog || !*prog) return NULL;
    if (prog[1] == ':' || prog[0] == '\\') {        /* an absolute path is taken as given */
        if (strlen(prog) >= sizeof path) return NULL;
        strlcpy(path, prog, sizeof path);
    } else if (path_join(path, sizeof path, cfg.dos95dir, prog) < 0) return NULL;
    return fs_exists(path) ? path : NULL;
}

const char *shell_assoc_for(const char *name)
{
    const char *e = path_ext(name);
    if (!*e) return NULL;
    return reg_program("Associations", e);
}

int shell_icon_for(const char *name, int attr)
{
    switch (shell_file_type(name, attr)) {
    case FT_DIR: return ICO_FOLDER;
    case FT_EXE: return ICO_DOSAPP;
    case FT_BAT: return ICO_BATCH;
    case FT_SHORTCUT: return ICO_DOSAPP;
    case FT_TEXT: return ICO_TEXT;
    case FT_DOC: return ICO_TEXT;
    case FT_IMAGE: return ICO_IMAGE;
    case FT_INI: return ICO_INI;
    case FT_JAR: return ICO_JAVA;
    case FT_D95: return ICO_LOGO;
    default: return ICO_FILE;
    }
}

const char *shell_type_name(int ft, const char *name)
{
    static char buf[300];                           /* (v41, #336: a type's whole description - 39 bytes were shown) */
    switch (ft) {
    case FT_DIR: return "File Folder";
    case FT_EXE: return "DOS Application";
    case FT_BAT: return "MS-DOS Batch File";
    case FT_SHORTCUT: return "Shortcut";
    case FT_TEXT: return "Text Document";
    case FT_DOC: {
        const char *e = path_ext(name);
        const char *d = *e ? reg_value("FileTypes", e) : NULL;
        if (d && *d) { strlcpy(buf, d, sizeof buf); return buf; }
        if (*e) { char up[256]; strlcpy(up, e, sizeof up); for (char *q = up; *q; q++) *q = toupper(*q);
                  ksnprintf(buf, sizeof buf, "%s Document", up); return buf; }
        return "Document"; }
    case FT_IMAGE: return "Bitmap Image";
    case FT_INI: return "Configuration Settings";
    case FT_DRIVE: return "Local Disk";
    case FT_D95: return "DOS95 Application";
    default: {
        const char *e = path_ext(name);
        if (*e) { char up[256]; strlcpy(up, e, sizeof up); for (char *p = up; *p; p++) *p = toupper(*p); ksnprintf(buf, sizeof buf, "%s File", up); return buf; }
        return "File"; }
    }
}

/* ---- shortcuts ------------------------------------------------------------------- */
static const char *icon_names[] = { "COMPUTER", "HDD", "FLOPPY", "CDROM", "NETDRIVE", "FOLDER", "FOLDEROPEN", "DOSAPP", "BATCH", "TEXT", "FILE", "IMAGE", "INI",
    "CONTROLPANEL", "DISPLAY", "MOUSE", "KEYBOARD", "SYSTEM", "DATETIME", "TASKBAR", "TRASH", "TRASHFULL", "SHUTDOWN", "RUN", "HELP", "FIND", "SETTINGS",
    "PROGRAMS", "DOCUMENTS", "TASKMAN", "INFO", "WARNING", "ERROR", "QUESTION", "EXPLORER", "LOGO", "SHORTCUT", "DESKTOP", "FONTS", "SOUND", "NETWORK", "PRINTER", "CLOCK",
    "JAVA" };

int shell_icon_by_name(const char *n)
{
    for (int i = 0; i < (int)ARRAY_LEN(icon_names) && i < ICO_COUNT; i++) if (!stricmp(icon_names[i], n)) return i;
    if (isdigit(n[0])) { int v = atoi(n); if (v >= 0 && v < ICO_COUNT) return v; }
    /* not one of the OS's own: a program supplying its own icon names a .BMP,
     * either absolute or relative to the DOS95 directory */
    if (strchr(n, '.')) {
        char p[260];
        if (n[1] == ':' || n[0] == '\\') { if (strlen(n) >= sizeof p) return -1; strlcpy(p, n, sizeof p); }
        else if (path_join(p, sizeof p, cfg.dos95dir, n) < 0) return -1;
        if (fs_exists(p)) return icon_register_file(p);
    }
    return -1;
}
const char *shell_icon_name(int id) { return (id >= 0 && id < (int)ARRAY_LEN(icon_names)) ? icon_names[id] : "DOSAPP"; }

int shortcut_load(const char *path, struct shortcut *sc)
{
    int n; char *buf = kread_text(path, &n);       /* (v41, #336: all of it - 2047 bytes were read, the rest left out) */
    if (!buf) return -1;
    char *save = ini_buf; int savelen = ini_len;
    ini_buf = buf; ini_len = n;
    memset(sc, 0, sizeof *sc);
    ini_str("Shortcut", "Target", sc->target, sizeof sc->target, "");
    ini_str("Shortcut", "Args", sc->args, sizeof sc->args, "");
    ini_str("Shortcut", "WorkDir", sc->workdir, sizeof sc->workdir, "");
    ini_str("Shortcut", "Title", sc->title, sizeof sc->title, "");
    ini_str("Shortcut", "Icon", sc->iconname, sizeof sc->iconname, "");
    sc->icon = sc->iconname[0] ? shell_icon_by_name(sc->iconname) : -1;
    sc->close_on_exit = ini_int("Shortcut", "CloseOnExit", cfg.close_on_exit);
    sc->speed = ini_int("Shortcut", "Speed", 100);      /* (v43, #507) */
    if (sc->speed <= 0 || sc->speed > 100) sc->speed = 100;
    ini_buf = save; ini_len = savelen;
    kfree(buf);
    if (!sc->title[0]) { strlcpy(sc->title, path_basename(path), sizeof sc->title); char *d = strrchr(sc->title, '.'); if (d) *d = 0; }
    return 0;
}

int shortcut_save(const char *path, const struct shortcut *sc)
{
    char sp[24] = "";                                   /* (v43, #507: a speed below full - none written for full) */
    if (sc->speed > 0 && sc->speed < 100) ksnprintf(sp, sizeof sp, "Speed=%d\r\n", sc->speed);
    char *buf = kaprintf("[Shortcut]\r\nTarget=%s\r\nArgs=%s\r\nWorkDir=%s\r\nTitle=%s\r\nIcon=%s\r\nCloseOnExit=%d\r\n%s",
                      sc->target, sc->args, sc->workdir, sc->title,
                      sc->iconname[0] ? sc->iconname : sc->icon >= 0 ? shell_icon_name(sc->icon) : "",   /* v38: a .BMP stays a .BMP */
                      sc->close_on_exit, sp);   /* (v41, #336: whole - 699 bytes were written, the last line or more lost) */
    if (!buf) return -1;
    int r = fs_write_file(path, buf, strlen(buf)) < 0 ? -1 : 0;
    kfree(buf);
    return r;
}

int shortcut_create_for(const char *target, const char *dest_dir, char *out_path, int n)
{
    struct shortcut sc; memset(&sc, 0, sizeof sc);
    /* v40 (#192): a shortcut holds 127 bytes of path (struct shortcut, exported): a longer
     * target is kept as its 8.3 form */
    if (strlen(target) < sizeof sc.target || fs_shortname(target, sc.target, sizeof sc.target) < 0)
        strlcpy(sc.target, target, sizeof sc.target);
    path_dirname(sc.target, sc.workdir, sizeof sc.workdir);
    const char *base = path_basename(target);
    /* v41 (#336): the target's whole name (until v41 its first 63 bytes - and past 79 the file's
     * name lost its own .dsl), shortened only so "Shortcut (n) to NAME.dsl" is still a long name */
    char name[256]; strlcpy(name, base, sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    if (path_is_root(target)) ksnprintf(name, sizeof name, "(%c)", toupper(target[0]));   /* v41 (#190): a drive, dragged */
    if (strlen(name) > 223) { int k = 223; while (k > 0 && ((uint8_t)name[k] & 0xC0) == 0x80) k--; name[k] = 0; }
    strlcpy(sc.title, name, sizeof sc.title);
    int attr = fs_getattr(target);
    sc.icon = shell_icon_for(target, attr < 0 ? 0 : attr);
    if (path_is_root(target)) { int t = fs_drive_type(toupper(target[0]) - 'A'); sc.icon = t == 1 ? ICO_DRIVE_FLOPPY : t == 3 ? ICO_DRIVE_CD : ICO_DRIVE_HDD; }
    if (sc.icon == ICO_FILE) sc.icon = -1;
    sc.close_on_exit = cfg.close_on_exit;
    /* numbered on past (99), never over a shortcut that is there (v41, #336) */
    char first[300], post[300], first83[16], stem83[9];
    ksnprintf(first, sizeof first, "Shortcut to %s.dsl", name);
    ksnprintf(post, sizeof post, ") to %s.dsl", name);
    { char s8[9]; strlcpy(s8, name, sizeof s8); ksnprintf(first83, sizeof first83, "%s.DSL", s8); }
    strlcpy(stem83, name, sizeof stem83);
    int r = fileops_unique_name(dest_dir, first, "Shortcut (", post, first83, stem83, ".DSL", out_path, n);
    if (r < 0) return r;
    return shortcut_save(out_path, &sc);
}
