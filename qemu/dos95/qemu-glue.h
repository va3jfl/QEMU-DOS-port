/* Interface between the QEMU side (the C files of qemu/dos95/, built with the QEMU headers)
 * and the DOS95 side (kernel/vpc.c, built with the kernel headers).  Plain C
 * types only - neither side includes the other's headers. */
#ifndef QEMU_GLUE_H
#define QEMU_GLUE_H
#include <stdint.h>

/* --- provided by kernel/vpc.c (DOS95 side) --- */
void vpc_display_resize(int w, int h);                                   /* guest screen size changed */
void vpc_display_update(int x, int y, int w, int h, const uint8_t *data, int linesize, int bpp,
                        uint32_t rmask, uint32_t gmask, uint32_t bmask); /* rectangle of the surface changed */
int  vpc_input_pop(int *type, int *a, int *b, int *c, int *d);          /* 0 if none; type 0 key, 1 mouse */
void vpc_thread_exit(int code) __attribute__((noreturn));
void vpc_set_tick_hook(void (*fn)(void));
int  vpc_stop_requested(void);                                           /* window closed: shut the guest down */
void vpc_status(const char *text);
/* network bridge (kernel/net.c): the guest NIC becomes a client of the DOS95 Ethernet bridge */
int   vpc_net_available(void);
void *vpc_net_open(const char *name);
void  vpc_net_close(void *h);
int   vpc_net_send(void *h, const uint8_t *frame, int len);      /* guest -> LAN */
int   vpc_net_recv(void *h, uint8_t *buf, int max);              /* LAN -> guest; 0 if nothing queued */
int   vpc_net_pending(void);                                     /* frames waiting for any guest */
/* sound: the guest's SB16 output through the kernel's host PCM engine (kernel/snd.c) */
int   vpc_snd_available(void);
int   vpc_snd_open(int rate);                                    /* 16-bit signed stereo */
void  vpc_snd_close(void);
int   vpc_snd_free(void);                                        /* bytes the ring can take now */
int   vpc_snd_write(const void *pcm, int bytes);                 /* returns bytes accepted */

#ifdef QDP
/* the QEMU DOS port: a CD or floppy image changed from the manager (kernel/vpc.c vpc_change_media) */
char *vpc_media_take(int drive);                                 /* 0 CD-ROM, 1 floppy A, 2 floppy B: the new path (NULL out) */
void  vpc_media_result(int drive, int ok);
#endif
/* --- provided by the QEMU side --- */
int  qemu_main(int argc, char **argv, char **envp);
void qemu_dos95_request_shutdown(void);
#endif
