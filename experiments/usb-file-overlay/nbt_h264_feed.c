/*
 * nbt_h264_feed - emulate the live Android Auto capture on the car without a phone: reads an Annex B H.264 file
 * that contains access unit delimiters (x264 aud=1) and writes it access unit by access unit into a FIFO in real
 * time (default 30 fps), looping the file. The FIFO is the one nbt_live_file reads (annexb.fifo).
 *
 * usage: nbt_h264_feed <file.h264> <fifo> [--fps N] [--seconds N]
 *   --fps N      access units per second (1..120, default 30)
 *   --seconds N  stop after N seconds (1..3600, default 120)
 * Only paths under /fs/ram3/ are accepted. The FIFO is opened read/write so the feeder never blocks on open.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int ok_path(const char *p) { return !strncmp(p, "/fs/ram3/", 9) && !strstr(p, ".."); }

/* offset of the next access unit delimiter (00 00 01 09 or 00 00 00 01 09) at or after 'from', or size */
static size_t next_aud(const unsigned char *d, size_t size, size_t from)
{
    size_t i;
    for (i = from; i + 4 < size; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 9)
            return (i > from && d[i - 1] == 0) ? i - 1 : i;
    return size;
}

int main(int argc, char **argv)
{
    const char *file, *fifo;
    long fps = 30, seconds = 120;
    int i, fd;
    FILE *f;
    unsigned char *d;
    long size;
    long long period, next, end;
    unsigned long long aus = 0, bytes = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s <file.h264> <fifo> [--fps N] [--seconds N]\n", argv[0]); return 2; }
    file = argv[1];
    fifo = argv[2];
    for (i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fps")) fps = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atol(argv[i + 1]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (fps < 1 || fps > 120 || seconds < 1 || seconds > 3600 || !ok_path(file) || !ok_path(fifo)) {
        fprintf(stderr, "bad arguments (paths must be under /fs/ram3/)\n");
        return 2;
    }
    f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", file); return 3; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) { fclose(f); return 3; }
    d = malloc((size_t)size);
    if (!d || fread(d, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 3; }
    fclose(f);
    fd = open(fifo, O_RDWR);
    if (fd < 0) { fprintf(stderr, "cannot open fifo %s\n", fifo); return 4; }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    period = 1000000000LL / fps;
    next = now_ns();
    end = next + (long long)seconds * 1000000000LL;
    while (!g_stop && now_ns() < end) {
        size_t pos = next_aud(d, (size_t)size, 0);
        if (pos >= (size_t)size) { fprintf(stderr, "no access unit delimiters in %s\n", file); break; }
        while (!g_stop && pos < (size_t)size && now_ns() < end) {
            size_t stop_at = next_aud(d, (size_t)size, pos + 5);
            size_t len = stop_at - pos, done = 0;
            long long wait;
            while (done < len && !g_stop) {
                ssize_t n = write(fd, d + pos + done, len - done);
                if (n < 0) { if (errno == EINTR) continue; g_stop = 1; break; }
                done += (size_t)n;
            }
            aus++;
            bytes += len;
            pos = stop_at;
            next += period;
            wait = next - now_ns();
            if (wait > 0) {
                struct timespec ts;
                ts.tv_sec = (time_t)(wait / 1000000000LL);
                ts.tv_nsec = (long)(wait % 1000000000LL);
                nanosleep(&ts, NULL);
            } else if (wait < -500000000LL) {
                next = now_ns();
            }
        }
    }
    close(fd);
    fprintf(stderr, "fed %llu access units, %llu bytes\n", aus, bytes);
    return 0;
}
