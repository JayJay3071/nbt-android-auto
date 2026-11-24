/* nbt_a2i_play: plays the Android Auto media sound (48 kHz, 16 bit, stereo PCM from a FIFO) through the head unit's own
 * media input of the amplifier router (liba2i_client.so).
 *
 * Car 2026-11-11:
 *  - Every stock source (DVD, music collection, Bluetooth) plays into /dev/io-amp/input_multich_0 (8 channels, a client
 *    may open it with 2). Sound written into input_stereo_0 was accepted but never audible.
 *  - The router serves only ONE client of that input: while the stock player has a track open, a second client gets no
 *    buffers ("GetNotification ... timeout"). So before playing, the stock player is told to stop (mmecli, the stock
 *    command line client of the media engine); the media source stays selected in iDrive, so the amplifier keeps
 *    routing the input. A file "sound.nostop" next to the program switches that off.
 * The input is only held while sound arrives: after 2 s of silence it is closed again.
 *
 * Usage: nbt_a2i_play <pcm fifo> [endpoint [channels]]      started by native_service for the whole service life
 * A2I API and config layout as in experiments/audio/nbt_a2i_beep.c (from the NBT-AAD reference), dlopen only.
 * Never open or read the other nodes in /dev/io-amp (routing, debug, ...): that restarted the head unit.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#pragma pack(push, 4)
typedef struct {
    uint32_t magic, direction, field_08, mode, buffer_size, channels, bytes_per_sample, sample_rate, field_20;
    const char* endpoint_name;
    uint32_t channel_type, field_2c, sample_bits, field_34, timeout_ms, reserved[4];
} a2i_cfg;
#pragma pack(pop)
typedef int (*open_t)(void**, const a2i_cfg*);
typedef int (*h_t)(void*);
typedef int (*close_t)(void**);
typedef int (*getbuf_t)(void*, void**, size_t*);
typedef int (*write_t)(void*, const void*, size_t, uint32_t);

#define RING (48000 * 4)          /* 1 s of sound */
#define PREBUFFER (48000 * 4 / 8) /* start playing with 125 ms in the buffer */
#define STOP_STOCK \
    "LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib /opt/mm/bin/mmecli -c /dev/mme/multichannel stop"

static volatile sig_atomic_t stop;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char ring[RING];
static unsigned head, fill; /* read position and bytes in the ring (under lock) */
static long long last_sound;
static int fifo = -1;

static void stopped(int n)
{
    (void)n;
    stop = 1;
}

static long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* own thread: FIFO -> ring, so the Android Auto session never finds the FIFO full, whatever the output does.
 * When the ring is full the oldest sound is dropped. */
static void* pump(void* arg)
{
    unsigned char b[16384];
    (void)arg;
    while (!stop) {
        fd_set set;
        struct timeval t = {0, 100000};
        int n;
        FD_ZERO(&set);
        FD_SET(fifo, &set);
        if (select(fifo + 1, &set, 0, 0, &t) <= 0)
            continue;
        while ((n = read(fifo, b, sizeof(b))) > 0) {
            int i;
            pthread_mutex_lock(&lock);
            for (i = 0; i < n; ++i) {
                if (fill == RING) {
                    head = (head + 4) % RING;
                    fill -= 4;
                }
                ring[(head + fill) % RING] = b[i];
                ++fill;
            }
            last_sound = ms();
            pthread_mutex_unlock(&lock);
        }
    }
    return 0;
}

static unsigned buffered(void)
{
    unsigned n;
    pthread_mutex_lock(&lock);
    n = fill;
    pthread_mutex_unlock(&lock);
    return n;
}

static long long silent_for(void)
{
    long long t;
    pthread_mutex_lock(&lock);
    t = ms() - last_sound;
    pthread_mutex_unlock(&lock);
    return t;
}

static void stop_stock_player(void)
{
    int r;
    if (!access("sound.nostop", F_OK))
        return;
    r = system(STOP_STOCK " > sound-stop.log 2>&1");
    printf("stock player asked to stop (rc %d)\n", r);
}

int main(int argc, char** argv)
{
    void* lib;
    open_t a_open;
    h_t a_notify;
    close_t a_close;
    getbuf_t a_getbuf;
    write_t a_write;
    pthread_t pump_thread;
    const char* endpoint = argc > 2 ? argv[2] : "input_multich_0";
    /* all 8 channels of the endpoint, as the stock player does; the sound goes to the first two. With 2 channels the
     * router read the stereo frames as 8-channel frames: badly distorted (car 2026-11-11, clean with 8). */
    const int nch = argc > 3 ? atoi(argv[3]) : 8;
    int starved = 0;
    if (argc < 2 || nch < 2 || nch > 8)
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    lib = dlopen("/opt/sys/lib/liba2i_client.so", RTLD_NOW);
    if (!lib) {
        printf("dlopen: %s\n", dlerror());
        return 3;
    }
    a_open = (open_t)dlsym(lib, "A2I_Client_Open");
    a_notify = (h_t)dlsym(lib, "A2I_Client_GetNotification");
    a_close = (close_t)dlsym(lib, "A2I_Client_Close");
    a_getbuf = (getbuf_t)dlsym(lib, "A2I_Client_GetBuffer");
    a_write = (write_t)dlsym(lib, "A2I_Client_Out_Write");
    if (!a_open || !a_notify || !a_close || !a_getbuf || !a_write) {
        printf("symbols missing\n");
        return 3;
    }
    fifo = open(argv[1], O_RDWR | O_NONBLOCK); /* O_RDWR: never EOF between sessions */
    if (fifo < 0)
        return 2;
    setvbuf(stdout, 0, _IOLBF, 0);
    last_sound = ms();
    if (pthread_create(&pump_thread, 0, pump, 0))
        return 3;
    /* the output loop must not wait behind ordinary programs (default priority 10) */
    {
        struct sched_param sp;
        int policy;
        if (!pthread_getschedparam(pthread_self(), &policy, &sp)) {
            sp.sched_priority = 21;
            printf("priority 21: %s\n", pthread_setschedparam(pthread_self(), SCHED_RR, &sp) ? "refused" : "set");
        }
    }
    printf("ready, endpoint %s (%d channels)\n", endpoint, nch);

    while (!stop) {
        void* h = 0;
        a2i_cfg cfg;
        int rc, primed = 0;
        long long opened, last_write;
        unsigned long long played = 0, gaps = 0;
        /* wait for sound */
        while (!stop && (buffered() < PREBUFFER || silent_for() > 500))
            usleep(20000);
        if (stop)
            break;
        stop_stock_player();
        if (starved)
            usleep(500000);
        /* "mode" is the number of packages the router buffers, "buffer_size" one package (256 sample frames, the
         * device's frame size). With only 2 packages (5 ms each) every scheduling delay was an audible gap (car
         * 2026-11-11: sound badly distorted), so as many as the router accepts: 16, 8, 4, then 2. */
        {
            static int packages = 16;
            for (;;) {
                memset(&cfg, 0, sizeof(cfg));
                cfg.magic = 0x02000000;
                cfg.field_08 = 1;
                cfg.mode = packages;
                cfg.buffer_size = 256 * 2 * nch;
                cfg.channels = nch;
                cfg.bytes_per_sample = 2;
                cfg.sample_rate = 48000;
                cfg.endpoint_name = endpoint;
                cfg.channel_type = 1;
                cfg.field_34 = 0x05555555u;
                cfg.timeout_ms = 500;
                h = 0;
                rc = a_open(&h, &cfg);
                if ((!rc && h) || packages <= 2)
                    break;
                printf("open with %d packages failed rc=%d\n", packages, rc);
                packages /= 2;
            }
            if (!rc && h)
                printf("opened with %d packages\n", packages);
        }
        if (rc || !h) {
            printf("open %s failed rc=%d: retry in 1 s\n", endpoint, rc);
            sleep(1);
            continue;
        }
        printf("playing into %s (%d channels)\n", endpoint, nch);
        opened = last_write = ms();
        starved = 0;
        while (!stop && silent_for() < 2000) {
            void* buf = 0;
            size_t size = 0;
            /* no buffer from the router for 2.5 s: another client (the stock player) owns the input */
            if (ms() - last_write > 2500) {
                starved = 1;
                break;
            }
            if (primed >= 2 && a_notify(h) != 0) {
                usleep(1000);
                continue;
            }
            rc = a_getbuf(h, &buf, &size);
            if (rc || !buf || !size) {
                usleep(2000);
                continue;
            }
            {
                /* stereo frames from the ring into the first two channels of each output frame, the rest stays silent */
                const size_t frames = size / (2 * nch);
                unsigned char* out = (unsigned char*)buf;
                size_t take, i;
                memset(out, 0, size);
                pthread_mutex_lock(&lock);
                take = fill / 4 < frames ? fill / 4 : frames;
                for (i = 0; i < take; ++i) {
                    unsigned k;
                    for (k = 0; k < 4; ++k)
                        out[i * 2 * nch + k] = ring[(head + i * 4 + k) % RING];
                }
                head = (head + take * 4) % RING;
                fill -= take * 4;
                pthread_mutex_unlock(&lock);
                gaps += (frames - take) * 4;
                played += take * 4;
            }
            rc = a_write(h, buf, size, 0);
            if (rc) {
                printf("write rc=%d\n", rc);
                break;
            }
            last_write = ms();
            if (primed < 2)
                ++primed;
        }
        a_close(&h);
        pthread_mutex_lock(&lock);
        head = fill = 0; /* old sound must not be played when the next piece starts */
        pthread_mutex_unlock(&lock);
        printf("closed after %lld s%s, %llu bytes played, %llu bytes of gaps\n", (ms() - opened) / 1000,
               starved ? " (no buffers: input owned by another player)" : "", played, gaps);
    }
    pthread_join(pump_thread, 0);
    close(fifo);
    return 0;
}
