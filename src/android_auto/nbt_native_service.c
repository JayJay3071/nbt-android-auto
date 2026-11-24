/* native_service: the supervisor of Android Auto on the NBT (installed as NbtAndroidAutoRT/native_service).
 *
 * Started by boot.sh in /fs/ram3/nbt-aa-native (RAM). It owns only its own child processes and RAM files; the stock
 * io-nme media service and the HMI are never signalled.
 *
 * What it does, in a loop for as long as it runs:
 *   1. find exactly one phone on USB (find_phone)
 *   2. switch it to Android Open Accessory mode   (oneplus_native --switch-aa <bus> <device>)
 *   3. start the video chain for one session:
 *        fifo_http      serves annexb.fifo (H.264 from the phone) on http://127.0.0.1:18765
 *        decode_runner  plays that URL with the stock Cinemo player into the stock video window
 *        nbt_hwvideo    asks VideoApp (MOST) to show that window in front of iDrive
 *   4. run the Android Auto session                (oneplus_native --capture-video <bus> <device>)
 *   5. when the session ends, stop the chain and start over.
 * nbt_knob (iDrive controller) and accessory_window (keeps the stock USB stack away from the phone) run for the whole
 * service life.
 *
 * Behaviour switches are files in the runtime directory (written by boot.sh / the HMI plugin):
 *   hw.mode     hardware video path (the normal mode today)
 *   center800   phone canvas 800x480 1:1 in the middle of the display      split800  same, at the left edge
 *   anamorph    phone canvas 800x480 stretched 1.6x horizontally ("wide")  res720    1280x720 canvas, cropped
 *   gate.mode   only touch the phone after the HMI page was opened
 *   peer.pin    pin the phone's TLS certificate instead of checking the chain
 *   shadow.path, wal.args   old experimental modes (see experiments/), not used by the installed package
 *
 * Status for the HMI: status.txt (private) and /fs/ram3/nbt-aa-state (world readable), see report().
 * Exit status: 2 = wrong arguments, not root, runtime directory missing or service already running; otherwise 0.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNTIME_DIR "/fs/ram3/nbt-aa-native"
#define HMI_STATE_FILE "/fs/ram3/nbt-aa-state"
#define HMI_REQUEST_FILE "/fs/ram3/nbt-aa-request"
#define CINEMO_LIBRARY_PATH "/opt/mm/lib/cinemo:/opt/mm/lib:/lib:/usr/lib"
#define SYSTEM_LIBRARY_PATH "/opt/sys/lib:/lib:/usr/lib"

static volatile sig_atomic_t stopping;

static void stopped(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- child processes ------------------------------------------------------------------------------------------- */

typedef struct {
    pid_t pid; /* 0 = not running */
    int status;
} Child;

/* 1 while the child runs; reaps it (pid = 0) once it has ended */
static int alive(Child* c)
{
    int s;
    pid_t r;
    if (!c->pid)
        return 0;
    r = waitpid(c->pid, &s, WNOHANG);
    if (r == c->pid) {
        c->pid = 0;
        c->status = s;
        return 0;
    }
    return r == 0;
}

/* stop a child and its whole process group: SIGTERM, up to 5 s grace, then SIGKILL */
static void end(Child* c)
{
    long long until;
    if (!alive(c))
        return;
    kill(-c->pid, SIGTERM);
    until = ms() + 5000;
    while (alive(c) && ms() < until)
        usleep(20000);
    if (alive(c)) {
        kill(-c->pid, SIGKILL);
        while (waitpid(c->pid, &c->status, 0) < 0 && errno == EINTR) {
        }
        c->pid = 0;
    }
}

/* start args[0] in its own process group, stdin from input (or /dev/null), stdout+stderr to log (or /dev/null) */
static Child spawn(char* const args[], const char* input, const char* log)
{
    Child c = {0, 0};
    c.pid = fork();
    if (!c.pid) {
        int fd;
        if (setpgid(0, 0))
            _exit(125);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        fd = open(input ? input : "/dev/null", O_RDONLY);
        if (fd < 0)
            _exit(126);
        dup2(fd, 0);
        if (fd > 2)
            close(fd);
        fd = open(log ? log : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            _exit(126);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        execv(args[0], args);
        _exit(127);
    }
    if (c.pid < 0)
        c.pid = 0;
    else
        setpgid(c.pid, c.pid);
    return c;
}

/* ---- status for the HMI page ------------------------------------------------------------------------------------ */

/* The private status.txt plus a world-readable copy /fs/ram3/nbt-aa-state ("state=..", "phone=0|1") for the HMI plugin
 * (aa_entry_plugin). Rewritten at least every 2 s while the service runs, so a stale file means "service down".
 * States: waiting_for_phone, several_devices, waiting_for_hmi, connecting, unsupported_device, replug_required,
 *         starting_decoder, running, draining, stopped. */
static int g_phone_present;
static long long g_last_report;
static char g_last_state[32];

static void report(const char* state, unsigned session)
{
    FILE* f = fopen("status.tmp", "w");
    if (f) {
        fprintf(f, "pid=%d\nstate=%s\nsession=%u\nphone=%d\n", (int)getpid(), state, session, g_phone_present);
        fclose(f);
        rename("status.tmp", "status.txt");
    }
    f = fopen(HMI_STATE_FILE ".tmp", "w");
    if (f) {
        fprintf(f, "state=%s\nphone=%d\nsession=%u\n", state, g_phone_present, session);
        fclose(f);
        chmod(HMI_STATE_FILE ".tmp", 0644);
        rename(HMI_STATE_FILE ".tmp", HMI_STATE_FILE);
    }
    snprintf(g_last_state, sizeof(g_last_state), "%s", state);
    g_last_report = ms();
}

static void keep_reported(unsigned session)
{
    if (ms() - g_last_report >= 2000)
        report(g_last_state, session);
}

/* ---- phone detection --------------------------------------------------------------------------------------------- */

/* Generic phone detection (any Android phone, no list): every USB device except
 *   - the head unit's own devices: PEIKER telematics 2239:f004 (vendor 0x2239) is always on bus 0 address 1
 *   - Apple (0x05ac, no Android Auto), hubs (class 0x09), communication/HID/audio/mass-storage/video device classes
 *   - devices that already answered the AOA probe with "not supported" (ignored until they are unplugged)
 * is a candidate. Accessory mode is 18d1:2d00, or 18d1:2d01 with USB debugging on. Exactly one candidate is required. */
#define USB_VENDOR_PEIKER 0x2239
#define USB_VENDOR_APPLE 0x05ac
#define USB_VENDOR_GOOGLE 0x18d1
#define AOA_PRODUCT_ACCESSORY 0x2d00
#define AOA_PRODUCT_ACCESSORY_ADB 0x2d01
#define PHONE_NONE 0
#define PHONE_NORMAL 1    /* a candidate phone in its normal USB mode: has to be switched to accessory mode */
#define PHONE_ACCESSORY 2 /* already in Android Open Accessory mode: ready for the session */

typedef struct {
    int bus, addr;
    unsigned v, p; /* USB vendor and product id */
} UsbDev;

static UsbDev g_ignored[8];
static int g_ignored_n;

static int ignored(const UsbDev* u)
{
    int i;
    for (i = 0; i < g_ignored_n; ++i)
        if (g_ignored[i].bus == u->bus && g_ignored[i].addr == u->addr && g_ignored[i].v == u->v &&
            g_ignored[i].p == u->p)
            return 1;
    return 0;
}

static void ignore_dev(int bus, int addr, unsigned v, unsigned p)
{
    if (g_ignored_n < 8) {
        UsbDev u = {bus, addr, v, p};
        g_ignored[g_ignored_n++] = u;
    }
}

static unsigned found_v, found_p; /* vendor/product of the last normal-mode candidate (for NBT_PHONE_ID) */
static unsigned found_aoa;        /* product id of the last accessory-mode device (0x2d00 or 0x2d01) */

/* Parses the output of the QNX "usb" tool. Returns PHONE_NORMAL / PHONE_ACCESSORY with *bus / *device set when exactly
 * one candidate is present, else PHONE_NONE. Also updates g_phone_present (any candidate seen). */
static int find_phone(int* bus, int* device)
{
    FILE* f = popen("LD_LIBRARY_PATH=/lib:/usr/lib:/opt/sys/lib usb", "r");
    char line[256];
    int b = -1, result = 0, matches = 0, n = 0, i, j;
    UsbDev dev[16];
    unsigned cls[16];
    int have_cls[16];
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        int d;
        unsigned x;
        if (sscanf(line, "USB %d", &b) == 1)
            continue;
        if (sscanf(line, "Device Address : %d", &d) == 1) {
            if (n < 16 && b >= 0 && d > 0) {
                dev[n].bus = b;
                dev[n].addr = d;
                dev[n].v = dev[n].p = 0;
                cls[n] = 0;
                have_cls[n] = 0;
                ++n;
            }
            continue;
        }
        if (!n)
            continue;
        if (sscanf(line, "Vendor : 0x%x", &x) == 1)
            dev[n - 1].v = x;
        else if (sscanf(line, "Product : 0x%x", &x) == 1)
            dev[n - 1].p = x;
        else if (!have_cls[n - 1] && sscanf(line, "Class : 0x%x", &x) == 1) {
            cls[n - 1] = x; /* the first Class line is the device class */
            have_cls[n - 1] = 1;
        }
    }
    pclose(f);

    /* forget ignored devices that are gone */
    for (i = 0; i < g_ignored_n;) {
        int seen = 0;
        for (j = 0; j < n; ++j)
            if (dev[j].bus == g_ignored[i].bus && dev[j].addr == g_ignored[i].addr && dev[j].v == g_ignored[i].v &&
                dev[j].p == g_ignored[i].p)
                seen = 1;
        if (seen)
            ++i;
        else
            g_ignored[i] = g_ignored[--g_ignored_n];
    }

    g_phone_present = 0;
    for (i = 0; i < n; ++i) {
        UsbDev* u = &dev[i];
        int type = PHONE_NONE;
        unsigned c = cls[i];
        if (!u->v || !u->p || u->v == USB_VENDOR_PEIKER || u->v == USB_VENDOR_APPLE)
            continue;
        /* audio, communication, HID, mass storage, hub, video */
        if (c == 0x01 || c == 0x02 || c == 0x03 || c == 0x08 || c == 0x09 || c == 0x0e)
            continue;
        if (u->v == USB_VENDOR_GOOGLE && (u->p == AOA_PRODUCT_ACCESSORY || u->p == AOA_PRODUCT_ACCESSORY_ADB))
            type = PHONE_ACCESSORY;
        else if (!ignored(u))
            type = PHONE_NORMAL;
        if (!type)
            continue;
        g_phone_present = 1;
        ++matches;
        *bus = u->bus;
        *device = u->addr;
        result = type;
        if (type == PHONE_NORMAL) {
            found_v = u->v;
            found_p = u->p;
        } else
            found_aoa = u->p;
    }
    return matches == 1 ? result : 0;
}

/* ---- small helpers ------------------------------------------------------------------------------------------------ */

/* 1 if the file contains token on one of its lines */
static int log_has(const char* path, const char* token)
{
    FILE* f = fopen(path, "r");
    char l[256];
    int hit = 0;
    if (!f)
        return 0;
    while (!hit && fgets(l, sizeof(l), f))
        hit = strstr(l, token) != 0;
    fclose(f);
    return hit;
}

/* read and discard everything currently in a non-blocking descriptor */
static void drain(int fd)
{
    char b[65536];
    while (read(fd, b, sizeof(b)) > 0) {
    }
}

/* split a line into at most max-1 space separated tokens in place (for the optional wal.args file) */
static int split(char* s, char** v, int max)
{
    int n = 0;
    char* p = s;
    while (*p && n < max - 1) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        v[n++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = 0;
    }
    v[n] = 0;
    return n;
}

/* center800: nbt_hwvideo hides the iDrive HMI window while Android Auto is in front; make sure it is visible again after
 * nbt_hwvideo ended (also after a crash or SIGKILL of it) */
static void hmi_visible(void)
{
    int r = system("LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_hwvideo --hmi-visible >/dev/null 2>&1");
    (void)r;
}

/* ---- session history ------------------------------------------------------------------------------------------------- */

/* Appends why a session (or its decoder) ended to a file that survives restarts and reboots: the tail of the session log
 * without the per-frame lines, the decoder statistics and what the stock media service logged about our stream.
 * Car 2026-11-11: the reason for a drop-out was lost again, the snapshot of the restart button only keeps the last
 * three session logs and 400 system log lines. The installer's "save diagnostic logs" fetches the file. */
#define HISTORY_FILE "/var/opt/hmi/data/rw/NbtAndroidAutoRT/sessions.log"

static void history(const char* what, unsigned session, const char* decoder_log, long seconds)
{
    char cmd[900];
    struct stat st;
    int r;
    if (!stat(HISTORY_FILE, &st) && st.st_size > 300000)
        rename(HISTORY_FILE, HISTORY_FILE ".1");
    snprintf(cmd, sizeof(cmd),
             "{ echo \"=== `date` %s session %u after %ld s\";"
             " grep -v -e 'Frame RX' -e ' Data: ' -e 'Streaming Media' -e '^.NAV' capture-%u.log | tail -25;"
             " echo '--- decoder'; tail -12 %s;"
             " echo '--- media service'; sloginfo | grep -e io-nme -e a2i | grep -v -e AVRCP -e 'no PTS' -e 'NmeGraph video' | tail -40; }"
             " >> " HISTORY_FILE " 2>&1",
             what, session, seconds, session, decoder_log);
    r = system(cmd);
    (void)r;
}

/* ---- decoder restart inside a running session ---------------------------------------------------------------------- */

/* Hardware video mode only. The stock player gives up when its input stalls ("stream inactive"), and fifo_http ends when
 * the player does not read for 10 s (2 s until 2026-11-12) or does not come back after closing. Until 2026-11-11 either one ended the whole Android Auto session; the phone then
 * refused to reconnect until it was replugged ("connects, then drops again at once"). Now only the video chain is
 * started again; the session (oneplus_native) notices that nobody reads the FIFO and asks the phone for a new key frame.
 * viewport: 0 = none, 1 = centred, 2 = left edge (iDrive stays visible). Returns 1 when the new player is connected. */
static int restart_video_chain(Child* http, Child* decoder, Child* hwv, const char* decoder_mode, int viewport,
                               const char* decoder_log, const char* hwvideo_log)
{
    long long limit;
    int r;
    end(hwv);
    if (viewport)
        hmi_visible();
    end(http);
    limit = ms() + 3000;
    while (alive(decoder) && ms() < limit)
        usleep(20000);
    end(decoder);
    r = system("slay -f -Q -s TERM NmeCmdLine >/dev/null 2>&1");
    usleep(300000);
    r = system("slay -f -Q -s KILL NmeCmdLine >/dev/null 2>&1");
    (void)r;
    unlink("http.ready");
    /* Throw away what is left in the FIFO: the cut-off end of the last picture. Car 2026-11-11: the restarted player got
     * that first, then showed nothing at all ("discard surface with no PTS", 0 frames in 47 s). */
    {
        int fd = open("annexb.fifo", O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            drain(fd);
            close(fd);
        }
    }
    {
        char* args[] = {"./fifo_http", "annexb.fifo", "http.ready", 0};
        *http = spawn(args, 0, 0);
    }
    {
        char* args[] = {"./decode_runner", "0", RUNTIME_DIR "/NmeCmdLine", "http://127.0.0.1:18765/live.h264",
                        (char*)decoder_mode, 0};
        *decoder = spawn(args, 0, decoder_log);
    }
    limit = ms() + 10000;
    while (!stopping && access("http.ready", F_OK) && alive(http) && alive(decoder) && ms() < limit)
        usleep(100000);
    if (stopping || access("http.ready", F_OK) || !alive(http) || !alive(decoder))
        return 0;
    {
        char* args[] = {"./nbt_hwvideo", "--rows", "480", viewport ? "--viewport" : 0,
                        viewport == 2 ? "0,0,800,480" : "240,0,800,480", viewport == 2 ? "--keep-hmi" : 0, 0};
        setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
        *hwv = spawn(args, 0, hwvideo_log);
        setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    }
    return 1;
}

/* ---- main ------------------------------------------------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    Child lease = {0, 0};   /* accessory_window: keeps the stock USB stack from claiming the phone */
    Child http = {0, 0};    /* fifo_http (or nbt_live_file in the old shadow mode) */
    Child decoder = {0, 0}; /* decode_runner -> NmeCmdLine */
    Child frames = {0, 0};  /* old modes only: native_frames (PNG) or nbt_wal_present */
    Child capture = {0, 0}; /* oneplus_native --capture-video: the Android Auto session */
    Child knob = {0, 0};    /* nbt_knob: iDrive controller -> input.fifo */
    Child hwv = {0, 0};     /* nbt_hwvideo: shows the video window in front of iDrive */
    Child sound = {0, 0};   /* nbt_a2i_play: Android Auto media sound -> media input of the amplifier router */
    (void)argv;
    int guard = -1, lock = -1, bus, device, type, attempts = 0;
    int live = 0, wal = 0, gate = 0, hw = 0, res720 = 0, center = 0, ana = 0, right = 0; /* mode switches */
    unsigned session = 0;
    char bus_text[16], device_text[16], shadow[200], walargs[160], *wargv[16];

    if (argc != 1 || geteuid() != 0 || chdir(RUNTIME_DIR))
        return 2;
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGPIPE, SIG_IGN);
    /* one instance only */
    lock = open("service.lock", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (lock < 0)
        return 2;
    close(lock);
    if (mkfifo("annexb.fifo", 0600) || mkfifo("raw.fifo", 0600))
        goto done;
    /* keep a reader on raw.fifo open so a writer (shared Cinemo service in the old PNG mode) never gets SIGPIPE */
    guard = open("raw.fifo", O_RDONLY | O_NONBLOCK);
    if (guard < 0)
        goto done;
    fcntl(guard, F_SETFD, FD_CLOEXEC);

    /* ---- environment for oneplus_native and the NBT-AAD core (read by the children) ---- */
    setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    setenv("NBT_AA_TLS_CERT", RUNTIME_DIR "/cert.pem", 1);
    setenv("NBT_AA_TLS_KEY", RUNTIME_DIR "/key.pem", 1);
    setenv("NBT_AA_TLS_CA", RUNTIME_DIR "/ca.pem", 1);
    /* Any phone: the phone's certificate must chain to ca.pem = Google Automotive Link root (valid until 2044), the root
     * behind every Android Auto phone; validity and key usage are still checked. No per-app leaf pin any more (the OnePlus
     * leaf sha256:893145f5... would break with another phone or an Android Auto app update). peer.pin restores a pin. */
    setenv("NBT_AA_TLS_PEER", "chain", 1);
    {
        FILE* f = fopen("peer.pin", "r");
        char pin[80];
        if (f) {
            if (fgets(pin, sizeof(pin), f)) {
                size_t n = strlen(pin);
                while (n && (pin[n - 1] == '\n' || pin[n - 1] == '\r'))
                    pin[--n] = 0;
                if (n == 71 && !strncmp(pin, "sha256:", 7)) /* "sha256:" + 64 hex digits */
                    setenv("NBT_AA_TLS_PEER", pin, 1);
            }
            fclose(f);
        }
    }
    setenv("NBT_AA_CAPTURE_PREFIX", RUNTIME_DIR "/session", 1);
    setenv("NBT_AA_LIVE_FIFO", RUNTIME_DIR "/annexb.fifo", 1);
    /* identity the head unit reports to the phone in the service discovery response */
    setenv("NBT_SDR_IDENTITY", "1", 1);
    setenv("NBT_CAR_MAKE", "BMW", 1);
    setenv("NBT_CAR_MODEL", "F07", 1);
    /* Phone canvas 800x480 (NBT_VID_RES=1 in capture). The reference default insets 0,240,0,0 are meant for a 1280x720 canvas
     * and left the phone only the top 240 rows (vehicle 2026-11-06: picture in the upper half only). 0,180,0,0 leaves
     * 800x300 = the 8:3 shape of the 1280x480 display, stretched 1.6x both ways. Not overwritten when the caller sets it. */
    setenv("NBT_VID_INSETS", "0,180,0,0", 0);
    /* pad each H.264 access unit to 8 KB with a filler NAL so the decoder's byte buffers fill quickly (input lag) */
    setenv("NBT_AA_PAD", "8192", 0);
    /* access unit delimiter behind every picture (input lag, see aa_video_capture.cpp); off with the file aud.off */
    if (!access("/var/opt/hmi/data/rw/NbtAndroidAutoRT/aud.off", F_OK))
        setenv("NBT_AA_AUD", "0", 1);
    /* SPS frame rate stays off (car 2026-11-12: ~10 s delay); sps.off kept for the binaries that had it on */
    if (!access("/var/opt/hmi/data/rw/NbtAndroidAutoRT/sps.off", F_OK))
        setenv("NBT_AA_SPS_FPS", "0", 1);
    /* Video frame rate limit (aa_session.cpp, paced video acks): the stock player shows about 25 pictures/s, the
     * phone sends 30 while the map moves and the player fell behind until it was restarted (every ~65 s, car
     * 2026-11-12). Default 20/s; the file video.fps holds another number (0 = no limit). */
    {
        char fps[16] = "20";
        FILE* f = fopen("/var/opt/hmi/data/rw/NbtAndroidAutoRT/video.fps", "r");
        if (f) {
            if (!fgets(fps, sizeof(fps), f))
                strcpy(fps, "20");
            fclose(f);
            fps[strcspn(fps, "\r\n ")] = 0;
        }
        setenv("NBT_AA_VIDEO_FPS", fps, 1);
    }
    /* Flow control (2026-11-12): a picture is acked to the phone only after it went to the player (aa_video_capture.cpp,
     * AASession::setVideoAckGate), and fifo_http waits 10 s for a player that does not read instead of 2 s. Off with
     * the file video.gate.off. Optional test values: video.pad (bytes each picture is padded to, default 8192) and
     * http.stall (milliseconds fifo_http waits). */
    setenv("NBT_AA_VIDEO_GATE", access("/var/opt/hmi/data/rw/NbtAndroidAutoRT/video.gate.off", F_OK) ? "1" : "0", 1);
    {
        static const char* const files[2][2] = {{"/var/opt/hmi/data/rw/NbtAndroidAutoRT/video.pad", "NBT_AA_PAD"},
                                                {"/var/opt/hmi/data/rw/NbtAndroidAutoRT/http.stall", "NBT_HTTP_STALL_MS"}};
        int i;
        for (i = 0; i < 2; ++i) {
            char value[16];
            FILE* f = fopen(files[i][0], "r");
            if (!f)
                continue;
            if (fgets(value, sizeof(value), f)) {
                value[strcspn(value, "\r\n ")] = 0;
                if (value[0] >= '0' && value[0] <= '9')
                    setenv(files[i][1], value, 1);
            }
            fclose(f);
        }
    }
    /* Media keys (skip from the steering wheel): declared to the phone unless the file mediakeys.off exists */
    if (access("/var/opt/hmi/data/rw/NbtAndroidAutoRT/mediakeys.off", F_OK))
        setenv("NBT_SDR_MEDIA_KEYS", "1", 1);
    setenv("NBT_CAR_YEAR", "2014", 1);
    setenv("NBT_HU_MODEL", "HU_NBT", 1);
    setenv("NBT_SDR_DROP", "2,4,9", 1); /* services not offered to the phone (see NBT-AAD sdr_builder) */
    setenv("NBT_RX_TIMEOUT_S", "5", 1);

    /* ---- mode switches (files in the runtime directory) ---- */
    /* Old experimental mode: if shadow.path names a file under /fs/usb0/, nbt_live_file shadows that file with the live
     * H.264 stream so the STOCK USB video player plays it (experiments/usb-file-overlay). In that mode fifo_http,
     * native_frames and decode_runner are not started. Without shadow.path nothing changes. */
    shadow[0] = 0;
    {
        FILE* f = fopen("shadow.path", "r");
        if (f) {
            if (fgets(shadow, sizeof(shadow), f)) {
                size_t n = strlen(shadow);
                while (n && (shadow[n - 1] == '\n' || shadow[n - 1] == '\r' || shadow[n - 1] == ' '))
                    shadow[--n] = 0;
                if ((strncmp(shadow, "/fs/usb0/", 9) && strncmp(shadow, "/fs/pfs0/", 9)) || n <= 9)
                    shadow[0] = 0;
            }
            fclose(f);
        }
    }
    live = shadow[0] != 0;
    /* Old experimental mode: if wal.args exists, nbt_wal_present (decoded picture as a WAL sprite in front of the HMI,
     * arguments from that file, may be empty) replaces native_frames (experiments/wal-overlay; crashed the car). */
    walargs[0] = 0;
    {
        FILE* f = fopen("wal.args", "r");
        if (f) {
            wal = 1;
            if (fgets(walargs, sizeof(walargs), f)) {
                size_t n = strlen(walargs);
                while (n && (walargs[n - 1] == '\n' || walargs[n - 1] == '\r'))
                    walargs[--n] = 0;
            }
            fclose(f);
        }
    }
    if (wal) {
        wargv[0] = "./nbt_wal_present";
        split(walargs, wargv + 1, 15);
    }
    gate = access("gate.mode", F_OK) == 0;
    /* Hardware video mode (hw.mode, car-proven path 2026-11-07): the decoder renders into the stock video window
     * (decode_runner "screen"), nbt_hwvideo asks VideoApp for the video connection and shows it in front of iDrive.
     * No YUV FIFO, no RGBA conversion, no WAL sprites. Takes precedence over wal.args. */
    hw = access("hw.mode", F_OK) == 0 && access("nbt_hwvideo", X_OK) == 0;
    /* res720 (hardware mode only): the phone sends 1280x720 and draws into rows 0..479 (1280x480, the display shape);
     * the decoder crops these rows. Sharper vertically than 800x300; the capture stays 800x480. */
    res720 = hw && access("res720", F_OK) == 0;
    if (res720) {
        setenv("NBT_VID_RES", "2", 1);
        setenv("NBT_VID_INSETS", "0,240,0,0", 1);
    }
    /* center800 (hardware mode only, wins over res720; user request 2026-11-08): the phone draws the whole 800x480 canvas,
     * shown 1:1 in the middle of the 1280x480 display (black bars of 240 px left and right). No scaling at all: as sharp as
     * the video path allows (the capture is 800x480 anyway), instead of 800x300 stretched 1.6x both ways. */
    center = hw && access("center800", F_OK) == 0;
    /* split800 (user idea 2026-11-08): same 1:1 picture at the LEFT edge (0..799); iDrive stays visible on the right
     * 480 px, exactly where its own split screen (480x480 windows) sits. This is the "16:9" mode of the HMI page. */
    right = hw && !center && access("split800", F_OK) == 0;
    if (right)
        center = 1;
    if (center) {
        res720 = 0;
        setenv("NBT_VID_RES", "1", 1);
        setenv("NBT_VID_INSETS", "0,0,0,0", 1);
    }
    /* anamorph (hardware mode only, user idea 2026-11-08): the phone is told that the pixels are 1.6x wider than tall
     * (VideoConfig pixel_aspect_ratio_e4 = 16000) and draws its whole 800x480 canvas squeezed horizontally; the GPU
     * stretches 800 -> 1280 only horizontally, so the picture fills the display with 480 instead of 300 rows.
     * This is the "wide" mode of the HMI page. */
    ana = hw && !center && access("anamorph", F_OK) == 0;
    if (ana) {
        res720 = 0;
        setenv("NBT_VID_RES", "1", 1);
        setenv("NBT_VID_INSETS", "0,0,0,0", 1);
        setenv("NBT_VID_PAR", "16000", 0);
    }

    /* ---- processes that run for the whole service life ---- */
    if (live) {
        char* args[] = {"./nbt_live_file", "--path", shadow, "--fifo", "annexb.fifo", "--ready", "http.ready",
                        "--seconds", "0", 0};
        unlink("http.ready");
        http = spawn(args, 0, "live_file.log");
        {
            long long limit = ms() + 5000;
            while (!stopping && access("http.ready", F_OK) && alive(&http) && ms() < limit)
                usleep(100000);
        }
        if (access("http.ready", F_OK) || !alive(&http))
            goto done;
    }
    {
        char* args[] = {"./accessory_window", "--owned-window", 0};
        lease = spawn(args, 0, "window.log");
    }
    usleep(500000);
    if (!alive(&lease))
        goto done;
    /* Optional iDrive controller (nbt_knob present): observe-only MOST client, writes key lines into input.fifo, which the
     * live AA session (oneplus_native --capture-video) reads. Runs for the whole service life, independent of sessions.
     * nbt_knob needs /opt/sys/lib (libsys_dsi_colibry.so.3 and its dependencies; car 2026-11-07: "Library cannot be found"
     * with the Cinemo library path), so it gets its own library path for the fork. */
    if (!access("nbt_knob", X_OK) && (!mkfifo("input.fifo", 0600) || errno == EEXIST)) {
        char* args[] = {"./nbt_knob", 0};
        setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
        knob = spawn(args, 0, "knob.log");
        setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    }
    /* Optional media sound over USB (nbt_a2i_play present, 2026-11-11): the media audio sink (9) is offered to the phone,
     * the session writes its 48 kHz PCM into audio.fifo and nbt_a2i_play plays it through the head unit's media input.
     * Without the program the phone keeps playing over Bluetooth as before. */
    if (!access("nbt_a2i_play", X_OK) && (!mkfifo("audio.fifo", 0600) || errno == EEXIST)) {
        /* optional sound.args: "<endpoint> <channels>" instead of the program's default */
        static char sargs[80];
        char* sv[4];
        char* args[] = {"./nbt_a2i_play", "audio.fifo", 0, 0, 0};
        FILE* f = fopen("sound.args", "r");
        if (f) {
            if (fgets(sargs, sizeof(sargs), f)) {
                size_t n = strlen(sargs);
                while (n && (sargs[n - 1] == '\n' || sargs[n - 1] == '\r'))
                    sargs[--n] = 0;
                if (split(sargs, sv, 3) == 2) {
                    args[2] = sv[0];
                    args[3] = sv[1];
                }
            }
            fclose(f);
        }
        setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
        sound = spawn(args, 0, "sound.log");
        setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
        setenv("NBT_AA_AUDIO_FIFO", RUNTIME_DIR "/audio.fifo", 1);
        setenv("NBT_SDR_DROP", "2,4", 1);
    }

    /* ---- one Android Auto session per loop pass ---- */
    while (!stopping && alive(&lease)) {
        /* Optional HMI gate (file gate.mode in the runtime directory): the phone is only touched after the Android Auto
         * page in the HMI was opened, which creates /fs/ram3/nbt-aa-request (see src/idrive/aa_entry_plugin.cpp). */
        if (gate && access(HMI_REQUEST_FILE, F_OK)) {
            find_phone(&bus, &device);
            report("waiting_for_hmi", session);
            sleep(1);
            continue;
        }
        type = find_phone(&bus, &device);
        if (!type) {
            attempts = 0;
            report(g_phone_present ? "several_devices" : "waiting_for_phone", session);
            sleep(1);
            continue;
        }
        if (attempts >= 3) {
            report("replug_required", session);
            sleep(2);
            continue;
        }

        /* step 2: switch a normal-mode phone to accessory mode; it then reappears as 18d1:2d00/2d01 */
        if (type == PHONE_NORMAL) {
            Child sw;
            char* args[] = {"./oneplus_native", "--switch-aa", bus_text, device_text, 0};
            char id[16];
            report("connecting", session);
            snprintf(id, sizeof(id), "%04x:%04x", found_v, found_p);
            setenv("NBT_PHONE_ID", id, 1);
            snprintf(bus_text, sizeof(bus_text), "%d", bus);
            snprintf(device_text, sizeof(device_text), "%d", device);
            sw = spawn(args, 0, "switch.log");
            while (!stopping && alive(&sw))
                usleep(100000);
            end(&sw);
            if (stopping)
                break;
            /* no AOA answer at all: not an Android phone (USB stick, other device): leave it alone until it is unplugged */
            if (!log_has("switch.log", "AOA_PROTOCOL_OK")) {
                ignore_dev(bus, device, found_v, found_p);
                report("unsupported_device", session);
                sleep(1);
                continue;
            }
            sleep(1);
            if (find_phone(&bus, &device) != PHONE_ACCESSORY) {
                ++attempts;
                continue;
            }
        }

        /* step 3: the video chain for this session */
        report("connecting", session);
        ++attempts;
        ++session;
        report("starting_decoder", session);
        drain(guard);
        /* no leftover client of the shared io-nme from an earlier, abruptly ended session (only our own NmeCmdLine copy
         * exists: the stock player works through mme/io-media-cinemo, never NmeCmdLine) */
        if (hw) {
            int r = system("slay -f -Q -s TERM NmeCmdLine >/dev/null 2>&1");
            (void)r;
            usleep(500000);
            r = system("slay -f -Q -s KILL NmeCmdLine >/dev/null 2>&1");
            (void)r;
        }
        if (found_aoa == AOA_PRODUCT_ACCESSORY_ADB)
            setenv("NBT_AOA_PRODUCT", "2d01", 1);
        else
            unsetenv("NBT_AOA_PRODUCT");
        if (!live) {
            unlink("http.ready");
            {
                char* args[] = {"./fifo_http", "annexb.fifo", "http.ready", 0};
                http = spawn(args, 0, 0);
            }
            if (hw) {
                /* hardware mode: nothing between decoder and screen */
            } else if (wal)
                frames = spawn(wargv, "raw.fifo", "present.log");
            else {
                char* args[] = {"./native_frames", "--seconds", "0", "--fps", "3", 0};
                frames = spawn(args, "raw.fifo", "frames.log");
            }
            {
                char* args[] = {"./decode_runner", "0", RUNTIME_DIR "/NmeCmdLine", "http://127.0.0.1:18765/live.h264",
                                hw ? ((center || ana) ? "screencenter" : res720 ? "screen720" : "screen")
                                   : RUNTIME_DIR "/raw.fifo",
                                0};
                char decname[40];
                snprintf(decname, sizeof(decname), "decoder-%u.log", session);
                decoder = spawn(args, 0, decname);
            }
            /* wait up to 10 s until the player is connected to fifo_http */
            {
                long long limit = ms() + 10000;
                while (!stopping && access("http.ready", F_OK) && alive(&http) && alive(&decoder) && ms() < limit)
                    usleep(100000);
            }
            /* the decoder already crops to the content rows (-vr), so the window is the plain 800x480 output;
             * viewport x,y,w,h on the 1280x480 display: centred (240,0) or left edge (0,0, iDrive stays visible) */
            if (hw && !stopping) {
                char* args[] = {"./nbt_hwvideo", "--rows", "480", center ? "--viewport" : 0,
                                right ? "0,0,800,480" : "240,0,800,480", right ? "--keep-hmi" : 0, 0};
                char hwname[40];
                snprintf(hwname, sizeof(hwname), "hwvideo-%u.log", session);
                setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
                hwv = spawn(args, 0, hwname);
                setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
            }
        }

        /* step 4: the Android Auto session itself (old live mode: nbt_live_file, kept in 'http', runs all the time) */
        if (!stopping && (live ? alive(&http) : (!access("http.ready", F_OK) && alive(&decoder) && alive(&http)))) {
            char* args[] = {"./oneplus_native", "--capture-video", bus_text, device_text, 0};
            snprintf(bus_text, sizeof(bus_text), "%d", bus);
            snprintf(device_text, sizeof(device_text), "%d", device);
            {
                char capname[40];
                snprintf(capname, sizeof(capname), "capture-%u.log", session);
                capture = spawn(args, 0, capname);
            }
            report("running", session);
            {
                long long began = ms(), chain_since = ms();
                int chain_restarts = 0;
                char decname[48], hwname[48];
                snprintf(decname, sizeof(decname), "decoder-%u.log", session);
                while (!stopping && alive(&capture)) {
                    const int chain_ok = alive(&http) && (live || (alive(&decoder) && (hw || alive(&frames))));
                    if (!chain_ok) {
                        /* a chain that ran for 30 s was a working one: start counting again. Only a decoder that dies
                         * five times in a row within 30 s each ends the session. Car 2026-11-12: with 120 s the
                         * restarts every ~65 s during navigation (player falling behind) were never reset, and the
                         * fifth one ended a 2-hour session (16:52) and a 20-minute one (17:13); the phone then needed
                         * replugging. */
                        if (ms() - chain_since >= 30000)
                            chain_restarts = 0;
                        if (!hw || live || chain_restarts >= 5)
                            break;
                        ++chain_restarts;
                        history("decoder ended in", session, decname, (long)((ms() - began) / 1000));
                        report("starting_decoder", session);
                        snprintf(decname, sizeof(decname), "decoder-%u-r%d.log", session, chain_restarts);
                        snprintf(hwname, sizeof(hwname), "hwvideo-%u-r%d.log", session, chain_restarts);
                        if (!restart_video_chain(&http, &decoder, &hwv,
                                                 (center || ana) ? "screencenter" : res720 ? "screen720" : "screen",
                                                 !center ? 0 : right ? 2 : 1, decname, hwname))
                            break;
                        /* the new player is connected: the session starts its stream again with config + key frame */
                        kill(capture.pid, SIGUSR1);
                        chain_since = ms();
                        report("running", session);
                    }
                    /* the player opened a new connection to fifo_http (its own reconnect after 5 s without data): the
                     * stream has to start again with config + key frame, as after a chain restart */
                    if (!access("http.resync", F_OK)) {
                        unlink("http.resync");
                        kill(capture.pid, SIGUSR1);
                    }
                    usleep(200000);
                    keep_reported(session);
                }
                history("end of", session, decname, (long)((ms() - began) / 1000));
                /* a session that ran for a minute was a good one: its end is not a failed attempt (review 2026-11-09:
                 * the counter was only reset by unplugging, so the third normal session end of a drive showed
                 * "replug the phone") */
                if (ms() - began >= 60000)
                    attempts = 0;
            }
        }

        /* step 5: tear the chain down (video window first, so iDrive is visible again) */
        report("draining", session);
        end(&hwv);
        if (center)
            hmi_visible();
        end(&capture);
        if (!live)
            end(&http);
        {
            long long limit = ms() + 3000;
            while (alive(&decoder) && ms() < limit)
                usleep(20000);
        }
        end(&decoder);
        {
            long long limit = ms() + 2000;
            while (alive(&frames) && ms() < limit)
                usleep(20000);
        }
        end(&frames);
        drain(guard);
        if (!live)
            unlink("http.ready");
        /* Clear only the FIFO bytes left by this completed session (live mode: nbt_live_file owns the FIFO reader). */
        if (!live) {
            int fd = open("annexb.fifo", O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                drain(fd);
                close(fd);
            }
        }
        if (!stopping)
            sleep(2);
    }

done:
    end(&hwv);
    if (center)
        hmi_visible();
    end(&capture);
    end(&http);
    end(&decoder);
    end(&frames);
    end(&knob);
    end(&sound);
    end(&lease);
    unlink("input.fifo");
    unlink("audio.fifo");
    if (guard >= 0) {
        drain(guard);
        close(guard);
    }
    unlink("annexb.fifo");
    unlink("raw.fifo");
    unlink("http.ready");
    unlink("service.lock");
    report("stopped", session);
    unlink(HMI_STATE_FILE);
    return 0;
}
