/*
 * nbt_wal_present - show the decoded Android Auto picture in front of the HMI through the WAL sprite interface.
 *
 * Input: raw I420 (YUV420P) frames of 800x480, 576000 bytes each, on stdin (the decoder's raw.fifo) or on --fifo.
 * A reader thread drains the pipe continuously and keeps only the newest frame (bounded latency); the presenter converts
 * it to RGBA and replaces the contents of ONE persistent sprite (an existing sprite id keeps its assignment, position,
 * size and visibility, so there is no flicker gap). The sprite is assigned to the HMI window (0x100). With --mode center
 * the picture sits in the middle of the 1280x480 display on a black background sprite, with --mode full it is
 * stretched over the whole width (the GPU scales, the texture may be smaller than the shown size).
 *
 * SAFETY (2026-11-06 incident): on the vehicle, sustained uploads faster than the WalServer render thread consumes them
 * deadlocked the server (its threads blocked on an internal mutex, the display froze, the car restarted). Therefore:
 *   - uploads are spaced: next upload not before  max(last start + 1000/max-fps, last end + settle-ms)
 *   - unchanged frames are not uploaded at all
 *   - one sprite per update is never larger than 1.6 MB (a 2.4 MB sprite blocked the client in the write)
 *   - a watchdog ends this process if one upload blocks for more than 6 s
 * Measured safe so far: 800x480 RGBA at 12.5 to 15 fps for 8-10 s runs. Defaults are deliberately lower.
 *
 * LOCK-FREE UPDATE PATH (default, --raw 1): libwal_nvidia's createSprite() holds the client mutex (impl+0x284) during
 * the whole ~1.5 MB write, and the client's receive thread needs the same mutex for every event the server forwards
 * (WalApplicationInterfaceImpl::processEventChanged). The server forwards events synchronously, so a forward that
 * arrives during our write can close a wait cycle (our write -> server reader -> server lock -> server forwarder ->
 * our receive thread -> our mutex). After the one-time setup through the library, frame updates are therefore written
 * directly to the library's own /dev/WAL descriptor (impl+0x280) in exactly the library's format, WITHOUT the mutex:
 *   {12, 0x40000007, size+0x400}  then  {24+size, 0x40000050, id, w, h, size, RGBA[size]}
 * (decoded from WalApplicationInterfaceImpl::createSprite, GLSerializer pads data to 8 bytes; size is a multiple of 8).
 *
 * usage: nbt_wal_present [--fifo PATH] [--mode center|full] [--max-fps N] [--settle-ms N] [--scale 100|80|60]
 *                        [--seconds N] [--idle S] [--raw 0|1]
 *   --fifo PATH    read this FIFO (created under /fs/ram3/ if missing) instead of stdin
 *   --max-fps N    at most N uploads per second (1..20, default 10)
 *   --settle-ms N  quiet time after an upload before the next one (0..500, default 40)
 *   --scale P      texture size in percent of 800x480: 100 (800x480), 80 (640x384), 60 (480x288), default 100
 *   --seconds N    stop after N seconds (0 = until EOF/SIGTERM)
 *   --idle S       hide the picture after S seconds without frames (default 5), show it again when frames return
 *   --raw 0|1      1 (default): frame updates bypass the library mutex (see LOCK-FREE UPDATE PATH), 0: library only
 *   --top N --rows M  use only rows N..N+M-1 of the frame (the area the phone draws in, set by NBT_VID_INSETS).
 *                  For the 1280x480 display: phone insets 0,180,0,0 and --mode full --rows 300 (800x300 -> 1280x480).
 * Log: stderr (statistics every 5 s). Only the WAL client connection and the sprite ids 0x7a30/0x7a31 are used.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IW 800
#define IH 480
#define FRAME_BYTES (IW * IH * 3 / 2)
#define SPRITE_ID 0x7a31UL
#define BG_ID 0x7a30UL
#define WINDOW_HMI 0x100UL
#define MAX_SPRITE_BYTES 1600000UL

typedef void *(*create_ptr_t)(void);
typedef void (*create_sprite_t)(void *self, unsigned long id, const void *data, unsigned long w, unsigned long h);
typedef void (*destroy_sprite_t)(void *self, unsigned long id);
typedef void (*assign_t)(void *self, unsigned long sprite, unsigned long window);
typedef void (*position_t)(void *self, unsigned long id, long x, long y);
typedef void (*size_t2)(void *self, unsigned long id, unsigned long w, unsigned long h);
typedef void (*visible_t)(void *self, unsigned long id, int visible);

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* ---- upload watchdog ---- */
static volatile double g_upload_start;  /* 0 when no upload is in progress */
static void *watchdog_thread(void *arg)
{
    (void)arg;
    for (;;) {
        double s = g_upload_start;
        usleep(500000);
        if (s != 0.0 && g_upload_start == s && now_ms() - s > 6000.0) {
            fprintf(stderr, "WATCHDOG: sprite upload blocked for more than 6 s, ending process\n");
            fflush(stderr);
            _exit(3);
        }
    }
    return NULL;
}

/* ---- frame hand-over: reader -> presenter (three rotating buffers, newest frame wins) ---- */
static unsigned char *g_buf[3];
static int g_fill = 0, g_ready = 1, g_work = 2;
static int g_has_new;
static unsigned long g_read_frames, g_skipped;
static int g_eof;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_fd = 0;

static void *reader_thread(void *arg)
{
    size_t used = 0;
    (void)arg;
    while (!g_stop) {
        fd_set fds;
        struct timeval tv = { 0, 200000 };
        ssize_t n;
        FD_ZERO(&fds);
        FD_SET(g_fd, &fds);
        if (select(g_fd + 1, &fds, NULL, NULL, &tv) < 0) { if (errno == EINTR) continue; break; }
        if (!FD_ISSET(g_fd, &fds)) continue;
        n = read(g_fd, g_buf[g_fill] + used, FRAME_BYTES - used);
        if (n == 0) { pthread_mutex_lock(&g_mx); g_eof = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mx); break; }
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        used += (size_t)n;
        if (used < FRAME_BYTES) continue;
        used = 0;
        pthread_mutex_lock(&g_mx);
        {
            int t = g_fill;
            g_fill = g_ready;
            g_ready = t;
        }
        if (g_has_new) g_skipped++;
        g_has_new = 1;
        g_read_frames++;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mx);
    }
    pthread_mutex_lock(&g_mx);
    g_eof = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

/* ---- I420 (BT.601 limited range) -> RGBA ---- */
static int t_y[256], t_rv[256], t_gu[256], t_gv[256], t_bu[256];
static unsigned char t_clip[1024];

static void init_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        t_y[i] = 298 * (i - 16) + 128;
        t_rv[i] = 409 * (i - 128);
        t_gu[i] = -100 * (i - 128);
        t_gv[i] = -208 * (i - 128);
        t_bu[i] = 516 * (i - 128);
    }
    for (i = 0; i < 1024; i++) {
        int v = i - 384;
        t_clip[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

/* content area of the 800x480 frame (--top/--rows): the phone draws only inside the area left free by the video
 * insets, e.g. bottom inset 180 -> rows 0..299 = 800x300 = exactly the 8:3 shape of the 1280x480 display */
static int g_top = 0, g_rows = IH;

/* full size: two pixels share one chroma pair */
static void i420_to_rgba_full(const unsigned char *yuv, unsigned char *out)
{
    const unsigned char *yp = yuv, *up = yuv + IW * IH, *vp = yuv + IW * IH * 5 / 4;
    int x, y;
    for (y = 0; y < g_rows; y++) {
        int sy = g_top + y;
        const unsigned char *yr = yp + sy * IW;
        const unsigned char *ur = up + (sy >> 1) * (IW / 2);
        const unsigned char *vr = vp + (sy >> 1) * (IW / 2);
        /* The WalServer sprite texture is read bottom-up (verified on the car): write the rows in reverse order. */
        unsigned char *o = out + (size_t)(g_rows - 1 - y) * IW * 4;
        for (x = 0; x < IW; x += 2) {
            int u = ur[x >> 1], v = vr[x >> 1];
            int rv = t_rv[v], gg = t_gu[u] + t_gv[v], bu = t_bu[u];
            int y0 = t_y[yr[x]], y1 = t_y[yr[x + 1]];
            o[0] = t_clip[((y0 + rv) >> 8) + 384];
            o[1] = t_clip[((y0 + gg) >> 8) + 384];
            o[2] = t_clip[((y0 + bu) >> 8) + 384];
            o[3] = 255;
            o[4] = t_clip[((y1 + rv) >> 8) + 384];
            o[5] = t_clip[((y1 + gg) >> 8) + 384];
            o[6] = t_clip[((y1 + bu) >> 8) + 384];
            o[7] = 255;
            o += 8;
        }
    }
}

/* scaled: nearest-neighbour sampling of an arbitrary tw x th target */
static int *g_xmap;
static void i420_to_rgba_scaled(const unsigned char *yuv, unsigned char *out, int tw, int th)
{
    const unsigned char *yp = yuv, *up = yuv + IW * IH, *vp = yuv + IW * IH * 5 / 4;
    int x, y;
    for (y = 0; y < th; y++) {
        int sy = g_top + y * g_rows / th;
        const unsigned char *yr = yp + sy * IW;
        const unsigned char *ur = up + (sy >> 1) * (IW / 2);
        const unsigned char *vr = vp + (sy >> 1) * (IW / 2);
        unsigned char *o = out + (size_t)(th - 1 - y) * tw * 4;   /* bottom-up texture, see i420_to_rgba_full */
        for (x = 0; x < tw; x++) {
            int sx = g_xmap[x];
            int u = ur[sx >> 1], v = vr[sx >> 1];
            int y0 = t_y[yr[sx]];
            o[0] = t_clip[((y0 + t_rv[v]) >> 8) + 384];
            o[1] = t_clip[((y0 + t_gu[u] + t_gv[v]) >> 8) + 384];
            o[2] = t_clip[((y0 + t_bu[u]) >> 8) + 384];
            o[3] = 255;
            o += 4;
        }
    }
}

/* write all bytes, retrying on EINTR (same behaviour as common::io::WalFifoOutputConnection::write) */
static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void *need(void *h, const char *name)
{
    void *p = dlsym(h, name);
    if (!p) { fprintf(stderr, "missing symbol %s\n", name); exit(3); }
    return p;
}

int main(int argc, char **argv)
{
    const char *fifo = NULL, *mode = "center";
    int max_fps = 10, settle_ms = 40, idle_s = 5, scale = 100, i;
    long seconds = 0;
    void *lib, *wal;
    create_ptr_t create_ptr;
    create_sprite_t create_sprite;
    destroy_sprite_t destroy_sprite;
    assign_t assign;
    position_t set_pos;
    size_t2 set_size;
    visible_t set_visible;
    unsigned char *rgba, black[16 * 16 * 4], *prev, *msg, *src;
    int have_prev = 0, tw, th, raw = 1, wal_fd = -1;
    unsigned long sprite_bytes;
    pthread_t reader, dog;
    double start, last_stat, last_start = 0, last_end = 0, period, hidden_since = 0;
    unsigned long presented = 0, win_presented = 0, unchanged = 0;
    double conv_sum = 0, up_sum = 0;
    int shown = 0, full;
    struct sigaction sa;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fifo")) fifo = argv[i + 1];
        else if (!strcmp(argv[i], "--mode")) mode = argv[i + 1];
        else if (!strcmp(argv[i], "--max-fps")) max_fps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--settle-ms")) settle_ms = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--scale")) scale = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--idle")) idle_s = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--raw")) raw = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--top")) g_top = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--rows")) g_rows = atoi(argv[i + 1]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (g_top < 0 || (g_top & 1) || g_rows < 120 || (g_rows & 1) || g_top + g_rows > IH) {
        fprintf(stderr, "--top and --rows must be even, --rows 120..480, top+rows <= 480\n");
        return 2;
    }
    if ((strcmp(mode, "center") && strcmp(mode, "full")) || max_fps < 1 || max_fps > 20 || settle_ms < 0 || settle_ms > 500 ||
        (scale != 100 && scale != 80 && scale != 60) || seconds < 0 || seconds > 86400 || idle_s < 1 || idle_s > 600 || (raw != 0 && raw != 1) ||
        (fifo && (strncmp(fifo, "/fs/ram3/", 9) || strstr(fifo, "..")))) {
        fprintf(stderr, "usage: %s [--fifo /fs/ram3/..] [--mode center|full] [--max-fps 1..20] [--settle-ms 0..500] [--scale 100|80|60] [--seconds N] [--idle S]\n", argv[0]);
        return 2;
    }
    full = !strcmp(mode, "full");
    period = 1000.0 / max_fps;
    tw = IW * scale / 100;
    th = g_rows * scale / 100;
    if ((unsigned long)tw * th * 4 > MAX_SPRITE_BYTES) { fprintf(stderr, "texture too large\n"); return 2; }
    for (i = 0; i < 3; i++) {
        g_buf[i] = malloc(FRAME_BYTES);
        if (!g_buf[i]) return 3;
    }
    prev = malloc(FRAME_BYTES);
    sprite_bytes = (unsigned long)tw * th * 4;   /* multiple of 8 for all allowed scales: no serializer padding */
    msg = malloc(24 + (size_t)sprite_bytes);     /* {len, 0x40000050, id, w, h, size} + RGBA, see header comment */
    rgba = msg ? msg + 24 : NULL;
    g_xmap = malloc(sizeof(int) * (size_t)tw);
    if (!prev || !msg || !g_xmap) return 3;
    for (i = 0; i < tw; i++) g_xmap[i] = i * IW / tw;
    for (i = 0; i < 16 * 16; i++) { black[i * 4] = 0; black[i * 4 + 1] = 0; black[i * 4 + 2] = 0; black[i * 4 + 3] = 255; }
    init_tables();
    if (fifo) {
        struct stat st;
        if (stat(fifo, &st) != 0) mkfifo(fifo, 0600);
        g_fd = open(fifo, O_RDWR | O_NONBLOCK);
        if (g_fd < 0) { fprintf(stderr, "cannot open %s\n", fifo); return 3; }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    lib = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 3; }
    create_ptr = (create_ptr_t)need(lib, "_ZN3wal23WalApplicationInterface9createPtrEv");
    create_sprite = (create_sprite_t)need(lib, "_ZN3wal23WalApplicationInterface12createSpriteEmPKvmm");
    destroy_sprite = (destroy_sprite_t)need(lib, "_ZN3wal23WalApplicationInterface13destroySpriteEm");
    assign = (assign_t)need(lib, "_ZN3wal23WalApplicationInterface20assignSpriteToWindowEmm");
    set_pos = (position_t)need(lib, "_ZN3wal23WalApplicationInterface17setSpritePositionEmll");
    set_size = (size_t2)need(lib, "_ZN3wal23WalApplicationInterface13setSpriteSizeEmmm");
    set_visible = (visible_t)need(lib, "_ZN3wal23WalApplicationInterface16setSpriteVisibleEmb");
    wal = create_ptr();
    if (!wal) { fprintf(stderr, "createPtr returned NULL\n"); return 3; }

    if (pthread_create(&reader, NULL, reader_thread, NULL)) { fprintf(stderr, "reader thread failed\n"); return 3; }
    if (pthread_create(&dog, NULL, watchdog_thread, NULL)) { fprintf(stderr, "watchdog thread failed\n"); return 3; }
    fprintf(stderr, "nbt_wal_present: mode=%s max-fps=%d settle=%dms scale=%d%% content rows %d..%d texture %dx%d idle=%ds input=%s\n",
            mode, max_fps, settle_ms, scale, g_top, g_top + g_rows - 1, tw, th, idle_s, fifo ? fifo : "stdin");
    start = last_stat = now_ms();
    while (!g_stop && (seconds == 0 || now_ms() - start < seconds * 1000.0)) {
        int idx = -1;
        struct timespec ts;
        double wait_until = last_start ? last_start + period : 0.0, w2 = last_end ? last_end + settle_ms : 0.0;
        if (w2 > wait_until) wait_until = w2;
        if (wait_until > now_ms()) {
            /* quiet time: never queue another upload before the server had time to consume the last one */
            double d = wait_until - now_ms();
            usleep((useconds_t)(d * 1000.0));
        }
        pthread_mutex_lock(&g_mx);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250 * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        while (!g_has_new && !g_eof && !g_stop) {
            if (pthread_cond_timedwait(&g_cv, &g_mx, &ts) != 0) break;
        }
        if (g_has_new) {
            int t = g_work;
            g_work = g_ready;
            g_ready = t;
            g_has_new = 0;
            idx = g_work;
        }
        if (!g_has_new && g_eof && idx < 0) { pthread_mutex_unlock(&g_mx); break; }
        pthread_mutex_unlock(&g_mx);
        /* HMI page control: while /fs/ram3/nbt-aa-hidden exists (written by the Android Auto plugin page when it loses
         * focus) the overlay is hidden and nothing is uploaded; it comes back with the next frame once the file is gone. */
        /* debounce 300 ms: BACK pops iDrive from the hold state for a moment and the plugin puts it straight back */
        if (access("/fs/ram3/nbt-aa-hidden", F_OK) != 0) hidden_since = 0;
        else if (hidden_since == 0) hidden_since = now_ms();
        if (hidden_since != 0 && now_ms() - hidden_since >= 300.0) {
            if (shown) {
                set_visible(wal, SPRITE_ID, 0);
                if (!full) set_visible(wal, BG_ID, 0);
                shown = 0;
                fprintf(stderr, "hidden by HMI page\n");
            }
            /* keep the newest picture so a static phone screen reappears at once when the page is opened again */
            if (idx >= 0) { memcpy(prev, g_buf[idx], FRAME_BYTES); have_prev = 1; }
            else usleep(50000);
            continue;
        }
        /* source: the new frame, or (page just reopened, no new frame) the last known picture */
        src = idx >= 0 ? g_buf[idx] : (!shown && have_prev ? prev : NULL);
        if (src && src != prev && have_prev && shown && memcmp(src, prev, FRAME_BYTES) == 0) {
            unchanged++;           /* static screen: nothing to upload */
            last_start = 0;
            last_end = 0;
        } else if (src) {
            double a, b, c;
            a = now_ms();
            if (scale == 100) i420_to_rgba_full(src, rgba);
            else i420_to_rgba_scaled(src, rgba, tw, th);
            b = now_ms();
            if (!shown) {
                if (!full) {
                    create_sprite(wal, BG_ID, black, 16, 16);
                    assign(wal, BG_ID, WINDOW_HMI);
                    set_pos(wal, BG_ID, 0, 0);
                    set_size(wal, BG_ID, 1280, 480);
                    set_visible(wal, BG_ID, 1);
                }
            }
            g_upload_start = now_ms();
            if (shown && raw && wal_fd >= 0) {
                /* lock-free update: same two messages as createSprite(), written without the library mutex */
                unsigned long pre[3], *h = (unsigned long *)msg;
                pre[0] = 12; pre[1] = 0x40000007UL; pre[2] = sprite_bytes + 0x400;
                h[0] = 24 + sprite_bytes; h[1] = 0x40000050UL; h[2] = SPRITE_ID; h[3] = (unsigned long)tw; h[4] = (unsigned long)th; h[5] = sprite_bytes;
                if (write_all(wal_fd, pre, sizeof(pre)) || write_all(wal_fd, msg, 24 + sprite_bytes)) {
                    fprintf(stderr, "raw write failed (%s), falling back to the library\n", strerror(errno));
                    raw = 0;
                }
            } else {
                create_sprite(wal, SPRITE_ID, rgba, (unsigned long)tw, (unsigned long)th);
            }
            g_upload_start = 0.0;
            if (wal_fd < 0 && raw) {
                /* the wrapper object holds the impl pointer; impl+0x280 is the /dev/WAL descriptor (-1 if not open) */
                void *impl = *(void **)wal;
                wal_fd = impl ? *(int *)((char *)impl + 0x280) : -1;
                fprintf(stderr, "lock-free updates %s (fd=%d)\n", wal_fd >= 0 ? "enabled" : "NOT available", wal_fd);
                if (wal_fd < 0) raw = 0;
            }
            c = now_ms();
            if (!shown) {
                assign(wal, SPRITE_ID, WINDOW_HMI);
                /* full: content area stretched over the display (exact for 800x300 -> 1280x480, factor 1.6 both ways);
                 * center: content area 1:1 in the middle */
                set_pos(wal, SPRITE_ID, full ? 0 : 240, full ? 0 : (480 - g_rows) / 2);
                set_size(wal, SPRITE_ID, full ? 1280 : 800, full ? 480 : (unsigned long)g_rows);
                set_visible(wal, SPRITE_ID, 1);
                shown = 1;
                fprintf(stderr, "picture shown\n");
            }
            if (src != prev) memcpy(prev, src, FRAME_BYTES);
            have_prev = 1;
            conv_sum += b - a;
            up_sum += c - b;
            last_start = a;
            last_end = c;
            presented++;
            win_presented++;
        } else if (shown && last_end && now_ms() - last_end > idle_s * 1000.0) {
            set_visible(wal, SPRITE_ID, 0);
            if (!full) set_visible(wal, BG_ID, 0);
            shown = 0;
            have_prev = 0;
            fprintf(stderr, "no frames for %d s: picture hidden\n", idle_s);
        }
        if (now_ms() - last_stat >= 5000.0) {
            double dt = (now_ms() - last_stat) / 1000.0;
            pthread_mutex_lock(&g_mx);
            fprintf(stderr, "stat read=%lu skipped=%lu unchanged=%lu presented=%lu (%.1f fps) convert=%.1f ms upload=%.1f ms\n",
                    g_read_frames, g_skipped, unchanged, presented, win_presented / dt,
                    win_presented ? conv_sum / win_presented : 0.0, win_presented ? up_sum / win_presented : 0.0);
            pthread_mutex_unlock(&g_mx);
            win_presented = 0;
            conv_sum = up_sum = 0;
            last_stat = now_ms();
        }
    }
    g_stop = 1;
    pthread_join(reader, NULL);
    set_visible(wal, SPRITE_ID, 0);
    destroy_sprite(wal, SPRITE_ID);
    if (!full) { set_visible(wal, BG_ID, 0); destroy_sprite(wal, BG_ID); }
    usleep(300000);
    fprintf(stderr, "nbt_wal_present: end read=%lu skipped=%lu unchanged=%lu presented=%lu\n", g_read_frames, g_skipped, unchanged, presented);
    return 0;
}
