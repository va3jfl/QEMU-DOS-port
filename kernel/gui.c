/* DOS95 GUI - main thread: video setup, desktop, event loop */
#include "shell.h"
#include "vesa.h"
#include "app.h"

extern int mouse_x, mouse_y, mouse_speed, mouse_swap;
extern void mouse_set_bounds(int w, int h);
extern void wm_main_loop(void);
extern void kbd_set_typematic(int rate, int delay);

static void ensure_dirs(void)
{
    fs_mkdir(cfg.desktop_dir);
    fs_mkdir(cfg.startmenu_dir);
    char p[200];
    path_join(p, sizeof p, cfg.startmenu_dir, "PROGRAMS"); fs_mkdir(p);
    path_join(p, sizeof p, cfg.dos95dir, "RECYCLED"); fs_mkdir(p);
    path_join(p, sizeof p, cfg.dos95dir, "FONTS"); fs_mkdir(p);
}

/* first run: create default shortcuts */
static void first_run(void)
{
    char p[200], q[200];
    path_join(p, sizeof p, cfg.startmenu_dir, "PROGRAMS");
    path_join(q, sizeof q, p, "*.*");
    struct fs_dirent e; struct fs_find fh;
    int empty = 1;
    if (fs_findfirst(q, 0x37, &e, &fh) == 0) { do { if (strcmp(e.name, ".") && strcmp(e.name, "..")) { empty = 0; break; } } while (fs_findnext(&fh, &e) == 0); fs_findclose(&fh); }
    if (!empty) return;
    struct shortcut sc;
    memset(&sc, 0, sizeof sc);
    /* v41 (#336, M6): the DOS's own command interpreter, whichever DOS this is - %COMSPEC%, not its
     * path at the first boot (C:\FREEDOS\BIN\COMMAND.COM made every such shortcut a dead one on
     * MS-DOS 7.1, whose is C:\COMMAND.COM) */
    strcpy(sc.target, "%COMSPEC%"); strcpy(sc.workdir, "C:\\"); strcpy(sc.title, "MS-DOS Prompt"); sc.icon = ICO_DOSAPP; sc.close_on_exit = 1;
    path_join(q, sizeof q, p, lfn_available ? "MS-DOS Prompt.dsl" : "DOSPRMPT.DSL"); shortcut_save(q, &sc);
    path_join(q, sizeof q, cfg.desktop_dir, lfn_available ? "MS-DOS Prompt.dsl" : "DOSPRMPT.DSL"); shortcut_save(q, &sc);
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "%COMSPEC%"); strcpy(sc.args, "/C EDIT"); strcpy(sc.workdir, "C:\\"); strcpy(sc.title, "Edit"); sc.icon = ICO_TEXT; sc.close_on_exit = 1;
    path_join(q, sizeof q, p, lfn_available ? "Editor.dsl" : "EDIT.DSL"); shortcut_save(q, &sc);
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "dos95:explorer"); strcpy(sc.title, "DOS95 Explorer"); sc.icon = ICO_EXPLORER;
    path_join(q, sizeof q, p, lfn_available ? "DOS95 Explorer.dsl" : "EXPLORER.DSL"); shortcut_save(q, &sc);
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "dos95:taskman"); strcpy(sc.title, "Task Manager"); sc.icon = ICO_TASKMAN;
    path_join(q, sizeof q, p, lfn_available ? "Task Manager.dsl" : "TASKMAN.DSL"); shortcut_save(q, &sc);
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "dos95:cpanel"); strcpy(sc.title, "Control Panel"); sc.icon = ICO_CONTROL_PANEL;
    path_join(q, sizeof q, p, lfn_available ? "Control Panel.dsl" : "CPANEL.DSL"); shortcut_save(q, &sc);
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "C:\\"); strcpy(sc.title, "Drive C"); sc.icon = ICO_DRIVE_HDD;
    path_join(q, sizeof q, cfg.desktop_dir, lfn_available ? "Drive C.dsl" : "DRIVEC.DSL"); shortcut_save(q, &sc);
}

/* Always keep an Accessories folder in Programs populated with the registered
 * native apps (creates missing shortcuts; leaves the user's own items alone). */
static void seed_apps(void)
{
    char p[220], acc[240], q[300];
    path_join(p, sizeof p, cfg.startmenu_dir, "PROGRAMS");
    path_join(acc, sizeof acc, p, "Accessories"); fs_mkdir(acc);
    extern const struct app *app_list(void);
    for (const struct app *a = app_list(); a; a = a->next) {
        if (a->hidden) continue;
        char fn[96]; ksnprintf(fn, sizeof fn, lfn_available ? "%s.dsl" : "%.8s.DSL", a->title);
        path_join(q, sizeof q, acc, fn);
        if (fs_exists(q)) continue;
        struct shortcut sc; memset(&sc, 0, sizeof sc);
        ksnprintf(sc.target, sizeof sc.target, "dos95:app:%s", a->name);
        strlcpy(sc.title, a->title, sizeof sc.title); sc.icon = a->icon;
        shortcut_save(q, &sc);
    }
}

/* A Windows 3.1 installation on C: gets a desktop + Start-menu shortcut that runs it
 * in standard mode inside its own DOS95 window. */
static void windows_shortcut(void)
{
    if (!fs_exists("C:\\WINDOWS\\WIN.COM")) return;
    char p[200], q[200];
    path_join(p, sizeof p, cfg.startmenu_dir, "PROGRAMS");
    path_join(q, sizeof q, cfg.desktop_dir, lfn_available ? "Windows 3.1.dsl" : "WIN31.DSL");
    if (fs_exists(q)) return;
    struct shortcut sc;
    memset(&sc, 0, sizeof sc);
    strcpy(sc.target, "%COMSPEC%"); strcpy(sc.args, "/C WIN /S");
    strcpy(sc.workdir, "C:\\WINDOWS"); strcpy(sc.title, "Windows 3.1"); sc.icon = ICO_DOSAPP; sc.close_on_exit = 1;
    shortcut_save(q, &sc);
    path_join(q, sizeof q, p, lfn_available ? "Windows 3.1.dsl" : "WIN31.DSL"); shortcut_save(q, &sc);
}

/* Change the screen mode while the desktop is running (Display Properties / Settings).
 * Returns 0 on success; on failure the old mode is restored. */
volatile uint32_t display_gen;                          /* v41 (#195): one more at each mode change */
int display_bpp(void) { return vesa_cur.bpp; }
int gui_set_mode(int w, int h, int bpp)
{
    struct vesa_mode *m = vesa_choose(w, h, bpp);   /* (that size's when there is one; v42: on one frame buffer, its size) */
    if (!m) return -1;
    if (m->w == vesa_cur.w && m->h == vesa_cur.h && m->bpp == vesa_cur.bpp) return 0;
    struct vesa_mode old = vesa_cur;
    gfx_cursor_show(0);
    if (vesa_set(m)) { gfx_cursor_show(1); return -1; }
    gfx_free();
    if (gfx_init(m->lfb, m->w, m->h, m->pitch, m->bpp, m->rpos, m->rsize, m->gpos, m->gsize, m->bpos, m->bsize)) {
        vesa_set(&old);
        gfx_init(old.lfb, old.w, old.h, old.pitch, old.bpp, old.rpos, old.rsize, old.gpos, old.gsize, old.bpos, old.bsize);
        m = &old;
    }
    mouse_set_bounds(m->w, m->h);
    extern void wm_screen_resized(void);
    wm_screen_resized();
    gfx_cursor_move(scr_w / 2, scr_h / 2);
    gfx_cursor_show(1);
    if (m != &old) { display_gen++; kprintf("gui: mode %dx%dx%d (display_gen %u)\n", scr_w, scr_h, vesa_cur.bpp, (unsigned)display_gen); wm_display_changed(); }
    return m == &old ? -1 : 0;
}

/* ---- v40 (#162): a program's screen mode --------------------------------------------------
 * A game asks for 640x480 (DirectDraw, Direct3D, ChangeDisplaySettings under Wine) and got a
 * pretend mode: its screen that size in the top left of DOS95's 800x600, the desktop and the
 * taskbar around it.  Now the display switches, as Windows switches it; the mode DOS95 had
 * comes back when the program asks (0x0x0), or when it ends - whichever program changed it
 * last.  Done on the GUI thread, which owns the screen (jw_call from a program's thread). */
static int user_w, user_h, user_bpp, mode_pid;          /* the mode before a program's; its pid */
struct modereq { int w, h, bpp, pid, result; };
static void mode_worker(void *a)
{
    struct modereq *r = a;
    if (!r->w) {                                           /* back to what it was */
        if (!mode_pid) { r->result = 0; return; }
        r->result = gui_set_mode(user_w, user_h, user_bpp);
        mode_pid = 0;
        return;
    }
    if (!mode_pid) { user_w = scr_w; user_h = scr_h; user_bpp = vesa_cur.bpp; }
    r->result = gui_set_mode(r->w, r->h, r->bpp);
    if (r->result == 0) mode_pid = r->pid;
    else if (!mode_pid) r->result = -1;
}
int display_mode(int w, int h, int bpp)
{
    extern struct kmod *kmod_self(void);
    extern int kmod_pid(struct kmod *m);
    extern void jw_call(void (*fn)(void *), void *arg);
    if (w < 0 || h < 0 || (w && (w < 320 || h < 200))) return -22;
    struct modereq r = { w, h, bpp ? bpp : vesa_cur.bpp, kmod_pid(kmod_self()), -1 };
    jw_call(mode_worker, &r);
    return r.result;
}
/* the modes the display has: (width, height, bits) triples into out, how many there are */
int display_modes(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < vesa_nmodes; i++) {
        if (out && n < max) { out[3 * n] = vesa_modes[i].w; out[3 * n + 1] = vesa_modes[i].h; out[3 * n + 2] = vesa_modes[i].bpp; }
        n++;
    }
    return n;
}
/* a program ended (kmod.c, on the GUI thread): the mode it set goes */
void display_mode_end(int pid)
{
    if (!mode_pid || mode_pid != pid) return;
    struct modereq r = { 0, 0, 0, pid, 0 };
    mode_worker(&r);
}

void gui_main(void *arg)
{
    (void)arg;
    kprintf("gui: starting\n");
    { extern void dos_handles_init(void); dos_handles_init(); }   /* v41 (#332): before the first file */
    config_load();
    extern void bootcon_say(const char *s), bootcon_stop(void);
    if (!vesa_enumerate()) {
        kprintf("gui: no VESA modes\n");
        /* (v41, #437) said where it is seen, not only on COM1 */
        bootcon_say("DOS95 cannot start its desktop: this graphics card's BIOS has no VESA mode of");
        bootcon_say("15, 16, 24 or 32 bits a pixel.  A VESA driver for the card (UniVBE) gives it them.");
        kernel_shutdown(0);
    }
    struct vesa_mode *m = vesa_choose(cfg.scr_w, cfg.scr_h, cfg.scr_bpp);
    bootcon_stop();                                   /* (v41, #437: the screen is the desktop's from here) */
    if (!m || vesa_set(m)) { kprintf("gui: cannot set mode\n"); vesa_text_mode(); kernel_shutdown(0); }
    if (gfx_init(m->lfb, m->w, m->h, m->pitch, m->bpp, m->rpos, m->rsize, m->gpos, m->gsize, m->bpos, m->bsize)) kernel_shutdown(0);
    mouse_set_bounds(m->w, m->h);
    mouse_speed = cfg.mouse_speed; mouse_swap = cfg.mouse_swap; dblclick_ms = cfg.dblclick_ms;
    kbd_set_typematic(cfg.kbd_repeat, cfg.kbd_delay);
    wm_init();
    cursor_set(CUR_WAIT);
    extern void splash_begin(void), splash_step(void), splash_end(void);
    splash_begin();                       /* animated boot status screen */
    gfx_cursor_move(scr_w / 2, scr_h / 2);
    splash_step();                        /* Display */
#ifndef QDP
    ensure_dirs();
#endif
    { extern void fontmgr_init(void); fontmgr_init(); }   /* built-ins, then the fonts directory */
    splash_step();                        /* File system */
    { extern void pci_init(void); pci_init(); }
    { extern void usb_init(void); usb_init(); }       /* (v42, #471: USB keyboards and mice - its own thread) */
    { extern void net_init(void); if (cfg.net_enabled) net_init(); }
    splash_step();                        /* Network */
    { extern void snd_mixer_apply_config(void); snd_mixer_apply_config(); }
    { extern void lpt_init(void), lpt_start(void); lpt_init(); lpt_start(); }   /* v41 (#251): the printer ports */
    splash_step();                        /* Audio */
    { extern void apps_init(void); apps_init(); }
#ifdef QDP
    /* the QEMU DOS port: no desktop - its own shell (qdp/kernel/qdpshell.c): the manager, or the setup */
    splash_step();                        /* Services */
    splash_step();                        /* Machines */
    splash_end();
    gfx_fill(0, 0, scr_w, scr_h, 0x0A1B3D);
    gfx_flush_all();
    gfx_cursor_show(1);
    cursor_set(CUR_ARROW);
    kprintf("gui: QEMU DOS port shell (%dx%dx%d)\n", scr_w, scr_h, scr_bpp);
    { extern void qdp_shell_start(void); qdp_shell_start(); }
    wm_main_loop();
    { extern void dos_handles_fini(void); dos_handles_fini(); }
    kernel_shutdown(0);
#else
    first_run();
    seed_apps();
    { extern void java_seed_shortcuts(void); java_seed_shortcuts(); }
    splash_step();                        /* Services */
    windows_shortcut();
    splash_step();                        /* Applications */
    desktop_init();
    splash_step();                        /* Desktop */
    splash_end();
    gfx_fill(0, 0, scr_w, scr_h, cfg.desktop_color);
    gfx_flush_all();
    gfx_cursor_show(1);
    cursor_set(CUR_ARROW);
    sound_event(SE_START);
    kprintf("gui: desktop ready (%dx%dx%d)\n", scr_w, scr_h, scr_bpp);
    { extern void tzclock_start(void); tzclock_start(); }   /* v41 (#299): the zone's clock (kernel/cpanel.c) */
    /* StartUp: every shortcut in <start menu>\PROGRAMS\STARTUP is launched at boot */
    { char sp[260], pat[260]; path_join(sp, sizeof sp, cfg.startmenu_dir, "PROGRAMS\\STARTUP");   /* (v41, #336: MAX_PATH - a long name past 239 bytes was not started) */
      path_join(pat, sizeof pat, sp, "*.*");
      struct fs_dirent e; struct fs_find fh;
      if (fs_findfirst(pat, 0x27, &e, &fh) == 0) {
          do {
              if (e.attr & FA_DIR) continue;
              char q[260]; if (path_join(q, sizeof q, sp, e.name) < 0) continue;
              shell_open(q, NULL);
          } while (fs_findnext(&fh, &e) == 0);
          fs_findclose(&fh);
      } }
    wm_main_loop();
    { extern void dos_handles_fini(void); dos_handles_fini(); }   /* v41 (#332): DOS's file tables back */
    kernel_shutdown(0);
#endif
}
