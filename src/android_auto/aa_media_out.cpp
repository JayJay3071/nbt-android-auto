// aa_media_out: Android Auto sound into one io-amp input of the NBT. See aa_media_out.hpp.
#include "aa_media_out.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace nbt {
namespace mediaout {
namespace {

// ---- A2I client (liba2i_client.so, layout and call order as nbt_a2i_beep, car-tested 2026-11-18) ----------------------
#pragma pack(push, 4)
struct A2ICfg {
    uint32_t magic, direction, field_08, mode, buffer_size, channels, bytes_per_sample, sample_rate, field_20;
    const char* endpoint_name;
    uint32_t channel_type, field_2c, sample_bits, field_34, timeout_ms, reserved[4];
};
#pragma pack(pop)
typedef int (*open_t)(void**, const A2ICfg*);
typedef int (*handle_t)(void*);
typedef int (*close_t)(void**);
typedef int (*getbuf_t)(void*, void**, size_t*);
typedef int (*write_t)(void*, const void*, size_t, uint32_t);
typedef const char* (*err_t)(int);

const int kNoBuffer = 0x14;        // GetBuffer: no free buffer yet
const unsigned kRate = 48000;      // output rate (frames per second)

long long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- one Android Auto stream: 48 kHz stereo frames waiting to be played ----------------------------------------------
struct Stream {
    const char* name;
    unsigned rate, channels;       // what the phone sends (from the stream start, else the defaults)
    unsigned prebuffer, maxQueue, cutTo;   // frames: reserve before playing, queue limit, level after a cut
    int16_t* buf;                  // ring of stereo frames
    unsigned cap, rd, count;
    bool waiting;                  // collecting the reserve (start, or after running dry)
    double pos;                    // interpolation: position of the next output frame between prev and the next input
    int16_t prevL, prevR;
    bool havePrev;
    unsigned long long in, out, dry, cut;   // counters (frames)
    uint8_t channel;                // AA channel
    unsigned long long reported;   // frames (out + cut) already reported as consumed
    double carry;                  // fraction of a phone byte not reported yet

    void init(const char* n, unsigned r, unsigned ch, unsigned pre, unsigned maxq, unsigned cutto) {
        name = n; rate = r; channels = ch; prebuffer = pre; maxQueue = maxq; cutTo = cutto;
        cap = kRate * 2;           // 2 s
        buf = (int16_t*)calloc(cap, 4);
        rd = count = 0; waiting = true; pos = 0; havePrev = false; prevL = prevR = 0;
        in = out = dry = cut = 0;
        reported = 0; carry = 0;
    }
    void put(int16_t l, int16_t r) {
        if (count == cap) { rd = (rd + 1) % cap; --count; ++cut; }
        const unsigned w = (rd + count) % cap;
        buf[2 * w] = l; buf[2 * w + 1] = r;
        ++count;
    }
    // phone PCM (s16le, rate / channels of the stream) -> 48 kHz stereo frames
    void push(const uint8_t* pcm, size_t len) {
        if (!buf) return;
        const unsigned ch = channels == 1 ? 1 : 2;
        const size_t frames = len / (2 * ch);
        const int16_t* s = (const int16_t*)pcm;
        in += frames;
        if (rate == kRate || rate == 0) {
            for (size_t i = 0; i < frames; ++i) put(s[i * ch], s[i * ch + ch - 1]);
        } else {
            // linear interpolation: output frames every rate/48000 input frames
            const double step = (double)rate / kRate;
            for (size_t i = 0; i < frames; ++i) {
                const int16_t l = s[i * ch], r = s[i * ch + ch - 1];
                if (!havePrev) { prevL = l; prevR = r; havePrev = true; pos = 0; }
                while (pos < 1.0) {
                    put((int16_t)(prevL + (l - prevL) * pos), (int16_t)(prevR + (r - prevR) * pos));
                    pos += step;
                }
                pos -= 1.0;
                prevL = l; prevR = r;
            }
        }
        if (count > maxQueue) {     // the phone ran ahead: keep the delay short
            const unsigned drop = count - cutTo;
            rd = (rd + drop) % cap; count -= drop; cut += drop;
        }
    }
    // one frame for the mixer; false while collecting the reserve or dry
    bool take(int& l, int& r) {
        if (waiting) {
            if (count < prebuffer) return false;
            waiting = false;
        }
        if (!count) { waiting = true; ++dry; return false; }
        l = buf[2 * rd]; r = buf[2 * rd + 1];
        rd = (rd + 1) % cap; --count; ++out;
        return true;
    }
    void reset() { cut += count; rd = count = 0; waiting = true; havePrev = false; pos = 0; }
    // phone bytes behind the frames that left the queue since the last call
    size_t consumedBytes() {
        const unsigned long long gone = out + cut;
        if (gone <= reported) return 0;
        const double bytes = (double)(gone - reported) * rate / kRate * (channels == 1 ? 2 : 4) + carry;
        reported = gone;
        const size_t whole = (size_t)bytes;
        carry = bytes - whole;
        return whole;
    }
};

enum { S_MEDIA, S_GUIDE, S_SYSTEM, S_N };
Stream g_s[S_N];
pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
pthread_t g_thread;
bool g_running = false;
volatile bool g_stop = false;
void* g_lib = 0;
void* g_h = 0;
open_t a_open; handle_t a_notify; close_t a_close; getbuf_t a_getbuf; write_t a_write; err_t a_err;
double g_duck = 1.0;               // media level: 1 normal, kDuck while a prompt / system sound plays
const double kDuck = 0.25;         // about -12 dB
const double kDuckStep = 1.0 / 2400;   // 50 ms ramp

ConsumedFn g_consumed = 0;
void* g_consumedUser = 0;

int streamFor(uint8_t ch) { return ch == 9 ? S_MEDIA : ch == 8 ? S_GUIDE : ch == 16 ? S_SYSTEM : -1; }

// fills one A2I buffer (stereo s16) from the streams
void mix(int16_t* o, size_t frames) {
    pthread_mutex_lock(&g_mx);
    for (size_t i = 0; i < frames; ++i) {
        int ml = 0, mr = 0, gl = 0, gr = 0, sl = 0, sr = 0;
        const bool m = g_s[S_MEDIA].take(ml, mr);
        const bool g = g_s[S_GUIDE].take(gl, gr);
        const bool s = g_s[S_SYSTEM].take(sl, sr);
        const bool prompt = g || s || !g_s[S_GUIDE].waiting || !g_s[S_SYSTEM].waiting;
        if (prompt && g_duck > kDuck) g_duck -= kDuckStep;
        else if (!prompt && g_duck < 1.0) g_duck += kDuckStep / 10;   // slower back up (0.5 s)
        int l = (m ? (int)(ml * g_duck) : 0) + gl + sl;
        int r = (m ? (int)(mr * g_duck) : 0) + gr + sr;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        o[2 * i] = (int16_t)l; o[2 * i + 1] = (int16_t)r;
    }
    pthread_mutex_unlock(&g_mx);
}

void* writer(void*) {
    unsigned primed = 0;
    unsigned long long writes = 0;
    long long nextLog = nowMs() + 10000;
    while (!g_stop) {
        void* buf = 0;
        size_t size = 0;
        if (primed >= 2 && a_notify(g_h) != 0) { usleep(1000); continue; }
        int rc = a_getbuf(g_h, &buf, &size);
        if (rc == kNoBuffer && primed < 2) { usleep(2000); continue; }
        if (rc || !buf || !size) { usleep(2000); continue; }
        mix((int16_t*)buf, size / 4);
        if (g_consumed) {
            size_t done[S_N];
            pthread_mutex_lock(&g_mx);
            for (int i = 0; i < S_N; ++i) done[i] = g_s[i].consumedBytes();
            pthread_mutex_unlock(&g_mx);
            for (int i = 0; i < S_N; ++i)
                if (done[i]) g_consumed(g_s[i].channel, done[i], g_consumedUser);
        }
        rc = a_write(g_h, buf, size, 0);
        if (rc) {
            printf("AA_MEDIA_OUT Out_Write rc=%d %s\n", rc, a_err ? a_err(rc) : "");
            usleep(20000);
            continue;
        }
        ++primed;
        ++writes;
        if (nowMs() >= nextLog) {
            nextLog = nowMs() + 30000;
            pthread_mutex_lock(&g_mx);
            for (int i = 0; i < S_N; ++i) {
                const Stream& t = g_s[i];
                if (t.in || t.out)
                    printf("AA_MEDIA_OUT %s: in=%llu out=%llu queued=%u dry=%llu cut=%llu frames (%u Hz %u ch)\n", t.name,
                           t.in, t.out, t.count, t.dry, t.cut, t.rate, t.channels);
            }
            pthread_mutex_unlock(&g_mx);
        }
    }
    printf("AA_MEDIA_OUT writer ended after %llu buffers\n", writes);
    return 0;
}

} // namespace

void setConsumedCallback(ConsumedFn fn, void* user) { g_consumed = fn; g_consumedUser = user; }

bool start(const char* endpoint) {
    if (g_running) return true;
    g_lib = dlopen("/opt/sys/lib/liba2i_client.so", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("liba2i_client.so", RTLD_NOW);
    if (!g_lib) { printf("AA_MEDIA_OUT no liba2i_client.so: %s\n", dlerror()); return false; }
    a_open = (open_t)dlsym(g_lib, "A2I_Client_Open");
    a_notify = (handle_t)dlsym(g_lib, "A2I_Client_GetNotification");
    a_close = (close_t)dlsym(g_lib, "A2I_Client_Close");
    a_getbuf = (getbuf_t)dlsym(g_lib, "A2I_Client_GetBuffer");
    a_write = (write_t)dlsym(g_lib, "A2I_Client_Out_Write");
    a_err = (err_t)dlsym(g_lib, "A2I_Client_GetErrorDescription");
    if (!a_open || !a_notify || !a_close || !a_getbuf || !a_write) {
        printf("AA_MEDIA_OUT A2I symbols missing\n");
        return false;
    }
    A2ICfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000; cfg.direction = 0; cfg.field_08 = 1; cfg.mode = 2; cfg.buffer_size = 0x400;
    cfg.channels = 2; cfg.bytes_per_sample = 2; cfg.sample_rate = kRate; cfg.endpoint_name = endpoint;
    cfg.channel_type = 1; cfg.sample_bits = 0; cfg.field_34 = 0x05555555; cfg.timeout_ms = 2000;
    const int rc = a_open(&g_h, &cfg);
    printf("AA_MEDIA_OUT A2I_Client_Open('%s') rc=%d %s\n", endpoint, rc, rc && a_err ? a_err(rc) : "");
    if (rc || !g_h) { g_h = 0; return false; }
    // reserve before playing / queue limit / level after a cut (frames at 48 kHz)
    g_s[S_MEDIA].init("media", 48000, 2, 4800, 24000, 9600);     // 100 ms / 500 ms / 200 ms
    g_s[S_GUIDE].init("guidance", 48000, 1, 2400, 24000, 9600);  // 50 ms
    g_s[S_SYSTEM].init("system", 16000, 1, 2400, 24000, 9600);
    g_s[S_MEDIA].channel = 9; g_s[S_GUIDE].channel = 8; g_s[S_SYSTEM].channel = 16;
    g_stop = false;
    if (pthread_create(&g_thread, 0, writer, 0)) {
        printf("AA_MEDIA_OUT no writer thread\n");
        a_close(&g_h);
        return false;
    }
    g_running = true;
    return true;
}

void stop() {
    if (!g_running) return;
    g_stop = true;
    pthread_join(g_thread, 0);
    a_close(&g_h);
    g_h = 0;
    g_running = false;
    printf("AA_MEDIA_OUT closed\n");
}

void onAudio(uint8_t channel, const uint8_t* pcm, size_t len) {
    const int i = streamFor(channel);
    if (i < 0 || !g_running) return;
    pthread_mutex_lock(&g_mx);
    g_s[i].push(pcm, len);
    pthread_mutex_unlock(&g_mx);
}

void onState(uint8_t channel, bool started, uint32_t rate, uint32_t channels) {
    const int i = streamFor(channel);
    if (i < 0 || !g_running) return;
    pthread_mutex_lock(&g_mx);
    Stream& t = g_s[i];
    if (started) {
        if (rate) t.rate = rate;
        if (channels) t.channels = channels;
        t.havePrev = false; t.pos = 0;
    } else if (t.count) {
        t.waiting = false;           // the stream ended: play the rest even if it is shorter than the reserve
    }
    pthread_mutex_unlock(&g_mx);
    printf("AA_MEDIA_OUT %s stream %s (%u Hz, %u ch)\n", t.name, started ? "started" : "stopped", t.rate, t.channels);
}

} // namespace mediaout
} // namespace nbt
