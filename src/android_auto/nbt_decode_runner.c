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
 *
 * io-nme's options are global (disassembly of io-nme 1.24.13 / NmeCmdLine, 2026-11-12):
 *   NMECTL_SET_OPTION (0x80000501) calls NmeOptions::SetOption on io-nme's one global g_options, which every playback
 *   graph reads when it is built, whichever client created it. NmeCmdLine turns its command line switches into such
 *   SET_OPTION calls: -a0 = "Audio/Enable=0", -wd/-wi/-wm = "Video/Hardware{Decode,Deinterlace,Mix}", -vd =
 *   "Plugin/VideoDevice", -n = the Null audio/video plugins, and so on. iDrive's player (io-media-cinemo) sets its
 *   options once when it starts. So our "-a0" switched the audio off for every playback iDrive built afterwards: the
 *   source that was playing kept playing, the next source switch (Bluetooth, AUX, ...) built a graph without audio, and
 *   it stayed so until io-nme was restarted (HMI reboot) - the bug seen on the car since 2026-11-12.
 * Therefore: no "-a0" any more (the phone's H.264 stream has no audio track anyway), and the options NmeCmdLine can
 * change are saved before it starts and written back when it ends (NME_OPTS below). The first save of each boot is kept
 * in nme-options.saved (boot.sh deletes it), so a decoder killed hard cannot make a changed value the "original".
 * Audio/Enable and Video/Enable are always written back as 1: iDrive never switches them off, and this also repairs an
 * io-nme that an older version of this program had already changed.
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
#include <fcntl.h>
#include <devctl.h>
#include <sys/neutrino.h>

#define NMECTL_SET_OPTION 0x80000501
#define NMECTL_GET_OPTION 0xc0000502
#define NME_OPTS_FILE "nme-options.saved" /* in the working directory (the runtime folder) */

/* every global option the NmeCmdLine switches we use (or used) can change */
static const char* const NME_OPTS[] = {
    "Audio/Enable",          "Video/Enable",           "Video/HardwareDecode", "Video/HardwareDeinterlace",
    "Video/HardwareMix",     "Video/FrameDropThreshold", "Video/VideoDisplayID", "Plugin/Audio",
    "Plugin/AudioDevice",    "Plugin/AudioRenderer",   "Plugin/Video",         "Plugin/VideoDevice",
    "Plugin/VideoRenderer",
};
#define NME_OPT_COUNT ((int)(sizeof(NME_OPTS) / sizeof(NME_OPTS[0])))
static char nme_saved[NME_OPT_COUNT][512];
static int nme_have[NME_OPT_COUNT];

/* the same call NmeCtrl() in libNme.so makes: devctlv with one send and one reply part; result = io-nme's error code */
static int nme_ctrl(int fd, unsigned dcmd, const void* in, int in_len, void* out, int out_len)
{
    iov_t sv, rv;
    int info = 0, r;
    SETIOV(&sv, (void*)in, in_len);
    SETIOV(&rv, out, out_len);
    r = devctlv(fd, (int)dcmd, in ? 1 : 0, out ? 1 : 0, &sv, &rv, &info);
    return r ? -r : info;
}

static int nme_get(int fd, const char* name, char* value, int size)
{
    memset(value, 0, size);
    if (nme_ctrl(fd, NMECTL_GET_OPTION, name, (int)strlen(name) + 1, value, size - 1))
        return -1;
    value[size - 1] = 0;
    return 0;
}

static int nme_set(int fd, const char* name, const char* value)
{
    char line[600];
    if (snprintf(line, sizeof(line), "%s=%s", name, value) >= (int)sizeof(line))
        return -1;
    return nme_ctrl(fd, NMECTL_SET_OPTION, line, (int)strlen(line) + 1, 0, 0);
}

/* the saved values: from nme-options.saved if this boot already has one, otherwise read from io-nme now and stored */
static void nme_save(void)
{
    FILE* f = fopen(NME_OPTS_FILE, "r");
    int fd, i, n = 0;
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            size_t l = strlen(line);
            if (l && line[l - 1] == '\n')
                line[l - 1] = 0;
            if (!eq)
                continue;
            *eq = 0;
            for (i = 0; i < NME_OPT_COUNT; ++i)
                if (!strcmp(line, NME_OPTS[i]) && strlen(eq + 1) < sizeof(nme_saved[i])) {
                    strcpy(nme_saved[i], eq + 1);
                    nme_have[i] = 1;
                    ++n;
                }
        }
        fclose(f);
        printf("NME_OPTIONS loaded %d from " NME_OPTS_FILE "\n", n);
        return;
    }
    fd = open("/dev/io-nme", O_RDONLY);
    if (fd < 0) {
        printf("NME_OPTIONS /dev/io-nme: %s\n", strerror(errno));
        return;
    }
    f = fopen(NME_OPTS_FILE ".tmp", "w");
    for (i = 0; i < NME_OPT_COUNT; ++i) {
        if (nme_get(fd, NME_OPTS[i], nme_saved[i], sizeof(nme_saved[i])))
            continue;
        nme_have[i] = 1;
        ++n;
        if (f)
            fprintf(f, "%s=%s\n", NME_OPTS[i], nme_saved[i]);
        printf("NME_OPTIONS saved %s=%s\n", NME_OPTS[i], nme_saved[i]);
    }
    close(fd);
    if (f && !fclose(f) && n)
        rename(NME_OPTS_FILE ".tmp", NME_OPTS_FILE);
    else
        unlink(NME_OPTS_FILE ".tmp");
}

/* writes the saved values back (only those that differ now); Audio/Enable and Video/Enable always as 1 */
static void nme_restore(const char* when)
{
    int fd = open("/dev/io-nme", O_RDONLY), i, changed = 0;
    if (fd < 0) {
        printf("NME_OPTIONS %s: /dev/io-nme: %s\n", when, strerror(errno));
        return;
    }
    for (i = 0; i < NME_OPT_COUNT; ++i) {
        const int on = !strcmp(NME_OPTS[i], "Audio/Enable") || !strcmp(NME_OPTS[i], "Video/Enable");
        const char* want = on ? "1" : nme_have[i] ? nme_saved[i] : 0;
        char now[512];
        if (!want || (!nme_get(fd, NME_OPTS[i], now, sizeof(now)) && !strcmp(now, want)))
            continue;
        printf("NME_OPTIONS %s: %s %s -> %s (%d)\n", when, NME_OPTS[i], now, want, nme_set(fd, NME_OPTS[i], want));
        ++changed;
    }
    close(fd);
    printf("NME_OPTIONS %s: %d written back\n", when, changed);
}

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
    setvbuf(stdout, NULL, _IOLBF, 0);

    nme_save();
    nme_restore("start"); /* repairs what an earlier run left (Audio/Enable=0 from the old "-a0") */
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

        /* Common options (no -a0: see the top of this file), -i -ic timing info, -vd NmeDeviceScreen -vi <output> render into the stock
         * window. -vf: no frame dropping (car 2026-11-07: the live stream has no usable timestamps, the renderer
         * dropped 992 of 1204 frames and the picture froze). -x/-y/-xx/-yy: window 800x480 at 0,0 (with only -vr the
         * window took the cropped size 800x300: lower part black, picture squashed, car 2026-11-07). -vr crops the
         * source in 1/1000 of its size, -va 3 stretches the crop to the window.
         * No -a0: see the top of this file. */

        if (!strcmp(argv[4], "screen720"))
            /* phone canvas 1280x720 with content rows 0..479 (insets 0,240,0,0) -> crop 667/1000 */
            execl(argv[2], "NmeCmdLine", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-vr", "0,0,1000,667", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else if (!strcmp(argv[4], "screencenter"))
            /* whole 800x480 phone canvas 1:1 (no crop); nbt_hwvideo places the window in the middle of the display */
            execl(argv[2], "NmeCmdLine", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else if (!strcmp(argv[4], "screen"))
            /* -vr 0,0,1000,625: only the phone's content rows 0..299 of 480 (insets 0,180,0,0) */
            execl(argv[2], "NmeCmdLine", "-wd", "0", "-wi", "0", "-wm", "0", "-vd", "NmeDeviceScreen", "-vi",
                  STOCK_VIDEO_OUTPUT,
                  "-vf", "-vr", "0,0,1000,625", "-va", "3", "-x", "800", "-y", "480", "-xx", "0", "-yy", "0",
                  "-i", "-ic", argv[3], (char*)0);
        else
            /* no display (-n): decoded frames go to the raw video output argv[4] (old PNG pipeline) */
            execl(argv[2], "NmeCmdLine", "-n", "-wd", "0", "-wi", "0", "-wm", "0", "-i", "-ic", "-dv", argv[4],
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
    nme_restore("end");
    return stage ? 124 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
