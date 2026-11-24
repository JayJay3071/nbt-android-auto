/* nbt_aa_wav_http: Android Auto media sound as an endless WAV stream over HTTP (2026-11-23).
 *
 * For the faked "iPod over Bluetooth" source the head unit's own player (io-nme, graph ipodstreamer) plays the URL the
 * Bluetooth audio driver names (normally the phone's A2DP stream "wms:.../dev/wms/player1"). nbt_ipod_tap --url makes
 * that URL point here, so the stock player plays the Android Auto sound that comes over USB - the official way, with
 * the head unit's own routing and volume handling, instead of our own player fighting it for the amplifier input.
 *
 * Reads 48 kHz / 16 bit / stereo PCM from the session's audio FIFO (the job nbt_a2i_play does otherwise; only one of
 * the two may run) into a 1 s ring, listens on 127.0.0.1:<port> and sends every client a WAV header followed by the
 * sound in real time, 10 ms per step; missing sound is filled with silence, so the stream never stalls (the head unit's
 * Cinemo drops an HTTP connection that brings no data for 5 s). A new connection replaces the old one. The ring is
 * kept short (latency at most ~300 ms).
 *
 * Usage: nbt_aa_wav_http <pcm fifo> [port, default 18766]      log on stderr
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define FRAME 4                   /* bytes per stereo frame */
#define RING (RATE * FRAME)       /* 1 s */
#define STEP (RATE / 100 * FRAME) /* 10 ms */
#define MAXFILL (RATE * FRAME * 3 / 10)
#define KEEPFILL (RATE * FRAME / 10)

static unsigned char g_ring[RING];
static size_t g_head, g_fill;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_stop;
static const char* g_fifo;
static unsigned long long g_in, g_dropped;
static int g_ls = -1;

static void note(const char* format, ...)
{
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* the session's sound into the ring; O_RDWR: never EOF when a session ends, never blocks on open */
static void* reader(void* arg)
{
    unsigned char buf[4096];
    int fd = open(g_fifo, O_RDWR);
    (void)arg;
    if (fd < 0) {
        note("cannot open %s: %s", g_fifo, strerror(errno));
        g_stop = 1;
        return 0;
    }
    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof(buf)), i;
        if (n <= 0) {
            usleep(10000);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < n; ++i) {
            g_ring[(g_head + g_fill) % RING] = buf[i];
            if (g_fill < RING)
                ++g_fill;
            else
                g_head = (g_head + 1) % RING;
        }
        g_in += (unsigned long long)n;
        if (g_fill > MAXFILL) { /* phone faster than real time: keep the latency short, whole frames only */
            const size_t drop = (g_fill - KEEPFILL) & ~(size_t)(FRAME - 1);
            g_head = (g_head + drop) % RING;
            g_fill -= drop;
            g_dropped += drop;
        }
        pthread_mutex_unlock(&g_lock);
    }
    close(fd);
    return 0;
}

static void put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int send_all(int s, const void* data, size_t n)
{
    const unsigned char* p = data;
    while (n) {
        ssize_t k = send(s, p, n, 0);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void serve(int s)
{
    char req[2048];
    unsigned char wav[44];
    static const char head[] = "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n";
    unsigned char out[STEP];
    struct timespec next;
    unsigned long long sent = 0, silent = 0;
    const uint32_t data_len = 0x7ffff000u;
    ssize_t n;
    int one = 1;
    struct timeval tv = {0, 300000};

    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = recv(s, req, sizeof(req) - 1, 0);
    if (n > 0) {
        char* eol;
        req[n] = 0;
        eol = strpbrk(req, "\r\n");
        if (eol)
            *eol = 0;
        note("request: %.200s", req);
    }
    memcpy(wav, "RIFF", 4);
    put32(wav + 4, 36 + data_len);
    memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16);
    wav[20] = 1, wav[21] = 0; /* PCM */
    wav[22] = 2, wav[23] = 0; /* stereo */
    put32(wav + 24, RATE);
    put32(wav + 28, RATE * FRAME);
    wav[32] = FRAME, wav[33] = 0;
    wav[34] = 16, wav[35] = 0;
    memcpy(wav + 36, "data", 4);
    put32(wav + 40, data_len);
    if (send_all(s, head, sizeof(head) - 1) || send_all(s, wav, sizeof(wav)))
        return;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_stop) {
        size_t take, i;
        fd_set r;
        struct timeval zero = {0, 0};
        pthread_mutex_lock(&g_lock);
        take = g_fill < STEP ? g_fill & ~(size_t)(FRAME - 1) : STEP;
        for (i = 0; i < take; ++i)
            out[i] = g_ring[(g_head + i) % RING];
        g_head = (g_head + take) % RING;
        g_fill -= take;
        pthread_mutex_unlock(&g_lock);
        if (take < STEP) {
            memset(out + take, 0, STEP - take);
            silent += STEP - take;
        }
        if (send_all(s, out, STEP))
            break;
        sent += STEP;
        if (sent % (RATE * FRAME * 60) < STEP)
            note("streaming: %llu s sent, %llu s of it silence, in %llu bytes, dropped %llu", sent / (RATE * FRAME),
                 silent / (RATE * FRAME), g_in, g_dropped);
        /* client closed, or a new client waiting (it replaces this one)? */
        FD_ZERO(&r);
        FD_SET(s, &r);
        FD_SET(g_ls, &r);
        if (select((s > g_ls ? s : g_ls) + 1, &r, 0, 0, &zero) > 0) {
            char junk[512];
            if (FD_ISSET(s, &r) && recv(s, junk, sizeof(junk), 0) <= 0)
                break;
            if (FD_ISSET(g_ls, &r)) {
                note("new client waiting, this one is replaced");
                break;
            }
        }
        next.tv_nsec += 10000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
    }
    note("client gone after %llu s (%llu s silence)", sent / (RATE * FRAME), silent / (RATE * FRAME));
}

int main(int argc, char** argv)
{
    struct sockaddr_in a;
    struct sigaction sa;
    pthread_t rt;
    int one = 1, port = 18766;
    if (argc < 2)
        return 2;
    g_fifo = argv[1];
    if (argc > 2)
        port = atoi(argv[2]);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    g_ls = socket(AF_INET, SOCK_STREAM, 0);
    if (g_ls < 0)
        return 2;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_ls, (struct sockaddr*)&a, sizeof(a)) || listen(g_ls, 4)) {
        note("cannot listen on 127.0.0.1:%d: %s", port, strerror(errno));
        return 2;
    }
    pthread_create(&rt, 0, reader, 0);
    note("WAV stream of %s on http://127.0.0.1:%d/aa.wav", g_fifo, port);
    while (!g_stop) {
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int s = accept(g_ls,(struct sockaddr*)&from, &len);
        if (s < 0) {
            if (errno != EINTR)
                usleep(100000);
            continue;
        }
        note("client connected");
        serve(s);
        close(s);
    }
    close(g_ls);
    note("stopped");
    return 0;
}
