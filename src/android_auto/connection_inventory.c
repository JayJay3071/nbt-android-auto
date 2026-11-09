/* QNX kernel-only connection inventory: no device opens, messages or fd names.
 * Avoids pidin fds / io_fdinfo calls into potentially unresponsive services.
 * API: QNX 6.5 ConnectServerInfo documentation (see ANDROID-AUTO.md).
 * A connection is evidence of a client, NOT proof of exclusive ownership.
 */
#include <sys/neutrino.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

static int scan(pid_t client, pid_t server, int verbose) {
    int next = 0, matches = 0;
    unsigned count;
    for (count = 0; count < 65536; ++count) {
        struct _server_info info;
        int found;
        memset(&info, 0, sizeof(info));
        found = ConnectServerInfo(client, next, &info);
        if (found < 0) {
            if (errno == EINVAL) return matches;
            fprintf(stderr, "CONNECTION_QUERY_FAILED client=%ld errno=%d\n", (long)client, errno);
            return -1;
        }
        if (found < next) return -1;
        if (info.nd == 0 && info.pid == server && !(info.flags & _NTO_COF_DEAD)) {
            ++matches;
            if (verbose) printf("CONNECTION client=%ld coid=%d server=%ld chid=%ld flags=%x\n",
                    (long)client, found, (long)server, (long)info.chid, (unsigned short)info.flags);
        }
        if (found == INT_MAX) return matches;
        next = found + 1;
    }
    fprintf(stderr, "CONNECTION_QUERY_LIMIT\n");
    return -1;
}

static int selftest(void) {
    int ch = ChannelCreate(0), fd, baseline, status;
    pid_t parent = getpid(), child;
    if (ch < 0) return 1;
    baseline = scan(parent, parent, 0);
    /* Side channel: fork/exit must not send POSIX IO_CLOSE to our dummy server. */
    fd = ConnectAttach(0, parent, ch, _NTO_SIDE_CHANNEL, 0);
    if (baseline < 0 || fd < 0 || scan(parent, parent, 0) != baseline + 1) return 1;
    child = fork();
    if (child < 0) return 1;
    if (child == 0) _exit(scan(parent, parent, 0) == baseline + 1 ? 0 : 1);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    if (ConnectDetach(fd) != 0 || scan(parent, parent, 0) != baseline || ChannelDestroy(ch) != 0) return 1;
    puts("CONNECTION_SELFTEST_OK own-connection cross-process detach");
    return 0;
}

static int positive_pid(const char* text) {
    char* end;
    long value;
    const char* p;
    if (!*text) return -1;
    for (p = text; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || *end || value <= 0 || value > INT_MAX) return -1;
    return (int)value;
}

int main(int argc, char** argv) {
    int client, server, matches;
    alarm(5);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 1) return selftest();
    if (argc != 3 || (client = positive_pid(argv[1])) < 0 || (server = positive_pid(argv[2])) < 0) {
        fprintf(stderr, "Usage: connection_inventory [client-pid server-pid]\n");
        return 2;
    }
    matches = scan(client, server, 1);
    if (matches < 0) return 3;
    printf("CONNECTION_QUERY_OK client=%d server=%d matches=%d exclusive_ownership_unknown=1\n",
           client, server, matches);
    return 0;
}
