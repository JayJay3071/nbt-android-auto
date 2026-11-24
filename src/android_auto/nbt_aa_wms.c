/* nbt_aa_wms: a Bluetooth A2DP sound device of our own that plays the Android Auto sound (2026-11-24).
 *
 * The faked "iPod over Bluetooth" source (nbt_ipod_tap --iap 1 in front of /fs/avrcp0, nbt_aa_ipod at /fs/btipod)
 * shows Android Auto with cover and length in iDrive, but the head unit's player for it (io-nme, graph ipodstreamer)
 * only plays the phone's A2DP stream: it asks the Bluetooth audio driver for the URL (devctl 0x0e6e), gets
 * "wms:/net/hu-intel.nbt//dev/wms/player1" and reads that device. It accepts nothing but such a "wms:" device (an
 * http URL failed, car 2026-11-23 22:49). Android Auto plays its media over USB, so that device is silent.
 *
 * This program is such a device, at /dev/wms/nbtaa, and the tap answers the URL request with it
 * (--url wms:/net/hu-intel.nbt//dev/wms/nbtaa). Protocol as recorded on the car from the real /dev/wms/player1
 * (OnePlus set to SBC, 2026-11-23 22:57):
 *   - devctl 0x40200520, 32 bytes, about once a second: eight u32; before streaming 1,1,2,0,1,1,0,0, while streaming
 *     1,1,0,2,1,1,0x69b,0 (with AAC the second word was 3 instead of 1).
 *   - read(2048): one A2DP media packet per read, blocking until the next one is due: 1 byte payload header
 *     (number of frames, here 8) followed by 8 SBC frames.
 * The SBC frames are made here from the session's PCM (audio.fifo, 48 kHz / 16 bit / stereo, read instead of
 * nbt_a2i_play - only one of the two may run), resampled to 44.1 kHz (as the phone used), 16 blocks, 8 subbands,
 * stereo, SNR allocation, bitpool 53 (the highest standard quality). Silence is sent while nothing arrives, so the
 * player keeps running. The encoder follows the A2DP specification (analysis window checked offline: filter bank
 * reconstruction 64 dB; CRC checked against the recorded frames).
 *
 * Usage: nbt_aa_wms --fifo <pcm fifo> [--path /dev/wms/nbtaa] [--log file] [--bitpool 53] [--rate 44100|48000]
 */
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IN_RATE 48000
#define RING (IN_RATE * 2)        /* stereo frames, 2 s */
#define TARGET (IN_RATE / 5)       /* jitter buffer: 200 ms */
#define MAXFILL (IN_RATE)          /* more than 1 s queued (should not happen): back to TARGET */
#define KEEPFILL TARGET
#define FRAMES_PER_PACKET 8
#define SAMPLES_PER_FRAME 128 /* 16 blocks x 8 subbands */
#define QUEUE 6
#define MAXPACKET 2048

static volatile sig_atomic_t g_stop;
static const char* g_fifo;
/* 48 kHz = the session's own rate, no resampling (offline check: 65 dB; with the cubic resampling to 44.1 kHz 44 dB).
 * --rate 44100 if the head unit's player should refuse 48 kHz SBC (the phone used 44.1 kHz). */
static int g_bitpool = 53, g_rate = 48000;
static unsigned long long g_packets, g_silent_packets, g_read_packets, g_underruns;
static volatile int g_buffering = 1; /* waiting until the jitter buffer holds TARGET */
static FILE* g_selftest; /* --selftest <file>: encode a test tone as fast as possible into a file, then end */

/* ---- log ---- */
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

/* ---- PCM in: the session's sound ---- */
static int16_t g_ring[RING][2];
static size_t g_head, g_fill;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

static void* reader(void* arg)
{
    unsigned char buf[4096 + 4];
    size_t carry = 0;
    int fd = open(g_fifo, O_RDWR); /* never EOF between sessions, never blocks on open */
    (void)arg;
    if (fd < 0) {
        note("cannot open %s: %s", g_fifo, strerror(errno));
        g_stop = 1;
        return 0;
    }
    while (!g_stop) {
        ssize_t n = read(fd, buf + carry, 4096);
        size_t total, frames, i;
        if (n <= 0) {
            usleep(10000);
            continue;
        }
        total = carry + (size_t)n;
        frames = total / 4;
        pthread_mutex_lock(&g_ring_lock);
        for (i = 0; i < frames; ++i) {
            size_t at = (g_head + g_fill) % RING;
            g_ring[at][0] = (int16_t)(buf[4 * i] | buf[4 * i + 1] << 8);
            g_ring[at][1] = (int16_t)(buf[4 * i + 2] | buf[4 * i + 3] << 8);
            if (g_fill < RING)
                ++g_fill;
            else
                g_head = (g_head + 1) % RING;
        }
        if (g_fill > MAXFILL) {
            g_head = (g_head + g_fill - KEEPFILL) % RING;
            g_fill = KEEPFILL;
        }
        pthread_mutex_unlock(&g_ring_lock);
        carry = total - frames * 4;
        memmove(buf, buf + frames * 4, carry);
    }
    close(fd);
    return 0;
}

/* ---- SBC encoder (A2DP specification, 8 subbands, 16 blocks, stereo, SNR allocation) ---- */
static const double k_window[80] = {
    0.00000000E+00,  1.56575398E-04,  3.43256425E-04,  5.54620202E-04,  8.23919506E-04,  1.13992507E-03,  1.47640169E-03,
    1.78371725E-03,  2.01182542E-03,  2.10371989E-03,  1.99454554E-03,  1.61656283E-03,  9.02154502E-04,  -1.78805361E-04,
    -1.64973098E-03, -3.49717454E-03, 5.65949473E-03,  8.02941163E-03,  1.04584443E-02,  1.27472335E-02,  1.46525263E-02,
    1.59045603E-02,  1.62208471E-02,  1.53184106E-02,  1.29371806E-02,  8.85757540E-03,  2.92408442E-03,  -4.91578024E-03,
    -1.46404076E-02, -2.61098752E-02, -3.90751381E-02, -5.31873032E-02, 6.79989431E-02,  8.29847578E-02,  9.75753918E-02,
    1.11196689E-01,  1.23264548E-01,  1.33264415E-01,  1.40753505E-01,  1.45389847E-01,  1.46955068E-01,  1.45389847E-01,
    1.40753505E-01,  1.33264415E-01,  1.23264548E-01,  1.11196689E-01,  9.75753918E-02,  8.29847578E-02,  -6.79989431E-02,
    -5.31873032E-02, -3.90751381E-02, -2.61098752E-02, -1.46404076E-02, -4.91578024E-03, 2.92408442E-03,  8.85757540E-03,
    1.29371806E-02,  1.53184106E-02,  1.62208471E-02,  1.59045603E-02,  1.46525263E-02,  1.27472335E-02,  1.04584443E-02,
    8.02941163E-03,  -5.65949473E-03, -3.49717454E-03, -1.64973098E-03, -1.78805361E-04, 9.02154502E-04,  1.61656283E-03,
    1.99454554E-03,  2.10371989E-03,  2.01182542E-03,  1.78371725E-03,  1.47640169E-03,  1.13992507E-03,  8.23919506E-04,
    5.54620202E-04,  3.43256425E-04,  1.56575398E-04};
static double g_cos[8][16];
static double g_x[2][80];

static void sbc_init(void)
{
    int k, i;
    for (k = 0; k < 8; ++k)
        for (i = 0; i < 16; ++i)
            g_cos[k][i] = cos((k + 0.5) * (i - 4) * M_PI / 8.0);
}

/* 8 new samples of one channel -> 8 subband samples */
static void analyze(int ch, const double* in, double* out)
{
    double* x = g_x[ch];
    double y[16];
    int i, j, k;
    memmove(x + 8, x, 72 * sizeof(double));
    for (i = 0; i < 8; ++i)
        x[7 - i] = in[i];
    for (i = 0; i < 16; ++i) {
        double s = 0;
        for (j = 0; j < 5; ++j)
            s += k_window[i + 16 * j] * x[i + 16 * j];
        y[i] = s;
    }
    for (k = 0; k < 8; ++k) {
        double s = 0;
        for (i = 0; i < 16; ++i)
            s += g_cos[k][i] * y[i];
        out[k] = s;
    }
}

static unsigned crc8_bits(unsigned crc, unsigned value, int nbits)
{
    while (nbits--) {
        const unsigned bit = (value >> nbits) & 1u, top = (crc >> 7) & 1u;
        crc = (crc << 1) & 0xffu;
        if (top ^ bit)
            crc ^= 0x1du;
    }
    return crc;
}

struct bitw {
    unsigned char* p;
    size_t pos; /* in bits */
};

static void put_bits(struct bitw* w, unsigned value, int nbits)
{
    while (nbits--) {
        if ((value >> nbits) & 1u)
            w->p[w->pos >> 3] |= (unsigned char)(0x80u >> (w->pos & 7));
        ++w->pos;
    }
}

static void allocate(const int sf[2][8], int bits[2][8])
{
    int need[2][8], ch, sb, max_need = 0, bitcount = 0, slicecount = 0, bitslice;
    for (ch = 0; ch < 2; ++ch)
        for (sb = 0; sb < 8; ++sb) {
            need[ch][sb] = sf[ch][sb]; /* SNR allocation */
            if (need[ch][sb] > max_need)
                max_need = need[ch][sb];
        }
    bitslice = max_need + 1;
    do {
        --bitslice;
        bitcount += slicecount;
        slicecount = 0;
        for (ch = 0; ch < 2; ++ch)
            for (sb = 0; sb < 8; ++sb) {
                if (need[ch][sb] > bitslice + 1 && need[ch][sb] < bitslice + 16)
                    ++slicecount;
                else if (need[ch][sb] == bitslice + 1)
                    slicecount += 2;
            }
    } while (bitcount + slicecount < g_bitpool);
    if (bitcount + slicecount == g_bitpool) {
        bitcount += slicecount;
        --bitslice;
    }
    for (ch = 0; ch < 2; ++ch)
        for (sb = 0; sb < 8; ++sb) {
            const int b = need[ch][sb] - bitslice;
            bits[ch][sb] = need[ch][sb] < bitslice + 2 ? 0 : (b > 16 ? 16 : b);
        }
    ch = 0;
    sb = 0;
    while (bitcount < g_bitpool && sb < 8) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
            ++bits[ch][sb];
            ++bitcount;
        } else if (need[ch][sb] == bitslice + 1 && g_bitpool > bitcount + 1) {
            bits[ch][sb] = 2;
            bitcount += 2;
        }
        if (ch == 1) {
            ch = 0;
            ++sb;
        } else
            ch = 1;
    }
    ch = 0;
    sb = 0;
    while (bitcount < g_bitpool && sb < 8) {
        if (bits[ch][sb] < 16) {
            ++bits[ch][sb];
            ++bitcount;
        }
        if (ch == 1) {
            ch = 0;
            ++sb;
        } else
            ch = 1;
    }
}

static size_t frame_length(void)
{
    return 4 + (4 * 8 * 2) / 8 + (size_t)((16 * g_bitpool + 7) / 8);
}

/* 128 stereo samples -> one SBC frame; returns its length */
static size_t encode_frame(const double pcm[SAMPLES_PER_FRAME][2], unsigned char* out)
{
    double s[16][2][8];
    int sf[2][8], bits[2][8], blk, ch, sb;
    unsigned crc;
    const size_t len = frame_length();
    struct bitw w;
    for (blk = 0; blk < 16; ++blk)
        for (ch = 0; ch < 2; ++ch) {
            double in[8];
            int i;
            for (i = 0; i < 8; ++i)
                in[i] = pcm[blk * 8 + i][ch];
            analyze(ch, in, s[blk][ch]);
        }
    for (ch = 0; ch < 2; ++ch)
        for (sb = 0; sb < 8; ++sb) {
            double m = 0;
            int f = 0;
            for (blk = 0; blk < 16; ++blk)
                if (fabs(s[blk][ch][sb]) > m)
                    m = fabs(s[blk][ch][sb]);
            while (f < 15 && m >= (double)(2 << f))
                ++f;
            sf[ch][sb] = f;
        }
    allocate((const int(*)[8])sf, bits);
    memset(out, 0, len);
    out[0] = 0x9c;
    /* sampling frequency (2 = 44.1 kHz, 3 = 48 kHz), 16 blocks (3), stereo (2), SNR (1), 8 subbands (1) */
    out[1] = (unsigned char)(((g_rate == 48000 ? 3 : 2) << 6) | (3 << 4) | (2 << 2) | (1 << 1) | 1);
    out[2] = (unsigned char)g_bitpool;
    crc = crc8_bits(0x0f, out[1], 8);
    crc = crc8_bits(crc, out[2], 8);
    w.p = out;
    w.pos = 32;
    for (ch = 0; ch < 2; ++ch)
        for (sb = 0; sb < 8; ++sb) {
            put_bits(&w, (unsigned)sf[ch][sb], 4);
            crc = crc8_bits(crc, (unsigned)sf[ch][sb], 4);
        }
    out[3] = (unsigned char)crc;
    for (blk = 0; blk < 16; ++blk)
        for (ch = 0; ch < 2; ++ch)
            for (sb = 0; sb < 8; ++sb) {
                const int b = bits[ch][sb];
                if (b) {
                    const unsigned levels = (1u << b) - 1u;
                    const double scale = (double)(2 << sf[ch][sb]);
                    long q = (long)floor((s[blk][ch][sb] / scale + 1.0) * levels / 2.0);
                    if (q < 0)
                        q = 0;
                    if (q > (long)levels - 1)
                        q = (long)levels - 1;
                    put_bits(&w, (unsigned)q, b);
                }
            }
    return len;
}

/* ---- packets, made in real time ---- */
static struct {
    unsigned char data[MAXPACKET];
    size_t len;
} g_queue[QUEUE];
static int g_q_head, g_q_count;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;

static void* encoder(void* arg)
{
    static double pcm[SAMPLES_PER_FRAME][2];
    const double step = (double)IN_RATE / g_rate;
    const long period_ns = (long)(1e9 * FRAMES_PER_PACKET * SAMPLES_PER_FRAME / g_rate);
    double pos = 0; /* position between hist[1] and hist[2] for cubic interpolation */
    double hist[4][2];
    struct timespec next;
    int f, i, ch;
    (void)arg;
    memset(hist, 0, sizeof(hist));
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!g_stop) {
        unsigned char packet[MAXPACKET];
        size_t len = 1, fill;
        int real = 0;
        double adj = 1.0;
        /* Jitter buffer (car 2026-11-24: 10-20 short gaps a minute - the phone delivers in bursts and its clock differs a
         * little from ours). Playing starts only with TARGET in the ring (again after it ran dry), and the speed is
         * corrected by at most 0.5 % so that the fill level stays around TARGET instead of running empty or over. */
        pthread_mutex_lock(&g_ring_lock);
        fill = g_fill;
        pthread_mutex_unlock(&g_ring_lock);
        if (g_buffering && fill >= TARGET)
            g_buffering = 0;
        if (!g_buffering && !g_selftest) {
            adj = 1.0 + 0.005 * ((double)fill - TARGET) / TARGET;
            if (adj > 1.005)
                adj = 1.005;
            if (adj < 0.995)
                adj = 0.995;
        }
        for (f = 0; f < FRAMES_PER_PACKET; ++f) {
            for (i = 0; i < SAMPLES_PER_FRAME; ++i) {
                while (pos >= 1.0) { /* next input frame */
                    pthread_mutex_lock(&g_ring_lock);
                    memmove(hist[0], hist[1], 3 * sizeof(hist[0]));
                    if (g_fill && !g_buffering) {
                        hist[3][0] = g_ring[g_head][0];
                        hist[3][1] = g_ring[g_head][1];
                        g_head = (g_head + 1) % RING;
                        --g_fill;
                        real = 1;
                    } else {
                        if (!g_buffering && !g_selftest) {
                            g_buffering = 1; /* ran dry: wait for TARGET again */
                            ++g_underruns;
                        }
                        hist[3][0] = hist[3][1] = 0;
                    }
                    pthread_mutex_unlock(&g_ring_lock);
                    pos -= 1.0;
                }
                for (ch = 0; ch < 2; ++ch) { /* Catmull-Rom between hist[1] and hist[2] */
                    const double p0 = hist[0][ch], p1 = hist[1][ch], p2 = hist[2][ch], p3 = hist[3][ch], t = pos;
                    pcm[i][ch] = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
                }
                pos += step * adj;
            }
            len += encode_frame((const double(*)[2])pcm, packet + len);
        }
        packet[0] = FRAMES_PER_PACKET; /* A2DP SBC payload header: not fragmented, 8 frames */
        if (g_selftest) {
            fwrite(packet + 1, 1, len - 1, g_selftest);
            if (++g_packets >= 80) {
                g_stop = 1;
                break;
            }
            continue;
        }
        pthread_mutex_lock(&g_q_lock);
        if (g_q_count == QUEUE) { /* nobody reads: drop the oldest */
            g_q_head = (g_q_head + 1) % QUEUE;
            --g_q_count;
        }
        memcpy(g_queue[(g_q_head + g_q_count) % QUEUE].data, packet, len);
        g_queue[(g_q_head + g_q_count) % QUEUE].len = len;
        ++g_q_count;
        ++g_packets;
        if (!real)
            ++g_silent_packets;
        pthread_cond_broadcast(&g_q_cond);
        pthread_mutex_unlock(&g_q_lock);
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
    }
    return 0;
}

/* ---- the device ---- */
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_attr;

static int d_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    note("open by pid %d", ctp->info.pid);
    pthread_mutex_lock(&g_q_lock); /* start with fresh packets */
    g_q_count = 0;
    pthread_mutex_unlock(&g_q_lock);
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int d_read(resmgr_context_t* ctp, io_read_t* msg, iofunc_ocb_t* ocb)
{
    unsigned char data[MAXPACKET];
    size_t len, want = msg->i.nbytes;
    struct timespec until;
    int status = iofunc_read_verify(ctp, msg, ocb, 0);
    if (status != EOK)
        return status;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    pthread_mutex_lock(&g_q_lock);
    while (!g_q_count && !g_stop)
        if (pthread_cond_timedwait(&g_q_cond, &g_q_lock, &until) == ETIMEDOUT)
            break;
    if (!g_q_count) {
        pthread_mutex_unlock(&g_q_lock);
        return EAGAIN;
    }
    len = g_queue[g_q_head].len;
    memcpy(data, g_queue[g_q_head].data, len);
    g_q_head = (g_q_head + 1) % QUEUE;
    --g_q_count;
    ++g_read_packets;
    pthread_mutex_unlock(&g_q_lock);
    if (len > want)
        len = want;
    MsgReply(ctp->rcvid, (int)len, data, len);
    return _RESMGR_NOREPLY;
}

static int d_devctl(resmgr_context_t* ctp, io_devctl_t* msg, iofunc_ocb_t* ocb)
{
    if ((msg->i.dcmd & 0xffff) == 0x0520 && msg->i.nbytes >= 32) {
        uint32_t* v = (uint32_t*)_DEVCTL_DATA(msg->o);
        const int streaming = g_read_packets > 0;
        static int logged[2];
        memset(&msg->o, 0, sizeof(msg->o));
        v[0] = 1;
        v[1] = 1; /* SBC (AAC: 3) */
        v[2] = streaming ? 0 : 2;
        v[3] = streaming ? 2 : 0;
        v[4] = 1;
        v[5] = 1;
        v[6] = streaming ? 0x69b : 0;
        v[7] = 0;
        if (!logged[streaming]++)
            note("status asked by pid %d (%s)", ctp->info.pid, streaming ? "streaming" : "before streaming");
        msg->o.nbytes = 32;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + 32);
    }
    {
        const int r = iofunc_devctl_default(ctp, msg, ocb);
        if (r == _RESMGR_DEFAULT) {
            note("devctl %08x (%u bytes) from pid %d: not known", (unsigned)msg->i.dcmd, (unsigned)msg->i.nbytes, ctp->info.pid);
            return ENOTTY;
        }
        return r;
    }
}

int main(int argc, char** argv)
{
    const char* path = "/dev/wms/nbtaa";
    const char* log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    struct sigaction sa;
    pthread_t rt, et;
    int i;
    unsigned seconds = 0;
    /* Started by native_service in place of nbt_a2i_play (boot.sh copies it under that name when the iPod-over-Bluetooth
     * mode is on): "nbt_a2i_play audio.fifo [endpoint channels]" - the fifo is taken, the rest does not apply here. */
    if (argc >= 2 && strncmp(argv[1], "--", 2)) {
        g_fifo = argv[1];
        argc = 1;
    }
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fifo"))
            g_fifo = argv[i + 1];
        else if (!strcmp(argv[i], "--path"))
            path = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else if (!strcmp(argv[i], "--bitpool"))
            g_bitpool = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--rate"))
            g_rate = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--selftest"))
            g_selftest = fopen(argv[i + 1], "wb");
        else
            return 2;
    }
    if (g_selftest) { /* 1 kHz + 7.3 kHz tone, 2 s, through the same path as the session sound */
        sbc_init();
        for (i = 0; i < IN_RATE * 2; ++i) {
            const double v = 9000 * sin(2 * M_PI * 1000 * i / IN_RATE) + 5000 * sin(2 * M_PI * 7300 * i / IN_RATE);
            g_ring[i][0] = (int16_t)v;
            g_ring[i][1] = (int16_t)(-v / 2);
        }
        g_fill = IN_RATE * 2;
        encoder(0);
        fclose(g_selftest);
        return 0;
    }
    if (!g_fifo || g_bitpool < 2 || g_bitpool > 250 || (g_rate != 44100 && g_rate != 48000))
        return 2;
    if (log && !freopen(log, "a", stderr))
        return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    sbc_init();
    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 4096;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = d_open;
    g_io.read = d_read;
    g_io.devctl = d_devctl;
    iofunc_attr_init(&g_attr, S_IFNAM | 0666, 0, 0);
    if (resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0, &g_connect, &g_io, &g_attr) < 0) {
        note("resmgr_attach(%s) failed: %s", path, strerror(errno));
        return 5;
    }
    pthread_create(&rt, 0, reader, 0);
    pthread_create(&et, 0, encoder, 0);
    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.context_free = dispatch_context_free;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.lo_water = 2;
    pattr.hi_water = 4;
    pattr.increment = 1;
    pattr.maximum = 8;
    pool = thread_pool_create(&pattr, 0);
    if (!pool || thread_pool_start(pool) < 0)
        return 6;
    note("A2DP device %s: SBC %d Hz, bitpool %d, %u bytes per frame, sound from %s", path, g_rate, g_bitpool,
         (unsigned)frame_length(), g_fifo);
    while (!g_stop) {
        sleep(1);
        if (++seconds % 60 == 0)
            note("packets made %llu (silent %llu), read %llu, ran dry %llu times", g_packets, g_silent_packets, g_read_packets, g_underruns);
    }
    note("stopped");
    return 0;
}
