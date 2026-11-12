/* DOS95 native app: QEMU machine window ("virtual PC").
 *
 * Runs QEMU 0.14.1 (qemu/) on its own kernel thread and shows the guest's screen
 * in a window.  The QEMU side talks to this file only through qemu/dos95/qemu-glue.h.
 *
 *   vpc_launch_cmdline("-m 32 -hda C:\\DISK.IMG -cdrom C:\\DSL.ISO -boot d")
 *
 * Keyboard: raw scancodes go to the guest while the window is active (DOS95
 * hotkeys still work).  Mouse: absolute if the guest has a USB tablet, else the
 * pointer is grabbed on a click and released with Ctrl+Alt.
 */
#include "shell.h"
#include "app.h"
#include "../qemu/dos95/qemu-glue.h"
#ifdef QDP
#include "qdp.h"
#endif

#define QEMU_DIR "C:\\DOS95\\QEMU"

struct vpc {
    struct window *win;
    uint32_t *fb; int fw, fh;             /* guest frame buffer (0xRRGGBB) */
    struct rect dirty; int dirty_posted, resize_posted;
    struct thread *thr;
    int exited, exit_code, stop_req, started;
    int grabbed, absolute, buttons, lastx, lasty;
    int argc; char **argv; char *cmd;     /* v41 (#336): any number of arguments, any length */
    char status[128];
#ifdef QDP
    char vmfile[260];                     /* the manager's .VM file */
    int paused;
    uint32_t status_at;                   /* jiffies the status line was set: it goes after a few seconds */
    volatile int fb_reading;              /* the manager reads fb for its picture (interrupts on): a resize keeps the old */
    uint32_t *fb_old;                     /* ... here, freed when the reading is over */
    struct rect dst;                      /* where the picture is drawn in the client area (fit, stretched, 1:1) */
#endif
};
static struct vpc *cur;                   /* one machine at a time (for now) */
#ifdef QDP
static int scale_mode;                    /* 0 fit (aspect kept, black bars), 1 stretched over the screen, 2 pixel for pixel */
static void vpc_say(struct vpc *v, const char *text);
#endif

/* ---- input queue (IRQ context -> QEMU thread) ---------------------------------------
 * v41 (#336): it grows while the machine is behind (no 512: keys typed then were lost); with
 * interrupts off - kmalloc never waits - and an event lost only when memory has no room */
struct inev { int type, a, b, c, d; };
static struct inev inq0[512], *inq = inq0;
static unsigned inq_cap = 512;                            /* a power of two */
static volatile unsigned inq_head, inq_tail;
static void inq_push(int type, int a, int b, int c, int d)
{
    uint32_t f = save_flags_cli();
    if (inq_tail - inq_head >= inq_cap) {
        struct inev *n = kmalloc(inq_cap * 2 * sizeof *n);
        if (n) {
            unsigned k = 0;
            for (unsigned i = inq_head; i != inq_tail; i++) n[k++] = inq[i % inq_cap];
            if (inq != inq0) kfree(inq);
            inq = n; inq_cap *= 2; inq_head = 0; inq_tail = k;
        }
    }
    if (inq_tail - inq_head < inq_cap) { inq[inq_tail % inq_cap] = (struct inev){ type, a, b, c, d }; inq_tail++; }
    restore_flags(f);
}
int vpc_input_pop(int *type, int *a, int *b, int *c, int *d)
{
    uint32_t f = save_flags_cli();
    if (inq_head == inq_tail) { restore_flags(f); return 0; }
    struct inev e = inq[inq_head % inq_cap]; inq_head++;
    restore_flags(f);
    *type = e.type; *a = e.a; *b = e.b; *c = e.c; *d = e.d;
    return 1;
}

/* ---- hooks called from IRQ context -------------------------------------------------- */
static int vpc_kbd_hook(int code, int release)
{
    struct vpc *v = cur;
    if (!v || !v->win || wm_active() != v->win) return 0;
    if (code & 0x100) inq_push(0, 0xE0, 0, 0, 0);
    inq_push(0, (code & 0x7F) | (release ? 0x80 : 0), 0, 0, 0);
    /* Ctrl+Alt releases the mouse grab (QEMU convention) */
    if (v->grabbed && !release) {
        extern uint16_t kbd_mods(void);
        int m = kbd_mods();
        if ((m & MOD_CTRL) && (m & MOD_ALT)) { struct event e = { EV_VMTITLE, 2, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
    }
    return 1;
}
static void vpc_mouse_hook(int dx, int dy, int buttons, int wheel)
{
    struct vpc *v = cur;
    if (!v) return;
    inq_push(1, dx, dy, wheel ? (wheel > 0 ? 1 : -1) : 0, buttons);   /* (v41: the notch as it came - it was turned round) */
}
static void vpc_grab(struct vpc *v, int on)
{
    if (v->grabbed == on) return;
    v->grabbed = on;
#ifdef QDP
    if (on) { mouse_grab_hook = vpc_mouse_hook; cursor_set(CUR_NONE); vpc_say(v, "The mouse is the machine's - Ctrl+Alt gives it back, Ctrl+Alt+Home: the QEMU manager"); }
    else { mouse_grab_hook = NULL; cursor_set(CUR_ARROW); vpc_say(v, ""); }
#else
    if (on) { mouse_grab_hook = vpc_mouse_hook; cursor_set(CUR_NONE); ksnprintf(v->status, sizeof v->status, "Mouse grabbed - press Ctrl+Alt to release"); }
    else { mouse_grab_hook = NULL; cursor_set(CUR_ARROW); v->status[0] = 0; }
#endif
    wm_invalidate(v->win);
}

/* ---- display (QEMU thread -> GUI) --------------------------------------------------- */
void vpc_display_resize(int w, int h)
{
    struct vpc *v = cur; if (!v) return;
    if (w == v->fw && h == v->fh && v->fb) return;
    uint32_t *nb = kzalloc(w * h * 4);
    if (!nb) {                                     /* v41 (#336): no room - the old screen kept, and said */
        ksnprintf(v->status, sizeof v->status, "Not enough memory to show the machine's %d x %d screen.", w, h);
        v->dirty = (struct rect){ 0, 0, v->fw, v->fh };
        if (!v->dirty_posted) { v->dirty_posted = 1; struct event e = { EV_VMTITLE, 0, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
        return;
    }
    uint32_t f = save_flags_cli();
    uint32_t *old = v->fb; v->fb = nb; v->fw = w; v->fh = h;
    v->dirty = (struct rect){ 0, 0, w, h };
#ifdef QDP
    if (v->fb_reading) { uint32_t *o2 = v->fb_old; v->fb_old = old; old = o2; }   /* (the manager is reading it: freed after) */
#endif
    restore_flags(f);
    if (old) kfree(old);
    if (!v->resize_posted) { v->resize_posted = 1; struct event e = { EV_VMTITLE, 1, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
}
void vpc_display_update(int x, int y, int w, int h, const uint8_t *data, int linesize, int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask)
{
    struct vpc *v = cur; if (!v || !v->fb) return;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > v->fw) w = v->fw - x; if (y + h > v->fh) h = v->fh - y;
    if (w <= 0 || h <= 0) return;
#ifdef QDP
    if (!v->started) { v->started = 1; v->status[0] = 0; struct event e = { EV_VMTITLE, 4, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
#else
    if (!v->started) { v->started = 1; v->status[0] = 0; }
#endif
    for (int j = 0; j < h; j++) {
        uint32_t *dst = v->fb + (y + j) * v->fw + x;
        const uint8_t *src = data + (y + j) * linesize;
        if (bpp == 32) { const uint32_t *s = (const uint32_t*)src + x; for (int i = 0; i < w; i++) dst[i] = s[i] & 0xFFFFFF; }
        else if (bpp == 16) { const uint16_t *s = (const uint16_t*)src + x; for (int i = 0; i < w; i++) { uint16_t p = s[i]; dst[i] = ((p >> 11) << 19) | (((p >> 5) & 63) << 10) | ((p & 31) << 3); } }
        else if (bpp == 15) { const uint16_t *s = (const uint16_t*)src + x; for (int i = 0; i < w; i++) { uint16_t p = s[i]; dst[i] = (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) | ((p & 31) << 3); } }
        else if (bpp == 24) { const uint8_t *s = src + x * 3; for (int i = 0; i < w; i++) dst[i] = s[i * 3] | (s[i * 3 + 1] << 8) | (s[i * 3 + 2] << 16); }
        else if (bpp == 8) { const uint8_t *s = src + x; for (int i = 0; i < w; i++) dst[i] = s[i] * 0x010101; }
    }
    uint32_t f = save_flags_cli();
    struct rect r = { x, y, w, h }; rect_union(&v->dirty, &r, &v->dirty);
    int post = !v->dirty_posted; v->dirty_posted = 1;
    restore_flags(f);
    if (post) { struct event e = { EV_VMTITLE, 0, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
}
void vpc_set_tick_hook(void (*fn)(void)) { sched_tick_hook = fn; }
int  vpc_stop_requested(void) { return cur ? cur->stop_req : 1; }
void vpc_status(const char *text) { if (cur) { strlcpy(cur->status, text, sizeof cur->status); } }

/* ---- network bridge for the guest NIC (see qemu/net/dos95.c) ---- */
#include "net.h"
/* the frames for the guest's card: its receive ring - 256 of them, as an e1000's own ring (a
 * card drops what comes while its ring is full; v41 #336: was 32, each a whole frame's room
 * made at once - now made as each frame comes); any number of cards (was 4) */
/* v41 (#336): the frames waiting for the guest - a ring that grows, as many as a sixteenth of the
 * memory free holds (256 were; past them frames were dropped) */
struct vnframe { int len; uint8_t data[]; };
struct vnet {
    struct vnet *next;
    struct netclient cl;
    int learned;                                   /* guest MAC seen on its first TX */
    struct vnframe **q; unsigned qcap; volatile unsigned qh, qt; uint32_t qbytes;
    uint32_t dropped;
};
static struct vnet *vnets;                         /* (interrupts off) */

static void vnet_rx(struct netclient *c, const uint8_t *frame, int len)
{
    struct vnet *v = c->priv;
    uint32_t fl = save_flags_cli();
    if (v->qbytes + (uint32_t)len > mm_free_pages() * 256u) { v->dropped++; restore_flags(fl); return; }
    if (v->qt - v->qh >= v->qcap) {
        unsigned nc = v->qcap ? v->qcap * 2 : 256;   /* (qh, qt only count up: a look at them unlocked stays true) */
        struct vnframe **t = kmalloc(sizeof *t * nc);
        if (!t) { v->dropped++; restore_flags(fl); return; }
        for (unsigned i = v->qh; i != v->qt; i++) t[i & (nc - 1)] = v->q[i & (v->qcap - 1)];
        if (v->q) kfree(v->q);
        v->q = t; v->qcap = nc;
    }
    struct vnframe *fr = kmalloc(sizeof *fr + len);
    if (!fr) { v->dropped++; restore_flags(fl); return; }
    fr->len = len; memcpy(fr->data, frame, len);
    v->q[v->qt & (v->qcap - 1)] = fr; v->qt++; v->qbytes += (uint32_t)len;
    restore_flags(fl);
}
int vpc_net_available(void) { return net_device() != NULL; }
void *vpc_net_open(const char *name)
{
    if (!net_device()) return NULL;
    struct vnet *v = kmalloc(sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->cl.rx = vnet_rx; v->cl.priv = v; v->cl.promisc = 1;       /* until the guest's MAC is known */
    net_random_mac(v->cl.mac);
    ksnprintf(v->cl.name, sizeof v->cl.name, "qemu %s", name ? name : "");
    if (net_add_client(&v->cl) != 0) { kfree(v); return NULL; }
    uint32_t fl = save_flags_cli();
    v->next = vnets; vnets = v;
    restore_flags(fl);
    return v;
}
void vpc_net_close(void *h)
{
    struct vnet *v = h;
    if (!v) return;
    net_remove_client(&v->cl);
    uint32_t fl = save_flags_cli();
    for (struct vnet **pp = &vnets; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
    while (v->qh != v->qt) { kfree(v->q[v->qh & (v->qcap - 1)]); v->qh++; }
    restore_flags(fl);
    if (v->q) kfree(v->q);
    kfree(v);
}
int vpc_net_send(void *h, const uint8_t *frame, int len)
{
    struct vnet *v = h;
    if (!v || len < 14) return -1;
    if (!v->learned && !(frame[6] & 1)) {                         /* learn the guest's MAC, drop promiscuity */
        memcpy(v->cl.mac, frame + 6, 6); v->learned = 1; v->cl.promisc = 0;
    }
    return net_client_send(&v->cl, frame, len);
}
int vpc_net_recv(void *h, uint8_t *buf, int max)
{
    struct vnet *v = h;
    if (!v || v->qh == v->qt) return 0;
    uint32_t fl = save_flags_cli();
    if (v->qh == v->qt) { restore_flags(fl); return 0; }
    struct vnframe *fr = v->q[v->qh & (v->qcap - 1)];
    v->qh++; v->qbytes -= (uint32_t)fr->len;
    restore_flags(fl);
    int len = fr->len;
    if (len > max) len = max;
    memcpy(buf, fr->data, len);
    kfree(fr);
    return len;
}
extern int snd_pcm_open(int rate); extern void snd_pcm_close(void); extern int snd_pcm_free(void); extern int snd_pcm_write(const void *d, int n);
int  vpc_snd_available(void) { return 1; }
int  vpc_snd_open(int rate) { return snd_pcm_open(rate); }
void vpc_snd_close(void) { snd_pcm_close(); }
int  vpc_snd_free(void) { return snd_pcm_free(); }
int  vpc_snd_write(const void *pcm, int bytes) { return snd_pcm_write(pcm, bytes); }

int vpc_net_pending(void)
{
    uint32_t fl = save_flags_cli();
    int any = 0;
    for (struct vnet *v = vnets; v && !any; v = v->next) if (v->qh != v->qt) any = 1;
    restore_flags(fl);
    return any;
}
#ifdef QDP
static void qdp_run_over(void);
#endif
void vpc_thread_exit(int code)
{
    struct vpc *v = cur;
    sched_tick_hook = NULL;
#ifdef QDP
    qdp_run_over();                                     /* (before "exited": the next run waits for it) */
#endif
    if (v) { v->exited = 1; v->exit_code = code; struct event e = { EV_VMTITLE, 3, 0, 0, 0, 0xFFFF, 0 }; ev_post(&e); }
    thread_exit();
    for (;;) ;
}

/* GUI-thread side of the events posted above (routed here from vmwin_event) */
void vpc_event(struct event *ev)
{
    struct vpc *v = cur; if (!v || !v->win) return;
    switch (ev->code) {
    case 0: {   /* dirty rectangle */
        uint32_t f = save_flags_cli(); struct rect r = v->dirty; v->dirty = (struct rect){ 0, 0, 0, 0 }; v->dirty_posted = 0; restore_flags(f);
#ifdef QDP
        /* (the picture is drawn in v->dst, not over the whole client area: the rectangle mapped there - before the first
         * paint, when dst is not known yet, the whole window) */
        if (r.w > 0 && v->dst.w <= 0) wm_invalidate(v->win);
        else if (r.w > 0) {
            int dw = v->dst.w, dh = v->dst.h;
            if (dw == v->fw && dh == v->fh) wm_invalidate_rect(v->win, v->dst.x + r.x, v->dst.y + r.y, r.w, r.h);
            else { int x0 = r.x * dw / v->fw, y0 = r.y * dh / v->fh, x1 = (r.x + r.w) * dw / v->fw + 1, y1 = (r.y + r.h) * dh / v->fh + 1; wm_invalidate_rect(v->win, v->dst.x + x0, v->dst.y + y0, x1 - x0, y1 - y0); }
        }
#else
        if (r.w > 0) {
            int cw = v->win->cw, ch = v->win->ch;
            if (cw == v->fw && ch == v->fh) wm_invalidate_rect(v->win, r.x, r.y, r.w, r.h);
            else { int x0 = r.x * cw / v->fw, y0 = r.y * ch / v->fh, x1 = (r.x + r.w) * cw / v->fw + 1, y1 = (r.y + r.h) * ch / v->fh + 1; wm_invalidate_rect(v->win, x0, y0, x1 - x0, y1 - y0); }
        }
#endif
        break; }
    case 1: {   /* guest resolution changed: fit the window */
        v->resize_posted = 0;
#ifdef QDP
        wm_invalidate(v->win);                      /* (the window is the screen: the picture fitted to it as it paints) */
        break;
#endif
        int tw = v->fw, th = v->fh;
        int maxw = scr_w - 16, maxh = desktop_h - 40;
        if (tw > maxw) { th = th * maxw / tw; tw = maxw; }
        if (th > maxh) { tw = tw * maxh / th; th = maxh; }
        int dw = tw - v->win->cw, dh = th - v->win->ch;
        if (dw || dh) { wm_resize(v->win, v->win->r.w + dw, v->win->r.h + dh); if (v->win->r.x + v->win->r.w > scr_w || v->win->r.y + v->win->r.h > desktop_h) wm_center(v->win, NULL); }
        wm_invalidate(v->win);
        break; }
    case 2: vpc_grab(v, 0); break;
#ifdef QDP
    case 4: qdpman_vm_changed(); break;            /* (its first picture: "Running") */
#endif
    case 3: {   /* QEMU exited */
        vpc_grab(v, 0);
        kbd_raw_hook = NULL;
#ifdef QDP
        {   /* the machine's window goes, the manager comes back and is told (its autoboot's "when it shuts down") */
            char f[260]; strlcpy(f, v->vmfile, sizeof f); int code = v->exit_code;
            wm_destroy(v->win);
            qdpman_vm_stopped(f, code);
            break;
        }
#endif
        ksnprintf(v->status, sizeof v->status, "The virtual machine has stopped (exit code %d).", v->exit_code);
        wm_invalidate(v->win);
        break; }
    }
}

/* ---- window ------------------------------------------------------------------------- */
static int vpc_proc(struct window *w, struct wevent *e)
{
    struct vpc *v = w->user;
    switch (e->type) {
    case WE_PAINT: {
        int cw = w->cw, ch = w->ch;
#ifdef QDP
        {   /* the picture's place: fit (aspect kept), stretched, or pixel for pixel - centred, black round it */
            int dw = cw, dh = ch;
            if (v->fb && v->fw > 0 && v->fh > 0) {
                if (scale_mode == 2 && v->fw <= cw && v->fh <= ch) { dw = v->fw; dh = v->fh; }
                else if (scale_mode != 1) {
                    if ((uint64_t)cw * v->fh <= (uint64_t)ch * v->fw) { dw = cw; dh = (int)((uint64_t)cw * v->fh / v->fw); }
                    else { dh = ch; dw = (int)((uint64_t)ch * v->fw / v->fh); }
                    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
                }
            }
            v->dst = (struct rect){ (cw - dw) / 2, (ch - dh) / 2, dw, dh };
            if (v->dst.y > 0) { gfx_fill(w->cx, w->cy, cw, v->dst.y, C_BLACK); gfx_fill(w->cx, w->cy + v->dst.y + dh, cw, ch - v->dst.y - dh, C_BLACK); }
            if (v->dst.x > 0) { gfx_fill(w->cx, w->cy + v->dst.y, v->dst.x, dh, C_BLACK); gfx_fill(w->cx + v->dst.x + dw, w->cy + v->dst.y, cw - v->dst.x - dw, dh, C_BLACK); }
        }
        struct rect clip0; gfx_get_clip(&clip0);
        gfx_clip_rect(&(struct rect){ w->cx + v->dst.x, w->cy + v->dst.y, v->dst.w, v->dst.h });
        int wcx = w->cx + v->dst.x, wcy = w->cy + v->dst.y;
        cw = v->dst.w; ch = v->dst.h;
#else
#define wcx (w->cx)                                 /* (DOS95's window: the client area itself, read as before) */
#define wcy (w->cy)
#endif
        if (v->fb && cw == v->fw && ch == v->fh) gfx_blit(wcx, wcy, cw, ch, v->fb, v->fw);
        else if (v->fb) {
            /* scale the guest screen to the client area: box filter when shrinking (every
             * source pixel contributes, so text stays readable), nearest when enlarging */
            struct rect clipr; gfx_get_clip(&clipr);
            int y0 = MAX(clipr.y, wcy), y1 = MIN(clipr.y + clipr.h, wcy + ch);
            int x0 = MAX(clipr.x, wcx), x1 = MIN(clipr.x + clipr.w, wcx + cw);
            if (y1 > y0 && x1 > x0) {
                uint32_t line[256];                /* v41 (#336): a row in pieces of 256 - nothing to allocate (was unchecked) */
                int shrink = v->fw > cw || v->fh > ch;
                for (int y = y0; y < y1; y++) {
                    int sy0 = (y - wcy) * v->fh / ch, sy1 = (y + 1 - wcy) * v->fh / ch;
                    if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 > v->fh) sy1 = v->fh; if (sy0 >= v->fh) sy0 = v->fh - 1;
                    for (int xs = x0; xs < x1; xs += 256) {
                        int xe = MIN(x1, xs + 256);
                        for (int x = xs; x < xe; x++) {
                            int sx0 = (x - wcx) * v->fw / cw, sx1 = (x + 1 - wcx) * v->fw / cw;
                            if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 > v->fw) sx1 = v->fw; if (sx0 >= v->fw) sx0 = v->fw - 1;
                            if (!shrink || (sx1 - sx0 == 1 && sy1 - sy0 == 1)) { line[x - xs] = v->fb[sy0 * v->fw + sx0]; continue; }
                            uint32_t r = 0, g = 0, b = 0, n = 0;
                            for (int sy = sy0; sy < sy1; sy++) { const uint32_t *src = v->fb + sy * v->fw; for (int sx = sx0; sx < sx1; sx++) { uint32_t p = src[sx]; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++; } }
                            line[x - xs] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
                        }
                        gfx_blit(xs, y, xe - xs, 1, line, xe - xs);
                    }
                }
            }
        } else gfx_fill(wcx, wcy, cw, ch, C_BLACK);
#ifndef QDP
#undef wcx
#undef wcy
#endif
#ifdef QDP
        gfx_set_clip(clip0.x, clip0.y, clip0.w, clip0.h);
        if (v->status[0]) {   /* the status line: at the top, in the middle, on a dark band */
            int tw = gfx_text_width(ui_font, v->status) + 24, th = ui_font->height + 10;
            int bx = w->cx + (w->cw - tw) / 2, by = w->cy + 8;
            gfx_blend_fill(bx, by, tw, th, 0x101828, 200);
            gfx_rect(bx, by, tw, th, 0x5A7AB0);
            gfx_text(ui_font, bx + 12, by + 5, 0xFFFFFF, v->status);
        }
        return 1; }
#else
        if (v->status[0]) {
            int tw = gfx_text_width(ui_font, v->status) + 12;
            gfx_fill(w->cx + 4, w->cy + 4, tw, ui_font->height + 6, C_GRAY);
            gfx_rect(w->cx + 4, w->cy + 4, tw, ui_font->height + 6, C_BLACK);
            gfx_text(ui_font, w->cx + 10, w->cy + 7, C_BLACK, v->status);
        }
        return 1; }
#endif
    case WE_ACTIVATE: if (!v->exited) kbd_raw_hook = vpc_kbd_hook; return 1;
    case WE_DEACTIVATE:
        kbd_raw_hook = NULL; vpc_grab(v, 0);
#ifdef QDP
        /* (the keys held as it lost the keyboard - Ctrl and Alt of Ctrl+Alt+Home - let go in the machine: they stayed
         * down there until pressed again) */
        if (!v->exited) { static const int up[] = { 0x9D, 0xB8, 0xAA, 0xB6, 0xE0, 0x9D, 0xE0, 0xB8 }; for (unsigned i = 0; i < sizeof up / sizeof up[0]; i++) inq_push(0, up[i], 0, 0, 0); }
#endif
        return 1;
#ifdef QDP
#define AX(e) ((MIN(MAX((e)->x - v->dst.x, 0), v->dst.w - 1)) * 32767 / MAX(1, v->dst.w - 1))
#define AY(e) ((MIN(MAX((e)->y - v->dst.y, 0), v->dst.h - 1)) * 32767 / MAX(1, v->dst.h - 1))
#else
#define AX(e) ((e)->x * 32767 / MAX(1, w->cw))
#define AY(e) ((e)->y * 32767 / MAX(1, w->ch))
#endif
    case WE_MOUSEDOWN:
        if (v->exited) return 1;
        if (v->grabbed) return 1;
        if (v->absolute) { v->buttons |= e->button; inq_push(1, AX(e), AY(e), 0, v->buttons); return 1; }
        vpc_grab(v, 1);
        return 1;
    case WE_MOUSEUP:
        if (v->absolute) { v->buttons &= ~e->button; inq_push(1, AX(e), AY(e), 0, v->buttons); }
        return 1;
    case WE_MOUSEMOVE:
        if (v->absolute && !v->exited) inq_push(1, AX(e), AY(e), 0, v->buttons);
        return 1;
    case WE_WHEEL:
        /* (v41: the machine's mouse gets the notch as it came - +1 towards the user, QEMU's own dz; it was turned round) */
        if (v->absolute) inq_push(1, AX(e), AY(e), e->id > 0 ? 1 : -1, v->buttons);
        return 1;
    case WE_KEYDOWN: case WE_KEYUP: case WE_CHAR: return 1;   /* raw hook already fed the guest */
    case WE_SIZE: wm_invalidate(w); return 1;
#ifdef QDP
    case WE_TIMER:
        if (v->status[0] && !v->grabbed && v->started && jiffies - v->status_at > 5000 / TICK_MS) { v->status[0] = 0; wm_invalidate(w); }
        return 1;
    case WE_DISPLAYCHANGE:
        wm_set_rect(w, 0, 0, scr_w, scr_h); wm_invalidate(w); return 1;
#endif
    case WE_CLOSE:
        if (v->exited) { wm_destroy(w); return 1; }
        if (!v->stop_req) {
            if (msgbox(w, "QEMU", "Power off the virtual machine?\n\nUnsaved guest data will be lost.", MB_YESNO | MB_ICONQUESTION) != ID_YES) return 1;
            v->stop_req = 1; strlcpy(v->status, "Shutting down...", sizeof v->status); wm_invalidate(w);
        }
        return 1;
    case WE_DESTROY:
        kbd_raw_hook = NULL; mouse_grab_hook = NULL; sched_tick_hook = NULL;
        if (cur == v) cur = NULL;
        if (v->fb) kfree(v->fb);
#ifdef QDP
        kfree(v->fb_old);
#endif
        for (int i = 0; i < v->argc; i++) kfree(v->argv[i]);
        if (v->argv) kfree(v->argv);
        kfree(v);
        return 1;
    }
    return 0;
}

/* ---- the QEMU thread ---------------------------------------------------------------- */
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
static int ctors_done;
#ifdef QDP
/* QEMU 0.14 was made to run once, in a process of its own; the QEMU DOS port runs machine after machine in this kernel.
 * So before each run QEMU's globals (together: qdp/kernel/linker-qdp.ld) are put back as they were before the first -
 * .data from a copy made then, .bss zeroed - and its constructors run again; and at a run's end (qdp_run_over) what it
 * had is given back: its atexit functions, disks, files and memory (qemu/dos95/libc.c qemu_dos95_run_over), and on this
 * side its network clients and its sound stream if it left them open.  (Until 2026-11-10 a second start failed - "drive
 * ... exists", the first run's drives still listed - and a third crashed the kernel; every run kept its memory.) */
extern char __qemu_data_start[], __qemu_data_end[], __qemu_bss_start[], __qemu_bss_end[];
static char *qemu_data0;
static int qemu_snapshot(void)
{
    if (qemu_data0) return 0;
    uint32_t n = (uint32_t)(__qemu_data_end - __qemu_data_start);
    if (!(qemu_data0 = kmalloc(n ? n : 1))) return -1;
    memcpy(qemu_data0, __qemu_data_start, n);
    return 0;
}
static char qemu_said[200];                             /* the last line QEMU wrote to stderr this run */
void vpc_qemu_said(const char *line) { strlcpy(qemu_said, line, sizeof qemu_said); }
const char *vpc_qemu_last_said(void) { return qemu_said; }
static void qdp_run_over(void)
{
    extern void qemu_dos95_run_over(void);
    qemu_dos95_run_over();
    for (;;) {
        uint32_t fl = save_flags_cli(); struct vnet *n = vnets; restore_flags(fl);
        if (!n) break;
        vpc_net_close(n);
    }
    snd_pcm_close();
}
#endif
static void vpc_thread(void *arg)
{
    struct vpc *v = arg;
#ifdef QDP
    memcpy(__qemu_data_start, qemu_data0, (uint32_t)(__qemu_data_end - __qemu_data_start));
    memset(__qemu_bss_start, 0, (uint32_t)(__qemu_bss_end - __qemu_bss_start));
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)();
#else
    if (!ctors_done) { ctors_done = 1; for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)(); }
#endif
    kprintf("vpc: starting qemu, %d args\n", v->argc);
    int r = qemu_main(v->argc, v->argv, NULL);
    vpc_thread_exit(r);
}

/* split a command line into argv (quotes allowed) - 0, or -1: no memory */
static int split_args(struct vpc *v, const char *cmd)
{
    int max = 2;
    for (const char *q = cmd; *q; q++) if (*q == ' ') max++;
    v->argv = kzalloc((max + 1) * sizeof *v->argv);
    if (!v->argv || !(v->argv[v->argc++] = kstrdup("qemu"))) return -1;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *a = kmalloc(strlen(p) + 1);
        if (!a) return -1;
        int n = 0, q = 0;
        while (*p && (q || *p != ' ')) { if (*p == '"') { q = !q; p++; continue; } a[n++] = *p++; }
        a[n] = 0;
        v->argv[v->argc++] = a;
    }
    v->argv[v->argc] = NULL;
    return 0;
}

#ifdef QDP
int vpc_launch_cmdline(const char *cmdline, const char *title) { return vpc_launch_machine(cmdline, title, ""); }
int vpc_launch_machine(const char *cmdline, const char *title, const char *vmfile)
#else
int vpc_launch_cmdline(const char *cmdline, const char *title)
#endif
{
    if (cur) { msgbox(NULL, "QEMU", "A virtual machine is already running.\nOnly one machine can run at a time.", MB_OK | MB_ICONWARN); return -1; }
    if (mm_free_pages() < 12 * 256) { msgbox(NULL, "QEMU", "Not enough free memory to start a virtual machine.\nClose some DOS windows and try again.", MB_OK | MB_ICONERROR); return -1; }
#ifdef QDP
    if (qemu_snapshot() < 0) { msgbox(NULL, "QEMU", "Not enough memory to start a virtual machine.", MB_OK | MB_ICONERROR); return -1; }
    { uint32_t f = save_flags_cli(); inq_head = inq_tail; restore_flags(f); }     /* (nothing the last run left unread) */
    qemu_said[0] = 0;
#endif
    struct vpc *v = kzalloc(sizeof *v);
#ifdef QDP
    /* the QEMU DOS port: QEMU's BIOS files are in its own folder's QEMU (C:\QEMUDOS\QEMU, or the folder an install
     * beside MS-DOS chose) - not DOS95's C:\DOS95\QEMU, which a disk the setup made has not */
    char qdir[200]; path_join(qdir, sizeof qdir, qdp_dir(), "QEMU");
    int fn = strlen(cmdline) + 96 + strlen(qdir);
    char *full = v ? kmalloc(fn) : NULL;
    if (full) ksnprintf(full, fn, "-L \"%s\" -monitor null -serial none -parallel none %s", qdir, cmdline);
#else
    int fn = strlen(cmdline) + 96;
    char *full = v ? kmalloc(fn) : NULL;                  /* v41 (#336): the whole command line (no 511 bytes) */
    if (full) ksnprintf(full, fn, "-L %s -monitor null -serial none -parallel none %s", QEMU_DIR, cmdline);
#endif
    if (!full || split_args(v, full) < 0) {
        if (full) kfree(full);
        if (v) { for (int i = 0; i < v->argc; i++) kfree(v->argv[i]); if (v->argv) kfree(v->argv); kfree(v); }
        msgbox(NULL, "QEMU", "Not enough memory to start a virtual machine.", MB_OK | MB_ICONERROR);
        return -1;
    }
    kfree(full);
    cur = v;
    for (int i = 1; i < v->argc; i++) if (!strcmp(v->argv[i], "-usbdevice") && i + 1 < v->argc && !strcmp(v->argv[i + 1], "tablet")) v->absolute = 1;
    v->fw = 640; v->fh = 400; v->fb = kzalloc(v->fw * v->fh * 4);
#ifdef QDP
    /* the QEMU DOS port: the machine is the whole screen - no frame, nothing round it (Ctrl+Alt+Home: the manager) */
    strlcpy(v->vmfile, vmfile ? vmfile : "", sizeof v->vmfile);
    struct window *w = v->fb ? wm_create(title ? title : "QEMU", 0, 0, scr_w, scr_h, WF_NOFRAME, vpc_proc, v) : NULL;
#else
    struct window *w = v->fb ? wm_create(title ? title : "QEMU", 0, 0, v->fw + 8, v->fh + 8 + 19, WF_TITLE | WF_MINBOX | WF_MAXBOX | WF_CLOSEBOX | WF_SYSMENU | WF_TASKBAR | WF_RESIZABLE, vpc_proc, v) : NULL;
#endif
    if (!w) {
        cur = NULL;
        if (v->fb) kfree(v->fb);
        for (int i = 0; i < v->argc; i++) kfree(v->argv[i]);
        kfree(v->argv); kfree(v);
        msgbox(NULL, "QEMU", "Not enough memory to start a virtual machine.", MB_OK | MB_ICONERROR);
        return -1;
    }
#ifdef QDP
    v->win = w; w->icon = ICO_COMPUTER;
    vpc_say(v, "Starting...");
    wm_set_timer(w, 500);
    wm_show(w, 1);
    wm_activate(w);
#else
    w->minw = 160; w->minh = 120;
    v->win = w; w->icon = ICO_COMPUTER;
    int dw = v->fw - w->cw, dh = v->fh - w->ch; if (dw || dh) wm_resize(w, w->r.w + dw, w->r.h + dh);
    wm_center(w, NULL);
    strlcpy(v->status, "Starting...", sizeof v->status);
    wm_show(w, 1);
#endif
    { uint32_t tf = save_flags_cli();                  /* (v41, #403 b.: its FPU state before it can run - kmod.c kmod_start) */
      v->thr = thread_create("qemu", vpc_thread, v, 3, 256 * 1024);
      if (v->thr) thread_use_fpu(v->thr);
      restore_flags(tf); }
    if (!v->thr) { wm_destroy(w); msgbox(NULL, "QEMU", "Not enough memory to start a virtual machine.", MB_OK | MB_ICONERROR); return -1; }
    return 0;
}

/* ---- app registration: Run "vpc <qemu args>" ---------------------------------------- */
static struct app vpc_app;
static void vpc_app_launch(void) { vpc_launch_cmdline("-m 32 -boot c", "QEMU"); }
static void vpc_app_launch_file(const char *args) { vpc_launch_cmdline(args, "QEMU"); }
void vpc_register(void)
{
    vpc_app.name = "vpc"; vpc_app.title = "QEMU"; vpc_app.icon = ICO_COMPUTER; vpc_app.single = 1; vpc_app.hidden = 1;
    vpc_app.launch = vpc_app_launch; vpc_app.launch_file = vpc_app_launch_file;
    app_register(&vpc_app);
}

#ifdef QDP
/* ---- the QEMU DOS port's manager drives the machine (qdp.h) ------------------------------------------------------
 * A command goes to the QEMU thread through the input queue (type 2: a, b) and is done there, between two of its
 * main loop's turns (qemu/dos95/display-dos95.c dos95_refresh): QEMU 0.14's state is its thread's alone */
enum { VCMD_PAUSE = 1, VCMD_RESUME, VCMD_RESET, VCMD_POWERBTN, VCMD_QUIT, VCMD_MEDIA };
static void vpc_say(struct vpc *v, const char *text)
{
    strlcpy(v->status, text, sizeof v->status);
    v->status_at = jiffies;
    if (v->win) wm_invalidate(v->win);
}
int vpc_state(void)
{
    struct vpc *v = cur;
    if (!v) return VPC_NONE;
    if (v->exited || v->stop_req) return VPC_STOPPING;
    if (!v->started) return VPC_STARTING;
    return v->paused ? VPC_PAUSED : VPC_RUNNING;
}
const char *vpc_machine_file(void) { return cur ? cur->vmfile : ""; }
struct window *vpc_window(void) { return cur ? cur->win : NULL; }
void vpc_show(void)
{
    struct vpc *v = cur;
    if (!v || !v->win) return;
    wm_show(v->win, 1);
    wm_activate(v->win);
    if (v->paused) vpc_say(v, "Paused - the QEMU manager (Ctrl+Alt+Home) resumes it");
}
void vpc_pause(int on)
{
    struct vpc *v = cur;
    if (!v || v->exited || v->paused == !!on) return;
    v->paused = !!on;
    inq_push(2, on ? VCMD_PAUSE : VCMD_RESUME, 0, 0, 0);
    vpc_say(v, on ? "Paused" : "");
}
void vpc_reset(void) { struct vpc *v = cur; if (v && !v->exited) { if (v->paused) vpc_pause(0); inq_push(2, VCMD_RESET, 0, 0, 0); } }
void vpc_power_button(void) { struct vpc *v = cur; if (v && !v->exited) { if (v->paused) vpc_pause(0); inq_push(2, VCMD_POWERBTN, 0, 0, 0); vpc_say(v, "The power button was pressed - the machine shuts itself down"); } }
void vpc_power_off(void) { struct vpc *v = cur; if (v && !v->exited) { v->stop_req = 1; inq_push(2, VCMD_QUIT, 0, 0, 0); } }
void vpc_send_cad(void)
{
    struct vpc *v = cur;
    if (!v || v->exited) return;
    static const int seq[] = { 0x1D, 0x38, 0xE0, 0x53, 0xE0, 0xD3, 0xB8, 0x9D };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) inq_push(0, seq[i], 0, 0, 0);
}
/* a CD or floppy image put in (path) or taken out (NULL) - the path kept until the QEMU thread has opened it */
static char *media_path[3];
int vpc_change_media(int drive, const char *path)
{
    struct vpc *v = cur;
    if (!v || v->exited || drive < 0 || drive > 2) return -1;
    char *p = NULL;
    if (path && path[0] && !(p = kstrdup(path))) return -1;
    uint32_t f = save_flags_cli();
    char *old = media_path[drive]; media_path[drive] = p;
    restore_flags(f);
    kfree(old);
    inq_push(2, VCMD_MEDIA, drive, 0, 0);
    return 0;
}
/* (the QEMU thread) the path of a drive's new medium, NULL taken out - the caller frees it */
char *vpc_media_take(int drive)
{
    if (drive < 0 || drive > 2) return NULL;
    uint32_t f = save_flags_cli();
    char *p = media_path[drive]; media_path[drive] = NULL;
    restore_flags(f);
    return p;
}
void vpc_media_result(int drive, int ok)
{
    struct vpc *v = cur;
    if (!v) return;
    static const char *const names[3] = { "CD-ROM", "floppy A:", "floppy B:" };
    char t[96]; ksnprintf(t, sizeof t, ok ? "The %s's medium was changed" : "The %s's medium could not be changed", names[drive]);
    vpc_say(v, t);
}
void vpc_screen_size(int *w, int *h) { struct vpc *v = cur; *w = v ? v->fw : 0; *h = v ? v->fh : 0; }
int vpc_thumbnail(uint32_t *dst, int w, int h)
{
    struct vpc *v = cur;
    if (!v || !v->fb || !v->started || w <= 0 || h <= 0) return 0;
    /* (a box filter: every pixel of the machine's screen counts - its text stays readable small; at most 4 x 4 of them a
     * pixel, and the screen read with interrupts on: a torn picture of a frame being drawn is redrawn the next second) */
    uint32_t f = save_flags_cli();
    const uint32_t *fb = v->fb; int fw = v->fw, fh = v->fh;
    v->fb_reading = 1;
    restore_flags(f);
    for (int y = 0; y < h; y++) {
        int sy0 = y * fh / h, sy1 = (y + 1) * fh / h; if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 - sy0 > 4) sy1 = sy0 + 4;
        for (int x = 0; x < w; x++) {
            int sx0 = x * fw / w, sx1 = (x + 1) * fw / w; if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 - sx0 > 4) sx1 = sx0 + 4;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < fh; sy++) for (int sx = sx0; sx < sx1 && sx < fw; sx++) { uint32_t p = fb[sy * fw + sx]; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++; }
            dst[y * w + x] = n ? ((r / n) << 16) | ((g / n) << 8) | (b / n) : 0;
        }
    }
    f = save_flags_cli();
    v->fb_reading = 0;
    uint32_t *old = v->fb_old; v->fb_old = NULL;
    restore_flags(f);
    kfree(old);
    return 1;
}
int vpc_scale_mode(void) { return scale_mode; }
void vpc_set_scale_mode(int m)
{
    static const char *const names[3] = { "The picture fitted to the screen (its shape kept)", "The picture stretched over the screen", "The picture pixel for pixel" };
    scale_mode = (m % 3 + 3) % 3;
    if (cur && cur->win) { vpc_say(cur, names[scale_mode]); wm_invalidate(cur->win); }
}
#endif
