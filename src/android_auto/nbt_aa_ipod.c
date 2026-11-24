/* nbt_aa_ipod: Android Auto as an iPod source of the head unit.
 *
 * A QNX resource manager that looks like the stock iPod driver (io-fs-media -dipod, "MediaFS") to the head unit:
 * the USB launcher starts it in place of that driver (see nbt_driver_stub.c / accessory_window.c), mme lists it as
 * an iPod media store, iDrive shows it as a source. What the head unit then asks an iPod (title, artist, album, length,
 * position, cover, play / next / previous) is answered from the running Android Auto session:
 *   <run>/media.state, <run>/media.cover   written by oneplus_native (now-playing data of the phone)
 *   <run>/input.fifo                       key presses to the phone ("K 87 1" = next down ...)
 * The sound does not pass through here: nbt_a2i_play plays it into /dev/io-amp/input_stereo_0, the amplifier input
 * the head unit routes while an iPod source is selected (sound.args "input_stereo_0 2").
 *
 * The protocol was written down on the car on 2026-11-14 with nbt_dir_mirror --forward in front of the stock
 * Bluetooth iPod driver (log .local/car-logs/20261113/ipod-fwd6-play.txt). Numbers below are what was seen there:
 *   devctl class 0x0e on .FS_info./control (stock player io-media-generic), .FS_info./current and
 *   .FS_info./playback/song~N (mme):
 *     0x64 song, 0x65 album, 0x66 artist, 0x67 genre, 0x68 composer: text, return value = length with the final 0
 *     0x6b length in ms as return value; 0x69, 0x6c, 0x6d "not supported"; 0x6a "not a control"
 *     0x0d play, 0x0e next, 0x0f previous; 0x13 / 0x15 read one byte (shuffle / repeat), 0x16 set repeat; 0x8c no data
 *     0x19 playback status, 16 x 32 bit: flags, state (1 playing, 2 paused), speed, number of the playing title in
 *          the playback list, the number before,
 *          length ms, position ms, 0, 0xffff, 0, 0xffffffff, change counter, repeat, 0...
 *     0x34 cover formats (return value = count; each 96 bytes: 16 x 0, width, height, bytes, 0, type[64])
 *     0x35 choose a cover format, 0x36 read the cover: 16 x 0 + the 96 byte format + the picture, in pieces of the
 *          asked size, return value = bytes of this piece (a short piece ends it)
 *     0x8b device description as XML, return value = its length
 *   messages (_IO_MSG, manager 0x13) of the HMI's media application on .FS_info./control:
 *     0x37 / 0x38 name of the playing app (32 bit length + text), 0x32 a list of numbers, 0x30 ok,
 *     0x0b "audiomode" + "analog" ok / "digital" refused (else the application starts its own USB sound path),
 *     0x80 see restart below; the rest "not supported"
 *   file system questions of the media application: mounted at / on, statistics ("ipod", read only)
 *
 * Usage: nbt_aa_ipod --mount /fs/ipod0 --run /fs/ram3/nbt-aa-native [--log file]
 * Only reads the two media files and writes key lines into input.fifo. Ends on SIGTERM; the mount goes with it.
 */
struct p_ocb;
#define IOFUNC_OCB_T struct p_ocb
#define RESMGR_OCB_T struct p_ocb
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
#include <sys/dcmd_blk.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

enum { N_ROOT, N_FSINFO, N_CONTROL, N_CURRENT, N_EAF, N_EVENTS, N_INFOXML, N_PLAYBACK, N_SONG, N_MUSIC, N_MUSICDIR };
/* The playback list (.FS_info./playback/song~N) works like the stock driver's: every title that starts playing is
 * added at the end, the playing one is the last, and the playback status names its number. The head unit looks the
 * title up by that number (car 2026-11-14: with a number that had no list entry, cover and skipping worked but no
 * title was shown). */
#define HISTORY 400 /* titles remembered; older numbers answer with the playing title */

struct p_ocb {
    iofunc_ocb_t hdr; /* must be first */
    int node, song;
    unsigned dirpos;
    size_t art_pos; /* cover read position of this open file */
    char rel[80];
};

static volatile sig_atomic_t g_stop;
static const char* g_mount;
static const char* g_run;
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_dir_attr, g_file_attr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_lines;
static volatile int g_restart; /* asked by message 0x80 */
static const char* g_usb;      /* --usb <bus>:<device> of the iPhone that stands in for us on the USB */
static int g_remount_ms;       /* --remount <ms>: answer message 0x80 by leaving the mount point and coming back */
static dispatch_t* g_dpp;
static volatile int g_id = -1;
static time_t g_last_remount;
static int g_verbose;          /* --verbose 1: every open, devctl and message is noted */
/* --takeover 1 (2026-11-18): the place is already held by the stock iPod driver, which the head unit started for a
 * real Apple device and announced by itself - the only way an iPod entry appears in iDrive, and it happens at every
 * start without anyone plugging anything while the device stays in the snap-in adapter. We then come in FRONT of that
 * driver at the same place, without the place ever disappearing (no "removed / inserted" for the head unit), and
 * keep showing the device description the stock driver showed (same name and serial number, so the media store stays
 * the same). The caller ends the stock driver afterwards. */
static int g_takeover;
static char g_stock_xml[4000]; /* info.xml as the stock driver served it */
static size_t g_stock_xml_len;

/* ---- what Android Auto is playing (under g_lock) ---- */
static struct {
    char title[200], artist[200], album[200], source[64];
    int playing;
    long position_s, duration_s;
    time_t stamp;      /* mtime of media.state: the moment "position" was true */
    time_t checked;
    unsigned meta;     /* grows with every change of title or picture */
    unsigned count;    /* entries in the playback list; the playing title is number count - 1 */
    unsigned changed;  /* flag bits for the next status read */
    unsigned repeat;
    unsigned char* cover;
    size_t cover_len;
    time_t cover_stamp;
    unsigned cover_w, cover_h;
    char cover_type[16];
} aa;
static struct {
    char title[120], artist[100], album[100];
} g_history[HISTORY];

static void note(const char* format, ...)
{
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    if (++g_lines > 20000)
        return;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void hex(char* out, size_t max, const unsigned char* p, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n && i < 64 && k + 4 < max; ++i)
        k += snprintf(out + k, max - k, "%02x ", p[i]);
    out[k] = 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void load_cover(void)
{
    char path[200];
    struct stat st;
    FILE* f;
    snprintf(path, sizeof(path), "%s/media.cover", g_run);
    if (stat(path, &st) || st.st_size < 32 || st.st_size > 4 * 1024 * 1024) {
        if (aa.cover_len)
            note("cover gone");
        aa.cover_len = 0;
        return;
    }
    if (st.st_mtime == aa.cover_stamp && (size_t)st.st_size == aa.cover_len)
        return;
    f = fopen(path, "rb");
    if (!f)
        return;
    free(aa.cover);
    aa.cover = malloc((size_t)st.st_size);
    aa.cover_len = aa.cover ? fread(aa.cover, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    aa.cover_stamp = st.st_mtime;
    aa.cover_w = aa.cover_h = 0;
    strcpy(aa.cover_type, "image/png");
    if (aa.cover_len > 24 && !memcmp(aa.cover, "\x89PNG", 4)) { /* size from the PNG header */
        aa.cover_w = (unsigned)aa.cover[16] << 24 | aa.cover[17] << 16 | aa.cover[18] << 8 | aa.cover[19];
        aa.cover_h = (unsigned)aa.cover[20] << 24 | aa.cover[21] << 16 | aa.cover[22] << 8 | aa.cover[23];
    } else if (aa.cover_len > 3 && aa.cover[0] == 0xff && aa.cover[1] == 0xd8)
        strcpy(aa.cover_type, "image/jpeg");
    note("cover %u bytes %s %ux%u", (unsigned)aa.cover_len, aa.cover_type, aa.cover_w, aa.cover_h);
    /* The phone sends the picture a moment after the title (often a small placeholder first). The head unit reads the
     * cover right at the title change, so it is told again that something changed (car 2026-11-14: covers "not
     * reliable"). */
    aa.changed |= 0xc0;
    ++aa.meta;
}

/* Reads media.state when it changed (looked at no more than twice a second). */
static void refresh(void)
{
    char path[200], line[400], title[200] = "", artist[200] = "", album[200] = "", source[64] = "";
    struct stat st;
    FILE* f;
    long position = 0, duration = 0;
    int state = 0;
    struct timespec now;
    static long last_ms;
    long ms;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
    if (last_ms && ms - last_ms < 500)
        return;
    last_ms = ms;
    load_cover();
    snprintf(path, sizeof(path), "%s/media.state", g_run);
    if (stat(path, &st) || st.st_mtime == aa.stamp)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
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
        else if (!strncmp(line, "source=", 7))
            snprintf(source, sizeof(source), "%s", line + 7);
        else if (!strncmp(line, "state=", 6))
            state = atoi(line + 6);
        else if (!strncmp(line, "position=", 9))
            position = atol(line + 9);
        else if (!strncmp(line, "duration=", 9))
            duration = atol(line + 9);
    }
    fclose(f);
    if (strcmp(title, aa.title) || strcmp(artist, aa.artist) || !aa.count) {
        const unsigned slot = aa.count % HISTORY;
        snprintf(g_history[slot].title, sizeof(g_history[slot].title), "%s", *title ? title : "Android Auto");
        snprintf(g_history[slot].artist, sizeof(g_history[slot].artist), "%s", artist);
        snprintf(g_history[slot].album, sizeof(g_history[slot].album), "%s", album);
        ++aa.count;
        ++aa.meta;
        aa.changed |= 0x80;
        note("title: %s / %s / %s (%ld s)", title, artist, album, duration);
    }
    if ((state == 2) != aa.playing) {
        aa.changed |= 0x80;
        note("state: %s", state == 2 ? "playing" : "paused");
    }
    strcpy(aa.title, title);
    strcpy(aa.artist, artist);
    strcpy(aa.album, album);
    strcpy(aa.source, source);
    aa.playing = state == 2;
    aa.position_s = position;
    aa.duration_s = duration;
    aa.stamp = st.st_mtime;
}

static void key(int code)
{
    char path[200], line[40];
    int fd, n;
    snprintf(path, sizeof(path), "%s/input.fifo", g_run);
    fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        note("key %d: no input.fifo (%s)", code, strerror(errno));
        return;
    }
    n = snprintf(line, sizeof(line), "K %d 1\n", code);
    if (write(fd, line, (size_t)n) != n)
        note("key %d: write failed", code);
    n = snprintf(line, sizeof(line), "K %d 0\n", code);
    if (write(fd, line, (size_t)n) != n)
        note("key %d: write failed", code);
    close(fd);
    note("key %d sent", code);
}

/* ---- the tree ---- */
static const char* const k_music[] = {"Albums", "Artists", "Audiobooks", "Composers", "Genres", "Playlists", "Podcasts", "Songs"};

static int is_dir(int node)
{
    return node == N_ROOT || node == N_FSINFO || node == N_PLAYBACK || node == N_MUSIC || node == N_MUSICDIR;
}

static int find(const char* path, int* song)
{
    unsigned i;
    *song = -1; /* the playing title */
    if (!*path)
        return N_ROOT;
    if (!strcmp(path, ".FS_info."))
        return N_FSINFO;
    if (!strcmp(path, "Music"))
        return N_MUSIC;
    if (!strncmp(path, "Music/", 6)) {
        for (i = 0; i < sizeof(k_music) / sizeof(*k_music); ++i)
            if (!strcmp(path + 6, k_music[i]))
                return N_MUSICDIR;
        return -1;
    }
    if (strncmp(path, ".FS_info./", 10))
        return -1;
    path += 10;
    if (!strcmp(path, "control"))
        return N_CONTROL;
    if (!strcmp(path, "current"))
        return N_CURRENT;
    if (!strcmp(path, "eaf"))
        return N_EAF;
    if (!strcmp(path, "events"))
        return N_EVENTS;
    if (!strcmp(path, "info.xml"))
        return N_INFOXML;
    if (!strcmp(path, "playback"))
        return N_PLAYBACK;
    if (!strncmp(path, "playback/song~", 14) || !strncmp(path, "playback/~", 10)) {
        const char* number = strchr(path, '~') + 1;
        if (*number < '0' || *number > '9' || !aa.count || (unsigned)atoi(number) >= aa.count + 8)
            return -1;
        /* numbers at or behind the end of the list are the playing title (the head unit reads around the number in
         * the playback status, see 0x0e19) */
        *song = (unsigned)atoi(number) >= aa.count ? (int)aa.count - 1 : atoi(number);
        return N_SONG;
    }
    return -1;
}

static const char* entry(int node, unsigned i, char* tmp, size_t max)
{
    static const char* const root[] = {".FS_info.", "Music"};
    static const char* const info[] = {"control", "current", "eaf", "events", "info.xml", "playback"};
    if (node == N_ROOT)
        return i < 2 ? root[i] : 0;
    if (node == N_FSINFO)
        return i < 6 ? info[i] : 0;
    if (node == N_MUSIC)
        return i < sizeof(k_music) / sizeof(*k_music) ? k_music[i] : 0;
    /* one entry more than titles: the status number (count) must lie inside the list; that last entry is the playing title */
    if (node == N_PLAYBACK && i < aa.count) {
        snprintf(tmp, max, "song~%u", i);
        return tmp;
    }
    return 0;
}

static size_t info_xml(char* out, size_t max)
{
    if (g_takeover && g_stock_xml_len && g_stock_xml_len < max) {
        memcpy(out, g_stock_xml, g_stock_xml_len);
        return g_stock_xml_len;
    }
    return (size_t)snprintf(out, max,
        "<?xml version=\"1.0\" standalone=\"yes\"?>\n<info>\n\t<media>\n\t\t<device>iPod</device>\n"
        "\t\t<protocol>\n\t\t\t<display_remote>NV</display_remote>\n\t\t\t<extended>NV</extended>\n"
        "\t\t\t<digital_audio>NV</digital_audio>\n\t\t\t<storage>NV</storage>\n\t\t</protocol>\n"
        "\t\t<name>Android Auto</name>\n\t\t<serial>NBTAA0001</serial>\n\t\t<swversion>27.0.1</swversion>\n"
        "\t\t<model>\n\t\t\t<id>0x00260000</id>\n\t\t\t<number>MG2L4QL</number>\n\t\t</model>\n"
        "\t\t<options>\n\t\t\t<value>0x40</value>\n\t\t</options>\n\t\t<audio>\n\t\t</audio>\n"
        "\t\t<albart>mediafs</albart>\n\t\t<display>\n"
        "\t\t\t<limit>\n\t\t\t\t<type>2</type>\n\t\t\t\t<format>le_rgb565</format>\n\t\t\t\t<height>76</height>\n\t\t\t\t<width>166</width>\n\t\t\t</limit>\n"
        "\t\t</display>\n\t\t<lastsync>2026-11-14 00:00:00</lastsync>\n\t</media>\n"
        "\t<fsys>\n\t\t<type>ipod</type>\n\t\t<mountpoint>%s</mountpoint>\n\t\t<mountdevice>file-2-ipod-8-media</mountdevice>\n"
        "\t\t<uuid>0D381341-0000-0040-8000-0000CA07DF11</uuid>\n\t</fsys>\n"
        "\t<device>\n\t\t<driver>ipod</driver>\n\t\t<catagory>media</catagory>\n"
        "\t\t<transport>\n\t\t\t<type>ser_ipod</type>\n\t\t\t<dev>/net/hu-intel.nbt/dev/IAP1</dev>\n\t\t</transport>\n"
        "\t\t<authcoproc>\n\t\t\t<type>i2c_ipod</type>\n\t\t\t<device>3</device>\n\t\t\t<firmware>1</firmware>\n"
        "\t\t\t<protocol>2.00</protocol>\n\t\t\t<devno>0x200</devno>\n\t\t\t<x509>\n\t\t\t\t<size>908</size>\n\t\t\t</x509>\n"
        "\t\t</authcoproc>\n\t</device>\n</info>\n",
        g_mount);
}

static const char* bundle(void)
{
    return !strcmp(aa.source, "Spotify") ? "com.spotify.client" : "com.google.android.projection.gearhead";
}

static size_t device_xml(char* out, size_t max)
{
    return (size_t)snprintf(out, max,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<device api_version=\"1\">\n"
        " <interface type=\"device\" path=\"/net/hu-intel.nbt/dev/IAP1\"/>\n <media>\n  <iPod>\n   <version>\n"
        "    <model_id value=\"00260000\" valuetype=\"hex\"/>\n    <model_number value=\"MG2L4QL\" valuetype=\"text\"/>\n"
        "    <firmware value=\"1B0001\" valuetype=\"hex\"/>\n   </version>\n   <capabilities>\n    <cta/>\n    <genius/>\n   </capabilities>\n"
        "   <transport value=\"ser_ipod\" valuetype=\"text\"/>\n"
        "   <NowPlayingApplicationBundleName value=\"%s\" valuetype=\"text\"/>\n"
        "   <NowPlayingApplicationDisplayName value=\"%s\" valuetype=\"text\"/>\n"
        "   <shuffle value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,tracks,albums\"/>\n"
        "   <repeat value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,one,all\"/>\n"
        "   <resumemusicapp value=\"off\" valuetype=\"enum\" modifiable=\"yes\" alternatives=\"off,on\"/>\n"
        "   <preferences>\n"
        "    <video value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,on,ask\"/>\n"
        "    <screen value=\"fill\" valuetype=\"enum\" modifiable=\"yes\" alternatives=\"fill,fit\"/>\n"
        "    <format value=\"ntsc\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"ntsc,pal\"/>\n"
        "    <lineout value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,on\"/>\n"
        "    <connection value=\"none\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"none,composite,svideo,component\"/>\n"
        "    <caption value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,on\"/>\n"
        "    <ratio value=\"full\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"full,wide\"/>\n"
        "    <subtitle value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,on\"/>\n"
        "    <audioalt value=\"off\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"off,on\"/>\n"
        "   </preferences>\n   <displayimage>\n    <upload value=\"on\" valuetype=\"enum\" modifiable=\"no\" alternatives=\"on\"/>\n"
        "   </displayimage>\n  </iPod>\n </media>\n</device>\n",
        bundle(), *aa.source ? aa.source : "Android Auto");
}

/* ---- resource manager ---- */
static int p_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    char path[80];
    struct p_ocb* o;
    iofunc_attr_t* attr;
    size_t n;
    int node, song, r;
    (void)handle;
    (void)extra;
    if (strlen(msg->connect.path) >= sizeof(path))
        return ENOENT;
    strcpy(path, msg->connect.path);
    n = strlen(path);
    while (n && path[n - 1] == '/')
        path[--n] = 0;
    pthread_mutex_lock(&g_lock);
    refresh(); /* the playback list may have grown */
    node = find(path, &song);
    pthread_mutex_unlock(&g_lock);
    if (node < 0) {
        note("open-missing %s pid %d", path, ctp->info.pid);
        return ENOENT;
    }
    o = calloc(1, sizeof(*o));
    if (!o)
        return ENOMEM;
    o->node = node;
    o->song = song;
    snprintf(o->rel, sizeof(o->rel), "%s", *path ? path : "/");
    attr = is_dir(node) ? &g_dir_attr : &g_file_attr;
    iofunc_attr_lock(attr);
    r = iofunc_ocb_attach(ctp, msg, &o->hdr, attr, &g_io);
    iofunc_attr_unlock(attr);
    if (r != EOK) {
        free(o);
        return r;
    }
    if (g_verbose)
        note("open %s pid %d flags %x", o->rel, ctp->info.pid, msg->connect.ioflag);
    return EOK;
}

static void forget_armed(struct p_ocb* o);

static int p_close(resmgr_context_t* ctp, void* reserved, RESMGR_OCB_T* o)
{
    forget_armed(o);
    return iofunc_close_ocb_default(ctp, reserved, &o->hdr);
}

static int p_stat(resmgr_context_t* ctp, io_stat_t* msg, RESMGR_OCB_T* o)
{
    char tmp[4096];
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_ino = (ino_t)(o->node * 16 + o->song + 1);
    msg->o.st_mode = is_dir(o->node) ? S_IFDIR | 0555 : o->node == N_INFOXML || o->node == N_SONG ? S_IFREG | 0444 : S_IFREG | 0666;
    msg->o.st_nlink = 1;
    msg->o.st_size = is_dir(o->node) ? 512 : o->node == N_INFOXML ? (off_t)info_xml(tmp, sizeof(tmp)) : 0;
    msg->o.st_blksize = 512;
    msg->o.st_mtime = msg->o.st_atime = msg->o.st_ctime = time(0);
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int p_lseek(resmgr_context_t* ctp, io_lseek_t* msg, RESMGR_OCB_T* o)
{
    char tmp[4096];
    /* mme measures info.xml by seeking to its end ("Loading xml, seek/read failure" without that, car 2026-11-14) */
    const off64_t size = o->node == N_INFOXML ? (off64_t)info_xml(tmp, sizeof(tmp)) : 0;
    off64_t target = msg->i.whence == SEEK_CUR   ? (off64_t)o->hdr.offset + msg->i.offset
                     : msg->i.whence == SEEK_END ? size + msg->i.offset
                                                 : msg->i.offset;
    if (target < 0)
        return EINVAL;
    if (is_dir(o->node)) {
        o->dirpos = 0;
        target = 0;
    }
    o->hdr.offset = target;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = target;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int p_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* o)
{
    const int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    size_t want = msg->i.nbytes, used = 0;
    char* buf;
    int status = iofunc_read_verify(ctp, msg, &o->hdr, 0);
    if (status != EOK)
        return status;
    if (want > 16384)
        want = 16384;
    buf = calloc(1, want ? want : 1);
    if (!buf)
        return ENOMEM;
    if (is_dir(o->node)) {
        char tmp[32];
        const char* name;
        while ((name = entry(o->node, o->dirpos, tmp, sizeof(tmp)))) {
            const size_t len = strlen(name);
            const size_t rec = (offsetof(struct dirent, d_name) + len + 1 + 7) & ~(size_t)7;
            struct dirent* d = (struct dirent*)(buf + used);
            if (used + rec > want)
                break;
            d->d_ino = o->dirpos + 1;
            d->d_offset = o->dirpos + 1;
            d->d_reclen = (short)rec;
            d->d_namelen = (short)len;
            memcpy(d->d_name, name, len + 1);
            used += rec;
            ++o->dirpos;
        }
    } else if (o->node == N_INFOXML) {
        char xml[4096];
        const size_t len = info_xml(xml, sizeof(xml));
        off64_t at = o->hdr.offset;
        if (xtype == _IO_XTYPE_OFFSET)
            at = ((struct _xtype_offset*)(&msg->i + 1))->offset;
        if (at >= 0 && (size_t)at < len) {
            used = len - (size_t)at < want ? len - (size_t)at : want;
            memcpy(buf, xml + at, used);
        }
        if (xtype != _IO_XTYPE_OFFSET)
            o->hdr.offset = at + (off64_t)used;
    }
    MsgReply(ctp->rcvid, (int)used, buf, used);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int p_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* o)
{
    note("write %s pid %d bytes %u (ignored)", o->rel, ctp->info.pid, (unsigned)msg->i.nbytes);
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return _RESMGR_NPARTS(0);
}

static int reply_text(resmgr_context_t* ctp, io_devctl_t* msg, const char* text)
{
    const unsigned room = msg->i.nbytes;
    unsigned n = (unsigned)strlen(text) + 1;
    char* out = (char*)_DEVCTL_DATA(msg->o);
    if (n > room) {
        n = room;
        if (!n)
            return EINVAL;
    }
    memset(&msg->o, 0, sizeof(msg->o));
    memcpy(out, text, n);
    out[n - 1] = 0;
    msg->o.ret_val = (int)n;
    msg->o.nbytes = n;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

static int reply_bytes(resmgr_context_t* ctp, io_devctl_t* msg, const void* data, unsigned n, int ret)
{
    memset(&msg->o, 0, sizeof(msg->o));
    if (n)
        memcpy(_DEVCTL_DATA(msg->o), data, n);
    msg->o.ret_val = ret;
    msg->o.nbytes = n;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

/* one cover format entry (96 bytes) */
static void art_entry(unsigned char* e)
{
    uint32_t v[4];
    memset(e, 0, 96);
    v[0] = aa.cover_w;
    v[1] = aa.cover_h;
    v[2] = (uint32_t)aa.cover_len;
    v[3] = 0;
    memcpy(e + 16, v, 16);
    strcpy((char*)e + 32, aa.cover_type);
}

static int p_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* o)
{
    const unsigned dcmd = (unsigned)msg->i.dcmd;
    const unsigned nbytes = msg->i.nbytes;
    const unsigned number = dcmd & 0xffffu; /* class and command */
    int r = -1;

    if (g_verbose && number != 0x0e19 && number != 0x0e36)
        note("devctl %s pid %d dcmd %08x nbytes %u", o->rel, ctp->info.pid, dcmd, nbytes);
    else if (g_verbose) { /* polled ones: noted the first time only */
        static unsigned seen19, seen36;
        unsigned* seen = number == 0x0e19 ? &seen19 : &seen36;
        if (!(*seen)++)
            note("devctl %s pid %d dcmd %08x nbytes %u (first of many)", o->rel, ctp->info.pid, dcmd, nbytes);
    }
    if ((dcmd & 0xff00u) == 0x0100u) { /* flags of the open file; "mount flags": read only */
        if (dcmd == 0x40040103u) {
            const int ro = 1;
            return reply_bytes(ctp, msg, &ro, sizeof(ro), 0);
        }
        return iofunc_devctl_default(ctp, msg, &o->hdr);
    }
    if (dcmd == (unsigned)DCMD_FSYS_STATVFS) {
        struct statvfs v;
        memset(&v, 0, sizeof(v));
        v.f_bsize = v.f_frsize = 512;
        v.f_files = 1024;
        v.f_ffree = v.f_favail = 996;
        v.f_fsid = 1;
        v.f_namemax = 1024;
        v.f_flag = ST_RDONLY;
        strcpy(v.f_basetype, "ipod");
        return reply_bytes(ctp, msg, &v, sizeof(v), 0);
    }
    if (dcmd == (unsigned)DCMD_FSYS_MOUNTED_ON || dcmd == (unsigned)DCMD_FSYS_MOUNTED_AT) {
        char text[256];
        memset(text, 0, sizeof(text));
        if (dcmd == (unsigned)DCMD_FSYS_MOUNTED_ON)
            strcpy(text, "/net/hu-intel.nbt/dev/blk/file-2-ipod-8-media");
        else
            snprintf(text, sizeof(text), "/net/hu-intel.nbt%s/", g_mount);
        return reply_bytes(ctp, msg, text, nbytes < 256 ? nbytes : 256, 0);
    }
    if (dcmd == (unsigned)DCMD_FSYS_MOUNTED_BY)
        return ENODEV;
    if (number == 0x0216) { /* "how many entries" of a folder: the playback list answers, the library folders do not */
        if (o->node == N_PLAYBACK)
            return reply_bytes(ctp, msg, 0, 0, (int)aa.count);
        return ENOSYS;
    }
    if ((dcmd & 0xff00u) != 0x0e00u)
        return ENOTTY;

    pthread_mutex_lock(&g_lock);
    refresh();
    switch (number) {
    case 0x0e8b: { /* device description */
        char xml[4096];
        const unsigned len = (unsigned)device_xml(xml, sizeof(xml));
        r = reply_bytes(ctp, msg, xml, len < nbytes ? len : nbytes, (int)len);
        break;
    }
    case 0x0e64: /* song */
    case 0x0e65: /* album */
    case 0x0e66: { /* artist */
        /* Every list entry answers with the PLAYING title (car 2026-11-24: which entry iDrive shows depends on mme's own
         * counting - two songs behind, or nothing - so no entry may hold an old title). */
        const unsigned n = aa.count ? aa.count - 1 : 0;
        const unsigned slot = n % HISTORY;
        r = reply_text(ctp, msg, number == 0x0e64 ? (*g_history[slot].title ? g_history[slot].title : "Android Auto")
                                 : number == 0x0e65 ? g_history[slot].album : g_history[slot].artist);
        break;
    }
    case 0x0e67: /* genre */
    case 0x0e68: /* composer */
        r = reply_text(ctp, msg, "");
        break;
    case 0x0e6b: /* length in ms */
        r = reply_bytes(ctp, msg, 0, 0, (int)(aa.duration_s * 1000));
        break;
    case 0x0e69: { /* release date: the stock driver answers it (48 bytes), so do we - with nothing in it. Refused,
                    * the title data reached mme's table but not the display (car 2026-11-14). */
        unsigned char empty[48];
        memset(empty, 0, sizeof(empty));
        r = reply_bytes(ctp, msg, empty, nbytes < 48 ? nbytes : 48, 1);
        break;
    }
    case 0x0e6c:
    case 0x0e6d:
        r = ENOTSUP;
        break;
    case 0x0e6a:
        r = ENOTTY;
        break;
    case 0x0e0d: /* play */
        note("play asked by pid %d (%s)", ctp->info.pid, aa.playing ? "already playing" : "paused: toggling");
        if (!aa.playing)
            key(85);
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e0c: /* sent by the head unit's two-wire player a few seconds after play (car 2026-11-23); refused, the
                  * player gave up ("mediafs module does not support this operation") and the title display stayed
                  * empty. Most likely pause or stop. Answered "done" without touching the phone: pausing on a guess
                  * would stop the music the user just started. */
        note("command 0x0c (pause/stop?) asked by pid %d - answered, nothing sent", ctp->info.pid);
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e0e: /* next */
        note("next asked by pid %d", ctp->info.pid);
        key(87);
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e0f: /* previous */
        note("previous asked by pid %d", ctp->info.pid);
        key(88);
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e8c:
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e13: { /* shuffle */
        const unsigned char b = 0;
        r = reply_bytes(ctp, msg, &b, 1, 0);
        break;
    }
    case 0x0e15: { /* repeat */
        const unsigned char b = (unsigned char)aa.repeat;
        r = reply_bytes(ctp, msg, &b, 1, 0);
        break;
    }
    case 0x0e16:
        if (nbytes >= 1)
            aa.repeat = *(unsigned char*)_DEVCTL_DATA(msg->i);
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e19: { /* playback status */
        uint32_t s[16];
        long elapsed = aa.position_s * 1000;
        if (aa.playing) {
            const long since = (long)(time(0) - aa.stamp) * 1000;
            if (since > 0)
                elapsed += since;
        }
        if (aa.duration_s > 0 && elapsed > aa.duration_s * 1000)
            elapsed = aa.duration_s * 1000;
        memset(s, 0, sizeof(s));
        s[0] = 0x110u | aa.changed;
        s[1] = aa.playing ? 1 : 2;
        s[2] = 1;
        /* Number of the playing title. Car 2026-11-24 00:36: with count-2 here iDrive showed the title from two songs
         * before, i.e. the display takes entry N-1; mme also reads song~(N+1) (real iPhone: 0x19e -> song~415), which
         * the list answers with the playing title. Our playing title is entry count-1, so N = count. */
        s[3] = aa.count > 1 ? aa.count - 2 : 0;
        s[4] = aa.count > 2 ? aa.count - 3 : 0;
        s[5] = (uint32_t)(aa.duration_s * 1000);
        s[6] = (uint32_t)elapsed;
        s[8] = 0xffff;
        s[10] = 0xffffffffu;
        s[11] = aa.meta + 3;
        s[12] = aa.repeat;
        aa.changed = 0;
        r = reply_bytes(ctp, msg, s, nbytes < sizeof(s) ? nbytes : (unsigned)sizeof(s), 0);
        break;
    }
    case 0x0e34: { /* cover formats */
        unsigned char e[96];
        if (!aa.cover_len) { /* every list entry shows the playing title's cover, like its text */
            r = reply_bytes(ctp, msg, 0, 0, 0);
            break;
        }
        art_entry(e);
        r = reply_bytes(ctp, msg, e, nbytes >= 96 ? 96 : 0, 1);
        break;
    }
    case 0x0e35: /* choose a cover format: the reading starts again */
        o->art_pos = 0;
        r = reply_bytes(ctp, msg, 0, 0, 0);
        break;
    case 0x0e36: { /* read the cover: 16 x 0, the format entry, the picture */
        const size_t total = 112 + aa.cover_len;
        unsigned char head[112];
        unsigned char* out;
        size_t n, done = 0;
        if (!aa.cover_len) { /* every list entry shows the playing title's cover, like its text */
            r = ENOENT;
            break;
        }
        n = o->art_pos < total ? total - o->art_pos : 0;
        if (n > nbytes)
            n = nbytes;
        memset(&msg->o, 0, sizeof(msg->o));
        out = (unsigned char*)_DEVCTL_DATA(msg->o);
        if (o->art_pos < 112) {
            memset(head, 0, sizeof(head));
            art_entry(head + 16);
            done = 112 - o->art_pos < n ? 112 - o->art_pos : n;
            memcpy(out, head + o->art_pos, done);
        }
        if (n > done)
            memcpy(out + done, aa.cover + (o->art_pos + done - 112), n - done);
        if (!o->art_pos)
            note("cover read by pid %d starts (%u bytes)", ctp->info.pid, (unsigned)total);
        o->art_pos += n;
        msg->o.ret_val = (int)n;
        msg->o.nbytes = (unsigned)n;
        r = _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
        break;
    }
    default: {
        char text[200];
        hex(text, sizeof(text), (const unsigned char*)_DEVCTL_DATA(msg->i), nbytes);
        note("devctl-unknown %s pid %d dcmd %08x nbytes %u: %s", o->rel, ctp->info.pid, dcmd, nbytes, text);
        r = ENOTSUP;
    }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* Notifications. Recorded from the stock Bluetooth iPod driver with a real iPhone (car 2026-11-23 22:10, nbt_ipod_tap):
 * the head unit's player (io-media-generic) arms ionotify on .FS_info./control (action 3, flags 0x40000000) and the
 * driver fires it every 500 ms while playing and at once on a change; each time the player reads the playback status
 * (0x0e19) and, when its flags say "title changed" (0x80), title / artist / album. Without these events the player
 * never asked again - the reason why our title never reached the display (the old forwarding mirror of 2026-11-14 could
 * not pass events on, so it looked as if the stock driver sent none). */
#define MAXARMED 32
static struct {
    int used, rcvid;
    struct sigevent event;
    struct p_ocb* o;
} g_armed[MAXARMED];

static int p_notify(resmgr_context_t* ctp, io_notify_t* msg, RESMGR_OCB_T* o)
{
    const int action = (int)msg->i.action;
    static unsigned logged;
    if (g_verbose && logged++ < 20)
        note("notify %s pid %d action %d flags %x", o->rel, ctp->info.pid, action, (unsigned)msg->i.flags);
    /* only the player's control file gets these events; the stock driver never fired the media application's
     * notification on .FS_info./eaf (it polls that every 10 s by itself; fired, it asked every second) */
    if ((action == _NOTIFY_ACTION_POLLARM || action == _NOTIFY_ACTION_TRANARM) && o->node == N_CONTROL) {
        int i;
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < MAXARMED; ++i) /* one entry per open file: a new arm replaces the old one */
            if (g_armed[i].used && g_armed[i].o == o)
                break;
        if (i == MAXARMED)
            for (i = 0; i < MAXARMED && g_armed[i].used; ++i) {
            }
        if (i < MAXARMED) {
            g_armed[i].used = 1;
            g_armed[i].rcvid = ctp->rcvid;
            g_armed[i].event = msg->i.event;
            g_armed[i].o = o;
        }
        pthread_mutex_unlock(&g_lock);
    }
    msg->o.flags = 0;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static void forget_armed(struct p_ocb* o)
{
    int i;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAXARMED; ++i)
        if (g_armed[i].used && g_armed[i].o == o)
            g_armed[i].used = 0;
    pthread_mutex_unlock(&g_lock);
}

/* main loop, every 200 ms: fire the armed notifications every 500 ms while playing, at once after a change */
static void notify_tick(void)
{
    static struct timespec last;
    struct timespec now;
    long ms;
    int i, fire;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - last.tv_sec) * 1000L + (now.tv_nsec - last.tv_nsec) / 1000000L;
    pthread_mutex_lock(&g_lock);
    refresh();
    fire = aa.changed || (aa.playing && ms >= 480);
    if (fire) {
        last = now;
        for (i = 0; i < MAXARMED; ++i)
            if (g_armed[i].used) {
                MsgDeliverEvent(g_armed[i].rcvid, &g_armed[i].event);
                g_armed[i].used = 0;
            }
    }
    pthread_mutex_unlock(&g_lock);
}

static int reply_msg(resmgr_context_t* ctp, const void* data, size_t n)
{
    size_t room = (size_t)ctp->info.dstmsglen;
    unsigned char* back = calloc(1, room ? room : 1);
    if (!back)
        return ENOMEM;
    memcpy(back, data, n < room ? n : room);
    MsgReply(ctp->rcvid, 0, back, room);
    free(back);
    return _RESMGR_NOREPLY;
}

static int p_msg(resmgr_context_t* ctp, io_msg_t* msg, RESMGR_OCB_T* o)
{
    const unsigned char* m = (const unsigned char*)msg;
    const int len = ctp->info.msglen;
    unsigned sub;
    char text[200];
    int r;
    if (len < 8 || m[4] != 0x13)
        return ENOSYS;
    sub = m[6];
    if (g_verbose) {
        hex(text, sizeof(text), m + 8, (size_t)len - 8);
        note("msg %s pid %d sub %02x room %d: %s", o->rel, ctp->info.pid, sub, (int)ctp->info.dstmsglen, text);
    }
    pthread_mutex_lock(&g_lock);
    refresh();
    if (o->node == N_EAF) {
        if (sub == 0x05) {
            /* as the stock Bluetooth iPod driver answered (car 2026-11-23): 2,5,2,0,0,6,2 */
            static const unsigned char table[28] = {2, 0, 0, 0, 5, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 2, 0, 0, 0};
            r = reply_msg(ctp, table, sizeof(table));
        } else
            r = EINVAL;
    } else if (sub == 0x37 || sub == 0x38) {
        unsigned char out[1028];
        const char* name = sub == 0x37 ? bundle() : *aa.source ? aa.source : "Android Auto";
        /* stock: bundle name length without the 0 ("com.spotify.client" -> 18), display name with it ("Spotify" -> 8) */
        const uint32_t n = (uint32_t)strlen(name) + (sub == 0x38 ? 1 : 0);
        memset(out, 0, sizeof(out));
        memcpy(out, &n, 4);
        memcpy(out + 4, name, strlen(name) + 1);
        r = reply_msg(ctp, out, 4 + n);
    } else if (sub == 0x32) {
        static const unsigned char list[16] = {5, 0, 0, 0, 0, 0, 2, 0, 4, 0, 5, 0, 7, 0, 0, 0};
        r = reply_msg(ctp, list, sizeof(list));
    } else if (sub == 0x30)
        r = reply_msg(ctp, "", 0);
    else if (sub == 0x0b) { /* "audiomode" "analog" | "digital" */
        const char* value = len > 34 ? (const char*)m + 34 : "";
        note("audiomode %.12s asked", value);
        /* Both are refused, as the stock Bluetooth iPod driver refuses "digital": accepted, "digital" makes the
         * application start its own USB sound path, and "analog" made it wait for sound on the analogue input - no
         * playback list, no sound, "unknown title" (car 2026-11-14 03:17). Refused, mme plays the store with its
         * two-wire player and our sound on input_stereo_0 is heard. */
        r = EBADF;
    } else if (sub == 0x80) {
        /* After a plug-in the media application first sends this and then waits for the device to register again on
         * the USB; only on that second arrival does it ask everything and list the source (car 2026-11-14: without
         * it the source appeared on every second plug-in only; refusing, answering or mounting again did not help).
         * So the iPhone that stands in for us is made to register again (nbt_usb_reset); the USB launcher then ends
         * this program and starts a new one. */
        note("message 0x80 from pid %d: %s", ctp->info.pid, g_usb ? "USB restart of the stand-in device" : "remount");
        /* Recorded from a real iPhone over Bluetooth (car 2026-11-23 22:10): the stock driver takes its mount point
         * away BEFORE it answers 0x80 (status 0, no data), the media application's next request already fails with
         * EBADF, and 2 s later the mount is back with the full description; then the application asks everything. */
        if (!g_usb && g_remount_ms > 0 && g_id >= 0 && (!g_last_remount || time(0) - g_last_remount >= 60)) {
            g_last_remount = time(0);
            resmgr_detach(g_dpp, g_id, _RESMGR_DETACH_PATHNAME);
            g_id = -1;
            note("remount: %s taken away, back in %d ms", g_mount, g_remount_ms);
        }
        g_restart = 1;
        r = reply_msg(ctp, "", 0);
    } else if (sub == 0x40) {
        /* Argument 1 = "name of your sound device": the media application then opens "snd:<answer>#PCM In" with its
         * own player (graph ipodstreamer) to carry the iPod's USB sound to the amplifier. Seen on the car 2026-11-14,
         * when the title was answered here by mistake: "Couldn't open audio device 'Cryin''". We have no such device
         * and need none: refused (as the stock Bluetooth driver does), the application leaves the playing to mme's
         * two-wire player, and our own sound on input_stereo_0 is what is heard. */
        r = ENOTSUP;
    } else if (sub == 0x41) {
        hex(text, sizeof(text), m + 8, (size_t)len - 8);
        note("message 0x41: %s", text);
        r = EINVAL;
    } else if (sub == 0x23 || sub == 0x20 || sub == 0x25)
        r = ENOTSUP;
    else {
        hex(text, sizeof(text), m, (size_t)len);
        note("msg-unknown %s pid %d length %d reply room %d: %s", o->rel, ctp->info.pid, len, (int)ctp->info.dstmsglen, text);
        r = ENOTSUP;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

int main(int argc, char** argv)
{
    const char* log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    struct sigaction sa;
    int i, id;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mount"))
            g_mount = argv[i + 1];
        else if (!strcmp(argv[i], "--run"))
            g_run = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else if (!strcmp(argv[i], "--usb"))
            g_usb = argv[i + 1];
        else if (!strcmp(argv[i], "--remount"))
            g_remount_ms = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--takeover"))
            g_takeover = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--verbose"))
            g_verbose = atoi(argv[i + 1]);
        else
            return 2;
    }
    /* /fs/ipodN: the place of a USB iPod (needs the USB launcher's announcement, i.e. an Apple device on the USB).
     * /fs/btipod: the place of an iPod connected by Bluetooth - the head unit lists what appears there without any
     * USB device (trial 2026-11-16; only while no iPhone is connected by Bluetooth, the stock driver uses it then). */
    /* /fs/avrcpN (2026-11-23): the Bluetooth audio entry of a phone; only with --takeover (we stand in front of the
     * stock Bluetooth audio driver, which stays underneath and takes over again when we end). */
    if (!g_mount || !g_run ||
        (strncmp(g_mount, "/fs/ipod", 8) && strcmp(g_mount, "/fs/btipod") && strncmp(g_mount, "/fs/avrcp", 9)) ||
        (!strncmp(g_mount, "/fs/avrcp", 9) && !g_takeover) || strlen(g_mount) > 12 ||
        strlen(g_run) > 100)
        return 2;
    if (log && !freopen(log, "a", stderr))
        return 2;
    if (g_takeover) { /* read the stock driver's description before we stand in front of it */
        char path[64];
        FILE* f;
        snprintf(path, sizeof(path), "%s/.FS_info./info.xml", g_mount);
        f = fopen(path, "rb");
        g_stock_xml_len = f ? fread(g_stock_xml, 1, sizeof(g_stock_xml) - 1, f) : 0;
        if (f)
            fclose(f);
        if (g_stock_xml_len < 100 || !strstr(g_stock_xml, "<serial>")) {
            note("takeover: no stock iPod driver at %s (info.xml: %u bytes), nothing done", g_mount, (unsigned)g_stock_xml_len);
            return 3;
        }
        note("takeover: stock description read, %u bytes", (unsigned)g_stock_xml_len);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    aa.changed = 0xc0;
    /* list entry 0 is a placeholder, so that a real title never has the number 0 (the head unit logs "expected 0"
     * for titles it does not want) */
    strcpy(g_history[0].title, "Android Auto");
    aa.count = 1;
    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 70000;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = p_open;
    g_io.read = p_read;
    g_io.write = p_write;
    g_io.lseek = p_lseek;
    g_io.stat = p_stat;
    g_io.devctl = p_devctl;
    g_io.close_ocb = p_close;
    g_io.notify = p_notify;
    g_io.msg = p_msg;
    iofunc_attr_init(&g_dir_attr, S_IFDIR | 0777, 0, 0);
    iofunc_attr_init(&g_file_attr, S_IFREG | 0777, 0, 0);
    id = resmgr_attach(dpp, &rattr, g_mount, _FTYPE_ANY, _RESMGR_FLAG_DIR | (g_takeover ? _RESMGR_FLAG_BEFORE : 0), &g_connect,
                       &g_io, &g_dir_attr);
    if (id < 0) {
        fprintf(stderr, "resmgr_attach(%s) failed: %s\n", g_mount, strerror(errno));
        return 5;
    }
    g_dpp = dpp;
    g_id = id;
    note("attached %s, data from %s", g_mount, g_run);
    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.context_free = dispatch_context_free;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.lo_water = 2;
    pattr.hi_water = 4;
    pattr.increment = 1;
    pattr.maximum = 8;
    pool = thread_pool_create(&pattr, 0);
    if (!pool || thread_pool_start(pool) < 0) {
        resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
        return 6;
    }
    while (!g_stop) {
        usleep(200000);
        notify_tick();
        if (g_restart) {
            int bus = -1, dev = -1;
            g_restart = 0;
            if (g_usb && sscanf(g_usb, "%d:%d", &bus, &dev) == 2 && bus >= 0 && bus < 8 && dev > 0 && dev < 128) {
                char cmd[300];
                snprintf(cmd, sizeof(cmd),
                         "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib:/proc/boot /fs/ram3/aatool/nbt_usb_reset %d %d >> /fs/ram3/aatool/reset.log 2>&1",
                         bus, dev);
                /* never twice within a minute: if the HMI asked again right after the fresh arrival, restarting once
                 * more would only go round in circles */
                struct stat st;
                const char* stamp = "/fs/ram3/aatool/reset.stamp";
                if (!stat(stamp, &st) && time(0) - st.st_mtime < 60) {
                    note("USB restart: skipped, the last one was %ld s ago", (long)(time(0) - st.st_mtime));
                    continue;
                }
                unlink(stamp);
                close(open(stamp, O_WRONLY | O_CREAT, 0644));
                usleep(300000);
                note("USB restart: exit code %d", system(cmd));
            } else if (g_remount_ms > 0) {
                /* Bluetooth place: the mount point was taken away in the 0x80 handler (as the stock driver does);
                 * come back after the pause (stock: 2 s) */
                if (g_id >= 0) {
                    note("remount: skipped, the last one was %ld s ago", (long)(time(0) - g_last_remount));
                    continue;
                }
                usleep((useconds_t)g_remount_ms * 1000u);
                id = resmgr_attach(dpp, &rattr, g_mount, _FTYPE_ANY, _RESMGR_FLAG_DIR | (g_takeover ? _RESMGR_FLAG_BEFORE : 0),
                                   &g_connect, &g_io, &g_dir_attr);
                if (id < 0) {
                    note("remount: resmgr_attach(%s) failed: %s", g_mount, strerror(errno));
                    break;
                }
                g_id = id;
                note("remount: attached %s again", g_mount);
            } else
                note("USB restart: no --usb given");
        }
    }
    if (id < 0)
        return 7;
    if (g_id >= 0)
        resmgr_detach(dpp, g_id, _RESMGR_DETACH_ALL);
    note("detached %s", g_mount);
    return 0;
}
