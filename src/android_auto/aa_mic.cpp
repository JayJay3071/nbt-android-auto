// aa_mic: the car's microphone to the phone, see aa_mic.hpp.
#include "aa_mic.hpp"
#include "aa_session.hpp"
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace nbt {
namespace mic {
namespace {

// A2I client configuration: the layout aa_media_out uses for playback; direction 1 = capture.
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
typedef const char* (*err_t)(int);

const uint32_t kCookieFactory = 0x0ABABABAu;   // the factory capture setting: no automatic start, Start is called
const unsigned kOutRate = 16000;               // what the phone expects: 16 kHz mono 16-bit
const unsigned kChunk = 256;                   // samples per message to the phone (16 ms)

void* g_lib = 0;
open_t a_open = 0;
handle_t a_start = 0, a_stop = 0, a_notify = 0;
close_t a_close = 0;
getbuf_t a_getbuf = 0;
err_t a_err = 0;

AASession* g_session = 0;
pthread_t g_thread;
bool g_threadOn = false;
pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
bool g_want = false, g_quit = false;   // guarded by g_m

const char* g_endpoint = "/output_multich_0";
unsigned g_rate = 22050, g_channels = 4, g_gain = 4;

bool wanted() {
    pthread_mutex_lock(&g_m);
    const bool w = g_want && !g_quit;
    pthread_mutex_unlock(&g_m);
    return w;
}

// session thread: never blocks
// nbt_knob: while this file exists and another source plays, connection b8 mutes it (as for a prompt)
const char* const kMicOpen = "/fs/ram3/nbt-aa-mic";

void onRequest(bool open, void*) {
    if (open) { if (FILE* f = fopen(kMicOpen, "w")) fclose(f); }
    else unlink(kMicOpen);
    pthread_mutex_lock(&g_m);
    g_want = open;
    pthread_cond_broadcast(&g_c);
    pthread_mutex_unlock(&g_m);
    printf("AA_MIC phone %s the microphone\n", open ? "opened" : "closed");
}

// One capture while the phone keeps the microphone open.
void capture() {
    A2ICfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000; cfg.direction = 1; cfg.field_08 = 1; cfg.mode = 8; cfg.buffer_size = 2048;
    cfg.channels = g_channels; cfg.bytes_per_sample = 2; cfg.sample_rate = g_rate; cfg.endpoint_name = g_endpoint;
    cfg.channel_type = g_channels == 1 ? 0 : 1; cfg.sample_bits = 0; cfg.field_34 = kCookieFactory; cfg.timeout_ms = 2000;
    void* h = 0;
    int rc = a_open(&h, &cfg);
    if (rc || !h) {
        printf("AA_MIC A2I_Client_Open(%s) rc=%d %s: no car microphone this time\n", g_endpoint, rc,
               rc && a_err ? a_err(rc) : "");
        return;
    }
    rc = a_start(h);
    if (rc) {
        printf("AA_MIC A2I_Client_Start rc=%d %s\n", rc, a_err ? a_err(rc) : "");
        a_close(&h);
        return;
    }
    printf("AA_MIC car microphone open (%s, %u Hz %u ch -> 16 kHz mono, gain x%u)\n", g_endpoint, g_rate, g_channels,
           g_gain);
    int16_t out[kChunk];
    unsigned n = 0, pos = 0, peak = 0;            // pos: next output after prev, in 1/16000 of an input sample
    int prev = 0;
    unsigned long frames = 0, chunks = 0, failures = 0;
    double energy = 0;
    unsigned long samples = 0;
    while (wanted()) {
        if (a_notify(h)) {
            ++failures;
            usleep(1000);
            continue;
        }
        void* buf = 0;
        size_t size = 0;
        if (a_getbuf(h, &buf, &size) || !buf || !size) continue;
        const int16_t* s = (const int16_t*)buf;
        const size_t count = size / (2 * g_channels);
        frames += count;
        for (size_t f = 0; f < count; ++f, s += g_channels) {
            const int cur = g_channels > 1 ? (s[0] + s[1]) / 2 : s[0];
            while (pos < kOutRate) {                // outputs falling between prev and cur
                int v = (prev + (int)(((long long)(cur - prev) * (long long)pos) / (long long)kOutRate)) * (int)g_gain;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                const unsigned a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                energy += (double)v * v;
                ++samples;
                out[n++] = (int16_t)v;
                if (n == kChunk) {
                    g_session->postMicData((const uint8_t*)out, sizeof(out));
                    ++chunks;
                    n = 0;
                }
                pos += g_rate;                      // one output = g_rate / 16000 input samples
            }
            pos -= kOutRate;                        // one input sample = 16000 units
            prev = cur;
        }
    }
    const int stop = a_stop(h);
    const int closed = a_close(&h);
    double rms = 0;
    if (samples) {                                  // square root without libm
        const double m = energy / samples;
        double x = m > 1 ? m : 1;
        for (int k = 0; k < 30; ++k) x = 0.5 * (x + m / x);
        rms = m > 0 ? x : 0;
    }
    printf("AA_MIC car microphone closed: %lu frames in, %lu chunks to the phone, level peak %u rms %d (of 32767), "
           "%lu notification failures, stop rc=%d, close rc=%d\n", frames, chunks, peak, (int)rms, failures, stop,
           closed);
}

void* thread(void*) {
    for (;;) {
        pthread_mutex_lock(&g_m);
        while (!g_want && !g_quit) pthread_cond_wait(&g_c, &g_m);
        const bool quit = g_quit;
        pthread_mutex_unlock(&g_m);
        if (quit) break;
        capture();
        // the phone closed it (or opening failed): wait for the next request, not for the same one again
        pthread_mutex_lock(&g_m);
        while (g_want && !g_quit) pthread_cond_wait(&g_c, &g_m);
        pthread_mutex_unlock(&g_m);
    }
    return 0;
}

unsigned envUnsigned(const char* name, unsigned def, unsigned lo, unsigned hi) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    const unsigned v = (unsigned)strtoul(e, 0, 10);
    return v >= lo && v <= hi ? v : def;
}

} // namespace

bool start(AASession& session) {
    const char* e = getenv("NBT_MIC");
    if (e && !strcmp(e, "0")) {
        printf("AA_MIC off (NBT_MIC=0)\n");
        return false;
    }
    if (!g_lib) {
        g_lib = dlopen("/opt/sys/lib/liba2i_client.so", RTLD_NOW);
        if (!g_lib) {
            printf("AA_MIC liba2i_client not loaded: %s\n", dlerror());
            return false;
        }
        a_open = (open_t)dlsym(g_lib, "A2I_Client_Open");
        a_start = (handle_t)dlsym(g_lib, "A2I_Client_Start");
        a_stop = (handle_t)dlsym(g_lib, "A2I_Client_Stop");
        a_notify = (handle_t)dlsym(g_lib, "A2I_Client_GetNotification");
        a_close = (close_t)dlsym(g_lib, "A2I_Client_Close");
        a_getbuf = (getbuf_t)dlsym(g_lib, "A2I_Client_GetBuffer");
        a_err = (err_t)dlsym(g_lib, "A2I_Client_GetErrorDescription");
        if (!a_open || !a_start || !a_stop || !a_notify || !a_close || !a_getbuf) {
            printf("AA_MIC liba2i_client symbols missing\n");
            g_lib = 0;
            return false;
        }
    }
    const char* ep = getenv("NBT_MIC_ENDPOINT");
    if (ep && *ep) g_endpoint = ep;
    g_rate = envUnsigned("NBT_MIC_RATE", 22050, 8000, 96000);
    g_channels = envUnsigned("NBT_MIC_CHANNELS", 4, 1, 8);
    g_gain = envUnsigned("NBT_MIC_GAIN", 4, 1, 16);
    g_session = &session;
    g_want = g_quit = false;
    if (pthread_create(&g_thread, 0, thread, 0)) {
        printf("AA_MIC thread not started\n");
        return false;
    }
    g_threadOn = true;
    session.setMicRequestCallback(onRequest, 0);
    printf("AA_MIC ready: car microphone on the phone's request (%s, gain x%u)\n", g_endpoint, g_gain);
    return true;
}

void stop() {
    if (!g_threadOn) return;
    pthread_mutex_lock(&g_m);
    g_quit = true;
    pthread_cond_broadcast(&g_c);
    pthread_mutex_unlock(&g_m);
    pthread_join(g_thread, 0);
    g_threadOn = false;
    unlink(kMicOpen);
}

} // namespace mic
} // namespace nbt
