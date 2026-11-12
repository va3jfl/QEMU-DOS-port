/* QEMU DOS port - what its shell (qdpshell.c), its manager (qdpman.c), its setup (qdpsetup.c) and the
 * machine window (kernel/vpc.c, under #ifdef QDP) share */
#ifndef QDP_H
#define QDP_H

#define QDP_NAME     "QEMU DOS port"
#define QDP_VERSION  "1.0"
#define QDP_ENGINE   "QEMU 0.14.1"

/* the hotkeys of the QEMU DOS port (kernel/kbd.c posts them as EV_HOTKEY codes; qdpshell.c qdp_hotkey) */
#define QDP_HOT_MANAGER   20     /* Ctrl+Alt+Home: the manager in front, or the machine again */
#define QDP_HOT_CAD       21     /* Ctrl+Alt+End: Ctrl+Alt+Del to the machine */
#define QDP_HOT_SCALE     22     /* Ctrl+Alt+S: the machine's picture: fit / stretched / pixel for pixel */

/* the machine running (vpc.c) - one at a time: QEMU 0.14's state is global */
enum { VPC_NONE = 0, VPC_STARTING, VPC_RUNNING, VPC_PAUSED, VPC_STOPPING };
int  vpc_state(void);                         /* VPC_* */
const char *vpc_machine_file(void);           /* the .VM file of the machine running ("" none) */
struct window *vpc_window(void);              /* its window (NULL none) */
int  vpc_launch_machine(const char *cmdline, const char *title, const char *vmfile);
void vpc_show(void);                          /* its window in front, the keyboard its */
void vpc_pause(int on);
void vpc_reset(void);
void vpc_power_button(void);                  /* ACPI: the guest shuts itself down */
void vpc_power_off(void);                     /* at once, as pulling the plug */
void vpc_send_cad(void);
int  vpc_change_media(int drive, const char *path);   /* drive 0 the CD-ROM, 1 floppy A, 2 floppy B; path NULL: ejected */
int  vpc_thumbnail(uint32_t *dst, int w, int h);       /* its screen scaled into dst (w x h): 1, or 0 (nothing to show) */
void vpc_screen_size(int *w, int *h);
int  vpc_scale_mode(void);                    /* 0 fit (aspect kept), 1 stretched, 2 pixel for pixel */
void vpc_set_scale_mode(int m);

/* the manager (qdpman.c) */
struct window *qdpman_window(void);
void qdpman_show(void);
void qdpman_vm_changed(void);                 /* a machine started, stopped, paused: its list and buttons again */
void qdpman_vm_stopped(const char *vmfile, int exit_code);   /* (the autoboot's "when it shuts down") */
int  qdpman_start_by_name(const char *name_or_file, int quiet);
void qdpman_autoboot(void);                   /* at the start: the machine set to start by itself (a countdown, Esc stops it) */

/* the shell (qdpshell.c) */
void qdp_shell_start(void);
void qdp_exit_to_dos(struct window *owner, int ask);
void qdp_restart(struct window *owner, int ask);
extern int qdp_setup_mode;                    /* QEMUDOS /SETUP: the setup, not the manager */
extern int qdp_no_autoboot;                   /* QEMUDOS /NOAUTO */
extern char qdp_start_machine[260];           /* QEMUDOS /M name: that machine at once */
void qdp_draw_big_text(int x, int y, int scale, uint32_t color, const char *s);
int  qdp_big_text_width(int scale, const char *s);
const char *vpc_qemu_last_said(void);            /* kernel/vpc.c: the last line QEMU wrote to stderr in its last run */
const char *qdp_dir(void);                    /* where QEMUDOS.COM is, no backslash at its end ("C:\QEMUDOS") */

/* the setup (qdpsetup.c) */
void qdp_setup_start(void);

#endif
