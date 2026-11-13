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
// Split-screen mode (2026-11-12, user: "pressing BACK right after fixes it"): iDrive also sees every tilt RIGHT and puts its
// focus into the split screen (menu on the right), also in the full-width modes where the split screen is behind the
// picture. When a tilt right is released while Android Auto is on screen (any picture mode), nbt_knob answers it with a BACK sent to the HMI as if the controller had sent it (the one place this
// program sends anything). Car-tested with nbt_key: this controller speaks the older CCS protocol, E5/01 fkt C20 from
// 3002 to 2025, one event per message, [2] = message counter; the BACK copies the controller's addresses and continues
// its counter. Only for C20 controllers (C46/C47 ones: not implemented). The BACK arrives here too: it is not forwarded
// to the phone (600 ms window).
//
// Steering wheel roller (2026-11-12, user: phone audio over AUX, so the wheel's skip buttons did nothing): MOST FBlock
// 0x15 instance 02, op Status, src 3002, one byte each (car trace 2026-11-12, every message arrives twice, once per HMI
// receiver):
//   fkt C30  0x80 = one step up, 0x7e = one step down, 0x7f = no step (also sent with every press)
//   fkt C31  01 pressed, 00 released
// Forwarded as Android Auto media keys, also while the Android Auto page is not on screen: up -> NEXT 87,
// down -> PREVIOUS 88, press -> PLAY_PAUSE 85. Override / switch off with NBT_WHEEL_UP / _DOWN / _PUSH = keycode / 0.
// iDrive still gets the roller too.
// 2026-11-13 (user): only while the audio source is AUX (the phone's audio); on Bluetooth / radio the roller is left to
// iDrive alone. Source from the C03 status below; unknown until the first switch after boot = treated as AUX.
// NBT_WHEEL_AUX_ONLY=0: always forward, as before.
//
// AUX source + Android Auto in front (2026-11-12, user: phone audio over AUX; favourite button 7 = the AUX page, which
// switches the source; favourite button 8 = the Android Auto entry, which leads straight to the picture while the
// session runs): when the first session of this service is running, nbt_knob presses favourite 7, waits 1.5 s, then
// presses favourite 8. So the car starts into Android Auto with AUX as the source. Later sessions (replug, drop-out)
// press nothing. Car trace 2026-11-12: favourite keypad = FBlock 15 instance 0C fkt C02 op Status, from 3002 to the
// HMI, 8 bytes = one per button, byte N-1: 02 -> 01 (~110 ms) -> 00 (~60 ms). The HMI's address is the destination of
// the controller's periodic E5/0F C06 (every 2 s). NBT_AUX_FAVORITE / NBT_AA_FAVORITE = button 1..8, 0 = off
// (defaults 7 / 8: what these buttons hold in THIS car; a favourite can also hold a phone number).
// 2026-11-13 (user): only favourite 8, as soon as the session runs - AUX is usually the source already, and another
// source may be wanted. NBT_AUX_FAVORITE now defaults to 0 (set it to 7 for the old AUX press).
// Switch (2026-11-12, "Start at boot" in the Android Auto menu): only once per boot (marker /fs/ram3/nbt-aa-fav-done,
// so restarting Android Auto presses nothing), and not while the file start-at-boot.off exists in the package
// (/var/opt/hmi/data/rw/NbtAndroidAutoRT; the menu creates / deletes it, read when the session starts).
//
// Pause on source change (2026-11-12, user: phone audio over AUX; music should stop when iDrive switches away from AUX).
// Car trace 2026-11-12 (AUX -> Bluetooth -> AUX -> Radio -> AUX): on every source switch 100E sends FBlock 03 inst 00
// fkt C03 op Status to the HMI (and 2011 / 2038): first "00 00" (2 bytes, the old source off), then 5 bytes with the new
// source in byte 3: BB = AUX, C7 = Bluetooth audio, 01 = radio. When the source leaves AUX while Android Auto runs,
// nbt_knob sends PLAY_PAUSE (85) to the phone; when it comes back to AUX and that pause was ours, PLAY_PAUSE again.
// Not MEDIA_PAUSE 127 / MEDIA_PLAY 126: car 2026-11-13, after a 127 the phone dropped the session and reconnected; 85 is
// the key the steering wheel push has sent without trouble. Drawback: 85 toggles, so music that was already paused when
// AUX was left starts (unheard, the source is not AUX) and is paused again on the way back. So the key goes out only
// while the phone reports "playing": the session writes the phone's media state to /fs/ram3/nbt-aa-media (2 = playing,
// 3 = paused, car 2026-11-13; patches/nbt-aad-aa_session-media-state.patch); no file = unknown = as before.
// NBT_AUX_SOURCE = source byte of AUX (hex, default BB), NBT_AUX_PAUSE=0 off, NBT_AUX_RESUME=0 pause only.
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

long long nowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

// --- BACK after a tilt right (split-screen mode), see the header ---
pthread_mutex_t g_backMx = PTHREAD_MUTEX_INITIALIZER;
uint16_t g_ccsSrc = 0, g_ccsDst = 0;   // addresses of the controller's last C20
uint8_t g_ccsCounter = 0;              // its last counter
bool g_tiltRightDown = false;
bool g_backPending = false;            // tilt right released: the main loop sends BACK
long long g_ignoreBackUntil = 0;       // our own BACK coming back: not for the phone

void onC20(const nbt::most::View& m, void*) {
    if (m.len < 7) return;
    pthread_mutex_lock(&g_backMx);
    if (m.data[2] != 0xFF) { g_ccsSrc = m.src; g_ccsDst = m.dst; g_ccsCounter = m.data[2]; }
    if (m.data[5] == 0x1D && m.data[3] == 1 && m.data[4] == 2) g_tiltRightDown = true;
    else if (m.data[5] == 0x1D && m.data[3] == 0 && g_tiltRightDown) {
        g_tiltRightDown = false;
        if (access("/fs/ram3/nbt-aa-hidden", F_OK) != 0) g_backPending = true;   // Android Auto on screen
    }
    pthread_mutex_unlock(&g_backMx);
}

uint8_t nextCounter(uint8_t c) { c = (uint8_t)(c + 1); return c == 0xFF ? 0 : c; }   // 0xFF = "ignore" (firmware)

nbt::most::Msg backMsg(uint16_t src, uint16_t dst, uint8_t counter, bool down) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = src; m.fblock = 0xE5; m.inst = 0x01; m.fkt = 0xC20; m.opType = 0xC; m.hdrTag = 0xFFFFu;
    const uint8_t d[7] = { 0x01, 0x1D, counter, (uint8_t)(down ? 1 : 0), 0x00, 0x00, 0x02 };   // hard key, sensor BACK
    m.data.assign(d, d + 7);
    return m;
}

// --- steering wheel roller -> media keys (see the header) ---
unsigned wheelKey(const char* name, unsigned dflt) {
    const char* v = getenv(name);
    return v && *v ? (unsigned)strtoul(v, 0, 10) : dflt;
}
unsigned g_wheelUp = 87, g_wheelDown = 88, g_wheelPush = 85;
bool g_wheelPushDown = false;
uint16_t g_wheelLastFkt = 0;
uint8_t g_wheelLastVal = 0;
long long g_wheelLastMs = 0;

void wheelKeyLine(unsigned key, int down, const char* what) {
    char line[48];
    const int n = snprintf(line, sizeof(line), "K %u %d\n", key, down);
    if (n <= 0 || n >= (int)sizeof(line)) return;
    printf("[Wheel->AA] %s %s", what, line);
    forward(line, (size_t)n);
}

unsigned g_auxSource = 0xBB;
int g_lastSource = -1;          // unknown until the first switch
bool g_wheelAuxOnly = true;

bool wheelAllowed(const char* what) {
    if (!g_wheelAuxOnly || g_lastSource < 0 || g_lastSource == (int)g_auxSource) return true;
    printf("[Wheel] %s ignored: source %02X is not AUX\n", what, g_lastSource);
    return false;
}

void onWheel(const nbt::most::View& m, void*) {
    if (m.len < 1 || (m.fkt != 0xC30 && m.fkt != 0xC31)) return;
    const uint8_t v = m.data[0];
    const long long t = nowMs();
    // the same message once per HMI receiver, ~1 ms apart: take the first copy only
    if (m.fkt == g_wheelLastFkt && v == g_wheelLastVal && t - g_wheelLastMs < 40) return;
    g_wheelLastFkt = m.fkt; g_wheelLastVal = v; g_wheelLastMs = t;
    if (m.fkt == 0xC30) {
        const unsigned key = v > 0x7f ? g_wheelUp : v < 0x7f ? g_wheelDown : 0;
        if (!key || !wheelAllowed(v > 0x7f ? "WHEEL_UP" : "WHEEL_DOWN")) return;
        wheelKeyLine(key, 1, v > 0x7f ? "WHEEL_UP" : "WHEEL_DOWN");
        wheelKeyLine(key, 0, v > 0x7f ? "WHEEL_UP" : "WHEEL_DOWN");
    } else if (g_wheelPush) {
        if (v == 1 && !g_wheelPushDown) { if (!wheelAllowed("WHEEL_PUSH")) return; g_wheelPushDown = true; wheelKeyLine(g_wheelPush, 1, "WHEEL_PUSH"); }
        else if (v == 0 && g_wheelPushDown) { g_wheelPushDown = false; wheelKeyLine(g_wheelPush, 0, "WHEEL_PUSH"); }
    }
}

// --- pause / resume Android Auto when the audio source leaves / returns to AUX (see the header) ---
bool g_auxPause = true, g_auxResume = true;
bool g_wePaused = false;        // the last pause came from us (resume only then)

bool sessionRunning() {
    char buf[160];
    FILE* f = fopen("/fs/ram3/nbt-aa-state", "r");
    if (!f) return false;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "state=running") != 0;
}

int mediaState() {   // 2 = playing, 3 = paused, 0 = unknown
    FILE* f = fopen("/fs/ram3/nbt-aa-media", "r");
    if (!f) return 0;
    int st = 0;
    if (fscanf(f, "%d", &st) != 1) st = 0;
    fclose(f);
    return st;
}

void onSource(const nbt::most::View& m, void*) {
    if (m.fkt != 0xC03 || m.len < 5) return;             // the 2-byte "00 00" is only the old source going off
    const int s = m.data[3];
    if (s == g_lastSource) return;                        // the same status once per receiver
    const int prev = g_lastSource;
    g_lastSource = s;
    printf("[Source] %02X -> %02X\n", prev < 0 ? 0xFF : prev, s);
    if (!g_auxPause || prev < 0) return;
    if (prev == (int)g_auxSource && s != (int)g_auxSource) {
        const int st = mediaState();
        g_wePaused = sessionRunning() && st != 3;          // already paused: nothing to do, nothing to resume
        printf("[Source] AUX left, phone media state %d\n", st);
        if (g_wePaused) { wheelKeyLine(85, 1, "AUX_LEFT_PAUSE"); wheelKeyLine(85, 0, "AUX_LEFT_PAUSE"); }
    } else if (s == (int)g_auxSource && g_wePaused) {
        g_wePaused = false;
        const int st = mediaState();
        printf("[Source] back on AUX, phone media state %d\n", st);
        if (g_auxResume && sessionRunning() && st != 2) { wheelKeyLine(85, 1, "AUX_BACK_PLAY"); wheelKeyLine(85, 0, "AUX_BACK_PLAY"); }
    }
}

// --- AUX source through a favourite button (see the header) ---
uint16_t g_hmiDev = 0, g_hmiSrc = 0;   // from the controller's periodic E5/0F C06: HMI address, controller-side sender
void onE5Periodic(const nbt::most::View& m, void*) {
    pthread_mutex_lock(&g_backMx);
    g_hmiDev = m.dst; g_hmiSrc = m.src;
    pthread_mutex_unlock(&g_backMx);
}

nbt::most::Msg favMsg(uint16_t src, uint16_t dst, unsigned button, uint8_t v) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = src; m.fblock = 0x15; m.inst = 0x0C; m.fkt = 0xC02; m.opType = 0xC; m.hdrTag = 0xFFFFu;
    m.data.assign(8, 0);
    m.data[button - 1] = v;
    return m;
}

unsigned g_auxButton = 7, g_aaButton = 8;
bool g_favDone = false;          // once per service life
long long g_favNextCheck = 0;
const char* const kFavDone = "/fs/ram3/nbt-aa-fav-done";                          // once per boot
const char* const kFavOff = "/var/opt/hmi/data/rw/NbtAndroidAutoRT/start-at-boot.off";   // menu: "Start at boot" off

void pressFavourite(nbt::most::Client& most, uint16_t src, uint16_t dst, unsigned button) {
    most.send(favMsg(src, dst, button, 2));
    usleep(110000);
    most.send(favMsg(src, dst, button, 1));
    usleep(60000);
    most.send(favMsg(src, dst, button, 0));
}

// main thread, every 200 ms until done: the service's public state
void favouritesIfNeeded(nbt::most::Client& most) {
    if (g_favDone || (!g_auxButton && !g_aaButton)) return;
    const long long now = nowMs();
    if (now < g_favNextCheck) return;
    g_favNextCheck = now + 200;                     // native_service writes "running" as soon as a session starts
    char buf[160];
    FILE* f = fopen("/fs/ram3/nbt-aa-state", "r");
    if (!f) return;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!strstr(buf, "state=running")) return;
    pthread_mutex_lock(&g_backMx);
    const uint16_t dst = g_hmiDev, src = g_hmiSrc;
    pthread_mutex_unlock(&g_backMx);
    if (!dst || !src) return;                                     // HMI address not known yet
    g_favDone = true;
    if (access(kFavDone, F_OK) == 0) { printf("[Knob] start at boot: already done in this boot\n"); return; }
    if (FILE* m = fopen(kFavDone, "w")) fclose(m);
    if (access(kFavOff, F_OK) == 0) { printf("[Knob] start at boot: switched off in the menu\n"); return; }
    if (g_auxButton) {
        printf("[Knob] session running: favourite %u (AUX) %04X->%04X\n", g_auxButton, src, dst);
        pressFavourite(most, src, dst, g_auxButton);
        if (g_aaButton) usleep(1500000);                          // let iDrive switch the source first
    }
    if (g_aaButton) {
        printf("[Knob] favourite %u (Android Auto)\n", g_aaButton);
        pressFavourite(most, src, dst, g_aaButton);
    }
}

// main thread: never send from the DSI callback thread
void sendBackIfPending(nbt::most::Client& most) {
    pthread_mutex_lock(&g_backMx);
    if (!g_backPending) { pthread_mutex_unlock(&g_backMx); return; }
    g_backPending = false;
    const uint16_t src = g_ccsSrc, dst = g_ccsDst;
    const uint8_t c1 = nextCounter(g_ccsCounter), c2 = nextCounter(c1);
    g_ccsCounter = c2;
    g_ignoreBackUntil = nowMs() + 600;
    pthread_mutex_unlock(&g_backMx);
    if (!src || !dst) return;
    printf("[Knob] tilt right: BACK to iDrive (%04X->%04X, counters %02x %02x)\n", src, dst, c1, c2);
    most.send(backMsg(src, dst, c1, true));
    usleep(100000);
    most.send(backMsg(src, dst, c2, false));
}

void onKnob(const nbt::car::CcsEvent& ev, void*) {
    char line[48];
    int n;
    if (ev.keycode == nbt::AAK_BACK) {
        pthread_mutex_lock(&g_backMx);
        const bool ours = nowMs() < g_ignoreBackUntil;
        pthread_mutex_unlock(&g_backMx);
        if (ours) { printf("[Knob] %s %s: our own BACK, not for the phone\n", ev.what, ev.down ? "down" : "up"); return; }
    }
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
    if (!most.addRoute(0xE5, 0x01, 0xC20, 0xC, &onC20, NULL)) printf("[Knob] no room for the split-screen BACK route\n");
    g_wheelUp = wheelKey("NBT_WHEEL_UP", 87); g_wheelDown = wheelKey("NBT_WHEEL_DOWN", 88); g_wheelPush = wheelKey("NBT_WHEEL_PUSH", 85);
    if (!most.addRoute(0x15, 0x02, nbt::most::kAny, 0xC, &onWheel, NULL)) printf("[Knob] no room for the steering wheel route\n");
    g_auxButton = wheelKey("NBT_AUX_FAVORITE", 0);
    g_aaButton = wheelKey("NBT_AA_FAVORITE", 8);
    if (g_auxButton > 8 || trace) g_auxButton = 0;   // a --trace copy (tests, boot.sh trace) never presses anything
    if (g_aaButton > 8 || trace) g_aaButton = 0;
    if (!most.addRoute(0xE5, 0x0F, 0xC06, 0xC, &onE5Periodic, NULL)) printf("[Knob] no room for the HMI address route\n");
    {
        const char* v = getenv("NBT_AUX_SOURCE");
        if (v && *v) g_auxSource = (unsigned)strtoul(v, 0, 16) & 0xFF;
        g_auxPause = wheelKey("NBT_AUX_PAUSE", 1) != 0 && !trace;   // a --trace copy never sends keys
        g_auxResume = wheelKey("NBT_AUX_RESUME", 1) != 0;
        g_wheelAuxOnly = wheelKey("NBT_WHEEL_AUX_ONLY", 1) != 0;
    }
    if (!most.addRoute(0x03, 0x00, 0xC03, 0xC, &onSource, NULL)) printf("[Knob] no room for the audio source route\n");
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
        usleep(20000);
        if (!trace) sendBackIfPending(most);
        favouritesIfNeeded(most);
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
