/* nbt_ipod_tap: records what the head unit and the stock Bluetooth iPod driver say to each other (2026-11-23).
 *
 * Why: our own iPod driver at /fs/btipod (iPod over Bluetooth) is opened by the head unit's media application, which
 * sends io_msg 0x80 (arg 2) and then waits for something we do not know - the same step that made the USB route need
 * a second plug-in. A real iPhone connected by Bluetooth gets past it. This program stands in front of the stock driver
 * while the iPhone connects and writes down every request and every answer, so the step can be rebuilt offline.
 *
 * How: it watches /proc/mount/<mount>/ (every 25 ms). As soon as the stock driver has mounted, it attaches in front of it
 * (_RESMGR_FLAG_BEFORE) and passes every request through unchanged: open, read, write, lseek, stat, devctl, io_msg and
 * ionotify (the stock driver's notifications come to us as pulses and are passed on to the client at once). When the
 * stock mount disappears, it takes its name away again and waits for the next mount (a remount shows in the log).
 *
 * To open the stock files it uses open() on the same path: its own request comes back to itself first and is answered
 * ENOENT, so the C library goes on to the next server = the stock driver. Right after attaching it checks that this
 * works; if not, it detaches at once and ends (the iPhone then works as without it).
 *
 * It changes nothing; only class-0x01 devctls (flags of the client's own open file) are answered by itself.
 *
 * Usage: nbt_ipod_tap --mount /fs/btipod --log /fs/ram3/aatool/tap.log [--seconds 900]
 */
struct t_ocb;
#define IOFUNC_OCB_T struct t_ocb
#define RESMGR_OCB_T struct t_ocb
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <devctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXARMED 64
#define MAXKEYS 512
#define FULL_LOG 40 /* the first answers of each kind are always written, later only when they change */

struct t_ocb {
    iofunc_ocb_t hdr; /* must be first */
    int fd;
    int pid;
    char rel[96];
    unsigned seen_meta; /* --run: Android Auto title data this client already got as events */
    long last_pos_ms;   /* --run: when it last got a position event (one per notification burst) */
};

static volatile sig_atomic_t g_stop;
static const char* g_mount;
static unsigned g_lines;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_dir_attr, g_file_attr;
static int g_chid = -1, g_coid = -1;
/* --iap 1 (in front of the Bluetooth audio driver /fs/avrcpN): the device description (devctl 0x0e8b) says
 * <iap status="supported"/> instead of "not_supported". The head unit's media application takes a phone as "iPod over
 * Bluetooth capable" only with that (strings "iap status=\"supported\"", "[DD] Device is IAP over BT capable ...") and
 * otherwise ignores whatever appears at /fs/btipod. The only change this program makes. */
static int g_iap;
/* --block-pid <pid> (car 2026-11-23): opens of this process are refused (EACCES, not ENOENT, so it does not reach the
 * stock driver either). For the faked iPod over Bluetooth the head unit's player io-nme streams the phone's A2DP sound
 * from /fs/avrcp0 into /dev/io-amp/input_stereo_0 - silence (Android Auto plays over USB) that keeps our own sound
 * player out of that input. Without access its graph fails and frees the input. */
static int g_block_pid;
/* --url <url> (2026-11-23): answer of the sound URL request (devctl 0x0e6e) instead of the phone's A2DP stream
 * ("wms:/net/hu-intel.nbt//dev/wms/player1"), e.g. http://127.0.0.1:18766/aa.wav served by nbt_aa_wav_http with the
 * Android Auto sound from USB. --run <dir>: the playback status (0x0e19) says "playing" / "paused" as Android Auto
 * does (<dir>/media.state, state=2 = playing) - the phone itself reports "paused" over Bluetooth, and a paused source
 * would not be played by the head unit's player. */
static const char* g_url;
static const char* g_run;
/* With --iap (2026-11-26): only a phone that connects by Bluetooth while a USB session runs is made an iPod (car: an
 * iPhone connected only by Bluetooth showed its titles but had no sound, the sound URL pointed at our empty device).
 * g_claimed = the current Bluetooth connection was made one; <run>/iap.claimed tells nbt-aa-iap.sh to start nbt_aa_ipod.
 * Without it, or once the session has ended, everything is passed through unchanged (plain Bluetooth audio). */
static volatile int g_claimed;

/* ---- what Android Auto plays (<run>/media.state of oneplus_native), read at most twice a second ---- */
static struct {
    char title[200], artist[200], album[200];
    int playing, valid;
    long position_s, duration_s;
    time_t stamp;
    unsigned meta; /* grows with every change of title / artist / album */
} aa;
static pthread_mutex_t g_aa_lock = PTHREAD_MUTEX_INITIALIZER;

static void aa_refresh(void) /* under g_aa_lock */
{
    static struct timespec last;
    struct timespec now;
    char path[160], line[400], title[200] = "", artist[200] = "", album[200] = "";
    struct stat st;
    FILE* f;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (aa.valid && (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 < 500)
        return;
    last = now;
    snprintf(path, sizeof(path), "%s/media.state", g_run);
    if (stat(path, &st) || (aa.valid && st.st_mtime == aa.stamp))
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    aa.playing = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, "title=", 6))
            snprintf(title, sizeof(title), "%s", line + 6);
        else if (!strncmp(line, "artist=", 7))
            snprintf(artist, sizeof(artist), "%s", line + 7);
        else if (!strncmp(line, "album=", 6))
            snprintf(album, sizeof(album), "%s", line + 6);
        else if (!strncmp(line, "state=", 6))
            aa.playing = atoi(line + 6) == 2; /* oneplus_native: 2 playing, 3 paused */
        else if (!strncmp(line, "position=", 9))
            aa.position_s = atol(line + 9);
        else if (!strncmp(line, "duration=", 9))
            aa.duration_s = atol(line + 9);
    }
    fclose(f);
    if (!aa.valid || strcmp(title, aa.title) || strcmp(artist, aa.artist) || strcmp(album, aa.album)) {
        ++aa.meta;
        strcpy(aa.title, title);
        strcpy(aa.artist, artist);
        strcpy(aa.album, album);
    }
    aa.stamp = st.st_mtime;
    aa.valid = 1;
}

static int aa_playing(void)
{
    int p;
    pthread_mutex_lock(&g_aa_lock);
    aa_refresh();
    p = aa.playing;
    pthread_mutex_unlock(&g_aa_lock);
    return p;
}

static long aa_position_ms(void) /* under g_aa_lock */
{
    long ms = aa.position_s * 1000;
    if (aa.playing && time(0) > aa.stamp)
        ms += (long)(time(0) - aa.stamp) * 1000;
    if (aa.duration_s > 0 && ms > aa.duration_s * 1000)
        ms = aa.duration_s * 1000;
    return ms;
}

/* Event queue of the Bluetooth audio driver (devctl 0x0e37, read by io-nme after each notification), recorded on the
 * car 2026-11-24 00:21 with a phone playing over Bluetooth. Records: u32 type, u32 size (multiple of 8), data:
 *   2 track changed:  index, length ms, 0, 0                                   (24 bytes)
 *   3 position:       0, position ms, length ms, 0                             (24 bytes)
 *   4 metadata:       key (1 song, 2 album, 3 artist, 9 length as text), 0, length ms, 12 x 0, text from byte 68
 *                     (size = 72 + text with its 0 rounded up to 4, rounded up to 8)
 *   5 text "MEDIABT_EVENT_NOW_PLAYING_CHANGED"                                 (48 bytes)
 * The phone's own queue says nothing while Android Auto plays over USB (or stale data); with --run its track / metadata /
 * position records are replaced by Android Auto's. */
static size_t ev_put(unsigned char* out, size_t room, uint32_t type, const uint32_t* words, int nwords, const char* text)
{
    size_t size = 8 + (size_t)nwords * 4;
    uint32_t head[2];
    if (text)
        size = (72 + ((strlen(text) + 1 + 3) & ~(size_t)3) + 7) & ~(size_t)7;
    else
        size = (size + 7) & ~(size_t)7;
    if (size > room)
        return 0;
    memset(out, 0, size);
    head[0] = type;
    head[1] = (uint32_t)size;
    memcpy(out, head, 8);
    memcpy(out + 8, words, (size_t)nwords * 4);
    if (text)
        memcpy(out + 68, text, strlen(text) + 1);
    return size;
}

static size_t ev_meta(unsigned char* out, size_t room, uint32_t key, const char* text, uint32_t length_ms)
{
    const uint32_t w[3] = {key, 0, length_ms};
    return ev_put(out, room, 4, w, 3, text);
}

/* builds Android Auto's events; seen = what this client got last (meta counter) */
static size_t aa_events(unsigned char* out, size_t room, unsigned* seen, int with_position)
{
    size_t used = 0;
    uint32_t len_ms, w[4];
    int changed;
    pthread_mutex_lock(&g_aa_lock);
    aa_refresh();
    len_ms = (uint32_t)(aa.duration_s > 0 ? aa.duration_s * 1000 : 0);
    changed = aa.valid && *seen != aa.meta;
    /* order as recorded: track changed, song, artist, album, length, position, now playing changed */
    if (changed) {
        char ms_text[16];
        w[0] = 0; /* index: always 0 with a phone (AVRCP has no playback list) */
        w[1] = len_ms;
        w[2] = w[3] = 0;
        used += ev_put(out + used, room - used, 2, w, 4, 0);
        used += ev_meta(out + used, room - used, 1, *aa.title ? aa.title : "Android Auto", len_ms);
        used += ev_meta(out + used, room - used, 3, aa.artist, len_ms);
        used += ev_meta(out + used, room - used, 2, aa.album, len_ms);
        snprintf(ms_text, sizeof(ms_text), "%u", (unsigned)len_ms);
        used += ev_meta(out + used, room - used, 9, ms_text, len_ms);
    }
    if (aa.valid && (with_position || changed)) {
        w[0] = 0;
        w[1] = (uint32_t)aa_position_ms();
        w[2] = len_ms;
        w[3] = 0;
        used += ev_put(out + used, room - used, 3, w, 4, 0);
    }
    if (changed) {
        if (room - used >= 48) {
            memset(out + used, 0, 48);
            w[0] = 5;
            w[1] = 48;
            memcpy(out + used, w, 8);
            memcpy(out + used + 8, "MEDIABT_EVENT_NOW_PLAYING_CHANGED", 34);
            used += 48;
        }
        *seen = aa.meta;
    }
    pthread_mutex_unlock(&g_aa_lock);
    return used;
}

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* A phone session is on: <run>/status.txt of native_service says connecting / starting_decoder / running. The service
 * rewrites it at least every 2 s, so a file older than 10 s means the service is gone. Read at most twice a second. */
static int session_on(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static long last_ms = -1000000;
    static int on;
    char path[160], line[80];
    struct stat st;
    FILE* f;
    int r;
    if (!g_run)
        return 1;
    pthread_mutex_lock(&lock);
    if (mono_ms() - last_ms >= 500) {
        last_ms = mono_ms();
        on = 0;
        snprintf(path, sizeof(path), "%s/status.txt", g_run);
        f = !stat(path, &st) && time(0) - st.st_mtime <= 10 ? fopen(path, "r") : 0;
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "state=", 6))
                on = !strncmp(line + 6, "running", 7) || !strncmp(line + 6, "starting_decoder", 16) ||
                     !strncmp(line + 6, "connecting", 10);
        if (f)
            fclose(f);
    }
    r = on;
    pthread_mutex_unlock(&lock);
    return r;
}

static void set_claimed(int on)
{
    char path[160];
    g_claimed = on;
    if (!g_run)
        return;
    snprintf(path, sizeof(path), "%s/iap.claimed", g_run);
    if (on)
        close(open(path, O_WRONLY | O_CREAT, 0644));
    else
        unlink(path);
}

/* ---- log ---- */
static void note(const char* format, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;
    time_t t;
    pthread_mutex_lock(&g_log_lock);
    if (++g_lines > 400000) {
        pthread_mutex_unlock(&g_log_lock);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    t = ts.tv_sec;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_log_lock);
}

static void hex(char* out, size_t room, const void* data, size_t n)
{
    const unsigned char* p = data;
    size_t i, used = 0;
    out[0] = 0;
    for (i = 0; i < n && used + 4 < room; ++i)
        used += (size_t)snprintf(out + used, room - used, "%02x%s", p[i], (i & 3) == 3 ? " " : "");
}

/* printable copy (for names / XML), dots for the rest */
static void text(char* out, size_t room, const void* data, size_t n)
{
    const unsigned char* p = data;
    size_t i;
    if (!room)
        return;
    if (n > room - 1)
        n = room - 1;
    for (i = 0; i < n; ++i)
        out[i] = p[i] >= 32 && p[i] < 127 ? (char)p[i] : (p[i] == '\n' ? '|' : '.');
    out[n] = 0;
}

static const char* who(int pid)
{
    static struct {
        int pid;
        char name[48];
    } cache[32];
    static int used;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    char path[64], buf[200];
    int i, fd;
    ssize_t n;
    const char* base;
    pthread_mutex_lock(&lock);
    for (i = 0; i < used; ++i)
        if (cache[i].pid == pid) {
            pthread_mutex_unlock(&lock);
            return cache[i].name;
        }
    snprintf(path, sizeof(path), "/proc/%d/exefile", pid);
    fd = open(path, O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0)
        strcpy(buf, "?");
    else
        buf[n] = 0;
    base = strrchr(buf, '/') ? strrchr(buf, '/') + 1 : buf;
    i = used < 32 ? used++ : 31;
    cache[i].pid = pid;
    snprintf(cache[i].name, sizeof(cache[i].name), "%s", base);
    pthread_mutex_unlock(&lock);
    return cache[i].name;
}

/* repeated polls (status twice a second, ...) only when the answer changes */
static struct {
    char key[120];
    unsigned count, hash;
} g_keys[MAXKEYS];

static unsigned fnv(const void* data, size_t n)
{
    const unsigned char* p = data;
    unsigned h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/* 1 = write this one, *count = how often this kind was seen */
static int worth(const char* key, const void* answer, size_t n, unsigned* count)
{
    unsigned h = fnv(answer, n);
    int i, r = 1;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAXKEYS && g_keys[i].key[0]; ++i)
        if (!strcmp(g_keys[i].key, key))
            break;
    if (i == MAXKEYS)
        i = MAXKEYS - 1;
    if (!g_keys[i].key[0])
        snprintf(g_keys[i].key, sizeof(g_keys[i].key), "%s", key);
    ++g_keys[i].count;
    if (g_keys[i].count > FULL_LOG && g_keys[i].hash == h)
        r = 0;
    g_keys[i].hash = h;
    *count = g_keys[i].count;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* /proc/mount/<mount>/ entries ("nd,pid,chid,handle,type"), ours left out; 1 = the stock driver is mounted */
static int stock_entries(char* list, size_t room)
{
    char path[160], mine[32];
    DIR* d;
    struct dirent* e;
    int found = 0;
    size_t used = 0;
    list[0] = 0;
    snprintf(path, sizeof(path), "/proc/mount%s", g_mount);
    snprintf(mine, sizeof(mine), ",%d,", (int)getpid());
    d = opendir(path);
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || strstr(e->d_name, mine))
            continue;
        found = 1;
        used += (size_t)snprintf(list + used, used < room ? room - used : 0, "%s ", e->d_name);
        if (used >= room)
            used = room - 1;
    }
    closedir(d);
    return found;
}

/* ---- notification passing ---- */
static struct {
    int used, rcvid, flags;
    struct t_ocb* o;
    struct sigevent event;
} g_armed[MAXARMED];

static void* pulse_thread(void* arg)
{
    struct _pulse p;
    (void)arg;
    while (!g_stop) {
        int i, rcvid = MsgReceivePulse(g_chid, &p, sizeof(p), 0);
        if (rcvid < 0)
            continue;
        if (p.code != 1)
            continue;
        i = p.value.sival_int;
        pthread_mutex_lock(&g_lock);
        if (i >= 0 && i < MAXARMED && g_armed[i].used) {
            struct t_ocb* o = g_armed[i].o;
            MsgDeliverEvent(g_armed[i].rcvid, &g_armed[i].event);
            g_armed[i].used = 0;
            pthread_mutex_unlock(&g_lock);
            note("event   %-28s pid %d (%s) flags %08x: stock fired, passed on", o->rel, o->pid, who(o->pid),
                 (unsigned)g_armed[i].flags);
        } else
            pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

/* ---- resource manager ---- */
static int t_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    char full[200], rel[96];
    struct t_ocb* o;
    int ioflag = msg->connect.ioflag, acc = (ioflag & 3) ? (ioflag & 3) - 1 : O_RDONLY, oflag, fd, r;
    (void)handle;
    (void)extra;
    if (ctp->info.pid == getpid()) /* our own open of the stock file: let the C library go on to the stock driver */
        return ENOENT;
    if (g_block_pid && ctp->info.pid == g_block_pid) {
        note("blocked open %s by pid %d", msg->connect.path, ctp->info.pid);
        return EACCES;
    }
    if (strlen(msg->connect.path) >= sizeof(rel))
        return ENAMETOOLONG;
    snprintf(rel, sizeof(rel), "%s", msg->connect.path);
    snprintf(full, sizeof(full), "%s%s%s", g_mount, *rel ? "/" : "", rel);
    oflag = acc | (ioflag & O_NONBLOCK);
    fd = open(full, oflag);
    if (fd < 0 && acc != O_RDONLY)
        fd = open(full, O_RDONLY | (ioflag & O_NONBLOCK));
    if (fd < 0) {
        const int e = errno;
        note("open    %-28s pid %d (%s) ioflag %x -> %s", *rel ? rel : "/", ctp->info.pid, who(ctp->info.pid), ioflag,
             strerror(e));
        return e;
    }
    o = calloc(1, sizeof(*o));
    if (!o) {
        close(fd);
        return ENOMEM;
    }
    o->fd = fd;
    o->pid = ctp->info.pid;
    snprintf(o->rel, sizeof(o->rel), "%s", *rel ? rel : "/");
    {
        struct stat st;
        const int dir = !fstat(fd, &st) && S_ISDIR(st.st_mode);
        r = iofunc_ocb_attach(ctp, msg, &o->hdr, dir ? &g_dir_attr : &g_file_attr, &g_io);
    }
    if (r != EOK) {
        close(fd);
        free(o);
        return r;
    }
    note("open    %-28s pid %d (%s) ioflag %x subtype %d", o->rel, o->pid, who(o->pid), ioflag, msg->connect.subtype);
    return EOK;
}

static int t_close(resmgr_context_t* ctp, void* reserved, RESMGR_OCB_T* o)
{
    int i;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAXARMED; ++i)
        if (g_armed[i].used && g_armed[i].o == o)
            g_armed[i].used = 0;
    pthread_mutex_unlock(&g_lock);
    note("close   %-28s pid %d", o->rel, o->pid);
    close(o->fd);
    return iofunc_close_ocb_default(ctp, reserved, &o->hdr);
}

static int t_stat(resmgr_context_t* ctp, io_stat_t* msg, RESMGR_OCB_T* o)
{
    struct stat st;
    if (fstat(o->fd, &st))
        return errno;
    msg->o = st;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int t_lseek(resmgr_context_t* ctp, io_lseek_t* msg, RESMGR_OCB_T* o)
{
    off_t r = lseek(o->fd, (off_t)msg->i.offset, msg->i.whence);
    char key[160];
    unsigned count;
    if (r < 0)
        return errno;
    snprintf(key, sizeof(key), "lseek %s %d", o->rel, msg->i.whence);
    if (worth(key, &r, sizeof(r), &count))
        note("lseek   %-28s pid %d whence %d offset %lld -> %lld", o->rel, o->pid, msg->i.whence, (long long)msg->i.offset,
             (long long)r);
    o->hdr.offset = r;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = r;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int t_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* o)
{
    const int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    size_t want = msg->i.nbytes;
    ssize_t got;
    char* buf;
    char key[160], shown[700];
    unsigned count;
    if (want > 65536)
        want = 65536;
    buf = malloc(want ? want : 1);
    if (!buf)
        return ENOMEM;
    if (xtype == _IO_XTYPE_OFFSET)
        got = pread(o->fd, buf, want, (off_t)((struct _xtype_offset*)(&msg->i + 1))->offset);
    else
        got = read(o->fd, buf, want);
    if (got < 0) {
        const int e = errno;
        note("read    %-28s pid %d want %u -> %s", o->rel, o->pid, (unsigned)want, strerror(e));
        free(buf);
        return e;
    }
    snprintf(key, sizeof(key), "read %s", o->rel);
    if (worth(key, buf, (size_t)got, &count)) {
        if (strstr(o->rel, ".xml"))
            text(shown, sizeof(shown), buf, (size_t)got);
        else
            hex(shown, 200, buf, (size_t)got > 64 ? 64 : (size_t)got);
        note("read    %-28s pid %d want %u got %d (#%u): %s", o->rel, o->pid, (unsigned)want, (int)got, count, shown);
    }
    MsgReply(ctp->rcvid, (int)got, buf, (size_t)got);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int t_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* o)
{
    size_t n = msg->i.nbytes;
    char* all;
    char shown[200];
    ssize_t done;
    if (n > 65536)
        return EINVAL;
    all = malloc(n ? n : 1);
    if (!all)
        return ENOMEM;
    if (resmgr_msgread(ctp, all, n, sizeof(msg->i)) < 0) {
        free(all);
        return EIO;
    }
    done = write(o->fd, all, n);
    hex(shown, sizeof(shown), all, n > 48 ? 48 : n);
    note("write   %-28s pid %d %u bytes -> %d: %s", o->rel, o->pid, (unsigned)n, (int)done, shown);
    free(all);
    if (done < 0)
        return errno;
    _IO_SET_WRITE_NBYTES(ctp, done);
    return _RESMGR_NPARTS(0);
}

static int t_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* o)
{
    const unsigned dcmd = (unsigned)msg->i.dcmd;
    unsigned nbytes = msg->i.nbytes, count;
    unsigned char* data = (unsigned char*)_DEVCTL_DATA(msg->i);
    unsigned char in[64];
    char key[160], shown_in[200], shown_out[1400];
    int info = 0, rc, ours;
    const size_t in_n = nbytes < sizeof(in) ? nbytes : sizeof(in);

    if ((dcmd & 0xff00u) == 0x0100u) /* flags of the client's own open file */
        return iofunc_devctl_default(ctp, msg, &o->hdr);
    if (nbytes > 65000)
        nbytes = 65000;
    memcpy(in, data, in_n);
    rc = devctl(o->fd, (int)dcmd, data, nbytes, &info);
    /* the answers below are only changed while this connection is our iPod and the session still runs */
    ours = !g_iap || (g_claimed && session_on());
    /* the URL always: the player asks once per Bluetooth connection, long before a session may start (car 2026-11-26);
     * without a session nbt_aa_wms hands on the phone's own stream */
    if (g_url && (dcmd & 0xffffu) == 0x0e6eu && nbytes > strlen(g_url)) {
        const char* was = rc == EOK ? (const char*)data : "(error)";
        char old[100];
        snprintf(old, sizeof(old), "%s", was);
        memset(data, 0, nbytes);
        strcpy((char*)data, g_url);
        info = (int)strlen(g_url);
        rc = EOK;
        note("url     %-28s pid %d (%s): %s -> %s", o->rel, o->pid, who(o->pid), old, g_url);
    }
    if (g_run && ours && rc == EOK && (dcmd & 0xffffu) == 0x0e19u && nbytes >= 8) {
        uint32_t* s = (uint32_t*)data;
        s[1] = aa_playing() ? 1 : 2;
        if (nbytes >= 28) { /* length and position as Android Auto has them */
            pthread_mutex_lock(&g_aa_lock);
            if (aa.valid && aa.duration_s > 0) {
                s[5] = (uint32_t)(aa.duration_s * 1000);
                s[6] = (uint32_t)aa_position_ms();
            }
            pthread_mutex_unlock(&g_aa_lock);
        }
    }
    /* --run: the event queue with Android Auto's title data (see aa_events); the phone's own track / metadata /
     * position / now-playing records are left out, anything else of it is passed on behind ours */
    if (g_run && ours && (dcmd & 0xffffu) == 0x0e37u && nbytes >= 64) {
        unsigned char* mine = malloc(nbytes);
        size_t used = 0, at = 0;
        const size_t stock = rc == EOK && info > 0 ? (size_t)info : 0;
        const long now = mono_ms();
        const int with_position = now - o->last_pos_ms >= 400;
        const unsigned before = o->seen_meta;
        if (mine) {
            used = aa_events(mine, nbytes, &o->seen_meta, with_position);
            if (with_position)
                o->last_pos_ms = now;
            while (at + 8 <= stock && stock <= nbytes) {
                uint32_t h[2];
                memcpy(h, data + at, 8);
                if (h[1] < 8 || at + h[1] > stock)
                    break;
                if ((h[0] < 2 || h[0] > 5) && used + h[1] <= nbytes) {
                    memcpy(mine + used, data + at, h[1]);
                    used += h[1];
                }
                at += h[1];
            }
            memcpy(data, mine, used);
            free(mine);
            info = (int)used;
            rc = EOK;
            if (before != o->seen_meta)
                note("events  %-28s pid %d (%s): Android Auto title data put into the queue (%u bytes)", o->rel, o->pid,
                     who(o->pid), (unsigned)used);
        }
    }
    if (g_run && ours && nbytes > 1 &&
        ((dcmd & 0xffffu) == 0x0e64u || (dcmd & 0xffffu) == 0x0e65u || (dcmd & 0xffffu) == 0x0e66u)) {
        pthread_mutex_lock(&g_aa_lock);
        aa_refresh();
        if (aa.valid) {
            const char* t = (dcmd & 0xffffu) == 0x0e64u ? (*aa.title ? aa.title : "Android Auto")
                            : (dcmd & 0xffffu) == 0x0e66u ? aa.artist : aa.album;
            const size_t n = strlen(t) + 1 < nbytes ? strlen(t) + 1 : nbytes;
            memset(data, 0, nbytes < 256 ? nbytes : 256);
            memcpy(data, t, n);
            data[n - 1] = 0;
            rc = EOK;
            info = (int)(n - 1); /* this driver answers the length without the 0 ("One Dance" -> 9) */
        }
        pthread_mutex_unlock(&g_aa_lock);
    }
    if (g_run && ours && rc == EOK && (dcmd & 0xffffu) == 0x0e6bu) {
        pthread_mutex_lock(&g_aa_lock);
        if (aa.valid && aa.duration_s > 0)
            info = (int)(aa.duration_s * 1000);
        pthread_mutex_unlock(&g_aa_lock);
    }
    /* always said, also without a session (car 2026-11-26: saying it only during a session meant Bluetooth off / on
     * after plugging in); without a session the answers above stay the phone's own and nbt-aa-iap.sh keeps the iPod
     * place away, so the phone is plain Bluetooth audio until a session starts */
    if (g_iap && rc == EOK && (dcmd & 0xffffu) == 0x0e8bu && nbytes > 64) {
        static const char from[] = "<iap status=\"not_supported\"/>", to[] = "<iap status=\"supported\"/>";
        char* end = memchr(data, 0, nbytes);
        char* at;
        if (!g_claimed)
            set_claimed(1);
        if (!end) {
            end = (char*)data + nbytes - 1;
            *end = 0;
        }
        at = strstr((char*)data, from);
        if (at) {
            const size_t shrink = sizeof(from) - sizeof(to);
            memcpy(at, to, sizeof(to) - 1);
            memmove(at + sizeof(to) - 1, at + sizeof(from) - 1, (size_t)(end - (at + sizeof(from) - 1)) + 1);
            memset(end - shrink + 1, 0, shrink);
            if (info > (int)shrink)
                info -= (int)shrink;
            note("iap     %-28s pid %d (%s): device description now says iap supported (ret %d)", o->rel, o->pid,
                 who(o->pid), info);
        }
    }
    snprintf(key, sizeof(key), "devctl %s %08x", o->rel, dcmd);
    {
        unsigned char sig[72];
        memset(sig, 0, sizeof(sig));
        memcpy(sig, &rc, sizeof(rc));
        memcpy(sig + 4, &info, sizeof(info));
        if (rc == EOK)
            memcpy(sig + 8, data, nbytes < 64 ? nbytes : 64);
        if (worth(key, sig, sizeof(sig), &count)) {
            hex(shown_in, sizeof(shown_in), in, in_n > 32 ? 32 : in_n);
            if (rc == EOK) {
                /* text answers (song, artist, ...) are easier to read as text */
                if (nbytes && data[0] >= 32 && data[0] < 127 && memchr(data, 0, nbytes < 256 ? nbytes : 256))
                    text(shown_out, sizeof(shown_out), data, strlen((char*)data));
                else
                    /* the event queue (0x0e37, track changed / metadata ...) in full: its layout is what we need */
                    hex(shown_out, sizeof(shown_out), data, (dcmd & 0xffffu) == 0x0e37u ? (nbytes > 300 ? 300 : nbytes) : (nbytes > 96 ? 96 : nbytes));
            } else
                snprintf(shown_out, sizeof(shown_out), "error %d (%s)", rc, strerror(rc));
            note("devctl  %-28s pid %d (%s) %08x nbytes %u (#%u) in [%s] -> ret %d: %s", o->rel, o->pid, who(o->pid), dcmd,
                 nbytes, count, shown_in, info, shown_out);
        }
    }
    if (rc != EOK)
        return rc;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = info;
    msg->o.nbytes = nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
}

static int t_notify(resmgr_context_t* ctp, io_notify_t* msg, RESMGR_OCB_T* o)
{
    const int action = msg->i.action, flags = msg->i.flags;
    const struct sigevent event = msg->i.event;
    char key[160];
    unsigned count;
    int bits, i = -1;
    if (action == _NOTIFY_ACTION_POLLARM || action == _NOTIFY_ACTION_TRANARM) {
        struct sigevent ours;
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < MAXARMED && g_armed[i].used; ++i) {
        }
        if (i < MAXARMED) {
            g_armed[i].used = 1;
            g_armed[i].rcvid = ctp->rcvid;
            g_armed[i].flags = flags;
            g_armed[i].o = o;
            g_armed[i].event = event;
        } else
            i = -1;
        pthread_mutex_unlock(&g_lock);
        if (i < 0)
            return EAGAIN;
        SIGEV_PULSE_INIT(&ours, g_coid, SIGEV_PULSE_PRIO_INHERIT, 1, i);
        bits = ionotify(o->fd, action, flags, &ours);
        if (bits != 0) { /* already met (POLLARM then arms nothing) or failed */
            pthread_mutex_lock(&g_lock);
            g_armed[i].used = 0;
            pthread_mutex_unlock(&g_lock);
        }
    } else
        bits = ionotify(o->fd, action, flags, 0);
    if (bits < 0) {
        const int e = errno;
        note("notify  %-28s pid %d action %d flags %08x -> %s", o->rel, o->pid, action, (unsigned)flags, strerror(e));
        return e;
    }
    snprintf(key, sizeof(key), "notify %s %d %08x", o->rel, action, (unsigned)flags);
    if (worth(key, &bits, sizeof(bits), &count))
        note("notify  %-28s pid %d (%s) action %d flags %08x (#%u) -> bits %08x%s", o->rel, o->pid, who(o->pid), action,
             (unsigned)flags, count, (unsigned)bits, i >= 0 && !bits ? ", armed" : "");
    msg->o.flags = bits;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int t_msg(resmgr_context_t* ctp, io_msg_t* msg, RESMGR_OCB_T* o)
{
    const int len = ctp->info.msglen;
    int room = ctp->info.dstmsglen;
    unsigned char* back;
    char shown_in[800], shown_out[800];
    long status;
    int e = 0;
    if (len <= 0)
        return ENOSYS;
    if (room < 0 || room > 65536)
        room = 65536;
    back = calloc(1, room ? (size_t)room : 1);
    if (!back)
        return ENOMEM;
    status = MsgSend(o->fd, msg, len, back, room);
    if (status == -1)
        e = errno;
    hex(shown_in, sizeof(shown_in), msg, len > 192 ? 192 : (size_t)len);
    hex(shown_out, sizeof(shown_out), back, room > 160 ? 160 : (size_t)room);
    note("msg     %-28s pid %d (%s) mgrid %04x sub %04x len %d room %d -> %s%ld%s: in [%s] out [%s]", o->rel, o->pid,
         who(o->pid), msg->i.mgrid, msg->i.subtype, len, room, e ? "error " : "status ", e ? (long)e : status,
         e ? strerror(e) : "", shown_in, shown_out);
    if (e) {
        free(back);
        return e;
    }
    MsgReply(ctp->rcvid, status, back, room);
    free(back);
    return _RESMGR_NOREPLY;
}

/* after attaching: can we still reach the stock driver through ourselves? */
static int self_test(void)
{
    char path[160], buf[64];
    int fd;
    ssize_t n;
    if (!strncmp(g_mount, "/dev/", 5)) { /* a device: just open and close it through ourselves */
        struct stat st;
        if (stat(g_mount, &st)) {
            note("self test: stat %s -> %s", g_mount, strerror(errno));
            return 0;
        }
        note("self test: %s reachable through the tap (mode %o)", g_mount, (unsigned)st.st_mode);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/.FS_info./info.xml", g_mount);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        note("self test: open %s -> %s", path, strerror(errno));
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    note("self test: stock info.xml readable through the tap (%d bytes)", (int)n);
    return n > 0;
}

int main(int argc, char** argv)
{
    const char* log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    struct sigaction sa;
    pthread_t pulses;
    char last[400] = "", now[400];
    int i, id = -1, alive, attaches = 0;
    unsigned seconds = 900;
    struct timespec start, t;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mount"))
            g_mount = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else if (!strcmp(argv[i], "--seconds"))
            seconds = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--iap"))
            g_iap = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--url"))
            g_url = argv[i + 1];
        else if (!strcmp(argv[i], "--run"))
            g_run = argv[i + 1];
        else if (!strcmp(argv[i], "--dump-events")) { /* test: the events made from <dir>/media.state, as hex */
            unsigned char buf[512];
            unsigned seen = 0;
            size_t n, k;
            g_run = argv[i + 1];
            n = aa_events(buf, sizeof(buf), &seen, 1);
            for (k = 0; k < n; ++k)
                printf("%02x%s", buf[k], (k & 3) == 3 ? " " : "");
            printf("\n%u bytes\n", (unsigned)n);
            n = aa_events(buf, sizeof(buf), &seen, 0);
            printf("second read: %u bytes\n", (unsigned)n);
            return 0;
        } else if (!strcmp(argv[i], "--block-pid"))
            g_block_pid = atoi(argv[i + 1]);
        else
            return 2;
    }
    /* /fs/...: media drivers; /dev/wms/...: the Bluetooth stack's A2DP sound device, which the head unit's player opens
     * for the URL "wms:/net/hu-intel.nbt//dev/wms/player1" (recorded to learn its protocol, 2026-11-23) */
    if (!g_mount || (strncmp(g_mount, "/fs/", 4) && strncmp(g_mount, "/dev/wms/", 9)) || strlen(g_mount) > 24)
        return 2;
    if (log && !freopen(log, "a", stderr))
        return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    g_chid = ChannelCreate(0);
    g_coid = g_chid < 0 ? -1 : ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
    if (g_coid < 0)
        return 4;
    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 70000;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = t_open;
    g_io.read = t_read;
    g_io.write = t_write;
    g_io.lseek = t_lseek;
    g_io.stat = t_stat;
    g_io.devctl = t_devctl;
    g_io.close_ocb = t_close;
    g_io.notify = t_notify;
    g_io.msg = t_msg;
    iofunc_attr_init(&g_dir_attr, S_IFDIR | 0555, 0, 0);
    iofunc_attr_init(&g_file_attr, S_IFREG | 0666, 0, 0);
    /* the pool must run before the first attach: the self test needs a second thread to answer our own open */
    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.context_free = dispatch_context_free;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.lo_water = 3;
    pattr.hi_water = 6;
    pattr.increment = 1;
    pattr.maximum = 16;
    /* a dispatch handle needs something attached before a pool can block on it: a private name nobody uses */
    {
        char own[64];
        snprintf(own, sizeof(own), "/dev/nbt_ipod_tap.%d", (int)getpid());
        if (resmgr_attach(dpp, &rattr, own, _FTYPE_ANY, 0, &g_connect, &g_io, &g_file_attr) < 0) {
            note("resmgr_attach(%s) failed: %s", own, strerror(errno));
            return 5;
        }
    }
    pool = thread_pool_create(&pattr, 0);
    if (!pool || thread_pool_start(pool) < 0)
        return 6;
    pthread_create(&pulses, 0, pulse_thread, 0);
    set_claimed(0);
    note("watching %s for %u s", g_mount, seconds);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!g_stop) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((unsigned)(t.tv_sec - start.tv_sec) >= seconds)
            break;
        alive = stock_entries(now, sizeof(now));
        if (strcmp(now, last)) {
            note("mount   %s: stock entries now [%s] (before [%s])", g_mount, now, last);
            strcpy(last, now);
        }
        if (alive && id < 0) {
            id = resmgr_attach(dpp, &rattr, g_mount, _FTYPE_ANY, _RESMGR_FLAG_DIR | _RESMGR_FLAG_BEFORE, &g_connect, &g_io,
                               &g_dir_attr);
            if (id < 0) {
                note("resmgr_attach(%s) failed: %s", g_mount, strerror(errno));
                break;
            }
            ++attaches;
            note("attached in front of the stock driver at %s (attach #%d)", g_mount, attaches);
            if (!self_test()) {
                resmgr_detach(dpp, id, _RESMGR_DETACH_PATHNAME);
                id = -1;
                note("stock driver not reachable through the tap: detached again, ending");
                break;
            }
        } else if (!alive && id >= 0) {
            resmgr_detach(dpp, id, _RESMGR_DETACH_PATHNAME);
            id = -1;
            set_claimed(0); /* the Bluetooth connection ended: the next one is decided again */
            note("stock driver left %s: name taken away, waiting for the next mount", g_mount);
        }
        usleep(15000);
    }
    if (id >= 0)
        resmgr_detach(dpp, id, _RESMGR_DETACH_PATHNAME);
    set_claimed(0);
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAXKEYS && g_keys[i].key[0]; ++i)
        fprintf(stderr, "summary %6u x %s\n", g_keys[i].count, g_keys[i].key);
    pthread_mutex_unlock(&g_lock);
    note("ended");
    return 0;
}
