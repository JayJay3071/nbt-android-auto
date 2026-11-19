/* nbt_decoder: Android Auto's picture through the car's own video path (2026-11-19).
 *
 * The phone's H.264 (Annex B, from native_service's FIFO) is decoded on the CPU with FFmpeg 2.8 (H.264 only, with its
 * SIMD assembly, scripts/ffmpeg_qnx.py) and handed to libnmsdk's INvVideo, which converts every picture into its own
 * NV24 surfaces and composites the stream into iDrive's EARLYVIDEO layer; libwal_nvidia places, raises and shows it.
 *
 *   nbt_decoder --input FIFO [--ready FILE] [--size WxH] [--fps 30|60]
 *   nbt_decoder --bench FILE         decode only, as fast as possible, and print the rate (no display needed)
 *
 *   --input  Annex B stream; one writer per Android Auto session, the program stays for the next one
 *   --ready  created whenever the program waits for a writer (native_service starts the session after it)
 *   --size   window size on the display (default 1280x480; split screen 800x480)
 *   --fps    stream rate given to the car (default 30): picture timestamps and how long the car holds each picture
 * Files: decoder.stale in the current folder (ms without a new picture before the window is hidden, default 5000,
 *   0 = never); in /var/opt/hmi/data/rw/NbtAndroidAutoRT: decoder.timeout (NV_NMSDK_TASK_TIMEOUT_MS, default 3000),
 *   decoder.repeat (ms without a new picture before the last one is handed to the car again, default 50, 0 = never).
 *
 * Threads
 *   main, inside NvKdLaunch: reads the FIFO, cuts it into access units, decodes, writes the picture as NV12 into a
 *     three-picture mailbox (the newest picture replaces one not yet taken); runs the window policy every <= 100 ms
 *   display: takes the newest picture and, while the window is shown, uploads it (VideoFrame) and presents it
 *     (VideoRenderFrame); with nothing new for decoder.repeat ms, the last picture again (the car fades the window out
 *     when no picture comes for a short while: still screens such as YouTube Music flickered)
 * Window policy: shown while the Android Auto page is in front (no /fs/ram3/nbt-aa-hidden), the session runs
 *   (/fs/ram3/nbt-aa-state says state=running and is at most 5 s old; a stop counts after 1.5 s) and pictures keep
 *   coming (decoder.stale). Hiding waits 400 ms (short page changes do not blink); the window is set again every 2 s.
 *   Nothing is shown before the session's first picture.
 *
 * The car's interface, as libnmsdk.so (2014) exports it (CNvVideoBase with full C++ signatures):
 *   INvVideo::GetNvVideo() -> object; virtual methods 0 Init, 2 VideoOpen(void** h, ICallback*, INvAvSync*,
 *   CConfDecoder*), 4 VideoConfigure(h, const CConfVideo&), 5 VideoFrame(h, EVideoFormat, const uint8_t** planes,
 *   uint32_t* pitches, int64_t pts, EPictureFormat, void*), 12/13 SetVideoSource/SetVideoDestination(h, const CRect&),
 *   14 VideoBufferCreate(void** b, w, h, float, bool), 16 VideoRenderFrame(buffer, h, CVideoRenderFrameParams*),
 *   17 VideoStreamCreate(h, void** buffers, n). All calls are queued to the library's worker thread and wait for it
 *   (NV_NMSDK_TASK_TIMEOUT_MS).
 *   - CConfVideo (OnVideoConfigure): width, height, two floats (the second the frame rate: below 32 each picture is
 *     held for two display refreshes, so 30 caps the stream at 30 pictures/s), decoder type in word 5 (0 none:
 *     pictures come with VideoFrame), a 64-bit value in words 9-10, surface count (1-20) in word 11, stream id in
 *     word 12 (stream property 0x1789 of kdCreateStreamNV)
 *   - VideoFrame: EVideoFormat 1 = NV12 (2/3 YV12, 4/5 packed 4:2:2); the library takes a free NV24 surface (it
 *     waits for one, and surfaces come back only once the window is composited) and converts into it on the CPU
 *   - VideoRenderFrame params: word 0 picture structure (3 = frame), surface 0 = the one VideoFrame just filled
 *   - window layers (libwal_nvidia LayerId): 1024 EARLYVIDEO, 1052 VIDEO; the stream with id -28 lands in EARLYVIDEO
 *     (1052 - 28), which iDrive leaves alone while Android Auto is in front (car 2026-11-19)
 * The program ends with _exit(): returning through NvKdLaunch crashed in the libraries' cleanup (car 2026-11-19).
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "libavcodec/avcodec.h"
#include "libavutil/cpu.h"

#define RT_DIR "/var/opt/hmi/data/rw/NbtAndroidAutoRT"
#define HIDDEN_FILE "/fs/ram3/nbt-aa-hidden"
#define STATE_FILE "/fs/ram3/nbt-aa-state"
#define LAYER_VIDEO 1052
#define STREAM_ID (-28)   /* -> layer 1024, EARLYVIDEO */

/* ---- the car's interfaces ---- */

struct conf_video {   /* INvVideo::CConfVideo, 13 words */
    uint32_t width, height;
    float aspect, fps;
    uint32_t word4, decoder, word6, word7, word8, value_lo, value_hi, surfaces;
    int32_t stream_id;
};
struct rect {
    uint32_t left, top, right, bottom;
};

typedef int (*nv_init_fn)(void*);
typedef int (*nv_open_fn)(void*, void**, void*, void*, void*);
typedef int (*nv_configure_fn)(void*, void*, const struct conf_video*);
typedef int (*nv_frame_fn)(void*, void*, int, const uint8_t* const*, const uint32_t*, int64_t, int, void*);
typedef int (*nv_rect_fn)(void*, void*, const struct rect*);
typedef int (*nv_buffer_fn)(void*, void**, uint32_t, uint32_t, float, int);
typedef int (*nv_render_fn)(void*, void*, void*, uint8_t*);
typedef int (*nv_stream_fn)(void*, void*, void**, uint32_t);
typedef void* (*wal_create_fn)(void);
typedef void (*wal_place_fn)(void*, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
typedef void (*wal_order_fn)(void*, const void*, unsigned long);
typedef void (*wal_visible_fn)(void*, unsigned long, int);

enum { NV_INIT = 0, NV_OPEN = 2, NV_CONFIGURE = 4, NV_FRAME = 5, NV_SOURCE = 12, NV_DEST = 13, NV_BUFFER = 14,
       NV_RENDER = 16, NV_STREAM = 17 };
enum { FORMAT_NV12 = 1, PICTURE_FRAME = 3, BUFFERS = 3, SURFACES = 4 };

static void* nv_method(void* obj, int index) { return (*(void***)obj)[index]; }

/* ICallback for VideoOpen: nothing to report with decoder type 0, every method returns 0 */
static int callback_nothing(void) { return 0; }
static void* g_callback_methods[32];
static struct { void** methods; } g_callback = {g_callback_methods};

/* ---- state ---- */

static struct {
    const char* input;
    const char* ready;
    int width, height, fps, bench;
    long stale_ms, repeat_ms;
} g_opt = {0, 0, 1280, 480, 30, 0, 5000, 50};

static volatile sig_atomic_t g_quit;

static struct {
    void* nv;
    void* stream;
    void* buffers[BUFFERS];
    int w, h, ready;
    void* wal;
    wal_place_fn place;
    wal_order_fn order;
    wal_visible_fn visible;
    unsigned long window;
} g_car;

/* three NV12 pictures: one being written, the newest finished one, one being shown */
struct picture {
    uint8_t* y;
    uint8_t* uv;
    unsigned long number;
};
static struct {
    struct picture pic[3];
    int writing, newest, showing, fresh;
    pthread_mutex_t lock;
    pthread_cond_t posted;
} g_box = {.writing = 0, .newest = 1, .showing = 2, .lock = PTHREAD_MUTEX_INITIALIZER,
           .posted = PTHREAD_COND_INITIALIZER};

static struct {
    volatile int shown;   /* -1 not set yet */
    long long refresh_at, hide_from, last_picture, state_read, stopped_since;
    int running, stale_said;
} g_win = {-1, 0, 0, 0, 0, 0, 1, 0};

/* statistics per interval (the display thread's part under g_box.lock) */
static struct {
    long long since;
    unsigned long decoded, errors, replaced, shown, hidden, repeated;
    long long decode_us, decode_max, upload_us, upload_max;
    unsigned long total;
} g_stat;

static AVCodecContext* g_dec;
static AVFrame* g_frame;

/* ---- helpers ---- */

static long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static long long now_ms(void) { return now_us() / 1000; }

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
    char line[400];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    const long long ms = now_ms();
    printf("%5lld.%03lld %s\n", ms / 1000 % 100000, ms % 1000, line);
    fflush(stdout);
}

static long read_number(const char* path, long fallback)
{
    long v = fallback;
    FILE* f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = fallback;
        fclose(f);
    }
    return v;
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ---- car: video and window ---- */

static int car_open(void)
{
    void* lib = dlopen("libnmsdk.so", RTLD_NOW | RTLD_GLOBAL);
    void* (*get)(void) = lib ? (void* (*)(void))dlsym(lib, "_ZN8INvVideo10GetNvVideoEv") : 0;
    g_car.nv = get ? get() : 0;
    if (!g_car.nv) {
        say("video: libnmsdk.so not usable (%s)", lib ? "no INvVideo" : dlerror());
        return -1;
    }
    const int r = ((nv_init_fn)nv_method(g_car.nv, NV_INIT))(g_car.nv);
    if (r < 0) {
        say("video: Init failed 0x%08x", r);
        return -1;
    }

    void* wal = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!wal)
        wal = dlopen("libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    wal_create_fn create = wal ? (wal_create_fn)dlsym(wal, "_ZN3wal15WalHmiInterface9createPtrEv") : 0;
    if (create) {
        g_car.place = (wal_place_fn)dlsym(wal, "_ZN3wal15WalHmiInterface24setWindowPositionAndSizeEmmmmm");
        g_car.order = (wal_order_fn)dlsym(wal, "_ZN3wal15WalHmiInterface9setZOrderEPKNS0_12OrderTypeDefEm");
        g_car.visible = (wal_visible_fn)dlsym(wal, "_ZN3wal15WalHmiInterface10setVisibleEmb");
        if (g_car.place && g_car.order && g_car.visible)
            g_car.wal = create();
    }
    if (!g_car.wal)
        say("window: libwal_nvidia.so not usable, the picture cannot be shown");
    g_car.window = (unsigned long)(LAYER_VIDEO + STREAM_ID);
    return 0;
}

/* the stream, sized like the first picture */
static int car_stream(int w, int h)
{
    for (int i = 0; i < 32; ++i)
        g_callback_methods[i] = (void*)callback_nothing;
    static uint8_t decoder_conf[32];
    void* nv = g_car.nv;
    int r = ((nv_open_fn)nv_method(nv, NV_OPEN))(nv, &g_car.stream, &g_callback, 0, decoder_conf);
    if (r < 0 || !g_car.stream) {
        say("video: VideoOpen failed 0x%08x", r);
        return -1;
    }
    const struct conf_video conf = {(uint32_t)w, (uint32_t)h, 1.0f, (float)g_opt.fps, 0, 0, 0, 0, 0, 50000000u, 0,
                                    SURFACES, STREAM_ID};
    r = ((nv_configure_fn)nv_method(nv, NV_CONFIGURE))(nv, g_car.stream, &conf);
    if (r < 0) {
        say("video: VideoConfigure failed 0x%08x", r);
        return -1;
    }
    for (int i = 0; i < BUFFERS; ++i) {
        r = ((nv_buffer_fn)nv_method(nv, NV_BUFFER))(nv, &g_car.buffers[i], (uint32_t)w, (uint32_t)h, 0.0f, 0);
        if (r < 0) {
            say("video: VideoBufferCreate failed 0x%08x", r);
            return -1;
        }
    }
    r = ((nv_stream_fn)nv_method(nv, NV_STREAM))(nv, g_car.stream, g_car.buffers, BUFFERS);
    if (r < 0) {
        say("video: VideoStreamCreate failed 0x%08x", r);
        return -1;
    }
    const struct rect all = {0, 0, (uint32_t)w, (uint32_t)h};
    ((nv_rect_fn)nv_method(nv, NV_SOURCE))(nv, g_car.stream, &all);
    ((nv_rect_fn)nv_method(nv, NV_DEST))(nv, g_car.stream, &all);
    g_car.w = w;
    g_car.h = h;
    g_car.ready = 1;
    say("video: stream %dx%d at %d fps, id %d (window %lu, %dx%d on the display)", w, h, g_opt.fps, STREAM_ID,
        g_car.window, g_opt.width, g_opt.height);
    return 0;
}

static void window_set(int show)
{
    if (!g_car.wal)
        return;
    if (show) {
        const struct { uint32_t id; uint16_t a, b; } order = {(uint32_t)g_car.window, 0, 0};
        g_car.place(g_car.wal, g_car.window, 0, 0, (unsigned long)g_opt.width, (unsigned long)g_opt.height);
        g_car.order(g_car.wal, &order, 1);
    }
    g_car.visible(g_car.wal, g_car.window, show);
}

/* upload and present one picture; the buffer turns over with every picture the car took */
static void car_present(const struct picture* p)
{
    static unsigned long turn, presented;
    const uint8_t* planes[3] = {p->y, p->uv, 0};
    const uint32_t pitches[3] = {(uint32_t)g_car.w, (uint32_t)g_car.w, 0};
    const int64_t pts = (int64_t)++presented * 1000000 / g_opt.fps;   /* every hand-over, repeats too */
    const long long t0 = now_us();
    int r = ((nv_frame_fn)nv_method(g_car.nv, NV_FRAME))(g_car.nv, g_car.stream, FORMAT_NV12, planes, pitches, pts,
                                                         PICTURE_FRAME, 0);
    const long long dt = now_us() - t0;
    if (r < 0) {
        say("video: VideoFrame failed 0x%08x after %lld ms (picture %lu)", r, dt / 1000, p->number);
        return;
    }
    uint8_t params[64];
    memset(params, 0, sizeof(params));
    params[0] = PICTURE_FRAME;
    params[0x14] = 1;   /* set on the car since the first working picture; its meaning is not traced in the library */
    r = ((nv_render_fn)nv_method(g_car.nv, NV_RENDER))(g_car.nv, g_car.buffers[turn++ % BUFFERS], g_car.stream, params);
    if (r < 0)
        say("video: VideoRenderFrame failed 0x%08x (picture %lu)", r, p->number);
    pthread_mutex_lock(&g_box.lock);
    ++g_stat.shown;
    g_stat.upload_us += dt;
    if (dt > g_stat.upload_max)
        g_stat.upload_max = dt;
    pthread_mutex_unlock(&g_box.lock);
}

/* ---- display thread ---- */

/* the newest picture; with nothing new for repeat_ms while the window is shown, the last one again: the car fades the
 * window out when no picture comes for a short while (car 2026-11-19: YouTube Music, ~6 pictures/s, the picture and
 * iDrive's page alternating about twice a second; Waze at ~30/s was steady) */
static void* display_thread(void* arg)
{
    (void)arg;
    int have = 0;
    for (;;) {
        pthread_mutex_lock(&g_box.lock);
        int repeat = 0;
        while (!g_box.fresh && !g_quit && !repeat) {
            if (!have || g_win.shown != 1 || g_opt.repeat_ms <= 0) {
                pthread_cond_wait(&g_box.posted, &g_box.lock);
                continue;
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += g_opt.repeat_ms % 1000 * 1000000;
            until.tv_sec += g_opt.repeat_ms / 1000 + until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            if (pthread_cond_timedwait(&g_box.posted, &g_box.lock, &until) == ETIMEDOUT && !g_box.fresh)
                repeat = 1;
        }
        if (g_quit) {
            pthread_mutex_unlock(&g_box.lock);
            return 0;
        }
        if (repeat)
            ++g_stat.repeated;
        else {
            const int take = g_box.newest;
            g_box.newest = g_box.showing;
            g_box.showing = take;
            g_box.fresh = 0;
            have = 1;
            if (g_win.shown != 1)
                ++g_stat.hidden;
        }
        const int show = g_box.showing;
        pthread_mutex_unlock(&g_box.lock);
        if (g_win.shown == 1)
            car_present(&g_box.pic[show]);
    }
}

static void post_picture(void)
{
    static int started;
    pthread_mutex_lock(&g_box.lock);
    const int done = g_box.writing;
    g_box.writing = g_box.newest;
    g_box.newest = done;
    if (g_box.fresh)
        ++g_stat.replaced;
    g_box.fresh = 1;
    pthread_cond_signal(&g_box.posted);
    pthread_mutex_unlock(&g_box.lock);
    if (!started) {
        pthread_t t;
        started = 1;
        if (pthread_create(&t, 0, display_thread, 0) != 0) {
            say("display thread could not start");
            g_quit = 1;
        }
    }
}

/* ---- window policy ---- */

static int session_running(long long now)
{
    if (now - g_win.state_read >= 500) {
        g_win.state_read = now;
        int running = 0;
        struct stat st;
        if (stat(STATE_FILE, &st) == 0 && time(0) - st.st_mtime <= 5) {
            char text[200] = "";
            const int fd = open(STATE_FILE, O_RDONLY);
            if (fd >= 0) {
                const ssize_t n = read(fd, text, sizeof(text) - 1);
                text[n > 0 ? n : 0] = 0;
                close(fd);
            }
            running = strstr(text, "state=running") != 0;
        }
        if (running) {
            g_win.stopped_since = 0;
            if (!g_win.running)
                say("session: running");
            g_win.running = 1;
        } else {
            if (!g_win.stopped_since)
                g_win.stopped_since = now;
            if (g_win.running && now - g_win.stopped_since >= 1500) {
                say("session: not running, window hidden");
                g_win.running = 0;
            }
        }
    }
    return g_win.running;
}

static void window_policy(void)
{
    const long long now = now_ms();
    /* a picture of this session first: the stream still holds the previous session's last one */
    int show = g_win.last_picture && access(HIDDEN_FILE, F_OK) != 0 && session_running(now);
    if (show && g_opt.stale_ms && now - g_win.last_picture >= g_opt.stale_ms) {
        if (!g_win.stale_said)
            say("window: no picture for %ld ms, hidden", g_opt.stale_ms);
        g_win.stale_said = 1;
        show = 0;
    }
    if (show)
        g_win.hide_from = 0;
    else {
        if (!g_win.hide_from)
            g_win.hide_from = now + 400;
        if (g_win.shown == 1 && now < g_win.hide_from)
            show = 1;   /* not yet */
    }
    if (!g_car.ready || (show == g_win.shown && now < g_win.refresh_at))
        return;
    if (show != g_win.shown)
        say("window: %s", show ? "shown" : "hidden");
    window_set(show);
    g_win.shown = show;
    g_win.refresh_at = now + 2000;
}

/* ---- decoding ---- */

static void stats_maybe(void)
{
    const long long now = now_ms();
    if (!g_stat.since)
        g_stat.since = now;
    if (now - g_stat.since < 5000)
        return;
    pthread_mutex_lock(&g_box.lock);
    const double secs = (now - g_stat.since) / 1000.0;
    if (g_stat.decoded || g_stat.errors)
        say("stats %.1f s: %lu decoded (%.1f/s), %lu shown (%lu repeats), %lu replaced, %lu while hidden, %lu errors; "
            "decode %.1f ms (max %.1f), upload %.1f ms (max %.1f)", secs, g_stat.decoded, g_stat.decoded / secs,
            g_stat.shown, g_stat.repeated, g_stat.replaced, g_stat.hidden, g_stat.errors,
            g_stat.decoded ? g_stat.decode_us / 1000.0 / g_stat.decoded : 0.0, g_stat.decode_max / 1000.0,
            g_stat.shown ? g_stat.upload_us / 1000.0 / g_stat.shown : 0.0, g_stat.upload_max / 1000.0);
    const unsigned long total = g_stat.total;
    memset(&g_stat, 0, sizeof(g_stat));
    g_stat.total = total;
    g_stat.since = now;
    pthread_mutex_unlock(&g_box.lock);
}

static int decoder_open(void)
{
    avcodec_register_all();
    AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    g_dec = codec ? avcodec_alloc_context3(codec) : 0;
    g_frame = av_frame_alloc();
    if (!g_dec || !g_frame)
        return -1;
    g_dec->thread_count = 1;
    g_dec->flags |= AV_CODEC_FLAG_LOW_DELAY;   /* a picture out for every picture in */
    if (avcodec_open2(g_dec, codec, 0) < 0)
        return -1;
    const int cpu = av_get_cpu_flags();
    say("decoder: FFmpeg %s H.264, SIMD%s%s%s", av_version_info(), cpu & AV_CPU_FLAG_SSE2 ? " SSE2" : "",
        cpu & AV_CPU_FLAG_SSSE3 ? " SSSE3" : "", cpu & AV_CPU_FLAG_SSE4 ? " SSE4.1" : "");
    return 0;
}

/* I420 -> NV12 into the picture being written */
static void to_nv12(const AVFrame* f, struct picture* p)
{
    for (int row = 0; row < f->height; ++row)
        memcpy(p->y + (size_t)row * g_car.w, f->data[0] + (size_t)row * f->linesize[0], (size_t)f->width);
    for (int row = 0; row < f->height / 2; ++row) {
        const uint8_t* u = f->data[1] + (size_t)row * f->linesize[1];
        const uint8_t* v = f->data[2] + (size_t)row * f->linesize[2];
        uint8_t* out = p->uv + (size_t)row * g_car.w;
        for (int x = 0; x < f->width / 2; ++x) {
            out[0] = u[x];
            out[1] = v[x];
            out += 2;
        }
    }
}

static void decode(uint8_t* data, size_t size)
{
    if (!g_dec && decoder_open() < 0) {
        say("decoder: FFmpeg H.264 not available");
        g_quit = 1;
        return;
    }
    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = data;
    pkt.size = (int)size;
    int got = 0;
    const long long t0 = now_us();
    const int r = avcodec_decode_video2(g_dec, g_frame, &got, &pkt);
    const long long dt = now_us() - t0;
    g_stat.decode_us += dt;
    if (dt > g_stat.decode_max)
        g_stat.decode_max = dt;
    if (r < 0) {
        ++g_stat.errors;
        return;
    }
    if (!got)
        return;
    ++g_stat.decoded;
    ++g_stat.total;
    const AVFrame* f = g_frame;
    if (g_opt.bench)
        return;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        ++g_stat.errors;
        return;
    }
    if (!g_car.ready) {
        say("decoder: first picture %dx%d", f->width, f->height);
        if (car_stream(f->width & ~1, f->height & ~1) != 0) {
            g_quit = 1;
            return;
        }
        for (int i = 0; i < 3; ++i) {
            g_box.pic[i].y = (uint8_t*)malloc((size_t)g_car.w * g_car.h);
            g_box.pic[i].uv = (uint8_t*)malloc((size_t)g_car.w * g_car.h / 2);
            if (!g_box.pic[i].y || !g_box.pic[i].uv) {
                say("out of memory");
                g_quit = 1;
                return;
            }
        }
    }
    if (f->width > g_car.w || f->height > g_car.h) {
        ++g_stat.errors;   /* larger than the stream set up for the first picture */
        return;
    }
    const long long now = now_ms();
    if (g_win.last_picture && now - g_win.last_picture >= 1000)
        say("decoder: %lld ms without a picture%s", now - g_win.last_picture, g_win.stale_said ? ", shown again" : "");
    g_win.last_picture = now;
    g_win.stale_said = 0;
    struct picture* p = &g_box.pic[g_box.writing];
    to_nv12(f, p);
    p->number = g_stat.total;
    post_picture();
}

/* ---- Annex B -> access units ---- */

enum { INPUT_SIZE = 1 << 20, AU_SIZE = 1 << 20 };
static uint8_t g_input[INPUT_SIZE];
static uint8_t g_au[AU_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
static size_t g_au_len;
static int g_au_slices;

/* a NAL unit with its start code; a new access unit starts with AUD, SPS, PPS, SEI or types 14-18 after a slice, or
 * with a slice whose first_mb_in_slice is 0 (its first bit is 1) */
static void nal_unit(const uint8_t* nal, size_t len)
{
    const size_t head = nal[2] == 1 ? 3 : 4;
    if (len <= head)
        return;
    const int type = nal[head] & 0x1f;
    if (type == 12)
        return;   /* filler */
    const int slice = type == 1 || type == 5;
    if (g_au_slices && ((type >= 6 && type <= 9) || (type >= 14 && type <= 18) ||
                        (slice && len > head + 1 && (nal[head + 1] & 0x80)))) {
        decode(g_au, g_au_len);
        g_au_len = 0;
        g_au_slices = 0;
    }
    if (g_au_len + len > AU_SIZE) {
        say("input: access unit larger than %d bytes, dropped", AU_SIZE);
        g_au_len = 0;
        g_au_slices = 0;
        return;
    }
    memcpy(g_au + g_au_len, nal, len);
    g_au_len += len;
    g_au_slices += slice;
}

/* one writer, until it closes the FIFO (0) or the program is told to stop (-1) */
static int read_session(int fd)
{
    size_t fill = 0, scan = 0;
    long start = -1;   /* the NAL unit being read */
    g_au_len = 0;
    g_au_slices = 0;
    while (!g_quit) {
        if (!g_opt.bench) {
            window_policy();
            fd_set in;
            FD_ZERO(&in);
            FD_SET(fd, &in);
            struct timeval wait = {0, 100000};
            if (select(fd + 1, &in, 0, 0, &wait) <= 0)
                continue;
        }
        stats_maybe();
        if (fill == INPUT_SIZE) {
            say("input: no start code in %d bytes, starting over", INPUT_SIZE);
            fill = scan = 0;
            start = -1;
        }
        const ssize_t n = read(fd, g_input + fill, INPUT_SIZE - fill);
        if (n == 0) {
            if (start >= 0)
                nal_unit(g_input + start, fill - (size_t)start);
            if (g_au_slices)
                decode(g_au, g_au_len);
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            say("input: %s", strerror(errno));
            return 0;
        }
        fill += (size_t)n;
        for (size_t i = scan; i + 3 <= fill; ++i) {
            if (g_input[i + 2] > 1) {
                i += 2;   /* no start code can end before i + 3 */
                continue;
            }
            if (g_input[i] || g_input[i + 1] || g_input[i + 2] != 1)
                continue;
            const size_t at = i && !g_input[i - 1] ? i - 1 : i;
            if (start >= 0 && at > (size_t)start)
                nal_unit(g_input + start, at - (size_t)start);
            start = (long)at;
            i += 2;
        }
        if (start > 0) {
            memmove(g_input, g_input + start, fill - (size_t)start);
            fill -= (size_t)start;
            start = 0;
        }
        scan = fill > 3 ? fill - 3 : 1;
    }
    return -1;
}

static int run(void)
{
    if (!g_opt.bench && car_open() != 0)
        return 2;
    for (;;) {
        if (g_opt.ready) {
            const int r = open(g_opt.ready, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (r >= 0)
                close(r);
        }
        say("input: waiting for a writer on %s", g_opt.input);
        int fd;
        do
            fd = open(g_opt.input, O_RDONLY);
        while (fd < 0 && errno == EINTR && !g_quit);
        if (fd < 0) {
            if (!g_quit)
                say("input: %s: %s", g_opt.input, strerror(errno));
            break;
        }
        const long long t0 = now_ms();
        const int end = read_session(fd);
        close(fd);
        if (g_opt.bench) {
            const double secs = (now_ms() - t0) / 1000.0;
            say("bench: %lu pictures in %.2f s, %.1f pictures/s, %lu errors", g_stat.total, secs,
                secs > 0 ? g_stat.total / secs : 0.0, g_stat.errors);
            return 0;
        }
        if (end < 0)
            break;
        say("input: writer closed, %lu pictures so far; window hidden until the next session", g_stat.total);
        if (g_car.ready)
            window_set(0);
        g_win.shown = 0;
        g_win.last_picture = 0;
        g_win.stale_said = 0;
        if (g_dec)
            avcodec_flush_buffers(g_dec);
    }
    say("stopping: window hidden");
    if (g_car.ready)
        window_set(0);
    usleep(200000);
    return 0;
}

static int kd_main(int argc, const char* const* argv)
{
    (void)argc;
    (void)argv;
    _exit(run());
}

static int usage(void)
{
    fprintf(stderr, "usage: nbt_decoder --input FIFO [--ready FILE] [--size WxH] [--fps 30|60]\n"
                    "       nbt_decoder --bench FILE\n");
    return 2;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--input") && value)
            g_opt.input = argv[++i];
        else if (!strcmp(a, "--ready") && value)
            g_opt.ready = argv[++i];
        else if (!strcmp(a, "--bench") && value) {
            g_opt.input = argv[++i];
            g_opt.bench = 1;
        } else if (!strcmp(a, "--size") && value) {
            if (sscanf(argv[++i], "%dx%d", &g_opt.width, &g_opt.height) != 2 || g_opt.width <= 0 || g_opt.height <= 0)
                return usage();
        } else if (!strcmp(a, "--fps") && value) {
            g_opt.fps = atoi(argv[++i]);
            if (g_opt.fps != 30 && g_opt.fps != 60)
                return usage();
        } else
            return usage();
    }
    if (!g_opt.input)
        return usage();
    g_opt.stale_ms = read_number("decoder.stale", 5000);
    g_opt.repeat_ms = read_number(RT_DIR "/decoder.repeat", 50);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    char timeout[16];
    snprintf(timeout, sizeof(timeout), "%ld", read_number(RT_DIR "/decoder.timeout", 3000));
    setenv("NV_NMSDK_TASK_TIMEOUT_MS", timeout, 0);
    say("nbt_decoder: %s %s, window %dx%d, %d fps, stale %ld ms, repeat after %ld ms", g_opt.bench ? "bench" : "input",
        g_opt.input, g_opt.width, g_opt.height, g_opt.fps, g_opt.stale_ms, g_opt.repeat_ms);
    if (g_opt.bench)
        _exit(run());
    /* the video calls need libKD's thread setup: run inside NvKdLaunch */
    void* kd = dlopen("libKD.so", RTLD_NOW | RTLD_GLOBAL);
    int (*launch)(int (*)(int, const char* const*), int, char**) =
        kd ? (int (*)(int (*)(int, const char* const*), int, char**))dlsym(kd, "NvKdLaunch") : 0;
    if (!launch) {
        say("libKD.so: NvKdLaunch not found");
        _exit(2);
    }
    launch(kd_main, argc, argv);
    _exit(0);
}
