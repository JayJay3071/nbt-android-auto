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
    bool active;                   // between the phone's stream start and stop
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
        rd = count = 0; waiting = true; active = false; pos = 0; havePrev = false; prevL = prevR = 0;
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
volatile bool g_stop = false;
bool g_running = false;
void* g_lib = 0;
open_t a_open; handle_t a_notify; close_t a_close; getbuf_t a_getbuf; write_t a_write; err_t a_err;
double g_duck = 1.0;               // media level: 1 normal, kDuck while a prompt / system sound plays
const double kDuck = 0.25;         // about -12 dB
const double kDuckStep = 1.0 / 2400;   // 50 ms ramp

ConsumedFn g_consumed = 0;
void* g_consumedUser = 0;

// ---- prompts while another source plays (2026-11-18) -------------------------------------------------------------
// The main output only plays while the car's source is Android Auto's (f6). On any other source (radio, Bluetooth,
// AUX, none) guidance and system sounds go to the second output (NBT_AA_PROMPT_OUT, input_stereo_3), which the
// amplifier plays while connection b8 (browser, interrupting: the main source is muted meanwhile) is built. nbt_knob
// owns the MOST link: it writes the current main source to kSourceFile, builds b8 while kPromptReq exists and writes
// kPromptReady once the amplifier has built it; until then the prompt waits (up to kPromptWaitMs).
const char* const kSourceFile = "/fs/ram3/nbt-aa-source";       // nbt_knob: main source (hex) or "none"
const char* const kPromptReq = "/fs/ram3/nbt-aa-prompt";         // here: a prompt waits, b8 wanted
const char* const kPromptReady = "/fs/ram3/nbt-aa-prompt-ready"; // nbt_knob: b8 is built
const long long kPromptWaitMs = 2500, kPromptTailMs = 800;
unsigned g_aaSource = 0xf6;        // NBT_AUX_SOURCE: Android Auto's source
bool g_elsewhere = false;          // prompts go to the prompt output (decided between prompts), under g_mx
bool g_ready = false;              // b8 built: the prompt output may take them, under g_mx

int streamFor(uint8_t ch) { return ch == 9 ? S_MEDIA : ch == 8 ? S_GUIDE : ch == 16 ? S_SYSTEM : -1; }

void clamp(int& v) { if (v > 32767) v = 32767; else if (v < -32768) v = -32768; }

// main output: media, plus guidance / system unless they go elsewhere
void mixMain(int16_t* o, size_t frames) {
    pthread_mutex_lock(&g_mx);
    for (size_t i = 0; i < frames; ++i) {
        int ml = 0, mr = 0, gl = 0, gr = 0, sl = 0, sr = 0;
        const bool m = g_s[S_MEDIA].take(ml, mr);
        bool g = false, s = false;
        if (!g_elsewhere) { g = g_s[S_GUIDE].take(gl, gr); s = g_s[S_SYSTEM].take(sl, sr); }
        const bool prompt = !g_elsewhere && (g || s || !g_s[S_GUIDE].waiting || !g_s[S_SYSTEM].waiting);
        if (prompt && g_duck > kDuck) g_duck -= kDuckStep;
        else if (!prompt && g_duck < 1.0) g_duck += kDuckStep / 10;   // slower back up (0.5 s)
        int l = (m ? (int)(ml * g_duck) : 0) + gl + sl;
        int r = (m ? (int)(mr * g_duck) : 0) + gr + sr;
        clamp(l); clamp(r);
        o[2 * i] = (int16_t)l; o[2 * i + 1] = (int16_t)r;
    }
    pthread_mutex_unlock(&g_mx);
}

// prompt output: guidance + system once b8 is built, else silence
void mixPrompt(int16_t* o, size_t frames) {
    pthread_mutex_lock(&g_mx);
    const bool go = g_elsewhere && g_ready;
    for (size_t i = 0; i < frames; ++i) {
        int gl = 0, gr = 0, sl = 0, sr = 0;
        if (go) { g_s[S_GUIDE].take(gl, gr); g_s[S_SYSTEM].take(sl, sr); }
        int l = gl + sl, r = gr + sr;
        clamp(l); clamp(r);
        o[2 * i] = (int16_t)l; o[2 * i + 1] = (int16_t)r;
    }
    pthread_mutex_unlock(&g_mx);
}

void reportConsumed() {
    if (!g_consumed) return;
    size_t done[S_N];
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < S_N; ++i) done[i] = g_s[i].consumedBytes();
    pthread_mutex_unlock(&g_mx);
    for (int i = 0; i < S_N; ++i)
        if (done[i]) g_consumed(g_s[i].channel, done[i], g_consumedUser);
}

// ---- one A2I output -------------------------------------------------------------------------------------------------
struct Out {
    const char* name;
    char endpoint[48];
    void* h;
    unsigned primed;
};
Out g_main = {"main", "", 0, 0}, g_prompt = {"prompt", "", 0, 0};
pthread_t g_mainThread, g_promptThread;
bool g_promptThreadOn = false;

bool openOut(Out& o) {
    A2ICfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000; cfg.direction = 0; cfg.field_08 = 1; cfg.mode = 2; cfg.buffer_size = 0x400;
    cfg.channels = 2; cfg.bytes_per_sample = 2; cfg.sample_rate = kRate; cfg.endpoint_name = o.endpoint;
    cfg.channel_type = 1; cfg.sample_bits = 0; cfg.field_34 = 0x05555555; cfg.timeout_ms = 2000;
    o.h = 0; o.primed = 0;
    const int rc = a_open(&o.h, &cfg);
    printf("AA_MEDIA_OUT %s: A2I_Client_Open('%s') rc=%d %s\n", o.name, o.endpoint, rc, rc && a_err ? a_err(rc) : "");
    if (rc || !o.h) { o.h = 0; return false; }
    return true;
}

void closeOut(Out& o) {
    if (o.h) { a_close(&o.h); o.h = 0; printf("AA_MEDIA_OUT %s: closed\n", o.name); }
}

// one buffer: wait for the input to ask, fill, hand back. false = nothing done (caller loops)
bool pump(Out& o, void (*fill)(int16_t*, size_t)) {
    void* buf = 0;
    size_t size = 0;
    if (o.primed >= 2 && a_notify(o.h) != 0) { usleep(1000); return false; }
    int rc = a_getbuf(o.h, &buf, &size);
    if (rc == kNoBuffer && o.primed < 2) { usleep(2000); return false; }
    if (rc || !buf || !size) { usleep(2000); return false; }
    fill((int16_t*)buf, size / 4);
    reportConsumed();
    rc = a_write(o.h, buf, size, 0);
    if (rc) {
        printf("AA_MEDIA_OUT %s: Out_Write rc=%d %s\n", o.name, rc, a_err ? a_err(rc) : "");
        usleep(20000);
        return false;
    }
    ++o.primed;
    return true;
}

void logStats() {
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < S_N; ++i) {
        const Stream& t = g_s[i];
        if (t.in || t.out)
            printf("AA_MEDIA_OUT %s: in=%llu out=%llu queued=%u dry=%llu cut=%llu frames (%u Hz %u ch)\n", t.name,
                   t.in, t.out, t.count, t.dry, t.cut, t.rate, t.channels);
    }
    pthread_mutex_unlock(&g_mx);
}

void* mainWriter(void*) {
    unsigned long long writes = 0;
    long long nextLog = nowMs() + 10000;
    while (!g_stop) {
        if (pump(g_main, mixMain)) ++writes;
        if (nowMs() >= nextLog) { nextLog = nowMs() + 30000; logStats(); }
    }
    printf("AA_MEDIA_OUT main writer ended after %llu buffers\n", writes);
    return 0;
}

// the car's main source from nbt_knob: -1 unknown (no file yet), -2 none
int carSource() {
    FILE* f = fopen(kSourceFile, "r");
    if (!f) return -1;
    char b[16] = {0};
    const bool ok = fgets(b, sizeof(b), f) != 0;
    fclose(f);
    if (!ok) return -1;
    if (!strncmp(b, "none", 4)) return -2;
    return (int)strtoul(b, 0, 16);
}

// prompt control + prompt output (opened only while a prompt goes over b8)
void* promptWriter(void*) {
    bool requested = false, gaveUp = false;
    long long requestedAt = 0, lastPrompt = 0, nextCheck = 0;
    while (!g_stop) {
        const long long now = nowMs();
        if (now >= nextCheck) {
            nextCheck = now + 50;
            const int src = carSource();
            const bool onAa = src == -1 || src == (int)g_aaSource;   // unknown: keep everything on the main output
            pthread_mutex_lock(&g_mx);
            const Stream& g = g_s[S_GUIDE];
            const Stream& s = g_s[S_SYSTEM];
            const bool pending = g.count || g.active || s.count || s.active;
            if (pending) lastPrompt = now;
            if (!pending && !requested && now - lastPrompt >= kPromptTailMs) {
                g_elsewhere = !onAa;      // only between prompts, never in the middle of one
                gaveUp = false;
            }
            const bool want = g_elsewhere && !gaveUp && (pending || now - lastPrompt < kPromptTailMs);
            pthread_mutex_unlock(&g_mx);
            if (want && !requested) {
                unlink(kPromptReady);
                if (FILE* f = fopen(kPromptReq, "w")) fclose(f);
                requested = true;
                requestedAt = now;
                printf("AA_MEDIA_OUT prompt while source %04x plays: b8 requested\n", src < 0 ? 0xffff : src);
                if (!g_prompt.h) openOut(g_prompt);
            }
            if (requested) {
                const bool ready = access(kPromptReady, F_OK) == 0;
                pthread_mutex_lock(&g_mx);
                if (ready && !g_ready) printf("AA_MEDIA_OUT b8 built after %lld ms: prompt plays\n", now - requestedAt);
                g_ready = ready && g_prompt.h;
                if (!ready && now - requestedAt > kPromptWaitMs && !gaveUp) {
                    gaveUp = true;            // no b8: let the main output take the prompt (inaudible, but no stall)
                    g_elsewhere = false;
                    printf("AA_MEDIA_OUT b8 not built within %lld ms: prompt dropped\n", kPromptWaitMs);
                }
                pthread_mutex_unlock(&g_mx);
            }
            if (requested && !want) {
                unlink(kPromptReq);
                requested = false;
                pthread_mutex_lock(&g_mx);
                g_ready = false;
                pthread_mutex_unlock(&g_mx);
                printf("AA_MEDIA_OUT prompt over: b8 released\n");
                closeOut(g_prompt);
            }
        }
        if (g_prompt.h) pump(g_prompt, mixPrompt);
        else usleep(20000);
    }
    if (requested) unlink(kPromptReq);
    closeOut(g_prompt);
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
    snprintf(g_main.endpoint, sizeof(g_main.endpoint), "%s", endpoint);
    if (!openOut(g_main)) return false;
    // reserve before playing / queue limit / level after a cut (frames at 48 kHz). Prompts may wait up to ~2 s for b8.
    g_s[S_MEDIA].init("media", 48000, 2, 4800, 24000, 9600);     // 100 ms / 500 ms / 200 ms
    g_s[S_GUIDE].init("guidance", 48000, 1, 2400, 92000, 48000); // 50 ms / ~1.9 s
    g_s[S_SYSTEM].init("system", 16000, 1, 2400, 92000, 48000);
    g_s[S_MEDIA].channel = 9; g_s[S_GUIDE].channel = 8; g_s[S_SYSTEM].channel = 16;
    if (const char* a = getenv("NBT_AUX_SOURCE")) if (*a) g_aaSource = (unsigned)strtoul(a, 0, 16);
    g_stop = false;
    if (pthread_create(&g_mainThread, 0, mainWriter, 0)) {
        printf("AA_MEDIA_OUT no writer thread\n");
        closeOut(g_main);
        return false;
    }
    const char* p = getenv("NBT_AA_PROMPT_OUT");
    if (p && *p) {
        snprintf(g_prompt.endpoint, sizeof(g_prompt.endpoint), "%s", p);
        unlink(kPromptReq);
        g_promptThreadOn = pthread_create(&g_promptThread, 0, promptWriter, 0) == 0;
        printf("AA_MEDIA_OUT prompts while another source plays -> %s over b8: %s\n", p, g_promptThreadOn ? "on" : "FAILED");
    }
    g_running = true;
    return true;
}

void stop() {
    if (!g_running) return;
    g_stop = true;
    pthread_join(g_mainThread, 0);
    if (g_promptThreadOn) pthread_join(g_promptThread, 0);
    g_promptThreadOn = false;
    closeOut(g_main);
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
    t.active = started;
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
