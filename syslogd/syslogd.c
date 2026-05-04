/* vi: set sw=4 ts=4: */
/*
 * Standalone syslogd - ring buffer, TCP/UDP, poll-based event loop.
 * Adapted from busybox 1.36.1 sysklogd.
 *
 * Licensed under GPLv2 or later.
 */
#include "compat.h"
#include <syslog.h>

#include <fcntl.h>
#include <netdb.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/if_packet.h>

#include <sys/un.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/klog.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#ifndef COMPILE_TIME
#define COMPILE_TIME __DATE__ " " __TIME__
#endif

#define DEBUG 0

#undef SYSLOGD_MARK

#define MAX_READ CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE
#define DNS_WAIT_SEC (2 * 60)

/* ============================================================
 * Ring Buffer (fixed-slot design for multi-consumer)
 * ============================================================ */
#define RINGBUF_MAGIC       0x524C4742
#define RINGBUF_VERSION     1
#define RINGBUF_MAX_ENTRIES 512
#define RINGBUF_MSG_SIZE    1024

struct ringbuf_slot {
    volatile uint64_t seq;
    uint32_t          len;
    char              msg[RINGBUF_MSG_SIZE - 12];
};

struct ringbuf_hdr {
    uint32_t          magic;
    uint32_t          version;
    uint32_t          num_entries;
    volatile uint64_t producer_seq;
    volatile uint64_t file_seq;
    volatile uint64_t remote_seq;
    uint64_t          _pad[5];
    struct ringbuf_slot slots[];
};

enum remote_proto { REMOTE_UDP, REMOTE_TCP };

typedef struct {
    int fd;
    enum remote_proto proto;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    const char *hostname;
    unsigned last_reconnect;
} remoteHost_t;

typedef struct tcp_client {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    char rbuf[2048];
    int rbuf_pos;
    struct tcp_client *next;
} tcp_client_t;

typedef struct logFile_t {
    const char *path;
    int fd;
    time_t last_log_time;
    unsigned size;
    uint8_t isRegular;
} logFile_t;

typedef struct syslog_conf_t {
    char *log_file;
    int log_level;
    unsigned log_file_size_kb;
    unsigned log_file_rotate;
    char *remote;
    int server_port;
    int kernel_log;
    int kmsg;
} syslog_conf_t;

typedef struct logRule_t {
    uint8_t enabled_facility_priomap[LOG_NFACILITIES];
    struct logFile_t *file;
    struct logRule_t *next;
} logRule_t;

/* Global state */
#define GLOBALS \
    logFile_t logFile;                  \
    int logLevel;                       \
    unsigned logFileSize;               \
    unsigned logFileRotate;             \
    int shmid;                          \
    int shm_size;                       \
    logRule_t *log_rules;               \
    int kmsgfd;                         \
    int primask;                        \
    struct ringbuf_hdr *ringbuf;        \
    uint64_t ringbuf_file_seq;          \
    uint64_t ringbuf_remote_seq;        \
    int tcp_listen_fd;                  \
    int udp_listen_fd;                  \
    int syslog_server_port;             \
    int ctl_listen_fd;                  \
    char *conf_file;                    \
    char *ctl_sock_path;                \
    syslog_conf_t conf;                 \
    tcp_client_t *tcp_clients;

struct init_globals {
    GLOBALS
};

struct globals {
    GLOBALS
    llist_t *remoteHosts;
    char *hostname;
    char recvbuf[MAX_READ * 2];
    char parsebuf[MAX_READ * 2];
    char printbuf[MAX_READ * 2 + 128];
};

const struct init_globals init_data = {
    .logFile = { .path = "/var/log/messages", .fd = -1 },
    .logLevel = 8,
    .logFileSize = 200 * 1024,
    .logFileRotate = 1,
    .shmid = -1,
    .shm_size = (CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE * 1024),
    .tcp_listen_fd = -1,
    .udp_listen_fd = -1,
    .ctl_listen_fd = -1,
};

void init_globals(void)
{
    ptr_to_globals = (struct globals *)xzalloc(sizeof(struct globals));
    memcpy(ptr_to_globals, &init_data, sizeof(init_data));
}

/* Options */
enum {
    OPTBIT_nofork = 0,
    OPTBIT_outfile,
    OPTBIT_loglevel,
    OPTBIT_filesize,
    OPTBIT_rotatecnt,
    OPTBIT_remotelog,
    OPTBIT_dup,
    OPTBIT_cfg,
    OPTBIT_kmsg,
    OPTBIT_kernel_log,
    OPTBIT_syslog_server,
    OPTBIT_viewlog,
    OPTBIT_follow,

    OPT_nofork         = 1 << OPTBIT_nofork,
    OPT_outfile        = 1 << OPTBIT_outfile,
    OPT_loglevel       = 1 << OPTBIT_loglevel,
    OPT_filesize       = 1 << OPTBIT_filesize,
    OPT_rotatecnt      = 1 << OPTBIT_rotatecnt,
    OPT_remotelog      = 1 << OPTBIT_remotelog,
    OPT_dup            = 1 << OPTBIT_dup,
    OPT_cfg            = 1 << OPTBIT_cfg,
    OPT_kmsg           = 1 << OPTBIT_kmsg,
    OPT_kernel_log     = 1 << OPTBIT_kernel_log,
    OPT_syslog_server  = 1 << OPTBIT_syslog_server,
    OPT_viewlog        = 1 << OPTBIT_viewlog,
    OPT_follow         = 1 << OPTBIT_follow,
};


static const CODE* find_by_name(const char *name, const CODE* c_set)
{
    for (; c_set->c_name; c_set++) {
        if (strcmp(name, c_set->c_name) == 0)
            return c_set;
    }
    return NULL;
}

static const CODE* find_by_val(int val, const CODE* c_set)
{
    for (; c_set->c_name; c_set++) {
        if (c_set->c_val == val)
            return c_set;
    }
    return NULL;
}

static void parse_syslogdcfg(const char *file)
{
    char *t;
    logRule_t **pp_rule;
    char *tok[8];
    parser_t *parser;

    parser = config_open2(file ? file : "/etc/syslog.conf",
                file ? xfopen_for_read : fopen_for_read);
    if (!parser)
        return;

    pp_rule = &G.log_rules;
    while (config_read(parser, tok, 8, 2, "# \t", PARSE_NORMAL | PARSE_MIN_DIE)) {
        char *cur_selector;
        logRule_t *cur_rule;

        if (!tok[1])
            goto cfgerr;

        cur_rule = *pp_rule = xzalloc(sizeof(*cur_rule));

        cur_selector = tok[0];
        do {
            const CODE *code;
            char *next_selector;
            uint8_t negated_prio;
            uint8_t single_prio;
            uint32_t facmap;
            uint8_t primap;
            unsigned i;

            next_selector = strchr(cur_selector, ';');
            if (next_selector)
                *next_selector++ = '\0';

            t = strchr(cur_selector, '.');
            if (!t)
                goto cfgerr;
            *t++ = '\0';

            negated_prio = 0;
            single_prio = 0;
            if (*t == '!') { negated_prio = 1; ++t; }
            if (*t == '=') { single_prio = 1; ++t; }

            if (*t == '*')
                primap = 0xff;
            else {
                uint8_t priority;
                code = find_by_name(t, bb_prioritynames);
                if (!code)
                    goto cfgerr;
                primap = 0;
                priority = code->c_val;
                if (priority == INTERNAL_NOPRI) {
                    negated_prio = 1;
                } else {
                    priority = 1 << priority;
                    do {
                        primap |= priority;
                        if (single_prio)
                            break;
                        priority >>= 1;
                    } while (priority);
                    if (negated_prio)
                        primap = ~primap;
                }
            }

            if (*cur_selector == '*')
                facmap = (1<<LOG_NFACILITIES) - 1;
            else {
                char *next_facility;
                facmap = 0;
                t = cur_selector;
                do {
                    next_facility = strchr(t, ',');
                    if (next_facility)
                        *next_facility++ = '\0';
                    code = find_by_name(t, bb_facilitynames);
                    if (!code)
                        goto cfgerr;
                    if (code->c_val != INTERNAL_MARK)
                        facmap |= 1<<(LOG_FAC(code->c_val));
                    t = next_facility;
                } while (t);
            }

            for (i = 0; i < LOG_NFACILITIES; ++i) {
                if (!(facmap & (1<<i)))
                    continue;
                if (negated_prio)
                    cur_rule->enabled_facility_priomap[i] &= primap;
                else
                    cur_rule->enabled_facility_priomap[i] |= primap;
            }

            cur_selector = next_selector;
        } while (cur_selector);

        if (strcmp(G.logFile.path, tok[1]) == 0) {
            cur_rule->file = &G.logFile;
            goto found;
        }
        for (cur_rule = G.log_rules; cur_rule != *pp_rule; cur_rule = cur_rule->next) {
            if (strcmp(cur_rule->file->path, tok[1]) == 0) {
                (*pp_rule)->file = cur_rule->file;
                cur_rule = *pp_rule;
                goto found;
            }
        }
        cur_rule->file = xzalloc(sizeof(*cur_rule->file));
        cur_rule->file->fd = -1;
        cur_rule->file->path = xstrdup(tok[1]);
 found:
        pp_rule = &cur_rule->next;
        for (int i = 0; i < 8; i++) {
            if (tok[i]) { free(tok[i]); tok[i] = NULL; }
        }
    }
    config_close(parser);
    return;

 cfgerr:
    for (int i = 0; i < 8; i++) {
        if (tok[i]) free(tok[i]);
    }
    config_close(parser);
    bb_error_msg_and_die("error in '%s' at line %d",
            file ? file : "/etc/syslog.conf",
            parser->lineno);
}

enum { KEY_ID = 0x414e4547 };

static int ringbuf_init(void)
{
    int sz = sizeof(struct ringbuf_hdr) + RINGBUF_MAX_ENTRIES * sizeof(struct ringbuf_slot);

    G.shmid = shmget(KEY_ID, sz, IPC_CREAT | 0644);
    if (G.shmid == -1) {
        bb_simple_perror_msg_and_die("shmget");
    }

    G.ringbuf = (struct ringbuf_hdr *)shmat(G.shmid, NULL, 0);
    if (G.ringbuf == (void*)-1L) {
        bb_simple_perror_msg_and_die("shmat");
    }

    if (G.ringbuf->magic != RINGBUF_MAGIC || G.ringbuf->version != RINGBUF_VERSION) {
        memset(G.ringbuf, 0, sz);
        G.ringbuf->magic = RINGBUF_MAGIC;
        G.ringbuf->version = RINGBUF_VERSION;
        G.ringbuf->num_entries = RINGBUF_MAX_ENTRIES;
        G.ringbuf->producer_seq = 1;
        G.ringbuf->file_seq = 1;
        G.ringbuf->remote_seq = 1;
    }

    /* Recover consumer positions from shared memory (survives crash).
     * Clamp to valid range: 0 or >producer_seq means uninitialized. */
    G.ringbuf_file_seq = G.ringbuf->file_seq;
    if (G.ringbuf_file_seq == 0 || G.ringbuf_file_seq > G.ringbuf->producer_seq)
        G.ringbuf_file_seq = G.ringbuf->producer_seq;

    G.ringbuf_remote_seq = G.ringbuf->remote_seq;
    if (G.ringbuf_remote_seq == 0 || G.ringbuf_remote_seq > G.ringbuf->producer_seq)
        G.ringbuf_remote_seq = G.ringbuf->producer_seq;

    return 0;
}

static void ringbuf_cleanup(void)
{
    if (G.shmid != -1) {
        shmdt(G.ringbuf);
        struct shmid_ds ds;
        if (shmctl(G.shmid, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0) {
            shmctl(G.shmid, IPC_RMID, NULL);
        }
    }
}

static void ringbuf_produce(const char *msg, int len)
{
    struct ringbuf_hdr *rb = G.ringbuf;
    uint64_t seq = rb->producer_seq;
    uint32_t idx = (seq - 1) % rb->num_entries;
    struct ringbuf_slot *slot = &rb->slots[idx];

    if (len > (int)(sizeof(slot->msg) - 1))
        len = sizeof(slot->msg) - 1;

    slot->seq = 0;
    __sync_synchronize();

    memcpy(slot->msg, msg, len);
    slot->msg[len] = '\0';
    slot->len = len + 1;

    __sync_synchronize();
    slot->seq = seq;
    rb->producer_seq = seq + 1;
}

static int ringbuf_consume(struct ringbuf_hdr *rb, uint64_t *next_seq,
                           char *out, int out_size)
{
    uint64_t ns = *next_seq;
    uint32_t idx = (ns - 1) % rb->num_entries;
    struct ringbuf_slot *slot = &rb->slots[idx];
    uint64_t s = slot->seq;

    if (s == 0 || s < ns) {
        return 0;
    }
    if (s > ns) {
        *next_seq = s;
        ns = s;
        idx = (ns - 1) % rb->num_entries;
        slot = &rb->slots[idx];
    }

    int len = slot->len;
    if (len > out_size) len = out_size;
    memcpy(out, slot->msg, len);
    (*next_seq)++;
    return 1;
}


/* ============================================================
 * Runtime Configuration File I/O
 * ============================================================ */

static void syslog_conf_free(syslog_conf_t *conf)
{
    free(conf->log_file);
    conf->log_file = NULL;
    free(conf->remote);
    conf->remote = NULL;
}

static int syslog_conf_read(const char *path, syslog_conf_t *out)
{
    FILE *fp;
    char line[512];
    char *p, *key, *value;

    memset(out, 0, sizeof(*out));

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        key = p;
        p = strchr(p, '=');
        if (!p) continue;
        *p++ = '\0';
        value = p;

        while (value[0] == ' ' || value[0] == '\t') value++;
        p = value + strlen(value) - 1;
        while (p >= value && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            { *p = '\0'; p--; }
        p = key + strlen(key) - 1;
        while (p >= key && (*p == ' ' || *p == '\t'))
            { *p = '\0'; p--; }

        if (strcmp(key, "log_file") == 0 && value[0]) {
            free(out->log_file);
            out->log_file = xstrdup(value);
        } else if (strcmp(key, "log_level") == 0) {
            out->log_level = atoi(value);
            if (out->log_level < 1) out->log_level = 1;
            if (out->log_level > 8) out->log_level = 8;
        } else if (strcmp(key, "log_size") == 0) {
            out->log_file_size_kb = atoi(value);
        } else if (strcmp(key, "log_rotate") == 0) {
            out->log_file_rotate = atoi(value);
            if (out->log_file_rotate > 99) out->log_file_rotate = 99;
        } else if (strcmp(key, "remote") == 0) {
            free(out->remote);
            out->remote = (value[0]) ? xstrdup(value) : NULL;
        } else if (strcmp(key, "server_port") == 0) {
            out->server_port = atoi(value);
            if (out->server_port < 0 || out->server_port > 65535) out->server_port = 0;
        } else if (strcmp(key, "kernel_log") == 0) {
            out->kernel_log = atoi(value) ? 1 : 0;
        } else if (strcmp(key, "kmsg") == 0) {
            out->kmsg = atoi(value) ? 1 : 0;
        }
    }

    fclose(fp);
    return 0;
}

static int syslog_conf_write(const char *path, const syslog_conf_t *conf)
{
    char tmp[280];
    FILE *fp;
    int n;

    n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || n >= (int)sizeof(tmp)) return -1;

    fp = fopen(tmp, "w");
    if (!fp) return -1;

    fprintf(fp, "# syslogd runtime configuration\n");
    if (conf->log_file) fprintf(fp, "log_file=%s\n", conf->log_file);
    fprintf(fp, "log_level=%d\n", conf->log_level);
    fprintf(fp, "log_size=%u\n", conf->log_file_size_kb);
    fprintf(fp, "log_rotate=%u\n", conf->log_file_rotate);
    if (conf->remote) fprintf(fp, "remote=%s\n", conf->remote);
    fprintf(fp, "server_port=%d\n", conf->server_port);
    fprintf(fp, "kernel_log=%d\n", conf->kernel_log);
    fprintf(fp, "kmsg=%d\n", conf->kmsg);

    fclose(fp);

    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void syslog_conf_snapshot(syslog_conf_t *conf)
{
    syslog_conf_free(conf);

    conf->log_file = xstrdup(G.logFile.path);
    conf->log_level = G.logLevel;
    conf->log_file_size_kb = G.logFileSize / 1024;
    conf->log_file_rotate = G.logFileRotate;
    conf->server_port = G.syslog_server_port;
    conf->kernel_log = (option_mask32 & OPT_kernel_log) ? 1 : 0;
    conf->kmsg = (option_mask32 & OPT_kmsg) ? 1 : 0;

    /* Snapshot single remote host */
    if (G.remoteHosts) {
        remoteHost_t *rh = (remoteHost_t*)G.remoteHosts->data;
        char buf[280];
        const char *proto = (rh->proto == REMOTE_TCP) ? "tcp://" : "udp://";
        snprintf(buf, sizeof(buf), "%s%s", proto, rh->hostname);
        conf->remote = xstrdup(buf);
    } else {
        conf->remote = NULL;
    }
}


static void kmsg_init(void)
{
    G.kmsgfd = xopen("/dev/kmsg", O_WRONLY);
    if (get_linux_version_code() < KERNEL_VERSION(3,5,0))
        G.primask = LOG_PRIMASK;
    else
        G.primask = -1;
}

static void kmsg_cleanup(void)
{
    if (ENABLE_FEATURE_CLEAN_UP)
        close(G.kmsgfd);
}

static void log_to_kmsg(int pri, const char *msg)
{
    pri &= G.primask;
    full_write(G.kmsgfd, G.printbuf,
        sprintf(G.printbuf, "<%d>%s\n", pri, msg));
}

static void log_locally(char *msg, logFile_t *log_file)
{
    int len = strlen(msg);

    if (log_file->fd > 1) {
    } else if (log_file->fd == 1) {
    } else {
        if (LONE_DASH(log_file->path)) {
            log_file->fd = 1;
            log_file->isRegular = 0;
        } else {
            log_file->fd = open(log_file->path,
                O_WRONLY | O_CREAT | O_NOCTTY | O_APPEND | O_NONBLOCK, 0666);
            if (log_file->fd < 0) {
                int fd = device_open(DEV_CONSOLE, O_WRONLY | O_NOCTTY | O_NONBLOCK);
                if (fd < 0) fd = 2;
                full_write(fd, msg, len);
                if (fd != 2) close(fd);
                return;
            }
            struct stat statf;
            log_file->isRegular = (fstat(log_file->fd, &statf) == 0 && S_ISREG(statf.st_mode));
            log_file->size = statf.st_size;
        }
    }

    if (G.logFileSize && log_file->isRegular && log_file->size > G.logFileSize) {
        if (G.logFileRotate) {
            int i = strlen(log_file->path) + 3 + 1;
            char oldFile[i], newFile[i];
            i = G.logFileRotate - 1;
            while (1) {
                sprintf(newFile, "%s.%d", log_file->path, i);
                if (i == 0) break;
                sprintf(oldFile, "%s.%d", log_file->path, --i);
                rename(oldFile, newFile);
            }
            rename(log_file->path, newFile);
        }
        unlink(log_file->path);
        close(log_file->fd);
        log_file->fd = open(log_file->path,
            O_WRONLY | O_CREAT | O_NOCTTY | O_APPEND | O_NONBLOCK, 0666);
        if (log_file->fd < 0) return;
        struct stat statf;
        log_file->size = (fstat(log_file->fd, &statf) == 0) ? statf.st_size : 0;
    }

    len = full_write(log_file->fd, msg, len);
    if (len > 0)
        log_file->size += len;
}

static void parse_fac_prio_20(int pri, char *res20)
{
    const CODE *c_pri, *c_fac;
    c_fac = find_by_val(LOG_FAC(pri) << 3, bb_facilitynames);
    if (c_fac) {
        c_pri = find_by_val(LOG_PRI(pri), bb_prioritynames);
        if (c_pri) {
            snprintf(res20, 20, "%s.%s", c_fac->c_name, c_pri->c_name);
            return;
        }
    }
    snprintf(res20, 20, "<%d>", pri);
}

static char *format_message(int pri, char *msg)
{
    time_t now;
    char *timestamp;
    char res[20];

    time(&now);
    timestamp = ctime(&now) + 4;
    timestamp[15] = '\0';

    parse_fac_prio_20(pri, res);
    sprintf(G.printbuf, "%s %.64s %s %s\n", timestamp, G.hostname, res, msg);
    return G.printbuf;
}

static void timestamp_and_log_internal(const char *msg)
{
    char *formatted = format_message(LOG_SYSLOG | LOG_INFO, (char*)msg);
    if (G.ringbuf)
        ringbuf_produce(formatted, strlen(formatted));
}

static void process_raw_message(char *tmpbuf, int len)
{
    char *p = tmpbuf;
    tmpbuf += len;

    while (p < tmpbuf) {
        char c;
        char *q = G.parsebuf;
        int pri = (LOG_USER | LOG_NOTICE);

        if (*p == '<') {
            pri = bb_strtou(p + 1, &p, 10);
            if (*p == '>') p++;
            if (pri & ~(LOG_FACMASK | LOG_PRIMASK))
                pri = (LOG_USER | LOG_NOTICE);
        }

        /* Strip client timestamp (Mmm DD HH:MM:SS ) */
        if (strlen(p) >= 16 && p[3] == ' ' && p[6] == ' '
            && p[9] == ':' && p[12] == ':' && p[15] == ' ') {
            p += 16;
        }

        while ((c = *p++)) {
            if (c == '\n') c = ' ';
            if (!(c & ~0x1f) && c != '\t') {
                *q++ = '^';
                c += '@';
            }
            *q++ = c;
        }
        *q = '\0';

        char *formatted = format_message(pri, G.parsebuf);

        if (option_mask32 & OPT_kmsg) {
            log_to_kmsg(pri, G.parsebuf);
        }

        if (G.ringbuf) {
            ringbuf_produce(formatted, strlen(formatted));
        } else {
            log_locally(formatted, &G.logFile);
        }
    }
}


static NOINLINE int create_socket(void)
{
    struct sockaddr_un sunx;
    int sock_fd;
    char *dev_log_name;

    memset(&sunx, 0, sizeof(sunx));
    sunx.sun_family = AF_UNIX;
    strcpy(sunx.sun_path, "/dev/log");

    dev_log_name = xmalloc_follow_symlinks("/dev/log");
    if (dev_log_name) {
        safe_strncpy(sunx.sun_path, dev_log_name, sizeof(sunx.sun_path));
        free(dev_log_name);
    }
    unlink(sunx.sun_path);

    sock_fd = xsocket(AF_UNIX, SOCK_DGRAM, 0);
    xbind(sock_fd, (struct sockaddr *) &sunx, sizeof(sunx));
    chmod("/dev/log", 0666);

    return sock_fd;
}

static int remote_resolve(remoteHost_t *rh)
{
    unsigned now = monotonic_sec();
    if ((now - rh->last_reconnect) < DNS_WAIT_SEC)
        return -1;
    rh->last_reconnect = now;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (rh->proto == REMOTE_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = (rh->proto == REMOTE_TCP) ? IPPROTO_TCP : IPPROTO_UDP;

    char *host = xstrdup(rh->hostname);
    char *port = strchr(host, ':');
    if (port) *port++ = '\0'; else port = "514";

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        free(host);
        return -1;
    }
    free(host);
    memcpy(&rh->addr, res->ai_addr, res->ai_addrlen);
    rh->addr_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int remote_connect(remoteHost_t *rh)
{
    if (rh->fd >= 0)
        return rh->fd;

    if (!rh->addr_len) {
        if (remote_resolve(rh) < 0)
            return -1;
    }

    rh->fd = socket(rh->addr.ss_family,
        (rh->proto == REMOTE_TCP) ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (rh->fd < 0) return -1;

    if (rh->proto == REMOTE_TCP) {
        int flags = fcntl(rh->fd, F_GETFL, 0);
        fcntl(rh->fd, F_SETFL, flags | O_NONBLOCK);

        if (connect(rh->fd, (struct sockaddr*)&rh->addr, rh->addr_len) < 0) {
            if (errno != EINPROGRESS) {
                close(rh->fd); rh->fd = -1; rh->addr_len = 0;
                return -1;
            }
        }
        int one = 1;
        setsockopt(rh->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    } else {
        connect(rh->fd, (struct sockaddr*)&rh->addr, rh->addr_len);
    }
    return rh->fd;
}

static void remote_send_one(remoteHost_t *rh, const char *msg, int len)
{
    int fd = remote_connect(rh);
    if (fd < 0) return;

    ssize_t n = send(fd, msg, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        switch (errno) {
        case EAGAIN:
        case EINTR:
            break;
        case ECONNRESET:
        case ENOTCONN:
        case EPIPE:
        case ETIMEDOUT:
            close(rh->fd); rh->fd = -1; rh->addr_len = 0;
            break;
        }
    }
}

static int syslog_server_init(int port)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            G.udp_listen_fd = fd;
        } else {
            close(fd);
        }
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            listen(fd, 16);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            G.tcp_listen_fd = fd;
        } else {
            close(fd);
        }
    }

    return (G.tcp_listen_fd >= 0 || G.udp_listen_fd >= 0) ? 0 : -1;
}

static void syslog_server_accept(void)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(G.tcp_listen_fd, (struct sockaddr*)&addr, &addr_len);
    if (fd < 0) return;

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    tcp_client_t *cl = xzalloc(sizeof(*cl));
    cl->fd = fd;
    memcpy(&cl->addr, &addr, sizeof(addr));
    cl->addr_len = addr_len;
    cl->next = G.tcp_clients;
    G.tcp_clients = cl;
}

static void syslog_server_recv_udp(void)
{
    char buf[2048];
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ssize_t n;

    while ((n = recvfrom(G.udp_listen_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                          (struct sockaddr*)&from, &fromlen)) > 0) {
        buf[n] = '\0';
        process_raw_message(buf, n);
    }
}

static void syslog_server_recv_tcp(tcp_client_t *cl, tcp_client_t **prev_ptr)
{
    char buf[2048];
    ssize_t n;

    n = recv(cl->fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (n <= 0) {
        close(cl->fd);
        *prev_ptr = cl->next;
        free(cl);
        return;
    }

    int i;
    for (i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || (c == '<' && cl->rbuf_pos > 0)) {
            if (cl->rbuf_pos > 0) {
                cl->rbuf[cl->rbuf_pos] = '\0';
                process_raw_message(cl->rbuf, cl->rbuf_pos);
                cl->rbuf_pos = 0;
            }
            if (c == '\n') continue;
        }
        if (cl->rbuf_pos < (int)(sizeof(cl->rbuf) - 1))
            cl->rbuf[cl->rbuf_pos++] = c;
    }
    if (cl->rbuf_pos > 0 && cl->rbuf[cl->rbuf_pos-1] == '\n')
        cl->rbuf_pos--;
}


#if ENABLE_FEATURE_KERNEL_LOG
static int klog_fd = -1;

static void kernel_log_init(void)
{
    klog_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (klog_fd >= 0) return;
    klogctl(1, NULL, 0);
}

static int kernel_log_read(char *buf, int len)
{
    if (klog_fd >= 0)
        return read(klog_fd, buf, len - 1);
    return klogctl(2, buf, len);
}

static void kernel_log_close(void)
{
    if (klog_fd >= 0) { close(klog_fd); klog_fd = -1; }
    else { klogctl(7, NULL, 0); klogctl(0, NULL, 0); }
}

static void kernel_log_process(void)
{
    char buf[4096];
    int n;

    while ((n = kernel_log_read(buf, sizeof(buf))) > 0) {
        char *p = buf;
        char *q;
        int pri = LOG_KERN | LOG_NOTICE;

        if (klog_fd >= 0) {
            char *semi = strchr(p, ';');
            if (semi) {
                *semi = 0;
                pri = LOG_KERN | (atoi(p) & LOG_PRIMASK);
                p = semi + 1;
            }
        } else if (*p == '<') {
            pri = LOG_KERN | (strtoul(p + 1, &p, 10) & LOG_PRIMASK);
            if (*p == '>') p++;
        }

        for (q = p; *q; q++) { if (*q == '\n') *q = ' '; }

        if (*p) {
            char *formatted = format_message(pri, p);
            if (G.ringbuf)
                ringbuf_produce(formatted, strlen(formatted));
            else
                log_locally(formatted, &G.logFile);
        }
    }
}
#else
static void kernel_log_init(void) {}
static void kernel_log_close(void) {}
static void kernel_log_process(void) {}
#endif


/* ============================================================
 * Runtime Configuration Apply
 * ============================================================ */

static void remote_close(remoteHost_t *rh)
{
    if (rh->fd >= 0) { close(rh->fd); rh->fd = -1; }
    rh->addr_len = 0;
}

static remoteHost_t *remote_create(const char *addr_str)
{
    remoteHost_t *rh = xzalloc(sizeof(*rh));

    if (strncmp(addr_str, "tcp://", 6) == 0) {
        rh->proto = REMOTE_TCP;
        rh->hostname = xstrdup(addr_str + 6);
    } else if (strncmp(addr_str, "udp://", 6) == 0) {
        rh->proto = REMOTE_UDP;
        rh->hostname = xstrdup(addr_str + 6);
    } else {
        rh->proto = REMOTE_UDP;
        rh->hostname = xstrdup(addr_str);
    }
    rh->fd = -1;
    rh->last_reconnect = monotonic_sec() - DNS_WAIT_SEC - 1;
    return rh;
}

static void remote_recreate(const char *remote_addr)
{
    /* Close and free existing remote host */
    if (G.remoteHosts) {
        remoteHost_t *rh = (remoteHost_t*)llist_pop(&G.remoteHosts);
        remote_close(rh);
        free((char*)rh->hostname);
        free(rh);
    }

    if (!remote_addr || !remote_addr[0])
        return;

    remoteHost_t *rh = remote_create(remote_addr);
    llist_add_to(&G.remoteHosts, rh);
}

/* Apply a single key=value change at runtime.
 * Returns 0 on success, -1 on unknown key or invalid value. */
static int conf_apply_one(const char *key, const char *value)
{
    if (strcmp(key, "log_level") == 0) {
        int v = atoi(value);
        if (v < 1) v = 1;
        if (v > 8) v = 8;
        if (v != G.logLevel) {
            G.logLevel = v;
            bb_error_msg("log_level changed to %d", v);
        }
    } else if (strcmp(key, "log_size") == 0) {
        unsigned v = (unsigned)atoi(value);
        G.logFileSize = v * 1024;
    } else if (strcmp(key, "log_rotate") == 0) {
        unsigned v = (unsigned)atoi(value);
        if (v > 99) v = 99;
        G.logFileRotate = v;
    } else if (strcmp(key, "log_file") == 0) {
        if (!value[0]) return -1;
        if (G.logFile.fd > 1) {
            close(G.logFile.fd);
            G.logFile.fd = -1;
        }
        G.logFile.path = xstrdup(value);
        G.logFile.size = 0;
        bb_error_msg("log_file changed to %s", value);
    } else if (strcmp(key, "kernel_log") == 0) {
        int v = atoi(value) ? 1 : 0;
        int cur = (option_mask32 & OPT_kernel_log) ? 1 : 0;
        if (v != cur) {
            if (v) {
#if ENABLE_FEATURE_KERNEL_LOG
                kernel_log_init();
#endif
                option_mask32 |= OPT_kernel_log;
            } else {
#if ENABLE_FEATURE_KERNEL_LOG
                kernel_log_close();
#endif
                option_mask32 &= ~OPT_kernel_log;
            }
            bb_error_msg("kernel_log changed to %d", v);
        }
    } else if (strcmp(key, "kmsg") == 0) {
        int v = atoi(value) ? 1 : 0;
        int cur = (option_mask32 & OPT_kmsg) ? 1 : 0;
        if (v != cur) {
            if (v) {
                if (G.kmsgfd < 0) {
                    G.kmsgfd = open("/dev/kmsg", O_WRONLY);
                    if (G.kmsgfd >= 0) {
                        if (get_linux_version_code() < KERNEL_VERSION(3,5,0))
                            G.primask = LOG_PRIMASK;
                        else
                            G.primask = -1;
                    }
                }
                option_mask32 |= OPT_kmsg;
            } else {
                if (G.kmsgfd >= 0) { close(G.kmsgfd); G.kmsgfd = -1; }
                option_mask32 &= ~OPT_kmsg;
            }
            bb_error_msg("kmsg changed to %d", v);
        }
    } else if (strcmp(key, "remote") == 0) {
        /* Compare with current remote */
        char cur_remote[280] = "";
        if (G.remoteHosts) {
            remoteHost_t *rh = (remoteHost_t*)G.remoteHosts->data;
            const char *proto = (rh->proto == REMOTE_TCP) ? "tcp://" : "udp://";
            snprintf(cur_remote, sizeof(cur_remote), "%s%s", proto, rh->hostname);
        }
        if (strcmp(cur_remote, value) != 0) {
            remote_recreate(value);
            G.ringbuf_remote_seq = G.ringbuf_file_seq;
            bb_error_msg("remote changed to %s", value[0] ? value : "(none)");
        }
    } else if (strcmp(key, "server_port") == 0) {
        int v = atoi(value);
        if (v < 0 || v > 65535) v = 0;
        if (v != G.syslog_server_port) {
            /* Close old listen sockets and all TCP clients */
            tcp_client_t *cl = G.tcp_clients;
            while (cl) {
                tcp_client_t *next = cl->next;
                close(cl->fd);
                free(cl);
                cl = next;
            }
            G.tcp_clients = NULL;
            if (G.tcp_listen_fd >= 0) { close(G.tcp_listen_fd); G.tcp_listen_fd = -1; }
            if (G.udp_listen_fd >= 0) { close(G.udp_listen_fd); G.udp_listen_fd = -1; }

            if (v > 0) {
                if (syslog_server_init(v) == 0) {
                    bb_error_msg("syslog server enabled on port %d", v);
                }
            } else {
                bb_error_msg("syslog server disabled");
            }
            G.syslog_server_port = v;
        }
    } else {
        return -1; /* unknown key */
    }

    /* Update conf snapshot and persist to config file */
    syslog_conf_snapshot(&G.conf);
    if (G.conf_file)
        syslog_conf_write(G.conf_file, &G.conf);

    return 0;
}


/* ============================================================
 * Control Socket Server
 * ============================================================ */

static int ctl_init(const char *sock_path)
{
    struct sockaddr_un sun;
    int fd;

    if (!sock_path)
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    safe_strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path));
    unlink(sun.sun_path);

    if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return -1;
    }

    listen(fd, 8);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void ctl_handle_client(int fd);

static void ctl_accept(void)
{
    int fd = accept(G.ctl_listen_fd, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ctl_handle_client(fd);
    close(fd);
}

static void ctl_handle_client(int fd)
{
    char buf[512];
    char resp[4096];
    ssize_t n;
    int pos = 0;

    /* Read until newline with timeout (non-blocking + short poll) */
    while (pos < (int)(sizeof(buf) - 1)) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 500) <= 0) break;
        n = read(fd, buf + pos, sizeof(buf) - 1 - pos);
        if (n <= 0) break;
        pos += n;
        buf[pos] = '\0';
        if (strchr(buf, '\n')) break;
    }
    buf[pos] = '\0';

    /* Strip trailing newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    if (strncmp(buf, "SET ", 4) == 0) {
        char *kv = buf + 4;
        /* Skip leading whitespace */
        while (*kv == ' ') kv++;
        char *eq = strchr(kv, '=');
        if (!eq) {
            write(fd, "ERROR invalid format, use SET key=value\n", 40);
            return;
        }
        *eq = '\0';
        char *k = kv;
        char *v = eq + 1;
        if (conf_apply_one(k, v) == 0)
            write(fd, "OK\n", 3);
        else
            write(fd, "ERROR unknown key\n", 18);
    } else if (strcmp(buf, "GET") == 0) {
        syslog_conf_snapshot(&G.conf);
        char *p = resp;
        int remain = sizeof(resp);
        int w;

        #define APPEND(fmt, val) do { \
            w = snprintf(p, remain, fmt, val); \
            if (w > 0) { p += w; remain -= w; } \
        } while(0)

        APPEND("log_file=%s\n", G.conf.log_file ? G.conf.log_file : "");
        APPEND("log_level=%d\n", G.conf.log_level);
        APPEND("log_size=%u\n", G.conf.log_file_size_kb);
        APPEND("log_rotate=%u\n", G.conf.log_file_rotate);
        APPEND("remote=%s\n", G.conf.remote ? G.conf.remote : "");
        APPEND("server_port=%d\n", G.conf.server_port);
        APPEND("kernel_log=%d\n", G.conf.kernel_log);
        APPEND("kmsg=%d\n", G.conf.kmsg);
        w = snprintf(p, remain, ".\n");
        if (w > 0) { p += w; remain -= w; }

        #undef APPEND
        write(fd, resp, strlen(resp));
    } else if (strcmp(buf, "RELOAD") == 0) {
        if (!G.conf_file) {
            write(fd, "ERROR no config file (-c not used)\n", 36);
        } else {
            syslog_conf_t reloaded;
            memset(&reloaded, 0, sizeof(reloaded));
            if (syslog_conf_read(G.conf_file, &reloaded) != 0) {
                write(fd, "ERROR cannot read config file\n", 30);
            } else {
                char buf_int[32];
                snprintf(buf_int, sizeof(buf_int), "%d", reloaded.log_level);
                conf_apply_one("log_level", buf_int);
                snprintf(buf_int, sizeof(buf_int), "%u", reloaded.log_file_size_kb);
                conf_apply_one("log_size", buf_int);
                snprintf(buf_int, sizeof(buf_int), "%u", reloaded.log_file_rotate);
                conf_apply_one("log_rotate", buf_int);
                conf_apply_one("log_file", reloaded.log_file ? reloaded.log_file : "");
                conf_apply_one("remote", reloaded.remote ? reloaded.remote : "");
                snprintf(buf_int, sizeof(buf_int), "%d", reloaded.server_port);
                conf_apply_one("server_port", buf_int);
                snprintf(buf_int, sizeof(buf_int), "%d", reloaded.kernel_log);
                conf_apply_one("kernel_log", buf_int);
                snprintf(buf_int, sizeof(buf_int), "%d", reloaded.kmsg);
                conf_apply_one("kmsg", buf_int);
                syslog_conf_free(&reloaded);
                write(fd, "OK\n", 3);
            }
        }
    } else {
        write(fd, "ERROR unknown command\n", 22);
    }
}


static void do_syslogd(int sock_fd) NORETURN;
static void do_syslogd(int sock_fd)
{
    int last_sz = -1;
    char *last_buf;
    char *recvbuf = G.recvbuf;
    struct pollfd fds[32];
    nfds_t nfds;

    signal_no_SA_RESTART_empty_mask(SIGTERM, record_signo);
    signal_no_SA_RESTART_empty_mask(SIGINT, record_signo);
    signal(SIGHUP, SIG_IGN);

    xmove_fd(sock_fd, STDIN_FILENO);

    if (option_mask32 & OPT_kmsg)
        kmsg_init();
#if ENABLE_FEATURE_KERNEL_LOG
    if (option_mask32 & OPT_kernel_log)
        kernel_log_init();
#endif

    timestamp_and_log_internal("syslogd started: " BB_VER " built " COMPILE_TIME);
    write_pidfile_std_path_and_ext("syslogd");

    while (!bb_got_signal) {
        nfds = 0;

        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        if (klog_fd >= 0) {
            fds[nfds].fd = klog_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (G.tcp_listen_fd >= 0) {
            fds[nfds].fd = G.tcp_listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (G.udp_listen_fd >= 0) {
            fds[nfds].fd = G.udp_listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        if (G.ctl_listen_fd >= 0) {
            fds[nfds].fd = G.ctl_listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        tcp_client_t *cl;
        for (cl = G.tcp_clients; cl && nfds < 32; cl = cl->next) {
            fds[nfds].fd = cl->fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        int ret = poll(fds, nfds, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (nfds_t i = 0; i < nfds; i++) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;

            int fd = fds[i].fd;

            if (fd == STDIN_FILENO) {
                last_buf = recvbuf;
                recvbuf = (recvbuf == G.recvbuf) ? G.recvbuf + MAX_READ : G.recvbuf;

                ssize_t sz = read(STDIN_FILENO, recvbuf, MAX_READ - 1);
                if (sz <= 0) {
                    if (errno != EINTR && !bb_got_signal)
                        bb_perror_msg("read from /dev/log");
                    continue;
                }

                while (sz > 0 && (recvbuf[sz-1] == '\0' || recvbuf[sz-1] == '\n'))
                    sz--;
                if (sz == 0) continue;

                if ((option_mask32 & OPT_dup) && (sz == last_sz) &&
                    memcmp(last_buf, recvbuf, sz) == 0)
                    continue;
                last_sz = sz;
                recvbuf[sz] = '\0';

                if (G.ringbuf) {
                    process_raw_message(recvbuf, sz);
                } else {
                    recvbuf[sz] = '\n';
                    llist_t *item;
                    for (item = G.remoteHosts; item; item = item->link) {
                        remoteHost_t *rh = (remoteHost_t*)item->data;
                        remote_send_one(rh, recvbuf, sz + 1);
                    }
                    process_raw_message(recvbuf, sz);
                }
            }
            else if (fd == klog_fd) {
                kernel_log_process();
            }
            else if (fd == G.udp_listen_fd) {
                syslog_server_recv_udp();
            }
            else if (fd == G.tcp_listen_fd) {
                syslog_server_accept();
            }
            else if (fd == G.ctl_listen_fd) {
                ctl_accept();
            }
            else {
                tcp_client_t **pp = &G.tcp_clients;
                int handled = 0;
                while (*pp) {
                    if ((*pp)->fd == fd) {
                        syslog_server_recv_tcp(*pp, pp);
                        handled = 1;
                        break;
                    }
                    pp = &(*pp)->next;
                }
                if (!handled) {
                    /* might be a remote host send fd that errored */
                }
            }
        }

        /* Drain ring buffer to file and remote */
        if (G.ringbuf) {
            char line[2048];
            while (ringbuf_consume(G.ringbuf, &G.ringbuf_file_seq, line, sizeof(line)) == 1) {
                log_locally(line, &G.logFile);
                G.ringbuf->file_seq = G.ringbuf_file_seq;
                __sync_synchronize();
            }

            llist_t *item;
            for (item = G.remoteHosts; item; item = item->link) {
                remoteHost_t *rh = (remoteHost_t*)item->data;
                while (ringbuf_consume(G.ringbuf, &G.ringbuf_remote_seq, line, sizeof(line)) == 1) {
                    remote_send_one(rh, line, strlen(line));
                    G.ringbuf->remote_seq = G.ringbuf_remote_seq;
                    __sync_synchronize();
                }
            }
        }
    }

    timestamp_and_log_internal("syslogd exiting");
    remove_pidfile_std_path_and_ext("syslogd");

#if ENABLE_FEATURE_KERNEL_LOG
    if (option_mask32 & OPT_kernel_log)
        kernel_log_close();
#endif

    tcp_client_t *cl = G.tcp_clients;
    while (cl) {
        tcp_client_t *next = cl->next;
        close(cl->fd);
        free(cl);
        cl = next;
    }
    if (G.tcp_listen_fd >= 0) close(G.tcp_listen_fd);
    if (G.udp_listen_fd >= 0) close(G.udp_listen_fd);
    if (G.ctl_listen_fd >= 0) {
        close(G.ctl_listen_fd);
        if (G.ctl_sock_path) unlink(G.ctl_sock_path);
    }
    if (G.conf_file) {
        free(G.conf_file);
        free(G.ctl_sock_path);
    }
    syslog_conf_free(&G.conf);

    ringbuf_cleanup();
    if (option_mask32 & OPT_kmsg)
        kmsg_cleanup();
    kill_myself_with_sig(bb_got_signal);
}


static char *get_primary_mac(void)
{
    struct ifaddrs *ifaddr, *ifa;
    char *mac = NULL;

    if (getifaddrs(&ifaddr) == -1)
        return NULL;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
        if (strcmp(ifa->ifa_name, "eth0") == 0) {
            struct sockaddr_ll *s = (struct sockaddr_ll *)ifa->ifa_addr;
            mac = xzalloc(18);
            snprintf(mac, 18, "%02x%02x%02x%02x%02x%02x",
                s->sll_addr[0], s->sll_addr[1], s->sll_addr[2],
                s->sll_addr[3], s->sll_addr[4], s->sll_addr[5]);
            goto done;
        }
    }

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (strncmp(ifa->ifa_name, "sit", 3) == 0) continue;
        struct sockaddr_ll *s = (struct sockaddr_ll *)ifa->ifa_addr;
        int has_mac = 0;
        for (int i = 0; i < 6; i++) { if (s->sll_addr[i]) { has_mac = 1; break; } }
        if (!has_mac) continue;
        mac = xzalloc(18);
        snprintf(mac, 18, "%02x%02x%02x%02x%02x%02x",
            s->sll_addr[0], s->sll_addr[1], s->sll_addr[2],
            s->sll_addr[3], s->sll_addr[4], s->sll_addr[5]);
        goto done;
    }

done:
    freeifaddrs(ifaddr);
    return mac;
}

static void print_help(void)
{
    printf(
        "syslogd - System logging daemon\n"
        "\n"
        "Usage: syslogd [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -n            Run in foreground\n"
        "  -O FILE       Log to FILE (default: /tmp/log/<mac>.log)\n"
        "  -l N          Log only messages more urgent than prio N (1-8)\n"
        "  -s SIZE       Max size (KB) before rotation (default 200KB, 0=off)\n"
        "  -b N          N rotated logs to keep (default 1, max 99, 0=purge)\n"
        "  -R [tcp://]HOST[:PORT]  Remote syslog host (default udp, port 514)\n"
        "  -D            Drop duplicate messages\n"
        "  -f FILE       Use FILE as config (default: /etc/syslog.conf)\n"
        "  -k            Read kernel log (dmesg) and record it\n"
        "  -K            Also write messages to /dev/kmsg\n"
        "  -r [PORT]     Listen for syslog on TCP+UDP port (default 514)\n"
        "  -v            Read ring buffer (dump all messages and exit)\n"
        "  -v -f         Read ring buffer and follow new messages\n"
        "  -v -n N       Show last N messages from ring buffer\n"
        "  -c FILE       Runtime config file (also enables control socket)\n"
        "  --set K=V     Send config change to running syslogd\n"
        "  --get         Query current config from running syslogd\n"
        "  --reload      Force running syslogd to reload config file\n"
        "  -h            Show this help\n"
        "\n"
        "Build: " COMPILE_TIME "\n"
    );
}

static int reader_mode(int follow, int last_n, int dump_all)
{
    int shmid = shmget(KEY_ID, 0, 0);
    if (shmid == -1) {
        fprintf(stderr, "syslogd: no ring buffer found (is syslogd running?)\n");
        return 1;
    }

    struct ringbuf_hdr *rb = (struct ringbuf_hdr *)shmat(shmid, NULL, SHM_RDONLY);
    if (rb == (void*)-1L) {
        fprintf(stderr, "syslogd: cannot attach to ring buffer\n");
        return 1;
    }

    if (rb->magic != RINGBUF_MAGIC) {
        fprintf(stderr, "syslogd: invalid ring buffer magic\n");
        shmdt(rb);
        return 1;
    }

    uint64_t next_seq;
    if (follow && !dump_all && last_n == 0) {
        next_seq = rb->producer_seq;
    } else if (last_n > 0) {
        uint64_t start = rb->producer_seq;
        next_seq = (start > (uint64_t)last_n) ? (start - last_n) : 1;
    } else {
        next_seq = 1;
    }

    char line[2048];
    while (1) {
        int ret;
        while ((ret = ringbuf_consume(rb, &next_seq, line, sizeof(line))) == 1) {
            fputs(line, stdout);
            fflush(stdout);
        }
        if (ret == -1) {
            fprintf(stderr, "syslogd: overrun - some messages lost\n");
        }

        if (!follow) break;

        struct pollfd pfd;
        pfd.fd = 0;
        pfd.events = 0;
        poll(&pfd, 0, 200);
    }

    shmdt(rb);
    return 0;
}

/* ============================================================
 * Control Socket Client Mode
 * ============================================================ */

static int ctl_client_mode(const char *sock_path, const char *cmd)
{
    struct sockaddr_un sun;
    int fd, n;
    char rbuf[4096];

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "syslogd: cannot create socket: %s\n", strerror(errno));
        return 1;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    safe_strncpy(sun.sun_path, sock_path, sizeof(sun.sun_path));

    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        fprintf(stderr, "syslogd: cannot connect to %s: %s\n"
                "Is syslogd running with -c?\n",
                sock_path, strerror(errno));
        close(fd);
        return 1;
    }

    /* Send command */
    n = write(fd, cmd, strlen(cmd));
    if (n <= 0) { close(fd); return 1; }

    /* Read response with timeout */
    {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 2000) > 0) {
            n = read(fd, rbuf, sizeof(rbuf) - 1);
            if (n > 0) {
                rbuf[n] = '\0';
                fputs(rbuf, stdout);
            }
        }
    }

    close(fd);
    return 0;
}


int syslogd_main(int argc, char **argv)
{
    int opts = 0;
    char *opt_l = NULL, *opt_O = NULL, *opt_s = NULL;
    char *opt_b = NULL, *opt_f = NULL, *opt_r = NULL, *opt_c = NULL;
    int opt;
    int view_follow = 0, view_lastn = 0, view_dumpall = 0;
    llist_t *remoteAddrList = NULL;
    char *client_cmd = NULL;  /* non-NULL = client mode */

    /* Check for -v (reader mode) BEFORE getopt.
     * -f and -n are sub-options of -v that conflict with getopt
     * (-f takes config file arg, -n sets nofork). */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-v") == 0) {
                view_dumpall = 1;
                /* Scan remaining args for -f and -n N */
                int j;
                for (j = i + 1; j < argc; j++) {
                    if (strcmp(argv[j], "-f") == 0)
                        view_follow = 1;
                    else if (strcmp(argv[j], "-n") == 0 && j + 1 < argc) {
                        j++;
                        view_lastn = atoi(argv[j]);
                        if (view_lastn <= 0) view_lastn = 50;
                    }
                }
                return reader_mode(view_follow, view_lastn, view_dumpall);
            }
        }
    }

    /* Check for --set / --get / --reload BEFORE getopt (client mode).
     * These must be checked first to avoid getopt misinterpreting
     * --set as -s -e -t short options. */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--get") == 0) {
                client_cmd = "GET\n";
            } else if (strcmp(argv[i], "--reload") == 0) {
                client_cmd = "RELOAD\n";
            } else if (strncmp(argv[i], "--set", 5) == 0) {
                const char *kv;
                if (argv[i][5] == '=') {
                    kv = argv[i] + 6;
                } else if (argv[i][5] == '\0' && i + 1 < argc) {
                    i++;
                    kv = argv[i];
                } else {
                    fprintf(stderr, "syslogd: --set requires key=value\n");
                    return 1;
                }
                static char setbuf[512];
                snprintf(setbuf, sizeof(setbuf), "SET %s\n", kv);
                client_cmd = setbuf;
            }
        }
    }

    /* Client mode: parse -c FILE and enter client mode */
    if (client_cmd) {
        /* Only parse -c from command line */
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                i++;
                opt_c = argv[i];
            }
        }
        char sock_path[280];
        if (!opt_c) {
            fprintf(stderr, "syslogd: --set/--get/--reload requires -c FILE\n");
            return 1;
        }
        snprintf(sock_path, sizeof(sock_path), "%s.ctl", opt_c);
        return ctl_client_mode(sock_path, client_cmd);
    }

    INIT_G();

    opterr = 0;
    optind = 1;
    while ((opt = getopt(argc, argv, "nO:l:s:b:R:Df:r:vkKhc:")) != -1) {
        switch (opt) {
        case 'n': opts |= OPT_nofork; break;
        case 'O': opt_O = optarg; break;
        case 'l': opt_l = optarg; break;
        case 's': opt_s = optarg; break;
        case 'b': opt_b = optarg; break;
        case 'R':
            opts |= OPT_remotelog;
            llist_add_to(&remoteAddrList, xstrdup(optarg));
            break;
        case 'D': opts |= OPT_dup; break;
        case 'f': opt_f = optarg; break;
        case 'r': opt_r = optarg ? optarg : "514"; break;
        case 'v':
            /* Handled before getopt — should not reach here */
            break;
        case 'k': opts |= OPT_kernel_log; break;
        case 'K': opts |= OPT_kmsg; break;
        case 'c': opt_c = optarg; break;
        case 'h':
            print_help();
            exit(0);
        }
    }

    option_mask32 = opts;

    /* ---- Load runtime config file (if -c given and file exists) ---- */
    if (opt_c) {
        G.conf_file = xstrdup(opt_c);

        if (syslog_conf_read(opt_c, &G.conf) == 0) {
            /* Apply loaded config as defaults (CLI args will override) */
            if (G.conf.log_file) {
                /* Don't override yet; CLI -O takes precedence later */
            }
            if (G.conf.log_level > 0)
                G.logLevel = G.conf.log_level;
            if (G.conf.log_file_size_kb > 0)
                G.logFileSize = G.conf.log_file_size_kb * 1024;
            G.logFileRotate = G.conf.log_file_rotate;
            if (G.conf.remote && !(opts & OPT_remotelog)) {
                llist_add_to(&remoteAddrList, xstrdup(G.conf.remote));
            }
            if (G.conf.server_port > 0 && !opt_r) {
                opt_r = xzalloc(16);
                snprintf(opt_r, 16, "%d", G.conf.server_port);
            }
            if (G.conf.kernel_log && !(opts & OPT_kernel_log))
                opts |= OPT_kernel_log;
            if (G.conf.kmsg && !(opts & OPT_kmsg))
                opts |= OPT_kmsg;
        }
        /* If file doesn't exist, we'll create it after CLI parsing */

        /* Control socket path = conf_file + ".ctl" */
        G.ctl_sock_path = xzalloc(strlen(opt_c) + 8);
        snprintf(G.ctl_sock_path, strlen(opt_c) + 8, "%s.ctl", opt_c);
    }

    /* ---- CLI overrides ---- */
    if (opt_l)
        G.logLevel = xatou_range_wrapper(opt_l, 1, 8);
    if (opt_s)
        G.logFileSize = xatoul_range_wrapper(opt_s, 0, INT_MAX/1024) * 1024;
    if (opt_b)
        G.logFileRotate = xatoul_range_wrapper(opt_b, 0, 99);

    if (!opt_O) {
        char *mac = get_primary_mac();
        if (mac) {
            char logpath[256];
            snprintf(logpath, sizeof(logpath), "/tmp/log/%s.log", mac);
            free(mac);
            mkdir("/tmp/log", 0755);
            G.logFile.path = xstrdup(logpath);
        }
    } else {
        G.logFile.path = opt_O;
    }

    /* Build remote host from CLI -R or config file */
    while (remoteAddrList) {
        remoteHost_t *rh = xzalloc(sizeof(*rh));
        char *addr = llist_pop(&remoteAddrList);
        if (strncmp(addr, "tcp://", 6) == 0) {
            rh->proto = REMOTE_TCP;
            rh->hostname = xstrdup(addr + 6);
        } else if (strncmp(addr, "udp://", 6) == 0) {
            rh->proto = REMOTE_UDP;
            rh->hostname = xstrdup(addr + 6);
        } else {
            rh->proto = REMOTE_UDP;
            rh->hostname = addr;
        }
        rh->fd = -1;
        rh->last_reconnect = monotonic_sec() - DNS_WAIT_SEC - 1;
        llist_add_to(&G.remoteHosts, rh);
    }

    parse_syslogdcfg(opt_f);

    G.hostname = safe_gethostname();
    {
        char *dot = strchr(G.hostname, '.');
        if (dot) *dot = '\0';
    }

    ringbuf_init();

    if (opt_r) {
        int port = atoi(opt_r);
        if (port <= 0 || port > 65535) port = 514;
        syslog_server_init(port);
        G.syslog_server_port = port;
    }

    /* ---- Write runtime config file ---- */
    if (opt_c) {
        syslog_conf_snapshot(&G.conf);
        syslog_conf_write(opt_c, &G.conf);

        /* Init control socket (must be before daemonize so path is valid) */
        G.ctl_listen_fd = ctl_init(G.ctl_sock_path);
    }

    int sock_fd = create_socket();

    if (!(opts & OPT_nofork)) {
        bb_daemonize_or_rexec(DAEMON_CHDIR_ROOT, argv);
    }

    do_syslogd(sock_fd);
}
