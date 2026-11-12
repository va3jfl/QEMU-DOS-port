/*
 * QEMU DOS port: the FM synthesizer of a Sound Blaster 16 - a Yamaha YMF262 (OPL3) - for the machine, as QEMU's
 * -soundhw adlib.  QEMU 0.14's own adlib.c (MAME's fmopl, an OPL2) is not in this tree; the sound here is Nuked OPL3's
 * (synth/src/Nuked-OPL3-765ec96, LGPL 2.1: the YMF262 as its die shows it), through DOS95's glue
 * (synth/dos95/opl3glue.c), the same chip a DOS box of DOS95 has (kernel/fmemu.c).
 *
 *   Its ports: 388h-38Bh (bank 0's index and data, bank 1's), the Sound Blaster's 220h-223h, and 228h-229h (bank 0).
 *   Its timers: a timer started and not masked is over at the next look at the status - the AdLib detection every game
 *       runs reads what it wrote, at any speed (VADLIBD's way, as kernel/fmemu.c); the status's bits 2..1 clear: an OPL3.
 *   Its sound: made at 44100 Hz stereo as QEMU's audio asks for it (its callback) - quiet two seconds after its notes
 *       are over, its voice is put to sleep until it is written again (no processor spent on silence).
 * Built for the QEMU DOS port only (Makefile.qdp); QEMU's soundhw list has it under CONFIG_ADLIB (arch_init.c).
 */
#include "hw.h"
#include "audiodev.h"
#include "audio/audio.h"
#include "isa.h"

extern unsigned fm_chip_size(void);
extern void fm_chip_reset(void *c, unsigned rate);
extern void fm_chip_write(void *c, unsigned reg, unsigned v);
extern int fm_chip_make(void *c, int16_t *out, unsigned n);

#define FM_FREQ   44100
#define FM_FRAMES 512
#define FM_QUIET  (FM_FREQ * 2)                 /* frames of silence before the voice sleeps */

typedef struct {
    QEMUSoundCard card;
    SWVoiceOut *voice;
    int active;
    uint32_t quiet;                             /* frames of silence made since the last sound */
    uint8_t idx[2];                             /* the index written last, bank 0 and bank 1 */
    uint8_t tctl;                               /* register 4: the timers' masks (bits 6, 5) and starts (bits 0, 1) */
    int16_t buf[FM_FRAMES * 2];
    void *chip;
} FMState;

static void fm_wake(FMState *s)
{
    s->quiet = 0;
    if (!s->active && s->voice) { AUD_set_active_out(s->voice, 1); s->active = 1; }
}
static void fm_index(FMState *s, int bank, uint32_t val) { s->idx[bank] = (uint8_t)val; }
static void fm_data(FMState *s, int bank, uint32_t val)
{
    unsigned reg = s->idx[bank] | (bank << 8);
    if (reg == 0x04) s->tctl = (val & 0x80) ? 0 : (uint8_t)val;   /* the timers: IRQ reset (the flags off), or starts and masks */
    fm_chip_write(s->chip, reg, val & 0xFF);
    fm_wake(s);
}
static uint32_t fm_status(FMState *s)
{
    uint32_t st = 0;
    if ((s->tctl & 0x01) && !(s->tctl & 0x40)) st |= 0x40;
    if ((s->tctl & 0x02) && !(s->tctl & 0x20)) st |= 0x20;
    if (st) st |= 0x80;
    return st;
}
/* 388h-38Bh and 220h-223h: index 0, data 1 (bank 0); index 2, data 3 (bank 1) */
static void fm_write4(void *opaque, uint32_t port, uint32_t val)
{
    FMState *s = opaque;
    int a = port & 3;
    if (a & 1) fm_data(s, a >> 1, val); else fm_index(s, a >> 1, val);
}
static uint32_t fm_read4(void *opaque, uint32_t port) { return (port & 3) == 0 ? fm_status(opaque) : 0xFF; }
/* 228h-229h: bank 0 */
static void fm_write2(void *opaque, uint32_t port, uint32_t val)
{
    FMState *s = opaque;
    if (port & 1) fm_data(s, 0, val); else fm_index(s, 0, val);
}
static uint32_t fm_read2(void *opaque, uint32_t port) { return (port & 1) == 0 ? fm_status(opaque) : 0xFF; }

static void fm_callback(void *opaque, int free)
{
    FMState *s = opaque;
    int frames = free / 4;
    while (frames > 0) {
        int n = frames > FM_FRAMES ? FM_FRAMES : frames;
        if (fm_chip_make(s->chip, s->buf, n)) s->quiet = 0; else s->quiet += n;
        int w = AUD_write(s->voice, s->buf, n * 4);
        if (w <= 0) break;
        frames -= w / 4;
    }
    if (s->quiet >= FM_QUIET && s->active) { AUD_set_active_out(s->voice, 0); s->active = 0; }
}

int Adlib_init(qemu_irq *pic)
{
    FMState *s = qemu_mallocz(sizeof *s);
    s->chip = qemu_mallocz(fm_chip_size());
    fm_chip_reset(s->chip, FM_FREQ);
    struct audsettings as;
    as.freq = FM_FREQ; as.nchannels = 2; as.fmt = AUD_FMT_S16; as.endianness = AUDIO_HOST_ENDIANNESS;
    AUD_register_card("adlib", &s->card);
    s->voice = AUD_open_out(&s->card, s->voice, "adlib", s, fm_callback, &as);
    if (!s->voice) {
        AUD_remove_card(&s->card);
        qemu_free(s->chip); qemu_free(s);
        return -1;
    }
    register_ioport_read(0x388, 4, 1, fm_read4, s);
    register_ioport_write(0x388, 4, 1, fm_write4, s);
    register_ioport_read(0x220, 4, 1, fm_read4, s);
    register_ioport_write(0x220, 4, 1, fm_write4, s);
    register_ioport_read(0x228, 2, 1, fm_read2, s);
    register_ioport_write(0x228, 2, 1, fm_write2, s);
    return 0;
}
