/* QEMU DOS port - its shell: what the screen shows when no window covers it, the start (the manager, a machine
 * named on the command line, the autoboot, or the setup), the hotkeys, Exit to MS-DOS and Restart.
 *
 * DOS95's desktop, taskbar and Start menu are not in this kernel (Makefile.qdp): the QEMU manager (qdpman.c) is the
 * one window, maximized over the whole screen, and a machine runs over the whole screen too (kernel/vpc.c); the
 * window under both is this file's - a dark screen with the name and the keys. */
#include "shell.h"
#include "app.h"
#include "font.h"
#include "qdp.h"

int qdp_setup_mode, qdp_no_autoboot;
char qdp_start_machine[260];

const char *qdp_dir(void)
{
    static char d[160];
    strlcpy(d, cfg.dos95dir, sizeof d);
    int n = strlen(d);
    if (n > 3 && d[n - 1] == '\\') d[n - 1] = 0;          /* (C:\ keeps its backslash) */
    return d;
}

/* ---- big letters: the UI's bold font, each pixel a scale x scale block ---------------------------------------- */
int qdp_big_text_width(int scale, const char *s)
{
    const struct bmfont *f = &font_ui_bold;
    int w = 0;
    for (; *s; s++) { uint8_t c = (uint8_t)*s; if (c < f->first || c > f->last) c = '?'; w += bmf_width(f, c - f->first); }
    return w * scale;
}
void qdp_draw_big_text(int x, int y, int scale, uint32_t color, const char *s)
{
    const struct bmfont *f = &font_ui_bold;
    int h = bmf_height(f);
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s; if (c < f->first || c > f->last) c = '?';
        int idx = c - f->first, gw = bmf_width(f, idx);
        const uint8_t *g = bmf_glyph(f, idx);
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < gw; col++) {
                int on;
                if (f->aa) on = g[row * gw + col] >= 128;
                else on = (g[row * ((gw + 7) / 8) + col / 8] >> (7 - col % 8)) & 1;
                if (on) gfx_fill(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        x += gw * scale;
    }
}

/* ---- the screen under the windows -------------------------------------------------------------------------------- */
static uint32_t mix(uint32_t a, uint32_t b, int t)
{
    int ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba = a & 0xFF, rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((ra + (rb - ra) * t / 255) << 16) | ((ga + (gb - ga) * t / 255) << 8) | (ba + (bb - ba) * t / 255);
}
static int back_proc(struct window *w, struct wevent *e)
{
    if (e->type == WE_PAINT) {
        struct rect c; gfx_get_clip(&c);
        int y0 = MAX(c.y, w->cy), y1 = MIN(c.y + c.h, w->cy + w->ch);
        for (int y = y0; y < y1; y++) gfx_hline(c.x, y, c.w, mix(0x0A1B3D, 0x1E4C8C, (y - w->cy) * 255 / MAX(1, w->ch - 1)));
        const char *n = QDP_NAME;
        int sc = w->cw >= 1024 ? 4 : 3, nw = qdp_big_text_width(sc, n), ny = w->cy + w->ch / 3;
        qdp_draw_big_text(w->cx + (w->cw - nw) / 2 + sc, ny + sc, sc, 0x081228, n);
        qdp_draw_big_text(w->cx + (w->cw - nw) / 2, ny, sc, 0xFFFFFF, n);
        static const char *const lines[] = {
            "Ctrl+Alt+Home      the QEMU manager, and back to the machine",
            "Ctrl+Alt+End       Ctrl+Alt+Del to the machine",
            "Ctrl+Alt               gives the mouse back",
            "Ctrl+Alt+S          the machine's picture: fitted, stretched, pixel for pixel",
        };
        int ly = ny + bmf_height(&font_ui_bold) * sc + 24;
        for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            int lw = gfx_text_width(ui_font, lines[0]);
            gfx_text(ui_font, w->cx + (w->cw - lw) / 2, ly, 0xAFC4E4, lines[i]);
            ly += ui_font->height + 6;
        }
        const char *v = QDP_NAME " " QDP_VERSION " - " QDP_ENGINE " on MS-DOS 7.1";
        gfx_text(&font_small, w->cx + w->cw - gfx_text_width(&font_small, v) - 10, w->cy + w->ch - font_small.height - 8, 0x7F95BC, v);
        return 1;
    }
    if (e->type == WE_DBLCLICK || (e->type == WE_MOUSEDOWN && e->button == MB_LEFT)) { qdpman_show(); return 1; }
    return 0;
}
void desktop_relayout(void)
{
    if (desktop_win) { wm_set_rect(desktop_win, 0, 0, scr_w, scr_h); wm_invalidate(desktop_win); }
}
void desktop_init(void)
{
    desktop_win = wm_create(QDP_NAME, 0, 0, scr_w, scr_h, WF_NOFRAME, back_proc, NULL);
    if (desktop_win) wm_show(desktop_win, 1);
}

/* ---- the command line: QEMUDOS [/M name] [/NOAUTO] [/SETUP] ----------------------------------------------------- */
static void parse_cmdline(void)
{
    char line[130]; strlcpy(line, boot->cmdline, sizeof line);
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if ((p[0] == '/' || p[0] == '-') && !strnicmp(p + 1, "SETUP", 5) && (!p[6] || p[6] == ' ')) { qdp_setup_mode = 1; p += 6; continue; }
        if ((p[0] == '/' || p[0] == '-') && !strnicmp(p + 1, "NOAUTO", 6) && (!p[7] || p[7] == ' ')) { qdp_no_autoboot = 1; p += 7; continue; }
        if ((p[0] == '/' || p[0] == '-') && (p[1] == 'M' || p[1] == 'm') && (!p[2] || p[2] == ' ' || p[2] == ':')) {
            p += 2; if (*p == ':') p++;
            while (*p == ' ') p++;
            int n = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && n < (int)sizeof qdp_start_machine - 1) qdp_start_machine[n++] = *p++; if (*p == '"') p++; }
            else while (*p && *p != ' ' && n < (int)sizeof qdp_start_machine - 1) qdp_start_machine[n++] = *p++;
            qdp_start_machine[n] = 0;
            continue;
        }
        while (*p && *p != ' ') p++;                      /* (anything else: not ours) */
    }
    kprintf("qdp: command line '%s'%s%s%s%s\n", boot->cmdline, qdp_setup_mode ? " - setup" : "", qdp_no_autoboot ? " - no autoboot" : "",
            qdp_start_machine[0] ? " - machine " : "", qdp_start_machine);
}

void qdp_shell_start(void)
{
    parse_cmdline();
    desktop_init();
    if (qdp_setup_mode) { qdp_setup_start(); return; }
    app_launch("qemu");                                    /* the manager (qdpman.c), maximized */
    if (qdp_start_machine[0]) qdpman_start_by_name(qdp_start_machine, 0);
    else if (!qdp_no_autoboot) qdpman_autoboot();
}

/* ---- the hotkeys (kernel/kbd.c) -------------------------------------------------------------------------------- */
void qdp_hotkey(int code)
{
    struct window *act = wm_active(), *vw = vpc_window(), *mw = qdpman_window();
    switch (code) {
    case QDP_HOT_MANAGER:                                   /* the manager in front - or, from it, the machine again */
        if (vw && act == vw) qdpman_show();
        else if (vw) vpc_show();
        else qdpman_show();
        break;
    case QDP_HOT_CAD: vpc_send_cad(); break;
    case QDP_HOT_SCALE: vpc_set_scale_mode(vpc_scale_mode() + 1); break;
    case 2: case 8:                                          /* Alt+Tab: between the manager and the machine */
        if (vw && act != vw) vpc_show(); else qdpman_show();
        break;
    case 4:                                                  /* Ctrl+Alt+Del (no machine has the keyboard) */
        qdp_restart(act && act != desktop_win ? act : mw, 1);
        break;
    case 6: if (act && (act->flags & WF_SYSMENU)) wm_open_sysmenu(act); break;
    case 7:                                                  /* Alt+F4: the window in front closed - the manager asks */
        if (act && act != desktop_win && act != vw) { struct wevent e; memset(&e, 0, sizeof e); e.type = WE_CLOSE; wm_send(act, &e); }
        break;
    }
}

/* ---- leaving: Exit to MS-DOS, Restart ------------------------------------------------------------------------------ */
/* a machine running: asked whether to turn it off first (it is turned off, and waited for, up to 10 s) - 1 go on, 0 not */
static int machine_off_first(struct window *owner, const char *what)
{
    if (vpc_state() == VPC_NONE) return 1;
    char *msg = kaprintf("A machine is running.\n\nTurn it off (as pulling its plug: what it has not saved is lost) and %s?", what);
    int r = msgbox(owner, QDP_NAME, msg ? msg : "A machine is running. Turn it off?", MB_YESNO | MB_ICONWARN);
    kfree(msg);
    if (r != ID_YES) return 0;
    vpc_power_off();
    for (int i = 0; i < 1000 && vpc_state() != VPC_NONE; i++) { wm_pump(); thread_sleep_ms(10); }
    return 1;
}
static void leave(int reboot)
{
    config_save();
    /* (as DOS95's Return to MS-DOS, v42 #480: a PC whose NVMe disk or USB controller the BIOS cannot have back restarts into
     * MS-DOS - DOSMODE.FLG tells QEMUDOS.COM, started again by AUTOEXEC.BAT, to leave DOS at its prompt) */
    int dosmode = 0;
    if (!reboot) {
        extern int ata_bios_lost(void), usb_bios_lost(void);
        if (ata_bios_lost() || usb_bios_lost()) {
            static const char flag[] = "The QEMU DOS port restarted the PC into MS-DOS (Exit to MS-DOS): QEMUDOS.COM deletes this file.\r\n";
            char p[160];
            if (path_join(p, sizeof p, cfg.dos95dir, "DOSMODE.FLG") == 0 && fs_write_file(p, flag, sizeof flag - 1) == (int)sizeof flag - 1) dosmode = 1;
        }
    }
    gfx_reset_clip();
    gfx_fill(0, 0, scr_w, scr_h, C_BLACK);
    gfx_cursor_show(0);
    const char *t = reboot ? "The computer restarts." : dosmode ? "The computer restarts into MS-DOS." : "Back to MS-DOS.";
    gfx_text(ui_font_bold, (scr_w - gfx_text_width(ui_font_bold, t)) / 2, scr_h / 2 - 20, 0xFF8000, t);
    gfx_flush_all();
    thread_sleep_ms(300);
    extern void vesa_text_mode(void);
    vesa_text_mode();
    { extern void dos_handles_fini(void); dos_handles_fini(); }
    kernel_shutdown(reboot);
}
void qdp_exit_to_dos(struct window *owner, int ask)
{
    if (ask && vpc_state() == VPC_NONE && msgbox(owner, QDP_NAME, "Exit to MS-DOS?\n\nType QEMUDOS at the prompt to come back.", MB_YESNO | MB_ICONQUESTION) != ID_YES) return;
    if (!machine_off_first(owner, "exit to MS-DOS")) return;
    leave(0);
}
void qdp_restart(struct window *owner, int ask)
{
    if (ask && vpc_state() == VPC_NONE && msgbox(owner, QDP_NAME, "Restart the computer?", MB_YESNO | MB_ICONQUESTION) != ID_YES) return;
    if (!machine_off_first(owner, "restart the computer")) return;
    leave(1);
}
