/* fifo_http: loopback HTTP server that streams H.264 from a FIFO to the stock Cinemo player.
 *
 * The stock player (started by decode_runner) cannot read a FIFO directly, but it can play an HTTP URL. This program
 * bridges the two: it listens on 127.0.0.1:18765, answers the player's request with an endless "video/h264" response and
 * copies everything that arrives in the FIFO to that connection.
 *
 * Usage: fifo_http <h264 fifo> <ready file>
 *   <ready file> is created (must not exist yet) once the first client is connected, so the service knows the player is
 *   reading.
 *
 * Exit codes: 0 = stopped, or the player is gone for good, 2 = bad arguments or setup error, 3 = no usable first client.
 *
 * Behaviour that matters for stability (2026-11-12, from the car logs of that day):
 *   - A player that does not read is waited for (NBT_HTTP_STALL_MS, default 10 s; it was 2 s). Every chain restart of
 *     that day began with exactly this 2 s give-up, while the player was still running and working off its own queue.
 *     With the video ack gate in the session the phone simply pauses meanwhile, nothing piles up.
 *   - The player may reconnect. The head unit's Cinemo configuration makes it drop a connection that brings no data for
 *     5 s and connect again 3 s later (http-waiting-for-data-timeout, http-retry-delay). The old program had closed its
 *     listening socket after the first client, so that reconnect ("Couldn't connect to server") ended the chain. Now a
 *     new connection replaces the old one; the file http.resync asks the service to make the session start the stream
 *     again with config + key frame, and nothing is sent to the new connection before that config.
 *   - TCP_NODELAY: a picture goes out in several pieces smaller than a TCP segment (the FIFO is small); with the Nagle
 *     algorithm each of them could wait for the receiver's delayed acknowledgement.
 *   - http.log (appended, in the working directory): connections, stalls of 500 ms and more, and why the program ended.
 */
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HTTP_PORT 18765
#define LOG_FILE "http.log"
#define RESYNC_FILE "http.resync"

static volatile sig_atomic_t stop;
static int g_server = -1, g_client = -1;
static long g_stall_ms = 10000;    /* how long a connected player may not read */
static long g_reconnect_ms = 9000; /* how long to wait for a new connection after the player closed its own */
static int g_sync;                 /* 1 = drop FIFO data until the next codec config (SPS) */
static unsigned char g_tail[4];    /* last bytes of the previous chunk while searching for the SPS start code */
static int g_tail_n;
static unsigned long long g_bytes; /* sent to the current connection */

static void stopped(int signal_number)
{
    (void)signal_number;
    stop = 1;
}

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void note(const char* format, ...)
{
    struct stat st;
    FILE* f;
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    f = fopen(LOG_FILE, !stat(LOG_FILE, &st) && st.st_size > 200000 ? "w" : "a");
    if (!f)
        return;
    gmtime_r(&t, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, format);
    vfprintf(f, format, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void drop_client(void)
{
    if (g_client >= 0)
        close(g_client);
    g_client = -1;
}

/* Take a waiting connection: read its request, answer with the header. The newest connection replaces the current one.
 * Returns 1 when a new client is connected. */
static int take_client(int first)
{
    static const char header[] = "HTTP/1.0 200 OK\r\nContent-Type: video/h264\r\nConnection: close\r\n\r\n";
    char request[4096];
    int fd = accept(g_server, 0, 0), used = 0, one = 1;
    long long limit = now_ms() + 5000;
    if (fd < 0)
        return 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    request[0] = 0;
    while (!strstr(request, "\r\n\r\n") && used < (int)sizeof(request) - 1 && !stop && now_ms() < limit) {
        fd_set set;
        struct timeval timeout = {0, 200000};
        int n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, 0, 0, &timeout) <= 0)
            continue;
        n = recv(fd, request + used, sizeof(request) - 1 - used, 0);
        if (n <= 0)
            break;
        used += n;
        request[used] = 0;
    }
    if (!strstr(request, "\r\n\r\n") || send(fd, header, sizeof(header) - 1, 0) != (int)sizeof(header) - 1) {
        close(fd);
        note("connection without a usable request dropped");
        return 0;
    }
    if (g_client >= 0)
        note("new connection replaces the current one after %llu bytes", g_bytes);
    drop_client();
    g_client = fd;
    g_bytes = 0;
    if (first)
        note("player connected");
    else {
        /* a connection in the middle of the stream: it must begin with the codec config */
        int resync = open(RESYNC_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (resync >= 0)
            close(resync);
        g_sync = 1;
        g_tail_n = 0;
        note("player reconnected: waiting for config + key frame");
    }
    return 1;
}

/* Send all n bytes to the client. Returns 1 on success; 0 when the connection is gone, was replaced, the player did not
 * read for g_stall_ms, or we are stopping. */
static int send_all(const unsigned char* data, unsigned n)
{
    long long blocked = 0;
    while (n && !stop && g_client >= 0) {
        int sent = send(g_client, data, n, 0);
        if (sent > 0) {
            if (blocked && now_ms() - blocked >= 500)
                note("player did not read for %lld ms", now_ms() - blocked);
            blocked = 0;
            data += sent;
            n -= sent;
            g_bytes += sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && errno == EAGAIN) {
            fd_set wr, rd;
            struct timeval timeout = {0, 200000};
            int top = g_client > g_server ? g_client : g_server;
            if (!blocked)
                blocked = now_ms();
            if (now_ms() - blocked >= g_stall_ms) {
                note("player did not read for %lld ms: giving up on this connection", now_ms() - blocked);
                drop_client();
                return 0;
            }
            FD_ZERO(&wr);
            FD_ZERO(&rd);
            FD_SET(g_client, &wr);
            FD_SET(g_server, &rd);
            if (select(top + 1, &rd, &wr, 0, &timeout) > 0 && FD_ISSET(g_server, &rd) && take_client(0))
                return 0; /* the rest of this chunk belongs to the old connection */
            continue;
        }
        note("connection closed by the player after %llu bytes (errno %d)", g_bytes, errno);
        drop_client();
        return 0;
    }
    return !n;
}

/* While g_sync: g_join = the last 3 bytes of the previous chunk + this chunk (so a start code cut in two by the chunk
 * border is found). Returns the offset in g_join where the codec config begins (start code + SPS, NAL type 7), or -1
 * after keeping the new tail. *total = bytes in g_join. */
static unsigned char g_join[65536 + 4];

static int find_config(const unsigned char* data, int n, int* total)
{
    int i;
    memcpy(g_join, g_tail, g_tail_n);
    memcpy(g_join + g_tail_n, data, n);
    *total = g_tail_n + n;
    for (i = 0; i + 4 <= *total; ++i)
        if (!g_join[i] && !g_join[i + 1] && g_join[i + 2] == 1 && (g_join[i + 3] & 0x1f) == 7)
            return i > 0 && !g_join[i - 1] ? i - 1 : i;
    g_tail_n = *total < 3 ? *total : 3;
    memmove(g_tail, g_join + *total - g_tail_n, g_tail_n);
    return -1;
}

int main(int argc, char** argv)
{
    static unsigned char data[65536];
    int fifo, one = 1, top;
    struct sockaddr_in addr;
    long long limit, alone_since = 0;
    const char* e;

    if (argc != 3)
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    if ((e = getenv("NBT_HTTP_STALL_MS")) && atol(e) >= 500 && atol(e) <= 120000)
        g_stall_ms = atol(e);
    if ((e = getenv("NBT_HTTP_RECONNECT_MS")) && atol(e) >= 0 && atol(e) <= 120000)
        g_reconnect_ms = atol(e);

    /* O_RDWR keeps the FIFO open even while no writer is attached, so reads never see EOF between sessions */
    fifo = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fifo < 0)
        return 2;

    /* loopback only; the socket keeps listening for the player's reconnects */
    g_server = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server < 0)
        return 2;
    setsockopt(g_server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_server, (struct sockaddr*)&addr, sizeof(addr)) || listen(g_server, 2))
        return 2;
    fcntl(g_server, F_SETFL, fcntl(g_server, F_GETFL) | O_NONBLOCK);
    unlink(RESYNC_FILE);

    /* the first client (the service gives the player 10 s to connect) */
    limit = now_ms() + 60000;
    while (g_client < 0 && !stop && now_ms() < limit) {
        fd_set set;
        struct timeval timeout = {0, 200000};
        FD_ZERO(&set);
        FD_SET(g_server, &set);
        if (select(g_server + 1, &set, 0, 0, &timeout) > 0)
            take_client(1);
    }
    if (g_client < 0)
        return stop ? 0 : 3;
    {
        int ready = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (ready < 0)
            return 3;
        if (write(ready, "ready\n", 6) != 6) {
            close(ready);
            return 3;
        }
        close(ready);
    }

    /* copy FIFO -> client until stopped or the player is gone for good */
    while (!stop) {
        fd_set set;
        struct timeval timeout = {0, 200000};
        int n;
        FD_ZERO(&set);
        FD_SET(fifo, &set);
        FD_SET(g_server, &set);
        top = fifo > g_server ? fifo : g_server;
        if (g_client >= 0) {
            FD_SET(g_client, &set); /* readable = the player closed the connection (it never sends after the request) */
            if (g_client > top)
                top = g_client;
        }
        if (select(top + 1, &set, 0, 0, &timeout) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (FD_ISSET(g_server, &set))
            take_client(0);
        else if (g_client >= 0 && FD_ISSET(g_client, &set)) {
            char scrap[256];
            int r = recv(g_client, scrap, sizeof(scrap), 0);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                note("connection closed by the player after %llu bytes", g_bytes);
                drop_client();
            }
        }
        if (g_client < 0) {
            /* nobody connected: wait for the reconnect, meanwhile throw the stream away so the session is not held up */
            if (!alone_since)
                alone_since = now_ms();
            if (now_ms() - alone_since >= g_reconnect_ms) {
                note("no reconnect within %ld ms: ending (the service starts a new player)", g_reconnect_ms);
                break;
            }
        } else
            alone_since = 0;
        if (!FD_ISSET(fifo, &set))
            continue;
        n = read(fifo, data, sizeof(data));
        if (n <= 0 || g_client < 0)
            continue;
        if (g_sync) {
            int total, at = find_config(data, n, &total);
            if (at < 0)
                continue;
            g_sync = 0;
            note("config found: stream goes to the new connection");
            send_all(g_join + at, total - at);
            continue;
        }
        send_all(data, n);
    }
    if (stop)
        note("stopped by the service after %llu bytes on the last connection", g_bytes);
    drop_client();
    close(g_server);
    close(fifo);
    return 0;
}
