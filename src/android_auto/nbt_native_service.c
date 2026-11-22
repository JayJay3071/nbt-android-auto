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
 *   decoder.mode  with nbt_decoder present (boot.sh: always, unless decoder.off in the package): it decodes and shows
 *               the picture instead of fifo_http + decode_runner + nbt_hwvideo; decoder.60fps = ask the phone for 60 fps
 *               (boot.sh: unless decoder.30fps)
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
#define DECODER_LIBRARY_PATH "/proc/boot:/lib:/usr/lib:/opt/sys/lib"

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

/* USB reset of the phone through oneplus_native --reset (see oneplus_aoa.c); log of the last one in reset.log */
static void reset_phone(int bus, int device, int type, int n)
{
    char bus_text[16], device_text[16];
    char* args[] = {"./oneplus_native", "--reset", bus_text, device_text, 0};
    Child c;
    long long limit;
    snprintf(bus_text, sizeof(bus_text), "%d", bus);
    snprintf(device_text, sizeof(device_text), "%d", device);
    if (type == PHONE_ACCESSORY)
        setenv("NBT_RESET_AOA", "1", 1);
    else {
        char id[16];
        snprintf(id, sizeof(id), "%04x:%04x", found_v, found_p);
        setenv("NBT_PHONE_ID", id, 1);
        unsetenv("NBT_RESET_AOA");
    }
    printf("native_service: failed attempt, USB reset %d of 3 (bus %d device %d)\n", n, bus, device);
    c = spawn(args, 0, "reset.log");
    limit = ms() + 10000;
    while (alive(&c) && ms() < limit)
        usleep(100000);
    end(&c);
    unsetenv("NBT_RESET_AOA");
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
    Child ndec = {0, 0};    /* decoder mode: nbt_decoder, started once and kept across sessions */
    (void)argv;
    int guard = -1, lock = -1, bus, device, type, attempts = 0, resets = 0;
    long long absent_since = 0;
    int live = 0, wal = 0, gate = 0, hw = 0, res720 = 0, center = 0, ana = 0, right = 0, dmode = 0; /* mode switches */
    unsigned session = 0;
    char bus_text[16], device_text[16], shadow[200], walargs[160], *wargv[16], ndecargs[160], *ndargv[24];

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
    setenv("NBT_CAR_YEAR", "2014", 1);
    setenv("NBT_HU_MODEL", "HU_NBT", 1);
    /* Android Auto sound in the car (2026-11-18, aa_media_out.hpp): the media channel (9) is offered to the phone, its
     * sound goes into io-amp input input_stereo_1, which plays as main source f6 (car-tested: built without a medium,
     * kept by iDrive, no iDrive player on the steering wheel). nbt_knob treats f6 as the Android Auto audio source (play /
     * NAV in Android Auto switch to it, leaving it pauses the phone) and leaves iDrive's AUX alone. The file media-jack in
     * the runtime directory brings back the cable setup: no media channel (the phone plays on its own jack), the
     * Android Auto source is AUX (bb) and choosing AUX opens Android Auto, as before. */
    if (access("media-jack", F_OK) != 0) {
        setenv("NBT_SDR_DROP", "2,4", 1); /* services not offered to the phone (see NBT-AAD sdr_builder) */
        setenv("NBT_AA_MEDIA_OUT", "input_stereo_1", 0);
        /* keep channel 8 a GUIDANCE sink (car 2026-11-18: the NBT-AAD SDR builder retypes it to SYSTEM_AUDIO in its
         * Bluetooth audio mode, "nav prompts over Bluetooth"; without Bluetooth audio the phone then spoke the
         * navigation prompts on its own speaker) */
        setenv("NBT_GUIDANCE_BT", "0", 1);
        /* prompts while another source plays: input_stereo_3 over connection b8 (nbt_knob builds it); file prompts-off
         * = only on Android Auto's own source */
        if (access("prompts-off", F_OK) != 0) setenv("NBT_AA_PROMPT_OUT", "input_stereo_3", 0);
        else { unsetenv("NBT_AA_PROMPT_OUT"); setenv("NBT_PROMPT_CONN", "0", 1); }
        setenv("NBT_AUX_SOURCE", "f6", 0);
        setenv("NBT_AUX_OPENS_AA", "0", 0);
    } else {
        setenv("NBT_SDR_DROP", "2,4,9", 1);
        unsetenv("NBT_AA_MEDIA_OUT");
        unsetenv("NBT_AA_PROMPT_OUT");
    }
    /* 15 s of silence before the session counts as dead (was 5; car 2026-11-17 and 2026-11-19: sessions ended after 5 s
     * without anything from the phone, while it had not crashed and its USB link stayed up). The phone pings once a
     * second, so silence is never normal; a short stall now freezes the picture instead of ending the session. A real
     * unplug still ends it at once (the USB transfer fails). */
    setenv("NBT_RX_TIMEOUT_S", "15", 1);

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
    /* decoder mode (optional, 2026-11-18): the picture is decoded and shown by nbt_decoder (src/android_auto/decoder:
     * FFmpeg H.264 on the CPU, shown through the car's INvVideo in iDrive's EARLYVIDEO window). Replaces fifo_http +
     * Cinemo (decode_runner) + nbt_hwvideo for each session; the window hides itself while /fs/ram3/nbt-aa-hidden
     * exists. Whole 800x480 canvas: full width (anamorph, stretched to 1280x480) or, with split800, 1:1 in an 800x480
     * window (--size 800x480) so iDrive's split screen stays visible on the right. center800 is not offered here.
     * decoder.60fps: ask the phone for 60 fps (NBT_VID_FPS) and configure the car's stream for 60 (--fps 60). decoder.args: extra
     * arguments for nbt_decoder (one line, after ours). */
    dmode = hw && access("decoder.mode", F_OK) == 0 && access("nbt_decoder", X_OK) == 0;
    if (dmode) {
        int n = 0;
        const int gsplit = right;
        const int want60 = access("decoder.60fps", F_OK) == 0;
        /* decoder.720 (full screen, test 2026-11-22): the phone draws a 1280x720 canvas with its interface in the top
         * 480 rows (inset 0,240,0,0, as NBT-AAD's own default for this panel) and nbt_decoder shows those rows 1:1:
         * sharp, instead of 800x480 stretched 1.6x horizontally. 2.4x the pixels to decode on the CPU. */
        const int sharp = !gsplit && access("decoder.720", F_OK) == 0;
        center = right = res720 = 0;
        ana = !gsplit && !sharp;
        setenv("NBT_VID_RES", sharp ? "2" : "1", 1);
        setenv("NBT_VID_INSETS", sharp ? "0,240,0,0" : "0,0,0,0", 1);
        if (gsplit || sharp)
            unsetenv("NBT_VID_PAR");
        else
            setenv("NBT_VID_PAR", "16000", 0);
        if (want60)
            setenv("NBT_VID_FPS", "60", 1);
        ndargv[n++] = "./nbt_decoder";
        ndargv[n++] = "--input";
        ndargv[n++] = "annexb.fifo";
        ndargv[n++] = "--ready";
        ndargv[n++] = "http.ready";
        if (gsplit) {
            ndargv[n++] = "--size";
            ndargv[n++] = "800x480";
        }
        if (sharp) {
            ndargv[n++] = "--rows";
            ndargv[n++] = "480";
        }
        /* decoder.hw (test 2026-11-22): the car's own hardware H.264 decoder; decoder.hwfail (written by nbt_decoder
         * when it gave up on it) turns it off until the next boot */
        if (access("decoder.hw", F_OK) == 0) {
            if (access("decoder.hwfail", F_OK) == 0)
                printf("[native] decoder.hw: off until the next boot (decoder.hwfail)\n");
            else
                ndargv[n++] = "--hw";
        }
        if (want60) {
            ndargv[n++] = "--fps";
            ndargv[n++] = "60";
        }
        ndargv[n] = 0;
        {
            FILE* f = fopen("decoder.args", "r");
            if (f) {
                if (fgets(ndecargs, sizeof(ndecargs), f)) {
                    size_t k = strlen(ndecargs);
                    while (k && (ndecargs[k - 1] == '\n' || ndecargs[k - 1] == '\r'))
                        ndecargs[--k] = 0;
                    split(ndecargs, ndargv + n, 24 - n);
                }
                fclose(f);
            }
        }
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
        /* steering-wheel voice button: a long press starts the Google Assistant while Android Auto is on screen (the
         * car's microphone goes to the phone, aa_mic); a short press stays with BMW's voice control.
         * NBT_PTT_ASSISTANT=0 leaves the long press to iDrive too. */
        setenv("NBT_PTT_ASSISTANT", "1", 0);
        setenv("LD_LIBRARY_PATH", SYSTEM_LIBRARY_PATH, 1);
        knob = spawn(args, 0, "knob.log");
        setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
    }

    /* decoder mode at boot (car 2026-11-18): with the phone already plugged in, nbt_decoder started ~19 s after boot
     * (our plugin starts the service as soon as iDrive loads it) showed its first pictures from 23 s on, and from
     * ~26 s every upload waited 3 s (VideoFrame 0x800f0053) for good. At 26 s iDrive logs "MARKER HMIReady" and its
     * navigation guidance viewer attaches its views; started after that (phone plugged in later) it works. So right
     * after boot the phone is not touched until "MARKER HMIReady" is in the system log, plus decoder.delay seconds
     * (default 2). Later starts (mode change, service restart) do not wait: after 90 s of uptime the check is
     * skipped, and the wait ends at 60 s of uptime at the latest. */
    if (dmode && ms() < 90000) {
        long grace = 2;
        FILE* f = fopen("decoder.delay", "r");
        if (f) {
            long v;
            if (fscanf(f, "%ld", &v) == 1 && v >= 0 && v <= 60)
                grace = v;
            fclose(f);
        }
        while (!stopping && ms() < 60000 && system("sloginfo 2>/dev/null | grep -q 'MARKER HMIReady'") != 0) {
            report("waiting_for_hmi", session);
            usleep(500000);
        }
        {
            long long until = ms() + grace * 1000;
            while (!stopping && ms() < until)
                usleep(100000);
        }
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
            /* gone for more than 10 s: really unplugged (a USB reset of ours only makes it vanish for a moment) */
            if (!absent_since)
                absent_since = ms();
            if (ms() - absent_since > 10000)
                resets = 0;
            report(g_phone_present ? "several_devices" : "waiting_for_phone", session);
            sleep(1);
            continue;
        }
        absent_since = 0;
        /* The last attempt failed (user 2026-11-12: after "Restart Android Auto" the phone often did not come back until
         * it was replugged): reset its USB port, the software version of pulling the cable, and try again. "Reconnect the
         * phone" only after 3 resets in a row; a real unplug (> 10 s) or a good session starts the count again. */
        if (attempts > 0) {
            if (resets >= 3) {
                report("replug_required", session);
                sleep(2);
                continue;
            }
            ++resets;
            report("connecting", session);
            reset_phone(bus, device, type, resets);
            attempts = 0;
            sleep(3);
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
        if (dmode) {
            /* nbt_decoder reads the FIFO itself and creates http.ready when it waits for a writer. It is started once
             * and kept for the following sessions (a closed FIFO only ends that session's stream): car
             * 2026-11-18/19, starting and killing it for every session froze the whole iDrive display when the phone
             * was plugged in again (once "NO SIGNAL" and an iDrive restart). Started again only if it ended. */
            if (!alive(&ndec)) {
                char ndecname[40];
                unlink("http.ready");
                if (access("decoder.hwfail", F_OK) == 0) {   /* nbt_decoder gave up on the hardware decoder */
                    int k = 0, j = 0;
                    for (; ndargv[k]; ++k)
                        if (strcmp(ndargv[k], "--hw") != 0)
                            ndargv[j++] = ndargv[k];
                    if (j != k)
                        printf("[native] decoder.hwfail: nbt_decoder again without --hw\n");
                    ndargv[j] = 0;
                }
                snprintf(ndecname, sizeof(ndecname), "nbt_decoder-%u.log", session);
                setenv("LD_LIBRARY_PATH", DECODER_LIBRARY_PATH, 1);
                ndec = spawn(ndargv, 0, ndecname);
                setenv("LD_LIBRARY_PATH", CINEMO_LIBRARY_PATH, 1);
            }
            {
                long long limit = ms() + 10000;
                while (!stopping && access("http.ready", F_OK) && alive(&ndec) && ms() < limit)
                    usleep(100000);
            }
        } else if (!live) {
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
        if (!stopping && (dmode ? (!access("http.ready", F_OK) && alive(&ndec))
                              : live ? alive(&http) : (!access("http.ready", F_OK) && alive(&decoder) && alive(&http)))) {
            char* args[] = {"./oneplus_native", "--capture-video", bus_text, device_text, 0};
            snprintf(bus_text, sizeof(bus_text), "%d", bus);
            snprintf(device_text, sizeof(device_text), "%d", device);
            {
                char capname[40];
                snprintf(capname, sizeof(capname), "capture-%u.log", session);
                unlink("/fs/ram3/nbt-aa-media"); /* the phone's media state, written anew by this session */
                capture = spawn(args, 0, capname);
            }
            report("running", session);
            {
                long long began = ms();
                while (!stopping && alive(&capture) && (dmode ? alive(&ndec) : alive(&http)) &&
                       (live || dmode || (alive(&decoder) && (hw || alive(&frames))))) {
                    usleep(200000);
                    keep_reported(session);
                }
                /* a session that ran for a minute was a good one: its end is not a failed attempt (review 2026-11-09:
                 * the counter was only reset by unplugging, so the third normal session end of a drive showed
                 * "replug the phone") */
                if (ms() - began >= 60000)
                    attempts = resets = 0;
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
        if (!live && !dmode)
            unlink("http.ready");
        /* Clear only the FIFO bytes left by this completed session (live mode: nbt_live_file owns the FIFO reader;
         * decoder mode: nbt_decoder keeps reading it). */
        if (!live && !dmode) {
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
    end(&ndec);
    end(&hwv);
    if (center)
        hmi_visible();
    end(&capture);
    end(&http);
    end(&decoder);
    end(&frames);
    end(&knob);
    end(&lease);
    unlink("input.fifo");
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
