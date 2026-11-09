/*
 * nbt_a2i_beep - write K short 880 Hz beeps (48 kHz, 16 bit, stereo PCM) directly into one io-amp input through the
 * NBT's A2I client library (/opt/sys/lib/liba2i_client.so), to find out which input is audible (2026-11-07, step
 * towards Android Auto media audio without Bluetooth). API and 76-byte config layout from the NBT-AAD reference
 * (include/a2i_client.h, audio/a2i_stream.cpp), dlopen only.
 *
 * usage: nbt_a2i_beep ENDPOINT BEEPS [COOKIE]      e.g. nbt_a2i_beep input_stereo_0 2
 *   COOKIE: legacy (0x05555555, default: the library starts the stream on the 2nd write) or factory (0x0ABABABA)
 * The process ends itself after at most 15 s (alarm).
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 4)
typedef struct {
    uint32_t magic, direction, field_08, mode, buffer_size, channels, bytes_per_sample, sample_rate, field_20;
    const char *endpoint_name;
    uint32_t channel_type, field_2c, sample_bits, field_34, timeout_ms, reserved[4];
} a2i_cfg;
#pragma pack(pop)
typedef int (*open_t)(void **, const a2i_cfg *);
typedef int (*h_t)(void *);
typedef int (*close_t)(void **);
typedef int (*getbuf_t)(void *, void **, size_t *);
typedef int (*write_t)(void *, const void *, size_t, uint32_t);
typedef const char *(*err_t)(int);

int main(int argc, char **argv)
{
    void *lib, *h = 0;
    open_t a_open; h_t a_start, a_notify; close_t a_close; getbuf_t a_getbuf; write_t a_write; err_t a_err;
    a2i_cfg cfg;
    int beeps, rc, primed = 0, i;
    unsigned long long pos = 0, total;
    uint32_t cookie = 0x05555555u;
    if (argc < 3) { fprintf(stderr, "usage: %s ENDPOINT BEEPS [legacy|factory]\n", argv[0]); return 2; }
    beeps = atoi(argv[2]);
    if (beeps < 1 || beeps > 9) return 2;
    if (argc > 3 && !strcmp(argv[3], "factory")) cookie = 0x0ABABABAu;
    alarm(15);
    lib = dlopen("/opt/sys/lib/liba2i_client.so", RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 3; }
    a_open = (open_t)dlsym(lib, "A2I_Client_Open");
    a_start = (h_t)dlsym(lib, "A2I_Client_Start");
    a_notify = (h_t)dlsym(lib, "A2I_Client_GetNotification");
    a_close = (close_t)dlsym(lib, "A2I_Client_Close");
    a_getbuf = (getbuf_t)dlsym(lib, "A2I_Client_GetBuffer");
    a_write = (write_t)dlsym(lib, "A2I_Client_Out_Write");
    a_err = (err_t)dlsym(lib, "A2I_Client_GetErrorDescription");
    if (!a_open || !a_start || !a_notify || !a_close || !a_getbuf || !a_write) { printf("symbols missing\n"); return 3; }
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000; cfg.direction = 0; cfg.field_08 = 1; cfg.mode = 2; cfg.buffer_size = 0x400;
    cfg.channels = 2; cfg.bytes_per_sample = 2; cfg.sample_rate = 48000; cfg.endpoint_name = argv[1];
    cfg.channel_type = 1; cfg.sample_bits = 0; cfg.field_34 = cookie; cfg.timeout_ms = 2000;
    rc = a_open(&h, &cfg);
    printf("A2I_Client_Open('%s', cookie 0x%08X) rc=%d %s\n", argv[1], cookie, rc, rc && a_err ? a_err(rc) : "");
    if (rc || !h) return 4;
    /* K beeps of 0.25 s with 0.3 s gaps, 0.5 s lead-in, 1 s tail */
    total = (unsigned long long)(48000 * (0.5 + beeps * 0.55 + 1.0));
    for (;;) {
        void *buf = 0; size_t size = 0;
        if (primed >= 2 && a_notify(h) != 0) { usleep(1000); continue; }
        rc = a_getbuf(h, &buf, &size);
        if (rc == 0x14 && primed < 2) { usleep(2000); continue; }
        if (rc || !buf || !size) { usleep(2000); continue; }
        {
            int16_t *s = (int16_t *)buf;
            size_t n = size / 4;
            for (i = 0; i < (int)n; i++, pos++) {
                double t = (double)pos / 48000.0 - 0.5;
                int on = t >= 0 && (int)(t / 0.55) < beeps && fmod(t, 0.55) < 0.25;
                int16_t v = on ? (int16_t)(9000 * sin(2 * M_PI * 880.0 * (double)pos / 48000.0)) : 0;
                s[2 * i] = v; s[2 * i + 1] = v;
            }
        }
        rc = a_write(h, buf, size, 0);
        if (rc) { printf("Out_Write rc=%d %s\n", rc, a_err ? a_err(rc) : ""); break; }
        if (++primed == 2 && cookie != 0x05555555u) {
            rc = a_start(h);
            printf("A2I_Client_Start rc=%d\n", rc);
        }
        if (pos >= total) break;
    }
    printf("written %llu frames\n", pos);
    a_close(&h);
    return 0;
}
