/*
 * studio.c - device-side agent for the Nova PC sync tool
 *
 * One TCP port (7777), length-prefixed frames (see studio_internal.h).
 * Threads: accept, per-client, metrics (1 Hz), kmsg tail, frame encoder
 * (in agent_frame.c). Payload encoding is cJSON; binary payloads use the
 * small headers documented per frame type.
 */
#include <arpa/inet.h>
#include <cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "studio_internal.h"

/*********************
 *   STATIC PROTOTYPES
 **********************/
static void *accept_thread(void *arg);
static void *client_thread(void *arg);
static void *metrics_thread(void *arg);
static void *kmsg_thread(void *arg);
static void *syslog_thread(void *arg);
static int handle_frame(struct sa_client *c, uint32_t type, const uint8_t *payload, uint32_t len);
static int read_full(int fd, uint8_t *buf, size_t len);
static int write_full(int fd, const uint8_t *buf, size_t len);
static char *build_hello_json(void);
static char *build_metrics_json(void);

/*********************
 *  STATIC VARIABLES
 **********************/
static int s_listen_fd = -1;
static bool s_running = false;
static pthread_t s_tid_accept, s_tid_metrics, s_tid_kmsg, s_tid_syslog;

/* extern-declared in studio_internal.h (agent_frame.c reads it) */
pthread_mutex_t s_clients_lock = PTHREAD_MUTEX_INITIALIZER;
struct sa_client *s_clients;

/* metrics state shared between metrics_thread and broadcast */
static pthread_mutex_t s_metrics_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_metrics_json[4096];
static unsigned long s_prev_idle;
static long s_last_total;
#define SA_CORE_MAX 4
static long s_core_total_prev[SA_CORE_MAX];
static unsigned long s_core_idle_prev[SA_CORE_MAX];
static int s_core_prev_n;

/*********************
 *      FUNCTIONS
 **********************/
void sa_log(const char *level, const char *fmt, ...)
{
    struct timeval tv;
    struct tm tmbuf;
    char tbuf[24];
    va_list ap;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tmbuf);
    strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M:%S", &tmbuf);
    fprintf(stderr, "[%s.%03d][%s][%s] ", tbuf, (int)(tv.tv_usec / 1000), SA_LOG_TAG, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------------- low-level io ---------------- */

static int write_full(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = read(fd, buf + off, len - off);
        if (n == 0)
            return -1;
        if (n < 0)
        {
            /* EAGAIN is a recv timeout, not a dead peer. Dropping the
             * session here made the PC tool reconnect every 12s. */
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                struct timespec ts = {0, 50 * 1000 * 1000};
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/* ---------------- client registry & send ---------------- */

int sa_send_raw(struct sa_client *c, uint32_t type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    int ret = -1;

    pthread_mutex_lock(&c->send_lock);
    wr_le32(hdr, type);
    wr_le32(hdr + 4, len);
    if (write_full(c->fd, hdr, 8) == 0 && (len == 0 || write_full(c->fd, data, len) == 0))
        ret = 0;
    pthread_mutex_unlock(&c->send_lock);
    return ret;
}

int sa_send_json(struct sa_client *c, uint32_t type, const char *json)
{
    return sa_send_raw(c, type, (const uint8_t *)json, (uint32_t)strlen(json));
}

void sa_broadcast_json(uint32_t type, const char *json)
{
    pthread_mutex_lock(&s_clients_lock);
    for (struct sa_client *c = s_clients; c; c = c->next)
    {
        if (!c->active)
            continue;
        bool want = (type == SA_PT_METRICS) ? c->want_metrics : c->want_log;
        if (want && sa_send_json(c, type, json) != 0)
            c->active = false;
    }
    pthread_mutex_unlock(&s_clients_lock);
}

void sa_broadcast_frame(const uint8_t *jpeg, uint32_t len)
{
    pthread_mutex_lock(&s_clients_lock);
    for (struct sa_client *c = s_clients; c; c = c->next)
    {
        if (!c->active || !c->want_frame)
            continue;
        if (sa_send_raw(c, SA_PT_FRAME, jpeg, len) != 0)
            c->active = false;
    }
    pthread_mutex_unlock(&s_clients_lock);
}

/* ---------------- metrics ---------------- */

static int parse_cpu_jiffies(const char *line, long *total_out, unsigned long *idle_out)
{
    unsigned long v[8] = {0};
    int n = 0;
    const char *p = line;
    while (*p && *p != ' ')
        p++;
    while (n < 8)
    {
        char *end;
        unsigned long x = strtoul(p, &end, 10);
        if (end == p)
            break;
        v[n++] = x;
        p = end;
    }
    if (n < 4)
        return -1;
    long total = 0;
    for (int i = 0; i < n; i++)
        total += (long)v[i];
    *total_out = total;
    *idle_out = v[3] + (n > 4 ? v[4] : 0); /* idle + iowait */
    return 0;
}

/* Aggregate plus each cpuN line. core_pct is this interval, 0 on the first sample. */
static long read_proc_stat_total(unsigned long *idle_out, double *core_pct, int *core_n)
{
    char line[256];
    FILE *fp = fopen("/proc/stat", "r");
    long total = -1;
    int nc = 0;
    if (!fp)
    {
        *core_n = 0;
        return -1;
    }
    while (fgets(line, sizeof(line), fp))
    {
        long t = 0;
        unsigned long idle = 0;
        if (strncmp(line, "cpu ", 4) == 0)
        {
            if (parse_cpu_jiffies(line, &t, &idle) == 0)
            {
                total = t;
                *idle_out = idle;
            }
            continue;
        }
        if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
            break;
        if (nc >= SA_CORE_MAX || parse_cpu_jiffies(line, &t, &idle) != 0)
            continue;
        double pct = 0;
        if (nc < s_core_prev_n && t > s_core_total_prev[nc])
        {
            long dt = t - s_core_total_prev[nc];
            long di = (long)idle - (long)s_core_idle_prev[nc];
            if (dt > 0)
                pct = 100.0 * (dt - di) / dt;
        }
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        core_pct[nc] = pct;
        s_core_total_prev[nc] = t;
        s_core_idle_prev[nc] = idle;
        nc++;
    }
    fclose(fp);
    s_core_prev_n = nc;
    *core_n = nc;
    return total;
}

/* per-process cpu delta for the top list; keeps previous snapshot */
#define SA_TOP_N 16 /* fixed rows, same idea as a steady top -H window */

typedef struct
{
    int tid;
    int pid;
    char state;
    char name[32]; /* "进程/线程" when this task is not the main thread */
    long jiffies;
    long rss_kb;
} sa_proc_t;

static sa_proc_t s_prev_procs[256];
static int s_prev_proc_cnt;
/* metrics thread only. Two 256-row copies on the stack are ~24KB and will
 * walk off a small pthread stack into the next mapping (bus error). */
static sa_proc_t s_now_procs[256];
static double s_now_pcpu[256];

/* /proc/<pid>/task/<tid>/stat — one line per thread, which is what top -H lists */
static int read_task_stat(const char *path, char *name, size_t name_sz, char *state, long *jiffies)
{
    FILE *fp = fopen(path, "r");
    char buf[512];
    if (!fp)
        return -1;
    size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[got] = 0;
    char *lp = strchr(buf, '(');
    char *rp = lp ? strrchr(buf, ')') : NULL;
    if (!lp || !rp || name_sz == 0)
        return -1;
    size_t nlen = (size_t)(rp - lp - 1);
    if (nlen >= name_sz)
        nlen = name_sz - 1;
    memcpy(name, lp + 1, nlen);
    name[nlen] = 0;
    long utime = 0, stime = 0;
    int field = 3;
    char *save = NULL;
    *state = '?';
    for (char *tok = strtok_r(rp + 2, " ", &save); tok && field <= 15; tok = strtok_r(NULL, " ", &save), field++)
    {
        if (field == 3 && tok[0])
            *state = tok[0];
        else if (field == 14)
            utime = strtol(tok, NULL, 10);
        else if (field == 15)
            stime = strtol(tok, NULL, 10);
    }
    *jiffies = utime + stime;
    return 0;
}

static int build_proc_snapshot(sa_proc_t *out, int cap)
{
    int cnt = 0;
    long pagesz = sysconf(_SC_PAGESIZE) / 1024;
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) && cnt < cap)
    {
        int pid = atoi(e->d_name);
        if (pid <= 0)
            continue;
        char path[80];
        long rss_kb = 0;
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        FILE *fp = fopen(path, "r");
        if (fp)
        {
            long tot = 0, res = 0;
            if (fscanf(fp, "%ld %ld", &tot, &res) == 2)
                rss_kb = res * pagesz;
            fclose(fp);
        }
        snprintf(path, sizeof(path), "/proc/%d/task", pid);
        DIR *td = opendir(path);
        if (!td)
            continue;
        struct dirent *te;
        while ((te = readdir(td)) && cnt < cap)
        {
            int tid = atoi(te->d_name);
            if (tid <= 0)
                continue;
            snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
            char name[16];
            char state = '?';
            long jiffies = 0;
            if (read_task_stat(path, name, sizeof(name), &state, &jiffies) != 0)
                continue;
            out[cnt].tid = tid;
            out[cnt].pid = pid;
            out[cnt].state = state;
            {
                const char *proc = "main";
                char pcomm[32] = {0};
                snprintf(path, sizeof(path), "/proc/%d/comm", pid);
                FILE *cf = fopen(path, "r");
                if (cf)
                {
                    if (fgets(pcomm, sizeof(pcomm), cf))
                    {
                        char *nl = strchr(pcomm, '\n');
                        if (nl)
                            *nl = 0;
                    }
                    fclose(cf);
                }
                if (pcomm[0])
                    proc = pcomm;
                if (tid == pid)
                    snprintf(out[cnt].name, sizeof(out[cnt].name), "%s", proc);
                else if (name[0] && strcmp(name, proc) != 0)
                    snprintf(out[cnt].name, sizeof(out[cnt].name), "%s/%s", proc, name);
                else
                    snprintf(out[cnt].name, sizeof(out[cnt].name), "%s/%d", proc, tid);
            }
            out[cnt].jiffies = jiffies;
            out[cnt].rss_kb = rss_kb;
            cnt++;
        }
        closedir(td);
    }
    closedir(d);
    return cnt;
}

static char *build_metrics_json(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long hz = sysconf(_SC_CLK_TCK);

    unsigned long idle_now = 0, idle_prev;
    double core_pct[SA_CORE_MAX] = {0};
    int core_n = 0;
    long total_now = read_proc_stat_total(&idle_now, core_pct, &core_n);

    pthread_mutex_lock(&s_metrics_lock);
    idle_prev = s_prev_idle;
    s_prev_idle = idle_now;
    pthread_mutex_unlock(&s_metrics_lock);

    double cpu_pct = 0;
    if (total_now > 0 && s_last_total > 0)
    {
        long dtot = total_now - s_last_total;
        long didle = (long)idle_now - (long)idle_prev;
        if (dtot > 0)
            cpu_pct = 100.0 * (dtot - didle) / dtot;
    }
    s_last_total = total_now > 0 ? total_now : s_last_total;

    /* meminfo */
    long mem_total = 0, mem_free = 0, mem_avail = 0;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp)
    {
        char line[128];
        while (fgets(line, sizeof(line), fp))
        {
            if (sscanf(line, "MemTotal: %ld kB", &mem_total) == 1) { }
            else if (sscanf(line, "MemFree: %ld kB", &mem_free) == 1) { }
            else if (sscanf(line, "MemAvailable: %ld kB", &mem_avail) == 1) { }
        }
        fclose(fp);
    }
    if (mem_avail > 0)
        mem_free = mem_avail;

    /* uptime + load */
    double uptime = 0;
    fp = fopen("/proc/uptime", "r");
    if (fp)
    {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp))
            uptime = atof(buf);
        fclose(fp);
    }
    double l1 = 0, l5 = 0, l15 = 0;
    fp = fopen("/proc/loadavg", "r");
    if (fp)
    {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp))
            sscanf(buf, "%lf %lf %lf", &l1, &l5, &l15);
        fclose(fp);
    }

    /* temperature (may not exist on every board) */
    double temp_c = -1;
    fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp)
    {
        int mt = 0;
        if (fscanf(fp, "%d", &mt) == 1)
            temp_c = mt / 1000.0;
        fclose(fp);
    }

    /* net rx/tx */
    long rx = -1, tx = -1;
    fp = fopen("/proc/net/dev", "r");
    if (fp)
    {
        char line[256];
        while (fgets(line, sizeof(line), fp))
        {
            char ifname[32];
            long rxb, txb;
            if (sscanf(line, " %31[^:]: %ld %*d %*d %*d %*d %*d %*d %*d %ld", ifname, &rxb, &txb) == 3 &&
                strcmp(ifname, "lo") != 0)
            {
                rx = rxb;
                tx = txb; /* keep last non-lo iface */
            }
        }
        fclose(fp);
    }

    /* top -H: one row per thread, delta vs previous snapshot.
     * Scratch lives in s_now_*; this thread is the only writer. */
    int cnt = build_proc_snapshot(s_now_procs, 256);
    pthread_mutex_lock(&s_metrics_lock);
    int prev_cnt = s_prev_proc_cnt;
    if (prev_cnt < 0 || prev_cnt > (int)(sizeof(s_prev_procs) / sizeof(s_prev_procs[0])))
        prev_cnt = 0;
    for (int i = 0; i < cnt; i++)
    {
        s_now_pcpu[i] = 0;
        for (int j = 0; j < prev_cnt; j++)
        {
            if (s_prev_procs[j].tid == s_now_procs[i].tid)
            {
                double dj = (double)(s_now_procs[i].jiffies - s_prev_procs[j].jiffies);
                s_now_pcpu[i] = hz > 0 ? dj * 100.0 / hz : 0; /* interval = 1 s */
                break;
            }
        }
    }
    s_prev_proc_cnt = cnt < (int)(sizeof(s_prev_procs) / sizeof(s_prev_procs[0]))
                          ? cnt
                          : (int)(sizeof(s_prev_procs) / sizeof(s_prev_procs[0]));
    memcpy(s_prev_procs, s_now_procs, sizeof(sa_proc_t) * (size_t)s_prev_proc_cnt);
    pthread_mutex_unlock(&s_metrics_lock);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", tv.tv_sec);
    cJSON_AddNumberToObject(root, "uptime", uptime);
    cJSON_AddNumberToObject(root, "cpu", ((int)(cpu_pct * 10)) / 10.0);
    cJSON *cpus = cJSON_AddArrayToObject(root, "cpus");
    for (int i = 0; i < core_n; i++)
        cJSON_AddItemToArray(cpus, cJSON_CreateNumber(((int)(core_pct[i] * 10)) / 10.0));
    cJSON_AddNumberToObject(root, "mem_total_kb", mem_total);
    cJSON_AddNumberToObject(root, "mem_free_kb", mem_free);
    cJSON *loads = cJSON_AddArrayToObject(root, "load");
    cJSON_AddItemToArray(loads, cJSON_CreateNumber(l1));
    cJSON_AddItemToArray(loads, cJSON_CreateNumber(l5));
    cJSON_AddItemToArray(loads, cJSON_CreateNumber(l15));
    if (temp_c >= 0)
        cJSON_AddNumberToObject(root, "temp_c", temp_c);
    if (rx >= 0)
    {
        cJSON *net = cJSON_AddObjectToObject(root, "net");
        cJSON_AddNumberToObject(net, "rx_kb", rx / 1024);
        cJSON_AddNumberToObject(net, "tx_kb", tx / 1024);
    }
    /* top -H: real threads (tid != pid) first, then fill with main tasks */
    cJSON *procs = cJSON_AddArrayToObject(root, "procs");
    int nout = 0;
    for (int pass = 0; pass < 2 && nout < SA_TOP_N; pass++)
    {
    for (;;)
    {
        int best = -1;
        double bestv = -1;
        for (int i = 0; i < cnt; i++)
        {
            int thr;
            if (s_now_procs[i].tid <= 0)
                continue;
            thr = s_now_procs[i].tid != s_now_procs[i].pid;
            if (pass == 0 && !thr)
                continue;
            if (s_now_pcpu[i] > bestv)
            {
                bestv = s_now_pcpu[i];
                best = i;
            }
        }
        if (best < 0 || nout >= SA_TOP_N)
            break;
        int tid = s_now_procs[best].tid;
        s_now_procs[best].tid = -tid;
        if (bestv < 0)
            bestv = 0;
        cJSON *o = cJSON_CreateObject();
        char st[2] = {s_now_procs[best].state ? s_now_procs[best].state : '?', 0};
        cJSON_AddNumberToObject(o, "tid", tid);
        cJSON_AddNumberToObject(o, "pid", s_now_procs[best].pid);
        cJSON_AddStringToObject(o, "state", st);
        cJSON_AddStringToObject(o, "name", s_now_procs[best].name);
        cJSON_AddNumberToObject(o, "pcpu", ((int)(bestv * 10)) / 10.0);
        cJSON_AddNumberToObject(o, "rss_kb", s_now_procs[best].rss_kb);
        cJSON_AddNumberToObject(o, "cpu_s", hz > 0 ? s_now_procs[best].jiffies / hz : 0);
        cJSON_AddItemToArray(procs, o);
        nout++;
    }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void sa_metrics_tick(void)
{
    char *json = build_metrics_json();
    if (json)
    {
        pthread_mutex_lock(&s_metrics_lock);
        snprintf(s_metrics_json, sizeof(s_metrics_json), "%s", json);
        pthread_mutex_unlock(&s_metrics_lock);
        sa_broadcast_json(SA_PT_METRICS, json);
        cJSON_free(json);
    }
}

static void *metrics_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "sa_metrics");
    while (s_running)
    {
        sa_metrics_tick();
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---------------- device log tail (kmsg + syslog) ---------------- */

static void emit_log_line(const char *tag, int level, const char *text)
{
    cJSON *o;
    char *json;
    if (!text || !text[0])
        return;
    o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddNumberToObject(o, "ts", time(NULL));
    cJSON_AddNumberToObject(o, "level", level & 7);
    cJSON_AddStringToObject(o, "tag", tag);
    cJSON_AddStringToObject(o, "text", text);
    json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json)
    {
        sa_broadcast_json(SA_PT_LOG, json);
        cJSON_free(json);
    }
}

static void *kmsg_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "sa_kmsg");
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        AG_LOGW("open /dev/kmsg failed (%s), LOG channel disabled", strerror(errno));
        return NULL;
    }
    lseek(fd, 0, SEEK_END); /* skip boot history, tail from now */

    char buf[1024];
    while (s_running)
    {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        buf[n] = 0;
        /*
         * kmsg line: "<pri>,<seq>,<ts_usec>,<cont>;<message>"
         * strip the header, keep level + text; timestamps from the message
         * itself so serial and TCP logs align on one axis on the PC.
         */
        if (buf[0] != '<')
            continue;
        int pri = atoi(buf + 1);
        char *semi = strchr(buf, ';');
        if (!semi)
            continue;
        char *msg = semi + 1;
        char *nl = strchr(msg, '\n');
        if (nl)
            *nl = 0;

        emit_log_line("kmsg", pri, msg);
    }
    close(fd);
    return NULL;
}

/* userspace log written by syslogd (-O /var/log/messages) */
static void *syslog_thread(void *arg)
{
    const char *path = "/var/log/messages";
    char buf[1024];
    char line[480];
    int llen = 0;
    int skip_partial = 0;
    int fd;
    (void)arg;
    pthread_setname_np(pthread_self(), "sa_syslog");

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        AG_LOGW("open %s failed (%s), syslog tail disabled", path, strerror(errno));
        return NULL;
    }
    {
        off_t sz = lseek(fd, 0, SEEK_END);
        off_t start = sz > 6000 ? sz - 6000 : 0;
        lseek(fd, start, SEEK_SET);
        skip_partial = start > 0;
    }

    while (s_running)
    {
        ssize_t n = read(fd, buf, sizeof(buf));
        off_t cur;
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            struct timespec ts = {0, 200 * 1000 * 1000};
            nanosleep(&ts, NULL);
            cur = lseek(fd, 0, SEEK_CUR);
            if (lseek(fd, 0, SEEK_END) < cur)
            {
                close(fd);
                fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                    return NULL;
                llen = 0;
                skip_partial = 0;
            }
            else
                lseek(fd, cur, SEEK_SET);
            continue;
        }
        for (ssize_t i = 0; i < n; i++)
        {
            if (buf[i] == '\n')
            {
                line[llen] = 0;
                if (!skip_partial)
                    emit_log_line("syslog", 6, line);
                skip_partial = 0;
                llen = 0;
            }
            else if (llen < (int)sizeof(line) - 1)
                line[llen++] = buf[i];
        }
    }
    close(fd);
    return NULL;
}

/* ---------------- protocol handlers ---------------- */

static char *build_hello_json(void)
{
    int w = 0, h = 0;
    int fps = 0, scale = 1;
    sa_frame_get_params(&fps, &scale);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "nova");
    cJSON_AddStringToObject(root, "agent", STUDIO_VERSION);
    cJSON_AddNumberToObject(root, "fps", fps);
    cJSON_AddNumberToObject(root, "scale", scale);
    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    const char *cap_names[] = {"metrics", "log", "frame", "exec", "file", "app", "snapshot", "reboot"};
    for (size_t i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++)
        cJSON_AddItemToArray(caps, cJSON_CreateString(cap_names[i]));
    if (sa_frame_probe(&w, &h) == 0 && w > 0)
    {
        cJSON *res = cJSON_AddArrayToObject(root, "res");
        cJSON_AddItemToArray(res, cJSON_CreateNumber(w));
        cJSON_AddItemToArray(res, cJSON_CreateNumber(h));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* run a shell command, capture stdout+stderr (up to cap); returns exit code or -1 */
static int run_capture(const char *cmd, char *out, size_t cap)
{
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    size_t got = 0;
    if (out && cap > 0)
    {
        char tmp[512];
        size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0 && got + 1 < cap)
        {
            if (got + n >= cap)
                n = cap - got - 1;
            memcpy(out + got, tmp, n);
            got += n;
        }
        out[got] = 0;
    }
    int rc = pclose(fp);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void handle_exec(struct sa_client *c, const uint8_t *payload, uint32_t len)
{
    cJSON *req = cJSON_ParseWithLength((const char *)payload, len);
    const char *cmd = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd")) : NULL;
    if (!cmd || !cmd[0])
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"exec: missing cmd\"}");
        cJSON_Delete(req);
        return;
    }

    char *out = malloc(SA_EXEC_OUT_CAP);
    int rc = out ? run_capture(cmd, out, SA_EXEC_OUT_CAP) : -1;

    cJSON *rep = cJSON_CreateObject();
    cJSON_AddBoolToObject(rep, "ok", rc == 0);
    cJSON_AddNumberToObject(rep, "exit", rc);
    cJSON_AddStringToObject(rep, "cmd", cmd);
    if (out)
    {
        /* clamp non-printable bytes so the JSON stays text-safe */
        for (char *p = out; *p; p++)
            if ((unsigned char)*p < 0x20 && *p != '\n' && *p != '\t')
                *p = '.';
        cJSON_AddStringToObject(rep, "out", out);
        free(out);
    }
    char *json = cJSON_PrintUnformatted(rep);
    cJSON_Delete(rep);
    cJSON_Delete(req);
    if (json)
    {
        sa_send_json(c, SA_PT_EXEC_RESULT, json);
        cJSON_free(json);
    }
}

static void handle_file_get(struct sa_client *c, const uint8_t *payload, uint32_t len)
{
    cJSON *req = cJSON_ParseWithLength((const char *)payload, len);
    const char *path = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "path")) : NULL;
    if (!path)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"file_get: missing path\"}");
        cJSON_Delete(req);
        return;
    }

    uint8_t *hdr = malloc(8 + SA_MAX_PAYLOAD + 1);
    if (!hdr)
    {
        cJSON_Delete(req);
        return;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    uint32_t status = 0, size = 0;
    if (fd >= 0)
    {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size <= SA_MAX_PAYLOAD)
        {
            ssize_t got = read(fd, hdr + 8, SA_MAX_PAYLOAD);
            if (got >= 0)
                size = (uint32_t)got;
            else
                status = 3;
        }
        else
            status = 2; /* too large or stat failed */
        close(fd);
    }
    else
        status = 1; /* open failed */

    wr_le32(hdr, status);
    wr_le32(hdr + 4, size);
    /* reuse FILE_GET as the reply type: PC keys on the 8B header */
    pthread_mutex_lock(&c->send_lock);
    uint8_t fh[8];
    wr_le32(fh, SA_PT_FILE_GET);
    wr_le32(fh + 4, 8 + size);
    int ok = write_full(c->fd, fh, 8) == 0 && write_full(c->fd, hdr, 8 + size) == 0;
    pthread_mutex_unlock(&c->send_lock);
    free(hdr);
    (void)ok;
    cJSON_Delete(req);
}

static void handle_file_put(struct sa_client *c, const uint8_t *payload, uint32_t len)
{
    if (len < 8)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"file_put: short payload\"}");
        return;
    }
    uint32_t pathlen = rd_le32(payload);
    uint32_t size = rd_le32(payload + 4);
    if (pathlen == 0 || pathlen > 255 || 8 + pathlen + size > len)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"file_put: bad header\"}");
        return;
    }
    char path[256];
    memcpy(path, payload + 8, pathlen < sizeof(path) ? pathlen : sizeof(path) - 1);
    path[pathlen < sizeof(path) - 1 ? pathlen : sizeof(path) - 1] = 0;

    /* write target whitelist: only debug-visible trees */
    static const char *allow[] = {"/tmp/", "/usr/apps/", "/etc/config/", "/mnt/", "/usr/data/", "/usr/lib/"};
    bool ok = false;
    for (size_t i = 0; i < sizeof(allow) / sizeof(allow[0]); i++)
        if (strncmp(path, allow[i], strlen(allow[i])) == 0)
            ok = true;
    if (!ok)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"file_put: path not allowed\"}");
        return;
    }

    /* dirs of the target may not exist yet */
    char mkdir_cmd[300];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"$(dirname '%s')\"", path);
    run_capture(mkdir_cmd, NULL, 0);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"file_put: open failed\"}");
        return;
    }
    ssize_t wrote = write(fd, payload + 8 + pathlen, size);
    close(fd);

    cJSON *rep = cJSON_CreateObject();
    cJSON_AddBoolToObject(rep, "ok", wrote == (ssize_t)size);
    cJSON_AddStringToObject(rep, "path", path);
    char *json = cJSON_PrintUnformatted(rep);
    cJSON_Delete(rep);
    if (json)
    {
        sa_send_json(c, SA_PT_EXEC_RESULT, json);
        cJSON_free(json);
    }
}

static void handle_app_push(struct sa_client *c, const uint8_t *payload, uint32_t len)
{
    if (len < 8)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"app_push: short payload\"}");
        return;
    }
    uint32_t jlen = rd_le32(payload);
    uint32_t dlen = rd_le32(payload + 4);
    if (jlen == 0 || jlen > 64 * 1024 || 8 + jlen + dlen != len)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"app_push: bad header\"}");
        return;
    }

    char *manifest = malloc(jlen + 1);
    memcpy(manifest, payload + 8, jlen);
    manifest[jlen] = 0;

    AG_LOGI("app_push: %u bytes, manifest %.*s", dlen, (int)(jlen > 200 ? 200 : jlen), manifest);
    cJSON *result = sa_app_install(manifest, payload + 8 + jlen, dlen);
    free(manifest);

    if (!result)
        result = cJSON_Parse("{\"ok\":false,\"error\":\"install: internal failure\"}");
    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    if (json)
    {
        sa_send_json(c, SA_PT_APP_RESULT, json);
        cJSON_free(json);
    }
}

static void handle_sub(struct sa_client *c, const uint8_t *payload, uint32_t len)
{
    cJSON *req = cJSON_ParseWithLength((const char *)payload, len);
    if (!req)
    {
        sa_send_json(c, SA_PT_ERR, "{\"error\":\"sub: bad json\"}");
        return;
    }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(req, "metrics")) != NULL)
        c->want_metrics = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(req, "log")) != NULL)
        c->want_log = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(req, "frame")) != NULL)
        c->want_frame = cJSON_IsTrue(v);

    int fps = -1, scale = -1;
    if ((v = cJSON_GetObjectItem(req, "fps")) != NULL && cJSON_IsNumber(v))
        fps = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(req, "scale")) != NULL && cJSON_IsNumber(v))
        scale = (int)v->valuedouble;
    if (fps >= 0 || scale > 0)
        sa_frame_set_params(fps, scale);

    if (c->want_log)
    {
        FILE *fp = fopen("/var/log/messages", "r");
        char line[480];
        if (fp)
        {
            if (fseek(fp, 0, SEEK_END) == 0)
            {
                long sz = ftell(fp);
                long start = sz > 3500 ? sz - 3500 : 0;
                fseek(fp, start, SEEK_SET);
                if (start > 0)
                    fgets(line, sizeof(line), fp);
            }
            while (fgets(line, sizeof(line), fp))
            {
                cJSON *o;
                char *json;
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                if (!line[0])
                    continue;
                o = cJSON_CreateObject();
                if (!o)
                    continue;
                cJSON_AddNumberToObject(o, "ts", time(NULL));
                cJSON_AddNumberToObject(o, "level", 6);
                cJSON_AddStringToObject(o, "tag", "syslog");
                cJSON_AddStringToObject(o, "text", line);
                json = cJSON_PrintUnformatted(o);
                cJSON_Delete(o);
                if (json)
                {
                    sa_send_json(c, SA_PT_LOG, json);
                    cJSON_free(json);
                }
            }
            fclose(fp);
        }
    }

    /* answer with the current metrics line so the panel is instantly populated */
    if (c->want_metrics)
    {
        pthread_mutex_lock(&s_metrics_lock);
        char snap[4096];
        snprintf(snap, sizeof(snap), "%s", s_metrics_json[0] ? s_metrics_json : "{}");
        pthread_mutex_unlock(&s_metrics_lock);
        sa_send_json(c, SA_PT_METRICS, snap);
    }
    cJSON_Delete(req);
}

static void handle_reboot(struct sa_client *c)
{
    sa_send_json(c, SA_PT_EXEC_RESULT, "{\"ok\":true,\"out\":\"rebooting\"}");
    AG_LOGI("reboot requested by PC client");
    usleep(200 * 1000);
    sync();
    reboot(RB_AUTOBOOT);
}

static int handle_frame(struct sa_client *c, uint32_t type, const uint8_t *payload, uint32_t len)
{
    switch (type)
    {
    case SA_PT_PING:
        return sa_send_json(c, SA_PT_PONG, "{\"ok\":true}");
    case SA_PT_HELLO:
    {
        char *json = build_hello_json();
        int ret = json ? sa_send_json(c, SA_PT_HELLO, json) : -1;
        free(json);
        return ret;
    }
    case SA_PT_SUB:
        handle_sub(c, payload, len);
        return 0;
    case SA_PT_SNAPSHOT:
    {
        uint8_t *jpeg = NULL;
        uint32_t jlen = 0;
        if (sa_frame_grab(&jpeg, &jlen) == 0 && jpeg)
        {
            sa_send_raw(c, SA_PT_FRAME, jpeg, jlen);
            free(jpeg);
        }
        else
            sa_send_json(c, SA_PT_ERR, "{\"error\":\"snapshot failed\"}");
        return 0;
    }
    case SA_PT_EXEC:
        handle_exec(c, payload, len);
        return 0;
    case SA_PT_REBOOT:
        handle_reboot(c);
        return 0;
    case SA_PT_FILE_GET:
        handle_file_get(c, payload, len);
        return 0;
    case SA_PT_FILE_PUT:
        handle_file_put(c, payload, len);
        return 0;
    case SA_PT_APP_PUSH:
        handle_app_push(c, payload, len);
        return 0;
    case SA_PT_INPUT:
    {
        cJSON *req = cJSON_ParseWithLength((const char *)payload, len);
        cJSON *op, *jx, *jy;
        int down = 0, x = 0, y = 0, sw = 0, sh = 0;
        if (!req)
            return 0;
        op = cJSON_GetObjectItem(req, "op");
        jx = cJSON_GetObjectItem(req, "x");
        jy = cJSON_GetObjectItem(req, "y");
        if (cJSON_IsNumber(jx))
            x = jx->valueint;
        if (cJSON_IsNumber(jy))
            y = jy->valueint;
        if (cJSON_IsString(op) && op->valuestring && strcmp(op->valuestring, "up") != 0)
            down = 1;
        if (sa_frame_probe(&sw, &sh) != 0 || sw <= 0 || sh <= 0)
        {
            sw = 800;
            sh = 480;
        }
        sa_input_touch(x, y, down, sw, sh);
        cJSON_Delete(req);
        return 0;
    }
    default:
    {
        char err[64];
        snprintf(err, sizeof(err), "{\"error\":\"unknown type 0x%x\"}", type);
        return sa_send_json(c, SA_PT_ERR, err);
    }
    }
}

/* ---------------- client / accept threads ---------------- */

static void *client_thread(void *arg)
{
    struct sa_client *c = arg;
    pthread_setname_np(pthread_self(), "sa_client");
    int fd = c->fd;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* The PC tool subscribes and then only receives. A 10s SO_RCVTIMEO
     * closed that idle socket, and the tool reconnected 2s later, forever.
     * Keepalive drops peers that actually go away. */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    {
        int idle = 30, intvl = 5, kcnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kcnt, sizeof(kcnt));
    }
#endif

    char *json = build_hello_json();
    if (json)
    {
        sa_send_json(c, SA_PT_HELLO, json);
        free(json);
    }

    uint8_t hdr[8];
    while (c->active && s_running)
    {
        if (read_full(fd, hdr, 8) != 0)
            break;
        uint32_t type = rd_le32(hdr);
        uint32_t len = rd_le32(hdr + 4);
        if (len > SA_MAX_PAYLOAD)
        {
            AG_LOGW("client payload too large (%u), drop", len);
            break;
        }
        uint8_t *payload = NULL;
        if (len > 0)
        {
            payload = malloc(len);
            if (!payload || read_full(fd, payload, len) != 0)
            {
                free(payload);
                break;
            }
        }
        int ret = handle_frame(c, type, payload, len);
        free(payload);
        if (ret != 0)
            break;
    }

    AG_LOGI("client fd=%d disconnected", fd);
    pthread_mutex_lock(&s_clients_lock);
    c->active = false;
    /* unlink; client_thread owns the free */
    struct sa_client **pp = &s_clients;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;
    pthread_mutex_unlock(&s_clients_lock);
    close(fd);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
    return NULL;
}

static void *accept_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "sa_accept");
    while (s_running)
    {
        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }

        pthread_mutex_lock(&s_clients_lock);
        int cnt = 0;
        for (struct sa_client *p = s_clients; p; p = p->next)
            if (p->active)
                cnt++;
        pthread_mutex_unlock(&s_clients_lock);
        if (cnt >= SA_MAX_CLIENTS)
        {
            AG_LOGW("too many clients, reject fd=%d", fd);
            close(fd);
            continue;
        }

        struct sa_client *c = calloc(1, sizeof(*c));
        if (!c)
        {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->active = true;
        pthread_mutex_init(&c->send_lock, NULL);

        pthread_mutex_lock(&s_clients_lock);
        c->next = s_clients;
        s_clients = c;
        pthread_mutex_unlock(&s_clients_lock);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, c) != 0)
        {
            c->active = false;
            AG_LOGE("client thread create failed");
            continue;
        }
        pthread_detach(tid);
        AG_LOGI("client fd=%d connected (%d active)", fd, cnt + 1);
    }
    return NULL;
}

/* ---------------- module lifecycle ---------------- */

int module_init(const char *config_path)
{
    (void)config_path;
    return 0;
}

int module_start(void)
{
    if (s_running)
        return 0;

    signal(SIGPIPE, SIG_IGN);

    s_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s_listen_fd < 0)
    {
        AG_LOGE("socket failed: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SA_DEFAULT_PORT);
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        AG_LOGE("bind :%d failed: %s", SA_DEFAULT_PORT, strerror(errno));
        close(s_listen_fd);
        s_listen_fd = -1;
        return -1;
    }
    listen(s_listen_fd, 4);

    s_running = true;
    pthread_create(&s_tid_accept, NULL, accept_thread, NULL);
    pthread_create(&s_tid_metrics, NULL, metrics_thread, NULL);
    pthread_create(&s_tid_kmsg, NULL, kmsg_thread, NULL);
    pthread_create(&s_tid_syslog, NULL, syslog_thread, NULL);

    if (sa_frame_init(NULL, NULL) != 0)
        AG_LOGW("frame channel init failed; SNAPSHOT/FRAME disabled");

    AG_LOGI("agent v%s listening on :%d", STUDIO_VERSION, SA_DEFAULT_PORT);
    return 0;
}

void module_exit(void)
{
    s_running = false;
    if (s_listen_fd >= 0)
    {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
}
