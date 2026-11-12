/* DOS95 host layer for QEMU: the "file" protocol block driver.
 *
 * Disk images are plain DOS files reached through the libc shim (open/pread/
 * pwrite on fs_*).  Synchronous only: block.c emulates the AIO interface on top
 * of bdrv_read/bdrv_write for drivers that don't provide it.  Images are
 * limited to 2 GB by DOS file offsets. */
#include "qemu-common.h"
#include "block_int.h"
#include "module.h"
#include "dos95-host.h"

#ifdef QDP
/* the QEMU DOS port: a disk past FAT32's 4 GB file, to 2 TB - a "split" image.  The file the machine names is a few
 * lines of text ("QEMU DOS port split disk image", size=, piece=); the disk's bytes are in the files beside it named as it
 * is with the extensions .000 to .FFF (hex), piece= bytes each.  A piece not there reads as zeros and is made at the first
 * write to it; one shorter than piece= reads as zeros past its end, and a write past its end writes the zeros between
 * first - so the pieces take the room the machine has written, and nothing it did not write is read back as anything but
 * zeros.  At most SPLIT_OPEN pieces are open at once (DOS's file handles): the one used longest ago is closed. */
#define SPLIT_SIG  "QEMU DOS port split disk image"
#define SPLIT_OPEN 6
struct split_piece { int fd, idx; int64_t len; uint32_t used; };
#endif
typedef struct {
    int fd; int64_t size; int readonly;
#ifdef QDP
    int split; int64_t piece; char base[512]; struct split_piece open[SPLIT_OPEN]; uint32_t clock;
#endif
} BDRVDos95State;
#ifdef QDP
static int64_t parse_u64(const char *p) { int64_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'); return v; }
/* the piece idx open (opened, made if create): its slot, or -1 (not there and not made: zeros) */
static int split_get(BDRVDos95State *s, int idx, int create)
{
    int lru = 0;
    for (int i = 0; i < SPLIT_OPEN; i++) {
        if (s->open[i].fd >= 0 && s->open[i].idx == idx) { s->open[i].used = ++s->clock; return i; }
        if (s->open[i].fd < 0 || s->open[i].used < s->open[lru].used) lru = i;
        if (s->open[i].fd < 0) { lru = i; break; }
    }
    char name[530]; snprintf(name, sizeof name, "%s.%03X", s->base, idx);
    int fd = open(name, s->readonly ? O_RDONLY : O_RDWR);
    if (fd < 0 && create && !s->readonly) fd = open(name, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (s->open[lru].fd >= 0) close(s->open[lru].fd);
    s->open[lru].fd = fd; s->open[lru].idx = idx; s->open[lru].used = ++s->clock;
    s->open[lru].len = lseek(fd, 0, SEEK_END);
    return lru;
}
static int split_rw(BDRVDos95State *s, int64_t off, uint8_t *buf, int len, int write)
{
    static uint8_t zero[65536];
    while (len > 0) {
        int idx = (int)(off / s->piece);
        int64_t in = off % s->piece;
        int n = (int64_t)len < s->piece - in ? len : (int)(s->piece - in);
        if (idx > 0xFFF) return -EIO;
        int k = split_get(s, idx, write);
        if (!write) {
            int got = 0;
            if (k >= 0 && in < s->open[k].len) {
                int want = (int64_t)n < s->open[k].len - in ? n : (int)(s->open[k].len - in);
                while (got < want) { ssize_t r = pread(s->open[k].fd, buf + got, want - got, in + got); if (r < 0) return -errno; if (r == 0) break; got += r; }
            }
            if (got < n) memset(buf + got, 0, n - got);
        } else {
            if (k < 0) return -EIO;
            struct split_piece *pc = &s->open[k];
            while (pc->len < in) {                                  /* (the zeros between its end and the write) */
                int z = in - pc->len > (int64_t)sizeof zero ? (int)sizeof zero : (int)(in - pc->len);
                ssize_t r = pwrite(pc->fd, zero, z, pc->len);
                if (r <= 0) return r < 0 ? -errno : -ENOSPC;
                pc->len += r;
            }
            int done = 0;
            while (done < n) { ssize_t r = pwrite(pc->fd, buf + done, n - done, in + done); if (r <= 0) return r < 0 ? -errno : -ENOSPC; done += r; }
            if (in + n > pc->len) pc->len = in + n;
        }
        off += n; buf += n; len -= n;
    }
    return 0;
}
#endif

static int dos95_raw_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVDos95State *s = bs->opaque;
    int oflags = (flags & BDRV_O_RDWR) ? O_RDWR : O_RDONLY;
    s->fd = open(filename, oflags);
    if (s->fd < 0 && oflags == O_RDWR) { s->fd = open(filename, O_RDONLY); s->readonly = 1; }
    if (s->fd < 0) return -errno;
    s->size = lseek(s->fd, 0, SEEK_END);
#ifdef QDP
    for (int i = 0; i < SPLIT_OPEN; i++) s->open[i].fd = -1;
    if (s->size < 4096) {
        char head[512]; ssize_t n = pread(s->fd, head, sizeof head - 1, 0);
        if (n > (ssize_t)strlen(SPLIT_SIG) && !memcmp(head, SPLIT_SIG, strlen(SPLIT_SIG))) {
            head[n] = 0;
            const char *sz = strstr(head, "size="), *pc = strstr(head, "piece=");
            int64_t size = sz ? parse_u64(sz + 5) : 0, piece = pc ? parse_u64(pc + 6) : 0;
            if (size <= 0 || piece < 65536 || piece > 0x7FFFFFFFLL || (piece & 511) || size > piece * 4096) return -EINVAL;
            s->split = 1; s->size = size & ~511LL; s->piece = piece;
            snprintf(s->base, sizeof s->base, "%s", filename);
            char *dot = strrchr(s->base, '.'), *bs = strrchr(s->base, '\\');
            if (dot && (!bs || dot > bs)) *dot = 0;
        }
    }
#endif
    return 0;
}
static int dos95_raw_read(BlockDriverState *bs, int64_t sector_num, uint8_t *buf, int nb_sectors)
{
    BDRVDos95State *s = bs->opaque;
    int64_t off = sector_num * 512; int len = nb_sectors * 512;
#ifdef QDP
    if (s->split) return split_rw(s, off, buf, len, 0);
#endif
    while (len > 0) {
        ssize_t r = pread(s->fd, buf, len, off);
        if (r < 0) return -errno;
        if (r == 0) { memset(buf, 0, len); return 0; }   /* reading past EOF: zeros (sparse images) */
        buf += r; off += r; len -= r;
    }
    return 0;
}
static int dos95_raw_write(BlockDriverState *bs, int64_t sector_num, const uint8_t *buf, int nb_sectors)
{
    BDRVDos95State *s = bs->opaque;
    if (s->readonly) return -EACCES;
    int64_t off = sector_num * 512; int len = nb_sectors * 512;
#ifdef QDP
    if (s->split) return split_rw(s, off, (uint8_t *)buf, len, 1);
#endif
    while (len > 0) {
        ssize_t r = pwrite(s->fd, buf, len, off);
        if (r <= 0) return r < 0 ? -errno : -EIO;
        buf += r; off += r; len -= r;
    }
    if (off > s->size) s->size = off;
    return 0;
}
static void dos95_raw_close(BlockDriverState *bs)
{
    BDRVDos95State *s = bs->opaque;
#ifdef QDP
    for (int i = 0; i < SPLIT_OPEN; i++) if (s->split && s->open[i].fd >= 0) { close(s->open[i].fd); s->open[i].fd = -1; }
#endif
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
}
static int64_t dos95_raw_getlength(BlockDriverState *bs) { BDRVDos95State *s = bs->opaque; return s->size; }
static int dos95_raw_truncate(BlockDriverState *bs, int64_t offset)
{
    BDRVDos95State *s = bs->opaque;
#ifdef QDP
    if (s->split) return -ENOTSUP;
#endif
    if (ftruncate(s->fd, offset) < 0) return -errno;
    s->size = offset; return 0;
}
static int dos95_raw_flush(BlockDriverState *bs) { return 0; }
static int dos95_raw_create(const char *filename, QEMUOptionParameter *options)
{
    int64_t total_size = 0;
    while (options && options->name) { if (!strcmp(options->name, BLOCK_OPT_SIZE)) total_size = options->value.n / 512; options++; }
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -EIO;
    int ret = 0;
    if (ftruncate(fd, total_size * 512) < 0) ret = -errno;
    close(fd);
    return ret;
}
static QEMUOptionParameter dos95_raw_create_options[] = {
    { .name = BLOCK_OPT_SIZE, .type = OPT_SIZE, .help = "Virtual disk size" },
    { NULL }
};
static BlockDriver bdrv_dos95_file = {
    .format_name = "file",
    .protocol_name = "file",
    .instance_size = sizeof(BDRVDos95State),
    .bdrv_probe = NULL,
    .bdrv_file_open = dos95_raw_open,
    .bdrv_read = dos95_raw_read,
    .bdrv_write = dos95_raw_write,
    .bdrv_close = dos95_raw_close,
    .bdrv_create = dos95_raw_create,
    .bdrv_flush = dos95_raw_flush,
    .bdrv_truncate = dos95_raw_truncate,
    .bdrv_getlength = dos95_raw_getlength,
    .create_options = dos95_raw_create_options,
};
static void bdrv_dos95_init(void) { bdrv_register(&bdrv_dos95_file); }
block_init(bdrv_dos95_init);
