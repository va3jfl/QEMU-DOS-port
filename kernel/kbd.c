/* DOS95 kernel - PS/2 keyboard driver, GUI event queue */
#include "kernel.h"

struct kbdstate kbd_state;
static int e0;

int gui_in_dos_call;
static const char sc_normal[128];
/* ---- event queue ---------------------------------------------------------- */
/* v41 (#336): the queue grows while the GUI thread is behind - no 256: the 257th event was
 * lost (a key, a console's "program gone", a DOS box's screen).  It doubles, with interrupts
 * off (an interrupt handler posts too: kmalloc never waits); an event is lost only when there
 * is no memory for a bigger queue.  Mouse moves are still folded into the last one. */
static struct event evq0[256], *evq = evq0;
static uint32_t evq_cap = 256;                            /* a power of two */
static volatile uint32_t ev_head, ev_tail;
struct thread *gui_thread;

void ev_post(struct event *e)
{
    uint32_t f = save_flags_cli();
    /* coalesce mouse moves */
    if (e->type == EV_MOUSEMOVE && ev_tail != ev_head) {
        struct event *last = &evq[(ev_tail - 1) % evq_cap];
        if (last->type == EV_MOUSEMOVE) { last->x = e->x; last->y = e->y; last->mods = e->mods; restore_flags(f); goto wake; }
    }
    if (ev_tail - ev_head >= evq_cap) {
        struct event *n = kmalloc(evq_cap * 2 * sizeof *n);
        if (n) {
            uint32_t k = 0;
            for (uint32_t i = ev_head; i != ev_tail; i++) n[k++] = evq[i % evq_cap];
            if (evq != evq0) kfree(evq);
            evq = n; evq_cap *= 2; ev_head = 0; ev_tail = k;
        }
    }
    if (ev_tail - ev_head < evq_cap) {
        evq[ev_tail % evq_cap] = *e;
        ev_tail++;
    }
    restore_flags(f);
wake:
    if (gui_thread && gui_thread->state == T_BLOCKED && !gui_in_dos_call) thread_wake(gui_thread);
}

int ev_peek(void) { return ev_head != ev_tail; }

char kbd_scan_to_char(int sc)
{
    sc &= 0x7F;
    return sc_normal[sc];
}

int ev_get(struct event *e)
{
    uint32_t f = save_flags_cli();
    if (ev_head == ev_tail) { restore_flags(f); return 0; }
    *e = evq[ev_head % evq_cap];
    ev_head++;
    restore_flags(f);
    return 1;
}

void ev_wait(void)
{
    uint32_t f = save_flags_cli();
    if (ev_head == ev_tail) { cur_thread->wake_at = 0; thread_block(); }
    restore_flags(f);
}

/* ---- scancode translation (US layout) --------------------------------------- */
static const char sc_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0, 0,0,0,0,0,0,0,0,0,0, 0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.', 0,0,0, 0,0
};
static const char sc_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0, 0,0,0,0,0,0,0,0,0,0, 0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.', 0,0,0, 0,0
};

int (*kbd_raw_hook)(int code, int release);
uint16_t kbd_mods(void)
{
    return (kbd_state.shift ? MOD_SHIFT : 0) | (kbd_state.ctrl ? MOD_CTRL : 0) | (kbd_state.alt ? MOD_ALT : 0) | (kbd_state.num ? MOD_NUM : 0);
}

extern struct menu *menu_current;
/* v42 (#471): a PC with no keyboard controller (no PS/2 ports - port 64h reads FFh): its keyboard is a USB one
 * (usbhid.c), and nothing is asked of a controller that is not there */
int kbc_present = 1;
uint8_t kbd_leds_now(void) { return (uint8_t)((kbd_state.scroll ? 1 : 0) | (kbd_state.num ? 2 : 0) | (kbd_state.caps ? 4 : 0)); }
void kbd_scancode(uint8_t sc);
static void kbd_irq(struct frame *f)
{
    (void)f;
    uint8_t sc = inb(0x60);
    pic_eoi(1);
    kbd_scancode(sc);
}
/* a scan code of set 1, as the keyboard controller hands it over - a PS/2 keyboard's (kbd_irq) or a USB
 * keyboard's (usbhid.c); interrupts off */
void kbd_scancode(uint8_t sc)
{
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc == 0xE1) { return; }
    int ext = e0; e0 = 0;
    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    /* modifier tracking */
    switch (code) {
    case 0x2A: case 0x36: kbd_state.shift = !release; break;
    case 0x1D: kbd_state.ctrl = !release; break;
    case 0x38: kbd_state.alt = !release; break;
    case 0x3A: if (!release) kbd_state.caps ^= 1; break;
    case 0x45: if (!release && !ext) kbd_state.num ^= 1; break;
    case 0x46: if (!release) kbd_state.scroll ^= 1; break;
    }
    if (!release && (code == 0x3A || (code == 0x45 && !ext) || code == 0x46)) { extern void usb_leds(uint8_t); usb_leds(kbd_leds_now()); }   /* (v42: a USB keyboard's lights) */

    /* GUI hotkeys (never delivered to VMs) */
    int hot = 0;
#ifdef QDP
    /* the QEMU DOS port: a machine that has the keyboard gets every key - Ctrl+Alt+Del, Alt+Tab, Alt+F4, Ctrl+Esc
     * are its own (a Windows guest needs them) - but Ctrl+Alt+Home (the QEMU manager), Ctrl+Alt+End (Ctrl+Alt+Del to
     * the machine, as a remote desktop's) and Ctrl+Alt+S (the picture fitted, stretched, pixel for pixel); without one,
     * Ctrl+Alt+Home takes the manager back to the machine */
    if (!release && kbd_state.ctrl && kbd_state.alt) {
        if (code == 0x47 && (ext || !kbd_state.num)) hot = 20;          /* Home (the grey key, or the keypad's 7) */
        else if (code == 0x4F && (ext || !kbd_state.num)) hot = 21;     /* End */
        else if (code == 0x1F && kbd_raw_hook) hot = 22;                /* S */
    }
    if (!hot && kbd_raw_hook) goto raw;
#endif
    if (!release) {
        if (kbd_state.ctrl && code == 0x01) hot = 1;                 /* Ctrl+Esc: start menu */
        else if (kbd_state.alt && code == 0x0F) hot = 2;             /* Alt+Tab */
        else if (kbd_state.alt && code == 0x01) hot = 3;             /* Alt+Esc */
        else if (kbd_state.ctrl && kbd_state.alt && code == 0x53) hot = 4;  /* Ctrl+Alt+Del */
        else if (kbd_state.alt && code == 0x1C && vm_focus) hot = 5; /* Alt+Enter: a DOS box's (v40, #219: only
                                                                        a DOS box's - a program's own key otherwise,
                                                                        as in Windows 95: 7-Zip's Properties) */
        else if (kbd_state.alt && code == 0x39) hot = 6;             /* Alt+Space */
        else if (kbd_state.alt && code == 0x3E && !fs_live) hot = 7; /* Alt+F4 (v41, #455: a full-screen DOS program's own key,
                                                                        as Windows 95 keeps it for it) */
        else if (kbd_state.ctrl && kbd_state.alt && code == 0x0F) hot = 8;  /* Ctrl+Alt+Tab */
        else if (kbd_state.ctrl && !kbd_state.alt && code == 0x57 && vm_focus) hot = 9;    /* v43 (#507): Ctrl+F11 - a DOS box */
        else if (kbd_state.ctrl && !kbd_state.alt && code == 0x58 && vm_focus) hot = 10;   /* slower, Ctrl+F12 faster (DOSBox's keys) */
    }
    if (hot) {
        struct event e = { EV_HOTKEY, hot, 0, 0, kbd_mods(), 0, 0 };
        ev_post(&e);
        return;
    }
    /* a native app (QEMU) that owns the keyboard gets raw scancodes, like a DOS box */
#ifdef QDP
  raw:
#endif
    if (kbd_raw_hook && kbd_raw_hook(code | (ext ? 0x100 : 0), release)) {
        if (code == 0x38 || code == 0x1D || code == 0x2A || code == 0x36) {
            struct event e = { release ? EV_KEYUP : EV_KEYDOWN, code | (ext ? 0x100 : 0), 0, 0, kbd_mods(), 0, 0 };
            ev_post(&e);
        }
        return;
    }
    /* Alt+Tab release handling: post key up of alt so the GUI can finish the switch */
    struct vm *vm = vm_focus;
    /* v41 (#336, K56): a menu up over a DOS box - its system menu (Alt+Space), the Start menu
     * (Ctrl+Esc) - takes the keys pressed, as in Windows 95 (they went to the box: the menu
     * could not be used from the keyboard, and what was typed for it the box's program got);
     * a release still goes to the box, so no key it saw go down is left down there */
    if (vm && (vm->state == VMS_RUNNING || vm->state == VMS_NEW) && (!menu_current || release)) {
        if (ext) vm_inject_key(vm, 0xE0);
        vm_inject_key(vm, sc);
        /* also mirror modifier keys to the GUI so Alt-release ends Alt+Tab */
        if (code == 0x38 || code == 0x1D || code == 0x2A || code == 0x36) {
            struct event e = { release ? EV_KEYUP : EV_KEYDOWN, code | (ext ? 0x100 : 0), 0, 0, kbd_mods(), 0, 0 };
            ev_post(&e);
        }
        return;
    }
    struct event e = { release ? EV_KEYUP : EV_KEYDOWN, code | (ext ? 0x100 : 0), 0, 0, kbd_mods(), 0, 0 };
    ev_post(&e);
    if (!release) {
        char ch = 0;
        if (!ext) {
            int up = kbd_state.shift;
            if (code >= 0x47 && code <= 0x53) { if (!kbd_state.num) ch = 0; else ch = sc_normal[code]; }
            else {
                ch = up ? sc_shift[code] : sc_normal[code];
                if (kbd_state.caps && isalpha(ch)) ch = up ? tolower(ch) : toupper(ch);
            }
            if (kbd_state.ctrl && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
            else if (kbd_state.ctrl && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
        } else if (code == 0x1C) ch = 13;
        else if (code == 0x35) ch = '/';
        if (ch) {
            struct event c = { EV_CHAR, (uint8_t)ch, 0, 0, kbd_mods(), 0, code };
            ev_post(&c);
        }
    }
}

static void kbc_wait_write(void) { int t = 100000; while ((inb(0x64) & 2) && --t) ; }
static void kbc_wait_read(void)  { int t = 100000; while (!(inb(0x64) & 1) && --t) ; }

void kbd_set_leds(void)
{
    uint8_t leds = (kbd_state.scroll ? 1 : 0) | (kbd_state.num ? 2 : 0) | (kbd_state.caps ? 4 : 0);
    { extern void usb_leds(uint8_t); usb_leds(leds); }
    if (!kbc_present) return;
    kbc_wait_write(); outb(0x60, 0xED);
    kbc_wait_read(); inb(0x60);
    kbc_wait_write(); outb(0x60, leds);
    kbc_wait_read(); inb(0x60);
}

void kbd_init(void)
{
    /* take the shift state DOS left behind */
    uint8_t *bda = PHYSLOW(0x417);
    kbd_state.num = (bda[0] & 0x20) ? 1 : 0;
    kbd_state.caps = (bda[0] & 0x40) ? 1 : 0;
    kbd_state.scroll = (bda[0] & 0x10) ? 1 : 0;
    if (inb(0x64) == 0xFF) {                         /* (v42: no keyboard controller - a USB keyboard's, usbhid.c) */
        kbc_present = 0;
        kprintf("kbd: no keyboard controller (no PS/2 ports) - the keyboard is a USB one\n");
        return;
    }
    /* flush */
    for (int i = 0; i < 32 && (inb(0x64) & 1); i++) inb(0x60);
    irq_register(1, kbd_irq);
    irq_mask(1, 0);
}

void kbd_set_typematic(int rate, int delay)
{
    { extern void usbhid_typematic(int, int); usbhid_typematic(rate, delay); }   /* (v42: a USB keyboard's repeat is the kernel's) */
    if (!kbc_present) return;
    uint8_t v = (rate & 31) | ((delay & 3) << 5);
    kbc_wait_write(); outb(0x60, 0xF3);
    kbc_wait_read(); inb(0x60);
    kbc_wait_write(); outb(0x60, v);
    kbc_wait_read(); inb(0x60);
}
