/* DOS95 host layer for QEMU: display + input.  The DisplayChangeListener copies
 * changed rectangles of QEMU's surface into the DOS95 window (kernel/vpc.c);
 * dpy_refresh (the 30 ms GUI timer) drains the window's input queue into the
 * PS/2 keyboard/mouse. */
#include "qemu-common.h"
#include "console.h"
#include "sysemu.h"
#include "qemu-timer.h"
#include "qemu-glue.h"
#include "dos95-host.h"

static void (*tick_hook)(void);
void qemu_dos95_set_tick_hook(void (*fn)(void)) { tick_hook = fn; vpc_set_tick_hook(fn); }
int qemu_dos95_alarm_pending(void);

/* main-loop sleep: wake on alarm, input, or timeout; the kernel tick is HZ (5 ms) */
void qemu_dos95_wait(int ms)
{
    if (ms <= 0) { thread_yield(); return; }
    uint32_t end = jiffies + (ms + 4) / 5;
    int t, a, b, c, d;
    while ((int32_t)(jiffies - end) < 0) {
        if (qemu_dos95_alarm_pending()) return;
        if (vpc_stop_requested()) return;
        if (vpc_net_pending()) return;
        thread_sleep_ms(5);
    }
    (void)t; (void)a; (void)b; (void)c; (void)d;
}
void qemu_dos95_exit(int code) { vpc_thread_exit(code); }
void qemu_dos95_request_shutdown(void) { qemu_system_shutdown_request(); }

static void dos95_update(DisplayState *ds, int x, int y, int w, int h)
{
    DisplaySurface *s = ds->surface;
    vpc_display_update(x, y, w, h, s->data, s->linesize, s->pf.bits_per_pixel, s->pf.rmask, s->pf.gmask, s->pf.bmask);
}
static void dos95_resize(DisplayState *ds)
{
    vpc_display_resize(ds_get_width(ds), ds_get_height(ds));
    dos95_update(ds, 0, 0, ds_get_width(ds), ds_get_height(ds));
}
#ifdef QDP
/* the QEMU DOS port's manager: pause, resume, reset, the power button, off, a medium changed (kernel/vpc.c VCMD_*) -
 * done here, on QEMU's own thread, as its monitor's stop, cont, system_reset, system_powerdown, quit, change, eject */
#include "block.h"
#include "block_int.h"
void kfree(void *p);
static void dos95_media(int drive)
{
    static const char *const dev[3] = { "ide1-cd0", "floppy0", "floppy1" };
    char *path = vpc_media_take(drive);
    BlockDriverState *bs = bdrv_find(dev[drive]);
    int ok = 0;
    if (bs) {
        bdrv_close(bs);                             /* (taken out, as eject -f) */
        if (!path) ok = 1;
        else {
            int flags = bdrv_is_read_only(bs) ? 0 : BDRV_O_RDWR;
            ok = bdrv_open(bs, path, flags, NULL) >= 0;
        }
    }
    vpc_media_result(drive, ok);
    kfree(path);                                    /* (kernel/vpc.c's kstrdup) */
}
static void dos95_command(int cmd, int arg)
{
    switch (cmd) {
    case 1: if (vm_running) vm_stop(EXCP_INTERRUPT); break;
    case 2: if (!vm_running) vm_start(); break;
    case 3: qemu_system_reset_request(); break;
    case 4: qemu_system_powerdown_request(); break;
    case 5: qemu_system_shutdown_request(); break;
    case 6: dos95_media(arg); break;
    }
}
#endif
static void dos95_refresh(DisplayState *ds)
{
    int type, a, b, c, d;
    while (vpc_input_pop(&type, &a, &b, &c, &d)) {
        if (type == 0) kbd_put_keycode(a);
#ifdef QDP
        else if (type == 2) dos95_command(a, b);
#endif
        else kbd_mouse_event(a, b, c, d);
    }
    if (vpc_stop_requested()) qemu_system_shutdown_request();
    vga_hw_update();
}
static void dos95_setdata(DisplayState *ds) { dos95_update(ds, 0, 0, ds_get_width(ds), ds_get_height(ds)); }

void dos95_display_init(DisplayState *ds)
{
    DisplayChangeListener *dcl = qemu_mallocz(sizeof *dcl);
    dcl->dpy_update = dos95_update;
    dcl->dpy_resize = dos95_resize;
    dcl->dpy_refresh = dos95_refresh;
    dcl->dpy_setdata = dos95_setdata;
    register_displaychangelistener(ds, dcl);
    dos95_resize(ds);
}
