#ifndef VESA_H
#define VESA_H
#include "types.h"
struct vesa_mode { uint16_t mode; int w, h, bpp, pitch; uint32_t lfb; uint8_t rsize, rpos, gsize, gpos, bsize, bpos; uint32_t gran; };
/* v41 (#439, reconstructed 2026-11-10: the v41-2 header was never packaged): a VESA 1.2 card's window granularity (0: linear) */
extern uint32_t gfx_bank_gran;
int vesa_bank(int b);
extern struct vesa_mode *vesa_modes;     /* (v41, #336: as many as the BIOS lists) */
extern int vesa_nmodes;
extern struct vesa_mode vesa_cur;
int vesa_enumerate(void);
struct vesa_mode *vesa_find(int w, int h, int bpp);
struct vesa_mode *vesa_choose(int w, int h, int bpp);
int vesa_set(struct vesa_mode *m);
void vesa_text_mode(void);
#endif
