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
 *   --rows   show only the top N rows of the phone's picture (1280x720 canvas with the interface in its top 480 rows:
 *            --rows 480 shows them 1:1 on the 1280x480 display)
 *   --hw     decode with the car's own hardware H.264 decoder (test, 2026-11-22): FFmpeg only parses (hwaccel
 *            h264_vdpau), libnmsdk's VideoDecode decodes into the car's surfaces, VideoRenderFrame shows them without
 *            any copy on the CPU (nbt_hwdec.h). If the car refuses the decoder at the start, the CPU path takes over;
 *            if hardware decoding keeps failing later, the program writes decoder.hwfail into the current folder and
 *            ends (native_service then starts the next session without --hw)
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
#include "libavcodec/vdpau.h"
#include "libavutil/cpu.h"
#include "libavutil/pixdesc.h"
#include "nbt_hwdec.h"
#include "nbt_panel_font.h"

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
/* VideoDecode(h, CBitStreamList* ({data, size} pairs), count, pts, bool, nvh264bspPictureDecode*, reference table
 * (8 bytes per row, the surface first), rows, surface in/out: -1 = take a free one and keep it, else decode into it) */
typedef int (*nv_decode_fn)(void*, void*, const void*, uint32_t, int64_t, int, void*, void**, uint32_t, void**);
typedef void* (*wal_create_fn)(void);
typedef void (*wal_place_fn)(void*, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
typedef void (*wal_order_fn)(void*, const void*, unsigned long);
typedef void (*wal_visible_fn)(void*, unsigned long, int);

enum { NV_INIT = 0, NV_OPEN = 2, NV_CONFIGURE = 4, NV_FRAME = 5, NV_DECODE = 6, NV_SOURCE = 12, NV_DEST = 13,
       NV_BUFFER = 14, NV_RENDER = 16, NV_STREAM = 17 };
enum { FORMAT_NV12 = 1, PICTURE_FRAME = 3, BUFFERS = 3, SURFACES = 4 };
/* hardware decoding: CConfVideo decoder type 3 = H.264 (NVVADecoderCreate with 16 reference frames), the library's
 * most surfaces (1-20); our handles 1..HW_HANDLES stand for them in FFmpeg's VDPAU frames */
enum { DECODER_H264 = 3, HW_HANDLES = 20, HW_GIVE_UP = 10 };

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
    int rows;   /* 0 = the whole picture */
    int hw;     /* --hw */
} g_opt = {0, 0, 1280, 480, 30, 0, 5000, 50, 0, 0};

/* rows of a decoded picture that are shown */
static int shown_rows(int height) { return g_opt.rows && height > g_opt.rows ? g_opt.rows : height; }

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

/* three NV12 pictures: one being written, the newest finished one, one being shown; with --hw the decoded surface
 * instead (the frame is held so FFmpeg does not decode into its surface while it may still be shown) */
struct picture {
    uint8_t* y;
    uint8_t* uv;
    AVFrame* hw;
    void* surface;
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

static struct {
    int active;                       /* this stream is decoded by the car */
    int failed;                       /* not for this program any more: the CPU path */
    AVVDPAUContext* ctx;
    void* surf[HW_HANDLES + 1];       /* the car's surface for each handle, 0 = none yet */
    void* table[2 * (HW_HANDLES + 1)];/* reference table for VideoDecode: row = handle */
    int in_use[HW_HANDLES + 1];
    uint8_t token[HW_HANDLES + 1];
    uint8_t bsp[BSP_SIZE];
    unsigned long pictures, errors, in_a_row;
    int64_t pts;
} g_hw;

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

static void panel_open(void);

/* the stream, sized like the first picture: decoder 0 = pictures from VideoFrame, DECODER_H264 = from VideoDecode;
 * source = the part of the picture shown, destination = where it goes in the stream (both 0 = the whole picture) */
static int car_stream_with(int w, int h, uint32_t decoder, uint32_t surfaces, const struct rect* source,
                           const struct rect* destination)
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
    const struct conf_video conf = {(uint32_t)w, (uint32_t)h, 1.0f, (float)g_opt.fps, 0, decoder, 0, 0, 0, 50000000u, 0,
                                    surfaces, STREAM_ID};
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
    ((nv_rect_fn)nv_method(nv, NV_SOURCE))(nv, g_car.stream, source ? source : &all);
    ((nv_rect_fn)nv_method(nv, NV_DEST))(nv, g_car.stream, destination ? destination : &all);
    g_car.w = w;
    g_car.h = h;
    g_car.ready = 1;
    say("video: stream %dx%d at %d fps, id %d, %s (window %lu, %dx%d on the display)", w, h, g_opt.fps, STREAM_ID,
        decoder ? "decoded by the car" : "pictures from the CPU", g_car.window, g_opt.width, g_opt.height);
    panel_open();
    return 0;
}

static int car_stream(int w, int h) { return car_stream_with(w, h, 0, SURFACES, 0, 0); }

static void panel_hide(void);

static void window_set(int show)
{
    if (!show)
        panel_hide();
    if (!g_car.wal)
        return;
    if (show) {
        const struct { uint32_t id; uint16_t a, b; } order = {(uint32_t)g_car.window, 0, 0};
        g_car.place(g_car.wal, g_car.window, 0, 0, (unsigned long)g_opt.width, (unsigned long)g_opt.height);
        g_car.order(g_car.wal, &order, 1);
    }
    g_car.visible(g_car.wal, g_car.window, show);
}

static unsigned long g_turn;

/* --hw: show a surface the car decoded (no copy) */
static void car_present_surface(const struct picture* p)
{
    uint8_t params[64];
    memset(params, 0, sizeof(params));
    params[0] = PICTURE_FRAME;
    memcpy(params + 0xc, &p->surface, sizeof(p->surface));   /* the surface to show (0 = the one VideoFrame filled) */
    params[0x14] = 1;
    const long long t0 = now_us();
    const int r = ((nv_render_fn)nv_method(g_car.nv, NV_RENDER))(g_car.nv, g_car.buffers[g_turn++ % BUFFERS], g_car.stream,
                                                                params);
    const long long dt = now_us() - t0;
    if (r < 0)
        say("video: VideoRenderFrame failed 0x%08x (picture %lu, surface %p)", r, p->number, p->surface);
    pthread_mutex_lock(&g_box.lock);
    ++g_stat.shown;
    g_stat.upload_us += dt;
    if (dt > g_stat.upload_max)
        g_stat.upload_max = dt;
    pthread_mutex_unlock(&g_box.lock);
}

/* upload and present one picture; the buffer turns over with every picture the car took */
static void car_present(const struct picture* p)
{
    static unsigned long presented;
    if (p->surface) {
        car_present_surface(p);
        return;
    }
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
    r = ((nv_render_fn)nv_method(g_car.nv, NV_RENDER))(g_car.nv, g_car.buffers[g_turn++ % BUFFERS], g_car.stream, params);
    if (r < 0)
        say("video: VideoRenderFrame failed 0x%08x (picture %lu)", r, p->number);
    pthread_mutex_lock(&g_box.lock);
    ++g_stat.shown;
    g_stat.upload_us += dt;
    if (dt > g_stat.upload_max)
        g_stat.upload_max = dt;
    pthread_mutex_unlock(&g_box.lock);
}


/* ---- trip computer box (decoder.panel, test 2026-11-23) ----
 * User: instead of iDrive's split screen, a box over Android Auto at x 1000, y 120, 280 x 100 with the trip computer.
 * The picture cannot be drawn on with hardware decoding (the car decodes into its own surfaces), so the box is a second
 * stream of its own (NV12 from the CPU through VideoFrame, like the CPU path), in its own window above Android Auto's.
 * Data: /fs/ram3/nbt-aa-tripkombi (nbt_knob, from the cluster: trip start, duration, distance, averages) and
 * /fs/ram3/nbt-aa-trip (HMI plugin, CDS: range, tank, temperatures, instantaneous consumption, averages as fallback).
 *   12:04        44 min        30 km
 *   7.4 L/100km           42.1 km/h
 *   833 km        52 L        now 0
 *   11.5°C        E 80°       O 73°
 * Shown while Android Auto's window is shown full width and /fs/ram3/nbt-aa-panel-off does not exist (long NAV in
 * nbt_knob). The car fades a window out when no picture comes for a short while, so the box is handed over again every
 * decoder.repeat ms (min 50); its text is redrawn every second.
 * Open on the car: whether the stream id gives a window above Android Auto's. Stream id -27 (window 1025, no name in
 * libwal_nvidia's LayerId table) by default, file decoder.panelid; z-order value 1 (OrderTypeDef +4, stored per window
 * by setZOrder; Android Auto's window uses 0), file decoder.panelz. A VideoFrame of the box taking over 500 ms or failing
 * 3 times ends the box for this run: the library serves all streams with one worker thread, so a box that never gets
 * composited must not hold up Android Auto's picture. */
#define PANEL_OFF_FILE "/fs/ram3/nbt-aa-panel-off"
#define TRIP_KOMBI_FILE "/fs/ram3/nbt-aa-tripkombi"
#define TRIP_CDS_FILE "/fs/ram3/nbt-aa-trip"
enum { PANEL_X = 1000, PANEL_Y = 120, PANEL_W = 280, PANEL_H = 100, PANEL_BG = 24, PANEL_FG = 235 };

static struct {
    int ok, failed, failures;
    volatile int shown;
    void* stream;
    void* buffers[BUFFERS];
    unsigned long window;
    int id, z;
    uint8_t y[PANEL_W * PANEL_H], uv[PANEL_W * PANEL_H / 2];   /* shown (under lock) */
    uint8_t draw[PANEL_W * PANEL_H];                           /* being drawn (main thread) */
    pthread_mutex_t lock;
    int have;
    long long refresh_at, text_at;
    unsigned long turn;
    int64_t presented;
} g_panel = {.lock = PTHREAD_MUTEX_INITIALIZER};

static int panel_glyph(unsigned char c) { return c >= 32 && c <= 126 ? c - 32 : c == 0xb0 ? 95 : '?' - 32; }

static int panel_width(const char* t)
{
    int w = 0;
    for (; *t; ++t)
        w += panel_glyphs[panel_glyph((unsigned char)*t)].advance;
    return w;
}

/* align: 0 left at x, 1 centred on x, 2 right at x */
static void panel_text_at(const char* t, int x, int top, int align)
{
    x -= align == 1 ? panel_width(t) / 2 : align == 2 ? panel_width(t) : 0;
    for (; *t; ++t) {
        const int g = panel_glyph((unsigned char)*t);
        const unsigned char* bits = panel_bits + panel_glyphs[g].offset;
        const int gw = panel_glyphs[g].width, gx = x + panel_glyphs[g].left;
        for (int row = 0; row < PANEL_FONT_HEIGHT; ++row) {
            const int y = top + row;
            if (y < 0 || y >= PANEL_H)
                continue;
            for (int col = 0; col < gw; ++col) {
                const int px = gx + col;
                const unsigned a = bits[row * gw + col];
                if (px < 0 || px >= PANEL_W || !a)
                    continue;
                uint8_t* d = &g_panel.draw[y * PANEL_W + px];
                const unsigned v = PANEL_BG + (PANEL_FG - PANEL_BG) * a / 255;
                if (v > *d)
                    *d = (uint8_t)v;
            }
        }
        x += panel_glyphs[g].advance;
    }
}

static int panel_read(const char* path, char* buf, size_t size)
{
    buf[0] = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    const ssize_t n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n > 0;
}

/* "key=value" line of nbt_knob's file */
static int kombi_value(const char* text, const char* key, int* out)
{
    char k[24];
    snprintf(k, sizeof(k), "%s=", key);
    for (const char* p = text; (p = strstr(p, k)); ++p)
        if (p == text || p[-1] == '\n')
            return sscanf(p + strlen(k), "%d", out) == 1 && *out >= 0;
    return 0;
}

/* a number after "key": on the CDS line that starts with name */
static int cds_value(const char* text, const char* name, const char* key, double* out)
{
    const size_t n = strlen(name);
    for (const char* line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : 0) {
        if (strncmp(line, name, n) || line[n] != ' ')
            continue;
        const char* end = strchr(line, '\n');
        char k[40];
        snprintf(k, sizeof(k), "\"%s\":", key);
        const char* p = strstr(line, k);
        if (!p || (end && p > end))
            return 0;
        return sscanf(p + strlen(k), "%lf", out) == 1;
    }
    return 0;
}

static void panel_compose(void)
{
    char kombi[256], cds[1200];
    const int have_kombi = panel_read(TRIP_KOMBI_FILE, kombi, sizeof(kombi));
    const int have_cds = panel_read(TRIP_CDS_FILE, cds, sizeof(cds));
    if (!have_kombi && !have_cds)
        return;
    char a[32], b[32], c[32];
    int v, w;
    double x, y;
    memset(g_panel.draw, PANEL_BG, sizeof(g_panel.draw));
    const int L = PANEL_W - 8, M = PANEL_W / 2, R = 8, top[4] = {1, 26, 51, 76};
    /* 1: trip start, duration, distance (cluster) */
    if (strstr(kombi, "start=") && sscanf(strstr(kombi, "start=") + 6, "%d:%d", &v, &w) == 2 && v >= 0)
        snprintf(a, sizeof(a), "%d:%02d", v, w);
    else
        strcpy(a, "--:--");
    int hours = 0, minutes;
    kombi_value(kombi, "hours", &hours);
    if (kombi_value(kombi, "minutes", &minutes) && hours > 0)
        snprintf(b, sizeof(b), "%d:%02d h", hours, minutes);
    else if (kombi_value(kombi, "minutes", &minutes))
        snprintf(b, sizeof(b), "%d min", minutes);
    else
        strcpy(b, "-- min");
    if (kombi_value(kombi, "km", &v))
        snprintf(c, sizeof(c), "%d km", v);
    else
        strcpy(c, "-- km");
    panel_text_at(a, R, top[0], 0);
    panel_text_at(b, M, top[0], 1);
    panel_text_at(c, L, top[0], 2);
    /* 2: average consumption and speed (cluster, x0.1; CDS averageConsumption2 / averageSpeed2 otherwise) */
    if (kombi_value(kombi, "cons", &v) && v < 4095)
        snprintf(a, sizeof(a), "%d.%d L/100km", v / 10, v % 10);
    else if (cds_value(cds, "driving.averageConsumption", "averageConsumption2", &x))
        snprintf(a, sizeof(a), "%.1f L/100km", x);
    else
        strcpy(a, "-- L/100km");
    if (kombi_value(kombi, "speed", &v) && v < 4095)
        snprintf(b, sizeof(b), "%d.%d km/h", v / 10, v % 10);
    else if (cds_value(cds, "driving.averageSpeed", "averageSpeed2", &x))
        snprintf(b, sizeof(b), "%.1f km/h", x);
    else
        strcpy(b, "-- km/h");
    panel_text_at(a, R, top[1], 0);
    panel_text_at(b, L, top[1], 2);
    /* 3: range, tank, instantaneous consumption (CDS engine.consumption, unit not known: shown as it comes) */
    if (cds_value(cds, "sensors.fuel", "range", &x))
        snprintf(a, sizeof(a), "%.0f km", x);
    else
        strcpy(a, "-- km");
    if (cds_value(cds, "sensors.fuel", "tanklevel", &x))
        snprintf(b, sizeof(b), "%.0f L", x);
    else
        strcpy(b, "-- L");
    if (cds_value(cds, "engine.consumption", "consumption", &x))
        snprintf(c, sizeof(c), "now %g", x);
    else
        strcpy(c, "now --");
    panel_text_at(a, R, top[2], 0);
    panel_text_at(b, M, top[2], 1);
    panel_text_at(c, L, top[2], 2);
    /* 4: outside, coolant, oil temperature */
    if (cds_value(cds, "sensors.temperatureExterior", "temperatureExterior", &x))
        snprintf(a, sizeof(a), "%.1f\xb0" "C", x);
    else
        strcpy(a, "--\xb0" "C");
    if (cds_value(cds, "engine.temperature", "engine", &x))
        snprintf(b, sizeof(b), "E %.0f\xb0", x);
    else
        strcpy(b, "E --\xb0");
    if (cds_value(cds, "engine.temperature", "oil", &y))
        snprintf(c, sizeof(c), "O %.0f\xb0", y);
    else
        strcpy(c, "O --\xb0");
    panel_text_at(a, R, top[3], 0);
    panel_text_at(b, M, top[3], 1);
    panel_text_at(c, L, top[3], 2);
    pthread_mutex_lock(&g_panel.lock);
    memcpy(g_panel.y, g_panel.draw, sizeof(g_panel.y));
    g_panel.have = 1;
    pthread_mutex_unlock(&g_panel.lock);
}

static void panel_fail(const char* why, int r)
{
    if (++g_panel.failures >= 3 || !strcmp(why, "slow")) {
        g_panel.failed = 1;
        say("panel: VideoFrame %s (0x%08x): the box is off for this run", why, r);
    }
}

/* the box's thread: hands the box over every decoder.repeat ms while it is shown */
static void* panel_thread(void* arg)
{
    (void)arg;
    const long period = g_opt.repeat_ms > 50 ? g_opt.repeat_ms : 50;
    while (!g_quit) {
        if (!g_panel.shown || g_panel.failed) {
            usleep(50000);
            continue;
        }
        static uint8_t y[PANEL_W * PANEL_H];
        pthread_mutex_lock(&g_panel.lock);
        memcpy(y, g_panel.y, sizeof(y));
        pthread_mutex_unlock(&g_panel.lock);
        const uint8_t* planes[3] = {y, g_panel.uv, 0};
        const uint32_t pitches[3] = {PANEL_W, PANEL_W, 0};
        const int64_t pts = ++g_panel.presented * 1000000 / g_opt.fps;
        const long long t0 = now_us();
        int r = ((nv_frame_fn)nv_method(g_car.nv, NV_FRAME))(g_car.nv, g_panel.stream, FORMAT_NV12, planes, pitches, pts,
                                                             PICTURE_FRAME, 0);
        const long long dt = now_us() - t0;
        if (dt > 500000 && g_panel.shown)   /* hidden meanwhile: its surfaces do not come back, not the box's fault */
            panel_fail("slow", r);
        else if (dt > 500000)
            continue;
        else if (r < 0)
            panel_fail("failed", r);
        else {
            uint8_t params[64];
            memset(params, 0, sizeof(params));
            params[0] = PICTURE_FRAME;
            params[0x14] = 1;
            r = ((nv_render_fn)nv_method(g_car.nv, NV_RENDER))(g_car.nv, g_panel.buffers[g_panel.turn++ % BUFFERS],
                                                              g_panel.stream, params);
            if (r < 0)
                panel_fail("render failed", r);
        }
        usleep(period * 1000);
    }
    return 0;
}

static void panel_open(void)
{
    if (g_panel.ok || g_panel.failed || access(RT_DIR "/decoder.panel", F_OK) != 0)
        return;
    g_panel.failed = 1;   /* until it is open */
    if (g_opt.width < PANEL_X + PANEL_W) {
        say("panel: window only %d wide (split screen), no box", g_opt.width);
        return;
    }
    g_panel.id = (int)read_number(RT_DIR "/decoder.panelid", -27);
    g_panel.z = (int)read_number(RT_DIR "/decoder.panelz", 1);
    void* nv = g_car.nv;
    static uint8_t decoder_conf[32];
    int r = ((nv_open_fn)nv_method(nv, NV_OPEN))(nv, &g_panel.stream, &g_callback, 0, decoder_conf);
    if (r < 0 || !g_panel.stream) {
        say("panel: VideoOpen failed 0x%08x", r);
        return;
    }
    const struct conf_video conf = {PANEL_W, PANEL_H, 1.0f, (float)g_opt.fps, 0, 0, 0, 0, 0, 50000000u, 0, SURFACES,
                                    g_panel.id};
    r = ((nv_configure_fn)nv_method(nv, NV_CONFIGURE))(nv, g_panel.stream, &conf);
    if (r < 0) {
        say("panel: VideoConfigure failed 0x%08x", r);
        return;
    }
    for (int i = 0; i < BUFFERS; ++i) {
        r = ((nv_buffer_fn)nv_method(nv, NV_BUFFER))(nv, &g_panel.buffers[i], PANEL_W, PANEL_H, 0.0f, 0);
        if (r < 0) {
            say("panel: VideoBufferCreate failed 0x%08x", r);
            return;
        }
    }
    r = ((nv_stream_fn)nv_method(nv, NV_STREAM))(nv, g_panel.stream, g_panel.buffers, BUFFERS);
    if (r < 0) {
        say("panel: VideoStreamCreate failed 0x%08x", r);
        return;
    }
    const struct rect all = {0, 0, PANEL_W, PANEL_H};
    ((nv_rect_fn)nv_method(nv, NV_SOURCE))(nv, g_panel.stream, &all);
    ((nv_rect_fn)nv_method(nv, NV_DEST))(nv, g_panel.stream, &all);
    memset(g_panel.y, PANEL_BG, sizeof(g_panel.y));
    memset(g_panel.uv, 128, sizeof(g_panel.uv));
    g_panel.window = (unsigned long)(LAYER_VIDEO + g_panel.id);
    pthread_t t;
    if (pthread_create(&t, 0, panel_thread, 0) != 0) {
        say("panel: thread could not start");
        return;
    }
    g_panel.failed = 0;
    g_panel.ok = 1;
    say("panel: stream %dx%d, id %d (window %lu, z %d) at %d,%d", PANEL_W, PANEL_H, g_panel.id, g_panel.window, g_panel.z,
        PANEL_X, PANEL_Y);
}

static void panel_hide(void)
{
    if (!g_panel.ok || !g_panel.shown)
        return;
    g_panel.shown = 0;
    if (g_car.wal)
        g_car.visible(g_car.wal, g_panel.window, 0);
    say("panel: hidden");
}

/* main thread, after the window policy: the box follows Android Auto's window; force = that window was just set */
static void panel_policy(int force)
{
    if (!g_panel.ok || !g_car.wal)
        return;
    const long long now = now_ms();
    if (now >= g_panel.text_at) {
        g_panel.text_at = now + 1000;
        panel_compose();
    }
    const int want = g_win.shown == 1 && !g_panel.failed && g_panel.have && access(PANEL_OFF_FILE, F_OK) != 0;
    if (!want) {
        panel_hide();
        return;
    }
    if (g_panel.shown && !force && now < g_panel.refresh_at)
        return;
    const struct { uint32_t id; uint16_t z, b; } order = {(uint32_t)g_panel.window, (uint16_t)g_panel.z, 0};
    g_car.place(g_car.wal, g_panel.window, PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
    g_car.order(g_car.wal, &order, 1);
    g_car.visible(g_car.wal, g_panel.window, 1);
    if (!g_panel.shown)
        say("panel: shown");
    g_panel.shown = 1;
    g_panel.refresh_at = now + 2000;
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
    if (!g_car.ready || (show == g_win.shown && now < g_win.refresh_at)) {
        panel_policy(0);
        return;
    }
    if (show != g_win.shown)
        say("window: %s", show ? "shown" : "hidden");
    window_set(show);
    g_win.shown = show;
    g_win.refresh_at = now + 2000;
    panel_policy(1);
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
    if (g_hw.active)
        say("hw: %lu pictures decoded by the car, %lu errors so far", g_hw.pictures, g_hw.errors);
    const unsigned long total = g_stat.total;
    memset(&g_stat, 0, sizeof(g_stat));
    g_stat.total = total;
    g_stat.since = now;
    pthread_mutex_unlock(&g_box.lock);
}

/* ---- the car's hardware decoder (--hw) ---- */

/* decoding by the car does not work out after all: note it for native_service (no --hw for the next sessions) and
 * end, the session ends with us (switching to the CPU in the middle of a stream would need a new car stream) */
static void hw_give_up(const char* why)
{
    say("hw: %s, giving up: decoder.hwfail written, the next session decodes on the CPU", why);
    const int fd = open("decoder.hwfail", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, why, strlen(why)) < 0) {
        }
        close(fd);
    }
    if (g_car.ready)
        window_set(0);
    _exit(3);
}

/* a reference picture's row in the reference table: its handle, once the car has a surface for it */
static int hw_row(VdpVideoSurface handle)
{
    return handle >= 1 && handle <= HW_HANDLES && g_hw.surf[handle] ? (int)handle : -1;
}

/* a picture the car did not decode; gives up when it never worked or keeps failing */
static VdpStatus hw_failed(VdpStatus status)
{
    ++g_hw.errors;
    ++g_hw.in_a_row;
    if (g_hw.in_a_row >= HW_GIVE_UP && !g_hw.pictures)
        hw_give_up("VideoDecode never worked");
    if (g_hw.in_a_row >= 10 * HW_GIVE_UP)
        hw_give_up("VideoDecode keeps failing");
    return status;
}

/* FFmpeg's VDPAU render call: one picture (its slices, each after 00 00 01) into the surface of handle target */
static VdpStatus hw_render(VdpDecoder decoder, VdpVideoSurface target, VdpPictureInfo const* info, uint32_t count,
                           VdpBitstreamBuffer const* buffers)
{
    (void)decoder;
    const VdpPictureInfoH264* pic = (const VdpPictureInfoH264*)info;
    static struct { const void* data; uint32_t size; } list[512];
    static int said_scaling;
    if (target < 1 || target > HW_HANDLES || count > 512)
        return VDP_STATUS_INVALID_HANDLE;
    if (!hw_flat_scaling(pic)) {
        if (!said_scaling)
            say("hw: the stream uses its own scaling matrices, which this hand-over does not carry");
        said_scaling = 1;
        hw_give_up("own scaling matrices");
    }
    if (hw_bsp_fill(g_hw.bsp, pic, hw_row) < 0) {
        if (g_hw.in_a_row < 3 || g_hw.in_a_row % 50 == 0)
            say("hw: a reference picture without a surface (picture %lu)", g_hw.pictures);
        return hw_failed(VDP_STATUS_INVALID_HANDLE);
    }
    for (uint32_t i = 0; i < count; ++i) {
        list[i].data = buffers[i].bitstream;
        list[i].size = buffers[i].bitstream_bytes;
    }
    for (int i = 0; i <= HW_HANDLES; ++i) {
        g_hw.table[2 * i] = g_hw.surf[i];
        g_hw.table[2 * i + 1] = 0;
    }
    void* surface = g_hw.surf[target] ? g_hw.surf[target] : (void*)-1;
    g_hw.pts += 1000000 / g_opt.fps;
    const long long t0 = now_us();
    const int r = ((nv_decode_fn)nv_method(g_car.nv, NV_DECODE))(g_car.nv, g_car.stream, list, count, g_hw.pts, 0,
                                                                g_hw.bsp, g_hw.table, HW_HANDLES + 1, &surface);
    const long long dt = now_us() - t0;
    if (r < 0 || !surface || surface == (void*)-1) {
        if (g_hw.in_a_row < 3 || g_hw.in_a_row % 50 == 0)
            say("hw: VideoDecode failed 0x%08x after %lld ms (picture %lu, handle %u, %u buffers, %d slices)", r,
                dt / 1000, g_hw.pictures, (unsigned)target, (unsigned)count, (int)pic->slice_count);
        return hw_failed(VDP_STATUS_ERROR);
    }
    if (!g_hw.pictures)
        say("hw: first picture decoded by the car in %lld ms (surface %p, %d slices, frame_num %u, %s)", dt / 1000,
            surface, (int)pic->slice_count, (unsigned)pic->frame_num, pic->is_reference ? "reference" : "not a reference");
    if (!g_hw.surf[target])
        say("hw: handle %u = surface %p", (unsigned)target, surface);
    g_hw.surf[target] = surface;
    g_hw.in_a_row = 0;
    ++g_hw.pictures;
    return VDP_STATUS_OK;
}

static void hw_release(void* opaque, uint8_t* data)
{
    (void)data;
    g_hw.in_use[(intptr_t)opaque] = 0;
}

static int hw_get_buffer(AVCodecContext* avctx, AVFrame* frame, int flags)
{
    if (frame->format != AV_PIX_FMT_VDPAU)
        return avcodec_default_get_buffer2(avctx, frame, flags);
    for (int h = 1; h <= HW_HANDLES; ++h) {
        if (g_hw.in_use[h])
            continue;
        frame->buf[0] = av_buffer_create(&g_hw.token[h], 1, hw_release, (void*)(intptr_t)h, 0);
        if (!frame->buf[0])
            return AVERROR(ENOMEM);
        g_hw.in_use[h] = 1;
        frame->data[3] = (uint8_t*)(intptr_t)h;
        return 0;
    }
    say("hw: all %d surfaces in use", HW_HANDLES);
    return AVERROR(ENOMEM);
}

/* FFmpeg asks for the output format once it knows the stream (SPS): the car decodes it if it takes the decoder */
static enum AVPixelFormat hw_get_format(AVCodecContext* avctx, const enum AVPixelFormat* formats)
{
    const enum AVPixelFormat* f = formats;
    while (*f != AV_PIX_FMT_NONE && *f != AV_PIX_FMT_VDPAU)
        ++f;
    if (*f == AV_PIX_FMT_VDPAU && !g_hw.failed && !g_opt.bench) {
        const int w = avctx->coded_width, h = avctx->coded_height;
        if (g_car.ready && g_hw.active && (w != g_car.w || h != g_car.h)) {
            say("hw: the stream changed to %dx%d (the car decodes %dx%d)", w, h, g_car.w, g_car.h);
            hw_give_up("picture size changed");
        }
        if (!g_car.ready) {
            const int rows = shown_rows(avctx->height);
            const struct rect source = {0, 0, (uint32_t)avctx->width, (uint32_t)rows};
            const struct rect destination = {0, 0, (uint32_t)w, (uint32_t)h};
            say("hw: stream %dx%d (picture %dx%d, %d rows shown), profile %d level %d: asking the car to decode it", w, h,
                avctx->width, avctx->height, rows, avctx->profile, avctx->level);
            if (car_stream_with(w, h, DECODER_H264, HW_HANDLES, &source, &destination) != 0) {
                say("hw: the car did not take the decoder, decoding on the CPU");
                g_hw.failed = 1;
                g_car.ready = 0;
            } else
                g_hw.active = 1;
        }
        if (g_hw.active)
            return AV_PIX_FMT_VDPAU;
    }
    while (*formats != AV_PIX_FMT_NONE && (av_pix_fmt_desc_get(*formats)->flags & AV_PIX_FMT_FLAG_HWACCEL))
        ++formats;
    return *formats;
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
    if (g_opt.hw) {
        g_hw.ctx = av_vdpau_alloc_context();
        if (!g_hw.ctx)
            return -1;
        g_hw.ctx->decoder = 1;   /* "created by the application": FFmpeg only calls render */
        g_hw.ctx->render = hw_render;
        g_dec->hwaccel_context = g_hw.ctx;
        g_dec->get_format = hw_get_format;
        g_dec->get_buffer2 = hw_get_buffer;
        g_dec->refcounted_frames = 1;   /* the frames we hold keep their surface (decode() unrefs g_frame) */
        for (int i = 0; i < 3; ++i)
            if (!(g_box.pic[i].hw = av_frame_alloc()))
                return -1;
    }
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
    const int h = shown_rows(f->height);
    for (int row = 0; row < h; ++row)
        memcpy(p->y + (size_t)row * g_car.w, f->data[0] + (size_t)row * f->linesize[0], (size_t)f->width);
    for (int row = 0; row < h / 2; ++row) {
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

static void picture_out(const AVFrame* f);

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
    picture_out(g_frame);
    if (g_dec->refcounted_frames)
        av_frame_unref(g_frame);
}

/* a decoded picture to the display thread */
static void picture_out(const AVFrame* f)
{
    if (g_opt.bench)
        return;
    if (f->format == AV_PIX_FMT_VDPAU) {
        const int handle = (int)(intptr_t)f->data[3];
        if (handle < 1 || handle > HW_HANDLES || !g_hw.surf[handle]) {
            ++g_stat.errors;
            return;
        }
        const long long now = now_ms();
        if (g_win.last_picture && now - g_win.last_picture >= 1000)
            say("decoder: %lld ms without a picture%s", now - g_win.last_picture, g_win.stale_said ? ", shown again" : "");
        g_win.last_picture = now;
        g_win.stale_said = 0;
        struct picture* p = &g_box.pic[g_box.writing];
        av_frame_unref(p->hw);   /* its surface may be decoded into again */
        if (av_frame_ref(p->hw, f) < 0) {
            ++g_stat.errors;
            return;
        }
        p->surface = g_hw.surf[handle];
        p->number = g_stat.total;
        post_picture();
        return;
    }
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        ++g_stat.errors;
        return;
    }
    if (!g_car.ready) {
        say("decoder: first picture %dx%d%s", f->width, f->height, shown_rows(f->height) < f->height ? ", top rows only" : "");
        if (car_stream(f->width & ~1, shown_rows(f->height) & ~1) != 0) {
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
    if (f->width > g_car.w || shown_rows(f->height) > g_car.h) {
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
            /* an access unit delimiter ends the picture before it: decode now, not when the next frame comes (the
             * writer puts one after each frame) */
            const size_t h = g_input[at + 2] == 1 ? at + 3 : at + 4;
            if (h < fill && (g_input[h] & 0x1f) == 9 && g_au_slices) {
                decode(g_au, g_au_len);
                g_au_len = 0;
                g_au_slices = 0;
            }
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
    fprintf(stderr, "usage: nbt_decoder --input FIFO [--ready FILE] [--size WxH] [--fps 30|60] [--rows N] [--hw]\n"
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
        } else if (!strcmp(a, "--hw")) {
            g_opt.hw = 1;
        } else if (!strcmp(a, "--rows") && value) {
            g_opt.rows = atoi(argv[++i]);
            if (g_opt.rows < 2)
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
    say("nbt_decoder: %s %s, window %dx%d, %d fps, stale %ld ms, repeat after %ld ms%s%s", g_opt.bench ? "bench" : "input",
        g_opt.input, g_opt.width, g_opt.height, g_opt.fps, g_opt.stale_ms, g_opt.repeat_ms,
        g_opt.rows ? ", top rows only" : "", g_opt.hw ? ", hardware decoding (test)" : "");
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
