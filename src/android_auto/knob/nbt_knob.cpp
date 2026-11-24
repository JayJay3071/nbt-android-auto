// nbt_knob - iDrive controller -> Android Auto key events for the original NBT (in the installed package, car-proven
// since 2026-11-07).
//
// Observe-only: it registers one DSI client on the MOST server and receives COPIES of the controller's status messages
// (FBlock 0xE5). Nothing is ever sent to the controller or any other function block; the HMI keeps receiving every event.
// The MOST/DSI client and the decoder are the NBT-AAD reference code (src/android_auto/knob/ref, PolyForm Noncommercial,
// notices kept), adapted for this NBT: libsys_dsi_colibry.so.3, MOSTSubSystemIntel.DSYSMOSTServer 4.4.
//
// Mapping wanted by the user (2026-11-06):
//   turn            -> AA rotary (keycode 65536, delta)   = focus forward / back in Android Auto
//   tilt up/down/left/right -> DPAD_UP/DOWN/LEFT/RIGHT (19..22)
//   push            -> DPAD_CENTER (23)
//   BACK key        -> BACK (4)
//   MENU, OPTION, COM, MEDIA, NAV, MAP -> NOT forwarded: they stay with iDrive (they leave the Android Auto page)
// Each event is one text line written to the FIFO the AA session reads (oneplus_native --capture-video, live mode):
//   "R <keycode> <delta>\n"   or   "K <keycode> <1 down|0 up>\n"
// While /fs/ram3/nbt-aa-hidden exists (Android Auto page not on screen) presses are not forwarded; releases always are,
// so the phone never keeps a key held down.
//
// usage: nbt_knob --probe                 only dlopen the DSI library and resolve its symbols (no connection)
//        nbt_knob [--seconds N] [--fifo PATH]   observe and forward (default FIFO /fs/ram3/nbt-aa-native/input.fifo)
// Environment: NBT_MOST_ROLE / NBT_MOST_MAJOR / NBT_MOST_MINOR, NBT_DSI_LIB, NBT_KNOB_INST, NBT_KNOB_CCS_INST,
//              NBT_KNOB_INVERT=1, NBT_KEY_<MENU|BACK|OPTIONS|COM|MEDIA|NAV|MAP>=keycode (0 = keep for iDrive)
#include "most/client.hpp"
#include "car/ccs_config.hpp"
#include "car/ccs_input.hpp"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
const char* g_fifo = "/fs/ram3/nbt-aa-native/input.fifo";
int g_fd = -1;
pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
unsigned g_sent = 0, g_dropped = 0;

void forward(const char* line, size_t n) {
    // DSI callback thread: never block (non-blocking FIFO, a full or absent reader just drops the event)
    pthread_mutex_lock(&g_mx);
    if (g_fd < 0) g_fd = open(g_fifo, O_WRONLY | O_NONBLOCK);
    if (g_fd >= 0 && write(g_fd, line, n) == (ssize_t)n) ++g_sent;
    else {
        ++g_dropped;
        if (g_fd >= 0 && errno != EAGAIN) { close(g_fd); g_fd = -1; }
    }
    pthread_mutex_unlock(&g_mx);
}

void onKnob(const nbt::car::CcsEvent& ev, void*) {
    char line[48];
    int n;
    // a hard key that stays with iDrive (MENU, OPTION, COM, MEDIA, NAV, MAP: keycode 0) while Android Auto is on
    // screen: tell the HMI page (aa_entry_plugin) that the driver wants iDrive, so it does not pull the page back
    if (!ev.keycode && ev.kind == 1 && ev.down && !ev.cancel && access("/fs/ram3/nbt-aa-hidden", F_OK) != 0) {
        int fd = open("/fs/ram3/nbt-aa-leave", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        // and hide the picture at once: the plugin only reacts to a focus CHANGE of its hold state, which does not come
        // when the state had already lost the focus (car 2026-11-07: MENU without effect after a while)
        fd = open("/fs/ram3/nbt-aa-hidden", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        printf("[Knob] %s: leave Android Auto (iDrive key)\n", ev.what);
        return;
    }
    if (!ev.keycode) return;
    const bool release = ev.kind == 1 && !ev.down;
    if (!release && access("/fs/ram3/nbt-aa-hidden", F_OK) == 0) {
        printf("[Knob] %s ignored: Android Auto page not on screen\n", ev.what);
        return;
    }
    if (ev.kind == 2) n = snprintf(line, sizeof(line), "R %u %d\n", (unsigned)ev.keycode, ev.delta);
    else n = snprintf(line, sizeof(line), "K %u %d\n", (unsigned)ev.keycode, ev.down ? 1 : 0);
    if (n <= 0 || n >= (int)sizeof(line)) return;
    printf("[Knob->AA] %s %s", ev.what, line);
    forward(line, (size_t)n);
}

// Steering wheel keys (MOST FBlock 0x15, observe only; 2026-11-12). Every change of a function's first payload bytes is
// logged ("[Wheel] 15/02 fkt=513 op=C: 01"), so one drive with the skip keys pressed shows which function they are (the
// 2026-11-07 trace only has 401 = periodic and 513 = MODE key). The file mfl.map then turns them into Android Auto media
// keys without a rebuild; one line per key:  <fkt hex> <first payload byte hex> <Android key code>
//   e.g.  514 01 87     (87 = MEDIA_NEXT, 88 = MEDIA_PREVIOUS, 85 = MEDIA_PLAY_PAUSE)
// A mapped key is sent as down + up when that value appears, also while the Android Auto page is not on screen (the
// music goes on then). Nothing is sent to MOST, and iDrive still gets the key as well.
//
// Car 2026-11-13: the skip key of the radio panel is 15/0C fkt C07 (02 while pressed, 00 released; 01 presumably the
// other direction), the thumb wheel of the steering wheel is 15/02 fkt C30 (80 at rest, above / below 80 while turned).
// Both belong to the stock source ("Musiksammlung"), which then opens its next track and hangs on the amplifier input
// that nbt_a2i_play holds. So the value may also be a comparison (">80", "<80"), a rule fires once when it starts to
// match, skip keys are only taken while Android Auto plays (media.state state=2), and one second after a forwarded key
// the stock player is told to stop again. The file is read again whenever it has changed.
const char* const kWheelMap = "/var/opt/hmi/data/rw/NbtAndroidAutoRT/mfl.map";
const char* const kMediaState = "/fs/ram3/nbt-aa-native/media.state";
struct WheelMap { unsigned fkt, value, key; char cmp; bool active; };
WheelMap g_wheel_map[16];
int g_wheel_map_n = 0;
time_t g_wheel_map_time = 0;
struct WheelSeen { unsigned fkt; unsigned char data[4]; unsigned len; unsigned logged; };
WheelSeen g_wheel_seen[24];
int g_wheel_seen_n = 0;

void loadWheelMap() {
    struct stat st;
    if (stat(kWheelMap, &st)) {
        if (g_wheel_map_n) printf("[Wheel] %s is gone: no key mappings\n", kWheelMap);
        g_wheel_map_n = 0;
        g_wheel_map_time = 0;
        return;
    }
    if (st.st_mtime == g_wheel_map_time) return;
    g_wheel_map_time = st.st_mtime;
    g_wheel_map_n = 0;
    FILE* f = fopen(kWheelMap, "r");
    char line[80];
    if (!f) return;
    while (g_wheel_map_n < 16 && fgets(line, sizeof(line), f)) {
        WheelMap m;
        char value[16];
        if (line[0] == '#' || sscanf(line, "%x %15s %u", &m.fkt, value, &m.key) != 3) continue;
        m.cmp = (value[0] == '<' || value[0] == '>') ? value[0] : '=';
        m.value = (unsigned)strtoul(value + (m.cmp == '=' ? 0 : 1), NULL, 16);
        m.active = false;
        if (m.key == 85 || m.key == 87 || m.key == 88 || m.key == 126 || m.key == 127) g_wheel_map[g_wheel_map_n++] = m;
    }
    fclose(f);
    printf("[Wheel] %d key mapping(s) from %s\n", g_wheel_map_n, kWheelMap);
}

// Android Auto is playing music right now (media.state from the session: "state=2")
bool aaPlaying() {
    char buf[400];
    int fd = open(kMediaState, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    return strstr(buf, "\nstate=2\n") != NULL;
}

void onWheel(const nbt::most::View& m, void*) {
    WheelSeen* s = 0;
    for (int i = 0; i < g_wheel_seen_n && !s; ++i)
        if (g_wheel_seen[i].fkt == m.fkt) s = &g_wheel_seen[i];
    if (!s) {
        if (g_wheel_seen_n >= 24) return;
        s = &g_wheel_seen[g_wheel_seen_n++];
        s->fkt = m.fkt;
        s->len = ~0u;
        s->logged = 0;
    }
    const unsigned len = m.len < 4 ? (unsigned)m.len : 4u;
    if (s->len == len && !memcmp(s->data, m.data, len)) return;   // repeated status: nothing new
    s->len = len;
    memcpy(s->data, m.data, len);
    if (s->logged < 200) {
        char text[64]; int n = 0;
        ++s->logged;
        for (size_t i = 0; i < m.len && i < 12; ++i) n += snprintf(text + n, sizeof(text) - n, " %02x", m.data[i]);
        text[n] = 0;
        printf("[Wheel] 15/%02X fkt=%03X op=%X:%s\n", m.inst, m.fkt, m.op, text);
    }
    if (!m.len) return;
    loadWheelMap();
    for (int i = 0; i < g_wheel_map_n; ++i) {
        WheelMap& r = g_wheel_map[i];
        char line[48];
        if (r.fkt != m.fkt) continue;
        const unsigned v = m.data[0];
        const bool match = r.cmp == '<' ? v < r.value : r.cmp == '>' ? v > r.value : v == r.value;
        const bool fire = match && !r.active;
        r.active = match;
        if (!fire) continue;
        if ((r.key == 87 || r.key == 88) && !aaPlaying()) {
            printf("[Wheel] key %u not taken: Android Auto is not playing\n", r.key);
            continue;
        }
        int n = snprintf(line, sizeof(line), "K %u 1\nK %u 0\n", r.key, r.key);
        printf("[Wheel->AA] media key %u\n", r.key);
        forward(line, (size_t)n);
        // the stock source got the same key and starts its own next track: stop it again (as nbt_a2i_play does at start)
        int rc = system("(sleep 1; LD_LIBRARY_PATH=/opt/mm/lib:/opt/sys/lib:/lib:/usr/lib /opt/mm/bin/mmecli -c "
                        "/dev/mme/multichannel stop >/dev/null 2>&1) &");
        (void)rc;
    }
}

// --trace: observe-only log of EVERY MOST message this client receives (header + first 48 payload bytes), to learn what the
// HMI sends to VideoApp's VideoConnectionMaster / VideoSink when a stock video page opens. Nothing is ever sent.
unsigned g_traced = 0;
int g_filter[8], g_filter_n = 0;          // --filter D7,D8: only these FBlocks (plus everything to/from our device)
unsigned g_mydev = 0;
void onTap(const nbt::most::View& m, nbt::most::MsgClass, void*) {
    char line[360]; int n;
    if (g_filter_n) {
        bool hit = g_mydev && (m.dst == g_mydev || m.src == g_mydev);
        for (int i = 0; i < g_filter_n && !hit; ++i) hit = m.fblock == g_filter[i];
        if (!hit) return;
    }
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    n = snprintf(line, sizeof(line), "%ld.%03ld %04X->%04X %02X/%02X fkt=%03X op=%X tx=%u var=%u/%u tag=%u len=%u:", (long)t.tv_sec,
                 t.tv_nsec / 1000000, m.src, m.dst, m.fblock, m.inst, m.fkt, m.op, m.txType, m.varTag, m.varVal, m.hdrTag,
                 (unsigned)m.len);
    for (size_t i = 0; i < m.len && i < 48 && n < (int)sizeof(line) - 4; ++i) n += snprintf(line + n, sizeof(line) - n, " %02x", m.data[i]);
    printf("%s\n", line);
    ++g_traced;
}

int probe() {
    const char* lib = getenv("NBT_DSI_LIB");
    if (!lib || !*lib) lib = "/opt/sys/lib/libsys_dsi_colibry.so.3";
    void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen(%s) -> %s\n", lib, h ? "OK" : dlerror());
    if (!h) return 3;
    // each line: one symbol, or "a|b" = either the Evo name or the original-NBT name
    static const char* const syms[] = {
        "_ZN14CDSIConnectionC1Ev", "_ZN14CDSIConnection3addER10CDSIClient", "_ZN14CDSIConnection5startEv",
        "_ZN10CDSIClientC1EPKcS1_ii", "_ZN10CDSIClient15setNotificationEj",
        "_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i|"
        "_ZN11CDSIOStreamC1ER11CDSIChannelR13CDSITCPSocketN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i",
        "_ZN11CDSIOStream5writeEPKvj|_ZN11CDSIOStream5writeIiEEvRKT_",
        "_ZN11CDSIOStream4sendEi", "_ZN11CDSIOStreamD1Ev", "_ZTV10CDSIClient", 0
    };
    int missing = 0;
    for (int i = 0; syms[i]; ++i) {
        char name[200]; snprintf(name, sizeof(name), "%s", syms[i]);
        char* alt = strchr(name, '|'); if (alt) *alt++ = 0;
        void* p = dlsym(h, name);
        const char* used = name;
        if (!p && alt) { p = dlsym(h, alt); used = alt; }
        printf("  %-100s %s\n", used, p ? "ok" : "MISSING");
        if (!p) ++missing;
    }
    printf(missing ? "NBT_KNOB_PROBE_MISSING %d\n" : "NBT_KNOB_PROBE_OK\n", missing);
    return missing ? 4 : 0;
}
}

int main(int argc, char** argv) {
    long seconds = 0;
    bool trace = false;
    // --send dst,fblock,inst,fkt,op,tx,varTag,varVal,tag[,hexbytes]   (hex, may be given several times; sent in order
    // 300 ms apart once the MOST link is up). src = our own device id.
    std::vector<nbt::most::Msg> sends;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--probe")) return probe();
        if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            char* s = argv[++i];
            while (*s && g_filter_n < 8) { g_filter[g_filter_n++] = (int)strtoul(s, &s, 16); if (*s == ',') ++s; else break; }
        }
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) {
            unsigned v[9]; char hex[400] = "";
            int k = sscanf(argv[++i], "%x,%x,%x,%x,%x,%x,%x,%x,%x,%399s", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], hex);
            if (k < 9) { fprintf(stderr, "bad --send %s\n", argv[i]); return 2; }
            nbt::most::Msg m;
            m.dst = (uint16_t)v[0]; m.src = 0; m.fblock = (uint8_t)v[1]; m.inst = (uint8_t)v[2]; m.fkt = (uint16_t)v[3];
            m.opType = (uint8_t)v[4]; m.txType = v[5]; m.varTag = v[6]; m.varVal = v[7]; m.hdrTag = v[8];
            for (size_t j = 0; k == 10 && hex[j] && hex[j + 1]; j += 2) { unsigned b; sscanf(hex + j, "%2x", &b); m.data.push_back((uint8_t)b); }
            sends.push_back(m);
        }
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--fifo") && i + 1 < argc) g_fifo = argv[++i];
        else { fprintf(stderr, "usage: %s --probe | [--seconds N] [--fifo /fs/ram3/...]\n", argv[0]); return 2; }
    }
    if (strncmp(g_fifo, "/fs/ram3/", 9) || strstr(g_fifo, "..") || seconds < 0) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);
    signal(SIGPIPE, SIG_IGN);
    // The user's mapping: only BACK of the hard keys goes to the phone (unless overridden in the environment).
    setenv("NBT_KEY_MENU", "0", 0);
    setenv("NBT_KEY_OPTIONS", "0", 0);
    setenv("NBT_KEY_BACK", "4", 0);
    setenv("NBT_KEY_COM", "0", 0);
    setenv("NBT_KEY_MEDIA", "0", 0);
    setenv("NBT_KEY_NAV", "0", 0);
    setenv("NBT_KEY_MAP", "0", 0);

    static nbt::most::Client most;
    static nbt::car::CcsInput ccs;
    if (!ccs.attach(most, nbt::car::readCcsConfig(), &onKnob, NULL)) return 3;
    loadWheelMap();
    if (!most.addRoute(0x15, nbt::most::kAny, nbt::most::kAny, nbt::most::kAny, &onWheel, NULL))
        printf("[Wheel] no room on the MOST client: steering wheel keys are not observed\n");
    if (trace && !most.setTap(&onTap, NULL)) return 3;
    if (!most.start("NBTPROJ_KNOB")) return 3;
    printf("nbt_knob: observing the iDrive controller, forwarding to %s%s\n", g_fifo, seconds ? "" : " until SIGTERM");
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long last_report = 0;
    if (!sends.empty()) {
        for (int w = 0; w < 50 && most.state() != nbt::most::kUp; ++w) usleep(100000);
        g_mydev = most.deviceId();
        printf("nbt_knob: link %s, own device 0x%04X, sending %u message(s)\n", nbt::most::linkStateName(most.state()), g_mydev,
               (unsigned)sends.size());
        for (size_t i = 0; i < sends.size() && !g_stop; ++i) { most.send(sends[i]); usleep(300000); }
    } else {
        for (int w = 0; w < 50 && most.state() != nbt::most::kUp; ++w) usleep(100000);
        g_mydev = most.deviceId();
    }
    while (!g_stop) {
        usleep(200000);
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (long)(t.tv_sec - t0.tv_sec);
        if (seconds && el >= seconds) break;
        if (el - last_report >= 30) {
            last_report = el;
            pthread_mutex_lock(&g_mx);
            printf("[Knob] t=%lds link=%s forwarded=%u dropped=%u traced=%u\n", el, nbt::most::linkStateName(most.state()), g_sent,
                   g_dropped, g_traced);
            pthread_mutex_unlock(&g_mx);
        }
    }
    printf("nbt_knob: end forwarded=%u dropped=%u\n", g_sent, g_dropped);
    fflush(stdout);
    _exit(0);   // the DSI library threads are not joined; leave without running their destructors
}
