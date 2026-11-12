/* QEMU DOS port - its setup: QEMUDOS /SETUP, started by the setup CD's (or USB stick's) AUTOEXEC.BAT.
 *
 * Two ways, chosen on its first page:
 *  - a disk of its own: the disk is erased, one MS-DOS 7.1 partition (FAT32, as large as asked - to all of the disk, up to
 *    MBR's 2 TB) is made and formatted, MS-DOS 7.1 and the QEMU DOS port are written to it, and the PC starts into the
 *    QEMU manager.  The partition, its file system and its files are written here, sector by sector, by INT 13h (the
 *    kernel answers it for a disk it drives itself): DOS gives its drive letters at its start, and the disk it would
 *    format has none yet.  The boot code is MS-DOS 7.1's own (SETUP\FAT32VBR.BIN, as its FORMAT /S wrote it) under the
 *    QEMU DOS port's MBR (SETUP\MBR.BIN, qdp/boot/mbr.asm).
 *  - beside an MS-DOS that is there: the QEMU DOS port copied to a folder of one of its drives, and a line for it in
 *    AUTOEXEC.BAT if asked - nothing formatted.
 * What is installed is the setup disk's SETUP\FILES folder, as the new disk's root is to be. */
#include "shell.h"
#include "dosfs.h"
#include "app.h"
#include "font.h"
#include "qdp.h"

extern int qdp_int13_rw(int unit, uint64_t lba, int count, void *buf, int write);
extern int qdp_int13_units(void);
extern int qdp_int13_size(int unit, uint64_t *sectors);
extern int ata_unit(uint8_t unit);
extern int ata_read(uint8_t unit, uint64_t lba, int count, uint8_t *buf);
extern int ata_write(uint8_t unit, uint64_t lba, int count, const uint8_t *buf);
extern const char *ata_unit_desc(uint8_t unit, char *out, int n);
extern int ata_unit_is_gpt(uint8_t unit);
extern void ata_unit_erase_ok(uint8_t unit);
extern void dcache_drop_unit(uint8_t unit);

#define SETUP_TITLE QDP_NAME " Setup"
static char src_root[64];                       /* "C:\SETUP\FILES" - where the files to install are */
static char setup_dir[64];                      /* "C:\SETUP" */

/* ---- sectors of a BIOS unit --------------------------------------------------------------------------------------- */
static int sd_io(int unit, uint64_t lba, int count, void *buf, int write)
{
    if (count <= 0) return 0;
    if (ata_unit((uint8_t)unit) >= 0) {             /* (a disk the kernel drives: its own reads and writes, 128 at a time) */
        uint8_t *p = buf;
        while (count > 0) {
            int n = count > 128 ? 128 : count;
            int r = write ? ata_write((uint8_t)unit, lba, n, p) : ata_read((uint8_t)unit, lba, n, p);
            if (r) return qdp_int13_rw(unit, lba, count, p, write);   /* (refused or given back to the BIOS: INT 13h) */
            p += (uint32_t)n * 512; lba += (uint64_t)n; count -= n;
        }
        return 0;
    }
    return qdp_int13_rw(unit, lba, count, buf, write);
}
static uint8_t zbuf[64 * 512];                  /* zeros */
static int sd_zero(int unit, uint64_t lba, uint64_t count, struct window *pg, const char *what, int pct0, int pct1)
{
    uint64_t done = 0;
    while (done < count) {
        int n = count - done > 64 ? 64 : (int)(count - done);
        if (sd_io(unit, lba + done, n, zbuf, 1)) return -1;
        done += (uint64_t)n;
        if (pg && (done & 0x3FFF) < 64) {
            char t[96]; ksnprintf(t, sizeof t, "%s %u%%", what, (uint32_t)(done * 100 / count));
            if (progress_update(pg, t, pct0 + (int)((uint64_t)(pct1 - pct0) * done / count))) return -2;
        }
    }
    return 0;
}

/* ---- the disks -------------------------------------------------------------------------------------------------- */
struct sdisk { int unit; uint64_t sectors; int gpt; char desc[96]; char parts[160]; };
static struct sdisk disks[16];
static int ndisks;
static const char *ptype_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12"; case 0x04: case 0x06: case 0x0E: return "FAT16"; case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT"; case 0x05: case 0x0F: return "extended"; case 0x82: return "Linux swap"; case 0x83: return "Linux";
    case 0xEE: return "GPT"; case 0xEF: return "EFI"; default: return "other";
    }
}
static void size_text(uint64_t sectors, char *out, int n)
{
    uint64_t mb = sectors / 2048;
    if (mb >= 10240) ksnprintf(out, n, "%u GB", (uint32_t)(mb / 1024));
    else if (mb >= 1024) ksnprintf(out, n, "%u.%u GB", (uint32_t)(mb / 1024), (uint32_t)(mb % 1024 * 10 / 1024));
    else ksnprintf(out, n, "%u MB", (uint32_t)mb);
}
static void find_disks(void)
{
    ndisks = 0;
    int n = qdp_int13_units();
    static uint8_t s0[512];
    for (int u = 0x80; u < 0x80 + n && u <= 0x8F && ndisks < 16; u++) {
        uint64_t secs;
        if (qdp_int13_size(u, &secs) < 0 || secs < 128 * 2048) continue;     /* (128 MB at least) */
        if (sd_io(u, 0, 1, s0, 0)) continue;
        if (*(uint32_t*)(s0 + 0x1B8) == 0x53504451u) continue;                 /* ('QDPS': the setup's own disk) */
        struct sdisk *d = &disks[ndisks++];
        d->unit = u; d->sectors = secs; d->gpt = ata_unit_is_gpt((uint8_t)u);
        char model[80]; ata_unit_desc((uint8_t)u, model, sizeof model);
        char sz[24]; size_text(secs, sz, sizeof sz);
        ksnprintf(d->desc, sizeof d->desc, "Disk %d - %s%s%s", u - 0x7F, sz, model[0] ? " - " : "", model);
        d->parts[0] = 0;
        int any = 0;
        if (s0[510] == 0x55 && s0[511] == 0xAA)
            for (int i = 0; i < 4; i++) {
                const uint8_t *e = s0 + 0x1BE + i * 16;
                uint32_t len = *(const uint32_t *)(e + 12);
                if (!e[4] || !len) continue;
                if (e[4] == 0xEE) d->gpt = 1;
                char t[48]; size_text(len, sz, sizeof sz); ksnprintf(t, sizeof t, "%s%s %s", any ? ", " : "", ptype_name(e[4]), sz);
                strlcat(d->parts, t, sizeof d->parts); any = 1;
            }
        if (d->gpt) strlcpy(d->parts, any ? "a GPT disk (Windows, Linux)" : "a GPT disk", sizeof d->parts);
        else if (!any) strlcpy(d->parts, "empty - no partitions", sizeof d->parts);
    }
}

/* ---- FAT32 written here -------------------------------------------------------------------------------------- */
struct fnode {                                  /* a file or folder of SETUP\FILES, as it goes on the new disk */
    char src[200];
    char name83[11];
    char lname[96];                             /* its long name, when it has one (vgabios-cirrus.bin): written as VFAT's entries */
    int nlfn;                                   /* the long name's entries (13 characters each), 0 none */
    uint8_t attr;
    uint32_t size, first, nclus;
    int parent, is_dir, nchild;
};
static struct fnode *nodes;
static int nnodes, cap_nodes;
static uint32_t clus_bytes;
/* an 8.3 name in a directory entry's form ("COMMAND COM"); 0, or -1 (not a short name) */
static int to83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    int bl = dot ? (int)(dot - name) : (int)strlen(name), el = dot ? (int)strlen(dot + 1) : 0;
    if (bl < 1 || bl > 8 || el > 3) return -1;
    for (int i = 0; i < bl; i++) out[i] = (char)toupper(name[i]);
    for (int i = 0; i < el; i++) out[8 + i] = (char)toupper(dot[1 + i]);
    return 0;
}
static int add_node(const char *src, const char *name, int parent, int is_dir, uint32_t size, uint8_t attr)
{
    if (nnodes == cap_nodes) {
        int nc = cap_nodes ? cap_nodes * 2 : 128;
        struct fnode *t = kmalloc(sizeof *t * (uint32_t)nc);
        if (!t) return -1;
        if (nodes) { memcpy(t, nodes, sizeof *t * (uint32_t)nnodes); kfree(nodes); }
        nodes = t; cap_nodes = nc;
    }
    struct fnode *f = &nodes[nnodes];
    memset(f, 0, sizeof *f);
    strlcpy(f->src, src, sizeof f->src);
    if (to83(name, f->name83) < 0) {
        /* a long name: its short one made as Windows 95 makes it (the first six letters, ~1, the extension's first three) -
         * made unique in its folder after the scan (short_names) */
        int ln = strlen(name);
        if (ln >= (int)sizeof f->lname) return -1;
        strlcpy(f->lname, name, sizeof f->lname);
        f->nlfn = (ln + 12) / 13;
        memset(f->name83, ' ', 11);
        const char *dot = strrchr(name, '.');
        int k = 0;
        for (const char *c = name; *c && c != dot && k < 6; c++) if (isalpha((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_' || *c == '-') f->name83[k++] = (char)toupper(*c);
        if (!k) f->name83[k++] = '_';
        f->name83[k] = '~'; f->name83[k + 1] = '1';
        if (dot) for (int i = 0; i < 3 && dot[1 + i]; i++) f->name83[8 + i] = (char)toupper(dot[1 + i]);
    }
    f->parent = parent; f->is_dir = is_dir; f->size = size; f->attr = attr;
    if (parent >= 0) nodes[parent].nchild++;
    return nnodes++;
}
static int scan_tree(const char *dir, int parent)
{
    char pat[220]; path_join(pat, sizeof pat, dir, "*.*");
    struct fs_dirent e; struct fs_find fh;
    if (fs_findfirst(pat, 0x37, &e, &fh) != 0) return 0;
    int rc = 0;
    do {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        char p[220]; if (path_join(p, sizeof p, dir, e.name) < 0) { rc = -1; break; }
        uint8_t attr = (e.attr & FA_DIR) ? 0x10 : 0x20;
        if (!stricmp(e.name, "IO.SYS") || !stricmp(e.name, "MSDOS.SYS")) attr = 0x27;        /* (read-only, hidden, system) */
        int k = add_node(p, e.name, parent, (e.attr & FA_DIR) != 0, (e.attr & FA_DIR) ? 0 : e.size, attr);
        if (k < 0) { rc = -1; break; }
        if (e.attr & FA_DIR) { if (scan_tree(p, k) < 0) { rc = -1; break; } }
    } while (fs_findnext(&fh, &e) == 0);
    fs_findclose(&fh);
    return rc;
}
/* each long-named node's short name unique in its folder (~1, ~2, ... ~9) */
static void short_names(void)
{
    for (int i = 0; i < nnodes; i++) {
        if (!nodes[i].nlfn) continue;
        int tilde = 0; while (tilde < 8 && nodes[i].name83[tilde] != '~') tilde++;
        for (int n = 1; n <= 9; n++) {
            nodes[i].name83[tilde + 1] = (char)('0' + n);
            int clash = 0;
            for (int j = 0; j < nnodes && !clash; j++) if (j != i && nodes[j].parent == nodes[i].parent && !memcmp(nodes[j].name83, nodes[i].name83, 11)) clash = 1;
            if (!clash) break;
        }
    }
}
static uint8_t sfn_sum(const char *s11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)s11[i]);
    return sum;
}
/* a long name's entries, last part first (VFAT): k entries at e */
static void lfn_entries(uint8_t *e, const struct fnode *f)
{
    int len = strlen(f->lname);
    uint8_t sum = sfn_sum(f->name83);
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = f->nlfn; k >= 1; k--) {
        uint8_t *x = e + 32 * (f->nlfn - k);
        memset(x, 0, 32);
        x[0] = (uint8_t)(k | (k == f->nlfn ? 0x40 : 0)); x[11] = 0x0F; x[12] = 0; x[13] = sum;
        for (int c = 0; c < 13; c++) {
            int at = (k - 1) * 13 + c;
            uint16_t ch = at < len ? (uint8_t)f->lname[at] : at == len ? 0x0000 : 0xFFFF;
            x[pos[c]] = (uint8_t)ch; x[pos[c] + 1] = (uint8_t)(ch >> 8);
        }
    }
}
static void dos_stamp(uint16_t *date, uint16_t *time)
{
    int y, mo, d, dow, h, mi, s;
    fs_get_datetime(&y, &mo, &d, &dow, &h, &mi, &s);
    if (y < 1980) y = 1980;
    *date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
    *time = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}
static void dirent(uint8_t *e, const char name[11], uint8_t attr, uint32_t clus, uint32_t size, uint16_t date, uint16_t time)
{
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attr;
    *(uint16_t*)(e + 14) = time; *(uint16_t*)(e + 16) = date; *(uint16_t*)(e + 18) = date;
    *(uint16_t*)(e + 20) = (uint16_t)(clus >> 16); *(uint16_t*)(e + 22) = time; *(uint16_t*)(e + 24) = date;
    *(uint16_t*)(e + 26) = (uint16_t)clus; *(uint32_t*)(e + 28) = size;
}

struct fmt {                                    /* the partition and its FAT32 */
    int unit;
    uint32_t start, sectors, spc, fatsz, clusters, serial;
    char label[11];
    uint64_t data;                              /* the first cluster's sector */
};
static uint64_t clus_lba(struct fmt *f, uint32_t c) { return f->data + (uint64_t)(c - 2) * f->spc; }
static int read_whole(const char *path, uint8_t *buf, int n)
{
    int h = fs_open(path, 0);
    if (h < 0) return -1;
    int got = fs_read(h, buf, n);
    fs_close(h);
    return got;
}
/* the whole install on a disk: 0, or a message (static) */
static const char *install_disk(struct sdisk *d, uint32_t part_mb, int spc_kb, const char *label, int full, int autostart)
{
    static uint8_t mbr[512], vbr[1536], sec[512];
    char p[200];
    path_join(p, sizeof p, setup_dir, "MBR.BIN");
    if (read_whole(p, mbr, 512) != 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) return "SETUP\\MBR.BIN is not on the setup disk.";
    path_join(p, sizeof p, setup_dir, "FAT32VBR.BIN");
    if (read_whole(p, vbr, 1536) != 1536 || memcmp(vbr + 3, "MSWIN4.1", 8) || vbr[1534] != 0x55) return "SETUP\\FAT32VBR.BIN (MS-DOS 7.1's boot record) is not on the setup disk.";

    /* the files first: what does not fit is said before anything is written */
    nnodes = 0;
    if (scan_tree(src_root, -1) < 0 || !nnodes) return "The files to install (SETUP\\FILES) could not be read.";
    short_names();

    struct fmt f; memset(&f, 0, sizeof f);
    f.unit = d->unit;
    f.start = 2048;
    uint64_t room = d->sectors - f.start - 64;                         /* (the disk's last 64 sectors left alone: a GPT's copy) */
    if (room > 0xFFFFFFFFull - f.start) room = 0xFFFFFFFFull - f.start;   /* (an MBR partition: 2 TB) */
    uint64_t want = part_mb ? (uint64_t)part_mb * 2048 : room;
    f.sectors = (uint32_t)(want < room ? want : room);
    uint64_t mb = f.sectors / 2048;
    uint32_t spc = spc_kb ? (uint32_t)spc_kb * 2 : mb < 8192 ? 8 : mb < 16384 ? 16 : mb < 32768 ? 32 : 64;
    for (;;) {                                                          /* FAT32: 65525 clusters at least */
        uint32_t t1 = f.sectors - 32, t2 = (256 * spc + 2) / 2;
        f.fatsz = (t1 + t2 - 1) / t2;
        f.clusters = (f.sectors - 32 - 2 * f.fatsz) / spc;
        if (f.clusters >= 65525 || spc == 1) break;
        spc /= 2;
    }
    if (f.clusters < 65525) return "The partition is too small for FAT32 (33 MB at least).";
    if (f.clusters > 0x0FFFFFF4) return "The partition is too large for its clusters: take larger clusters.";
    f.spc = spc; f.data = (uint64_t)f.start + 32 + 2 * (uint64_t)f.fatsz;
    f.serial = (krand() & 0xFFFF0000u) | (jiffies & 0xFFFF);
    memset(f.label, ' ', 11); for (int i = 0; i < 11 && label[i]; i++) f.label[i] = (char)toupper(label[i]);
    clus_bytes = spc * 512;

    /* clusters: the root's (2 on), then each folder's, then each file's - every one in a run */
    uint32_t next = 2;
    int root_entries = 2;                                               /* (the label, and room) */
    for (int i = 0; i < nnodes; i++) if (nodes[i].parent < 0) root_entries += 1 + nodes[i].nlfn;
    for (int i = 0; i < nnodes; i++) if (nodes[i].parent >= 0 && nodes[i].nlfn) nodes[nodes[i].parent].nchild += nodes[i].nlfn;   /* (a folder's entries) */
    uint32_t root_clus = (root_entries * 32 + clus_bytes - 1) / clus_bytes;
    next += root_clus;
    for (int i = 0; i < nnodes; i++) if (nodes[i].is_dir) { nodes[i].nclus = ((nodes[i].nchild + 2) * 32 + clus_bytes - 1) / clus_bytes; nodes[i].first = next; next += nodes[i].nclus; }
    for (int i = 0; i < nnodes; i++) if (!nodes[i].is_dir && nodes[i].size) { nodes[i].nclus = (nodes[i].size + clus_bytes - 1) / clus_bytes; nodes[i].first = next; next += nodes[i].nclus; }
    if (next - 2 > f.clusters) return "The files do not fit on the partition.";
    uint32_t used = next - 2;

    struct window *pg = progress_dialog(SETUP_TITLE, "Writing the partition table...");
    const char *err = NULL;
    if (d->gpt) ata_unit_erase_ok((uint8_t)d->unit);
    /* the disk's first megabyte (an old MBR, a GPT's header and table) and its last 64 sectors (a GPT's copy) */
    if (sd_zero(d->unit, 0, f.start, NULL, NULL, 0, 0) || sd_zero(d->unit, d->sectors - 64, 64, NULL, NULL, 0, 0)) { err = "The disk could not be written."; goto out; }
    progress_update(pg, "Formatting: the reserved sectors...", 2);
    if (sd_zero(d->unit, f.start, 32, NULL, NULL, 0, 0)) { err = "The disk could not be written."; goto out; }
    /* the FATs: zeros, both, then their first entries and the chains */
    {
        int r = sd_zero(d->unit, f.start + 32, 2 * (uint64_t)f.fatsz, pg, "Formatting: the FATs...", 3, full ? 10 : 40);
        if (r) { err = r == -2 ? "Cancelled - the disk is not usable: run the setup again." : "The disk could not be written."; goto out; }
    }
    if (full) {
        int r = sd_zero(d->unit, f.data, (uint64_t)f.clusters * f.spc, pg, "Formatting: every sector of the partition...", 10, 70);
        if (r) { err = r == -2 ? "Cancelled - the disk is not usable: run the setup again." : "The disk could not be written."; goto out; }
    }
    {
        uint32_t nfat = next + 1, fsecs = (nfat * 4 + 511) / 512;
        uint32_t *fat = kzalloc(fsecs * 512);
        if (!fat) { err = "Not enough memory."; goto out; }
        fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF;
        for (uint32_t c = 2; c < 2 + root_clus; c++) fat[c] = c + 1 < 2 + root_clus ? c + 1 : 0x0FFFFFFF;
        for (int i = 0; i < nnodes; i++) for (uint32_t k = 0; k < nodes[i].nclus; k++) fat[nodes[i].first + k] = k + 1 < nodes[i].nclus ? nodes[i].first + k + 1 : 0x0FFFFFFF;
        int bad = sd_io(d->unit, f.start + 32, (int)fsecs, fat, 1) || sd_io(d->unit, f.start + 32 + f.fatsz, (int)fsecs, fat, 1);
        kfree(fat);
        if (bad) { err = "The disk could not be written."; goto out; }
    }
    /* the folders: the root, then each */
    uint16_t date, time; dos_stamp(&date, &time);
    {
        uint32_t maxclus = root_clus;
        for (int i = 0; i < nnodes; i++) if (nodes[i].is_dir && nodes[i].nclus > maxclus) maxclus = nodes[i].nclus;
        uint8_t *db = kmalloc(maxclus * clus_bytes);
        if (!db) { err = "Not enough memory."; goto out; }
        for (int dirn = -1; dirn < nnodes; dirn++) {
            if (dirn >= 0 && !nodes[dirn].is_dir) continue;
            uint32_t nc = dirn < 0 ? root_clus : nodes[dirn].nclus, first = dirn < 0 ? 2 : nodes[dirn].first;
            memset(db, 0, nc * clus_bytes);
            int k = 0;
            if (dirn < 0) {
                /* IO.SYS and MSDOS.SYS first (MS-DOS 7.1's boot code finds IO.SYS in the root), then the label, then the rest */
                for (int pass = 0; pass < 2; pass++)
                    for (int i = 0; i < nnodes; i++) {
                        if (nodes[i].parent != -1) continue;
                        int sys = !memcmp(nodes[i].name83, "IO      SYS", 11) || !memcmp(nodes[i].name83, "MSDOS   SYS", 11);
                        if (sys != (pass == 0)) continue;
                        if (pass == 1 && k == 2) dirent(db + 32 * k++, f.label, 0x08, 0, 0, date, time);
                        if (nodes[i].nlfn) { lfn_entries(db + 32 * k, &nodes[i]); k += nodes[i].nlfn; }
                        dirent(db + 32 * k++, nodes[i].name83, nodes[i].attr, nodes[i].first, nodes[i].is_dir ? 0 : nodes[i].size, date, time);
                    }
                if (k <= 2) dirent(db + 32 * k++, f.label, 0x08, 0, 0, date, time);
            } else {
                dirent(db + 32 * k++, ".          ", 0x10, nodes[dirn].first, 0, date, time);
                dirent(db + 32 * k++, "..         ", 0x10, nodes[dirn].parent < 0 ? 0 : nodes[nodes[dirn].parent].first, 0, date, time);
                for (int i = 0; i < nnodes; i++) if (nodes[i].parent == dirn) {
                    if (nodes[i].nlfn) { lfn_entries(db + 32 * k, &nodes[i]); k += nodes[i].nlfn; }
                    dirent(db + 32 * k++, nodes[i].name83, nodes[i].attr, nodes[i].first, nodes[i].is_dir ? 0 : nodes[i].size, date, time);
                }
            }
            if (sd_io(d->unit, clus_lba(&f, first), (int)(nc * f.spc), db, 1)) { kfree(db); err = "The disk could not be written."; goto out; }
        }
        kfree(db);
    }
    /* the files */
    {
        uint64_t total = 0, done = 0;
        for (int i = 0; i < nnodes; i++) if (!nodes[i].is_dir) total += nodes[i].size;
        uint8_t *buf = kmalloc(64 * 1024);
        if (!buf) { err = "Not enough memory."; goto out; }
        for (int i = 0; i < nnodes && !err; i++) {
            struct fnode *n = &nodes[i];
            if (n->is_dir || !n->size) continue;
            char t[160]; ksnprintf(t, sizeof t, "Copying %s", n->src + strlen(src_root));
            if (progress_update(pg, t, 70 + (int)(total ? done * 28 / total : 0))) { err = "Cancelled - the disk is not usable: run the setup again."; break; }
            int h = fs_open(n->src, 0);
            if (h < 0) { err = "A file of the setup disk could not be read."; break; }
            uint64_t lba = clus_lba(&f, n->first);
            uint32_t left = n->size;
            while (left) {
                uint32_t chunk = left > 65536 ? 65536 : left;
                int got = fs_read(h, buf, (int)chunk);
                if (got != (int)chunk) { err = "A file of the setup disk could not be read."; break; }
                uint32_t secs = (chunk + 511) / 512;
                if (chunk % 512) memset(buf + chunk, 0, secs * 512 - chunk);
                if (sd_io(d->unit, lba, (int)secs, buf, 1)) { err = "The disk could not be written."; break; }
                lba += secs; left -= chunk; done += chunk;
            }
            fs_close(h);
        }
        kfree(buf);
        if (err) goto out;
    }
    /* CONFIG.SYS and AUTOEXEC.BAT are SETUP\FILES' - the line that starts the QEMU DOS port in AUTOEXEC.BAT, if asked,
     * is its last: the setup writes it in the file's last cluster (the file was given room: setup\files\autoexec.bat
     * ends with a line of 64 blanks the setup turns into the start line or leaves blank) */
    for (int i = 0; i < nnodes; i++) {
        if (nodes[i].parent != -1 || memcmp(nodes[i].name83, "AUTOEXECBAT", 11) || !nodes[i].size) continue;
        uint32_t off = nodes[i].size >= 66 ? nodes[i].size - 66 : 0;
        uint64_t lba = clus_lba(&f, nodes[i].first) + off / 512;
        static uint8_t two[1024];
        int n = (off % 512) + 66 > 512 ? 2 : 1;
        if (sd_io(d->unit, lba, n, two, 0) == 0) {
            uint8_t *line = two + off % 512;
            const char *start = autostart ? "C:\\QEMUDOS\\QEMUDOS.COM" : "REM QEMUDOS.COM starts the QEMU DOS port";
            if (!memcmp(line, "REM QDP-START-LINE", 18)) {
                memset(line, ' ', 64);
                memcpy(line, start, strlen(start));
                sd_io(d->unit, lba, n, two, 1);
            }
        }
    }
    /* the boot record: MS-DOS 7.1's (sectors 0 and 2), its BPB this partition's; FSInfo; the copy at 6 */
    {
        uint8_t *v = vbr;
        *(uint16_t*)(v + 11) = 512; v[13] = (uint8_t)f.spc; *(uint16_t*)(v + 14) = 32; v[16] = 2; *(uint16_t*)(v + 17) = 0; *(uint16_t*)(v + 19) = 0;
        v[21] = 0xF8; *(uint16_t*)(v + 22) = 0; *(uint16_t*)(v + 24) = 63; *(uint16_t*)(v + 26) = 255; *(uint32_t*)(v + 28) = f.start; *(uint32_t*)(v + 32) = f.sectors;
        *(uint32_t*)(v + 36) = f.fatsz; *(uint16_t*)(v + 40) = 0; *(uint16_t*)(v + 42) = 0; *(uint32_t*)(v + 44) = 2; *(uint16_t*)(v + 48) = 1; *(uint16_t*)(v + 50) = 6;
        memset(v + 52, 0, 12); v[64] = 0x80; v[65] = 0; v[66] = 0x29; *(uint32_t*)(v + 67) = f.serial; memcpy(v + 71, f.label, 11); memcpy(v + 82, "FAT32   ", 8);
        uint8_t *fi = v + 512;
        memset(fi, 0, 512);
        *(uint32_t*)fi = 0x41615252; *(uint32_t*)(fi + 484) = 0x61417272;
        *(uint32_t*)(fi + 488) = f.clusters - used; *(uint32_t*)(fi + 492) = next; *(uint32_t*)(fi + 508) = 0xAA550000;
        if (sd_io(d->unit, f.start, 3, v, 1) || sd_io(d->unit, f.start + 6, 3, v, 1)) { err = "The disk could not be written."; goto out; }
    }
    /* last, the MBR: one partition, active, FAT32 (LBA); until it is written the disk starts nothing */
    progress_update(pg, "Writing the master boot record...", 99);
    {
        memset(mbr + 0x1BE, 0, 64);
        *(uint32_t*)(mbr + 0x1B8) = f.serial ^ 0x5A5A0000u; mbr[0x1BC] = mbr[0x1BD] = 0;
        uint8_t *e = mbr + 0x1BE;
        e[0] = 0x80; e[1] = 32; e[2] = 33; e[3] = 0;                   /* (2048 = cylinder 0, head 32, sector 33 at 255 x 63) */
        e[4] = 0x0C;
        uint32_t end = f.start + f.sectors - 1, c = end / (255 * 63), h = end / 63 % 255, s = end % 63 + 1;
        if (c > 1023) { c = 1023; h = 254; s = 63; }
        e[5] = (uint8_t)h; e[6] = (uint8_t)(((c >> 2) & 0xC0) | s); e[7] = (uint8_t)c;
        *(uint32_t*)(e + 8) = f.start; *(uint32_t*)(e + 12) = f.sectors;
        if (sd_io(d->unit, 0, 1, mbr, 1)) { err = "The disk could not be written."; goto out; }
        if (sd_io(d->unit, 0, 1, sec, 0) || memcmp(sec, mbr, 512)) { err = "What was written to the disk does not read back the same."; goto out; }
    }
    dcache_drop_unit((uint8_t)d->unit);
    kprintf("setup: unit %02x - partition %u+%u, FAT32 %u clusters of %u KB, FAT %u sectors, %u clusters used\n",
            d->unit, f.start, f.sectors, f.clusters, f.spc / 2, f.fatsz, used);
  out:
    progress_close(pg);
    return err;
}

/* ---- beside an MS-DOS that is there ----------------------------------------------------------------------------- */
static int copy_file(const char *from, const char *to)
{
    int h = fs_open(from, 0);
    if (h < 0) return -1;
    int o = fs_create(to);
    if (o < 0) { fs_close(h); return -1; }
    static uint8_t buf[32768];
    int r = 0, n;
    while ((n = fs_read(h, buf, sizeof buf)) > 0) if (fs_write(o, buf, n) != n) { r = -1; break; }
    if (n < 0) r = -1;
    fs_close(h); fs_close(o);
    return r;
}
static int copy_tree(const char *from, const char *to, struct window *pg)
{
    fs_mkdir(to);
    char pat[220]; path_join(pat, sizeof pat, from, "*.*");
    struct fs_dirent e; struct fs_find fh;
    if (fs_findfirst(pat, 0x37, &e, &fh) != 0) return 0;
    int rc = 0;
    do {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        char a[220], b[220];
        if (path_join(a, sizeof a, from, e.name) < 0 || path_join(b, sizeof b, to, e.name) < 0) { rc = -1; break; }
        if (e.attr & FA_DIR) { if (copy_tree(a, b, pg) < 0) { rc = -1; break; } continue; }
        char t[200]; ksnprintf(t, sizeof t, "Copying %s", b);
        progress_update(pg, t, 50);
        if (copy_file(a, b) < 0) { rc = -1; break; }
    } while (fs_findnext(&fh, &e) == 0);
    fs_findclose(&fh);
    return rc;
}
static const char *install_beside(int drive, const char *folder, int autostart)
{
    char dst[200], src[200];
    ksnprintf(dst, sizeof dst, "%c:%s%s", 'A' + drive, folder[0] == '\\' ? "" : "\\", folder);
    int n = strlen(dst); if (n > 3 && dst[n - 1] == '\\') dst[n - 1] = 0;
    path_join(src, sizeof src, src_root, "QEMUDOS");
    struct window *pg = progress_dialog(SETUP_TITLE, "Copying...");
    int r = copy_tree(src, dst, pg);
    progress_close(pg);
    if (r < 0) return "The files could not be copied (is the drive full?).";
    if (autostart) {
        /* the line at the end of that drive's AUTOEXEC.BAT - the drive holds the MS-DOS that starts (IO.SYS in its root: the
         * caller checked), and when that DOS starts the drive is its C:, whatever letter it has under the setup's own DOS -
         * not twice */
        char line[220], ae[16];
        ksnprintf(line, sizeof line, "C:%s\\QEMUDOS.COM", dst + 2);
        ksnprintf(ae, sizeof ae, "%c:\\AUTOEXEC.BAT", 'A' + drive);
        int len = 0; char *old = kread_text(ae, &len);
        if (!old || !strstr(old, line)) {
            char *nw = kaprintf("%s%s%s\r\n", old ? old : "@ECHO OFF\r\n", old && len && old[len - 1] != '\n' ? "\r\n" : "", line);
            if (!nw || fs_write_file(ae, nw, strlen(nw)) < 0) { kfree(nw); kfree(old); return "The files are copied, but AUTOEXEC.BAT could not be written."; }
            kfree(nw);
        }
        kfree(old);
    }
    return NULL;
}

/* ---- the pages ------------------------------------------------------------------------------------------------- */
static struct window *setup_win;
static int setup_back_proc(struct window *w, struct wevent *e)
{
    if (e->type == WE_PAINT) {
        gfx_fill(w->cx, w->cy, w->cw, w->ch, 0x0A1B3D);
        int sc = w->cw >= 1024 ? 4 : 3;
        const char *t = SETUP_TITLE;
        qdp_draw_big_text(w->cx + 30, w->cy + 24, sc, 0xFFFFFF, t);
        gfx_text(ui_font, w->cx + 32, w->cy + 30 + bmf_height(&font_ui_bold) * sc, 0xAFC4E4, "MS-DOS 7.1 and " QDP_ENGINE " virtual machines - the computer starts into the QEMU manager");
        return 1;
    }
    return 0;
}
enum { W_DISK = 10, W_BESIDE, W_DOS, W_RESTART, P_LIST = 20, P_SIZE, P_SIZELBL, P_CLUS, P_LABEL, P_FULL, P_AUTO, P_DRIVE, P_FOLDER };
static int page(struct window **dp, const char *title, int W, int H)
{
    struct window *d = wm_create(title, 0, 0, W, H, WF_DLG | WF_MODAL, dialog_std_proc, NULL);
    if (!d) return -1;
    d->owner = setup_win; wm_center(d, setup_win);
    *dp = d;
    return 0;
}
static int run_page(struct window *d) { wm_show(d, 1); wm_run_modal(d); return d->result; }
static void done_page(const char *what)
{
    char *msg = kaprintf("%s\n\nRestart the computer now?  (Take the setup CD or USB stick out first, or start from the disk in the "
                         "computer's boot menu.)", what);
    int r = msgbox(setup_win, SETUP_TITLE, msg ? msg : what, MB_YESNO | MB_ICONINFO);
    kfree(msg);
    if (r == ID_YES) qdp_restart(setup_win, 0);
}
static void disk_page(void)
{
    find_disks();
    if (!ndisks) { msgbox(setup_win, SETUP_TITLE, "There is no hard disk of 128 MB or more in this computer that the setup can see (besides its own).", MB_OK | MB_ICONWARN); return; }
    struct window *d; int W = 560, H = 430;
    if (page(&d, "Install on a disk", W, H) < 0) return;
    ctl_add(d, CT_LABEL, 0, 12, 12, W - 24, 32, "The disk is erased - everything on it is lost.  One MS-DOS 7.1 partition (FAT32) is made on it, MS-DOS 7.1 "
            "and the QEMU DOS port are put on it, and the computer starts from it.", 0);
    struct control *lv = ctl_add(d, CT_LISTBOX, P_LIST, 12, 52, W - 24, 120, "", 0);
    for (int i = 0; i < ndisks; i++) { char t[300]; ksnprintf(t, sizeof t, "%s  -  %s", disks[i].desc, disks[i].parts); ctl_lv_add(lv, t, ICO_DRIVE_HDD, NULL); }
    ctl_lv_select(lv, 0, 1);
    ctl_add(d, CT_LABEL, 0, 12, 188, 170, 16, "&Partition size (MB, 0 = all):", 0);
    struct control *se = ctl_add(d, CT_EDIT, P_SIZE, 190, 184, 100, 21, "0", CS_NUMBER); se->maxlen = 8;
    ctl_add(d, CT_LABEL, P_SIZELBL, 300, 188, W - 312, 16, "(all of the disk, to 2 TB: an MBR's most)", 0);
    ctl_add(d, CT_LABEL, 0, 12, 220, 170, 16, "&Cluster size:", 0);
    struct control *cl = ctl_add(d, CT_COMBO, P_CLUS, 190, 216, 160, 21, "", CS_DROPLIST);
    static const char *const cn[] = { "Automatic", "4 KB", "8 KB", "16 KB", "32 KB" };
    for (int i = 0; i < 5; i++) ctl_combo_add(cl, cn[i]);
    cl->sel = 0;
    ctl_add(d, CT_LABEL, 0, 12, 252, 170, 16, "&Volume label:", 0);
    struct control *le = ctl_add(d, CT_EDIT, P_LABEL, 190, 248, 160, 21, "QEMUDOS", 0); le->maxlen = 11;
    struct control *fu = ctl_add(d, CT_CHECK, P_FULL, 12, 282, W - 24, 16, "&Full format: zeros written to every sector (slow on a large disk; it checks the disk)", 0); fu->checked = 0;
    struct control *au = ctl_add(d, CT_CHECK, P_AUTO, 12, 306, W - 24, 16, "&Start the QEMU manager when the computer starts", 0); au->checked = 1;
    ctl_add(d, CT_LABEL, 0, 12, 330, W - 24, 32, "The disk gets the UEFI start files too (\\EFI\\BOOT): a PC with UEFI only starts it through DOS95's BIOS layer "
            "(Secure Boot off).", 0);
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 91, H - 24 - 34, 85, 23, "&Install", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 91, H - 24 - 34, 85, 23, "&Back", 0);
    d->focus = lv;
    for (;;) {
        if (run_page(d) != ID_OK) break;
        d->result = 0;
        int s = lv->sel; if (s < 0 || s >= ndisks) { msgbox(d, SETUP_TITLE, "Choose a disk.", MB_OK | MB_ICONINFO); continue; }
        struct sdisk *k = &disks[s];
        uint32_t mb = (uint32_t)atoi(se->text);
        static const int spck[] = { 0, 4, 8, 16, 32 };
        int spc_kb = cl->sel >= 0 && cl->sel < 5 ? spck[cl->sel] : 0;
        char *msg = kaprintf("Everything on %s is erased:\n%s\n\nInstall the QEMU DOS port on it?", k->desc, k->parts);
        int ok = msgbox(d, SETUP_TITLE, msg ? msg : "Everything on the disk is erased. Install?", MB_YESNO | MB_ICONWARN);
        kfree(msg);
        if (ok != ID_YES) continue;
        d->enabled = 0;
        const char *err = install_disk(k, mb, spc_kb, le->text[0] ? le->text : "QEMUDOS", fu->checked, au->checked);
        d->enabled = 1;
        if (err) { msgbox(d, SETUP_TITLE, err, MB_OK | MB_ICONERROR); continue; }
        wm_destroy(d); d = NULL;
        char t[200]; ksnprintf(t, sizeof t, "The QEMU DOS port is installed on %s.", k->desc);
        done_page(t);
        return;
    }
    if (d) wm_destroy(d);
}
static void beside_page(void)
{
    struct window *d; int W = 520, H = 330;
    if (page(&d, "Install beside MS-DOS", W, H) < 0) return;
    ctl_add(d, CT_LABEL, 0, 12, 12, W - 24, 48, "The QEMU DOS port is copied to a folder of a drive this computer's MS-DOS has.  Nothing is formatted.  "
            "It needs MS-DOS 7.1 (Windows 95/98's), HIMEM.SYS loaded and no EMM386.", 0);
    struct control *lb = ctl_add(d, CT_LISTBOX, P_DRIVE, 12, 62, W - 24, 110, "", 0);
    int drv[26], nd = 0;
    uint32_t mask = fs_drives();
    for (int k = 2; k < 26; k++) {
        if (!(mask & (1u << k)) || fs_drive_type(k) != 2) continue;
        char probe[40]; ksnprintf(probe, sizeof probe, "%c:\\SETUP\\FILES", 'A' + k);
        if (fs_getattr(probe) >= 0) continue;                              /* (the setup's own drive) */
        uint32_t tot = 0, fr = 0; fs_diskfree(k + 1, &tot, &fr);
        char t[120]; ksnprintf(t, sizeof t, "%c:   %u MB free of %u MB", 'A' + k, fr / 1024, tot / 1024);
        ctl_lv_add(lb, t, ICO_DRIVE_HDD, NULL); drv[nd++] = k;
    }
    if (!nd) { wm_destroy(d); msgbox(setup_win, SETUP_TITLE, "MS-DOS has no hard disk drive here besides the setup's own.\n\n(A disk with no FAT partition has no drive letter: install on it as a disk of its own.)", MB_OK | MB_ICONWARN); return; }
    ctl_lv_select(lb, 0, 1);
    ctl_add(d, CT_LABEL, 0, 12, 188, 80, 16, "&Folder:", 0);
    struct control *fe = ctl_add(d, CT_EDIT, P_FOLDER, 100, 184, 200, 21, "\\QEMUDOS", 0); fe->maxlen = 60;
    struct control *au = ctl_add(d, CT_CHECK, P_AUTO, 12, 218, W - 24, 16, "&Start it when that drive's MS-DOS starts (a line at the end of its AUTOEXEC.BAT)", 0); au->checked = 1;
    ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 2 * 91, H - 24 - 34, 85, 23, "&Install", CS_DEFAULT);
    ctl_add(d, CT_BUTTON, ID_CANCEL, W - 6 - 91, H - 24 - 34, 85, 23, "&Back", 0);
    d->focus = lb;
    for (;;) {
        if (run_page(d) != ID_OK) break;
        d->result = 0;
        int s = lb->sel; if (s < 0 || s >= nd) continue;
        char io[16]; ksnprintf(io, sizeof io, "%c:\\IO.SYS", 'A' + drv[s]);
        int sysdrive = fs_getattr(io) >= 0;
        if (au->checked && !sysdrive && msgbox(d, SETUP_TITLE, "That drive holds no MS-DOS that starts (no IO.SYS in its root): the QEMU DOS port is copied, "
                                                 "and the line that would start it is not written - add it to your AUTOEXEC.BAT.\n\nCopy it?", MB_YESNO | MB_ICONQUESTION) != ID_YES) continue;
        const char *err = install_beside(drv[s], fe->text, au->checked && sysdrive);
        if (err) { msgbox(d, SETUP_TITLE, err, MB_OK | MB_ICONERROR); continue; }
        char t[300]; ksnprintf(t, sizeof t, "The QEMU DOS port is in %c:%s%s%s", 'A' + drv[s], fe->text[0] == '\\' ? "" : "\\", fe->text,
                                !sysdrive ? " - type QEMUDOS in its folder at a DOS prompt to start it."
                                : au->checked ? " - the MS-DOS on that drive starts it by itself (its AUTOEXEC.BAT)." : " - type QEMUDOS in its folder to start it.");
        wm_destroy(d);
        done_page(t);
        return;
    }
    wm_destroy(d);
}
void qdp_setup_start(void)
{
    /* the files to install: SETUP\FILES of the drive QEMUDOS.COM was started from */
    ksnprintf(setup_dir, sizeof setup_dir, "%c:\\SETUP", cfg.dos95dir[0] ? cfg.dos95dir[0] : 'C');
    path_join(src_root, sizeof src_root, setup_dir, "FILES");
    setup_win = wm_create(SETUP_TITLE, 0, 0, scr_w, scr_h, WF_NOFRAME, setup_back_proc, NULL);
    if (setup_win) wm_show(setup_win, 1);
    if (fs_getattr(src_root) < 0) {
        msgbox(setup_win, SETUP_TITLE, "The files to install (SETUP\\FILES) are not on the drive the setup started from.", MB_OK | MB_ICONERROR);
    }
    for (;;) {
        struct window *d; int W = 540, H = 300;
        if (page(&d, SETUP_TITLE, W, H) < 0) return;
        ctl_add(d, CT_LABEL, 0, 14, 14, W - 28, 32, "Welcome.  The QEMU DOS port is a PC that runs PCs: MS-DOS 7.1 starts it, and QEMU 0.14's virtual machines get "
                "the whole screen.  Where is it to go?", 0);
        struct control *r1 = ctl_add(d, CT_RADIO, W_DISK, 20, 60, W - 40, 16, "On a &disk of its own - the disk is erased (MS-DOS 7.1 and the QEMU DOS port)", 0);
        struct control *r2 = ctl_add(d, CT_RADIO, W_BESIDE, 20, 86, W - 40, 16, "&Beside an MS-DOS this computer has - nothing is erased", 0);
        struct control *r3 = ctl_add(d, CT_RADIO, W_DOS, 20, 112, W - 40, 16, "&Exit to MS-DOS (the setup disk's)", 0);
        struct control *r4 = ctl_add(d, CT_RADIO, W_RESTART, 20, 138, W - 40, 16, "&Restart the computer", 0);
        r1->group = r2->group = r3->group = r4->group = 1; r1->checked = 1;
        ctl_add(d, CT_LABEL, 0, 14, 172, W - 28, 48, "A disk of its own holds as many machines as it has room for: FAT32 to 2 TB, each machine's disks growing as they "
                "are written.  The setup CD's MS-DOS 7.1 is the one installed.", 0);
        ctl_add(d, CT_BUTTON, ID_OK, W - 6 - 91, H - 24 - 34, 85, 23, "&Next", CS_DEFAULT);
        d->focus = r1;
        run_page(d);
        int which = r1->checked ? 1 : r2->checked ? 2 : r3->checked ? 3 : 4;
        if (d->result != ID_OK) which = 0;
        wm_destroy(d);
        switch (which) {
        case 1: disk_page(); break;
        case 2: beside_page(); break;
        case 3: qdp_exit_to_dos(setup_win, 1); break;
        case 4: qdp_restart(setup_win, 1); break;
        }
    }
}
