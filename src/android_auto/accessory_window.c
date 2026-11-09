/* Temporary QNX path-space lease for a controlled phone reconnect.
 * Resource-manager technique adapted from NBT-AAD usb_direct.cpp.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
 * No file modification, USB requests, endpoint reads or service termination.
 */
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static volatile sig_atomic_t stopping;
static void stop_window(int sig) { (void)sig; stopping = 1; }
static int refuse_open(resmgr_context_t* c, io_open_t* m, RESMGR_HANDLE_T* h, void* e) {
    (void)c; (void)m; (void)h; (void)e;
    return EACCES;
}
static double seconds_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) _exit(8);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int serve(const char* const* paths, unsigned count, unsigned duration, int ready_fd) {
    dispatch_t* dp = NULL;
    dispatch_context_t* context = NULL;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    resmgr_attr_t ra;
    iofunc_attr_t attr;
    struct timespec tick = {0, 100000000L};
    int ids[6] = {-1, -1, -1, -1, -1, -1}, result = 1;
    unsigned i;
    double end;
    pid_t owner=getppid();
    if (!count || count > 6 || duration > 120) return 2;
    stopping = 0;
    signal(SIGTERM, stop_window); signal(SIGINT, stop_window);
    /* Hard fallback uses default SIGALRM: process exit removes registrations. */
    signal(SIGALRM, SIG_DFL); if(duration)alarm(duration + 5);
    dp = dispatch_create();
    if (!dp) goto done;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect, _RESMGR_IO_NFUNCS, &io);
    connect.open = refuse_open;
    connect.unlink = NULL; connect.rename = NULL; connect.mknod = NULL;
    connect.readlink = NULL; connect.link = NULL; connect.mount = NULL;
    iofunc_attr_init(&attr, S_IFREG | 0555, NULL, NULL);
    memset(&ra, 0, sizeof(ra)); ra.nparts_max = 1; ra.msg_max_size = 2048;
    for (i = 0; i < count; ++i) {
        ids[i] = resmgr_attach(dp, &ra, paths[i], _FTYPE_ANY, 0, &connect, &io, &attr);
        if (ids[i] == -1) goto done;
    }
    context = dispatch_context_alloc(dp);
    if (!context) goto done;
    dispatch_timeout(dp, &tick);
    end = seconds_now() + duration;
    if (ready_fd >= 0) {
        if (write(ready_fd, "R", 1) != 1) goto done;
        close(ready_fd); ready_fd = -1;
    }
    printf("ACCESSORY_WINDOW_READY seconds=%u paths=%u pid=%ld\n", duration, count, (long)getpid());
    while (!stopping && (duration ? seconds_now() < end : getppid()==owner && owner>1)) {
        dispatch_context_t* received = dispatch_block(context);
        if (received) { context = received; dispatch_handler(context); }
        else if (errno != ETIMEDOUT && errno != EINTR) goto done;
    }
    result = 0;
done:
    if (result) fprintf(stderr, "ACCESSORY_WINDOW_FAILED errno=%d\n", errno);
    if (ready_fd >= 0) close(ready_fd);
    if (dp) for (i = 0; i < count; ++i) if (ids[i] != -1) resmgr_detach(dp, ids[i], _RESMGR_DETACH_ALL);
    if (context) dispatch_context_free(context);
    if (dp) dispatch_destroy(dp);
    alarm(0);
    puts("ACCESSORY_WINDOW_RELEASED");
    return result;
}
static void require(int condition, const char* name) {
    if (!condition) { fprintf(stderr, "FAIL %s errno=%d\n", name, errno); exit(1); }
}
static void readable(const char* path) {
    char b[8]; int fd = open(path, O_RDONLY);
    require(fd >= 0, "original readable");
    require(read(fd, b, sizeof(b)) == 8 && memcmp(b, "original", 8) == 0, "original contents unchanged");
    require(close(fd) == 0, "close original");
}
static void roundtrip(const char* const* paths, unsigned count, int termination) {
    int pipefd[2], status, fd;
    pid_t child;
    char ready;
    fd_set set;
    struct timeval timeout = {5, 0};
    unsigned i;
    require(pipe(pipefd) == 0, "ready pipe");
    child = fork(); require(child >= 0, "lease child");
    if (!child) {
        close(pipefd[0]);
        _exit(serve(paths, count, termination ? 10 : 1, pipefd[1]));
    }
    close(pipefd[1]); FD_ZERO(&set); FD_SET(pipefd[0], &set);
    require(select(pipefd[0] + 1, &set, NULL, NULL, &timeout) == 1, "bounded ready wait");
    require(read(pipefd[0], &ready, 1) == 1 && ready == 'R', "lease ready");
    close(pipefd[0]);
    for (i = 0; i < count; ++i) {
        fd = open(paths[i], O_RDONLY);
        require(fd == -1 && errno == EACCES, "lease denies original path");
    }
    if (termination) require(kill(child, termination) == 0, "test child termination");
    require(waitpid(child, &status, 0) == child, "lease child reaped");
    if (termination == SIGKILL) require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "crash case");
    else require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "normal lease exit");
    for (i = 0; i < count; ++i) readable(paths[i]);
}
int main(int argc, char** argv) {
    static const char* const vehicle_paths[] = {
        "/opt/mm/bin/aoap-switch", "/fs/sda0/opt/mm/bin/aoap-switch",
        "/opt/mm/bin/devc-serusb-aoap", "/fs/sda0/opt/mm/bin/devc-serusb-aoap",
        "/opt/share/bin/io-fs-media", "/fs/sda0/opt/share/bin/io-fs-media"
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && (!strcmp(argv[1], "--phone-window-90s") || !strcmp(argv[1],"--owned-window"))) {
        unsigned i; struct stat st;
        if (geteuid() != 0) { fputs("Vehicle window requires root.\n", stderr); return 2; }
        for (i = 0; i < 6; ++i) if (stat(vehicle_paths[i], &st) != 0 || !S_ISREG(st.st_mode)) return 3;
        if(!strcmp(argv[1],"--owned-window")&&getppid()<=1)return 2;
        return serve(vehicle_paths, 6, !strcmp(argv[1],"--owned-window")?0:90, -1);
    }
    if (argc == 1) {
        char dir[96], path[6][256];
        const char* paths[6];
        int fd; unsigned i;
        alarm(20);
        snprintf(dir, sizeof(dir), "/tmp/nbt-window-test-%ld", (long)getpid());
        require(mkdir(dir, 0700) == 0, "new private fixture directory");
        for (i = 0; i < 6; ++i) {
            snprintf(path[i], sizeof(path[i]), "%s/fixture%u", dir, i);
            paths[i] = path[i];
            fd = open(paths[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
            require(fd >= 0 && write(fd, "original", 8) == 8 && close(fd) == 0, "create fixture");
            readable(paths[i]);
        }
        roundtrip(paths, 6, 0); roundtrip(paths, 6, SIGTERM); roundtrip(paths, 6, SIGKILL);
        for (i = 0; i < 6; ++i) require(unlink(paths[i]) == 0, "remove owned fixture");
        require(rmdir(dir) == 0, "remove owned directory");
        puts("ACCESSORY_WINDOW_SELFTEST_OK paths=6 expiry SIGTERM SIGKILL original-preserved");
        return 0;
    }
    fputs("Usage: accessory_window [--phone-window-90s]\n", stderr);
    return 2;
}
