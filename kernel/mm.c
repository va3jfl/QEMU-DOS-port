/* DOS95 kernel - physical page allocator, kernel heap, page tables */
#include "kernel.h"

extern uint32_t boot_pd[];              /* page directory (linked address) */
extern uint32_t boot_physbase;
extern char _end[];

PERCPU uint32_t *kernel_pd;           /* v44: each processor's own directory (smp.c: a copy of boot_pd) */
PERCPU uint32_t  kernel_pd_phys;
/* v44 (item 3a, M4): an entry every processor's directory has the same - the kernel's half, the pool's identity, the big
 * memory's tables (vmem.c) - written in each (kernel.h) */
void pd_set_all(int i, uint32_t v)
{
    extern uint32_t boot_pd[];
    boot_pd[i] = v;
    for (int k = 1; k < ncpus; k++) if (cpuinfo[k].pd) cpuinfo[k].pd[i] = v;
}

static uint32_t pool_start, pool_end;   /* physical */
#ifdef QDP
/* the QEMU DOS port runs no program: no address space of a program's own, no shared libraries' region at 2 GB
 * (aspace.c turns it off when the pool reaches past it) - the pool is all the memory below the kernel's 3 GB: a machine
 * of QEMU 0.14's most (2047 MB) has its memory on a PC of 4 GB */
#define POOL_TOP KERNEL_VBASE
#else
#define POOL_TOP SHR_BASE
#endif
static uint32_t *bitmap;                /* 1 bit per page, 1 = used */
static uint32_t npages, nfree, search_hint;
static uint32_t npages_ram;             /* v42 (#491): the pool's pages that are memory (npages: its span, gaps and all) */
static uint16_t *page_mark;             /* v38: per page, the program that asked for it (mm_reclaim) */
/* v40 (#186): what each program has of it - pages and heap bytes by mark: its memory's size.
 * v41 (#336): a mark is a program's place in kmod.c's table plus one, as many as there are
 * programs (it was 1..32 in a byte, the tables 64 long); these grow with that table
 * (mm_marks_ensure).  A mark is at most MARK_MAX: 14 bits in a heap block's magic - more
 * programs at once than 4 GB can hold */
static uint32_t *mark_pages, *mark_bytes;
static int nmarks;
#define MARK_MAX 0x3FFF
int mm_marks_ensure(int n)                  /* marks below n: 0, or -1 (no memory, or past MARK_MAX) */
{
    if (n <= nmarks) return 0;
    if (n > MARK_MAX + 1) return -1;
    int nc = nmarks ? nmarks : 64;
    while (nc < n) nc *= 2;
    if (nc > MARK_MAX + 1) nc = MARK_MAX + 1;
    uint32_t *a = kzalloc(sizeof(uint32_t) * 2 * (uint32_t)nc);
    if (!a) return -1;
    uint32_t f = save_flags_cli();
    uint32_t *old = mark_pages;
    if (old) { memcpy(a, mark_pages, sizeof(uint32_t) * (uint32_t)nmarks); memcpy(a + nc, mark_bytes, sizeof(uint32_t) * (uint32_t)nmarks); }
    mark_pages = a; mark_bytes = a + nc; nmarks = nc;
    restore_flags(f);
    kfree(old);
    return 0;
}
extern int cpu_level;

static inline void bit_set(uint32_t i) { bitmap[i >> 5] |= 1u << (i & 31); }
static inline void bit_clr(uint32_t i) { bitmap[i >> 5] &= ~(1u << (i & 31)); }
static inline int  bit_get(uint32_t i) { return (bitmap[i >> 5] >> (i & 31)) & 1; }

/* v41 (#391): shared libraries no program has are kept until their memory is wanted (kmod.c,
 * "shared libraries").  Their pages are free in every count (mm_kept_pages: what a program is
 * told, what the kernel leaves itself, the caches' sizes), and when the pool cannot give what
 * is asked they are let go, the one longest unused first, and the pool is asked again
 * (kmod_units_trim: pages given back, 0 when nothing is kept) - Windows 95's standby memory */
uint32_t mm_kept_pages;
uint32_t __attribute__((weak)) kmod_units_trim(uint32_t want) { (void)want; return 0; }
/* v44 (item 3a, M6): the fewest free pages there have been (Task Manager's peak of memory in use: XP's Commit Charge
 * Peak) - looked at where pages are taken */
uint32_t mm_free_min_pages = 0xFFFFFFFFu;
static inline void free_low(void) { if (nfree + mm_kept_pages < mm_free_min_pages) mm_free_min_pages = nfree + mm_kept_pages; }
uint32_t mm_free_pages(void) { return nfree + mm_kept_pages; }
uint32_t mm_pool_end(void) { return pool_end; }
uint32_t mm_pool_start(void) { return pool_start; }
uint32_t mm_total_pages(void) { return npages_ram ? npages_ram : npages; }

uint32_t page_alloc(int n)
{
  again:;
    uint32_t f = save_flags_cli();
    uint32_t i = search_hint, run = 0, start = 0;
    uint32_t tried = 0;
    while (tried < npages) {
        if (i >= npages) { i = 0; run = 0; }
        if (!bit_get(i)) {
            if (run == 0) start = i;
            run++;
            if (run == (uint32_t)n) {
                for (uint32_t k = start; k <= i; k++) bit_set(k);
                nfree -= n; free_low();
                search_hint = (n == 1) ? i + 1 : search_hint;
                restore_flags(f);
                uint32_t phys = pool_start + start * 4096;
                memset((void*)phys, 0, n * 4096);
                return phys;
            }
        } else run = 0;
        i++; tried++;
    }
    restore_flags(f);
    if (n > 0 && kmod_units_trim((uint32_t)n)) goto again;   /* v41 (#391): kept libraries let go: once more */
    kprintf("page_alloc(%d) FAILED, free=%u\n", n, nfree);
    return 0;
}

/* v41 (#336, K38): n pages from a multiple of `align` bytes, all below physical `limit` - an ISA
 * DMA buffer (below 16 MB, not across a 64 or 128 KB boundary): the pool searched from its start
 * for the first such free run (the sound driver tried page_alloc eight times, hoping one landed
 * low, and gave up after the eighth); 0 when none is free */
uint32_t page_alloc_below(int n, uint32_t limit, uint32_t align)
{
    if (n <= 0 || align < 4096 || (align & 4095)) return 0;
  again:;
    uint32_t f = save_flags_cli();
    uint32_t phys = (pool_start + align - 1) / align * align;
    for (; phys >= pool_start && phys + (uint32_t)n * 4096 <= limit && phys + (uint32_t)n * 4096 <= pool_end; phys += align) {
        uint32_t i = (phys - pool_start) / 4096, k = 0;
        while (k < (uint32_t)n && !bit_get(i + k)) k++;
        if (k < (uint32_t)n) continue;
        for (k = 0; k < (uint32_t)n; k++) bit_set(i + k);
        nfree -= n; free_low();
        restore_flags(f);
        memset((void*)phys, 0, (uint32_t)n * 4096);
        return phys;
    }
    restore_flags(f);
    if (kmod_units_trim((uint32_t)n)) goto again;   /* v41 (#391): a kept library may lie there */
    return 0;
}

void page_free(uint32_t phys, int n)
{
    if (phys < pool_start || phys >= pool_end) return;
    uint32_t f = save_flags_cli();
    uint32_t i = (phys - pool_start) / 4096;
    for (int k = 0; k < n; k++) { if (bit_get(i + k)) { bit_clr(i + k); nfree++; } }
    if (page_mark) for (int k = 0; k < n && i + (uint32_t)k < npages; k++) {
        uint16_t mk = page_mark[i + k];
        if (mk && mk < nmarks && mark_pages[mk]) mark_pages[mk]--;
        page_mark[i + k] = 0;
    }
    if (i < search_hint) search_hint = i;
    restore_flags(f);
}

/* ---- v38: memory a Windows program (Wine) sees -----------------------------------------
 * A program with its own address space (kernel/aspace.c) has, from 4 MB up to
 * as_hole_end, pages of its own where every other program sees the pool at
 * linear == physical.  So what such a program reaches through that identity mapping -
 * its image, stacks, libraries, the user heap (windows, controls), shared memory - must
 * lie at or above as_hole_end; kernel memory and other programs may use the pool below.
 * page_alloc_high() hands out only those pages; page_alloc_high_pref() prefers them. */
uint32_t as_hole_end;                    /* 0: no private address spaces on this machine */
static uint32_t hi_hint, lo_hint;
uint32_t page_alloc_high(int n)
{
    if (!as_hole_end || as_hole_end <= pool_start) return page_alloc(n);
  again:;
    uint32_t f = save_flags_cli();
    uint32_t lo = (as_hole_end - pool_start) / 4096;
    uint32_t i = hi_hint > lo ? hi_hint : lo, run = 0, start = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (; i < npages; i++) {
            if (!bit_get(i)) {
                if (run == 0) start = i;
                if (++run == (uint32_t)n) {
                    for (uint32_t k = start; k <= i; k++) bit_set(k);
                    nfree -= n; free_low();
                    if (n == 1) hi_hint = i + 1;
                    restore_flags(f);
                    uint32_t phys = pool_start + start * 4096;
                    memset((void*)phys, 0, n * 4096);
                    return phys;
                }
            } else run = 0;
        }
        i = lo; run = 0;
    }
    restore_flags(f);
    if (n > 0 && kmod_units_trim((uint32_t)n)) goto again;   /* v41 (#391): kept libraries let go: once more */
    f = save_flags_cli();
    {   /* what there was: free pages above the hole, and the longest run of them */
        uint32_t fr = 0, best = 0, cur = 0;
        for (uint32_t k = lo; k < npages; k++) if (!bit_get(k)) { fr++; if (++cur > best) best = cur; } else cur = 0;
        restore_flags(f);
        kprintf("page_alloc_high(%d) FAILED: %u pages free above %u MB, %u in one run (%u free in all)\n",
                n, fr, as_hole_end >> 20, best, nfree);
    }
    return 0;
}
/* v38: a page only a program with its own address space uses - its private frames, its
 * page tables (kernel/aspace.c) - from below as_hole_end first, where nothing that every
 * program must see can be put anyway, leaving the memory above it for what they share
 * (every Wine process's DLLs, stacks and heap: with private pages taken anywhere, the
 * shared memory ran out with a third Windows program) */
uint32_t page_alloc_low(int n)
{
    if (!as_hole_end || as_hole_end <= pool_start) return page_alloc(n);
    uint32_t f = save_flags_cli();
    uint32_t lo = (as_hole_end - pool_start) / 4096, run = 0, start = 0;
    if (lo > npages) lo = npages;
    for (uint32_t i = lo_hint < lo ? lo_hint : 0, pass = 0; pass < 2; pass++, i = 0) {
        for (; i < lo; i++) {
            if (!bit_get(i)) {
                if (run == 0) start = i;
                if (++run == (uint32_t)n) {
                    for (uint32_t k = start; k <= i; k++) bit_set(k);
                    nfree -= n; free_low();
                    lo_hint = i + 1;
                    restore_flags(f);
                    uint32_t phys = pool_start + start * 4096;
                    memset((void*)phys, 0, n * 4096);
                    return phys;
                }
            } else run = 0;
        }
        run = 0;
    }
    restore_flags(f);
    return page_alloc(n);                   /* none left below: anywhere */
}
uint32_t page_alloc_high_pref(int n)
{
    if (!as_hole_end) return page_alloc(n);
    uint32_t p = page_alloc_high(n);
    if (!p) { static int said; if (!said++) kprintf("mm: memory above %u MB used up - a Windows program may not see what is allocated now\n", as_hole_end >> 20); p = page_alloc(n); }
    return p;
}
/* a change to a kernel page directory entry reaches the private directories too */
void __attribute__((weak)) aspace_pde_changed(int pde) { (void)pde; }

/* map one page in a page table (pt = linear address of page table) */
void map_page(uint32_t *pt, uint32_t lin, uint32_t phys, uint32_t flags)
{
    pt[(lin >> 12) & 1023] = (phys & 0xFFFFF000) | flags;
}

/* map a physical range at a linear address in the kernel page directory,
 * creating page tables from the pool as needed (identity for pool pages) */
void kmap_range(uint32_t lin, uint32_t phys, uint32_t size, uint32_t flags)
{
    uint32_t end = lin + size;
    for (uint32_t l = lin & 0xFFFFF000; l < end; l += 4096, phys += 4096) {
        uint32_t pde = l >> 22;
        if (!(kernel_pd[pde] & PTE_P)) {
            uint32_t ptp = page_alloc(1);
            if (!ptp) kpanic("out of memory mapping %x", l);
            pd_set_all((int)pde, ptp | PTE_P | PTE_W);
            aspace_pde_changed((int)pde);
            /* v41: a VCPI client's directory is a copy of the kernel's - a table of the kernel's
             * half made since (a device mapped later: kmap_mmio) reaches it too */
            if (pde >= (KERNEL_VBASE >> 22))
                for (int i = 0; i < nvms; i++) if (vms[i] && vms[i]->pm_pd) vms[i]->pm_pd[pde] = kernel_pd[pde];
        }
        uint32_t *pt = (uint32_t*)(kernel_pd[pde] & 0xFFFFF000);   /* identity mapped */
        pt[(l >> 12) & 1023] = (phys & 0xFFFFF000) | flags;
    }
    tlb_flush_all();
}

/* ------------------------------------------------------------------ */
/* heap                                                                 */
struct hdr { uint32_t size; uint32_t free; struct hdr *next; uint32_t magic; };
#define HMAGIC 0x48454150
/* v38: a block a program asked for (kmod.c's gate) has HTAG | its mark for a magic instead
 * of HMAGIC (HTAG | 0x4150), so it can be found when the program ends (v41, #336: 14 bits of
 * mark, 1..MARK_MAX - it was 8) */
#define HTAG   0x48450000u
#define HEAPBLK(m) (((m) & 0xFFFF0000u) == HTAG)
static struct hdr *heap_head;

/* v38: every arena, so one that is wholly free again can go back to the pool (heap_trim).
 * v41 (#336): the table grows, in pages of its own (it was 512 long: an arena past its end
 * never went back); only no page for a bigger one leaves an arena out of it.  It is kept in
 * order of address, so an arena's start is found by halving (arena_start). */
struct arena { uint32_t base, pages; };
struct arenas { struct arena *a; int n, cap, lost; };
static struct arenas heap_ar, uheap_ar;
static void arena_note(struct arenas *t, uint32_t base, int pages)
{
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 512;
        struct arena *a = (struct arena*)page_alloc((int)((nc * sizeof(struct arena) + 4095) / 4096));
        if (!a) { t->lost++; return; }
        if (t->a) {
            memcpy(a, t->a, (size_t)t->n * sizeof(struct arena));
            page_free((uint32_t)t->a, (int)((t->cap * sizeof(struct arena) + 4095) / 4096));
        }
        t->a = a; t->cap = nc;
    }
    int i = t->n;
    while (i > 0 && t->a[i - 1].base > base) { t->a[i] = t->a[i - 1]; i--; }
    t->a[i].base = base; t->a[i].pages = (uint32_t)pages; t->n++;
}
static int arena_find(const struct arenas *t, uint32_t base)
{
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t->a[mid].base == base) return mid;
        if (t->a[mid].base < base) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
/* v41 (#333): a block that starts an arena is not joined to the block before it - two arenas
 * one after the other in memory stay two, each able to go back */
static int arena_start(const void *p) { return arena_find(&heap_ar, (uint32_t)p) >= 0 || arena_find(&uheap_ar, (uint32_t)p) >= 0; }
/* ---- v41 (#412): a heap's free blocks, by size ---------------------------------------------
 * kmalloc looked at every block of the heap, taken or free, from the first to the first free one
 * big enough: a program with a hundred thousand blocks had every call read the whole list
 * (FileZilla's start: 5.9 of its 24 s at 125 million instructions a second in kmalloc_user).
 * The free blocks are on lists by size as well now - a list a size for the small ones (16 to
 * 496 bytes, a step of 16), a list a power of two above - linked through their own first eight
 * bytes (a free block has sixteen at least).  The list of all blocks by address is what it was:
 * joining neighbours, giving an arena back and finding a program's blocks at its end go by it,
 * and the lists by size are made again from it should they be found wrong - the user heap's
 * blocks are a program's to write, so a link there is looked at before it is followed. */
#define NBINS 56
struct flink { struct hdr *fnext, *fprev; };
#define FL(h) ((struct flink*)((h) + 1))
struct heap { struct hdr **head; struct arenas *ar; int user; struct hdr *bin[NBINS]; };
static struct hdr *uheap_head;
static struct heap kheap = { &heap_head, &heap_ar, 0, { 0 } }, uheap = { &uheap_head, &uheap_ar, 1, { 0 } };
static int bin_of(uint32_t size)
{
    if (size < 512) return (int)(size >> 4) - 1;
    int b = 31;
    for (uint32_t x = size >> 10; x && b < NBINS - 1; x >>= 1) b++;
    return b;
}
/* the arena of the heap's that holds address a: its index, or -1 (the one found last looked at first) */
static int arena_has(const struct arenas *t, uint32_t a)
{
    static int hint;
    if (hint < t->n && a >= t->a[hint].base && a < t->a[hint].base + t->a[hint].pages * 4096) return hint;
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < t->a[mid].base) hi = mid - 1;
        else if (a >= t->a[mid].base + t->a[mid].pages * 4096) lo = mid + 1;
        else { hint = mid; return mid; }
    }
    return -1;
}
/* is h a free block of the heap's: inside one of its arenas, where a header can be, a free block's header */
static int fl_ok(const struct heap *hp, const struct hdr *h)
{
    uint32_t a = (uint32_t)h;
    if (!a || (a & 15)) return 0;
    int i = arena_has(hp->ar, a);
    if (i < 0) return 0;
    uint32_t end = hp->ar->a[i].base + hp->ar->a[i].pages * 4096;
    return a + sizeof(struct hdr) + 16 <= end && h->magic == HMAGIC && h->free == 1 && h->size >= 16 && h->size <= end - a - sizeof(struct hdr);
}
static void bin_put(struct heap *hp, struct hdr *h)
{
    if (h->size < 16 || (hp->ar->lost && arena_has(hp->ar, (uint32_t)h) < 0)) return;   /* (one in an arena that is on no table: found by the list) */
    int b = bin_of(h->size);
    FL(h)->fprev = NULL; FL(h)->fnext = hp->bin[b];
    if (hp->bin[b]) FL(hp->bin[b])->fprev = h;
    hp->bin[b] = h;
}
static void bins_rebuild(struct heap *hp, const struct hdr *but)
{
    static int said;
    if (said < 4) { said++; kprintf("heap: the %s heap's lists of free blocks by size were not right - made again from the heap itself\n", hp->user ? "user" : "kernel"); }
    for (int i = 0; i < NBINS; i++) hp->bin[i] = NULL;
    for (struct hdr *h = *hp->head; h; h = h->next) if (h != but && h->free == 1 && h->magic == HMAGIC) bin_put(hp, h);
}
/* a free block off its list */
static void bin_take(struct heap *hp, struct hdr *h)
{
    if (h->size < 16 || (hp->ar->lost && arena_has(hp->ar, (uint32_t)h) < 0)) return;
    int b = bin_of(h->size);
    struct hdr *p = FL(h)->fprev, *n = FL(h)->fnext;
    int ok = p ? (!hp->user || fl_ok(hp, p)) && FL(p)->fnext == h : hp->bin[b] == h;
    if (ok && n) ok = (!hp->user || fl_ok(hp, n)) && FL(n)->fprev == h;
    if (!ok) { bins_rebuild(hp, h); return; }
    if (p) FL(p)->fnext = n; else hp->bin[b] = n;
    if (n) FL(n)->fprev = p;
}
/* (interrupts off) n bytes - a multiple of 16 - from the heap's free blocks: the block, or NULL */
static void *heap_take(struct heap *hp, struct hdr *h, uint32_t n)
{
    if (h->size >= n + sizeof(struct hdr) + 32) {
        struct hdr *s = (struct hdr*)((uint8_t*)(h + 1) + n);
        s->size = h->size - n - sizeof(struct hdr);
        s->free = 1; s->magic = HMAGIC; s->next = h->next;
        h->next = s; h->size = n;
        bin_put(hp, s);
    }
    h->free = 0;
    return h + 1;
}
static void *heap_alloc(struct heap *hp, uint32_t n)
{
    for (int again = 0; again < 2; again++) {
        int bad = 0;
        for (int b = bin_of(n); b < NBINS && !bad; b++) {
            uint32_t steps = 0;
            for (struct hdr *h = hp->bin[b]; h; h = FL(h)->fnext) {
                if ((hp->user && !fl_ok(hp, h)) || ++steps > 4000000u) { bad = 1; break; }
                if (h->size < n) continue;
                bin_take(hp, h);
                return heap_take(hp, h, n);
            }
        }
        if (!bad) break;
        bins_rebuild(hp, NULL);
    }
    /* (an arena on no table - no page was to be had for a bigger table - has its free blocks on the list only) */
    if (hp->ar->lost)
        for (struct hdr *h = *hp->head; h; h = h->next)
            if (h->free == 1 && h->size >= n && arena_has(hp->ar, (uint32_t)h) < 0) return heap_take(hp, h, n);
    return NULL;
}
/* v41 (#333): free blocks next to each other made one (kfree joins a block only to the one
 * after it) - before the heap asks the pool for another arena, and when it is trimmed */
static void heap_join(struct heap *hp)
{
    for (struct hdr *h = *hp->head; h; h = h->next)
        if (h->free == 1 && h->next && h->next->free == 1 && (uint8_t*)(h + 1) + h->size == (uint8_t*)h->next && !arena_start(h->next)) {
            bin_take(hp, h);
            do {
                bin_take(hp, h->next);
                h->size += sizeof(struct hdr) + h->next->size;
                h->next = h->next->next;
            } while (h->next && h->next->free == 1 && (uint8_t*)(h + 1) + h->size == (uint8_t*)h->next && !arena_start(h->next));
            bin_put(hp, h);
        }
}

uint32_t heap_arena_kb, uheap_arena_kb;         /* v38: what the two heaps hold of the pool (the log) */
/* v40 (#169): no room for an arena is an answer - NULL from kmalloc - not a panic: a
 * program whose load asked for more than there is fails to load, the system goes on
 * (D95GL.DLL's 3 MB name table after its 59 MB file and 42 MB image took DOS95 down) */
static int heap_add_arena(uint32_t bytes)
{
    int pages = (bytes + 4095) / 4096;
    uint32_t p = page_alloc(pages);
    if (!p) { kprintf("heap: no room for %u KB more (%u KB free)\n", pages * 4, mm_free_pages() * 4); return -1; }
    heap_arena_kb += pages * 4;
    arena_note(&heap_ar, p, pages);
    struct hdr *h = (struct hdr*)p;
    h->size = pages * 4096 - sizeof(struct hdr);
    h->free = 1; h->magic = HMAGIC;
    h->next = heap_head;
    heap_head = h;
    bin_put(&kheap, h);
    return 0;
}

void *kmalloc(size_t n)
{
    n = (n + 15) & ~15u;
    if (!n) n = 16;                                     /* (v41, #412: no free block of no bytes) */
    uint32_t f = save_flags_cli();
    for (int pass = 0; pass < 3; pass++) {
        void *p = heap_alloc(&kheap, n);
        if (p) { restore_flags(f); return p; }
        if (pass == 0) { heap_join(&kheap); continue; }          /* v41 (#333): then look again */
        if (pass == 2) break;
        if (heap_add_arena(n > 256 * 1024 ? n + 4096 : 512 * 1024) < 0 &&
            (n > 256 * 1024 || heap_add_arena(n + 4096 + sizeof(struct hdr)) < 0)) break;   /* the least that would do */
    }
    restore_flags(f);
    kprintf("kmalloc(%u): out of memory\n", n);
    return NULL;
}

void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }

/* ---- the user heap -----------------------------------------------------------
 * A second heap whose pages are mapped read/write for ring-3 native programs.
 * Windows, controls and menus are allocated here, because the toolkit's API lets a
 * program write their fields directly -- w->icon = ..., c->maxlen = ..., and the
 * dozens of other places every app does it -- so a program running at ring 3 has to
 * be able to reach them.  Everything else the kernel owns (the scheduler, the page
 * tables, the VM state, the general heap) stays out of its reach: read-only at
 * best.  A bad program can therefore wreck its own windows, which crashes the GUI
 * and is contained, but it cannot corrupt the kernel.
 * The block header is the same, so kfree() frees a block from either heap. */

static int uheap_add_arena(uint32_t bytes)
{
    int pages = (bytes + 4095) / 4096;
    uint32_t p = page_alloc_high_pref(pages);       /* windows and controls: a Windows program's too */
    if (!p) { kprintf("user heap: no room for %u KB more (%u KB free)\n", pages * 4, mm_free_pages() * 4); return -1; }   /* v40 (#169) */
    uheap_arena_kb += pages * 4;
    arena_note(&uheap_ar, p, pages);
    mm_user_range(p, pages);
    struct hdr *h = (struct hdr*)p;
    h->size = pages * 4096 - sizeof(struct hdr);
    h->free = 1; h->magic = HMAGIC;
    h->next = uheap_head;
    uheap_head = h;
    bin_put(&uheap, h);
    return 0;
}

void *kmalloc_user(size_t n)
{
    n = (n + 15) & ~15u;
    if (!n) n = 16;
    uint32_t f = save_flags_cli();
    for (int pass = 0; pass < 3; pass++) {
        void *p = heap_alloc(&uheap, n);
        if (p) { restore_flags(f); return p; }
        if (pass == 0) { heap_join(&uheap); continue; }          /* v41 (#333): then look again */
        if (pass == 2) break;
        if (uheap_add_arena(n > 64 * 1024 ? n + 4096 : 128 * 1024) < 0 &&
            (n > 64 * 1024 || uheap_add_arena(n + 4096 + sizeof(struct hdr)) < 0)) break;
    }
    restore_flags(f);
    kprintf("kmalloc_user(%u): out of memory\n", n);
    return NULL;
}
void *kzalloc_user(size_t n) { void *p = kmalloc_user(n); if (p) memset(p, 0, n); return p; }
char *kstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = kmalloc(n); if (p) memcpy(p, s, n); return p; }

/* Make a linear range user-accessible in the kernel page directory, whoever
 * mapped it (boot paging or mm_init).  DPMI hands ring-3 clients memory from
 * the pool at linear==physical; those pages must carry PTE_U. */
void mm_user_range(uint32_t lin, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t l = lin + i * 4096;
        uint32_t pde = kernel_pd[l >> 22];
        if (!(pde & PTE_P) || vm_slot(l >> 22)) continue;   /* v40: the region's pages are user pages when they come */
        if (!(pde & PTE_U)) { pd_set_all((int)(l >> 22), pde | PTE_U); aspace_pde_changed((int)(l >> 22)); }   /* user access needs U at both levels */
        uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000);
        pt[(l >> 12) & 0x3FF] |= PTE_U | PTE_P | PTE_W;
    }
    tlb_flush_all();
}

/* A guard page below a ring-3 stack: present for the kernel, not for ring 3, so
 * a program whose stack runs into it faults - and crash containment ends it -
 * instead of writing over whatever lies below.  The U bit has to be cleared in
 * the page's own entry: the pool's page tables are made with U set (see
 * mm_init), and a 4 MB region any user page lives in has U in its directory
 * entry.  Returns the entry's old U bit, which mm_unguard_page() puts back
 * when the page is freed. */
int mm_guard_page(uint32_t lin)
{
    uint32_t pde = kernel_pd[lin >> 22];
    if (!(pde & PTE_P) || vm_slot(lin >> 22)) return 0;
    uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000);
    int had = (pt[(lin >> 12) & 0x3FF] & PTE_U) != 0;
    pt[(lin >> 12) & 0x3FF] &= ~(uint32_t)PTE_U;
    tlb_flush_all();
    return had;
}
void mm_unguard_page(uint32_t lin, int had_u)
{
    uint32_t pde = kernel_pd[lin >> 22];
    if (!had_u || !(pde & PTE_P) || vm_slot(lin >> 22)) return;
    uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000);
    pt[(lin >> 12) & 0x3FF] |= PTE_U;
    tlb_flush_all();
}

/* Read-only for ring 3: the page directory entry must carry U (it covers 4 MB and
 * the PTEs decide), but the page itself loses W, so a native program running at
 * ring 3 can read kernel constants - the fonts it rasterises text from, the icon
 * pixels, the colour tables - and faults if it tries to write them. */
void mm_user_range_ro(uint32_t lin, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t l = lin + i * 4096;
        uint32_t pde = kernel_pd[l >> 22];
        if (!(pde & PTE_P) || vm_slot(l >> 22)) continue;
        if (!(pde & PTE_U)) { pd_set_all((int)(l >> 22), pde | PTE_U); aspace_pde_changed((int)(l >> 22)); }
        uint32_t *pt = (uint32_t*)(pde & 0xFFFFF000);
        pt[(l >> 12) & 0x3FF] = (pt[(l >> 12) & 0x3FF] | PTE_U | PTE_P) & ~(uint32_t)PTE_W;
    }
    tlb_flush_all();
}

/* v38: a large block in pages of its own, for what a program's request makes big - a
 * Windows DLL's file (Wine's shell32.dll.so is 10 MB): the pages go back to the pool when
 * it is freed, where a heap arena never goes back, and when there is no room the answer
 * is NULL, not a panic.  kfree() knows it by its header (free == HPAGES, size = pages). */
/* v40: a big one (1 MB and up - a program's or a DLL's file being loaded) is in pages that
 * need not be one run of the pool (kernel/vmem.c, #170): D95GL.DLL's 59 MB file needed 59
 * MB in one row */
#define HPAGES 2
#define HVMEM  3
void *kmalloc_pages(size_t n)
{
    int pages = (int)((n + sizeof(struct hdr) + 4095) / 4096);
    uint32_t p = 0, kind = HPAGES;
    if (pages >= 32 && (p = vm_alloc((uint32_t)pages * 4096, 0))) kind = HVMEM;   /* (v41, #402: from 128 KB up - it was 1 MB;
                                                                                    * 3 MB free in pieces could not hold a 900 KB file) */
    if (!p) p = page_alloc(pages);
    if (!p) return NULL;
    struct hdr *h = (struct hdr*)p;
    h->size = (uint32_t)pages; h->free = kind; h->next = NULL; h->magic = HMAGIC;
    return h + 1;
}

void kfree(void *p)
{
    if (!p) return;
    struct hdr *h = (struct hdr*)p - 1;
    if (!HEAPBLK(h->magic)) {
        extern int kh3_defer(void *p);
        if (kh3_defer(p)) return;                       /* v41 (#416): a block of a program's own heap (kernel/kheap3.h) */
        kpanic("kfree: bad block %p", p);
    }
    if (h->free == HPAGES) { extern void kmod_partial_gone(const void *f); kmod_partial_gone(p); h->magic = 0; page_free((uint32_t)h, (int)h->size); return; }   /* (#415: a file read in part, forgotten) */
    if (h->free == HVMEM) { extern void kmod_partial_gone(const void *f); kmod_partial_gone(p); h->magic = 0; vm_free((uint32_t)h, h->size * 4096, -1); return; }   /* (#415) */
    uint32_t f = save_flags_cli();
    if (h->magic != HMAGIC) {                           /* a program's block (v40: its count) */
        uint32_t mk = h->magic & 0xFFFF;
        if (mk < (uint32_t)nmarks) mark_bytes[mk] = mark_bytes[mk] > h->size + sizeof *h ? mark_bytes[mk] - (h->size + sizeof *h) : 0;
    }
    if (h->free == 1) {                                 /* (v41, #412: freed twice - it is on a list by size already) */
        static int said;
        if (said < 4) { said++; kprintf("kfree: block %p freed twice (thread %d)\n", p, cur_thread ? cur_thread->id : -1); }
        restore_flags(f);
        return;
    }
    struct heap *hp = arena_has(&uheap_ar, (uint32_t)h) >= 0 ? &uheap : &kheap;   /* (v41, #412: whose lists by size) */
    h->free = 1; h->magic = HMAGIC;
    /* coalesce with following free block if physically adjacent (v41: not another arena's start) */
    struct hdr *n = h->next;
    if (n && n->free == 1 && (uint8_t*)(h + 1) + h->size == (uint8_t*)n && !arena_start(n)) {
        bin_take(hp, n);
        h->size += sizeof(struct hdr) + n->size;
        h->next = n->next;
    }
    bin_put(hp, h);
    restore_flags(f);
}

/* ---- v38: what a program asked for goes back when it ends --------------------------
 * A ring-3 program's own memory - its C library's malloc() and mmap(), the gate's
 * kmalloc and page_alloc (kmod.c) - comes from the pool like the kernel's, and nothing
 * took back what a program had not freed itself when it ended.  Few programs free
 * everything; Wine's processes free next to nothing (they exit), so every Windows
 * program left megabytes behind and a few sessions of them used the memory up (#144).
 * So the gate marks what it hands out with the program's mark (its slot, 1..32), pages
 * in page_mark and heap blocks in their magic, and mod_free() calls mm_reclaim() with
 * the mark: it frees what is still marked, joins the free blocks of the heaps and gives
 * an arena that is wholly free back to the pool.  What the kernel allocates itself on a
 * program's behalf (windows, controls, handles, its image and stacks) is not marked:
 * that has an owner of its own that frees it. */
void mm_mark_pages(uint32_t phys, int n, int mark)
{
    if (!page_mark || phys < pool_start || phys >= pool_end || n <= 0) return;
    uint32_t i = (phys - pool_start) / 4096;
    uint32_t f = save_flags_cli();
    for (int k = 0; k < n && i + (uint32_t)k < npages; k++) {
        uint16_t old = page_mark[i + k];
        if (old && old < nmarks && mark_pages[old]) mark_pages[old]--;
        page_mark[i + k] = (uint16_t)mark;
        if (mark > 0 && mark < nmarks) mark_pages[mark]++;
    }
    restore_flags(f);
}
void mm_mark_block(void *p, int mark)
{
    if (!p || mark <= 0 || mark > MARK_MAX) return;
    struct hdr *h = (struct hdr*)p - 1;
    if (h->magic == HMAGIC && h->free == 0) {
        h->magic = HTAG | (uint32_t)mark;
        if (mark < nmarks) mark_bytes[mark] += h->size + sizeof *h;
    }
}
/* v40 (#186): what a program has of the pool (KB): its pages and its heap blocks */
uint32_t mm_mark_kb(int mark)
{
    if (mark <= 0 || mark >= nmarks) return 0;
    return mark_pages[mark] * 4 + mark_bytes[mark] / 1024;
}
/* free blocks next to each other become one; an arena that is one free block goes back;
 * *used: what is in use in the heap, in bytes.  (v41: a block of the list that is an arena's
 * start and all of it free is marked, then the marked ones are taken out of the list in one
 * pass - not a search of the whole list for each arena) */
#define HTRIM 0x5452494Du
static uint32_t heap_trim(struct heap *hp, uint32_t *used)
{
    struct hdr **head = hp->head; struct arenas *t = hp->ar;
    uint32_t pages = 0;
    int marked = 0;
    *used = 0;
    heap_join(hp);
    for (struct hdr *h = *head; h; h = h->next) {
        if (h->free != 1) { *used += h->size + sizeof *h; continue; }
        int a = arena_find(t, (uint32_t)h);
        if (a >= 0 && h->size == t->a[a].pages * 4096 - sizeof(struct hdr)) { bin_take(hp, h); h->magic = HTRIM; marked++; }
    }
    if (!marked) return 0;
    for (struct hdr **pp = head; *pp; ) { if ((*pp)->magic == HTRIM) *pp = (*pp)->next; else pp = &(*pp)->next; }
    int k = 0;
    for (int a = 0; a < t->n; a++) {
        struct hdr *b = (struct hdr*)t->a[a].base;
        if (b->magic == HTRIM) { b->magic = 0; page_free(t->a[a].base, (int)t->a[a].pages); pages += t->a[a].pages; }
        else t->a[k++] = t->a[a];                        /* (kept in order) */
    }
    t->n = k;
    return pages;
}
/* returns KB: what the program had left (*left), and what the heaps gave back to the pool;
 * heap_used_kb and uheap_used_kb: what is in use in them now (the log) */
uint32_t heap_used_kb, uheap_used_kb;
uint32_t mm_reclaim(int mark, uint32_t *left)
{
    uint32_t pages = 0, bytes = 0, back, hu, uu;
    uint32_t f = save_flags_cli();
    if (mark > 0 && mark < nmarks) { mark_pages[mark] = 0; mark_bytes[mark] = 0; }
    if (mark > 0 && page_mark)
        for (uint32_t i = 0; i < npages; i++)
            if (page_mark[i] == (uint16_t)mark) {
                page_mark[i] = 0;
                if (bit_get(i)) { bit_clr(i); nfree++; pages++; if (i < search_hint) search_hint = i; }
            }
    if (mark > 0)
        for (struct hdr *h = uheap_head; h; h = h->next)
            if (h->free == 0 && h->magic == (HTAG | (uint32_t)mark)) { h->free = 1; h->magic = HMAGIC; bytes += h->size + sizeof *h; bin_put(&uheap, h); }
    uint32_t hp = heap_trim(&kheap, &hu), up = heap_trim(&uheap, &uu);
    heap_arena_kb -= hp * 4; uheap_arena_kb -= up * 4;
    heap_used_kb = hu / 1024; uheap_used_kb = uu / 1024;
    back = (hp + up) * 4;
    restore_flags(f);
    if (left) *left = pages * 4 + bytes / 1024;
    return back;
}

/* v41 (#333): the heaps trimmed every few seconds (the desktop's thread, kernel/wm.c wm_pump) - an
 * arena wholly free again goes back to the pool while programs run, not only when one ends */
void mm_trim_tick(void)
{
    static uint32_t last;
    if (jiffies - last < 5 * HZ) return;
    last = jiffies;
    mm_reclaim(0, NULL);
}

static uint32_t pool_alias_end;
uint32_t pool_alias(uint32_t lin) { return (lin >= pool_start && lin < pool_alias_end) ? lin + POOL_ALIAS : lin; }
/* v41 (#336): a kernel stack's guard page - its alias (where stacks are used) unmapped (on) or
 * mapped again (off); the identity mapping keeps it.  The page tables of the kernel's half are
 * every address space's too, so the guard is everywhere */
void mm_alias_guard(uint32_t phys, int on)
{
    if (phys < pool_start || phys >= pool_alias_end) return;    /* (no alias: no guard - its stack is used at phys) */
    uint32_t lin = POOL_ALIAS + phys, pde = lin >> 22;
    if (!(kernel_pd[pde] & PTE_P)) return;
    uint32_t *pt = (uint32_t*)(kernel_pd[pde] & 0xFFFFF000u);
    pt[(lin >> 12) & 1023] = on ? 0 : (phys & 0xFFFFF000u) | PTE_P | PTE_W;
    tlb_flush_all();                                             /* (a 386 has no INVLPG) */
}
/* v41: pages below pool_alias_end - what is used while another directory may be live (a VCPI
 * client's, a Windows program's own) must be reached through the alias: kernel stacks, LDTs.
 * Until v41 these came from anywhere: on a machine past 256 MB one could lie above the alias
 * and be reached at linear == physical, which such a directory may map to a page of its own.
 * None left there: anywhere, said once. */
uint32_t page_alloc_alias(int n)
{
    if (pool_alias_end >= pool_end) return page_alloc(n);
    uint32_t f = save_flags_cli();
    uint32_t lim = (pool_alias_end - pool_start) / 4096, run = 0, start = 0;
    for (uint32_t i = search_hint < lim ? search_hint : 0, pass = 0; pass < 2; pass++, i = 0) {
        for (; i < lim; i++) {
            if (!bit_get(i)) {
                if (run == 0) start = i;
                if (++run == (uint32_t)n) {
                    for (uint32_t k = start; k <= i; k++) bit_set(k);
                    nfree -= n; free_low();
                    restore_flags(f);
                    uint32_t phys = pool_start + start * 4096;
                    memset((void*)phys, 0, n * 4096);
                    return phys;
                }
            } else run = 0;
        }
        run = 0;
    }
    restore_flags(f);
    static int said;
    if (!said++) kprintf("mm: no room below %u MB for a kernel stack - one above it (a VCPI client must not map it)\n", pool_alias_end >> 20);
    return page_alloc(n);
}
/* v41: device memory at an address of the kernel's (see kernel.h): a mapping already there that
 * holds the range is given again (a display mode set again maps its frame buffer again).  As
 * many as the window holds (124 MB of addresses; "252" this said until v41, #336).  v41 (#336):
 * a range from a mapping's start past its end, that mapping the window's last, grows it where it
 * is (the same addresses: whoever holds them keeps them) */
struct mmiomap { struct mmiomap *next; uint32_t phys, size, va; };
static struct mmiomap *mmio_maps;
static uint32_t mmio_next = MMIO_BASE;
void *kmap_mmio(uint32_t phys, uint32_t size)
{
    uint32_t p0 = phys & ~0xFFFu, sz = ((phys & 0xFFF) + size + 0xFFF) & ~0xFFFu;
    struct mmiomap *nm = kmalloc(sizeof *nm);               /* (made before the lock: kept if needed) */
    uint32_t f = save_flags_cli();
    for (struct mmiomap *m = mmio_maps; m; m = m->next) {
        if (p0 >= m->phys && p0 + sz <= m->phys + m->size) {
            uint32_t va = m->va + (p0 - m->phys) + (phys & 0xFFF);
            restore_flags(f);
            if (nm) kfree(nm);
            return (void *)va;
        }
        if (p0 >= m->phys && p0 < m->phys + m->size && m->va + m->size == mmio_next &&
            p0 + sz - m->phys - m->size <= MMIO_END - mmio_next) {       /* (the last: grown in place) */
            uint32_t more = p0 + sz - (m->phys + m->size), at = mmio_next, from = m->phys + m->size;
            m->size += more; mmio_next += more;
            uint32_t va = m->va + (p0 - m->phys) + (phys & 0xFFF);
            restore_flags(f);
            if (nm) kfree(nm);
            kmap_range(at, from, more, PTE_P | PTE_W | PTE_PCD | PTE_PWT);
            return (void *)va;
        }
    }
    if (!nm || sz > MMIO_END - mmio_next) {
        uint32_t left = MMIO_END - mmio_next;
        restore_flags(f);
        if (nm) kfree(nm);
        kprintf("mm: no room to map device memory %08x (+%u KB): %u KB of the window left\n", phys, size / 1024, left / 1024);
        return NULL;
    }
    uint32_t va = mmio_next;
    mmio_next += sz;
    nm->phys = p0; nm->size = sz; nm->va = va; nm->next = mmio_maps; mmio_maps = nm;
    restore_flags(f);
    kmap_range(va, p0, sz, PTE_P | PTE_W | PTE_PCD | PTE_PWT);
    return (void *)(va + (phys & 0xFFF));
}

/* ------------------------------------------------------------------ */
void mm_init(struct bootinfo *bi)
{
    kernel_pd = boot_pd;
    kernel_pd_phys = boot_physbase + ((uint32_t)boot_pd - KERNEL_VBASE);

    uint32_t kend = boot_physbase + ((uint32_t)_end - KERNEL_VBASE);
    uint32_t main_start = (kend + 4095) & ~4095u;
    pool_start = main_start;
    pool_end   = (bi->ext_base + bi->ext_size) & ~4095u;
    /* v42 (#491): the loader's other blocks - a UEFI firmware leaves RAM in pieces (OVMF: 1-8 MB and 9 MB up)
     * and only the largest was used.  The pool spans them all, what lies between them (the firmware's) marked
     * used; below 4 MB is every machine's first page table's (V86 memory): not used */
    uint32_t blk_a[9], blk_b[9];
    int nb = 0;
    blk_a[nb] = main_start; blk_b[nb] = pool_end; nb++;
    if (bi->ext_magic == 0x4B4C4258u)
        for (int i = 0; i < bi->ext_n && i < 8; i++) {
            uint32_t a = (bi->ext_list[i].base + 4095) & ~4095u, b = (bi->ext_list[i].base + bi->ext_list[i].size) & ~4095u;
            if (a < 0x400000u) a = 0x400000u;
            if (b > POOL_TOP) b = POOL_TOP;
            if (b < a + 0x10000u) continue;
            blk_a[nb] = a; blk_b[nb] = b; nb++;
            if (a < pool_start) pool_start = a;
            if (b > pool_end) pool_end = b;
        }
    /* v41: the pool is mapped at linear == physical, under the shared libraries' region (2 GB,
     * SHR_BASE) and the kernel's half (3 GB) - memory past 2 GB is not used (until v41 it was
     * mapped on into the kernel's half: a machine with 3 GB and more did not start) */
    if (pool_end > POOL_TOP) {
#ifdef QDP
        kprintf("mm: %u MB of memory above 3 GB not used (32-bit addresses: the pool is mapped where it is)\n", (pool_end - POOL_TOP) >> 20);
#else
        kprintf("mm: %u MB of memory above 2 GB not used (32-bit addresses: the pool is mapped where it is)\n", (pool_end - SHR_BASE) >> 20);
#endif
        pool_end = POOL_TOP;
        if (blk_b[0] > POOL_TOP) blk_b[0] = POOL_TOP;
    }
    npages = (pool_end - pool_start) / 4096;

    /* bitmap lives at the start of the main block (after the kernel): every page used, then the blocks' free */
    uint32_t bmbytes = ((npages + 31) / 32) * 4;
    uint32_t bmpages = (bmbytes + 4095) / 4096;
    bitmap = (uint32_t*)main_start;
    memset(bitmap, 0xFF, bmpages * 4096);
    nfree = 0;
    for (int k = 0; k < nb; k++)
        for (uint32_t p = blk_a[k]; p < blk_b[k]; p += 4096) {
            if (p >= main_start && p < main_start + bmpages * 4096) continue;   /* (the bitmap's own) */
            uint32_t i = (p - pool_start) / 4096;
            if (bit_get(i)) { bit_clr(i); nfree++; }
        }
    npages_ram = nfree + bmpages;
    search_hint = (main_start - pool_start) / 4096 + bmpages;
    if (nb > 1) {
        kprintf("mm: memory in %d pieces:", nb);
        for (int k = 0; k < nb; k++) kprintf(" %08x-%08x", blk_a[k], blk_b[k]);
        kprintf(" (%u KB besides the largest)\n", (npages_ram - (blk_b[0] - main_start) / 4096) * 4);
    }

    /* identity-map the whole pool (boot mapped only ~12MB) */
    for (uint32_t p = pool_start; p < pool_end; p += 0x400000) {
        uint32_t pde = p >> 22;
        if (kernel_pd[pde] & PTE_P) continue;
        uint32_t ptp = page_alloc(1);
        uint32_t *pt = (uint32_t*)ptp;
        /* PTE_U so ring-3 DPMI clients can reach memory they allocate from the
         * pool (linear == physical here); ring 0 is unaffected by the U bit. */
        for (int i = 0; i < 1024; i++) pt[i] = ((p & 0xFFC00000) + i * 4096) | PTE_P | PTE_W | PTE_U;
        kernel_pd[pde] = ptp | PTE_P | PTE_W;
    }
    flush_tlb();
    /* A second, HIGH mapping of the pool (POOL_ALIAS + phys).  Kernel thread stacks
     * are addressed through it: a trap out of a VCPI client (Windows' DOSX) arrives
     * while the CLIENT's page directory is live, and the client owns every linear
     * address below 3 GB - it may map the stack's identity address to a page of its
     * own.  Only the kernel half (>= 3 GB) is guaranteed intact there. */
    pool_alias_end = pool_end < POOL_ALIAS_MAX ? pool_end : POOL_ALIAS_MAX;
    if (pool_alias_end > pool_start) kmap_range(POOL_ALIAS + pool_start, pool_start, pool_alias_end - pool_start, PTE_P | PTE_W);
    page_mark = (uint16_t*)page_alloc((int)((npages * 2 + 4095) / 4096));   /* v38: zeroed - nobody's */
    heap_add_arena(1024 * 1024);
    /* private address spaces (v38): a quarter of the memory, 16 to 64 MB, from 4 MB up -
     * room for a Windows program at 0x400000; none on a machine under 48 MB */
    {
        uint32_t h = (pool_end / 4) & ~0x3FFFFFu;
        if (h > 0x04000000u) h = 0x04000000u;
        if (h < 0x01000000u) h = 0x01000000u;
        as_hole_end = (pool_end >= 0x03000000u && h > main_start + 0x400000u) ? h : 0;   /* (v42: the main block's start - as before #491) */
    }
    vm_shared_init();                               /* v41 (#390): big memory such a program sees (vmem.c) */
    kprintf("mm: pool %08x-%08x, %u pages (%u KB), heap ok, private spaces %s%x\n", pool_start, pool_end, npages, npages * 4,
            as_hole_end ? "0x400000-" : "off", as_hole_end);
}
