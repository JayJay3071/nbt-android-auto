/*
 * nbt_live_file - shadow ONE path under /fs/usb0 (or /fs/ram3) with a QNX resource manager that serves a raw
 * H.264 (Annex B) byte stream. The stock HMI USB video player opens the file through io-nme (NmeApp ThreadOpen)
 * and plays it through the normal chain (HMI video page, VideoApp, DisplayManager, capture, WAL). The path
 * manager resolves open() to the LONGEST matching prefix, so while this process runs the attached name wins over
 * the real file on the stick; when it exits the real file is visible again. Nothing is ever written to the stick.
 *
 * Sources
 *   --loop <file.h264>  serves that file again and again (random access by offset); used for plumbing tests.
 *   --fifo <path>       LIVE: reads the Android Auto H.264 FIFO continuously into a ring buffer. Every open()
 *                       (io-nme opens the file several times: probes, then playback) starts with the latest SPS/PPS and
 *                       the most recent IDR picture, then follows the live stream. A reader that falls behind by more
 *                       than MAX_BACKLOG bytes skips forward to the newest IDR (bounded latency). When the reader has
 *                       caught up, an access unit delimiter is sent after 20 ms of silence (so the parser can output the
 *                       last frame of a burst), then a filler NAL every second (the player never sees EOF).
 *
 * usage: nbt_live_file --path /fs/usb0/<name> (--loop <file.h264> | --fifo <fifo>) [--seconds N] [--size BYTES]
 *                      [--ready <file>]
 *   --seconds N  self-terminate after N seconds (1..86400); 0 = run until SIGTERM (used by the service)
 *   --size BYTES size reported by stat (default 2147483647)
 *   --ready FILE created (empty) once the name is attached
 * Log: stderr (redirect to a file in /fs/ram3).
 */
/* QNX 6.5: without these the handle, OCB and thread pool parameter types default to void. */
#define RESMGR_HANDLE_T struct _iofunc_attr
#define RESMGR_OCB_T struct _iofunc_ocb
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CHUNK (256 * 1024)
#define MAX_LOOP_FILE (64 * 1024 * 1024)
#define RING_SIZE (8u * 1024u * 1024u)
#define MAX_BACKLOG (768u * 1024u)
#define MAX_STATES 32
#define PARAM_MAX 512

static unsigned char *g_data;           /* loop source */
static size_t g_len;
static volatile sig_atomic_t g_stop;
static volatile unsigned g_opens, g_closes, g_reads, g_seeks, g_skips;
static volatile unsigned long long g_bytes;

static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_attr;

/* ---- live ring ---- */
static int g_live;
static const char *g_fifo_path;
static unsigned char *g_ring;
static unsigned long long g_end;        /* absolute number of bytes received */
static unsigned long long g_idr_start;  /* absolute offset of the newest IDR slice start code */
static int g_have_idr;
static unsigned char g_sps[PARAM_MAX], g_pps[PARAM_MAX];
static size_t g_sps_len, g_pps_len;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

typedef struct {
    void *ocb;
    int used, started, aud;
    unsigned long long pos;
    unsigned char pre[2 * PARAM_MAX];
    size_t pre_len, pre_done;
} St;
static St g_st[MAX_STATES];

static const unsigned char AUD_NAL[] = { 0, 0, 0, 1, 0x09, 0xF0 };
static const unsigned char FILLER_NAL[] = { 0, 0, 0, 1, 0x0C, 0xFF, 0xFF, 0x80 };

static void logf2(const char *what, long long a, long long b)
{
    fprintf(stderr, "%ld %s %lld %lld\n", (long)time(NULL), what, a, b);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- ring helpers (caller holds g_mx) ---- */
static void ring_copy_out(unsigned long long abs, size_t n, unsigned char *dst)
{
    size_t at = (size_t)(abs % RING_SIZE), first = RING_SIZE - at;
    if (first > n) first = n;
    memcpy(dst, g_ring + at, first);
    if (n > first) memcpy(dst + first, g_ring, n - first);
}

static void ring_copy_in(unsigned long long abs, const unsigned char *src, size_t n)
{
    size_t at = (size_t)(abs % RING_SIZE), first = RING_SIZE - at;
    if (first > n) first = n;
    memcpy(g_ring + at, src, first);
    if (n > first) memcpy(g_ring, src + first, n - first);
}

/* NAL scanner state (writer thread only) */
static int sc_zeros, sc_want_hdr, sc_prev_type = -1;
static unsigned long long sc_nal_start, sc_prev_start;

static void finalize_nal(int type, unsigned long long from, unsigned long long to)
{
    size_t n;
    if (to <= from) return;
    n = (size_t)(to - from);
    if (n > PARAM_MAX) return;
    if (type == 7) { ring_copy_out(from, n, g_sps); g_sps_len = n; }
    else if (type == 8) { ring_copy_out(from, n, g_pps); g_pps_len = n; }
}

/* Called with the bytes already stored in the ring and g_mx held; first byte has absolute offset 'base'. */
static void scan_nals(unsigned long long base, const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char b = p[i];
        unsigned long long a = base + i;
        if (sc_want_hdr) {
            int type = b & 0x1f;
            if (sc_prev_type >= 0) finalize_nal(sc_prev_type, sc_prev_start, sc_nal_start);
            if (type == 5 && sc_prev_type != 5) { g_idr_start = sc_nal_start; g_have_idr = 1; }
            sc_prev_type = type;
            sc_prev_start = sc_nal_start;
            sc_want_hdr = 0;
            sc_zeros = 0;
            continue;
        }
        if (b == 0) { if (sc_zeros < 8) sc_zeros++; }
        else if (b == 1 && sc_zeros >= 2) { sc_nal_start = a - 2; sc_want_hdr = 1; sc_zeros = 0; }
        else sc_zeros = 0;
    }
}

static void *writer_thread(void *arg)
{
    int fd = open(g_fifo_path, O_RDWR | O_NONBLOCK);
    unsigned char buf[32768];
    (void)arg;
    if (fd < 0) { logf2("fifo-open-failed", errno, 0); g_stop = 1; return NULL; }
    while (!g_stop) {
        fd_set set;
        struct timeval tv = { 0, 200000 };
        int n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, NULL, NULL, &tv) < 0) { if (errno == EINTR) continue; break; }
        if (!FD_ISSET(fd, &set)) continue;
        n = (int)read(fd, buf, sizeof(buf));
        if (n <= 0) continue;
        pthread_mutex_lock(&g_mx);
        ring_copy_in(g_end, buf, (size_t)n);
        scan_nals(g_end, buf, (size_t)n);
        g_end += (unsigned long long)n;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mx);
    }
    close(fd);
    return NULL;
}

static void wait_ms(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&g_cv, &g_mx, &ts);
}

static St *get_state(void *ocb, int create)
{
    int i;
    for (i = 0; i < MAX_STATES; i++) if (g_st[i].used && g_st[i].ocb == ocb) return &g_st[i];
    if (!create) return NULL;
    for (i = 0; i < MAX_STATES; i++) if (!g_st[i].used) {
        memset(&g_st[i], 0, sizeof(g_st[i]));
        g_st[i].used = 1;
        g_st[i].ocb = ocb;
        return &g_st[i];
    }
    return NULL;
}

/* Fill buf with up to 'want' bytes for this reader; returns the byte count (always > 0). */
static size_t live_fill(void *ocb, unsigned char *buf, size_t want)
{
    St *st;
    size_t n = 0;
    pthread_mutex_lock(&g_mx);
    st = get_state(ocb, 1);
    if (!st) { n = sizeof(FILLER_NAL); memcpy(buf, FILLER_NAL, n); pthread_mutex_unlock(&g_mx); return n; }
    for (;;) {
        if (g_stop) { n = sizeof(FILLER_NAL); memcpy(buf, FILLER_NAL, n); break; }
        if (!st->started) {
            if (g_have_idr) {
                st->pos = g_idr_start;
                st->pre_len = 0;
                st->pre_done = 0;
                memcpy(st->pre, g_sps, g_sps_len); st->pre_len += g_sps_len;
                memcpy(st->pre + st->pre_len, g_pps, g_pps_len); st->pre_len += g_pps_len;
                st->started = 1;
                logf2("reader-start", (long long)g_idr_start, (long long)g_end);
                continue;
            }
            wait_ms(1000);
            if (!g_have_idr) { n = sizeof(FILLER_NAL); memcpy(buf, FILLER_NAL, n); break; }
            continue;
        }
        if (st->pre_done < st->pre_len) {
            n = st->pre_len - st->pre_done;
            if (n > want) n = want;
            memcpy(buf, st->pre + st->pre_done, n);
            st->pre_done += n;
            break;
        }
        if ((g_idr_start > st->pos && g_end - st->pos > MAX_BACKLOG) || g_end - st->pos > RING_SIZE - 65536u) {
            g_skips++;
            logf2("skip-to-idr", (long long)(g_end - st->pos), (long long)g_idr_start);
            st->pos = g_idr_start;
            st->pre_done = 0;
            continue;
        }
        if (st->pos < g_end) {
            unsigned long long avail = g_end - st->pos;
            n = avail > want ? want : (size_t)avail;
            ring_copy_out(st->pos, n, buf);
            st->pos += n;
            st->aud = 0;
            break;
        }
        if (!st->aud) {
            wait_ms(20);
            if (st->pos < g_end) continue;
            n = sizeof(AUD_NAL);
            memcpy(buf, AUD_NAL, n);
            st->aud = 1;
            break;
        }
        wait_ms(1000);
        if (st->pos < g_end) continue;
        n = sizeof(FILLER_NAL);
        memcpy(buf, FILLER_NAL, n);
        break;
    }
    pthread_mutex_unlock(&g_mx);
    return n;
}

/* ---- resource manager handlers ---- */
static int my_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle, void *extra)
{
    int r = iofunc_open_default(ctp, msg, handle, extra);
    if (r == _RESMGR_NPARTS(0) || r == EOK) {
        g_opens++;
        logf2("open", g_opens, ctp->info.pid);
    } else {
        logf2("open-failed", r, ctp->info.pid);
    }
    return r;
}

static int my_close(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb)
{
    g_closes++;
    logf2("close", g_closes, (long long)ocb->offset);
    if (g_live) {
        St *st;
        pthread_mutex_lock(&g_mx);
        st = get_state(ocb, 0);
        if (st) st->used = 0;
        pthread_mutex_unlock(&g_mx);
    }
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

static int my_lseek(resmgr_context_t *ctp, io_lseek_t *msg, RESMGR_OCB_T *ocb)
{
    long long want = (long long)msg->i.offset;
    int whence = msg->i.whence;
    int r = iofunc_lseek_default(ctp, msg, ocb);
    g_seeks++;
    if (g_seeks <= 40) logf2(whence == SEEK_SET ? "seek-set" : whence == SEEK_CUR ? "seek-cur" : "seek-end", want, (long long)ocb->offset);
    return r;
}

static int my_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    size_t want, done = 0, pos;
    unsigned char *buf;
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    want = msg->i.nbytes;
    if (want > MAX_CHUNK) want = MAX_CHUNK;
    if (want == 0 || (!g_live && g_len == 0)) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    buf = malloc(want);
    if (!buf) return ENOMEM;
    if (g_live) {
        done = live_fill(ocb, buf, want);
    } else {
        pos = (size_t)((unsigned long long)ocb->offset % g_len);
        while (done < want) {
            size_t n = g_len - pos;
            if (n > want - done) n = want - done;
            memcpy(buf + done, g_data + pos, n);
            done += n;
            pos = (pos + n) % g_len;
        }
    }
    ocb->offset += (off_t)done;
    g_bytes += done;
    g_reads++;
    if (g_reads <= 20 || (g_reads % 500) == 0) logf2("read", (long long)want, (long long)ocb->offset);
    MsgReply(ctp->rcvid, (int)done, buf, done);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > MAX_LOOP_FILE) { fclose(f); return -2; }
    g_data = malloc((size_t)size);
    if (!g_data || fread(g_data, 1, (size_t)size, f) != (size_t)size) { fclose(f); return -3; }
    g_len = (size_t)size;
    fclose(f);
    return 0;
}

static int path_allowed(const char *p)
{
    /* usb0 = USB mass storage, pfs0 = phone mounted by the stock MTP driver, ram3 = tests */
    if (strncmp(p, "/fs/usb0/", 9) != 0 && strncmp(p, "/fs/pfs0/", 9) != 0 && strncmp(p, "/fs/ram3/", 9) != 0) return 0;
    if (strstr(p, "..")) return 0;
    return strlen(p) < 200;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *loop = NULL, *ready = NULL;
    long seconds = 300;
    long long size = 2147483647LL;
    int i, id, rc;
    dispatch_t *dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t *tpp;
    pthread_t writer;
    time_t start, last_stat;
    struct sigaction sa;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--path")) path = argv[i + 1];
        else if (!strcmp(argv[i], "--loop")) loop = argv[i + 1];
        else if (!strcmp(argv[i], "--fifo")) g_fifo_path = argv[i + 1];
        else if (!strcmp(argv[i], "--ready")) ready = argv[i + 1];
        else if (!strcmp(argv[i], "--seconds")) seconds = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--size")) size = atoll(argv[i + 1]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!path || (!loop && !g_fifo_path) || (loop && g_fifo_path) || seconds < 0 || seconds > 86400 || size < 1) {
        fprintf(stderr, "usage: %s --path /fs/usb0/<name> (--loop <file.h264> | --fifo <fifo>) [--seconds 0..86400] [--size BYTES] [--ready FILE]\n", argv[0]);
        return 2;
    }
    if (!path_allowed(path)) { fprintf(stderr, "path must be under /fs/usb0/ or /fs/ram3/\n"); return 2; }
    g_live = g_fifo_path != NULL;
    if (g_live) {
        struct stat fst;
        /* The car has no mkfifo/mknod command: create the FIFO here if it is missing (RAM only). */
        if (stat(g_fifo_path, &fst) != 0 && !strncmp(g_fifo_path, "/fs/ram3/", 9) && !strstr(g_fifo_path, "..")) mkfifo(g_fifo_path, 0600);
        g_ring = malloc(RING_SIZE);
        if (!g_ring) { fprintf(stderr, "no memory for ring\n"); return 3; }
    } else {
        rc = load_file(loop);
        if (rc) { fprintf(stderr, "cannot load %s (%d)\n", loop, rc); return 3; }
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    dpp = dispatch_create();
    if (!dpp) { fprintf(stderr, "dispatch_create failed\n"); return 4; }
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 4096;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = my_open;
    g_io.read = my_read;
    g_io.lseek = my_lseek;
    g_io.close_ocb = my_close;
    iofunc_attr_init(&g_attr, S_IFREG | 0444, NULL, NULL);
    g_attr.nbytes = (off_t)size;
    id = resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0, &g_connect, &g_io, &g_attr);
    if (id < 0) { fprintf(stderr, "resmgr_attach(%s) failed: %s\n", path, strerror(errno)); return 5; }
    logf2(g_live ? "attached-live" : "attached-loop", (long long)g_len, size);

    if (g_live && pthread_create(&writer, NULL, writer_thread, NULL)) {
        fprintf(stderr, "writer thread failed\n");
        resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
        return 8;
    }

    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.context_free = dispatch_context_free;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.lo_water = 3;
    pattr.hi_water = 6;
    pattr.increment = 1;
    pattr.maximum = 12;
    tpp = thread_pool_create(&pattr, 0);
    if (!tpp) { fprintf(stderr, "thread_pool_create failed\n"); resmgr_detach(dpp, id, _RESMGR_DETACH_ALL); return 6; }
    start = last_stat = time(NULL);
    if (thread_pool_start(tpp) < 0) { fprintf(stderr, "thread_pool_start failed\n"); resmgr_detach(dpp, id, _RESMGR_DETACH_ALL); return 7; }
    if (ready) { int fd = open(ready, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) close(fd); }
    while (!g_stop && (seconds == 0 || (long)(time(NULL) - start) < seconds)) {
        sleep(1);
        if (g_live && time(NULL) - last_stat >= 5) {
            last_stat = time(NULL);
            pthread_mutex_lock(&g_mx);
            logf2("live-stat end", (long long)g_end, (long long)g_idr_start);
            pthread_mutex_unlock(&g_mx);
        }
    }
    g_stop = 1;
    resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
    logf2("detached opens", g_opens, (long long)g_bytes);
    fprintf(stderr, "stats opens=%u closes=%u reads=%u seeks=%u skips=%u bytes=%llu\n", g_opens, g_closes, g_reads, g_seeks, g_skips, g_bytes);
    return 0;
}
