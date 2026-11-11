/* decode_runner: starts the stock Cinemo command line player (NmeCmdLine) as our own child and keeps it under control.
 *
 * NmeCmdLine is only a client of the shared, stock io-nme media service; this program never starts, stops or changes
 * io-nme itself. It forks, runs NmeCmdLine in its own process group, waits for it and stops it again (SIGTERM, then
 * SIGKILL after 4 s) when asked to or when the time limit is reached.
 *
 * Usage: decode_runner <seconds> <path to NmeCmdLine> <input URL or file> <mode>
 *   seconds  0 = no limit, otherwise 1..3600
 *   mode     "screen"       hardware video path: draw into the stock video window of Cinemo output 1 (the window
 *                           nbt_hwvideo makes visible), phone content rows 0..299 of 480 stretched to 800x480
 *            "screen720"    same, for a 1280x720 phone canvas (content rows 0..479)
 *            "screencenter" same, whole 800x480 canvas 1:1, nbt_hwvideo centres the window
 *            anything else  no display: decode to the raw video file/FIFO given as <mode> (old PNG pipeline)
 *
 * Exit status: 124 = stopped by us, otherwise the player's exit status (128 + signal if it was killed),
 *              2 = bad arguments, 125 = process group setup failed, 127 = NmeCmdLine could not be started.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Cinemo output 1 = the stock video window classes that VideoApp/DisplayManager show (same as the stock USB player) */
#define STOCK_VIDEO_OUTPUT                                                                                          \
    "?video-output-id=1&video-output-aspect-x=1&video-output-aspect-y=1"                                            \
    "&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB"                                                     \
    "&video-subpicture-window-class=CinemoSubpicture1_TaskA,CinemoSubpicture1_TaskB"

static volatile sig_atomic_t stop;

static void stopped(int signal_number)
{
    (void)signal_number;
    stop = 1;
}

static long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char** argv)
{
    pid_t child;
    int status, stage = 0; /* stage: 0 = running, 1 = SIGTERM sent, 2 = SIGKILL sent */
    long seconds;
    char* end;
    long long deadline;

    if (argc != 5)
        return 2;
    seconds = strtol(argv[1], &end, 10);
    if (*end || seconds < 0 || seconds > 3600)
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);

    child = fork();
    if (child < 0)
        return 2;
    if (!child) {
        struct rlimit core;
        /* Software decoding on purpose (-wd 0 -wi 0 -wm 0, car test 2026-11-10): hardware decoding (-wd 1) gave a
         * picture, but the io-nme video queue grew to 0.85 MB (seconds of delay; software keeps it at 0.004 MB), and a
         * playback speed above normal (-s 1200) made NmeCmdLine stop after the first frame of the live stream. */
        memset(&core, 0, sizeof(core));
        if (setrlimit(RLIMIT_CORE, &core) || setpgid(0, 0)) /* no core files, own process group */
            _exit(125);

        /* Common options: -a0 no audio, -i -ic timing info, -vd NmeDeviceScreen -vi <output> render into the stock
         * window. -vf: no frame dropping (car 2026-11-07: the live stream has no usable timestamps, the renderer
         * dropped 992 of 1204 frames and the picture froze). -x/-y/-xx/-yy: window 800x480 at 0,0 (with only -vr the
         * window took the cropped size 800x300: lower part black, picture squashed, car 2026-11-07). -vr crops the
         * source in 1/1000 of its size, -va 3 stretches the crop to the window. */

        if (!strcmp(argv[4], "screen720"))
            /* phone canvas 1280x720 with content rows 0..479 (insets 0,240,0,0) -> crop 667/1000 */
            execl(argv[2], "NmeCmdLine", "-a0", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-vr", "0,0,1000,667", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else if (!strcmp(argv[4], "screencenter"))
            /* whole 800x480 phone canvas 1:1 (no crop); nbt_hwvideo places the window in the middle of the display */
            execl(argv[2], "NmeCmdLine", "-a0", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else if (!strcmp(argv[4], "screen"))
            /* -vr 0,0,1000,625: only the phone's content rows 0..299 of 480 (insets 0,180,0,0) */
            execl(argv[2], "NmeCmdLine", "-a0", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-vr", "0,0,1000,625", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else
            /* no display (-n): decoded frames go to the raw video output argv[4] (old PNG pipeline) */
            execl(argv[2], "NmeCmdLine", "-n", "-a0", "-wd", "0", "-wi", "0", "-wm", "0", "-i", "-ic", "-dv", argv[4],
                  argv[3], (char*)0);
        _exit(127);
    }

    if (setpgid(child, child) && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return 125;
    }
    deadline = seconds ? ms() + seconds * 1000LL : 0;
    for (;;) {
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child)
            break;
        if (r < 0 && errno != EINTR)
            return 125;
        if (stop || (deadline && ms() >= deadline)) {
            /* 4 s for a clean tear-down of the client's graph inside the shared io-nme (car 2026-11-07: after abrupt
             * session ends the next session showed no picture until a reboot; suspected leftover graph/window) */
            if (!stage) {
                kill(-child, SIGTERM);
                stage = 1;
                deadline = ms() + 4000;
                stop = 0;
            } else {
                kill(-child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
                stage = 2;
                break;
            }
        }
        usleep(10000);
    }
    printf("DECODE_CLIENT_END forced=%d status=%d\n", stage, status);
    return stage ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
