/* DOS95 kernel - boot splash / initialization status screen.
 *
 * An XP-style animated boot screen: a dark vertical gradient with the DOS95
 * wordmark, and a KDE-style row of stage icons that light up (grey -> full
 * colour, with a caption) as each subsystem comes up.  Purely cosmetic; it
 * runs on the GUI thread between setting the video mode and showing the
 * desktop, so the user watches the system assemble itself instead of a blank
 * screen. */
#include "kernel.h"
#include "gfx.h"
#include "icons.h"
#include "font.h"

struct stage { int icon; const char *label; };
#ifdef QDP
static const struct stage stages[] = {      /* (the QEMU DOS port: gui.c's steps, its last two the manager's) */
    { ICO_DISPLAY, "Display" },
    { ICO_DRIVE_HDD, "Disks" },
    { ICO_NETWORK, "Network" },
    { ICO_SOUND, "Audio" },
    { ICO_SETTINGS, "Engine" },
    { ICO_COMPUTER, "Machines" },
};
#else
static const struct stage stages[] = {
    { ICO_DISPLAY, "Display" },
    { ICO_DRIVE_HDD, "File system" },
    { ICO_NETWORK, "Network" },
    { ICO_SOUND, "Audio" },
    { ICO_SETTINGS, "Services" },
    { ICO_DOSAPP, "Applications" },
    { ICO_COMPUTER, "Desktop" },
};
#endif
#define NSTAGE (int)(sizeof stages / sizeof stages[0])
#define ICON_SZ 32
#define GAP 40

static int done_count;
static int base_x, base_y, row_w;
static int active;

static uint32_t mix(uint32_t a, uint32_t b, int t)   /* t 0..255 */
{
    int ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba = a & 0xFF;
    int rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ra + (rb - ra) * t / 255, g = ga + (gb - ga) * t / 255, bl = ba + (bb - ba) * t / 255;
    return (r << 16) | (g << 8) | bl;
}

static void background(void)
{
    uint32_t top = 0x0A1B3D, bot = 0x1E4C8C;         /* deep navy -> steel blue */
    for (int y = 0; y < scr_h; y++)
        gfx_hline(0, y, scr_w, mix(top, bot, y * 255 / (scr_h - 1)));
    /* wordmark */
    int cx = scr_w / 2;
#ifdef QDP
    {   /* the QEMU DOS port: its name three times the UI font's size, what it is under it */
        extern void qdp_draw_big_text(int x, int y, int scale, uint32_t color, const char *s);
        extern int qdp_big_text_width(int scale, const char *s);
        const char *qn = "QEMU DOS port";
        int sc = scr_w >= 1024 ? 4 : 3, qw = qdp_big_text_width(sc, qn), qy = scr_h / 3 - font_ui_bold.height * sc / 2;
        qdp_draw_big_text(cx - qw / 2 + sc, qy + sc, sc, 0x081228, qn);
        qdp_draw_big_text(cx - qw / 2, qy, sc, 0xFFFFFF, qn);
        const char *qs = "QEMU 0.14.1 virtual machines on MS-DOS 7.1";
        int qsw = gfx_text_width(&font_ui, qs);
        gfx_text(&font_ui, cx - qsw / 2, qy + font_ui_bold.height * sc + 10, 0xAFC4E4, qs);
        return;
    }
#endif
    const char *name = "DOS95";
    int nw = gfx_text_width(&font_ui_bold, name);
    /* draw the logo and an enlarged look by stacking the bold word with a shadow */
    gfx_icon(ICO_LOGO, ICON_SZ, cx - ICON_SZ - nw / 2 - 8, scr_h / 3 - ICON_SZ / 2);
    gfx_text(&font_ui_bold, cx - nw / 2 + 1, scr_h / 3 - font_ui_bold.height / 2 + 1, 0x081228, name);
    gfx_text(&font_ui_bold, cx - nw / 2, scr_h / 3 - font_ui_bold.height / 2, 0xFFFFFF, name);
    const char *sub = "Multitasking DOS";
    int sw = gfx_text_width(&font_small, sub);
    gfx_text(&font_small, cx - sw / 2, scr_h / 3 + font_ui_bold.height, 0xAFC4E4, sub);
}

static void draw_stage(int i, int lit)
{
    int x = base_x + i * (ICON_SZ + GAP);
    int y = base_y;
    if (lit) gfx_icon(stages[i].icon, ICON_SZ, x, y);
    else     gfx_icon_gray(stages[i].icon, ICON_SZ, x, y);
    const char *l = stages[i].label;
    int lw = gfx_text_width(&font_small, l);
    gfx_text(&font_small, x + ICON_SZ / 2 - lw / 2, y + ICON_SZ + 4, lit ? 0xFFFFFF : 0x5E7196, l);
    gfx_flush(x - GAP / 2, y - 2, ICON_SZ + GAP, ICON_SZ + 20);
}

void splash_begin(void)
{
    row_w = NSTAGE * ICON_SZ + (NSTAGE - 1) * GAP;
    base_x = (scr_w - row_w) / 2;
    base_y = scr_h * 2 / 3;
    done_count = 0; active = 1;
    background();
    for (int i = 0; i < NSTAGE; i++) draw_stage(i, 0);
    gfx_flush_all();
}

/* advance to the next stage: light the icon for the work about to run and
 * leave a short beat so the animation is visible even on a fast machine */
void splash_step(void)
{
    if (!active || done_count >= NSTAGE) return;
    draw_stage(done_count, 1);
    done_count++;
    /* a small pulse of the newly-lit icon */
    thread_sleep_ms(260);
}

void splash_end(void)
{
    if (!active) return;
    for (; done_count < NSTAGE; done_count++) draw_stage(done_count, 1);
    thread_sleep_ms(400);
    active = 0;
}

int splash_active(void) { return active; }
