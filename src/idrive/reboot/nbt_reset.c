/* nbt_reset - reboot the NBT head unit from the head unit itself.
 * Sends UDS 11 01 (hardReset) to ECU 0x63 (head unit) through the gateway's HSFZ port 6801, the same request as
 * nbt_reset.py on the laptop and Tool32's STEUERGERAETE_RESET.
 *   nbt_reset            find the gateway with the HSFZ 6811 broadcast, then reset
 *   nbt_reset <ip>       use this gateway address
 *   nbt_reset -n [ip]    only find / connect to the gateway, no reset (test)
 * Car 2026-11-12: from the head unit, 169.254.255.255 gives "No route to host"; 255.255.255.255 is tried next.
 * With the address given, the reset was accepted and the head unit rebooted.
 * Installed with the NbtReboot plugin, which runs it from the "Restart now" button.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TESTER 0xF4
#define HU 0x63

static int wait_fd(int fd, int write, int ms)
{
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
}

/* HSFZ identification broadcast to one address; the gateway answers from its own address */
static int discover_at(const char *bcast, struct in_addr *gw)
{
    static const unsigned char ident[6] = {0, 0, 0, 0, 0x00, 0x11};
    unsigned char buf[512];
    struct sockaddr_in to, from;
    socklen_t len = sizeof(from);
    int on = 1, s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(6811);
    to.sin_addr.s_addr = inet_addr(bcast);
    if (sendto(s, ident, sizeof(ident), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "broadcast %s: %s\n", bcast, strerror(errno));
        close(s);
        return -1;
    }
    if (wait_fd(s, 0, 4000) <= 0 || recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &len) <= 0) { close(s); return -1; }
    close(s);
    *gw = from.sin_addr;
    return 0;
}

static int discover(struct in_addr *gw)
{
    return discover_at("169.254.255.255", gw) == 0 || discover_at("255.255.255.255", gw) == 0 ? 0 : -1;
}

static int read_exact(int s, unsigned char *b, int n, int ms)
{
    int got = 0;
    while (got < n) {
        int r;
        if (wait_fd(s, 0, ms) <= 0) return -1;
        r = read(s, b + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const unsigned char req[10] = {0, 0, 0, 4, 0x00, 0x01, TESTER, HU, 0x11, 0x01};
    struct sockaddr_in gw;
    unsigned char hdr[6], body[256];
    int s, flags, err = 0, tries, test = 0;
    socklen_t elen = sizeof(err);

    memset(&gw, 0, sizeof(gw));
    gw.sin_family = AF_INET;
    gw.sin_port = htons(6801);
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { test = 1; --argc; ++argv; }
    if (argc > 1) {
        gw.sin_addr.s_addr = inet_addr(argv[1]);
    } else if (discover(&gw.sin_addr) != 0) {
        fprintf(stderr, "no gateway answered the 6811 broadcast; pass its address: nbt_reset <ip>\n");
        return 1;
    }
    printf("gateway %s\n", inet_ntoa(gw.sin_addr));
    fflush(stdout);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    if (connect(s, (struct sockaddr *)&gw, sizeof(gw)) < 0 && errno != EINPROGRESS) { perror("connect"); return 1; }
    if (wait_fd(s, 1, 5000) <= 0) { fprintf(stderr, "connect: timed out\n"); return 1; }
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err) { fprintf(stderr, "connect: %s\n", strerror(err)); return 1; }
    fcntl(s, F_SETFL, flags);
    if (test) { printf("connected to the gateway's diagnostic port, no reset sent (-n)\n"); return 0; }

    if (write(s, req, sizeof(req)) != (int)sizeof(req)) { perror("send"); return 1; }
    for (tries = 0; tries < 8; ++tries) {
        unsigned len, ctrl;
        if (read_exact(s, hdr, 6, 5000) < 0) break;
        len = ((unsigned)hdr[0] << 24) | ((unsigned)hdr[1] << 16) | ((unsigned)hdr[2] << 8) | hdr[3];
        ctrl = ((unsigned)hdr[4] << 8) | hdr[5];
        if (len < 2 || len > sizeof(body) || read_exact(s, body, (int)len, 5000) < 0) break;
        if (ctrl == 0x0002) continue;                           /* the gateway's ack of our request */
        if (len >= 4 && body[2] == 0x51 && body[3] == 0x01) { printf("reset accepted (51 01), rebooting\n"); return 0; }
        if (len >= 5 && body[2] == 0x7F && body[3] == 0x11) {
            if (body[4] == 0x78) continue;                      /* responsePending */
            printf("reset refused: 7F 11 %02X\n", body[4]);
            return 2;
        }
        printf("unexpected answer, %u bytes\n", len);
    }
    printf("no answer from the gateway\n");
    return 3;
}
