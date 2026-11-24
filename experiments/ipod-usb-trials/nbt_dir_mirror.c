/* nbt_dir_mirror: research tool. Mirrors a source directory read-only at a mount point (QNX resource manager) and logs
 * everything a client asks for: every open, every read (file, offset, size), every write (with the first bytes) and
 * every devctl (number, size, first bytes of its data). Unknown devctls are answered "not supported".
 *
 * Used 2026-11-14 to see what the stock media management (mme, storage type "ipod" / mediafs) asks an iPod driver:
 * the source directory is a copy of the layout the stock driver shows under /fs/btipod (.FS_info./info.xml, control,
 * current, playback/, Music/...), mounted at /fs/ipod0, which mcd reports to mme as an inserted medium.
 *
 * Usage: nbt_dir_mirror --mount /fs/<name> --source <directory> [--log <file>]
 *
 * --forward 1 (2026-11-14): listening proxy. The source is the mount of a real driver (the stock iPod driver at
 * /fs/btipod); every devctl, message, read, write and notification request is passed on to it and logged together
 * with its answer. That shows the whole conversation between the HMI / mme and an iPod driver.
 * --override <dir>: a regular file found there under the same relative path is served instead of the source's
 * (used for .FS_info./info.xml, so that the device has its own name and serial number).
 * Writes are accepted and thrown away (so a client that writes commands into a control file goes on).
 * Runs until SIGTERM; the mount disappears when the program ends.
 */
struct m_ocb;
#define IOFUNC_OCB_T struct m_ocb
#define RESMGR_OCB_T struct m_ocb
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <devctl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/dcmd_blk.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

struct m_ocb {
    iofunc_ocb_t hdr; /* must be first */
    int is_dir, fd;
    int local;   /* served from the override directory: never forwarded */
    int special; /* neither regular file nor directory (the driver's control / event files) */
    DIR* dir;
    char pending[NAME_MAX + 1];
    int have_pending;
    unsigned index;
    char real[400], rel[260];
};

static volatile sig_atomic_t g_stop;
static const char* g_source;
static const char* g_mount_path; /* where we are mounted (for the "mounted at" answer) */
static resmgr_connect_funcs_t g_connect;
static resmgr_io_funcs_t g_io;
static iofunc_attr_t g_dir_attr, g_file_attr;
static unsigned g_lines;
static int g_single;
static iofunc_mount_t g_mnt; /* device number for the served file: the loader maps an executable and needs dev + inode */
static int g_forward;          /* --forward 1 */
static const char* g_override; /* --override <dir> */
static const char* g_answers; /* --answers <dir>: a file named like the devctl number (8 hex digits) is sent back as the answer */

static void note(const char* format, ...)
{
    va_list ap;
    time_t t = time(0);
    struct tm tm;
    if (++g_lines > (g_forward ? 30000u : 4000u))
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
    for (i = 0; i < n && i < 120 && k + 4 < max; ++i)
        k += snprintf(out + k, max - k, "%02x ", p[i]);
    out[k] = 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int m_open(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra)
{
    const char* path = msg->connect.path;
    struct stat st;
    struct m_ocb* o;
    iofunc_attr_t* attr;
    int r;
    (void)handle;
    (void)extra;
    if (strstr(path, "..") || strlen(path) > 250)
        return ENOENT;
    o = calloc(1, sizeof(*o));
    if (!o)
        return ENOMEM;
    o->fd = -1;
    snprintf(o->rel, sizeof(o->rel), "%s", *path ? path : "/");
    snprintf(o->real, sizeof(o->real), "%s%s%s", g_source, *path ? "/" : "", path);
    if (g_override && *path) {
        char other[400];
        snprintf(other, sizeof(other), "%s/%s", g_override, path);
        if (!stat(other, &st) && S_ISREG(st.st_mode)) {
            snprintf(o->real, sizeof(o->real), "%s", other);
            o->local = 1;
        }
    }
    if (stat(o->real, &st)) {
        r = errno;
        note("open-missing %s pid %d flags %x", o->rel, ctp->info.pid, msg->connect.ioflag);
        free(o);
        return r;
    }
    o->is_dir = S_ISDIR(st.st_mode);
    if (o->is_dir) {
        o->dir = opendir(o->real);
        if (o->dir && g_forward && !o->local)
            o->fd = open(o->real, O_RDONLY);
    } else {
        o->special = !S_ISREG(st.st_mode);
        if (g_forward && !o->local) /* the same kind of access the client asked for (the control file gets commands) */
            o->fd = open(o->real, (int)((msg->connect.ioflag & 3) - 1) | (msg->connect.ioflag & O_NONBLOCK));
        if (o->fd < 0)
            o->fd = open(o->real, O_RDONLY);
    }
    if (!o->dir && o->fd < 0) {
        r = errno ? errno : ENOENT;
        free(o);
        return r;
    }
    attr = o->is_dir ? &g_dir_attr : &g_file_attr;
    iofunc_attr_lock(attr);
    r = iofunc_ocb_attach(ctp, msg, &o->hdr, attr, &g_io);
    iofunc_attr_unlock(attr);
    if (r != EOK) {
        if (o->dir)
            closedir(o->dir);
        if (o->fd >= 0)
            close(o->fd);
        free(o);
        return r;
    }
    note("open %s pid %d flags %x", o->rel, ctp->info.pid, msg->connect.ioflag);
    return EOK;
}

/* forward mode: armed notifications. The real driver would wake us, not our client, so a helper thread asks the
 * driver four times a second whether the awaited condition is there and then delivers the client's event. */
struct armed {
    struct m_ocb* o;
    int rcvid, fd, flags, used;
    struct sigevent event;
};
static struct armed g_armed[32];
static pthread_mutex_t g_armed_lock = PTHREAD_MUTEX_INITIALIZER;

static int real_fd(struct m_ocb* o)
{
    return o->fd; /* directories: a second handle opened next to the listing (forward mode only) */
}

static void* armed_thread(void* arg)
{
    unsigned i;
    (void)arg;
    while (!g_stop) {
        usleep(250000);
        pthread_mutex_lock(&g_armed_lock);
        for (i = 0; i < 32; ++i) {
            int bits;
            if (!g_armed[i].used)
                continue;
            bits = ionotify(g_armed[i].fd, _NOTIFY_ACTION_POLL, g_armed[i].flags, 0);
            if (bits > 0) {
                note("notify-fired %s bits %x", g_armed[i].o->rel, (unsigned)bits);
                MsgDeliverEvent(g_armed[i].rcvid, &g_armed[i].event);
                g_armed[i].used = 0;
            }
        }
        pthread_mutex_unlock(&g_armed_lock);
    }
    return 0;
}

static int m_close(resmgr_context_t* ctp, void* reserved, RESMGR_OCB_T* o)
{
    unsigned i;
    pthread_mutex_lock(&g_armed_lock);
    for (i = 0; i < 32; ++i)
        if (g_armed[i].used && g_armed[i].o == o)
            g_armed[i].used = 0;
    pthread_mutex_unlock(&g_armed_lock);
    if (o->dir)
        closedir(o->dir);
    if (o->fd >= 0)
        close(o->fd);
    o->dir = 0;
    o->fd = -1;
    return iofunc_close_ocb_default(ctp, reserved, &o->hdr);
}

static int m_stat(resmgr_context_t* ctp, io_stat_t* msg, RESMGR_OCB_T* o)
{
    struct stat st;
    if (stat(o->real, &st))
        return errno;
    msg->o = st;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int m_lseek(resmgr_context_t* ctp, io_lseek_t* msg, RESMGR_OCB_T* o)
{
    off64_t base = 0, target;
    if (msg->i.whence == SEEK_CUR)
        base = o->hdr.offset;
    else if (msg->i.whence == SEEK_END) {
        struct stat st;
        if (o->is_dir || fstat(o->fd, &st))
            return EINVAL;
        base = st.st_size;
    }
    target = base + msg->i.offset;
    if (target < 0)
        return EINVAL;
    if (o->is_dir) {
        rewinddir(o->dir);
        o->have_pending = 0;
        o->index = 0;
        target = 0;
    }
    o->hdr.offset = target;
    if (msg->i.combine_len & _IO_COMBINE_FLAG)
        return EOK;
    msg->o = target;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static size_t put_entry(struct m_ocb* o, char* buf, size_t used, size_t max, const char* name)
{
    const size_t len = strlen(name);
    const size_t rec = (offsetof(struct dirent, d_name) + len + 1 + 7) & ~(size_t)7;
    struct dirent* d;
    if (used + rec > max)
        return 0;
    d = (struct dirent*)(buf + used);
    memset(d, 0, rec);
    d->d_ino = ++o->index;
    d->d_offset = o->index;
    d->d_reclen = (short)rec;
    d->d_namelen = (short)len;
    memcpy(d->d_name, name, len + 1);
    return rec;
}

static int m_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* o)
{
    const int xtype = msg->i.xtype & _IO_XTYPE_MASK;
    size_t want = msg->i.nbytes, used = 0;
    char* buf;
    int status = iofunc_read_verify(ctp, msg, &o->hdr, 0);
    if (status != EOK)
        return status;
    if (want > 256 * 1024)
        want = 256 * 1024;
    if (!want) {
        note("read %s pid %d want 0", o->rel, ctp->info.pid);
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    buf = malloc(want);
    if (!buf)
        return ENOMEM;
    if (o->is_dir) {
        struct dirent* e;
        if (o->have_pending) {
            used += put_entry(o, buf, used, want, o->pending);
            o->have_pending = 0;
        }
        while ((e = readdir(o->dir))) {
            const size_t rec = put_entry(o, buf, used, want, e->d_name);
            if (!rec) {
                snprintf(o->pending, sizeof(o->pending), "%s", e->d_name);
                o->have_pending = 1;
                break;
            }
            used += rec;
        }
    } else {
        off64_t at = o->hdr.offset;
        ssize_t n;
        if (xtype == _IO_XTYPE_OFFSET)
            at = ((struct _xtype_offset*)(&msg->i + 1))->offset;
        if (o->special && g_forward) { /* event / control file of the real driver: plain read, its error passed on */
            n = read(o->fd, buf, want);
            if (n < 0) {
                const int e = errno;
                note("read %s pid %d want %u -> error %d", o->rel, ctp->info.pid, (unsigned)msg->i.nbytes, e);
                free(buf);
                return e;
            }
            if (n > 0) {
                char text[400];
                hex(text, sizeof(text), (const unsigned char*)buf, (size_t)n);
                note("read-data %s: %s", o->rel, text);
            }
        } else
            n = pread(o->fd, buf, want, at);
        if (n < 0)
            n = 0;
        used = (size_t)n;
        if (xtype != _IO_XTYPE_OFFSET)
            o->hdr.offset = at + n;
        note("read %s pid %d at %lld want %u got %u", o->rel, ctp->info.pid, (long long)at, (unsigned)msg->i.nbytes,
             (unsigned)used);
    }
    MsgReply(ctp->rcvid, (int)used, buf, used);
    free(buf);
    return _RESMGR_NOREPLY;
}

static int m_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* o)
{
    char text[200], raw[96];
    size_t n = msg->i.nbytes, k, i;
    k = n < sizeof(raw) ? n : sizeof(raw);
    if (resmgr_msgread(ctp, raw, k, sizeof(msg->i)) < 0)
        k = 0;
    hex(text, sizeof(text), (const unsigned char*)raw, k);
    for (i = 0; i < k; ++i)
        if (raw[i] < 32 || raw[i] > 126)
            raw[i] = '.';
    note("write %s pid %d bytes %u: %.*s | %s", o->rel, ctp->info.pid, (unsigned)n, (int)k, raw, text);
    if (g_forward && !o->local && !o->is_dir && n <= 65536) {
        char* all = malloc(n ? n : 1);
        ssize_t done;
        if (!all)
            return ENOMEM;
        if (resmgr_msgread(ctp, all, n, sizeof(msg->i)) < 0) {
            free(all);
            return EIO;
        }
        done = write(o->fd, all, n);
        free(all);
        if (done < 0) {
            const int e = errno;
            note("write-forwarded %s -> error %d", o->rel, e);
            return e;
        }
        n = (size_t)done;
    }
    _IO_SET_WRITE_NBYTES(ctp, n);
    return _RESMGR_NPARTS(0);
}

static int m_devctl(resmgr_context_t* ctp, io_devctl_t* msg, RESMGR_OCB_T* o)
{
    char text[200];
    hex(text, sizeof(text), (const unsigned char*)_DEVCTL_DATA(msg->i), msg->i.nbytes);
    note("devctl %s pid %d dcmd %08x nbytes %u: %s", o->rel, ctp->info.pid, (unsigned)msg->i.dcmd, (unsigned)msg->i.nbytes,
         text);
    /* The answers of the stock iPod driver, asked on the car 2026-11-14 at /fs/btipod: file system "ipod", read only,
     * no blocks; "mounted at" with the network prefix and a trailing slash. The media application of the HMI stopped
     * after the unanswered file system question. */
    if (g_mount_path && !strncmp(g_mount_path, "/fs/ipod", 8)) {
        const char* text = 0;
        char at[128];
        if (msg->i.dcmd == DCMD_FSYS_STATVFS) {
            struct statvfs* v = (struct statvfs*)_DEVCTL_DATA(msg->o);
            memset(v, 0, sizeof(*v));
            v->f_bsize = v->f_frsize = 512;
            v->f_files = 1024;
            v->f_ffree = v->f_favail = 996;
            v->f_fsid = 1;
            v->f_namemax = 1024;
            v->f_flag = ST_RDONLY;
            strcpy(v->f_basetype, "ipod");
            msg->o.ret_val = 0;
            msg->o.nbytes = sizeof(*v);
            note("devctl-answered file system statistics");
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*v));
        }
        if ((unsigned)msg->i.dcmd == 0x40040103u) { /* "mount flags": read only */
            *(int*)_DEVCTL_DATA(msg->o) = 1;
            msg->o.ret_val = 0;
            msg->o.nbytes = sizeof(int);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(int));
        }
        if (msg->i.dcmd == DCMD_FSYS_MOUNTED_ON)
            text = "/net/hu-intel.nbt/dev/blk/file-2-ipod-8-media";
        else if (msg->i.dcmd == DCMD_FSYS_MOUNTED_AT) {
            snprintf(at, sizeof(at), "/net/hu-intel.nbt%s/", g_mount_path);
            text = at;
        }
        if (text) {
            char* out = (char*)_DEVCTL_DATA(msg->o);
            memset(out, 0, 256);
            strncpy(out, text, 255);
            msg->o.ret_val = 0;
            msg->o.nbytes = 256;
            note("devctl-answered %s", text);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + 256);
        }
        if (msg->i.dcmd == DCMD_FSYS_MOUNTED_BY)
            return ENODEV;
    }
    /* "get / set the flags of this open file" belongs to our own handle, not to the driver's (passed on, mme could no
     * longer list /Music/Albums: "Not a directory", car 2026-11-14) */
    if (g_forward && ((unsigned)msg->i.dcmd & 0xff00u) == 0x0100u)
        return iofunc_devctl_default(ctp, msg, &o->hdr);
    if (g_forward && !o->local && real_fd(o) >= 0) {
        const int dcmd = msg->i.dcmd;
        unsigned n = msg->i.nbytes;
        int info = 0, rc;
        char back[400];
        if (n > 60000)
            n = 60000;
        rc = devctl(real_fd(o), dcmd, _DEVCTL_DATA(msg->i), n, &info);
        hex(back, sizeof(back), (const unsigned char*)_DEVCTL_DATA(msg->i), n);
        note("devctl-forwarded %08x -> rc %d info %d: %s", (unsigned)dcmd, rc, info, rc ? "" : back);
        if (rc)
            return rc;
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = info;
        msg->o.nbytes = n;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
    }
    if (g_answers) {
        char path[300];
        int fd;
        snprintf(path, sizeof(path), "%s/%08x", g_answers, (unsigned)msg->i.dcmd);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            const unsigned room = msg->i.nbytes < 7000 ? (unsigned)msg->i.nbytes : 7000u;
            char* out = (char*)_DEVCTL_DATA(msg->o);
            ssize_t n;
            memset(out, 0, room);
            n = read(fd, out, room);
            close(fd);
            if (n < 0)
                n = 0;
            msg->o.ret_val = (int)n;
            msg->o.nbytes = room;
            note("devctl-answered %08x with %d bytes", (unsigned)msg->i.dcmd, (int)n);
            return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + room);
        }
    }
    return iofunc_devctl_default(ctp, msg, &o->hdr);
}

/* logging wrappers around the library defaults (to see which request of the program loader fails) */
static int m_mmap(resmgr_context_t* ctp, io_mmap_t* msg, RESMGR_OCB_T* o)
{
    const unsigned prot = msg->i.prot;
    const long long off = (long long)msg->i.offset;
    int r = iofunc_mmap_default(ctp, msg, &o->hdr);
    note("mmap %s pid %d prot %x offset %lld -> %d", o->rel, ctp->info.pid, prot, off, r);
    return r;
}

static int m_fdinfo(resmgr_context_t* ctp, io_fdinfo_t* msg, RESMGR_OCB_T* o)
{
    int r = iofunc_fdinfo_default(ctp, msg, &o->hdr);
    note("fdinfo %s pid %d -> %d", o->rel, ctp->info.pid, r);
    return r;
}

static int m_openfd(resmgr_context_t* ctp, io_openfd_t* msg, RESMGR_OCB_T* o)
{
    int r = iofunc_openfd_default(ctp, msg, &o->hdr);
    note("openfd %s pid %d -> %d", o->rel, ctp->info.pid, r);
    return r;
}

/* requests the mirror has no answer for yet: noted, then refused, to learn what the media application wants from the
 * iPod's control and event files */
static int m_notify(resmgr_context_t* ctp, io_notify_t* msg, RESMGR_OCB_T* o)
{
    note("notify %s pid %d action %d flags %x", o->rel, ctp->info.pid, (int)msg->i.action, (unsigned)msg->i.flags);
    if (g_forward && !o->local && real_fd(o) >= 0) {
        const int action = msg->i.action, flags = msg->i.flags;
        const struct sigevent event = msg->i.event;
        int bits = ionotify(real_fd(o), _NOTIFY_ACTION_POLL, flags, 0);
        unsigned i;
        if (bits < 0) {
            const int e = errno;
            note("notify-forwarded -> error %d", e);
            return e;
        }
        note("notify-forwarded -> bits %x", (unsigned)bits);
        if (!bits && (action == _NOTIFY_ACTION_POLLARM || action == _NOTIFY_ACTION_TRANARM)) {
            pthread_mutex_lock(&g_armed_lock);
            for (i = 0; i < 32 && g_armed[i].used; ++i) {
            }
            if (i < 32) {
                g_armed[i].o = o;
                g_armed[i].rcvid = ctp->rcvid;
                g_armed[i].fd = real_fd(o);
                g_armed[i].flags = flags;
                g_armed[i].event = event;
                g_armed[i].used = 1;
            }
            pthread_mutex_unlock(&g_armed_lock);
        }
        msg->o.flags = bits;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    return ENOSYS;
}

static int m_msg(resmgr_context_t* ctp, io_msg_t* msg, RESMGR_OCB_T* o)
{
    char text[400];
    int len = ctp->info.msglen;
    hex(text, sizeof(text), (const unsigned char*)msg, len > 0 && len < 120 ? (unsigned)len : 120u);
    note("msg %s pid %d length %d (sent %d, reply room %d): %s", o->rel, ctp->info.pid, len, (int)ctp->info.srcmsglen,
         (int)ctp->info.dstmsglen, text);
    /* Message 0x80 ("tell me about events") is not passed on: the driver would send its events to this program, not to
     * the media application, which then waits for ever and does not list the device (car 2026-11-14). Refused, the
     * application falls back to asking by itself and lists the device. */
    if (g_forward && len >= 8 && ((const unsigned char*)msg)[6] == 0x80)
        return ENOSYS;
    if (g_forward && !o->local && real_fd(o) >= 0 && len > 0) {
        int room = ctp->info.dstmsglen;
        unsigned char* back;
        long status;
        if (room < 0 || room > 65536)
            room = 65536;
        back = calloc(1, room ? (size_t)room : 1);
        if (!back)
            return ENOMEM;
        status = MsgSend(real_fd(o), msg, len, back, room);
        if (status == -1) {
            const int e = errno;
            note("msg-forwarded -> error %d", e);
            free(back);
            return e;
        }
        hex(text, sizeof(text), back, (size_t)room);
        note("msg-forwarded -> status %ld reply: %s", status, text);
        MsgReply(ctp->rcvid, status, back, room);
        free(back);
        return _RESMGR_NOREPLY;
    }
    return ENOSYS;
}

static int m_unblock(resmgr_context_t* ctp, io_pulse_t* msg, RESMGR_OCB_T* o)
{
    note("unblock %s pid %d", o->rel, ctp->info.pid);
    return iofunc_unblock_default(ctp, msg, &o->hdr);
}

int main(int argc, char** argv)
{
    const char *mount = 0, *log = 0;
    dispatch_t* dpp;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t* pool;
    struct sigaction sa;
    struct stat st;
    int i, id;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mount"))
            mount = argv[i + 1];
        else if (!strcmp(argv[i], "--source"))
            g_source = argv[i + 1];
        else if (!strcmp(argv[i], "--answers"))
            g_answers = argv[i + 1];
        else if (!strcmp(argv[i], "--forward"))
            g_forward = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--override"))
            g_override = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))
            log = argv[i + 1];
        else
            return 2;
    }
    /* A regular file as source: that one file is served at the mount path, in front of whatever is there (used to put
     * our own program where the USB launcher looks for the stock iPod driver). */
    g_mount_path = mount;
    if (!mount || !g_source || (strncmp(mount, "/fs/", 4) && strcmp(mount, "/opt/share/bin/io-fs-media")) ||
        strstr(mount, "..") || strlen(g_source) > 120 || stat(g_source, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
        return 2;
    g_single = S_ISREG(st.st_mode);
    if (log && !freopen(log, "a", stderr))
        return 2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    dpp = dispatch_create();
    if (!dpp)
        return 4;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 70000;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect, _RESMGR_IO_NFUNCS, &g_io);
    g_connect.open = m_open;
    g_io.read = m_read;
    g_io.write = m_write;
    g_io.lseek = m_lseek;
    g_io.stat = m_stat;
    g_io.devctl = m_devctl;
    g_io.close_ocb = m_close;
    g_io.mmap = m_mmap;
    g_io.fdinfo = m_fdinfo;
    g_io.openfd = m_openfd;
    g_io.notify = m_notify;
    g_io.msg = m_msg;
    g_io.unblock = m_unblock;
    iofunc_attr_init(&g_dir_attr, S_IFDIR | 0777, 0, 0);
    iofunc_attr_init(&g_file_attr, S_IFREG | 0777, 0, 0);
    id = resmgr_attach(dpp, &rattr, mount, _FTYPE_ANY, g_single ? _RESMGR_FLAG_BEFORE : _RESMGR_FLAG_DIR, &g_connect, &g_io,
                       g_single ? &g_file_attr : &g_dir_attr);
    if (id < 0) {
        fprintf(stderr, "resmgr_attach(%s) failed: %s\n", mount, strerror(errno));
        return 5;
    }
    if (g_single) {
        /* executable served from here: mmap() of it needs a mount structure, a device/inode pair and the size */
        g_file_attr.mount = &g_mnt;
        resmgr_devino(id, &g_mnt.dev, &g_file_attr.inode);
        g_file_attr.nbytes = st.st_size;
    }
    note("attached %s", mount);
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
    if (g_forward) {
        pthread_t t;
        pthread_create(&t, 0, armed_thread, 0);
    }
    while (!g_stop)
        sleep(1);
    resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
    note("detached %s", mount);
    return 0;
}
