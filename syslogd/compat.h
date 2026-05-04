/*
 * Compatibility layer to replace busybox libbb for standalone syslogd.
 * Provides the minimal subset of libbb functions/macros used by syslogd.
 */
#ifndef SYSLOGD_COMPAT_H
#define SYSLOGD_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <getopt.h>
#include <netdb.h>

/*
 * CODE struct for syslog priority/facility name lookup.
 * We define our own copy instead of using glibc's SYSLOG_NAMES
 * to avoid multiple definition at link time.
 */
typedef struct _code {
    const char *c_name;
    int c_val;
} CODE;

/* Internal priority constants (normally gated by SYSLOG_NAMES) */
#ifndef INTERNAL_NOPRI
#define INTERNAL_NOPRI  0x10
#endif
#ifndef INTERNAL_MARK
#define INTERNAL_MARK   LOG_MAKEPRI(LOG_NFACILITIES << 3, 0)
#endif

/* These are defined in syslog_names.c */
extern const CODE *const bb_prioritynames;
extern const CODE *const bb_facilitynames;

/* ============================================================
 * Feature configuration - enable all features by default
 * ============================================================ */
#define ENABLE_FEATURE_ROTATE_LOGFILE       1
#define ENABLE_FEATURE_REMOTE_LOG           1
#define ENABLE_FEATURE_SYSLOGD_DUP          1
#define ENABLE_FEATURE_SYSLOGD_CFG          1
#define ENABLE_FEATURE_SYSLOGD_PRECISE_TIMESTAMPS 0
#define ENABLE_FEATURE_IPC_SYSLOG           1
#define ENABLE_FEATURE_KMSG_SYSLOG          1
#define ENABLE_FEATURE_KERNEL_LOG           1

#define CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE 256
#define CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE   16

#define IF_FEATURE_ROTATE_LOGFILE(x) x
#define IF_FEATURE_REMOTE_LOG(x)     x
#define IF_FEATURE_SYSLOGD_DUP(x)    x
#define IF_FEATURE_SYSLOGD_CFG(x)    x
#define IF_FEATURE_IPC_SYSLOG(x)     x
#define IF_FEATURE_KMSG_SYSLOG(x)    x
#define IF_NOT_FEATURE_SYSLOGD_CFG(x)

#define ENABLE_FEATURE_CLEAN_UP 1

/* ============================================================
 * Compiler attributes
 * ============================================================ */
#define NORETURN     __attribute__((__noreturn__))
#define NOINLINE     __attribute__((__noinline__))
#define FAST_FUNC    __attribute__((__used__))
#define EXTERNALLY_VISIBLE __attribute__((__externally_visible__))
#define MAIN_EXTERNALLY_VISIBLE
#define UNUSED_PARAM __attribute__((__unused__))
#define ALIGNED(x)   __attribute__((__aligned__(x)))

/* ============================================================
 * Global option mask (replaces busybox option_mask32)
 * ============================================================ */
extern uint32_t option_mask32;

/* ============================================================
 * Signal handling
 * ============================================================ */
extern volatile sig_atomic_t bb_got_signal;

void record_signo(int sig);
void kill_myself_with_sig(int sig) NORETURN;
void signal_no_SA_RESTART_empty_mask(int sig, void (*handler)(int));

/* ============================================================
 * Memory allocation wrappers (abort on failure)
 * ============================================================ */
void *xzalloc(size_t size);
char *xstrdup(const char *s);
char *xmalloc_follow_symlinks(const char *path);

/* ============================================================
 * I/O wrappers (abort on failure)
 * ============================================================ */
int xopen(const char *pathname, int flags);
int xsocket(int domain, int type, int protocol);
void xbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
void xmove_fd(int oldfd, int newfd);
ssize_t full_write(int fd, const void *buf, size_t count);
FILE *xfopen_for_read(const char *path);
FILE *fopen_for_read(const char *path);

/* ============================================================
 * String / number utilities
 * ============================================================ */
#define LONE_DASH(s) ((s)[0] == '-' && !(s)[1])
unsigned long bb_strtou(const char *arg, char **endp, int base);
char *safe_strncpy(char *dst, const char *src, size_t size);
char *safe_gethostname(void);

/* ============================================================
 * Error reporting
 * ============================================================ */
void bb_error_msg(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void bb_error_msg_and_die(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2))) NORETURN;
void bb_simple_error_msg(const char *s);
void bb_simple_error_msg_and_die(const char *s) NORETURN;
void bb_perror_msg(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void bb_perror_msg_and_die(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2))) NORETURN;
void bb_simple_perror_msg(const char *s);
void bb_simple_perror_msg_and_die(const char *s) NORETURN;

/* ============================================================
 * getopt32 - simplified busybox-style option parser
 * ============================================================ */
unsigned long getopt32(char **argv, const char *applet_opts, ...);

/* ============================================================
 * Daemon helpers
 * ============================================================ */
#define DAEMON_CHDIR_ROOT 1
#define DAEMON_ONLY_SANITIZE 2
void bb_daemonize_or_rexec(int flags, char **argv);

/* ============================================================
 * PID file helpers (no-op by default, simple implementation)
 * ============================================================ */
void write_pidfile_std_path_and_ext(const char *applet);
void remove_pidfile_std_path_and_ext(const char *applet);

/* ============================================================
 * Config file parser (simplified)
 * ============================================================ */
typedef struct parser_t {
    FILE *fp;
    int lineno;
} parser_t;

parser_t *config_open(const char *filename);
parser_t *config_open2(const char *filename,
        FILE *(*fopen_func)(const char *path));
void config_close(parser_t *parser);

#define PARSE_NORMAL    0x00
#define PARSE_MIN_DIE   0x01
int config_read(parser_t *parser, char **tokens, int max, int min,
                const char *delim, int flags);

unsigned long xatoul_range_wrapper(const char *arg, unsigned long lower, unsigned long upper);
unsigned long xatou_range_wrapper(const char *arg, unsigned long lower, unsigned long upper);

#define INIT_G() init_globals()
extern void init_globals(void);

/* ============================================================
 * Other utilities
 * ============================================================ */
unsigned monotonic_sec(void);
void xgettimeofday(struct timeval *tv);

#define get_linux_version_code() linux_version_code()
unsigned long linux_version_code(void);

#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))

/* device_open - simplified, just opens the device */
#define DEV_CONSOLE "/dev/console"
int device_open(const char *device, int flags);

/* common_bufsiz */
void setup_common_bufsiz(void);
extern char bb_common_bufsiz1[];

/* ============================================================
 * Globals pointer pattern (busybox uses ptr_to_globals)
 * ============================================================ */
struct globals;
extern struct globals *ptr_to_globals;
#define G (*ptr_to_globals)
#define SET_PTR_TO_GLOBALS(x) (ptr_to_globals = (struct globals *)(x))

/* ============================================================
 * Linked list (simplified)
 * ============================================================ */
typedef struct llist_t {
    struct llist_t *link;
    char *data;
} llist_t;

char *llist_pop(llist_t **elm);
void llist_add_to(llist_t **old_head, void *data);

/* ============================================================
 * Version
 * ============================================================ */
#define BB_VER "1.36.1-standalone"

#endif /* SYSLOGD_COMPAT_H */
