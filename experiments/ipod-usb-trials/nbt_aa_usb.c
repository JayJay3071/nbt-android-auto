/* nbt_aa_usb: Android Auto as a "USB" medium of the stock media system (QNX resource manager).
 *
 * Why (car 2026-11-13): the stock media management (mcd) reports every mount that appears under /fs/usb* as an inserted
 * USB medium, mme reads its files into its database, and when mme plays such a file the HMI shows title and artist (also
 * in the instrument cluster) and the title list can be scrolled with the steering wheel. So Android Auto's music is
 * offered as files of such a medium and played by the STOCK player:
 *
 *   /fs/usb0/01.flac   "previous title"   } all three carry the same live sound of the phone (media sink, 48 kHz
 *   /fs/usb0/02.flac   the current song   } stereo), as an endless FLAC stream of uncompressed ("verbatim") frames;
 *   /fs/usb0/03.flac   "next title"       } only the tags differ. 02 has the phone's title, artist, album and cover.
 *
 * The control script (scripts/nbt-aa-usb-ctl.sh) tells mme to read the files, builds a track session of the three and
 * plays 02. When the stock player starts reading 01 or 03 continuously (the driver picked the previous / next entry),
 * the script sends MEDIA_PREVIOUS / MEDIA_NEXT to the phone; when the phone's song changes, this program rewrites the
 * tags and the script starts over with the new 02.
 *
 * Inputs: the PCM FIFO of the session (audio.fifo, read here instead of by nbt_a2i_play) and media.state / media.cover
 * written by the session (aa_video_capture.cpp). Output for the script: <dir>/usb.state, rewritten 4 times a second:
 *   gen       counts the tag changes (a new value = the files have new tags)
 *   aa_state  playback state of the phone (2 = playing), aa_track = its song counter
 *   pcm_age   milliseconds since the last sound arrived
 *   tN_bytes  sound bytes file N has delivered without a pause of more than a second, tN_age = ms since its last read
 *
 * A reader never waits long: with no sound from the phone a file delivers silence (the first second at once, so that
 * mme's metadata readers are not slowed down, then about real time). A reader that still has the file open when the
 * tags change gets end-of-file. Nothing is ever written to the medium; devctl answers make it look like a FAT stick
 * with the label "Android Auto".
 *
 * Usage: nbt_aa_usb --mount /fs/usb0 --dir <runtime dir> [--log <file>]
 *   <runtime dir> holds audio.fifo, media.state, media.cover; usb.state is written there.
 * Runs until SIGTERM; the mount disappears when the program ends (for mme: the medium was removed).
 */
struct aa_ocb;
#define IOFUNC_OCB_T struct aa_ocb
#define RESMGR_OCB_T struct aa_ocb
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dcmd_blk.h>
#include <sys/dcmd_cam.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define TRACKS 40                        /* entries of the ring (01.flac .. 40.flac) */
#define FIRST 5                          /* entries before this one start as "<", the others as ">" */
#define BLOCK 1024                       /* samples per FLAC frame (21 ms at 48 kHz) */
#define BLOCK_BYTES (BLOCK * 4)          /* the same as 16 bit stereo PCM */
#define FRAME_MAX (16 + 2 * (1 + BLOCK * 2) + 2)
#define RING (1u << 20)                  /* PCM ring, about 5 s */
#define MAX_LAG (48000u * 4u * 6u / 10u) /* a reader more than 0.6 s behind jumps to 0.1 s behind */
#define KEEP_LAG (48000u * 4u / 10u)
#define COVER_MAX (1024 * 1024)
#define FILE_SPAN 0x30000000             /* size reported beyond the header: about 70 minutes of sound */

struct track {
    char name[16];
    iofunc_attr_t attr;
    unsigned char* header; /* "fLaC" + metadata blocks */
    size_t header_len;
    unsigned gen;
    unsigned eof_seq;          /* raised to end the file for everybody who has it open (usb.eof) */
    unsigned long long streak; /* sound bytes delivered without a pause of more than a second */
    long long last_read_ms;
};

struct aa_ocb {
    iofunc_ocb_t hdr; /* must be first: the library frees the OCB through this pointer */
    int track;        /* -1 = the directory */
    unsigned dir_index;
    unsigned gen;                 /* tag generation when the file was opened */
    unsigned eof_seq;             /* the file's eof_seq when it was opened */
    unsigned char frame[FRAME_MAX];
    size_t frame_len, frame_off;
    unsigned frame_no;
    unsigned long long pcm_pos;   /* next PCM byte (absolute) */
    int started;
    unsigned free_frames;         /* silence frames this reader may still get without waiting */
};

static volatile sig_atomic_t g_stop;
static const char *g_mount, *g_dir;
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_dir_attr;
static struct track g_track[TRACKS];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER; /* ring, headers, counters */
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static unsigned char* g_ring;
static unsigned long long g_pcm_end; /* absolute number of PCM bytes received */
static long long g_pcm_ms;           /* when the last PCM arrived */
static unsigned g_gen, g_opens, g_devctls[32], g_devctl_n;
static int g_aa_state, g_aa_track = -1;
static time_t g_start_time;
static int g_silent; /* --silent 1: the files carry silence and audio.fifo is left to nbt_a2i_play (sound on the direct path, the stock player only "plays" for the display and the list) */
static int g_keep_open; /* --keep-open 1: a song change does not end the files (music collection mode: the HMI would move on to the next file by itself) */
static unsigned char g_crc8[256];
static unsigned short g_crc16[256];

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void note(const char* what, const char* text, long long a, long long b)
{
    time_t t = time(0);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d %s %s %lld %lld\n", tm.tm_hour, tm.tm_min, tm.tm_sec, what, text ? text : "", a, b);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- FLAC ---------------------------------------------------------------------------------------------------------- */

static void crc_init(void)
{
    int i, k;
    for (i = 0; i < 256; ++i) {
        unsigned c8 = i, c16 = i << 8;
        for (k = 0; k < 8; ++k) {
            c8 = (c8 & 0x80) ? ((c8 << 1) ^ 0x07) & 0xff : (c8 << 1) & 0xff;
            c16 = (c16 & 0x8000) ? ((c16 << 1) ^ 0x8005) & 0xffff : (c16 << 1) & 0xffff;
        }
        g_crc8[i] = (unsigned char)c8;
        g_crc16[i] = (unsigned short)c16;
    }
}

/* one frame of BLOCK samples from interleaved little-endian PCM (pcm = 0: silence); returns its length */
static size_t flac_frame(unsigned char* out, unsigned number, const unsigned char* pcm)
{
    size_t n = 0, i;
    unsigned c8 = 0, c16 = 0;
    int ch;
    out[n++] = 0xff;
    out[n++] = 0xf8;               /* sync, fixed block size */
    out[n++] = 0xaa;               /* 1024 samples, 48 kHz */
    out[n++] = 0x18;               /* 2 channels independent, 16 bit */
    if (number < 0x80)             /* frame number, coded like UTF-8 */
        out[n++] = (unsigned char)number;
    else if (number < 0x800) {
        out[n++] = (unsigned char)(0xc0 | (number >> 6));
        out[n++] = (unsigned char)(0x80 | (number & 0x3f));
    } else if (number < 0x10000) {
        out[n++] = (unsigned char)(0xe0 | (number >> 12));
        out[n++] = (unsigned char)(0x80 | ((number >> 6) & 0x3f));
        out[n++] = (unsigned char)(0x80 | (number & 0x3f));
    } else if (number < 0x200000) {
        out[n++] = (unsigned char)(0xf0 | (number >> 18));
        out[n++] = (unsigned char)(0x80 | ((number >> 12) & 0x3f));
        out[n++] = (unsigned char)(0x80 | ((number >> 6) & 0x3f));
        out[n++] = (unsigned char)(0x80 | (number & 0x3f));
    } else {
        out[n++] = (unsigned char)(0xf8 | ((number >> 24) & 0x03));
        out[n++] = (unsigned char)(0x80 | ((number >> 18) & 0x3f));
        out[n++] = (unsigned char)(0x80 | ((number >> 12) & 0x3f));
        out[n++] = (unsigned char)(0x80 | ((number >> 6) & 0x3f));
        out[n++] = (unsigned char)(0x80 | (number & 0x3f));
    }
    for (i = 0; i < n; ++i)
        c8 = g_crc8[c8 ^ out[i]];
    out[n++] = (unsigned char)c8;
    for (ch = 0; ch < 2; ++ch) {
        out[n++] = 0x02; /* subframe: verbatim */
        if (pcm)
            for (i = 0; i < BLOCK; ++i) {
                out[n++] = pcm[i * 4 + ch * 2 + 1]; /* big endian */
                out[n++] = pcm[i * 4 + ch * 2];
            }
        else {
            memset(out + n, 0, BLOCK * 2);
            n += BLOCK * 2;
        }
    }
    for (i = 0; i < n; ++i)
        c16 = (g_crc16[((c16 >> 8) ^ out[i]) & 0xff] ^ (c16 << 8)) & 0xffff;
    out[n++] = (unsigned char)(c16 >> 8);
    out[n++] = (unsigned char)c16;
    return n;
}

static size_t put_be32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
    return 4;
}

static size_t put_le32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
    return 4;
}

static size_t put_comment(unsigned char* p, const char* key, const char* value)
{
    const size_t k = strlen(key), v = strlen(value);
    size_t n = put_le32(p, (unsigned)(k + 1 + v));
    memcpy(p + n, key, k);
    n += k;
    p[n++] = '=';
    memcpy(p + n, value, v);
    return n + v;
}

/* New header for one track (g_mx held): STREAMINFO, tags, optional cover. */
static void build_header(struct track* t, int number, const char* title, const char* artist, const char* album,
                         const unsigned char* cover, size_t cover_len)
{
    unsigned char* h = malloc(4096 + strlen(title) + strlen(artist) + strlen(album) + cover_len);
    size_t n = 0, at, len;
    char num[8];
    if (!h)
        return;
    memcpy(h, "fLaC", 4);
    n = 4;
    /* STREAMINFO (34 bytes): block size 1024..1024, frame size unknown, then 20 bits sample rate 48000 (0x0BB80),
     * 3 bits channels - 1, 5 bits sample size - 1, 36 bits number of samples (0 = unknown), MD5 not set */
    h[n++] = 0x00;
    h[n++] = 0;
    h[n++] = 0;
    h[n++] = 34;
    h[n++] = BLOCK >> 8;
    h[n++] = BLOCK & 0xff;
    h[n++] = BLOCK >> 8;
    h[n++] = BLOCK & 0xff;
    memset(h + n, 0, 6);
    n += 6;
    h[n++] = 0x0b;
    h[n++] = 0xb8;
    h[n++] = 0x02;
    h[n++] = 0xf0;
    memset(h + n, 0, 4 + 16);
    n += 4 + 16;
    /* VORBIS_COMMENT (the last block unless a cover follows) */
    at = n;
    n += 4;
    n += put_le32(h + n, 6);
    memcpy(h + n, "nbt-aa", 6);
    n += 6;
    n += put_le32(h + n, 4);
    snprintf(num, sizeof(num), "%d", number);
    n += put_comment(h + n, "TITLE", title);
    n += put_comment(h + n, "ARTIST", artist);
    n += put_comment(h + n, "ALBUM", album);
    n += put_comment(h + n, "TRACKNUMBER", num);
    len = n - at - 4;
    h[at] = (unsigned char)((cover_len ? 0x00 : 0x80) | 4);
    h[at + 1] = (unsigned char)(len >> 16);
    h[at + 2] = (unsigned char)(len >> 8);
    h[at + 3] = (unsigned char)len;
    if (cover_len) {
        const char* mime = cover_len > 4 && cover[0] == 0x89 && cover[1] == 'P' ? "image/png" : "image/jpeg";
        at = n;
        n += 4;
        n += put_be32(h + n, 3); /* front cover */
        n += put_be32(h + n, (unsigned)strlen(mime));
        memcpy(h + n, mime, strlen(mime));
        n += strlen(mime);
        n += put_be32(h + n, 0); /* description */
        n += put_be32(h + n, 0); /* width, height, depth, colours: unknown */
        n += put_be32(h + n, 0);
        n += put_be32(h + n, 0);
        n += put_be32(h + n, 0);
        n += put_be32(h + n, (unsigned)cover_len);
        memcpy(h + n, cover, cover_len);
        n += cover_len;
        len = n - at - 4;
        h[at] = 0x80 | 6;
        h[at + 1] = (unsigned char)(len >> 16);
        h[at + 2] = (unsigned char)(len >> 8);
        h[at + 3] = (unsigned char)len;
    }
    free(t->header);
    t->header = h;
    t->header_len = n;
    t->gen = g_gen;
    t->streak = 0;
    /* size and time for stat(): plain values, set without the attribute's lock on purpose (the read handler holds that
     * lock and then takes g_mx, the other order here could deadlock) */
    /* Size and time stay the same for every song (car 2026-11-14): when they changed, mme dropped the library entry
     * and made a new one with a new number, which the HMI's running track session does not contain ("Could not find
     * fid in tracksession"). The tags are refreshed by a metadata-only resync instead. */
    t->attr.nbytes = (off_t)FILE_SPAN;
    t->attr.mtime = t->attr.ctime = g_start_time;
}

/* ---- inputs: PCM FIFO, media.state --------------------------------------------------------------------------------- */

static void* pcm_thread(void* unused)
{
    char path[200];
    static unsigned char buf[32768];
    int fd;
    (void)unused;
    snprintf(path, sizeof(path), "%s/audio.fifo", g_dir);
    fd = open(path, O_RDWR | O_NONBLOCK); /* O_RDWR: never end-of-file between sessions */
    if (fd < 0) {
        note("pcm-fifo-failed", path, errno, 0);
        return 0;
    }
    while (!g_stop) {
        fd_set set;
        struct timeval tv = {0, 200000};
        int n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, 0, 0, &tv) <= 0 || !FD_ISSET(fd, &set))
            continue;
        n = (int)read(fd, buf, sizeof(buf));
        if (n <= 0)
            continue;
        pthread_mutex_lock(&g_mx);
        {
            const size_t at = (size_t)(g_pcm_end % RING), first = RING - at < (size_t)n ? RING - at : (size_t)n;
            memcpy(g_ring + at, buf, first);
            if ((size_t)n > first)
                memcpy(g_ring, buf + first, (size_t)n - first);
        }
        g_pcm_end += (unsigned)n;
        g_pcm_ms = now_ms();
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mx);
    }
    close(fd);
    return 0;
}

/* value of "key=" in a media.state text (up to the end of the line), "" if missing */
static void state_value(const char* text, const char* key, char* out, size_t max)
{
    const size_t k = strlen(key);
    const char* p = text;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, k) && p[k] == '=') {
            size_t n = strcspn(p + k + 1, "\r\n");
            if (n >= max)
                n = max - 1;
            memcpy(out, p + k + 1, n);
            out[n] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p)
            ++p;
    }
}

/* Follows media.state: new tags for the three files whenever the phone's song changes; writes usb.state. */
static void* state_thread(void* unused)
{
    char path[200], cover_path[200], out_path[200], tmp_path[210];
    static char text[1200], title[220], artist[220], album[220], source[80], value[32];
    static char last_title[220], last_artist[220], last_album[220];
    static unsigned char cover[COVER_MAX];
    long last_cover = -1;
    int have_tags = 0;
    (void)unused;
    snprintf(path, sizeof(path), "%s/media.state", g_dir);
    snprintf(cover_path, sizeof(cover_path), "%s/media.cover", g_dir);
    snprintf(out_path, sizeof(out_path), "%s/usb.state", g_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    while (!g_stop) {
        int fd = open(path, O_RDONLY), i;
        ssize_t n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
        long cover_size = 0;
        if (fd >= 0)
            close(fd);
        if (n > 0) {
            text[n] = 0;
            state_value(text, "state", value, sizeof(value));
            g_aa_state = atoi(value);
            state_value(text, "track", value, sizeof(value));
            g_aa_track = atoi(value);
            state_value(text, "cover", value, sizeof(value));
            cover_size = atol(value);
            state_value(text, "title", title, sizeof(title));
            state_value(text, "artist", artist, sizeof(artist));
            state_value(text, "album", album, sizeof(album));
            state_value(text, "source", source, sizeof(source));
        } else {
            g_aa_state = 0;
            title[0] = 0;
        }
        /* The phone often sends a song twice within a moment (first with a small cover, then with the final one). New
         * tags are only taken when title, artist, album and cover size have been the same for 0.7 s, otherwise every
         * song change would restart the playback twice. */
        {
            static char candidate[700], seen[700];
            static long long candidate_ms;
            snprintf(seen, sizeof(seen), "%s|%s|%s|%ld", title, artist, album, cover_size);
            if (strcmp(seen, candidate)) {
                snprintf(candidate, sizeof(candidate), "%s", seen);
                candidate_ms = now_ms();
            }
            if (now_ms() - candidate_ms < 700)
                title[0] = 0; /* not settled yet: no new tags in this round */
        }
        if (title[0] && (!have_tags || strcmp(title, last_title) || strcmp(artist, last_artist) ||
                         strcmp(album, last_album) || cover_size != last_cover)) {
            size_t cover_len = 0;
            if (cover_size > 0 && cover_size <= COVER_MAX) {
                fd = open(cover_path, O_RDONLY);
                if (fd >= 0) {
                    ssize_t c = read(fd, cover, COVER_MAX);
                    close(fd);
                    if (c == cover_size)
                        cover_len = (size_t)c;
                }
            }
            /* a cover that is announced but not complete yet: try again on the next round */
            if (cover_size <= 0 || cover_size > COVER_MAX || cover_len) {
                const char* by = source[0] ? source : "Android Auto";
                pthread_mutex_lock(&g_mx);
                ++g_gen;
                (void)by; /* ring: the files keep their first tags, the script writes each song into mme's database */
                pthread_mutex_unlock(&g_mx);
                snprintf(last_title, sizeof(last_title), "%s", title);
                snprintf(last_artist, sizeof(last_artist), "%s", artist);
                snprintf(last_album, sizeof(last_album), "%s", album);
                last_cover = cover_size;
                have_tags = 1;
                note("tags", title, g_gen, (long long)cover_len);
            }
        }
        /* usb.state for the control script */
        {
            char out[600];
            const long long now = now_ms();
            int len;
            pthread_mutex_lock(&g_mx);
            len = snprintf(out, sizeof(out), "gen=%u\naa_state=%d\naa_track=%d\npcm_age=%lld\n", g_gen, g_aa_state,
                           g_aa_track, g_pcm_ms ? now - g_pcm_ms : 999999LL);
            {
                /* the entry the stock player is reading now: the one with the longest run of sound */
                int reading = 0;
                unsigned long long best = 0;
                for (i = 0; i < TRACKS; ++i) {
                    const long long age = g_track[i].last_read_ms ? now - g_track[i].last_read_ms : 999999LL;
                    if (age > 1000)
                        g_track[i].streak = 0;
                    else if (g_track[i].streak > best) {
                        best = g_track[i].streak;
                        reading = i + 1;
                    }
                }
                len += snprintf(out + len, sizeof(out) - len, "reading=%d\nreading_bytes=%llu\n", reading, best);
            }
            pthread_mutex_unlock(&g_mx);
            fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const ssize_t w = write(fd, out, (size_t)len);
                close(fd);
                if (w == len)
                    rename(tmp_path, out_path);
            }
        }
        /* usb.eof (written by the script): the number of an entry that ends now for whoever has it open; the stock
         * player then walks on to the next entry by itself */
        {
            char eof_path[210], num[16];
            snprintf(eof_path, sizeof(eof_path), "%s/usb.eof", g_dir);
            fd = open(eof_path, O_RDONLY);
            if (fd >= 0) {
                const ssize_t k = read(fd, num, sizeof(num) - 1);
                close(fd);
                unlink(eof_path);
                if (k > 0) {
                    num[k] = 0;
                    i = atoi(num);
                    if (i >= 1 && i <= TRACKS) {
                        pthread_mutex_lock(&g_mx);
                        ++g_track[i - 1].eof_seq;
                        pthread_mutex_unlock(&g_mx);
                        note("eof", g_track[i - 1].name, i, 0);
                    }
                }
            }
        }
        usleep(200000);
    }
    return 0;
}

/* ---- resource manager ------------------------------------------------------------------------------------------------ */

static int aa_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    const char* path = msg->connect.path;
    struct aa_ocb* o;
    iofunc_attr_t* attr;
    int r, track = -1, i;
    (void)handle;
    (void)extra;
    if ((msg->connect.ioflag & _IO_FLAG_WR) || (msg->connect.ioflag & (O_CREAT | O_TRUNC)))
        return EROFS;
    if (*path) {
        for (i = 0; i < TRACKS; ++i)
            if (!strcmp(path, g_track[i].name))
                track = i;
        if (track < 0)
            return ENOENT;
    }
    o = calloc(1, sizeof(*o));
    if (!o)
        return ENOMEM;
    o->track = track;
    o->free_frames = 48; /* one second of silence at once */
    pthread_mutex_lock(&g_mx);
    o->gen = track >= 0 ? g_track[track].gen : 0;
    o->eof_seq = track >= 0 ? g_track[track].eof_seq : 0;
    if (track >= 0 && !g_track[track].header) { /* no song known yet */
        pthread_mutex_unlock(&g_mx);
        free(o);
        return ENOENT;
    }
    pthread_mutex_unlock(&g_mx);
    attr = track >= 0 ? &g_track[track].attr : &g_dir_attr;
    iofunc_attr_lock(attr);
    r = iofunc_ocb_attach(ctp, msg, &o->hdr, attr, &g_io);
    iofunc_attr_unlock(attr);
    if (r != EOK) {
        free(o);
        return r;
    }
    if (++g_opens <= 400)
        note("open", *path ? path : "/", ctp->info.pid, g_opens);
    return EOK;
}

static int aa_lseek(resmgr_context_t* ctp, io_lseek_t* msg, RESMGR_OCB_T* o)
{
    if (o->track < 0) { /* directory: only "back to the start" */
        if (msg->i.whence != SEEK_SET || msg->i.offset != 0)
            return EINVAL;
        o->dir_index = 0;
        o->hdr.offset = 0;
        if (msg->i.combine_len & _IO_COMBINE_FLAG)
            return EOK;
        msg->o = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    return iofunc_lseek_default(ctp, msg, &o->hdr);
}

/* The next frame for this reader into o->frame. Called with the file's attribute locked; it is unlocked while waiting
 * for sound, so other requests for the same file are not held up. */
static void next_frame(struct aa_ocb* o)
{
    unsigned char pcm[BLOCK_BYTES];
    int have = 0;
    iofunc_attr_unlock(o->hdr.attr);
    pthread_mutex_lock(&g_mx);
    if (!o->started) {
        o->pcm_pos = g_pcm_end & ~3ULL;
        o->started = 1;
    }
    if (g_pcm_end - o->pcm_pos > MAX_LAG)
        o->pcm_pos = (g_pcm_end - KEEP_LAG) & ~3ULL;
    if (g_pcm_end - o->pcm_pos < BLOCK_BYTES) {
        const int flowing = g_pcm_ms && now_ms() - g_pcm_ms < 500;
        if (flowing || !o->free_frames) { /* sound is coming (or the free silence is used up): wait for it */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (flowing ? 150 : 21) * 1000000L;
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
            while (g_pcm_end - o->pcm_pos < BLOCK_BYTES && !g_stop)
                if (pthread_cond_timedwait(&g_cv, &g_mx, &ts))
                    break;
        } else
            --o->free_frames;
    }
    if (g_pcm_end - o->pcm_pos >= BLOCK_BYTES) {
        const size_t at = (size_t)(o->pcm_pos % RING), first = RING - at < BLOCK_BYTES ? RING - at : BLOCK_BYTES;
        memcpy(pcm, g_ring + at, first);
        if (first < BLOCK_BYTES)
            memcpy(pcm + first, g_ring, BLOCK_BYTES - first);
        o->pcm_pos += BLOCK_BYTES;
        have = 1;
    }
    pthread_mutex_unlock(&g_mx);
    o->frame_len = flac_frame(o->frame, o->frame_no++, have ? pcm : 0);
    o->frame_off = 0;
    iofunc_attr_lock(o->hdr.attr);
}

static int aa_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* o)
{
    const int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    size_t want = msg->i.nbytes, used = 0;
    char* buf;
    int status = iofunc_read_verify(ctp, msg, &o->hdr, 0);
    if (status != EOK)
        return status;
    if (want > 64 * 1024)
        want = 64 * 1024;
    if (!want) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    buf = malloc(want);
    if (!buf)
        return ENOMEM;
    if (o->track < 0) {
        /* directory entries: ".", "..", the three files */
        static const char* const dots[2] = {".", ".."};
        if (xtype != _IO_XTYPE_NONE) {
            free(buf);
            return ENOSYS;
        }
        while (o->dir_index < 2 + TRACKS) {
            const char* name = o->dir_index < 2 ? dots[o->dir_index] : g_track[o->dir_index - 2].name;
            const size_t len = strlen(name);
            const size_t rec = (offsetof(struct dirent, d_name) + len + 1 + 7) & ~(size_t)7;
            struct dirent* d = (struct dirent*)(buf + used);
            if (o->dir_index >= 2 && !g_track[o->dir_index - 2].header) { /* no song yet: an empty medium */
                ++o->dir_index;
                continue;
            }
            if (used + rec > want)
                break;
            memset(d, 0, rec);
            d->d_ino = o->dir_index + 1;
            d->d_offset = o->dir_index + 1;
            d->d_reclen = (short)rec;
            d->d_namelen = (short)len;
            memcpy(d->d_name, name, len + 1);
            used += rec;
            ++o->dir_index;
        }
    } else {
        struct track* t = &g_track[o->track];
        off64_t at = o->hdr.offset;
        int live = 0;
        if (xtype == _IO_XTYPE_OFFSET)
            at = ((struct _xtype_offset*)(&msg->i + 1))->offset;
        else if (xtype != _IO_XTYPE_NONE) {
            free(buf);
            return ENOSYS;
        }
        pthread_mutex_lock(&g_mx);
        if ((o->gen != t->gen && !g_keep_open) || o->eof_seq != t->eof_seq) /* the tags changed since this reader opened the file: end of file */
            want = 0;
        else if (at < (off64_t)t->header_len) {
            used = t->header_len - (size_t)at;
            if (used > want)
                used = want;
            memcpy(buf, t->header + at, used);
        } else
            live = 1;
        pthread_mutex_unlock(&g_mx);
        if (live) {
            /* frame after frame; the last one may be cut by the buffer and is continued by the next read */
            if (o->frame_off >= o->frame_len)
                next_frame(o);
            while (used < want) {
                size_t n = o->frame_len - o->frame_off;
                if (n > want - used)
                    n = want - used;
                memcpy(buf + used, o->frame + o->frame_off, n);
                o->frame_off += n;
                used += n;
                if (o->frame_off < o->frame_len || used + FRAME_MAX > want)
                    break;
                /* more only if the sound for it is already there: never wait with data in hand */
                pthread_mutex_lock(&g_mx);
                status = o->started && g_pcm_end - o->pcm_pos >= BLOCK_BYTES;
                pthread_mutex_unlock(&g_mx);
                if (!status)
                    break;
                next_frame(o);
            }
            pthread_mutex_lock(&g_mx);
            {
                const long long now = now_ms();
                if (t->last_read_ms && now - t->last_read_ms > 1000)
                    t->streak = 0;
                t->streak += used;
                t->last_read_ms = now;
            }
            pthread_mutex_unlock(&g_mx);
        }
        if (xtype == _IO_XTYPE_NONE)
            o->hdr.offset = at + (off64_t)used;
    }
    MsgReply(ctp->rcvid, (int)used, buf, used);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int aa_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* o)
{
    unsigned i;
    const char* text = 0;
    if (msg->i.dcmd == DCMD_FSYS_STATVFS) {
        struct statvfs* v = (struct statvfs*)_DEVCTL_DATA(msg->o);
        memset(v, 0, sizeof(*v));
        v->f_bsize = v->f_frsize = 512;
        v->f_blocks = 2 * 1024 * 1024; /* "1 GB stick", full */
        v->f_files = 64;
        v->f_namemax = 255;
        v->f_flag = ST_RDONLY;
        strcpy(v->f_basetype, "dos");
        msg->o.ret_val = 0;
        msg->o.nbytes = sizeof(*v);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*v));
    }
    /* What mme (media storage "devb") and the HMI's media application ask a USB stick (car 2026-11-13): where it is
     * mounted, by which file system, its volume label (the name of the medium) and the serial number of the device. */
    if (msg->i.dcmd == DCMD_FSYS_MOUNTED_AT)
        text = g_mount;
    else if (msg->i.dcmd == DCMD_FSYS_MOUNTED_ON)
        text = "/dev/umass0";
    else if (msg->i.dcmd == DCMD_FSYS_MOUNTED_BY)
        text = "dos";
    else if (msg->i.dcmd == DCMD_FSYS_OPTIONS)
        text = "ro";
    else if (msg->i.dcmd == DCMD_FSYS_LABEL)
        text = "Android Auto";
    else if (msg->i.dcmd == DCMD_CAM_DEV_SERIAL_NUMBER)
        text = "NBTAA0001";
    if (text) {
        char* out = (char*)_DEVCTL_DATA(msg->o);
        memset(out, 0, 256);
        strncpy(out, text, 255);
        msg->o.ret_val = 0;
        msg->o.nbytes = 256;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + 256);
    }
    for (i = 0; i < g_devctl_n && g_devctls[i] != (unsigned)msg->i.dcmd; ++i) {
    }
    if (i == g_devctl_n && g_devctl_n < 32) {
        g_devctls[g_devctl_n++] = (unsigned)msg->i.dcmd;
        note("devctl-unknown", "", (long long)(unsigned)msg->i.dcmd, ctp->info.pid);
    }
    return iofunc_devctl_default(ctp, msg, &o->hdr);
}

int main(int argc, char** argv)
{
    const char* log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    pthread_t pcm, state;
    struct sigaction sa;
    struct stat st;
    int i, id;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mount"))
            g_mount = argv[i + 1];
        else if (!strcmp(argv[i], "--dir"))
            g_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--silent"))
            g_silent = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--keep-open"))
            g_keep_open = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else
            return 2;
    }
    /* allowed places: a USB slot of mme, or a folder inside the music collection (the HMI then plays the files itself) */
    if (!g_mount || !g_dir || (strncmp(g_mount, "/fs/", 4) && strncmp(g_mount, "/mnt/quota/mm/EntServer/", 24)) ||
        strstr(g_mount, "..") || strlen(g_dir) > 120 ||
        stat(g_dir, &st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "usage: %s --mount /fs/<name> --dir <runtime directory> [--log <file>]\n", argv[0]);
        return 2;
    }
    if (log && !freopen(log, "a", stderr))
        return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    g_start_time = time(0);
    crc_init();
    g_ring = malloc(RING);
    if (!g_ring)
        return 3;

    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 4096;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = aa_open;
    g_io.read = aa_read;
    g_io.lseek = aa_lseek;
    g_io.devctl = aa_devctl;
    iofunc_attr_init(&g_dir_attr, S_IFDIR | 0555, 0, 0);
    for (i = 0; i < TRACKS; ++i) {
        snprintf(g_track[i].name, sizeof(g_track[i].name), "%02d.flac", i + 1);
        iofunc_attr_init(&g_track[i].attr, S_IFREG | 0444, 0, 0);
        g_track[i].attr.inode = 10 + i;
        build_header(&g_track[i], i + 1, i < FIRST ? "<" : ">", "Android Auto", "Android Auto", 0, 0);
    }
    if ((!g_silent && pthread_create(&pcm, 0, pcm_thread, 0)) || pthread_create(&state, 0, state_thread, 0))
        return 3;
    id = resmgr_attach(dpp, &rattr, g_mount, _FTYPE_ANY, _RESMGR_FLAG_DIR, &g_connect, &g_io, &g_dir_attr);
    if (id < 0) {
        fprintf(stderr, "resmgr_attach(%s) failed: %s\n", g_mount, strerror(errno));
        return 5;
    }
    note("attached", g_mount, 0, 0);

    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.context_free = dispatch_context_free;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.lo_water = 2;
    pattr.hi_water = 5;
    pattr.increment = 1;
    pattr.maximum = 10;
    pool = thread_pool_create(&pattr, 0);
    if (!pool || thread_pool_start(pool) < 0) {
        resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
        return 6;
    }
    while (!g_stop)
        sleep(1);
    resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
    note("detached", g_mount, g_opens, 0);
    return 0;
}
