/* nbt_devctl_probe: research tool. Sends one devctl to a path and prints what comes back (text and hex).
 * Used to see what the stock iPod driver answers to the requests mme sends to its ".FS_info./control" file, so that
 * nbt_aa_usb can give the same kind of answer.
 * Usage: nbt_devctl_probe <path> <dcmd hex> <bytes> [input text]
 * Only "get" style requests are meant to be used (direction bit 0x40000000); the program refuses others unless the
 * fourth argument is given.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    static unsigned char buf[65536];
    unsigned dcmd, n, i;
    int fd, rc, info = 0;
    if (argc < 4)
        return 2;
    dcmd = (unsigned)strtoul(argv[2], 0, 16);
    n = (unsigned)atoi(argv[3]);
    if (n > sizeof(buf))
        n = sizeof(buf);
    if ((dcmd & 0x80000000u) && argc < 5) {
        printf("refused: %08x sends data to the device\n", dcmd);
        return 2;
    }
    if (argc >= 5)
        strncpy((char*)buf, argv[4], sizeof(buf) - 1);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    rc = devctl(fd, (int)dcmd, buf, n, &info);
    printf("devctl %08x on %s: rc=%d (%s) info=%d\n", dcmd, argv[1], rc, rc ? strerror(rc) : "ok", info);
    if (!rc && getenv("OUT")) { /* raw answer into a file (info = its length when the driver reports one) */
        FILE* f = fopen(getenv("OUT"), "wb");
        if (f) {
            fwrite(buf, 1, info > 0 && (unsigned)info <= n ? (unsigned)info : n, f);
            fclose(f);
        }
    }
    if (!rc) {
        unsigned last = 0;
        for (i = 0; i < n; ++i)
            if (buf[i])
                last = i + 1;
        printf("text: ");
        for (i = 0; i < last && i < 3000; ++i)
            putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : buf[i] == '\n' ? '\n' : '.');
        printf("\nhex (%u bytes used): ", last);
        for (i = 0; i < last && i < 96; ++i)
            printf("%02x ", buf[i]);
        printf("\n");
    }
    close(fd);
    return 0;
}
