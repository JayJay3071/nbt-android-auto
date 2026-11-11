// aa_video_capture: runs one Android Auto session (the NBT-AAD core, nbt::AASession) over the phone's USB accessory link
// and takes care of what the head unit needs from it. Part of the oneplus_native program (--capture-video).
//
// Two modes:
//   capture mode (diagnostics)  H.264 frames go to <prefix>.h264 with an index <prefix>.csv; bounded by time, frame
//                               count and 8 MB.
//   live mode (the product)     NBT_AA_LIVE_FIFO is set by native_service: H.264 goes into annexb.fifo for the decoder
//                               (via a queue and a writer thread), iDrive controller keys come in from input.fifo,
//                               navigation guidance goes out to nav.state for nbt_cluster. Runs until SIGTERM.
//
// Data flow:
//   USB (transfer callback) --reader thread--> socket pair --> AASession (TLS, protocol)
//   AASession video callback --> save() --> live queue --> live_writer thread --> annexb.fifo --> fifo_http --> decoder
//   input.fifo (nbt_knob) --> handle_input() --> AASession key / rotary events
//   AASession navigation callback --> nav_status() --> /fs/ram3/nbt-aa-native/nav.state
#include "aa_video_capture.h"
#include "aa_session.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <deque>
#include <vector>

namespace {

// ===== Android Auto navigation status (channel 10) -> nav.state for nbt_cluster (2026-11-08) ==========================
//
// Message ids and protobuf field numbers (aasdk navigationstatus):
//   0x8003 NavigationStatus          {1 status: 1 active, 2 inactive, 3 rerouting}
//   0x8006 NavigationState           {1 steps[] {1 maneuver {1 type, 2 exit_number, 3 exit_angle}, 2 road {1 name},
//                                                3 lanes[] {1 directions[] {1 shape, 2 highlighted}}}}
//   0x8007 NavigationCurrentPosition {1 step_distance {1 distance {1 meters, 2 display_value, 3 display_units}}}
// older phones:
//   0x8004 NextTurnEvent             {1 road, 2 turn_side, 3 event, 6 exit_angle}
//   0x8005 NextTurnDistanceEvent     {1 distance_m, 3 display_distance_e3, 4 unit}
// Maneuver type = androidx.car.app.navigation.model.Maneuver TYPE_*.
// nav.state is written atomically (tmp + rename) whenever something changes; the file is small and in RAM.
enum NavMessage {
    kNavStatus = 0x8003,
    kNextTurnEvent = 0x8004,         // old phones
    kNextTurnDistanceEvent = 0x8005, // old phones
    kNavState = 0x8006,
    kNavCurrentPosition = 0x8007
};

struct NavInfo {
    int status;         // 1 active, 2 inactive, 3 rerouting (0 = nothing received yet)
    int type;           // maneuver type of the next step
    int unit;           // display unit of dist_text (1 m, 2/3 km, 4/5 mi, 6 ft, 7 yd)
    int type2;          // maneuver type of the step after (-1 = none)
    long long dist_m;   // distance to the next maneuver in metres (-1 = unknown)
    long long exit_angle;
    bool has_angle;
    std::string road, dist_text, road2, lanes; // lanes: "4h,1o,1o" (see nav_status)
    unsigned seq;       // incremented on every write
    NavInfo() : status(0), type(0), unit(0), type2(-1), dist_m(-1), exit_angle(0), has_angle(false), seq(0) {}
};
NavInfo g_nav;

// ---- minimal protobuf reader (no generated code on the head unit) ----

// one varint at p (advances p); false if it runs past e
bool pb_varint(const uint8_t*& p, const uint8_t* e, unsigned long long& v)
{
    v = 0;
    for (int sh = 0; p < e && sh < 64; sh += 7) {
        uint8_t b = *p++;
        v |= (unsigned long long)(b & 0x7f) << sh;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

// First occurrence of field num in a message: varint -> *v, length-delimited -> *d / *n.
// Returns the wire type (0 or 2) or -1 if the field is missing or the message is malformed.
int pb_field(const uint8_t* p, size_t len, unsigned num, unsigned long long* v, const uint8_t** d, size_t* n)
{
    const uint8_t* e = p + len;
    while (p < e) {
        unsigned long long tag, x;
        if (!pb_varint(p, e, tag))
            return -1;
        unsigned w = (unsigned)(tag & 7), f = (unsigned)(tag >> 3); // wire type, field number
        if (w == 0) { // varint
            if (!pb_varint(p, e, x))
                return -1;
            if (f == num) {
                if (v)
                    *v = x;
                return 0;
            }
        } else if (w == 2) { // length-delimited
            if (!pb_varint(p, e, x) || x > (unsigned long long)(e - p))
                return -1;
            if (f == num) {
                if (d)
                    *d = p;
                if (n)
                    *n = (size_t)x;
                return 2;
            }
            p += x;
        } else if (w == 1) { // 64 bit
            if (e - p < 8)
                return -1;
            p += 8;
        } else if (w == 5) { // 32 bit
            if (e - p < 4)
                return -1;
            p += 4;
        } else
            return -1;
    }
    return -1;
}

// sub-message (or bytes) field num
bool pb_sub(const uint8_t* p, size_t len, unsigned num, const uint8_t** d, size_t* n)
{
    return pb_field(p, len, num, 0, d, n) == 2;
}

// k-th (0-based) length-delimited occurrence of field num (repeated sub-messages such as NavigationState.steps)
bool pb_sub_nth(const uint8_t* p, size_t len, unsigned num, unsigned k, const uint8_t** d, size_t* n)
{
    const uint8_t* e = p + len;
    while (p < e) {
        unsigned long long tag, x;
        if (!pb_varint(p, e, tag))
            return false;
        unsigned w = (unsigned)(tag & 7), f = (unsigned)(tag >> 3);
        if (w == 0) {
            if (!pb_varint(p, e, x))
                return false;
        } else if (w == 2) {
            if (!pb_varint(p, e, x) || x > (unsigned long long)(e - p))
                return false;
            if (f == num) {
                if (!k) {
                    *d = p;
                    *n = (size_t)x;
                    return true;
                }
                --k;
            }
            p += x;
        } else if (w == 1) {
            if (e - p < 8)
                return false;
            p += 8;
        } else if (w == 5) {
            if (e - p < 4)
                return false;
            p += 4;
        } else
            return false;
    }
    return false;
}

// integer field num
bool pb_int(const uint8_t* p, size_t len, unsigned num, long long* v)
{
    unsigned long long x;
    if (pb_field(p, len, num, &x, 0, 0) != 0)
        return false;
    *v = (long long)x;
    return true;
}

// string field num, control characters replaced by spaces ("" if missing)
std::string pb_str(const uint8_t* p, size_t len, unsigned num)
{
    const uint8_t* d;
    size_t n;
    if (pb_field(p, len, num, 0, &d, &n) != 2)
        return std::string();
    std::string s((const char*)d, n);
    for (size_t i = 0; i < s.size(); ++i)
        if ((unsigned char)s[i] < 0x20)
            s[i] = ' ';
    return s;
}

// ---- nav.state ----

void nav_write()
{
    char buf[512];
    ++g_nav.seq;
    int n = snprintf(buf, sizeof(buf),
                     "seq=%u\nstatus=%d\ntype=%d\nangle=%lld\nhas_angle=%d\ndist_m=%lld\ndist_text=%s\nunit=%d\nroad=%s\n"
                     "type2=%d\nroad2=%s\nlanes=%s\n",
                     g_nav.seq, g_nav.status, g_nav.type, g_nav.exit_angle, g_nav.has_angle ? 1 : 0, g_nav.dist_m,
                     g_nav.dist_text.c_str(), g_nav.unit, g_nav.road.substr(0, 120).c_str(), g_nav.type2,
                     g_nav.road2.substr(0, 120).c_str(), g_nav.lanes.c_str());
    if (n <= 0)
        return;
    int fd = open("/fs/ram3/nbt-aa-native/nav.state.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    if (w == n)
        rename("/fs/ram3/nbt-aa-native/nav.state.tmp", "/fs/ram3/nbt-aa-native/nav.state");
}

// AASession navigation callback: one message of the navigation status channel
void nav_status(uint16_t id, const uint8_t* b, size_t len, void*)
{
    long long x;
    const uint8_t* d;
    size_t n;
    if (id == kNavStatus) {
        if (pb_int(b, len, 1, &x))
            g_nav.status = (int)x;
    } else if (id == kNavState) {
        // first step = the next maneuver
        const uint8_t* st;
        size_t sn;
        if (!pb_sub(b, len, 1, &st, &sn))
            return;
        if (pb_sub(st, sn, 1, &d, &n)) { // maneuver
            g_nav.type = 0;
            g_nav.has_angle = false;
            if (pb_int(d, n, 1, &x))
                g_nav.type = (int)x;
            if (pb_int(d, n, 3, &x)) {
                g_nav.exit_angle = x;
                g_nav.has_angle = true;
            }
        }
        if (pb_sub(st, sn, 2, &d, &n)) // road
            g_nav.road = pb_str(d, n, 1);
        // lanes of this step: field 3 repeated {1 repeated LaneDirection {1 shape, 2 highlighted}} -> "4h,1o,1o" (left to
        // right; per lane the highlighted direction, else the first; shape 1 straight, 2/3 slight, 4/5 normal, 6/7 sharp,
        // 8/9 u-turn left/right; h = highlighted, o = other)
        g_nav.lanes.clear();
        {
            const uint8_t* ln;
            size_t lnn;
            for (unsigned li = 0; li < 10 && pb_sub_nth(st, sn, 3, li, &ln, &lnn); ++li) {
                int shape = -1;
                bool hl = false;
                const uint8_t* dd;
                size_t dn;
                for (unsigned di = 0; di < 6 && pb_sub_nth(ln, lnn, 1, di, &dd, &dn); ++di) {
                    long long sh = 0, h = 0;
                    pb_int(dd, dn, 1, &sh);
                    pb_int(dd, dn, 2, &h);
                    if (h) {
                        shape = (int)sh;
                        hl = true;
                        break;
                    }
                    if (shape < 0)
                        shape = (int)sh;
                }
                char t[8];
                snprintf(t, sizeof(t), "%s%d%c", g_nav.lanes.empty() ? "" : ",", shape < 0 ? 0 : shape, hl ? 'h' : 'o');
                g_nav.lanes += t;
            }
        }
        // second step = the maneuver after the next one
        g_nav.type2 = -1;
        g_nav.road2.clear();
        const uint8_t* st2;
        size_t sn2;
        if (pb_sub_nth(b, len, 1, 1, &st2, &sn2)) {
            g_nav.type2 = 0;
            if (pb_sub(st2, sn2, 1, &d, &n) && pb_int(d, n, 1, &x))
                g_nav.type2 = (int)x;
            if (pb_sub(st2, sn2, 2, &d, &n))
                g_nav.road2 = pb_str(d, n, 1);
        }
        if (!g_nav.status)
            g_nav.status = 1;
    } else if (id == kNavCurrentPosition) {
        const uint8_t* sd;
        size_t sdn;
        if (pb_sub(b, len, 1, &sd, &sdn) && pb_sub(sd, sdn, 1, &d, &n)) { // step_distance.distance
            if (pb_int(d, n, 1, &x))
                g_nav.dist_m = x;
            g_nav.dist_text = pb_str(d, n, 2);
            if (pb_int(d, n, 3, &x))
                g_nav.unit = (int)x;
        }
    } else if (id == kNextTurnEvent) {
        // old event numbers -> maneuver type for a left / right turn: {event, type if left, type if right}
        static const int legacy[][3] = {{1, 1, 1},    {2, 2, 2},    {3, 5, 6},    {4, 7, 8},    {5, 9, 10},
                                        {6, 11, 12},  {7, 15, 16},  {8, 23, 24},  {9, 25, 26},  {10, 27, 28},
                                        {11, 34, 32}, {12, 34, 32}, {13, 34, 32}, {14, 36, 36}, {16, 37, 37},
                                        {17, 38, 38}, {19, 39, 39}};
        long long side = 0, ev = 0;
        bool hs = pb_int(b, len, 2, &side); // turn_side: 1 left, 2 right
        int pl = 0, pr = 0;
        if (pb_int(b, len, 3, &ev))
            for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); ++i)
                if (legacy[i][0] == ev) {
                    pl = legacy[i][1];
                    pr = legacy[i][2];
                }
        g_nav.type = hs && side == 1 ? pl : hs && side == 2 ? pr : (pl == pr ? pl : 0);
        g_nav.road = pb_str(b, len, 1);
        g_nav.has_angle = pb_int(b, len, 6, &x);
        if (g_nav.has_angle)
            g_nav.exit_angle = x;
        if (!g_nav.status)
            g_nav.status = 1;
    } else if (id == kNextTurnDistanceEvent) {
        if (pb_int(b, len, 1, &x))
            g_nav.dist_m = x;
        if (pb_int(b, len, 4, &x))
            g_nav.unit = (int)x;
        if (pb_int(b, len, 3, &x)) { // display distance in 1/1000 of the unit
            char t[32];
            if (g_nav.unit == 3 || g_nav.unit == 5) // km / mi with one decimal
                snprintf(t, sizeof(t), "%lld.%lld", x / 1000, (x % 1000) / 100);
            else
                snprintf(t, sizeof(t), "%lld", (x + 500) / 1000);
            g_nav.dist_text = t;
        }
    } else
        return;
    printf("[NAV] msg 0x%04X status=%d type=%d angle=%lld dist=%lldm '%s' road='%s'\n", id, g_nav.status, g_nav.type,
           g_nav.has_angle ? g_nav.exit_angle : -1LL, g_nav.dist_m, g_nav.dist_text.c_str(), g_nav.road.c_str());
    nav_write();
}

} // namespace

namespace {

// ===== video: session thread -> queue -> writer thread -> decoder FIFO =================================================
//
// Live mode (car 2026-11-07): writing H.264 into the decoder FIFO directly from the AA session thread blocked that thread
// whenever the decoder fell behind (up to 2 s per frame). Media acks and controller keys then queued behind it (3-4 s input
// lag), the phone throttled the video and finally ended the session. Now the session thread only queues the access unit;
// a writer thread feeds the FIFO. When the queue exceeds its budget the frames are dropped up to the next IDR picture, so
// the decoder always resumes on a clean picture and the session thread never waits.
struct LiveQueue {
    pthread_mutex_t m;
    pthread_cond_t cv;
    std::deque<std::vector<uint8_t> > items; // H.264 access units
    size_t bytes;                            // total size of items
    bool stop, dropping;                     // dropping: skipping frames until the next IDR / config
    unsigned dropped, drops;                 // dropped frames, number of drop runs
    LiveQueue() : bytes(0), stop(false), dropping(false), dropped(0), drops(0)
    {
        pthread_mutex_init(&m, 0);
        pthread_cond_init(&cv, 0);
    }
    ~LiveQueue()
    {
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&m);
    }
};
const size_t kLiveBudget = 3 * 1024 * 1024; // queue limit in bytes

struct Capture {
    pthread_mutex_t mutex;
    nbt_capture_transfer transfer; // USB transfer callback from oneplus_aoa.c
    void* transport;
    int socket;      // our end of the socket pair to the session
    int data;        // capture file or live FIFO
    int index;       // capture mode: .csv index
    int error;
    size_t bytes;
    unsigned frames, configs, frame_limit;
    bool live;
    LiveQueue queue;
    Capture() : socket(-1), data(-1), index(-1), error(0), bytes(0), frames(0), configs(0), frame_limit(300), live(false)
    {
        pthread_mutex_init(&mutex, 0);
    }
    ~Capture() { pthread_mutex_destroy(&mutex); }
};

// Annex-B access unit contains an IDR slice (NAL type 5)?
bool has_idr(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i + 3 < n; ++i)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5)
            return true;
    return false;
}

// session thread: never blocks on the decoder
void live_enqueue(Capture* c, const uint8_t* p, size_t n, bool config)
{
    LiveQueue& q = c->queue;
    pthread_mutex_lock(&q.m);
    const bool restart = config || has_idr(p, n);
    if (q.dropping && !restart) {
        ++q.dropped;
        pthread_mutex_unlock(&q.m);
        return;
    }
    if (q.bytes + n > kLiveBudget) {
        if (!restart) {
            q.dropping = true;
            ++q.drops;
            ++q.dropped;
            pthread_mutex_unlock(&q.m);
            return;
        }
        // a fresh IDR/config does not fit: throw away the old backlog, the decoder restarts on this picture
        while (!q.items.empty() && q.bytes + n > kLiveBudget) {
            q.bytes -= q.items.front().size();
            q.items.pop_front();
            ++q.dropped;
        }
    }
    q.dropping = false;
    q.items.push_back(std::vector<uint8_t>(p, p + n));
    // NBT_AA_PAD (bytes, car 2026-11-07): a static Android Auto screen is only ~600 bytes per frame (~16 KB/s), so any
    // byte-sized input buffer of the decoder chain (HTTP client, demuxer) delays the picture by seconds. Each access
    // unit is padded with one H.264 filler NAL (type 12, ignored by decoders) up to this size.
    static long pad = -1;
    if (pad < 0) {
        const char* e = getenv("NBT_AA_PAD");
        pad = e ? strtol(e, 0, 10) : 0;
        if (pad < 0 || pad > 65536)
            pad = 0;
    }
    if (!config && pad > 0 && n + 6 < (size_t)pad) {
        std::vector<uint8_t>& v = q.items.back();
        const size_t fill = (size_t)pad - n - 6;
        static const uint8_t head[5] = {0, 0, 0, 1, 0x0c}; // start code + NAL header type 12 (filler data)
        v.insert(v.end(), head, head + 5);
        v.insert(v.end(), fill, (uint8_t)0xff);
        v.push_back(0x80); // rbsp trailing bits
    }
    q.bytes += q.items.back().size();
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.m);
}

long long now_ms();

// write everything; a non-blocking descriptor may stay full for at most 2 s
bool write_all(int fd, const unsigned char* p, size_t n)
{
    long long deadline = 0;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN) {
            if (!deadline)
                deadline = now_ms() + 2000;
            if (now_ms() >= deadline)
                return false;
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            timeval t = {0, 100000};
            if (select(fd + 1, 0, &set, 0, &t) < 0 && errno != EINTR)
                return false;
            continue;
        }
        if (k <= 0)
            return false;
        p += k;
        n -= k;
    }
    return true;
}

// one H.264 access unit (or codec config) from the session
void save(Capture* c, const uint8_t* p, size_t n, uint64_t pts, bool config)
{
    pthread_mutex_lock(&c->mutex);
    if (!c->error && c->frames < c->frame_limit) {
        const bool annexb = n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1));
        if (!annexb || n > 2 * 1024 * 1024 || (!c->live && n > 8 * 1024 * 1024 - c->bytes))
            c->error = 1;
        else {
            char row[160];
            int k = snprintf(row, sizeof(row), "%s,%u,%llu,%u,%u\n", config ? "config" : "frame", c->frames,
                             (unsigned long long)pts, (unsigned)c->bytes, (unsigned)n);
            if (c->live)
                live_enqueue(c, p, n, config);
            else if (!write_all(c->data, p, n) || (c->index >= 0 && !write_all(c->index, (const unsigned char*)row, k)))
                c->error = 1;
            else {
                c->bytes += n;
                if (config)
                    ++c->configs;
                else
                    ++c->frames;
            }
        }
    }
    pthread_mutex_unlock(&c->mutex);
}

// AASession video frame callback
void video(uint32_t, const uint8_t* p, size_t n, uint64_t pts, void* v)
{
    save((Capture*)v, p, n, pts, false);
}

// The phone asked for the screen back (car 2026-11-10: the "exit to car" icon in the Android Auto rail, picked with the
// controller): it stops the video, our decoder ran dry ("stream inactive"), the service ended the session and the phone
// refused the reconnect - the "connected but nothing works" drop-out. This head unit has no native screen to hand back
// to (MENU leaves Android Auto), so the projection is given back to the phone half a second later and AA simply goes on.
volatile long long g_refocus_at = 0; // monotonic ms at which to send video focus PROJECTED again (0 = nothing pending)

void focus_request(bool projected, uint32_t reason, void*)
{
    if (projected)
        return;
    printf("AA_FOCUS phone asked for the native screen (reason %u): giving the projection back in 0.5 s\n",
           (unsigned)reason);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    g_refocus_at = (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000 + 500;
}

// AASession codec config callback (SPS/PPS)
void config(const uint8_t* p, size_t n, void* v)
{
    save((Capture*)v, p, n, 0, true);
}

// reader thread: USB -> socket pair (the session reads the other end)
void* reader(void* v)
{
    Capture* c = (Capture*)v;
    unsigned char b[65536];
    for (;;) {
        unsigned n = sizeof(b);
        if (!c->transfer(c->transport, 1, b, &n) || !n || n > sizeof(b) || !write_all(c->socket, b, n))
            break;
    }
    shutdown(c->socket, SHUT_RDWR);
    return 0;
}

long long now_ms()
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// live mode writer: queue -> decoder FIFO. Waits as long as the decoder needs (the queue absorbs it), stops on a hard error.
void* live_writer(void* v)
{
    Capture* c = (Capture*)v;
    LiveQueue& q = c->queue;
    for (;;) {
        pthread_mutex_lock(&q.m);
        while (q.items.empty() && !q.stop)
            pthread_cond_wait(&q.cv, &q.m);
        if (q.stop) {
            pthread_mutex_unlock(&q.m);
            return 0;
        }
        std::vector<uint8_t> item;
        item.swap(q.items.front());
        q.items.pop_front();
        q.bytes -= item.size();
        pthread_mutex_unlock(&q.m);
        size_t off = 0;
        while (off < item.size()) {
            ssize_t k = write(c->data, &item[off], item.size() - off);
            if (k > 0) {
                off += (size_t)k;
                continue;
            }
            if (k < 0 && errno == EINTR)
                continue;
            if (k < 0 && errno == EAGAIN) { // FIFO full: wait for the decoder, unless we are stopping
                pthread_mutex_lock(&q.m);
                bool stop = q.stop;
                pthread_mutex_unlock(&q.m);
                if (stop)
                    return 0;
                fd_set set;
                FD_ZERO(&set);
                FD_SET(c->data, &set);
                timeval t = {0, 100000};
                select(c->data + 1, 0, &set, 0, &t);
                continue;
            }
            pthread_mutex_lock(&c->mutex);
            c->error = 1;
            pthread_mutex_unlock(&c->mutex);
            printf("AA_LIVE_WRITER_FAILED errno=%d\n", errno);
            return 0;
        }
    }
}

volatile sig_atomic_t live_stop = 0;

void stop_live(int)
{
    live_stop = 1;
}

// ===== iDrive controller input (live mode) =============================================================================
//
// nbt_knob writes "R <key> <delta>" (rotary) / "K <key> <0|1>" (key up/down) lines into this FIFO.
// Opened O_RDWR so the FIFO never reports EOF when nbt_knob is not running. Only the keys declared on input channel 7
// are accepted: HOME 3, BACK 4, CALL 5, DPAD 19..23, SEARCH 84, ROTARY 65536, MEDIA 65537, NAVIGATION 65538.
const char* const kInputFifo = "/fs/ram3/nbt-aa-native/input.fifo";

bool allowed_key(unsigned k)
{
    return k == 3 || k == 4 || k == 5 || (k >= 19 && k <= 23) || k == 84 || k == 65536 || k == 65537 || k == 65538;
}

struct InputLines {
    char b[256]; // partial line buffer
    size_t n;
    InputLines() : n(0) {}
};

void handle_input(int fd, InputLines& in, nbt::AASession& session)
{
    for (;;) {
        if (in.n >= sizeof(in.b))
            in.n = 0; // overlong garbage: drop it
        ssize_t r = read(fd, in.b + in.n, sizeof(in.b) - in.n);
        if (r <= 0)
            return;
        in.n += (size_t)r;
        char* start = in.b;
        char* nl;
        while ((nl = (char*)memchr(start, '\n', in.n - (size_t)(start - in.b)))) {
            *nl = 0;
            char kind = 0;
            unsigned key = 0;
            int value = 0;
            if (sscanf(start, "%c %u %d", &kind, &key, &value) == 3 && allowed_key(key)) {
                if (kind == 'R' && key == 65536 && value >= -20 && value <= 20 && value)
                    session.postRelativeEvent(key, value);
                else if (kind == 'K' && (value == 0 || value == 1))
                    session.postKeyEvent(key, value == 1);
            }
            start = nl + 1;
        }
        size_t rest = in.n - (size_t)(start - in.b);
        memmove(in.b, start, rest);
        in.n = rest;
    }
}

} // namespace

// ===== entry point (called by oneplus_aoa.c once the phone is in accessory mode) =======================================
//
// transfer: USB bulk transfer callback (direction 0 = to the phone, 1 = from the phone); context: its transport.
// prefix:   capture mode file prefix (must be under /fs/ram3/nbt-aa- when running as root).
// Returns 0 = session produced video, 2 = bad arguments / files, 3 = session failed.
extern "C" int nbt_video_capture_run(nbt_capture_transfer transfer, void* context, const char* prefix)
{
    if (!transfer || !prefix || !*prefix || strlen(prefix) > 200)
        return 2;
    if (geteuid() == 0 && strncmp(prefix, "/fs/ram3/nbt-aa-", sizeof("/fs/ram3/nbt-aa-") - 1))
        return 2;
    signal(SIGPIPE, SIG_IGN);
    Capture c;
    c.transfer = transfer;
    c.transport = context;
    unsigned seconds = 15;
    const char* live_fifo = getenv("NBT_AA_LIVE_FIFO");
    const char* requested = getenv("NBT_AA_CAPTURE_SECONDS");
    if (requested) {
        char* end = 0;
        long n = strtol(requested, &end, 10);
        if (!*requested || *end || n < 1 || n > 60)
            return 2;
        seconds = (unsigned)n;
        c.frame_limit = seconds * 30;
    }
    const std::string name(prefix);

    // ---- output: live FIFO (only the one native_service creates) or capture files ----
    if (live_fifo) {
        if (strcmp(live_fifo, "/fs/ram3/nbt-aa-native/annexb.fifo"))
            return 2;
        struct stat st;
        if (lstat(live_fifo, &st) || !S_ISFIFO(st.st_mode))
            return 2;
        c.data = open(live_fifo, O_WRONLY | O_NONBLOCK);
        if (c.data >= 0) {
            struct stat opened;
            if (fstat(c.data, &opened) || !S_ISFIFO(opened.st_mode)) {
                close(c.data);
                return 2;
            }
        }
        c.live = true;
        c.frame_limit = ~0u;
        seconds = 0;
        live_stop = 0;
        signal(SIGTERM, stop_live);
        signal(SIGINT, stop_live);
    } else
        c.data = open((name + ".h264").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (c.data < 0)
        return 2;
    if (!c.live)
        c.index = open((name + ".csv").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!c.live && c.index < 0) {
        close(c.data);
        return 2;
    }

    // ---- session over a socket pair: pair[0] for the session, pair[1] for our USB pump ----
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        close(c.data);
        close(c.index);
        return 2;
    }
    c.socket = pair[1];
    nbt::AASession session;
    session.setVideoFrameCallback(video, &c);
    session.setCodecConfigCallback(config, &c);
    if (c.live) {
        session.setNavStatusCallback(nav_status, 0);
        unlink("/fs/ram3/nbt-aa-native/nav.state");
        g_refocus_at = 0;
        session.setVideoFocusCallback(focus_request, 0);
    }
    setenv("NBT_VID_RES", "1", 0); // 800x480 unless the service asks for 1280x720 (res720)
    printf("AA_VIDEO_CAPTURE_BEGIN max_seconds=%u max_bytes=8388608 max_frames=%u no_display=1 no_audio_playback=1\n",
           seconds, c.frame_limit);
    if (!session.start(pair[0])) {
        close(pair[1]);
        close(c.data);
        close(c.index);
        return 3;
    }
    pthread_t thread, writer;
    bool have_writer = false;
    if (c.live) {
        if (pthread_create(&writer, 0, live_writer, &c)) {
            session.stop();
            close(pair[1]);
            close(c.data);
            return 3;
        }
        have_writer = true;
    }
    if (pthread_create(&thread, 0, reader, &c)) {
        if (have_writer) {
            pthread_mutex_lock(&c.queue.m);
            c.queue.stop = true;
            pthread_cond_signal(&c.queue.cv);
            pthread_mutex_unlock(&c.queue.m);
            pthread_join(writer, 0);
        }
        session.stop();
        close(pair[1]);
        close(c.data);
        close(c.index);
        return 3;
    }
    int input = -1;
    InputLines lines;
    if (c.live) {
        struct stat st;
        if (lstat(kInputFifo, &st) == 0 && S_ISFIFO(st.st_mode))
            input = open(kInputFifo, O_RDWR | O_NONBLOCK);
        printf("AA_INPUT %s\n", input >= 0 ? "controller FIFO open" : "no controller FIFO (video only)");
    }
    // Optional (NBT_AA_BG_FOCUS=1, off until car-tested): while the Android Auto page is not on screen
    // (/fs/ram3/nbt-aa-hidden) tell the phone it lost the screen (video focus NATIVE): it pauses the video and sends a
    // fresh key frame when the page comes back. Off: the phone keeps streaming in the background (proven path).
    const char* bg = getenv("NBT_AA_BG_FOCUS");
    const bool bg_focus = c.live && bg && !strcmp(bg, "1");
    int shown_state = 1;
    long long next_focus_check = 0;
    const long long start = now_ms();
    // Clean end (car 2026-11-10): an abrupt end (service restart, decoder gone) left Android Auto on the phone stuck until
    // it was force-stopped ("Invalid channel ... premature encryption" right after the next VersionRequest). Before
    // stopping, a ByeByeRequest goes out and the session keeps pumping both directions for up to 1 s until the phone's
    // ByeByeResponse ends it.
    long long bye_until = 0;

    // ---- main loop: socket pair -> USB, controller FIFO -> session, focus and shutdown handling ----
    while (start >= 0 && session.isActive()) {
        pthread_mutex_lock(&c.mutex);
        const bool finished = c.error || c.frames >= c.frame_limit;
        pthread_mutex_unlock(&c.mutex);
        const bool want_stop = live_stop || (!c.live && now_ms() - start >= seconds * 1000LL) || finished;
        if (want_stop && !bye_until) {
            session.postByeBye();
            bye_until = now_ms() + 1000;
            printf("AA_BYEBYE requested\n");
        }
        if (bye_until && now_ms() >= bye_until) {
            printf("AA_BYEBYE no answer within 1 s\n");
            break;
        }
        if (!bye_until && g_refocus_at && now_ms() >= g_refocus_at) {
            g_refocus_at = 0;
            session.postVideoFocus(true);
            printf("AA_FOCUS projection given back to the phone\n");
        }
        if (!bye_until && bg_focus && now_ms() >= next_focus_check && now_ms() - start >= 2000) {
            next_focus_check = now_ms() + 250;
            const int visible = access("/fs/ram3/nbt-aa-hidden", F_OK) != 0;
            if (visible != shown_state) {
                shown_state = visible;
                session.postVideoFocus(visible != 0);
                printf("AA_VIDEO_FOCUS %s\n", visible ? "PROJECTED (page on screen)" : "NATIVE (iDrive on screen)");
            }
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c.socket, &fds);
        if (input >= 0)
            FD_SET(input, &fds);
        struct timeval timeout = {0, 100000};
        int ready = select((input > c.socket ? input : c.socket) + 1, &fds, 0, 0, &timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        if (ready && input >= 0 && FD_ISSET(input, &fds))
            handle_input(input, lines, session);
        if (ready && FD_ISSET(c.socket, &fds)) { // session -> phone
            unsigned char b[65536];
            ssize_t n = recv(c.socket, b, sizeof(b), 0);
            if (n <= 0)
                break;
            unsigned size = n;
            if (!transfer(context, 0, b, &size) || size != (unsigned)n)
                break;
        }
    }

    // ---- shutdown ----
    if (input >= 0)
        close(input);
    if (bye_until)
        printf("AA_BYEBYE %s\n", session.isActive() ? "session still open: closing it" : "session ended cleanly");
    shutdown(c.socket, SHUT_RDWR);
    session.stop();
    if (c.live)
        unlink("/fs/ram3/nbt-aa-native/nav.state");
    // USB reader's individual transfer is bounded; no cancellation of a live URB.
    pthread_join(thread, 0);
    if (have_writer) {
        pthread_mutex_lock(&c.queue.m);
        c.queue.stop = true;
        pthread_cond_signal(&c.queue.cv);
        printf("AA_LIVE_QUEUE dropped=%u drop_runs=%u left=%u bytes\n", c.queue.dropped, c.queue.drops,
               (unsigned)c.queue.bytes);
        pthread_mutex_unlock(&c.queue.m);
        pthread_join(writer, 0);
    }
    close(c.socket);
    close(c.data);
    close(c.index);
    printf("AA_VIDEO_CAPTURE_END frames=%u configs=%u bytes=%u invalid=%d no_display=1\n", c.frames, c.configs,
           (unsigned)c.bytes, c.error);
    return !c.error && c.frames && c.configs ? 0 : 3;
}
