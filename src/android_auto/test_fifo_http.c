/* test_fifo_http: VM test of fifo_http with a stand-in for the player (no Cinemo needed).
 *
 * Run in an empty directory that contains ./fifo_http:   ./test_fifo_http
 * A feeder thread plays the session: it writes a codec config and then padded pictures (20 per second) into the FIFO and
 * writes a new config whenever fifo_http asks for a resync (file http.resync), as native_service + the session do.
 * The main thread plays the player:
 *   1. reads, then does not read for 4 s, then reads again   -> the connection must survive (it died after 2 s before)
 *   2. closes and connects again                             -> the new connection must begin with the codec config
 *   3. closes for good                                       -> fifo_http must end by itself (reconnect time-out)
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static volatile int feeding = 1;
static volatile int resyncs;

static void put(int fd, const unsigned char* p, int n)
{
    while (n > 0 && feeding) {
        int k = write(fd, p, n);
        if (k > 0) {
            p += k;
            n -= k;
        } else
            usleep(5000);
    }
}

static void* feeder(void* unused)
{
    static unsigned char config[] = {0, 0, 0, 1, 0x67, 0x42, 0x80, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    static unsigned char picture[8198];
    int fd = open("annexb.fifo", O_WRONLY | O_NONBLOCK);
    (void)unused;
    memset(picture, 0xff, sizeof(picture));
    picture[0] = picture[1] = picture[2] = 0;
    picture[3] = 1;
    picture[4] = 0x41;
    put(fd, config, sizeof(config));
    while (feeding) {
        if (!access("http.resync", F_OK)) {
            unlink("http.resync");
            ++resyncs;
            usleep(300000); /* the phone needs a moment for the key frame */
            put(fd, config, sizeof(config));
        }
        put(fd, picture, sizeof(picture));
        usleep(50000);
    }
    close(fd);
    return 0;
}

static int connect_player(void)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    static const char request[] = "GET /live.h264 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(18765);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a))) {
        close(fd);
        return -1;
    }
    if (write(fd, request, sizeof(request) - 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* read n bytes; the first up to 200 are kept in head. Returns the number read. */
static int take(int fd, int n, unsigned char* head, int* head_n)
{
    unsigned char b[4096];
    int got = 0;
    while (got < n) {
        int k = read(fd, b, sizeof(b));
        if (k <= 0)
            break;
        if (head && *head_n < 200) {
            int c = k < 200 - *head_n ? k : 200 - *head_n;
            memcpy(head + *head_n, b, c);
            *head_n += c;
        }
        got += k;
    }
    return got;
}

int main(void)
{
    pthread_t t;
    pid_t child;
    unsigned char head[200];
    int head_n = 0, fd, got, status, i, ok = 1;
    char* body;

    signal(SIGPIPE, SIG_IGN);
    unlink("annexb.fifo");
    unlink("http.ready");
    unlink("http.log");
    unlink("http.resync");
    if (mkfifo("annexb.fifo", 0600))
        return 2;
    child = fork();
    if (!child) {
        execl("./fifo_http", "fifo_http", "annexb.fifo", "http.ready", (char*)0);
        _exit(127);
    }
    usleep(300000);
    pthread_create(&t, 0, feeder, 0);

    fd = connect_player();
    if (fd < 0) {
        puts("FAIL connect");
        return 1;
    }
    got = take(fd, 200000, head, &head_n);
    head[head_n < 199 ? head_n : 199] = 0;
    body = strstr((char*)head, "\r\n\r\n");
    printf("1. first connection: %d bytes, header %s, stream starts with config %s\n", got, body ? "ok" : "MISSING",
           body && !memcmp(body + 4, "\0\0\0\1\x67", 5) ? "yes" : "NO");
    ok = ok && body && !memcmp(body + 4, "\0\0\0\1\x67", 5) && !access("http.ready", F_OK);
    sleep(4); /* the player does not read */
    got = take(fd, 300000, 0, 0);
    printf("   after 4 s without reading: %d bytes more (%s)\n", got, got >= 300000 ? "connection survived" : "DIED");
    ok = ok && got >= 300000;
    close(fd);

    sleep(1);
    fd = connect_player();
    head_n = 0;
    got = fd < 0 ? -1 : take(fd, 100000, head, &head_n);
    head[head_n < 199 ? head_n : 199] = 0;
    body = fd < 0 ? 0 : strstr((char*)head, "\r\n\r\n");
    i = body && (!memcmp(body + 4, "\0\0\0\1\x67", 5) || !memcmp(body + 4, "\0\0\1\x67", 4));
    printf("2. reconnect: %d bytes, resync requests %d, new stream starts with config %s\n", got, resyncs, i ? "yes" : "NO");
    ok = ok && got >= 100000 && resyncs == 1 && i;
    if (fd >= 0)
        close(fd);

    for (i = 0; i < 150 && waitpid(child, &status, WNOHANG) == 0; ++i)
        usleep(100000);
    printf("3. after the last close: fifo_http %s after %d.%d s\n", i < 150 ? "ended by itself" : "STILL RUNNING", i / 10,
           i % 10);
    ok = ok && i < 150 && i >= 80;
    if (i >= 150) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    feeding = 0;
    pthread_join(t, 0);
    puts(ok ? "FIFO_HTTP_TEST_OK" : "FIFO_HTTP_TEST_FAILED");
    return ok ? 0 : 1;
}
