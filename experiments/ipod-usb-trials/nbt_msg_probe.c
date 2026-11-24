/* nbt_msg_probe: research tool. Sends one raw message (given as hex bytes) to an opened path and prints the status and
 * the bytes that come back. Used to learn what the stock iPod driver answers to the message the HMI's media application
 * sends to ".FS_info./control" right after an iPod was announced (car 2026-11-14), so that our stand-in can give the
 * same answer.
 * Usage: nbt_msg_probe <path> <reply bytes> <hex byte> <hex byte> ...
 * Only for messages that were seen being sent by the stock software itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    static unsigned char out[512], in[65536];
    unsigned n = 0, room, i, last = 0;
    int fd, rc;
    if (argc < 4)
        return 2;
    room = (unsigned)atoi(argv[2]);
    if (room > sizeof(in))
        room = sizeof(in);
    for (i = 3; i < (unsigned)argc && n < sizeof(out); ++i)
        out[n++] = (unsigned char)strtoul(argv[i], 0, 16);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    errno = 0;
    rc = MsgSend(fd, out, n, in, room);
    printf("sent %u bytes to %s: status %d errno %d (%s)\n", n, argv[1], rc, errno, errno ? strerror(errno) : "ok");
    for (i = 0; i < room; ++i)
        if (in[i])
            last = i + 1;
    printf("reply (%u bytes used):", last);
    for (i = 0; i < last && i < 400; ++i)
        printf(" %02x", in[i]);
    printf("\ntext: ");
    for (i = 0; i < last && i < 2000; ++i)
        putchar(in[i] >= 32 && in[i] < 127 ? in[i] : '.');
    printf("\n");
    close(fd);
    return 0;
}
