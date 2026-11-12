/* DOS95 host layer for QEMU: a small C library mapped onto the DOS95 kernel.
 *
 * Everything QEMU 0.14.1's i386 system emulator needs from libc, implemented on
 * kmalloc / page_alloc, the DOS file services (fs_*), the scheduler and kprintf.
 * No threads (QEMU runs single-threaded on one kernel thread), no sockets, no
 * signals - those calls return errors / are no-ops.
 */
#include "dos95-host.h"

int errno;

/* ---- memory ------------------------------------------------------------------------- */
#define BIG_ALLOC (256 * 1024)            /* above this we hand out whole pages */
#ifdef QDP
/* the QEMU DOS port runs machine after machine in one kernel: every block QEMU has is on a list (mlist), and when a
 * machine's run is over what is still on it is freed (qemu_dos95_run_over, at this file's end) - as a process's end
 * would.  (32 bytes: the blocks stay 16-byte aligned.)  The list is changed with interrupts off: QEMU's one thread, on
 * the first processor (v44: kernel threads stay there), is the only one that allocates */
struct mhdr { uint32_t magic, size; void *base; uint32_t pages; struct mhdr *prev, *next; uint32_t pad[2]; };
static struct mhdr *mlist;
static inline uint32_t m_cli(void) { uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void m_sti(uint32_t f) { __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }
static void m_link(struct mhdr *h) { uint32_t f = m_cli(); h->prev = NULL; h->next = mlist; if (mlist) mlist->prev = h; mlist = h; m_sti(f); }
static void m_unlink(struct mhdr *h)
{
    uint32_t f = m_cli();
    if (h->prev) h->prev->next = h->next; else if (mlist == h) mlist = h->next;
    if (h->next) h->next->prev = h->prev;
    h->prev = h->next = NULL;
    m_sti(f);
}
#else
struct mhdr { uint32_t magic, size; void *base; uint32_t pages; };
#endif
#define MMAGIC 0x51454D55u

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    if (n >= BIG_ALLOC) {
        uint32_t pages = (n + sizeof(struct mhdr) + 4095) / 4096;
        uint32_t p = page_alloc(pages);
        if (!p) return NULL;
        struct mhdr *h = (struct mhdr*)p;
        h->magic = MMAGIC; h->size = n; h->base = (void*)p; h->pages = pages;
#ifdef QDP
        m_link(h);
#endif
        return h + 1;
    }
    struct mhdr *h = kmalloc(n + sizeof *h);
    if (!h) return NULL;
    h->magic = MMAGIC; h->size = n; h->base = h; h->pages = 0;
#ifdef QDP
    m_link(h);
#endif
    return h + 1;
}
void free(void *p)
{
    if (!p) return;
    struct mhdr *h = (struct mhdr*)p - 1;
    if (h->magic != MMAGIC) { kprintf("qemu: free(%p): bad block\n", p); return; }
    h->magic = 0;
#ifdef QDP
    m_unlink(h);
#endif
    if (h->pages) page_free((uint32_t)h->base, h->pages);
    else kfree(h->base);
}
void *calloc(size_t a, size_t b)
{
    if (b && a > (size_t)-1 / b) return NULL;             /* (v41, #336: a size past 32 bits - none, not a small one) */
    size_t n = a * b; void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    struct mhdr *h = (struct mhdr*)p - 1;
    if (h->size >= n && h->size / 2 <= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    free(p);
    return q;
}
/* aligned allocation: over-allocate and keep the real pointer in front */
void *dos95_memalign(size_t align, size_t n)
{
    if (align < 16) align = 16;
    uint8_t *raw = malloc(n + align + sizeof(void*));
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1);
    ((void**)a)[-1] = raw;
    return (void*)a;
}
void dos95_free_aligned(void *p) { if (p) free(((void**)p)[-1]); }

#ifdef QDP
/* (the QEMU DOS port: a page in front of the pages asked for holds the block's place on mlist - the address given is
 * still a page's, and what QEMU leaves mapped is freed at the run's end with the rest) */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!(flags & MAP_ANONYMOUS)) { errno = ENODEV; return MAP_FAILED; }
    uint32_t pages = (len + 4095) / 4096 + 1;
    uint32_t p = page_alloc(pages);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    struct mhdr *h = (struct mhdr*)p;
    h->magic = MMAGIC; h->size = len; h->base = (void*)p; h->pages = pages;
    m_link(h);
    return (void*)(p + 4096);
}
int munmap(void *addr, size_t len)
{
    struct mhdr *h = (struct mhdr*)((uint32_t)addr - 4096);
    if (((uint32_t)addr & 4095) || h->magic != MMAGIC) { kprintf("qemu: munmap(%p): not mapped\n", addr); errno = EINVAL; return -1; }
    h->magic = 0; m_unlink(h);
    page_free((uint32_t)h->base, h->pages);
    return 0;
}
#else
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!(flags & MAP_ANONYMOUS)) { errno = ENODEV; return MAP_FAILED; }
    uint32_t pages = (len + 4095) / 4096;
    uint32_t p = page_alloc(pages);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    return (void*)p;
}
int munmap(void *addr, size_t len) { page_free((uint32_t)addr, (len + 4095) / 4096); return 0; }
#endif
int mprotect(void *addr, size_t len, int prot) { return 0; }
int madvise(void *addr, size_t len, int advice) { return 0; }
int msync(void *addr, size_t len, int flags) { return 0; }
int mlock(const void *addr, size_t len) { return 0; }
int munlock(const void *addr, size_t len) { return 0; }
int getpagesize(void) { return 4096; }
long sysconf(int name) { if (name == _SC_PAGESIZE) return 4096; if (name == _SC_NPROCESSORS_ONLN) return 1; return -1; }

/* ---- process ------------------------------------------------------------------------ */
void qemu_dos95_exit(int code);           /* the launcher's exit path (thread ends) */
void abort(void) { kprintf("qemu: abort()\n"); qemu_dos95_exit(134); for (;;) ; }
/* (v41, #336, K29: as many as it registers - 16 until v41, the 17th dropped) */
static void (*atexit_first[32])(void), (**atexit_fns)(void) = atexit_first; static int natexit, atexit_cap = 32;
int atexit(void (*fn)(void))
{
    if (natexit == atexit_cap) {
        void (**n)(void) = malloc(sizeof *n * (size_t)(atexit_cap * 2));
        if (!n) return -1;
        memcpy(n, atexit_fns, sizeof *n * (size_t)natexit);
        if (atexit_fns != atexit_first) free(atexit_fns);
        atexit_fns = n; atexit_cap *= 2;
    }
    atexit_fns[natexit++] = fn;
    return 0;
}
void exit(int code)
{
    kprintf("qemu: exit(%d) from %p\n", code, __builtin_return_address(0));
    while (natexit > 0) atexit_fns[--natexit]();
    qemu_dos95_exit(code);
    for (;;) ;
}
void dos95_assert_fail(const char *expr, const char *file, int line, const char *func)
{
    kprintf("qemu: assertion '%s' failed at %s:%d (%s)\n", expr, file, line, func);
    abort();
}
int getpid(void) { return 1; }
uid_t getuid(void) { return 0; } uid_t geteuid(void) { return 0; } gid_t getgid(void) { return 0; }
int isatty(int fd) { return 0; }
char *getenv(const char *name) { return NULL; }
int setenv(const char *name, const char *value, int overwrite) { return 0; }
int unsetenv(const char *name) { return 0; }
int system(const char *cmd) { return -1; }
int gethostname(char *name, size_t len) { strlcpy(name, "dos95", len); return 0; }
int uname(struct utsname *u) { strcpy(u->sysname, "DOS95"); strcpy(u->nodename, "dos95"); strcpy(u->release, "1.0"); strcpy(u->version, "1"); strcpy(u->machine, "i386"); return 0; }
pid_t waitpid(pid_t p, int *status, int opts) { return -1; }
int kill(pid_t pid, int sig) { return -1; }
int raise(int sig) { if (sig == SIGABRT) abort(); return 0; }
sighandler_t signal(int sig, sighandler_t h) { return SIG_DFL; }
int sigaction(int sig, const struct sigaction *a, struct sigaction *old) { return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *old) { if (old) sigemptyset(old); return 0; }
int setitimer(int which, const struct itimerval *nv, struct itimerval *ov) { return 0; }
int getrlimit(int r, struct rlimit *l) { l->rlim_cur = l->rlim_max = 1024; return 0; }
int setrlimit(int r, const struct rlimit *l) { return 0; }
int pipe(int fds[2]) { errno = ENOSYS; return -1; }
int dup(int fd) { errno = ENOSYS; return -1; }
int dup2(int a, int b) { errno = ENOSYS; return -1; }
int fcntl(int fd, int cmd, ...) { return 0; }
int ioctl(int fd, unsigned long req, ...) { errno = ENOTTY; return -1; }
int mkstemp(char *tmpl) { errno = ENOSYS; return -1; }
char *realpath(const char *path, char *resolved) { strcpy(resolved, path); return resolved; }
int access(const char *path, int mode) { return fs_getattr(path) >= 0 ? 0 : (errno = ENOENT, -1); }
int mkdir(const char *path, mode_t mode) { return fs_mkdir(path) == 0 ? 0 : (errno = EEXIST, -1); }
int rmdir(const char *p) { return fs_rmdir(p) == 0 ? 0 : (errno = ENOTEMPTY, -1); }
int unlink(const char *path) { return fs_unlink(path) == 0 ? 0 : (errno = ENOENT, -1); }
int remove(const char *path) { return unlink(path); }
int rename(const char *a, const char *b) { return fs_rename(a, b) == 0 ? 0 : (errno = EIO, -1); }
int chdir(const char *p) { return fs_chdir(p) == 0 ? 0 : -1; }
char *getcwd(char *buf, size_t n) { if (fs_getcwd(0, buf, n) < 0) strlcpy(buf, "C:\\", n); return buf; }
mode_t umask(mode_t m) { return 022; }
int chmod(const char *p, mode_t m) { return 0; }
int usleep(useconds_t us) { thread_sleep_ms(us < 1000 ? 1 : us / 1000); return 0; }
unsigned sleep(unsigned s) { thread_sleep_ms(s * 1000); return 0; }
int nanosleep(const struct timespec *req, struct timespec *rem) { thread_sleep_ms(req->tv_sec * 1000 + req->tv_nsec / 1000000); return 0; }
struct passwd *getpwnam(const char *n) { return NULL; }
struct passwd *getpwuid(unsigned u) { return NULL; }
int tcgetattr(int fd, struct termios *t) { return -1; }
int tcsetattr(int fd, int a, const struct termios *t) { return -1; }
int poll(struct pollfd *f, unsigned n, int t) { if (t > 0) thread_sleep_ms(t); return 0; }
DIR *opendir(const char *p) { return NULL; }
struct dirent *readdir(DIR *d) { return NULL; }
int closedir(DIR *d) { return 0; }
int fnmatch(const char *pat, const char *s, int flags) { return FNM_NOMATCH; }
char *basename(char *p) { char *b = strrchr(p, '\\'); if (!b) b = strrchr(p, '/'); return b ? b + 1 : p; }
char *dirname(char *p) { char *b = strrchr(p, '\\'); if (!b) b = strrchr(p, '/'); if (b) *b = 0; else strcpy(p, "."); return p; }

/* sockets: not available */
int socket(int d, int t, int p) { errno = ENOSYS; return -1; }
int bind(int s, const struct sockaddr *a, socklen_t l) { errno = ENOSYS; return -1; }
int listen(int s, int b) { errno = ENOSYS; return -1; }
int accept(int s, struct sockaddr *a, socklen_t *l) { errno = ENOSYS; return -1; }
int connect(int s, const struct sockaddr *a, socklen_t l) { errno = ENOSYS; return -1; }
ssize_t send(int s, const void *b, size_t n, int f) { errno = ENOSYS; return -1; }
ssize_t recv(int s, void *b, size_t n, int f) { errno = ENOSYS; return -1; }
ssize_t sendto(int s, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t l) { errno = ENOSYS; return -1; }
ssize_t recvfrom(int s, void *b, size_t n, int f, struct sockaddr *a, socklen_t *l) { errno = ENOSYS; return -1; }
int setsockopt(int s, int lvl, int opt, const void *v, socklen_t l) { return -1; }
int getsockopt(int s, int lvl, int opt, void *v, socklen_t *l) { return -1; }
int getsockname(int s, struct sockaddr *a, socklen_t *l) { return -1; }
int getpeername(int s, struct sockaddr *a, socklen_t *l) { return -1; }
int shutdown(int s, int how) { return -1; }
int socketpair(int d, int t, int p, int sv[2]) { errno = ENOSYS; return -1; }
struct hostent *gethostbyname(const char *n) { return NULL; }
int getaddrinfo(const char *n, const char *s, const struct addrinfo *h, struct addrinfo **r) { return EAI_NONAME; }
void freeaddrinfo(struct addrinfo *r) {}
const char *gai_strerror(int e) { return "no network"; }
int getnameinfo(const struct sockaddr *sa, socklen_t l, char *h, socklen_t hl, char *s, socklen_t sl, int f) { return EAI_NONAME; }
in_addr_t inet_addr(const char *s) { struct in_addr a; return inet_aton(s, &a) ? a.s_addr : INADDR_NONE; }
int inet_aton(const char *s, struct in_addr *a)
{
    uint32_t v = 0; int i;
    for (i = 0; i < 4; i++) { char *e; long b = strtol(s, &e, 10); if (e == s || b < 0 || b > 255) return 0; v = (v << 8) | b; s = e; if (i < 3) { if (*s != '.') return 0; s++; } }
    a->s_addr = htonl(v); return 1;
}
char *inet_ntoa(struct in_addr a) { static char b[20]; uint32_t v = ntohl(a.s_addr); snprintf(b, sizeof b, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255); return b; }

/* ---- strings ------------------------------------------------------------------------ */
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char *strncat(char *d, const char *s, size_t n) { char *r = d; d += strlen(d); while (n-- && *s) *d++ = *s++; *d = 0; return r; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strndup(const char *s, size_t n) { size_t l = strnlen(s, n); char *p = malloc(l + 1); if (p) { memcpy(p, s, l); p[l] = 0; } return p; }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char*)s; return NULL; }
char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) { *save = s; return NULL; }
    char *tok = s; s += strcspn(s, delim);
    if (*s) *s++ = 0;
    *save = s; return tok;
}
char *strtok(char *s, const char *delim) { static char *save; return strtok_r(s, delim, &save); }
char *strerror(int e)
{
    switch (e) {
    case 0: return "Success"; case ENOENT: return "No such file or directory"; case EIO: return "Input/output error";
    case EBADF: return "Bad file descriptor"; case ENOMEM: return "Cannot allocate memory"; case EACCES: return "Permission denied";
    case EEXIST: return "File exists"; case ENOTDIR: return "Not a directory"; case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument"; case ENOSPC: return "No space left on device"; case ENOSYS: return "Function not implemented";
    case ENOTSUP: return "Operation not supported"; case EMEDIUMTYPE: return "Wrong medium type"; case ENOMEDIUM: return "No medium found";
    case EAGAIN: return "Resource temporarily unavailable"; case ERANGE: return "Result out of range"; case ENODEV: return "No such device";
    default: { static char b[32]; snprintf(b, sizeof b, "Error %d", e); return b; }
    }
}
void perror(const char *s) { fprintf(stderr, "%s: %s\n", s ? s : "", strerror(errno)); }

/* ---- number parsing ----------------------------------------------------------------- */
static unsigned long long parse_ull(const char *s, char **end, int base, int *neg, int *ok)
{
    const char *p = s; unsigned long long v = 0; *neg = 0; *ok = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') { *neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { base = 16; p += 2; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    for (;;) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = dos95_toupper(*p) - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; p++; *ok = 1;
    }
    if (end) *end = (char*)(*ok ? p : s);
    return v;
}
unsigned long long strtoull(const char *s, char **end, int base) { int neg, ok; unsigned long long v = parse_ull(s, end, base, &neg, &ok); return neg ? -v : v; }
long long strtoll(const char *s, char **end, int base) { int neg, ok; unsigned long long v = parse_ull(s, end, base, &neg, &ok); return neg ? -(long long)v : (long long)v; }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }
double strtod(const char *s, char **end)
{
    const char *p = s; double v = 0; int neg = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; }
    if (*p == '.') { p++; double f = 0.1; while (isdigit((unsigned char)*p)) { v += (*p - '0') * f; f /= 10; p++; } }
    if (*p == 'e' || *p == 'E') { char *e; long ex = strtol(p + 1, &e, 10); if (e != p + 1) { p = e; while (ex > 0) { v *= 10; ex--; } while (ex < 0) { v /= 10; ex++; } } }
    if (end) *end = (char*)p;
    return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, NULL); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
static unsigned rnd_state = 12345;
int rand(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 1) & RAND_MAX; }
void srand(unsigned seed) { rnd_state = seed; }

/* ---- qsort / bsearch ---------------------------------------------------------------- */
static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    /* shell sort: simple, no recursion, good enough for QEMU's small tables */
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
                swap_bytes(b + (j - gap) * size, b + j * size, size);
}
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; const char *p = (const char*)base + mid * size; int c = cmp(key, p); if (c == 0) return (void*)p; if (c < 0) hi = mid; else lo = mid + 1; }
    return NULL;
}

/* ---- time --------------------------------------------------------------------------- */
static int64_t boot_epoch = -1;       /* wall-clock seconds at jiffies == 0 */
static uint64_t pit_base_cycles;
static uint32_t pit_last; static uint64_t pit_accum;
static uint64_t cycles64(void)         /* 64-bit extension of the 32-bit PIT cycle counter */
{
    uint32_t f = save_flags_cli();
    uint32_t c = pit_cycles();
    pit_accum += (uint32_t)(c - pit_last); pit_last = c;
    uint64_t r = pit_accum;
    restore_flags(f);
    return r;
}
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + doe - 719468;
}
static void civil_from_days(int64_t z, int *yy, int *mm, int *dd)
{
    z += 719468; int era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = z - era * 146097LL;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int y = yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1; unsigned m = mp + (mp < 10 ? 3 : -9);
    *yy = y + (m <= 2); *mm = m; *dd = d;
}
static void sync_epoch(void)
{
    if (boot_epoch >= 0) return;
    int y, mo, d, dow, h, mi, s; fs_get_datetime(&y, &mo, &d, &dow, &h, &mi, &s);
    int64_t now = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    boot_epoch = now - (int64_t)(cycles64() / PIT_CLK);
    pit_base_cycles = 0;
}
int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    sync_epoch();
    uint64_t c = cycles64();
    tv->tv_sec = boot_epoch + c / PIT_CLK;
    tv->tv_usec = (c % PIT_CLK) * 1000000ULL / PIT_CLK;
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    return 0;
}
int settimeofday(const struct timeval *tv, const struct timezone *tz) { return -1; }
int clock_gettime(clockid_t id, struct timespec *ts)
{
    uint64_t c = cycles64();
    if (id == CLOCK_REALTIME) { sync_epoch(); ts->tv_sec = boot_epoch + c / PIT_CLK; }
    else ts->tv_sec = c / PIT_CLK;
    ts->tv_nsec = (c % PIT_CLK) * 1000000000ULL / PIT_CLK;
    return 0;
}
time_t time(time_t *t) { struct timeval tv; gettimeofday(&tv, NULL); if (t) *t = tv.tv_sec; return tv.tv_sec; }
clock_t clock(void) { return (clock_t)(cycles64() * 1000000ULL / PIT_CLK); }
struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    int64_t secs = *t; int64_t days = secs / 86400; int64_t rem = secs % 86400; if (rem < 0) { rem += 86400; days--; }
    int y, m, d; civil_from_days(days, &y, &m, &d);
    memset(out, 0, sizeof *out);
    out->tm_sec = rem % 60; out->tm_min = (rem / 60) % 60; out->tm_hour = rem / 3600;
    out->tm_mday = d; out->tm_mon = m - 1; out->tm_year = y - 1900;
    out->tm_wday = (int)((days % 7 + 11) % 7);
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    return out;
}
struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
struct tm *localtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
time_t timegm(struct tm *tm) { return days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday) * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec; }
time_t mktime(struct tm *tm) { return timegm(tm); }
char *asctime(const struct tm *tm) { static char b[32]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec); return b; }
char *ctime(const time_t *t) { return asctime(gmtime(t)); }
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) { return snprintf(s, max, "%04d-%02d-%02d %02d:%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec); }

/* select(): QEMU's main loop only ever gives us a timeout (no host fds exist) */
int select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv)
{
    int ms = tv ? tv->tv_sec * 1000 + tv->tv_usec / 1000 : 0;
    extern void dos95_net_poll(void);
    qemu_dos95_wait(ms);
    dos95_net_poll();                 /* frames from the DOS95 bridge -> guest NIC */
    return 0;
}

/* ---- fenv (x87) --------------------------------------------------------------------- */
static inline unsigned short fpu_getcw(void) { unsigned short cw; __asm__ volatile("fnstcw %0" : "=m"(cw)); return cw; }
static inline void fpu_setcw(unsigned short cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }
int fesetround(int mode) { fpu_setcw((fpu_getcw() & ~0xC00) | (mode & 0xC00)); return 0; }
int fegetround(void) { return fpu_getcw() & 0xC00; }
int feclearexcept(int ex) { __asm__ volatile("fnclex"); return 0; }
int fetestexcept(int ex) { unsigned short sw; __asm__ volatile("fnstsw %0" : "=m"(sw)); return sw & ex & FE_ALL_EXCEPT; }
int feraiseexcept(int ex) { return 0; }

/* ---- files -------------------------------------------------------------------------- */
/* (v41, #336, K29) as many as DOS lets the system session have open - its handle table holds 255
 * (INT 21h AH=67h, kernel/dossvc.c), shared by every DOS95 program; each descriptor here is one
 * of its handles.  32 until v41 */
#define MAXFD 256
struct fdesc { int used; int dosh; int64_t pos; int64_t size; int writable; };
static struct fdesc fds[MAXFD];

static int fd_alloc(void) { for (int i = 3; i < MAXFD; i++) if (!fds[i].used) { fds[i].used = 1; return i; } return -1; }
int open(const char *path, int flags, ...)
{
    int acc = flags & O_ACCMODE, h;
    int exists = fs_getattr(path) >= 0;
    if ((flags & O_CREAT) && (flags & O_EXCL) && exists) { errno = EEXIST; return -1; }
    if (!exists && !(flags & O_CREAT)) { errno = ENOENT; return -1; }
    if (!exists || (flags & O_TRUNC)) { h = fs_create(path); if (h < 0) { errno = EACCES; return -1; } fs_close(h); }
    h = fs_open(path, acc == O_RDONLY ? 0 : 2);
    if (h < 0) { if (acc != O_RDONLY) h = fs_open(path, 0); if (h < 0) { errno = ENOENT; return -1; } acc = O_RDONLY; }
    int fd = fd_alloc(); if (fd < 0) { fs_close(h); errno = EMFILE; return -1; }
    int64_t sz = fs_filesize64(h);
    fds[fd].dosh = h; fds[fd].pos = 0; fds[fd].size = sz < 0 ? 0 : sz; fds[fd].writable = acc != O_RDONLY;
    if (flags & O_APPEND) fds[fd].pos = fds[fd].size;
    return fd;
}
int close(int fd) { if (fd < 3 || fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; } fs_close(fds[fd].dosh); fds[fd].used = 0; return 0; }
/* v41 (#336, F24, K29): a file's places to FAT32's 4 GB - 1 (fs_seek64) - an image file to 4 GB
 * - 1 on a FAT32 disk (2 GB - 1 until v41: the 32-bit calls) */
#define FILE_MAX 0xFFFFFFFFLL
static int fd_seek(struct fdesc *f, int64_t pos)
{
    if (pos < 0 || pos > FILE_MAX) { errno = EOVERFLOW; return -1; }
    if (fs_seek64(f->dosh, pos, 0) < 0) { errno = EIO; return -1; }
    return 0;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    if (fd < 3 || fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; }
    struct fdesc *f = &fds[fd];
    if (off < 0) { errno = EINVAL; return -1; }
    if (off >= f->size) return 0;
    if (fd_seek(f, off) < 0) return -1;
    size_t got = 0;
    while (got < n) { int chunk = n - got > 32768 ? 32768 : (int)(n - got); int r = fs_read(f->dosh, (char*)buf + got, chunk); if (r < 0) { errno = EIO; return -1; } if (r == 0) break; got += r; }
    return got;
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    if (fd < 3 || fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; }
    struct fdesc *f = &fds[fd];
    if (!f->writable) { errno = EBADF; return -1; }
    if (off < 0) { errno = EINVAL; return -1; }
    /* a file is 4 GB - 1 at most: what fits of the write, none - EFBIG (POSIX) */
    if (n > 0 && off >= FILE_MAX) { errno = EFBIG; return -1; }
    if ((int64_t)n > FILE_MAX - off) n = (size_t)(FILE_MAX - off);
    if (fd_seek(f, off) < 0) return -1;
    size_t done = 0;
    while (done < n) { int chunk = n - done > 32768 ? 32768 : (int)(n - done); int r = fs_write(f->dosh, (const char*)buf + done, chunk); if (r <= 0) { errno = ENOSPC; return done ? (ssize_t)done : -1; } done += r; }
    if (off + (int64_t)done > f->size) f->size = off + done;
    return done;
}
ssize_t read(int fd, void *buf, size_t n)
{
    if (fd == 0) return 0;
    ssize_t r = pread(fd, buf, n, fds[fd].pos); if (r > 0) fds[fd].pos += r; return r;
}
#ifdef QDP
/* (the QEMU DOS port: what QEMU writes to stderr is passed on line by line to kernel/vpc.c, which keeps the last - the
 * manager shows it when a machine stops at once: "could not load PC BIOS", an option QEMU does not know) */
void vpc_qemu_said(const char *line);
static char said[200]; static int saidn;
static void qdp_said(const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\n' || c == '\r') { if (saidn) { said[saidn] = 0; vpc_qemu_said(said); saidn = 0; } }
        else if (saidn < (int)sizeof said - 1) said[saidn++] = c;
    }
}
#endif
ssize_t write(int fd, const void *buf, size_t n)
{
#ifdef QDP
    if (fd == 2) qdp_said(buf, n);
#endif
    if (fd == 1 || fd == 2) { char tmp[256]; size_t i = 0; while (i < n) { size_t c = n - i > 255 ? 255 : n - i; memcpy(tmp, (const char*)buf + i, c); tmp[c] = 0; kputs(tmp); i += c; } return n; }
    ssize_t r = pwrite(fd, buf, n, fds[fd].pos); if (r > 0) fds[fd].pos += r; return r;
}
off_t lseek(int fd, off_t off, int whence)
{
    if (fd < 3 || fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; }
    struct fdesc *f = &fds[fd];
    int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? f->pos + off : f->size + off;
    if (np < 0) { errno = EINVAL; return -1; }
    f->pos = np; return np;
}
int ftruncate(int fd, off_t len)
{
    if (fd < 3 || fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; }
    struct fdesc *f = &fds[fd];
    if (len < 0) { errno = EINVAL; return -1; }
    if (len > FILE_MAX) { errno = EFBIG; return -1; }
    if (len < f->size) { if (fd_seek(f, len) < 0) return -1; if (fs_write(f->dosh, "", 0) < 0) { errno = EIO; return -1; } f->size = len; return 0; }
    if (len > f->size) {  /* extend by writing zeros */
        static char zero[4096]; int64_t pos = f->size; if (fd_seek(f, pos) < 0) return -1;
        while (pos < len) { int chunk = len - pos > 4096 ? 4096 : (int)(len - pos); if (fs_write(f->dosh, zero, chunk) != chunk) { errno = ENOSPC; return -1; } pos += chunk; }
        f->size = len;
    }
    return 0;
}
int fsync(int fd) { return 0; }
int fdatasync(int fd) { return 0; }
int fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof *st);
    if (fd < 3) { st->st_mode = S_IFCHR; return 0; }
    if (fd >= MAXFD || !fds[fd].used) { errno = EBADF; return -1; }
    st->st_mode = S_IFREG | 0644; st->st_size = fds[fd].size; st->st_blksize = 4096; st->st_blocks = (fds[fd].size + 511) / 512;
    return 0;
}
int stat(const char *path, struct stat *st)
{
    int a = fs_getattr(path); if (a < 0) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    if (a & 0x10) { st->st_mode = S_IFDIR | 0755; return 0; }
    st->st_mode = S_IFREG | 0644;
    int h = fs_open(path, 0); if (h >= 0) { int64_t sz = fs_filesize64(h); st->st_size = sz < 0 ? 0 : sz; fs_close(h); }
    st->st_blksize = 4096; st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}
int lstat(const char *path, struct stat *st) { return stat(path, st); }
ssize_t readv(int fd, const struct iovec *iov, int n) { ssize_t t = 0; for (int i = 0; i < n; i++) { ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < iov[i].iov_len) break; } return t; }
ssize_t writev(int fd, const struct iovec *iov, int n) { ssize_t t = 0; for (int i = 0; i < n; i++) { ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len); if (r < 0) return t ? t : -1; t += r; } return t; }
ssize_t preadv(int fd, const struct iovec *iov, int n, off_t off) { ssize_t t = 0; for (int i = 0; i < n; i++) { ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off + t); if (r < 0) return t ? t : -1; t += r; if ((size_t)r < iov[i].iov_len) break; } return t; }
ssize_t pwritev(int fd, const struct iovec *iov, int n, off_t off) { ssize_t t = 0; for (int i = 0; i < n; i++) { ssize_t r = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + t); if (r < 0) return t ? t : -1; t += r; } return t; }

/* ---- stdio -------------------------------------------------------------------------- */
struct dos95_FILE { int fd; int eof, err; int ungot; };
static struct dos95_FILE f_stdin = { 0 }, f_stdout = { 1 }, f_stderr = { 2 };
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;

FILE *fopen(const char *path, const char *mode)
{
    int flags = O_RDONLY;
    if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC; else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    if (strchr(mode, '+')) flags = (flags & ~O_ACCMODE) | O_RDWR;
    int fd = open(path, flags, 0644); if (fd < 0) return NULL;
    FILE *f = calloc(1, sizeof *f);
    if (!f) { close(fd); return NULL; }                   /* (v41, #336: until v41 a NULL written to) */
    f->fd = fd; f->ungot = -1; return f;
}
FILE *fdopen(int fd, const char *mode) { FILE *f = calloc(1, sizeof *f); if (!f) return NULL; f->fd = fd; f->ungot = -1; return f; }
int fclose(FILE *f) { if (!f || f->fd < 3) return 0; close(f->fd); free(f); return 0; }
size_t fread(void *buf, size_t size, size_t n, FILE *f) { ssize_t r = read(f->fd, buf, size * n); if (r <= 0) { if (r < 0) f->err = 1; else f->eof = 1; return 0; } return r / size; }
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f) { ssize_t r = write(f->fd, buf, size * n); if (r < 0) { f->err = 1; return 0; } return r / size; }
int fseeko(FILE *f, off_t off, int whence) { f->eof = 0; return lseek(f->fd, off, whence) < 0 ? -1 : 0; }
int fseek(FILE *f, long off, int whence) { return fseeko(f, off, whence); }
off_t ftello(FILE *f) { return lseek(f->fd, 0, SEEK_CUR); }
long ftell(FILE *f) { off_t p = ftello(f); if (p > 0x7FFFFFFF) { errno = EOVERFLOW; return -1; } return (long)p; }   /* (v41, F24: it was cut) */
void rewind(FILE *f) { fseeko(f, 0, SEEK_SET); }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fileno(FILE *f) { return f->fd; }
int fflush(FILE *f) { return 0; }
void setbuf(FILE *f, char *buf) {}
int setvbuf(FILE *f, char *buf, int mode, size_t size) { return 0; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { return NULL; }
int fputc(int c, FILE *f) { unsigned char ch = c; return write(f->fd, &ch, 1) == 1 ? c : EOF; }
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { return write(f->fd, s, strlen(s)) < 0 ? EOF : 0; }
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }
int fgetc(FILE *f) { unsigned char c; if (f->ungot >= 0) { int r = f->ungot; f->ungot = -1; return r; } if (read(f->fd, &c, 1) != 1) { f->eof = 1; return EOF; } return c; }
int getc(FILE *f) { return fgetc(f); }
char *fgets(char *buf, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) { int c = fgetc(f); if (c == EOF) break; buf[i++] = c; if (c == '\n') break; }
    if (i == 0) return NULL;
    buf[i] = 0; return buf;
}

/* printf family: full-featured enough for QEMU (flags, width, precision, h/hh/l/ll/z/t, d i u o x X c s p n %, f/g/e via double) */
struct outbuf { char *p; size_t cap, len; };
static void ob_put(struct outbuf *o, char c) { if (o->len + 1 < o->cap) o->p[o->len] = c; o->len++; }
static void ob_pad(struct outbuf *o, int n, char c) { while (n-- > 0) ob_put(o, c); }
/* v41 (#336): %f %e %g %a exact, as glibc prints them (java/dos95/fmtfp.h's, for a double) - until
 * v41 a short fmt_double: %e and %g printed as %f, numbers past 2^64 wrong */
#define FMTFP_NO_LDOUBLE
#include "../../java/dos95/fmtfp.h"
static int do_vformat(struct outbuf *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { ob_put(o, *fmt); continue; }
        fmt++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = -1, prec = -1, lng = 0, sz = 0;
        for (;;) { if (*fmt == '-') left = 1; else if (*fmt == '+') plus = 1; else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1; else if (*fmt == '0') zero = 1; else break; fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else if (isdigit((unsigned char)*fmt)) { width = 0; while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0'); }
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; } else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        for (;;) { if (*fmt == 'l') lng++; else if (*fmt == 'h') lng--; else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') sz = 1; else if (*fmt == 'L' || *fmt == 'q') lng = 2; else break; fmt++; }
        char conv = *fmt; if (!conv) break;
        char tmp[80]; const char *s = tmp; int len = 0, lead0 = 0; char sign = 0; const char *prefix = ""; char *fbuf = NULL;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long v; int neg = 0, base = 10; const char *dig = "0123456789abcdef";
            if (conv == 'p') { v = (uintptr_t)va_arg(ap, void*); base = 16; prefix = "0x"; }
            else if (lng >= 2) { long long sv = va_arg(ap, long long); if (conv == 'd' || conv == 'i') { neg = sv < 0; v = neg ? -(unsigned long long)sv : sv; } else v = sv; }
            else { long sv = va_arg(ap, long); if (conv == 'd' || conv == 'i') { if (lng == -1) sv = (short)sv; else if (lng == -2) sv = (signed char)sv; neg = sv < 0; v = neg ? -(unsigned long)sv : (unsigned long)sv; }
                   else { unsigned long uv = sv; if (lng == -1) uv = (unsigned short)uv; else if (lng == -2) uv = (unsigned char)uv; v = uv; } }
            if (conv == 'x') base = 16; else if (conv == 'X') { base = 16; dig = "0123456789ABCDEF"; } else if (conv == 'o') base = 8;
            if (alt && conv == 'x' && v) prefix = "0x"; if (alt && conv == 'X' && v) prefix = "0X";
            int i = 79; tmp[i] = 0;
            if (v == 0 && prec == 0) { /* nothing */ } else do { tmp[--i] = dig[v % base]; v /= base; } while (v);
            if (prec > 0 && 79 - i < prec) lead0 = prec - (79 - i);   /* (v41, #336: written as they go out - a precision past 79 wrote below tmp) */
            if (alt && conv == 'o' && !lead0 && (i == 79 || tmp[i] != '0')) tmp[--i] = '0';   /* (v41, #336: %#o's leading 0, C99 7.19.6.1) */
            s = tmp + i; len = 79 - i;
            if (neg) sign = '-'; else if (plus && (conv == 'd' || conv == 'i')) sign = '+'; else if (space && (conv == 'd' || conv == 'i')) sign = ' ';
            if (prec >= 0) zero = 0;
            break; }
        case 'c': tmp[0] = (char)va_arg(ap, int); tmp[1] = 0; len = 1; break;
        case 's': s = va_arg(ap, const char*); if (!s) s = "(null)"; len = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s); break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A': {
            /* fmtfp.h: exact, as C99 and glibc (v41, #336 - the kernel's too) */
            double v = va_arg(ap, double);
            int up = conv >= 'A' && conv <= 'Z';
            uint64_t vb; memcpy(&vb, &v, 8);
            if (vb >> 63) { sign = '-'; vb &= ~(1ULL << 63); memcpy(&v, &vb, 8); } else if (plus) sign = '+'; else if (space) sign = ' ';
            if (((vb >> 52) & 0x7FF) == 0x7FF) { s = (vb & 0xFFFFFFFFFFFFFULL) ? (up ? "NAN" : "nan") : (up ? "INF" : "inf"); len = 3; zero = 0; break; }
            len = fmt_fp_dbl(tmp, (int)sizeof tmp, &fbuf, v, prec, alt, dos95_tolower(conv), up);
            if (fbuf) s = fbuf;
            if (conv == 'a' || conv == 'A') prefix = up ? "0X" : "0x";
            break; }
        case 'n': { int *ip = va_arg(ap, int*); *ip = o->len; continue; }
        case '%': tmp[0] = '%'; tmp[1] = 0; len = 1; break;
        default: tmp[0] = '%'; tmp[1] = conv; tmp[2] = 0; len = 2; break;
        }
        int total = len + lead0 + (sign ? 1 : 0) + strlen(prefix);
        int pad = width > total ? width - total : 0;
        if (!left && !zero) ob_pad(o, pad, ' ');
        if (sign) ob_put(o, sign);
        for (const char *q = prefix; *q; q++) ob_put(o, *q);
        if (!left && zero) ob_pad(o, pad, '0');
        ob_pad(o, lead0, '0');
        for (int i = 0; i < len; i++) ob_put(o, s[i]);
        if (left) ob_pad(o, pad, ' ');
        free(fbuf);
    }
    if (o->cap) o->p[o->len < o->cap ? o->len : o->cap - 1] = 0;
    return o->len;
}
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { struct outbuf o = { buf, n, 0 }; return do_vformat(&o, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r; }
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    /* (v41, #336, K29: all of it - cut at 1023 characters until v41) */
    char buf[1024]; va_list ap2; va_copy(ap2, ap);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    if (r >= (int)sizeof buf) {
        char *big = malloc(r + 1);
        if (big) { vsnprintf(big, r + 1, fmt, ap2); write(f->fd, big, r); free(big); }
        else write(f->fd, buf, sizeof buf - 1);
    } else if (r > 0) write(f->fd, buf, r);
    va_end(ap2);
    return r;
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2); va_end(ap2);
    *out = malloc(n + 1); if (!*out) return -1;
    return vsnprintf(*out, n + 1, fmt, ap);
}
int asprintf(char **out, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vasprintf(out, fmt, ap); va_end(ap); return r; }

/* sscanf: %d %i %u %x %X %o %s %c %n %[..] %*, with h/l/ll/z — what QEMU's option parsers use */
int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int count = 0; const char *s = str;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*fmt != '%') { if (*s != *fmt) break; s++; continue; }
        fmt++; int skip = 0, width = 0, lng = 0;
        if (*fmt == '*') { skip = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        for (;;) { if (*fmt == 'l') lng++; else if (*fmt == 'h') lng--; else if (*fmt == 'z') ; else break; fmt++; }
        char conv = *fmt; if (!conv) break;
        if (conv == 'n') { if (!skip) *va_arg(ap, int*) = s - str; continue; }
        if (conv == '%') { if (*s != '%') break; s++; continue; }
        if (conv != 'c' && conv != '[') while (isspace((unsigned char)*s)) s++;
        if (conv == 'c') { int w = width ? width : 1; if ((int)strlen(s) < w) break; if (!skip) { char *d = va_arg(ap, char*); memcpy(d, s, w); count++; } s += w; continue; }
        if (conv == 's' || conv == '[') {
            char set[256] = {0}; int neg = 0;
            if (conv == '[') { fmt++; if (*fmt == '^') { neg = 1; fmt++; } if (*fmt == ']') { set[(unsigned char)']'] = 1; fmt++; } while (*fmt && *fmt != ']') { if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') { for (int c = (unsigned char)fmt[0]; c <= (unsigned char)fmt[2]; c++) set[c] = 1; fmt += 3; } else set[(unsigned char)*fmt++] = 1; } }
            const char *start = s; int w = width ? width : 0x7fffffff;
            while (*s && w-- > 0) { int in = conv == 's' ? !isspace((unsigned char)*s) : (set[(unsigned char)*s] != neg); if (!in) break; s++; }
            if (s == start) break;
            if (!skip) { char *d = va_arg(ap, char*); memcpy(d, start, s - start); d[s - start] = 0; count++; }
            continue;
        }
        int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
        char *end; int neg, ok; unsigned long long v;
        if (width) { char tmp[64]; strlcpy(tmp, s, width + 1 < 64 ? width + 1 : 64); v = parse_ull(tmp, &end, base, &neg, &ok); end = (char*)s + (end - tmp); }
        else v = parse_ull(s, &end, base, &neg, &ok);
        if (!ok) break;
        s = end; if (neg) v = -v;
        if (!skip) {
            if (lng >= 2) *va_arg(ap, long long*) = v;
            else if (lng == -1) *va_arg(ap, short*) = v;
            else if (lng == -2) *va_arg(ap, char*) = v;
            else *va_arg(ap, int*) = v;
            count++;
        }
    }
    va_end(ap);
    return count == 0 && !*s ? -1 : count;
}

#ifdef QDP
/* ---- the QEMU DOS port: a machine's run is over (kernel/vpc.c vpc_thread_exit, on QEMU's thread, before it ends) ----
 * What a process's end would give back, given back: the atexit functions not run yet (a run that returned from main -
 * exit() runs them itself: the sound's close among them), the machine's disks closed as QEMU closes them (bdrv_close_all:
 * a qcow2 image's tables written), every file still open closed (DOS's handles are few), and every block of memory QEMU
 * still has freed.  QEMU's globals are put back before the next run (kernel/vpc.c), so nothing points at them then. */
void bdrv_close_all(void);
void qemu_dos95_run_over(void)
{
    while (natexit > 0) atexit_fns[--natexit]();
    bdrv_close_all();
    int files = 0;
    for (int i = 3; i < MAXFD; i++) if (fds[i].used) { fs_close(fds[i].dosh); fds[i].used = 0; files++; }
    uint32_t f = m_cli(); struct mhdr *h = mlist; mlist = NULL; m_sti(f);
    uint32_t blocks = 0, kb = 0;
    while (h) {
        struct mhdr *n = h->next;
        blocks++; kb += h->pages ? h->pages * 4 : (h->size + 1023) / 1024;
        h->magic = 0;
        if (h->pages) page_free((uint32_t)h->base, h->pages); else kfree(h->base);
        h = n;
    }
    kprintf("qemu: the run is over - %u blocks (%u KB) freed, %d files closed\n", blocks, kb, files);
}
#endif
