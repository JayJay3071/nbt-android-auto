/* nbt_launch_tap: research tool. Listens at the message channel of the stock USB launcher (/dev/medialauncher) as
 * one more reader and writes down what arrives - the text lines the head unit's software gets when a USB device comes
 * or goes ("ADD_USB_AUD_/fs/ipod0 DETECTION_FINISHED", "REM_USB_...").
 *
 * Why (2026-11-14): Android Auto shall appear as an iPod source without an iPhone on the USB. The source is listed
 * only after the launcher has announced an iPod, so the next step is to announce one ourselves. Before anything is
 * put in front of that channel, this tool shows whether a plain extra reader is given the messages at all (the
 * launcher's own log suggests it serves each reader separately) and what they look like byte for byte.
 *
 * It only opens the channel for reading, waits with select() and reads. It sends no devctl and writes nothing.
 * Usage: nbt_launch_tap [--path /dev/medialauncher] [--log file] [--seconds 600]
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void stopped(int n)
{
    (void)n;
    stop = 1;
}

static void stamp(FILE* f)
{
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int main(int argc, char** argv)
{
    const char* path = "/dev/medialauncher";
    FILE* out = stdout;
    long seconds = 600;
    unsigned empty = 0;
    time_t end;
    struct stat st;
    int fd, i;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--path"))
            path = argv[i + 1];
        else if (!strcmp(argv[i], "--log")) {
            out = fopen(argv[i + 1], "a");
            if (!out)
                return 2;
        } else if (!strcmp(argv[i], "--seconds"))
            seconds = atol(argv[i + 1]);
        else
            return 2;
    }
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    stamp(out);
    if (fd < 0) {
        fprintf(out, "open %s failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (!fstat(fd, &st))
        fprintf(out, "opened %s (mode %o), listening for %ld s\n", path, (unsigned)st.st_mode, seconds);
    else
        fprintf(out, "opened %s, listening for %ld s\n", path, seconds);
    fflush(out);
    end = time(0) + seconds;
    while (!stop && time(0) < end) {
        char buf[512];
        fd_set set;
        struct timeval wait = {2, 0};
        int ready, k;
        ssize_t n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        ready = select(fd + 1, &set, 0, 0, &wait);
        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            stamp(out);
            fprintf(out, "message (%d bytes, select %d): ", (int)n, ready);
            for (k = 0; k < n; ++k)
                fputc(buf[k] >= 32 && buf[k] < 127 ? buf[k] : '.', out);
            fputs(" |", out);
            for (k = 0; k < n && k < 64; ++k)
                fprintf(out, " %02x", (unsigned char)buf[k]);
            fputc('\n', out);
            fflush(out);
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            stamp(out);
            fprintf(out, "read error: %s\n", strerror(errno));
            fflush(out);
            sleep(1);
        } else if (ready > 0 && ++empty <= 5) { /* woken, but nothing for this reader */
            stamp(out);
            fprintf(out, "woken without a message for us (%u)\n", empty);
            fflush(out);
        }
    }
    stamp(out);
    fputs("end\n", out);
    close(fd);
    return 0;
}
