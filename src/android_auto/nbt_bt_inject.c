/* nbt_bt_inject: puts the Android Auto now-playing state into the head unit's Bluetooth audio entry.
 *
 * Situation (car 2026-11-23): the phone is connected by Bluetooth (telephone + audio) and runs Android Auto over USB.
 * Its media sound goes to Android Auto; our sound player plays it into the amplifier input the head unit routes for the
 * Bluetooth source, so it is audible on the phone's Bluetooth entry in iDrive. But over Bluetooth (AVRCP) the phone
 * reports "paused" - its music does not go over Bluetooth - so the Bluetooth entry shows no title, and the head unit
 * seems to switch the sound off about 20 s after the Bluetooth tab is left.
 *
 * This program stands in front of the stock Bluetooth audio driver (io-fs-media_BT, module iofs-mediabt) at its mount
 * point /fs/avrcpN, WITHOUT replacing it: every request is passed to the stock driver unchanged - connection, sound
 * URL, events, track data, commands to the phone - except
 *   - the playback status (DCMD_MEDIA_PLAYBACK_STATUS, 0x0e19): "playing" instead of "paused" while Android Auto
 *     plays, with Android Auto's length and position;
 *   - song / artist / album (0x0e64 / 0x0e66 / 0x0e65): Android Auto's, when the stock driver has none;
 *   - cover (0x0e34 / 0x0e35 / 0x0e36): Android Auto's picture; the stock driver answers "not supported" there.
 *
 * A program attached at the same path cannot reach the stock driver by path any more (every new open would come back
 * to itself). So before attaching it opens every file of the stock driver's small tree (/, .FS_info., control,
 * playback, audio, info.xml) several times and keeps those handles in a pool: each open of a client gets a handle of
 * its own (the stock driver may keep per-handle state, e.g. the event queue). The directory listings are read before
 * attaching, too. ("current" is a link to "playback" in the stock tree; it is served as playback.)
 *
 * It ends by itself when the stock driver's mount disappears (phone disconnected): it watches /proc/mount/<mount>/
 * for an attachment of another process. While it runs, /fs/avrcpN exists only as long as the stock one does.
 *
 * Usage: nbt_bt_inject --mount /fs/avrcp0 --run /fs/ram3/nbt-aa-native [--log file] [--verbose 1]
 * Data: <run>/media.state, <run>/media.cover (written by oneplus_native). It never writes to the phone itself.
 */
struct b_ocb;
#define IOFUNC_OCB_T struct b_ocb
#define RESMGR_OCB_T struct b_ocb
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

#define POOL 8         /* handles per stock file */
#define MAXNAMES 32
#define MAXARMED 32

enum { N_ROOT, N_FSINFO, N_CONTROL, N_PLAYBACK, N_AUDIO, N_INFOXML, N_COUNT };
static const char* const k_rel[N_COUNT] = {"", ".FS_info.", ".FS_info./control", ".FS_info./playback", ".FS_info./audio",
                                           ".FS_info./info.xml"};

struct node {
    int exists, is_dir;
    struct stat st;
    int fd[POOL];
    int users[POOL];
    int nnames;
    char names[MAXNAMES][64];
};

struct b_ocb {
    iofunc_ocb_t hdr; /* must be first */
    int node, slot;
    unsigned dirpos;
    size_t art_pos;
    char rel[64];
};

static volatile sig_atomic_t g_stop;
static const char* g_mount;
static const char* g_run;
static int g_verbose;
static unsigned g_lines;
static struct node g_nodes[N_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_dir_attr, g_file_attr;
static unsigned g_counts[256]; /* requests per DCMD_MEDIA number, for the log */

/* ---- what Android Auto is playing ---- */
static struct {
    char title[200], artist[200], album[200];
    int playing, valid;
    long position_s, duration_s;
    time_t stamp;
    unsigned char* cover;
    size_t cover_len;
    time_t cover_stamp;
    unsigned cover_w, cover_h;
    char cover_type[16];
} aa;

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
    if (stat(path, &st) || st.st_size < 32 || st.st_size > 2 * 1024 * 1024) {
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
    if (aa.cover_len > 24 && !memcmp(aa.cover, "\x89PNG", 4)) {
        aa.cover_w = (unsigned)aa.cover[16] << 24 | aa.cover[17] << 16 | aa.cover[18] << 8 | aa.cover[19];
        aa.cover_h = (unsigned)aa.cover[20] << 24 | aa.cover[21] << 16 | aa.cover[22] << 8 | aa.cover[23];
    } else if (aa.cover_len > 3 && aa.cover[0] == 0xff && aa.cover[1] == 0xd8)
        strcpy(aa.cover_type, "image/jpeg");
}

/* media.state, read at most twice a second (under g_lock) */
static void refresh(void)
{
    char path[200], line[400];
    struct stat st;
    struct timespec now;
    static long last_ms;
    long ms;
    FILE* f;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
    if (last_ms && ms - last_ms < 500)
        return;
    last_ms = ms;
    load_cover();
    snprintf(path, sizeof(path), "%s/media.state", g_run);
    if (stat(path, &st)) {
        aa.valid = 0;
        return;
    }
    if (st.st_mtime == aa.stamp)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    aa.title[0] = aa.artist[0] = aa.album[0] = 0;
    aa.playing = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, "title=", 6))
            snprintf(aa.title, sizeof(aa.title), "%s", line + 6);
        else if (!strncmp(line, "artist=", 7))
            snprintf(aa.artist, sizeof(aa.artist), "%s", line + 7);
        else if (!strncmp(line, "album=", 6))
            snprintf(aa.album, sizeof(aa.album), "%s", line + 6);
        else if (!strncmp(line, "state=", 6))
            aa.playing = atoi(line + 6) == 2; /* oneplus_native: 2 = playing, 3 = paused */
        else if (!strncmp(line, "position=", 9))
            aa.position_s = atol(line + 9);
        else if (!strncmp(line, "duration=", 9))
            aa.duration_s = atol(line + 9);
    }
    fclose(f);
    aa.stamp = st.st_mtime;
    aa.valid = 1;
    if (g_verbose)
        note("android auto: %s, %s / %s (%ld of %ld s)", aa.playing ? "playing" : "paused", aa.title, aa.artist,
             aa.position_s, aa.duration_s);
}

/* ---- the stock tree, opened before we attach ---- */
static int prepare(void)
{
    int i, k, open_files = 0;
    for (i = 0; i < N_COUNT; ++i) {
        char path[160];
        struct node* n = &g_nodes[i];
        snprintf(path, sizeof(path), "%s%s%s", g_mount, *k_rel[i] ? "/" : "", k_rel[i]);
        for (k = 0; k < POOL; ++k)
            n->fd[k] = -1;
        if (stat(path, &n->st))
            continue;
        n->exists = 1;
        n->is_dir = S_ISDIR(n->st.st_mode);
        for (k = 0; k < POOL; ++k) {
            int fd = n->is_dir ? -1 : open(path, O_RDWR);
            if (fd < 0)
                fd = open(path, O_RDONLY);
            n->fd[k] = fd;
            if (fd >= 0)
                ++open_files;
        }
        if (n->is_dir) {
            DIR* d = opendir(path);
            struct dirent* e;
            while (d && (e = readdir(d)) && n->nnames < MAXNAMES) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                snprintf(n->names[n->nnames++], sizeof(n->names[0]), "%s", e->d_name);
            }
            if (d)
                closedir(d);
        }
        note("stock %-20s %s, %d names, handles %s", *k_rel[i] ? k_rel[i] : "/", n->is_dir ? "folder" : "file", n->nnames,
             n->fd[0] >= 0 ? "ok" : "NONE");
    }
    return g_nodes[N_ROOT].exists && g_nodes[N_CONTROL].exists && g_nodes[N_CONTROL].fd[0] >= 0 && open_files > POOL;
}

/* Is the stock driver still attached at our mount? /proc/mount/<mount>/ lists one entry per attachment
 * ("nd,pid,chid,handle,type"); ours is skipped. */
static int stock_alive(void)
{
    char path[160], mine[32];
    DIR* d;
    struct dirent* e;
    int found = 0;
    snprintf(path, sizeof(path), "/proc/mount%s", g_mount);
    snprintf(mine, sizeof(mine), ",%d,", (int)getpid());
    d = opendir(path);
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (!strstr(e->d_name, mine))
            found = 1;
    }
    closedir(d);
    return found;
}

static int find_node(const char* path)
{
    int i;
    if (!strcmp(path, ".FS_info./current"))
        return N_PLAYBACK;
    for (i = 0; i < N_COUNT; ++i)
        if (g_nodes[i].exists && !strcmp(path, k_rel[i]))
            return i;
    return -1;
}

/* ---- notification forwarding: the stock driver would wake us, not the client ---- */
static struct {
    int used, rcvid, fd, flags;
    struct b_ocb* o;
    struct sigevent event;
} g_armed[MAXARMED];

static void* armed_thread(void* arg)
{
    int i;
    (void)arg;
    while (!g_stop) {
        usleep(200000);
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < MAXARMED; ++i) {
            int bits;
            if (!g_armed[i].used)
                continue;
            bits = ionotify(g_armed[i].fd, _NOTIFY_ACTION_POLL, g_armed[i].flags, 0);
            if (bits > 0 || bits < 0) {
                MsgDeliverEvent(g_armed[i].rcvid, &g_armed[i].event);
                g_armed[i].used = 0;
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return 0;
}

static int ocb_fd(struct b_ocb* o)
{
    return o->slot >= 0 ? g_nodes[o->node].fd[o->slot] : -1;
}

/* ---- resource manager ---- */
static int b_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    char path[64];
    struct b_ocb* o;
    struct node* n;
    size_t len;
    int node, k, best = -1, r;
    (void)handle;
    (void)extra;
    if (strlen(msg->connect.path) >= sizeof(path))
        return ENOENT;
    strcpy(path, msg->connect.path);
    len = strlen(path);
    while (len && path[len - 1] == '/')
        path[--len] = 0;
    node = find_node(path);
    if (node < 0)
        return ENOENT;
    o = calloc(1, sizeof(*o));
    if (!o)
        return ENOMEM;
    o->node = node;
    o->slot = -1;
    snprintf(o->rel, sizeof(o->rel), "%s", *path ? path : "/");
    n = &g_nodes[node];
    pthread_mutex_lock(&g_lock);
    for (k = 0; k < POOL; ++k) /* a handle of its own, else the least used one */
        if (n->fd[k] >= 0 && (best < 0 || n->users[k] < n->users[best]))
            best = k;
    if (best >= 0) {
        o->slot = best;
        ++n->users[best];
    }
    pthread_mutex_unlock(&g_lock);
    r = iofunc_ocb_attach(ctp, msg, &o->hdr, n->is_dir ? &g_dir_attr : &g_file_attr, &g_io);
    if (r != EOK) {
        pthread_mutex_lock(&g_lock);
        if (o->slot >= 0)
            --n->users[o->slot];
        pthread_mutex_unlock(&g_lock);
        free(o);
        return r;
    }
    if (g_verbose > 1)
        note("open %s pid %d handle %d", o->rel, ctp->info.pid, o->slot);
    return EOK;
}

static int b_close(resmgr_context_t* ctp, void* reserved, RESMGR_OCB_T* o)
{
    int i;
    pthread_mutex_lock(&g_lock);
    if (o->slot >= 0)
        --g_nodes[o->node].users[o->slot];
    for (i = 0; i < MAXARMED; ++i)
        if (g_armed[i].used && g_armed[i].o == o)
            g_armed[i].used = 0;
    pthread_mutex_unlock(&g_lock);
    return iofunc_close_ocb_default(ctp, reserved, &o->hdr);
}

static int b_stat(resmgr_context_t* ctp, io_stat_t* msg, RESMGR_OCB_T* o)
{
    struct stat st;
    int fd = ocb_fd(o);
    if (fd < 0 || fstat(fd, &st))
        st = g_nodes[o->node].st;
    msg->o = st;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int b_lseek(resmgr_context_t* ctp, io_lseek_t* msg, RESMGR_OCB_T* o)
{
    off64_t target = msg->i.whence == SEEK_CUR ? (off64_t)o->hdr.offset + msg->i.offset : msg->i.offset;
    if (msg->i.whence == SEEK_END)
        target = (off64_t)g_nodes[o->node].st.st_size + msg->i.offset;
    if (target < 0)
        return EINVAL;
    if (g_nodes[o->node].is_dir) {
        o->dirpos = 0;
        target = 0;
    }
    o->hdr.offset = target;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = target;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int b_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* o)
{
    const int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    struct node* n = &g_nodes[o->node];
    size_t want = msg->i.nbytes, used = 0;
    char* buf;
    int status = iofunc_read_verify(ctp, msg, &o->hdr, 0);
    if (status != EOK)
        return status;
    if (want > 65536)
        want = 65536;
    buf = calloc(1, want ? want : 1);
    if (!buf)
        return ENOMEM;
    if (n->is_dir) {
        while ((int)o->dirpos < n->nnames) {
            const char* name = n->names[o->dirpos];
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
    } else {
        int fd = ocb_fd(o);
        off64_t at = o->hdr.offset;
        ssize_t got;
        if (xtype == _IO_XTYPE_OFFSET)
            at = ((struct _xtype_offset*)(&msg->i + 1))->offset;
        got = fd < 0 ? -1 : (o->node == N_INFOXML ? pread(fd, buf, want, at) : read(fd, buf, want));
        if (got < 0) {
            const int e = fd < 0 ? EBADF : errno;
            free(buf);
            return e;
        }
        used = (size_t)got;
        if (xtype != _IO_XTYPE_OFFSET)
            o->hdr.offset = at + (off64_t)used;
    }
    MsgReply(ctp->rcvid, (int)used, buf, used);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int b_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* o)
{
    size_t n = msg->i.nbytes;
    char* all;
    ssize_t done;
    int fd = ocb_fd(o);
    if (fd < 0 || n > 65536)
        return EBADF;
    all = malloc(n ? n : 1);
    if (!all)
        return ENOMEM;
    if (resmgr_msgread(ctp, all, n, sizeof(msg->i)) < 0) {
        free(all);
        return EIO;
    }
    done = write(fd, all, n);
    free(all);
    if (done < 0)
        return errno;
    _IO_SET_WRITE_NBYTES(ctp, done);
    return _RESMGR_NPARTS(0);
}

static int reply_bytes(resmgr_context_t* ctp, io_devctl_t* msg, const void* data, unsigned n, int ret)
{
    memset(&msg->o, 0, sizeof(msg->o));
    if (n && data != _DEVCTL_DATA(msg->o))
        memmove(_DEVCTL_DATA(msg->o), data, n);
    msg->o.ret_val = ret;
    msg->o.nbytes = n;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

static int reply_text(resmgr_context_t* ctp, io_devctl_t* msg, const char* text, unsigned room)
{
    unsigned n = (unsigned)strlen(text) + 1;
    char* out = (char*)_DEVCTL_DATA(msg->o);
    if (!room)
        return EINVAL;
    memset(&msg->o, 0, sizeof(msg->o)); /* header only; i and o share the same place, room was read before */
    if (n > room)
        n = room;
    memmove(out, text, n);
    out[n - 1] = 0;
    msg->o.ret_val = (int)n;
    msg->o.nbytes = n;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

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

static int b_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* o)
{
    const unsigned dcmd = (unsigned)msg->i.dcmd;
    const unsigned number = dcmd & 0xffffu;
    unsigned nbytes = msg->i.nbytes;
    unsigned char* data = (unsigned char*)_DEVCTL_DATA(msg->i);
    int fd = ocb_fd(o), info = 0, rc;

    if ((dcmd & 0xff00u) == 0x0100u) /* flags of our own open file */
        return iofunc_devctl_default(ctp, msg, &o->hdr);
    if ((dcmd & 0xff00u) == 0x0e00u) {
        pthread_mutex_lock(&g_lock);
        if (!g_counts[number & 0xff]++ && g_verbose)
            note("first request 0x%04x (%08x, %u bytes) on %s from pid %d", number, dcmd, nbytes, o->rel, ctp->info.pid);
        refresh();
        /* cover: the stock driver has none */
        if (number == 0x0e34 || number == 0x0e35 || number == 0x0e36) {
            int r;
            if (!aa.valid || !aa.cover_len) {
                pthread_mutex_unlock(&g_lock);
                goto forward;
            }
            if (number == 0x0e34) {
                unsigned char e[96];
                art_entry(e);
                r = reply_bytes(ctp, msg, e, nbytes >= 96 ? 96 : 0, 1);
            } else if (number == 0x0e35) {
                o->art_pos = 0;
                r = reply_bytes(ctp, msg, 0, 0, 0);
            } else {
                const size_t total = 112 + aa.cover_len;
                unsigned char head[112];
                unsigned char* out = (unsigned char*)_DEVCTL_DATA(msg->o);
                size_t n = o->art_pos < total ? total - o->art_pos : 0, done = 0;
                if (n > nbytes)
                    n = nbytes;
                if (o->art_pos < 112) {
                    memset(head, 0, sizeof(head));
                    art_entry(head + 16);
                    done = 112 - o->art_pos < n ? 112 - o->art_pos : n;
                    memmove(out, head + o->art_pos, done);
                }
                if (n > done)
                    memmove(out + done, aa.cover + (o->art_pos + done - 112), n - done);
                memset(&msg->o, 0, sizeof(msg->o));
                if (!o->art_pos && g_verbose)
                    note("cover read by pid %d (%u bytes)", ctp->info.pid, (unsigned)total);
                o->art_pos += n;
                msg->o.ret_val = (int)n;
                msg->o.nbytes = (unsigned)n;
                r = _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
            }
            pthread_mutex_unlock(&g_lock);
            return r;
        }
        pthread_mutex_unlock(&g_lock);
    }
forward:
    if (fd < 0)
        return ENOTSUP;
    if (nbytes > 65000)
        nbytes = 65000;
    rc = devctl(fd, (int)dcmd, data, nbytes, &info);
    if (rc == EOK && (dcmd & 0xff00u) == 0x0e00u) {
        pthread_mutex_lock(&g_lock);
        if (aa.valid) {
            if (number == 0x0e19 && nbytes >= 28) { /* playback status: state, length and position from Android Auto */
                uint32_t* s = (uint32_t*)data;
                long elapsed = aa.position_s * 1000;
                if (aa.playing) {
                    const long since = (long)(time(0) - aa.stamp) * 1000;
                    if (since > 0)
                        elapsed += since;
                }
                if (aa.duration_s > 0 && elapsed > aa.duration_s * 1000)
                    elapsed = aa.duration_s * 1000;
                s[1] = aa.playing ? 1 : 2;
                if (aa.duration_s > 0) {
                    s[5] = (uint32_t)(aa.duration_s * 1000);
                    s[6] = (uint32_t)elapsed;
                }
            } else if (number == 0x0e6b && aa.duration_s > 0 && info <= 0) {
                info = (int)(aa.duration_s * 1000);
            } else if ((number == 0x0e64 || number == 0x0e65 || number == 0x0e66) && info <= 1) {
                const char* text = number == 0x0e64 ? aa.title : number == 0x0e66 ? aa.artist : aa.album;
                if (*text) {
                    int r = reply_text(ctp, msg, text, msg->i.nbytes);
                    pthread_mutex_unlock(&g_lock);
                    return r;
                }
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (rc != EOK)
        return rc;
    return reply_bytes(ctp, msg, data, nbytes, info);
}

static int b_notify(resmgr_context_t* ctp, io_notify_t* msg, RESMGR_OCB_T* o)
{
    const int action = msg->i.action, flags = msg->i.flags;
    const struct sigevent event = msg->i.event;
    int fd = ocb_fd(o), bits, i;
    if (fd < 0)
        return EBADF;
    bits = ionotify(fd, _NOTIFY_ACTION_POLL, flags, 0);
    if (bits < 0)
        return errno;
    if (!bits && (action == _NOTIFY_ACTION_POLLARM || action == _NOTIFY_ACTION_TRANARM)) {
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < MAXARMED && g_armed[i].used; ++i) {
        }
        if (i < MAXARMED) {
            g_armed[i].used = 1;
            g_armed[i].rcvid = ctp->rcvid;
            g_armed[i].fd = fd;
            g_armed[i].flags = flags;
            g_armed[i].o = o;
            g_armed[i].event = event;
        }
        pthread_mutex_unlock(&g_lock);
    }
    msg->o.flags = bits;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int b_msg(resmgr_context_t* ctp, io_msg_t* msg, RESMGR_OCB_T* o)
{
    const int len = ctp->info.msglen;
    int room = ctp->info.dstmsglen, fd = ocb_fd(o);
    unsigned char* back;
    long status;
    if (fd < 0 || len <= 0)
        return ENOSYS;
    if (room < 0 || room > 65536)
        room = 65536;
    back = calloc(1, room ? (size_t)room : 1);
    if (!back)
        return ENOMEM;
    status = MsgSend(fd, msg, len, back, room);
    if (status == -1) {
        const int e = errno;
        free(back);
        return e;
    }
    MsgReply(ctp->rcvid, status, back, room);
    free(back);
    return _RESMGR_NOREPLY;
}

int main(int argc, char** argv)
{
    const char* log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    struct sigaction sa;
    pthread_t armed;
    int i, id;
    unsigned seconds = 0;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mount"))
            g_mount = argv[i + 1];
        else if (!strcmp(argv[i], "--run"))
            g_run = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else if (!strcmp(argv[i], "--verbose"))
            g_verbose = atoi(argv[i + 1]);
        else
            return 2;
    }
    if (!g_mount || !g_run || strncmp(g_mount, "/fs/avrcp", 9) || strlen(g_mount) > 12 || strlen(g_run) > 100)
        return 2;
    if (log && !freopen(log, "a", stderr))
        return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    if (!stock_alive()) {
        note("no stock driver at %s, nothing done", g_mount);
        return 3;
    }
    if (!prepare()) {
        note("stock tree at %s incomplete, nothing done", g_mount);
        return 3;
    }
    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 70000;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = b_open;
    g_io.read = b_read;
    g_io.write = b_write;
    g_io.lseek = b_lseek;
    g_io.stat = b_stat;
    g_io.devctl = b_devctl;
    g_io.close_ocb = b_close;
    g_io.notify = b_notify;
    g_io.msg = b_msg;
    iofunc_attr_init(&g_dir_attr, S_IFDIR | 0555, 0, 0);
    iofunc_attr_init(&g_file_attr, S_IFREG | 0666, 0, 0);
    id = resmgr_attach(dpp, &rattr, g_mount, _FTYPE_ANY, _RESMGR_FLAG_DIR | _RESMGR_FLAG_BEFORE, &g_connect, &g_io,
                       &g_dir_attr);
    if (id < 0) {
        note("resmgr_attach(%s) failed: %s", g_mount, strerror(errno));
        return 5;
    }
    note("in front of the stock Bluetooth audio driver at %s", g_mount);
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
    pattr.maximum = 10;
    pool = thread_pool_create(&pattr, 0);
    if (!pool || thread_pool_start(pool) < 0) {
        resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
        return 6;
    }
    pthread_create(&armed, 0, armed_thread, 0);
    while (!g_stop) {
        sleep(1);
        if (++seconds % 2 == 0 && !stock_alive()) { /* phone gone: the place must disappear with it */
            note("stock driver left %s, stepping back", g_mount);
            break;
        }
        if (g_verbose && seconds % 60 == 0) {
            pthread_mutex_lock(&g_lock);
            note("requests so far: status %u, events? %u, song %u, cover %u", g_counts[0x19], g_counts[0x1a], g_counts[0x64],
                 g_counts[0x34]);
            pthread_mutex_unlock(&g_lock);
        }
    }
    resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
    note("detached %s", g_mount);
    return 0;
}
