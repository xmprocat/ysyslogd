/*
 * Implementation of the busybox libbb compatibility layer.
 */
#include "compat.h"
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <syslog.h>

/* ============================================================
 * Global option mask
 * ============================================================ */
uint32_t option_mask32;

/* ============================================================
 * Signal handling
 * ============================================================ */
volatile sig_atomic_t bb_got_signal;

void record_signo(int sig)
{
    bb_got_signal = sig;
}

void kill_myself_with_sig(int sig)
{
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void signal_no_SA_RESTART_empty_mask(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    /* No SA_RESTART so that read() is interrupted */
    sigaction(sig, &sa, NULL);
}

/* ============================================================
 * Memory allocation
 * ============================================================ */
void *xzalloc(size_t size)
{
    void *p = calloc(1, size);
    if (!p) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return p;
}

char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) {
        bb_simple_error_msg_and_die("out of memory");
    }
    return p;
}

char *xmalloc_follow_symlinks(const char *path)
{
    /* Resolve symlinks using realpath, but return only the basename part */
    char resolved[PATH_MAX];
    /* Try realpath first */
    if (realpath(path, resolved)) {
        return xstrdup(resolved);
    }
    /* If path doesn't exist, just return a copy of the path */
    return NULL;
}

/* ============================================================
 * I/O wrappers
 * ============================================================ */
int xopen(const char *pathname, int flags)
{
    int fd = open(pathname, flags);
    if (fd < 0) {
        bb_perror_msg_and_die("can't open '%s'", pathname);
    }
    return fd;
}

int xsocket(int domain, int type, int protocol)
{
    int fd = socket(domain, type, protocol);
    if (fd < 0) {
        bb_simple_perror_msg_and_die("socket");
    }
    return fd;
}

void xbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (bind(sockfd, addr, addrlen) < 0) {
        bb_simple_perror_msg_and_die("bind");
    }
}

void xmove_fd(int oldfd, int newfd)
{
    if (oldfd != newfd) {
        if (dup2(oldfd, newfd) < 0) {
            bb_simple_perror_msg_and_die("dup2");
        }
        close(oldfd);
    }
}

ssize_t full_write(int fd, const void *buf, size_t count)
{
    ssize_t total = 0;
    while (count > 0) {
        ssize_t n = write(fd, buf, count);
        if (n < 0) {
            if (errno == EINTR) continue;
            return total ? total : -1;
        }
        if (n == 0) break;
        buf = (const char *)buf + n;
        count -= n;
        total += n;
    }
    return total;
}

FILE *xfopen_for_read(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        bb_perror_msg_and_die("can't open '%s' for reading", path);
    }
    return f;
}

FILE *fopen_for_read(const char *path)
{
    return fopen(path, "r");
}

/* ============================================================
 * String / number utilities
 * ============================================================ */
unsigned long bb_strtou(const char *arg, char **endp, int base)
{
    unsigned long v;
    char *tmp_end;
    errno = 0;
    v = strtoul(arg, &tmp_end, base);
    if (endp) *endp = tmp_end;
    return v;
}

char *safe_strncpy(char *dst, const char *src, size_t size)
{
    if (size == 0) return dst;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
    return dst;
}

char *safe_gethostname(void)
{
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        return xstrdup(buf);
    }
    return xstrdup("(none)");
}

/* ============================================================
 * Error reporting
 * ============================================================ */
void bb_error_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "syslogd: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void bb_error_msg_and_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "syslogd: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void bb_simple_error_msg(const char *s)
{
    fprintf(stderr, "syslogd: %s\n", s);
}

void bb_simple_error_msg_and_die(const char *s)
{
    bb_simple_error_msg(s);
    exit(1);
}

void bb_perror_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "syslogd: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
}

void bb_perror_msg_and_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "syslogd: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
    exit(1);
}

void bb_simple_perror_msg(const char *s)
{
    fprintf(stderr, "syslogd: %s: %s\n", s, strerror(errno));
}

void bb_simple_perror_msg_and_die(const char *s)
{
    bb_simple_perror_msg(s);
    exit(1);
}


/* ============================================================
 * Daemon helpers
 * ============================================================ */
void bb_daemonize_or_rexec(int flags, char **argv)
{
    pid_t pid;

    /* First fork: stop being session leader */
    pid = fork();
    if (pid < 0) {
        bb_perror_msg_and_die("fork");
    }
    if (pid > 0) {
        _exit(0); /* parent exits */
    }

    /* Become session leader */
    if (setsid() < 0) {
        bb_perror_msg_and_die("setsid");
    }

    /* Second fork: make sure we're never a session leader again */
    pid = fork();
    if (pid < 0) {
        bb_perror_msg_and_die("fork");
    }
    if (pid > 0) {
        _exit(0);
    }

    /* Redirect stdin/stdout/stderr to /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    if (flags & DAEMON_CHDIR_ROOT) {
        if (chdir("/") < 0) {}
    }
}

/* ============================================================
 * PID file
 * ============================================================ */
void write_pidfile_std_path_and_ext(const char *applet)
{
    char path[256];
    snprintf(path, sizeof(path), "/var/run/%s.pid", applet);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

void remove_pidfile_std_path_and_ext(const char *applet)
{
    char path[256];
    snprintf(path, sizeof(path), "/var/run/%s.pid", applet);
    unlink(path);
}

/* ============================================================
 * Config file parser (simplified)
 * ============================================================ */
parser_t *config_open(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) return NULL;
    parser_t *parser = xzalloc(sizeof(parser_t));
    parser->fp = fp;
    parser->lineno = 0;
    return parser;
}

parser_t *config_open2(const char *filename,
        FILE *(*fopen_func)(const char *path))
{
    FILE *fp = fopen_func(filename);
    if (!fp) return NULL;
    parser_t *parser = xzalloc(sizeof(parser_t));
    parser->fp = fp;
    parser->lineno = 0;
    return parser;
}

void config_close(parser_t *parser)
{
    if (parser) {
        if (parser->fp) fclose(parser->fp);
        free(parser);
    }
}

int config_read(parser_t *parser, char **tokens, int max, int min,
                const char *delim, int flags)
{
    char line[4096];
    int ntok = 0;

    if (!parser || !parser->fp) return 0;

    while (fgets(line, sizeof(line), parser->fp)) {
        parser->lineno++;

        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;

        /* Strip trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* Remove inline comments */
        char *cmt = strchr(p, '#');
        if (cmt) *cmt = '\0';

        /* Tokenize */
        char *saveptr;
        char *tok = strtok_r(p, delim ? delim : " \t", &saveptr);
        while (tok && ntok < max) {
            tokens[ntok] = xstrdup(tok);
            ntok++;
            tok = strtok_r(NULL, delim ? delim : " \t", &saveptr);
        }

        /* Clear remaining tokens */
        while (ntok < max) {
            tokens[ntok] = NULL;
            ntok++;
        }

        if (ntok < min && (flags & PARSE_MIN_DIE)) {
            /* Die on malformed lines */
            return 0;
        }

        return 1;
    }

    return 0;
}

/* ============================================================
 * Other utilities
 * ============================================================ */
unsigned monotonic_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (unsigned)ts.tv_sec;
    return (unsigned)time(NULL);
}

void xgettimeofday(struct timeval *tv)
{
    if (gettimeofday(tv, NULL) < 0) {
        bb_simple_perror_msg_and_die("gettimeofday");
    }
}

unsigned long linux_version_code(void)
{
    struct utsname u;
    unsigned int major, minor, patch;

    if (uname(&u) != 0)
        return 0;

    if (sscanf(u.release, "%u.%u.%u", &major, &minor, &patch) >= 2)
        return KERNEL_VERSION(major, minor, patch);

    return 0;
}

unsigned long xatoul_range_wrapper(const char *arg, unsigned long lower, unsigned long upper)
{
    char *end;
    unsigned long v = strtoul(arg, &end, 10);
    if (*end != '\0' || v < lower || v > upper) {
        bb_error_msg_and_die("bad value '%s' (expected %lu-%lu)", arg, lower, upper);
    }
    return v;
}

unsigned long xatou_range_wrapper(const char *arg, unsigned long lower, unsigned long upper)
{
    return xatoul_range_wrapper(arg, lower, upper);
}

/* strchrnul - GNU extension, may not be available everywhere */
static char *my_strchrnul(const char *s, int c)
{
    while (*s && *s != c)
        s++;
    return (char *)s;
}
#define strchrnul my_strchrnul

int device_open(const char *device, int flags)
{
    return open(device, flags);
}

/* ============================================================
 * Common buffer (simplified - 1KB static buffer)
 * ============================================================ */
#define COMMON_BUFSIZE 1024
char bb_common_bufsiz1[COMMON_BUFSIZE] ALIGNED(sizeof(long long));

void setup_common_bufsiz(void)
{
    /* No-op for standalone version */
}

/* ============================================================
 * Globals pointer
 * ============================================================ */
struct globals *ptr_to_globals;

/* ============================================================
 * Linked list
 * ============================================================ */
char *llist_pop(llist_t **elm)
{
    llist_t *item = *elm;
    char *data;

    if (!item) return NULL;
    data = item->data;
    *elm = item->link;
    free(item);
    return data;
}

void llist_add_to(llist_t **old_head, void *data)
{
    llist_t *new_item = xzalloc(sizeof(llist_t));
    new_item->data = (char *)data;
    new_item->link = *old_head;
    *old_head = new_item;
}
