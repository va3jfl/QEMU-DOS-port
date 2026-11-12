/* QEMU DOS port - what the kernel's own code calls in the parts the QEMU DOS port leaves out of DOS95's
 * kernel (Makefile.qdp QDP_OUT): the desktop and its programs, the shell, drag and drop, the Java runtime,
 * the DOS boxes' windows and the system sounds.  Each one here answers as DOS95 does when the part has
 * nothing to do: no taskbar (0 pixels high), no drop target, no program started by the shell, no sound
 * scheme.  The QEMU DOS port's own shell is qdpshell.c. */
#include "shell.h"
#include "app.h"

/* ---- desktop.c: the taskbar, the desktop, Alt+Tab ---------------------------------------------------- */
extern void qdp_hotkey(int code);
void desktop_hotkey(int code) { qdp_hotkey(code); }
int  shell_set_wallpaper(const char *path, int style) { return -1; }
int  switcher_key_hook(struct event *ev) { return 0; }
int  taskbar_button_rect(struct window *w, struct rect *r) { return 0; }
int  taskbar_h(void) { return 0; }
void taskbar_refresh(void) { }
void taskbar_zorder(void) { }

/* ---- dnd.c: drag and drop between programs --------------------------------------------------------- */
int  dnd_answer(int a, int b) { return -1; }
int  dnd_current(int *a) { return 0; }
int  dnd_data(int a, int b, char *c, int d) { return -1; }
int  dnd_end(int a) { return -1; }
int  dnd_move(int a, int b, int c) { return -1; }
int  dnd_set_data(const char *a, int b, const char *c, int d) { return -1; }
int  dnd_start(const char *a, int b, const char *c, int d, int e) { return -1; }
void dnd_window_gone(struct window *w) { }
int  dnd_wm_key(int a, int b, int c) { return 0; }
int  dnd_wm_mouse(int a, int b, int c, int d, int e) { return 0; }
void wm_accept_drops(struct window *w, int on) { }

/* ---- explorer.c, find.c, shell.c, shellext.c: the shell ----------------------------------------------- */
void explorer_ctx_program_ended(int pid) { }
struct window *explorer_open(const char *path) { return NULL; }
void explorer_refresh_all(void) { }
void find_dialog_in(const char *dir) { }
int  shell_open(const char *path, struct window *owner) { return -1; }
void shell_properties(const char *path, struct window *owner) { }
int  shell_run_command(const char *cmd, struct window *owner) { return -1; }
int  shell_run_program(const char *a, const char *b, const char *c, const char *d, int e) { return -1; }
int  shell_windows_exe(const char *path) { return 0; }
int  shx_answer(const char *buf, int len) { return -1; }
int  shx_next(char *buf, int max, int ms) { return -1; }
void shx_program_ended(int pid) { }

/* ---- fileops.c: a file name not yet taken in a folder (the QEMU manager's new machine files) -------------
 * first (the long name), else pre + n + post (2, 3, ...); without long names first83, else stem83 cut so
 * that "~n" fits, + ext83.  The path written to out; 0, or -1 (none free, or out too small) */
int fileops_unique_name(const char *dir, const char *first, const char *pre, const char *post,
                        const char *first83, const char *stem83, const char *ext83, char *out, int n)
{
    char name[260];
    for (int k = 1; k < 1000; k++) {
        if (lfn_available) {
            if (k == 1) strlcpy(name, first, sizeof name);
            else ksnprintf(name, sizeof name, "%s%d%s", pre, k, post);
        } else {
            if (k == 1) strlcpy(name, first83, sizeof name);
            else {
                char num[8]; ksnprintf(num, sizeof num, "~%d", k);
                int keep = 8 - (int)strlen(num); if (keep < 1) keep = 1;
                char stem[9]; strlcpy(stem, stem83, sizeof stem); if ((int)strlen(stem) > keep) stem[keep] = 0;
                ksnprintf(name, sizeof name, "%s%s%s", stem, num, ext83);
            }
        }
        if (path_join(out, n, dir, name) < 0) return -1;
        if (!fs_exists(out)) return 0;
    }
    return -1;
}

/* ---- javaapp.c, javawin.c: the Java runtime ------------------------------------------------------------ */
int  java_heap_mb(void) { return 0; }
const char *java_home_dir(void) { return ""; }
int  java_launch(const char *cmdline, const char *title) { return -1; }
void java_seed_shortcuts(void) { }
void javaapp_register(void) { }
/* a function run on the GUI thread (gui.c display_mode: a program's screen mode) - the QEMU DOS port has
 * no program that asks, and its GUI thread is the only caller */
void jw_call(void (*fn)(void *), void *arg) { fn(arg); }

/* ---- saver.c: the screen saver ---------------------------------------------------------------------- */
void saver_check(void) { }
void saver_idle_reset(void) { }

/* ---- wave.c: the system sounds (none in the QEMU DOS port) --------------------------------------------- */
const char *const sound_event_names[SE_COUNT] = {
    "Default sound", "Asterisk", "Critical Stop", "Exclamation", "Question",
    "Start", "Exit", "Menu popup", "Open program", "Close program",
    "Maximize", "Minimize", "Restore Up", "Restore Down", "Empty Recycle Bin",
};
const char *const sound_event_keys[SE_COUNT] = {
    "Default", "Asterisk", "CriticalStop", "Exclamation", "Question", "Start", "Exit",
    "MenuPopup", "Open", "Close", "Maximize", "Minimize", "RestoreUp", "RestoreDown", "EmptyRecycle",
};
const char *const sound_scheme_default[SE_COUNT] = { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" };
void sound_event(int ev)
{
    if ((ev == SE_CRITICAL || ev == SE_EXCLAMATION || ev == SE_DEFAULT) && cfg.beep) { extern void kbd_beep(void); kbd_beep(); }
}
int  wave_play(const char *path) { return -1; }

/* ---- taskman.c, cpanel.c ------------------------------------------------------------------------------ */
int tm_speed_ms = 1000, tm_cpu_graphs = 1, tm_kernel_times, tm_on_top = 1, tm_page;
const char *tz_env_value(void) { return cfg.tz_name; }
void tzclock_start(void) { }

/* ---- vmwin.c: DOS boxes' windows (the QEMU DOS port starts none) - the events of the system's own
 * windows routed as vmwin.c routes them: the QEMU machine's, a program's, a console's ------------------- */
void vmwin_event(struct event *ev)
{
    if (ev->type == EV_VMTITLE && ev->vmid == 0xFFFF) { extern void vpc_event(struct event *ev); vpc_event(ev); return; }
    if (ev->type == EV_VMTITLE && ev->vmid == 0xFFFC) { extern void kmod_event(struct event *ev); kmod_event(ev); return; }
    if (ev->type == EV_VMTITLE && ev->vmid == 0xFFFD) { extern void console_event(struct event *ev); console_event(ev); return; }
    if (ev->type == EV_VMEXIT && ev->vmid < nvms && vms[ev->vmid] && !(vms[ev->vmid]->flags & VMF_SYSTEM)) vm_destroy(vms[ev->vmid]);
}
struct window *vmwin_launch(const char *a, const char *b, const char *c, const char *d, int e) { return NULL; }
void vmwin_refresh(struct vm *vm) { }

/* ---- shell.c: the beep of a message box (the PC speaker, ~800 Hz, 60 ms) ------------------------------------------ */
void kbd_beep(void)
{
    if (!cfg.beep) return;
    outb(0x43, 0xB6);
    uint32_t div = 1193182 / 800;
    outb(0x42, div & 0xFF); outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
    thread_sleep_ms(60);
    outb(0x61, inb(0x61) & ~3);
}

/* ---- javaapp.c: a thread of the Java runtime's kind (kmod.c starts a ring-3 program's threads with it) - none here ---- */
void *jthread_create(const char *name, void (*fn)(void *), void *arg, int prio, unsigned stack) { return NULL; }
