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
// Startup NEXT (user 2026-11-17: once per start, Android Auto skipped to the next song; knob.log of that boot showed one
// WHEEL_UP ~30-60 s after start with nobody touching the roller): the first C30 after nbt_knob starts is a status
// report, not a turn, and is only taken as the baseline now; and only 0x80 / 0x7e count as one step (before, any value
// above / below 0x7f did). The raw value is logged.
// Phone list (user 2026-11-17: the roller also skipped songs while scrolling the contact list opened with the steering
// wheel phone button). Car trace 2026-11-17: the phone button = 15/02 fkt 400 (01 pressed, 00 released); nothing on MOST
// marks the list closing. So after the phone button the roller stays with iDrive until it has been still for
// NBT_LIST_HOLD_S seconds (default 4, the list's own timeout: user 2026-11-17, it closes 4 s after the last turn;
// every step inside the list restarts the time). 0 = off.
// Forwarded as Android Auto media keys, also while the Android Auto page is not on screen: up -> NEXT 87,
// down -> PREVIOUS 88, press -> PLAY_PAUSE 85. Override / switch off with NBT_WHEEL_UP / _DOWN / _PUSH = keycode / 0.
// iDrive still gets the roller too.
// 2026-11-13 (user): only while the audio source is AUX (the phone's audio); on Bluetooth / radio the roller is left to
// iDrive alone. Source from the C03 status below (asked for at start); still unknown = treated as AUX.
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
// 2026-11-17: no favourite button any more (user: free favourite 8). nbt_knob creates /fs/ram3/nbt-aa-show and the HMI
// plugin brings the picture to the front with its focus event (car 2026-11-17: works while another screen is in front).
// The favourite code stays for NBT_AUX_FAVORITE (off by default).
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
// Current source at start (car 2026-11-17, nbt_key source): a Get of 03/00 C03 is refused (Error 05), but a
// notification registration with 100E (03/00 fkt 001 Set {01, our device id, 0C 03}) is answered at once with the
// current C03 status, and later switches come to us as well. nbt_knob registers whenever its MOST link comes up and
// clears the registration (02 ...) when it ends normally.
// Source switch (car 2026-11-17, nbt_key cm): 03/00 fkt 053 op StartResultAck to 100E, data {handle hi, handle lo,
// source hi, source lo}: "00 10 00 bb" switched the car to AUX (C03 to the HMI, then ResultAck "00 10"); with 2 or 3
// bytes 100E answers ErrorAck "06 01" (parameter 1 wrong). No HMI page change, the picture stays.
// Play -> AUX (user 2026-11-17): when the phone's media state changes to playing (2) while a session runs and the known
// source is not AUX, nbt_knob switches the source to AUX that way. Only on the change to "playing" (session start with
// autoplay, play on the phone / wheel / in Android Auto), so leaving AUX while music plays is not undone.
// NBT_PLAY_AUX=0 off.
// NBT_AUX_SOURCE = source byte of AUX (hex, default BB), NBT_AUX_PAUSE=0 off, NBT_AUX_RESUME=0 pause only.
// 2026-11-18: with Android Auto sound in the car (aa_media_out) native_service sets NBT_AUX_SOURCE=f6 (main source
// f6, io-amp input input_stereo_1) and NBT_AUX_OPENS_AA=0: everything above that says AUX then means f6, and iDrive's
// own AUX stays factory. Without it (file media-jack) the Android Auto source is AUX, as before.
// NAV while Android Auto is on screen (user 2026-11-18, instead of a long NAV press): the source is not AUX -> switch
// to AUX and start playback on the phone unless it already plays (or unless our earlier AUX-left pause is about to be
// undone by the back-to-AUX resume); the source is AUX -> PLAY_PAUSE (85) to the phone. The show request still goes
// out, because iDrive opens its navigation on NAV. NBT_NAV_AUDIO=0 off.
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
// Controller messages in iDrive (HmiMain3D CHmiCCSController::onCCSPushButton, disassembled 2026-11-19): the counter is
// not checked for sequence. A C20 is dropped when its counter is ff or when the controller is not "alive" yet (no
// message since HMI start); either case marks it alive and makes iDrive send the rotary encoder config again. Other
// drops: sender / receiver not 01 / 1D, HMI power state not 3..5, key state unchanged (press while pressed).
// nbt_knob asks the controller for its counter at start (Get, E5/01 fkt C20, to 3002 from our own device), only to
// know whether the controller has sent anything yet (answer ff: nothing since boot).
uint16_t g_selfDev = 0;
bool g_counterKnown = false;
uint8_t g_ccsCounter = 0;              // its last counter
bool g_tiltRightDown = false;
bool g_backPending = false;            // tilt right released: the main loop sends BACK
long long g_ignoreBackUntil = 0;       // our own BACK coming back: not for the phone
// NAV on request (2026-11-19, parking assistant): switched off with its button while it had come up over Android Auto,
// iDrive stays on its inactive parking screen and ignores the HMI plugin's focus events there (car log: 5 events, none
// taken); the driver's NAV there brings Android Auto back (iDrive leaves the screen, our NAV handler sends the show
// request). The plugin creates /fs/ram3/nbt-aa-hmi-nav; nbt_knob sends one NAV (C20 hard key bit 0x20, as the
// controller would), which comes back here and is handled exactly like the driver's NAV.
bool g_navKeyPending = false;
// our own C20 (BACK guard, BACK, NAV) comes back here with our own device as its source (car 2026-11-19: 2032): while
// they can still come back, a C20 does not change the controller's addresses or counter
long long g_ownC20Until = 0;
// BACK guard (2026-11-17, test): iDrive's BACK goes back in its history and leaves Android Auto; bringing the page back
// with a focus event per BACK made iDrive ignore the event after a burst (car log 2026-11-17, as on 2026-11-07). A BACK
// pressed while iDrive's focus is in the split screen only brings the focus back (car: our BACK after a tilt right).
// So on the driver's BACK press, while Android Auto is on screen, nbt_knob sends a tilt right (press + release) at once:
// if iDrive acts on the BACK release, the focus is in the split screen by then and BACK just returns it. Our own tilt
// is neither answered with a BACK nor sent to the phone. /fs/ram3/nbt-aa-guard tells the plugin to wait for it.
// NBT_BACK_GUARD=0 off. Car 2026-11-17: works (iDrive acts on the BACK release).
// Tilt left (2026-11-17, test): iDrive treats it as "back" too, but climbs one of the 8 nested hold levels per tilt (car
// log: level 7 -> 0, then the entry page and one hold event). The same guard on the tilt left press: if iDrive acts on
// the tilt release with its focus in the split screen, no level is used. NBT_TILT_GUARD=0 off.
// Car 2026-11-17: the focus went into the split screen (its options entry showed) and came back only sometimes, slowly.
// So the driver's tilt left release is now answered with a BACK, exactly like the tilt right fix: the focus returns at
// once. Our own tilt coming back (press, then release, within 300 ms) is recognised in order, so the driver's release
// is not mistaken for ours.
bool g_backGuard = true;
bool g_tiltGuard = true;
bool g_guardPending = false;
bool g_guardTiltLeft = false;          // the guard was for a tilt left: its release gets a BACK
int g_ownTiltStage = 0;                // our guard tilt coming back: 2 = its press next, 1 = its release next
long long g_ownTiltUntil = 0;
long long g_ignoreTiltUntil = 0;       // our tilt right: not for the phone (onKnob)

bool sessionRunning();
// the controller tricks (BACK guard, tilt left guard, BACK after tilt right) only while the Android Auto picture is on
// screen AND a phone session runs (user 2026-11-17: not in the Android Auto menus while no phone is connected)
bool aaActive() { return access("/fs/ram3/nbt-aa-hidden", F_OK) != 0 && sessionRunning(); }

void onC20(const nbt::most::View& m, void*) {
    if (m.len < 7) return;
    pthread_mutex_lock(&g_backMx);
    if (g_selfDev && m.dst == g_selfDev) {   // the answer to our Get: only its counter
        const bool fresh = !g_counterKnown;
        if (m.data[2] != 0xFF) { g_ccsCounter = m.data[2]; g_counterKnown = true; }
        pthread_mutex_unlock(&g_backMx);
        if (fresh) printf("[Knob] controller answered the Get (from %04X): counter %02x%s\n", m.src, m.data[2],
                          m.data[2] == 0xFF ? " (none)" : "");
        return;
    }
    const long long tm = nowMs();
    if (m.data[2] != 0xFF && tm >= g_ownC20Until) {
        g_ccsSrc = m.src; g_ccsDst = m.dst; g_ccsCounter = m.data[2]; g_counterKnown = true;
    }
    if (g_backGuard && m.data[5] == 0x00 && m.data[3] == 1 && (m.data[6] & 0x02) && tm >= g_ignoreBackUntil &&
        aaActive())
        g_guardPending = true;                                         // the driver's BACK press: tilt right now
    if (m.data[5] == 0x1D && g_ownTiltStage && tm < g_ownTiltUntil) {   // our own guard tilt coming back
        if (g_ownTiltStage == 2 && m.data[3] == 1 && m.data[4] == 2) { g_ownTiltStage = 1; pthread_mutex_unlock(&g_backMx); return; }
        if (g_ownTiltStage == 1 && m.data[3] == 0) { g_ownTiltStage = 0; pthread_mutex_unlock(&g_backMx); return; }
    }
    if (tm >= g_ownTiltUntil) g_ownTiltStage = 0;
    if (g_tiltGuard && m.data[5] == 0x1D && m.data[3] == 1 && m.data[4] == 8 && aaActive()) {
        g_guardPending = true;                                         // the driver's tilt left press: the same guard
        g_guardTiltLeft = true;
    } else if (m.data[5] == 0x1D && m.data[3] == 0 && g_guardTiltLeft) {
        g_guardTiltLeft = false;                                       // its release: BACK, as after a tilt right
        if (aaActive()) g_backPending = true;
    }
    if (m.data[5] == 0x1D && m.data[3] == 1 && m.data[4] == 2) g_tiltRightDown = true;
    else if (m.data[5] == 0x1D && m.data[3] == 0 && g_tiltRightDown) {
        g_tiltRightDown = false;
        if (aaActive()) g_backPending = true;   // Android Auto on screen
    }
    pthread_mutex_unlock(&g_backMx);
}

uint8_t nextCounter(uint8_t c) { c = (uint8_t)(c + 1); return c == 0xFF ? 0 : c; }   // 0xFF = "ignore" (firmware)

// hard key message as the controller sends it; sensor bit = slot of ccs_decoder.hpp (0x01 MENU, 0x02 BACK, ...)
nbt::most::Msg hardKeyMsg(uint16_t src, uint16_t dst, uint8_t counter, bool down, uint8_t bit) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = src; m.fblock = 0xE5; m.inst = 0x01; m.fkt = 0xC20; m.opType = 0xC; m.hdrTag = 0xFFFFu;
    const uint8_t d[7] = { 0x01, 0x1D, counter, (uint8_t)(down ? 1 : 0), 0x00, 0x00, bit };
    m.data.assign(d, d + 7);
    return m;
}
// counter ff, status 3 (no change): dropped by iDrive, which marks the controller alive (see g_selfDev)
nbt::most::Msg rebaseMsg(uint16_t src, uint16_t dst) {
    nbt::most::Msg m = hardKeyMsg(src, dst, 0xFF, false, 0x00);
    m.data[3] = 0x03;
    return m;
}
nbt::most::Msg backMsg(uint16_t src, uint16_t dst, uint8_t counter, bool down) {
    return hardKeyMsg(src, dst, counter, down, 0x02);   // hard key, sensor BACK
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
int g_lastSource = -1;          // -1 unknown until the first status, -2 no main source (empty table)
bool g_wheelAuxOnly = true;

bool wheelAllowed(const char* what) {
    if (!g_wheelAuxOnly || g_lastSource < 0 || g_lastSource == (int)g_auxSource) return true;
    printf("[Wheel] %s ignored: source %02X is not the Android Auto source %02X\n", what, g_lastSource, g_auxSource);
    return false;
}

long long g_listUntil = 0;      // roller for iDrive's phone list until then
unsigned g_listHoldMs = 4000;

void onWheel(const nbt::most::View& m, void*) {
    if (m.fkt == 0x400 && m.len >= 1 && m.data[0] == 1 && g_listHoldMs) {   // phone button pressed
        const bool copy = g_listUntil - (nowMs() + g_listHoldMs) > -200;   // the same press, once per HMI receiver
        g_listUntil = nowMs() + g_listHoldMs;
        if (!copy) printf("[Wheel] phone button: roller stays with iDrive (phone list) until %u s without a turn\n", g_listHoldMs / 1000);
        return;
    }
    if (m.len < 1 || (m.fkt != 0xC30 && m.fkt != 0xC31)) return;
    const uint8_t v = m.data[0];
    const long long t = nowMs();
    // the same message once per HMI receiver, ~1 ms apart: take the first copy only
    if (m.fkt == g_wheelLastFkt && v == g_wheelLastVal && t - g_wheelLastMs < 40) return;
    g_wheelLastFkt = m.fkt; g_wheelLastVal = v; g_wheelLastMs = t;
    static bool seenC30 = false;
    if (m.fkt == 0xC30 && !seenC30) {
        seenC30 = true;
        printf("[Wheel] first roller status %02x after start: baseline only, no key\n", v);
        return;
    }
    if (nowMs() < g_listUntil) {                                         // scrolling iDrive's phone list
        if (m.fkt == 0xC30) g_listUntil = nowMs() + g_listHoldMs;
        printf("[Wheel] %s %02x: phone list open, not for the phone\n", m.fkt == 0xC30 ? "roller" : "push", v);
        return;
    }
    if (m.fkt == 0xC30) {
        if (v != 0x80 && v != 0x7e && v != 0x7f) printf("[Wheel] roller value %02x: not one step, ignored\n", v);
        const unsigned key = v == 0x80 ? g_wheelUp : v == 0x7e ? g_wheelDown : 0;
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

// registers (set) / clears our notification for the source status with the source master 100E
bool sourceNotification(nbt::most::Client& most, bool set) {
    const uint16_t me = (uint16_t)most.deviceId();
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;
    m.dst = 0x100E; m.src = me; m.fblock = 0x03; m.inst = 0x00; m.fkt = 0x001; m.opType = 0x0; m.hdrTag = 0xFFFFu;
    const uint8_t d[5] = { (uint8_t)(set ? 0x01 : 0x02), (uint8_t)(me >> 8), (uint8_t)me, 0x0C, 0x03 };
    m.data.assign(d, d + 5);
    const bool ok = most.send(m);
    printf("[Source] notification %s for %04X at 100E: %s\n", set ? "set" : "cleared", me, ok ? "sent" : "FAILED");
    return ok;
}

long long g_ourSourceRequestMs = -100000;   // our own switch request: the AUX that follows is not the driver's
bool g_auxOpensAa = true;
// one ConnectionMaster command to 100E (car 2026-11-18): fkt 053 BuildHighSyncConnection / 054 Remove..., op 6
// StartResultAck, data {handle hi, lo, connection hi, lo}. Returns the handle (0 = not sent).
unsigned connectionCmd(nbt::most::Client& most, unsigned fkt, unsigned conn) {
    static unsigned handle = 0x0100;
    handle = (handle + 1) & 0xFFFF;
    if (handle < 0x0100) handle = 0x0100;                         // keep clear of the HMI's small handles
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;
    m.dst = 0x100E; m.src = (uint16_t)most.deviceId(); m.fblock = 0x03; m.inst = 0x00; m.fkt = (uint16_t)fkt; m.opType = 0x6;
    m.hdrTag = 0xFFFFu;
    const uint8_t d[4] = { (uint8_t)(handle >> 8), (uint8_t)handle, (uint8_t)(conn >> 8), (uint8_t)conn };
    m.data.assign(d, d + 4);
    return most.send(m) ? handle : 0;
}

// asks the source master 100E to switch to `source` (ConnectionMaster 053, see the header)
bool requestSource(nbt::most::Client& most, unsigned source) {
    g_ourSourceRequestMs = nowMs();
    const unsigned handle = connectionCmd(most, 0x053, source);
    printf("[Source] switch to %02X requested (handle %04X): %s\n", source, handle, handle ? "sent" : "FAILED");
    return handle != 0;
}

// --- Android Auto prompts while another source plays (2026-11-18, aa_media_out) ---
// The session writes prompts that come while the main source is not Android Auto's into io-amp input_stereo_3 and
// creates kPromptReq; nbt_knob then builds connection b8 (the browser: interrupting, mutes the main source, plays
// input_stereo_3; car-tested with nbt_key + nbt_a2i_beep), writes kPromptReady when 100E has built it and removes it
// (054) once kPromptReq is gone. kSourceFile tells the session the current main source.
const char* const kSourceFile = "/fs/ram3/nbt-aa-source";
const char* const kPromptReq = "/fs/ram3/nbt-aa-prompt";
const char* const kPromptReady = "/fs/ram3/nbt-aa-prompt-ready";
unsigned g_promptConn = 0xB8;      // NBT_PROMPT_CONN (0 = off)
bool g_promptBuilt = false;        // our b8 is up or requested
unsigned g_promptHandle = 0;       // handle of our b8 build
long long g_promptNext = 0;

void promptRouteIfNeeded(nbt::most::Client& most) {
    if (!g_promptConn) return;
    const long long now = nowMs();
    if (now < g_promptNext) return;
    g_promptNext = now + 50;
    const bool want = access(kPromptReq, F_OK) == 0 && sessionRunning();
    if (want && !g_promptBuilt) {
        unlink(kPromptReady);
        g_promptHandle = connectionCmd(most, 0x053, g_promptConn);
        g_promptBuilt = true;
        printf("[Prompt] Android Auto prompt: build %02X (handle %04X)\n", g_promptConn, g_promptHandle);
    } else if (!want && g_promptBuilt) {
        const unsigned h = connectionCmd(most, 0x054, g_promptConn);
        g_promptBuilt = false;
        g_promptHandle = 0;
        unlink(kPromptReady);
        printf("[Prompt] prompt over: remove %02X (handle %04X)\n", g_promptConn, h);
    }
}

// 100E's answer to our build / remove: ResultAck (D) or ErrorAck (9, "06 nn" = parameter nn wrong, 42 not available)
void onSourceResult(const nbt::most::View& m, void*) {
    if ((m.fkt != 0x053 && m.fkt != 0x054) || m.len < 2 || m.data[0] == 0x00) return;  // handles below 0x0100: the HMI's
    const unsigned handle = (unsigned)m.data[0] << 8 | m.data[1];
    printf("[Source] %s handle %04X: %s", m.fkt == 0x053 ? "build" : "remove", handle,
           m.op == 0xD ? "done" : m.op == 0x9 ? "ERROR" : "?");
    for (size_t i = 2; i < m.len && i < 8; ++i) printf(" %02x", m.data[i]);
    printf("\n");
    if (m.fkt == 0x053 && handle == g_promptHandle && g_promptBuilt && m.op == 0xD) {
        if (FILE* f = fopen(kPromptReady, "w")) fclose(f);
    }
}

// Main audio sources: the connection IDs of category 01 in HmiMain3D's connection table (0x9877344, car 2026-11-18).
// The C03 table can list more than one connection: an interrupting one (traffic news, a prompt, the browser b8) sits
// next to the main source and mutes it. The source is the first main-source entry; none = -2 (car 2026-11-18: CD/DVD
// without a disc leaves the table empty, "00 00", so Android Auto music had no connection while nbt_knob still
// believed in F6 and NAV only toggled play / pause).
const uint16_t kMainSources[] = {
    0x001, 0x068, 0x062, 0x018, 0x020, 0x024, 0x027, 0x028, 0x086, 0x01b, 0x050, 0x053, 0x04d, 0x080, 0x07d,
    0x077, 0x09a, 0x09d, 0x0a0, 0x0b4, 0x0bb, 0x0be, 0x123, 0x0c1, 0x122, 0x1b0, 0x1b3, 0x088, 0x0c7, 0x150,
    0x153, 0x156, 0x159, 0x0ea, 0x0fc, 0x0ed, 0x0fd, 0x0f0, 0x0fe, 0x0f3, 0x0f6, 0x0f9, 0x1d0, 0x1d3, 0x1d6,
    0x0df, 0x100, 0x109, 0x110, 0x119, 0x124, 0x127, 0x12a, 0x12d, 0x200, 0x209, 0x20f, 0x212, 0x10c, 0x11c,
    0x20c, 0x2a0, 0x2a3, 0x166, 0x169, 0x16c, 0x16f, 0x071, 0x220, 0x221
};
bool isMainSource(unsigned id) {
    for (size_t i = 0; i < sizeof(kMainSources) / sizeof(kMainSources[0]); ++i)
        if (kMainSources[i] == id) return true;
    return false;
}
int g_prevMain = -1;            // the last real main source (for "AUX chosen", which must look past an empty table)

void onSource(const nbt::most::View& m, void*) {
    if (m.fkt != 0xC03 || m.len < 2) return;
    // {00 00} then per connection {id hi, id lo, state}: state 00 playing, 04 muted by an interrupting connection
    int s = -2;
    for (size_t i = 2; i + 2 < (size_t)m.len && i < 2 + 3 * 8; i += 3) {
        const unsigned id = (unsigned)m.data[i] << 8 | m.data[i + 1];
        if (isMainSource(id)) { s = (int)id; break; }
    }
    if (s == g_lastSource) return;                        // the same status once per receiver
    const int prev = g_lastSource;
    g_lastSource = s;
    printf("[Source] %02X -> %02X (table:", prev < 0 ? 0xFF : prev, s < 0 ? 0xFF : s);
    for (size_t i = 2; i < m.len && i < 26; ++i) printf(" %02x", m.data[i]);
    printf(")\n");
    if (FILE* f = fopen(kSourceFile, "w")) {               // for the session's prompt routing (aa_media_out)
        if (s >= 0) fprintf(f, "%04x\n", s); else fputs("none\n", f);
        fclose(f);
    }
    const int prevMain = g_prevMain;
    if (s >= 0) g_prevMain = s;
    // AUX chosen in iDrive (user 2026-11-17: the AUX page is the phone's audio, show Android Auto instead): the driver
    // switched to AUX (not our own request of the last 3 s) while a session runs -> show request to the HMI plugin.
    // NBT_AUX_OPENS_AA=0 off.
    if (g_auxOpensAa && prevMain >= 0 && prevMain != (int)g_auxSource && s == (int)g_auxSource &&
        nowMs() - g_ourSourceRequestMs > 3000 && sessionRunning()) {
        if (FILE* f = fopen("/fs/ram3/nbt-aa-show", "w")) fclose(f);
        printf("[Source] AUX chosen in iDrive: Android Auto to the front (show request)\n");
    }
    if (!g_auxPause || prev == -1) return;                // -1: the first status after start, nothing changed
    if (prev == (int)g_auxSource && s != (int)g_auxSource) {
        const int st = mediaState();
        g_wePaused = sessionRunning() && st != 3;          // already paused: nothing to do, nothing to resume
        printf("[Source] Android Auto source %02X left, phone media state %d\n", g_auxSource, st);
        if (g_wePaused) { wheelKeyLine(85, 1, "AUX_LEFT_PAUSE"); wheelKeyLine(85, 0, "AUX_LEFT_PAUSE"); }
    } else if (s == (int)g_auxSource && g_wePaused) {
        g_wePaused = false;
        const int st = mediaState();
        printf("[Source] back on the Android Auto source %02X, phone media state %d\n", g_auxSource, st);
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
// --- phone starts playing while the source is not AUX: switch the source to AUX (see the header) ---
bool g_playAux = true;
bool g_navOpensAa = true;
int g_prevMedia = -1;           // -1 = not read yet (the first read is only the starting point)
long long g_mediaNextCheck = 0;
bool g_navAudio = true;
bool g_navAudioPending = false;  // NAV pressed while Android Auto was on screen; under g_backMx

void playSwitchIfNeeded(nbt::most::Client& most) {
    if (!g_playAux) return;
    const long long now = nowMs();
    if (now < g_mediaNextCheck) return;
    g_mediaNextCheck = now + 200;
    const int st = mediaState(), prev = g_prevMedia;
    g_prevMedia = st;
    if (prev < 0 || st != 2 || prev == 2) return;                // only the change to "playing"
    if (g_lastSource == -1 || g_lastSource == (int)g_auxSource || !sessionRunning()) return;   // -2 (no source): switch
    printf("[Source] phone plays while the source is %02X\n", g_lastSource < 0 ? 0xFF : g_lastSource);
    requestSource(most, g_auxSource);
}

// main thread: NAV pressed while Android Auto was on screen -> Android Auto audio / play-pause (see the header)
void navAudioIfNeeded(nbt::most::Client& most) {
    pthread_mutex_lock(&g_backMx);
    const bool go = g_navAudioPending;
    g_navAudioPending = false;
    pthread_mutex_unlock(&g_backMx);
    if (!go || !sessionRunning()) return;
    const int st = mediaState();
    const bool onAux = g_lastSource == (int)g_auxSource;
    printf("[Knob] NAV in Android Auto: source %02X, phone media state %d\n", g_lastSource < 0 ? 0xFF : g_lastSource, st);
    if (!onAux) {
        requestSource(most, g_auxSource);
        if (st == 2) return;                                       // already playing
        if (g_wePaused && g_auxResume) return;                     // the back-to-AUX resume (onSource) starts it
        g_prevMedia = 2;                                           // our own play: no second switch request
        g_mediaNextCheck = nowMs() + 3000;
    }
    wheelKeyLine(85, 1, "NAV_PLAY_PAUSE");
    wheelKeyLine(85, 0, "NAV_PLAY_PAUSE");
}

void favouritesIfNeeded(nbt::most::Client& most) {
    if (g_favDone) return;
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
    if (g_auxButton && (!dst || !src)) return;                    // HMI address not known yet (favourite only)
    // Decoder mode (2026-11-19): brought to the front automatically while iDrive was still starting, nbt_decoder's
    // window got stuck for good (picture up at 22.8, 31.5, 32.0, 33.3 s after boot: stuck; 34.3 s: worked; opened by
    // hand later: always worked). With decoder.mode the show waits until the head unit
    // has run for decoder.showdelay seconds (default 40); the session itself starts as early as before.
    if (access("/fs/ram3/nbt-aa-native/decoder.mode", F_OK) == 0) {
        static long long minUp = -1;
        if (minUp < 0) {
            minUp = 40;
            if (FILE* d = fopen("/var/opt/hmi/data/rw/NbtAndroidAutoRT/decoder.showdelay", "r")) {
                long v;
                if (fscanf(d, "%ld", &v) == 1 && v >= 0 && v <= 300) minUp = v;
                fclose(d);
            }
        }
        struct timespec up;
        clock_gettime(CLOCK_MONOTONIC, &up);
        if (up.tv_sec < minUp) {
            static bool told = false;
            if (!told) {
                told = true;
                printf("[Knob] start at boot: decoder mode, waiting until %lld s after boot (now %ld s)\n", minUp,
                       (long)up.tv_sec);
            }
            return;   // checked again in 200 ms
        }
    }
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
        if (FILE* s = fopen("/fs/ram3/nbt-aa-show", "w")) fclose(s);   // the HMI plugin shows the picture (see the header)
        printf("[Knob] start at boot: Android Auto to the front (show request)\n");
    }
}

nbt::most::Msg tiltRightMsg(uint16_t src, uint16_t dst, uint8_t counter, bool down) {
    nbt::most::Msg m = backMsg(src, dst, counter, down);
    m.data[4] = down ? 0x02 : 0x00; m.data[5] = 0x1D; m.data[6] = 0x01;   // car trace: 01 1d c 01 02 1d 01 / 01 1d c 00 00 1d 01
    return m;
}

// main thread: the tilt right of the BACK guard
void sendGuardIfPending(nbt::most::Client& most) {
    pthread_mutex_lock(&g_backMx);
    if (!g_guardPending) { pthread_mutex_unlock(&g_backMx); return; }
    g_guardPending = false;
    const uint16_t src = g_ccsSrc, dst = g_ccsDst;
    const uint8_t c1 = nextCounter(g_ccsCounter), c2 = nextCounter(c1);
    g_ccsCounter = c2;
    g_ignoreTiltUntil = nowMs() + 600;
    g_ownC20Until = nowMs() + 600;
    g_ownTiltStage = 2; g_ownTiltUntil = nowMs() + 300;
    pthread_mutex_unlock(&g_backMx);
    if (!src || !dst) { pthread_mutex_lock(&g_backMx); g_ownTiltStage = 0; pthread_mutex_unlock(&g_backMx); return; }
    if (FILE* f = fopen("/fs/ram3/nbt-aa-guard", "w")) fclose(f);
    most.send(tiltRightMsg(src, dst, c1, true));
    usleep(30000);
    most.send(tiltRightMsg(src, dst, c2, false));
    printf("[Knob] BACK guard: tilt right to iDrive (%04X->%04X, counters %02x %02x)\n", src, dst, c1, c2);
}

// main thread: the NAV asked for by the HMI plugin (parking assistant off).
// Addresses (car 2026-11-19, nbt_key --watch right after boot): the controller sends to the HMI at 2021 (C20 3002 ->
// 2021, and its periodic E5/0F C06 too). Until then nbt_knob had sent NAV to 2025 when no controller key had been seen
// since boot, and iDrive never got it; after one real press it had learned 3002 -> 2021 and NAV worked. So without a
// controller key seen yet, the addresses of the periodic C06 (every 2 s) are used, as for the favourite buttons.
// The first controller message after an HMI start is dropped by iDrive (HmiMain3D onCCSPushButton: it only marks the
// controller "alive"), and so is any with counter ff: one ff message goes first while the controller is silent.
void sendNavIfPending(nbt::most::Client& most) {
    pthread_mutex_lock(&g_backMx);
    if (!g_navKeyPending) { pthread_mutex_unlock(&g_backMx); return; }
    g_navKeyPending = false;
    uint16_t src = g_ccsSrc, dst = g_ccsDst;
    const char* from = "last controller key";
    if (!src || !dst) {
        src = g_hmiSrc; dst = g_hmiDev; from = "periodic C06";
        if (!src || !dst) { src = 0x3002; dst = 0x2021; from = "default"; }
    }
    const bool known = g_counterKnown;
    const uint8_t c1 = nextCounter(g_ccsCounter), c2 = nextCounter(c1);
    g_ccsCounter = c2;
    g_ownC20Until = nowMs() + 1000;
    pthread_mutex_unlock(&g_backMx);
    printf("[Knob] NAV to iDrive for the HMI plugin (%04X->%04X from the %s, counters %02x %02x%s)\n", src, dst, from,
           c1, c2, known ? "" : ", controller silent since boot: ff first");
    if (!known) { most.send(rebaseMsg(src, dst)); usleep(100000); }
    most.send(hardKeyMsg(src, dst, c1, true, 0x20));
    usleep(100000);
    most.send(hardKeyMsg(src, dst, c2, false, 0x20));
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
    g_ownC20Until = nowMs() + 600;
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
    if (!strcmp(ev.what, "TILT_RIGHT")) {
        pthread_mutex_lock(&g_backMx);
        const bool ours = nowMs() < g_ignoreTiltUntil;
        pthread_mutex_unlock(&g_backMx);
        if (ours) { printf("[Knob] TILT_RIGHT %s: our own (BACK guard), not for the phone\n", ev.down ? "down" : "up"); return; }
    }
    if (ev.keycode == nbt::AAK_BACK) {
        pthread_mutex_lock(&g_backMx);
        const bool ours = nowMs() < g_ignoreBackUntil;
        pthread_mutex_unlock(&g_backMx);
        if (ours) { printf("[Knob] %s %s: our own BACK, not for the phone\n", ev.what, ev.down ? "down" : "up"); return; }
        // the driver's BACK while Android Auto is on screen (2026-11-17): iDrive goes back in its own history and leaves
        // the page; this flag tells the HMI plugin to bring Android Auto back (aa_entry_plugin, Checker)
        if (ev.down && access("/fs/ram3/nbt-aa-hidden", F_OK) != 0) {
            int fd = open("/fs/ram3/nbt-aa-back", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }
    }
    // NAV opens Android Auto (user 2026-11-17): iDrive still gets the key and opens its navigation, the show request
    // (HMI plugin, checked every 250 ms) brings Android Auto back over it. NBT_NAV_OPENS_AA=0: NAV stays with iDrive.
    if (!ev.keycode && ev.kind == 1 && ev.down && !ev.cancel && g_navOpensAa && !strcmp(ev.what, "NAV") && sessionRunning()) {
        if (g_navAudio && aaActive()) {                             // already in Android Auto: its audio (main thread)
            pthread_mutex_lock(&g_backMx);
            g_navAudioPending = true;
            pthread_mutex_unlock(&g_backMx);
        }
        if (FILE* s = fopen("/fs/ram3/nbt-aa-show", "w")) fclose(s);
        printf("[Knob] NAV: Android Auto to the front (show request)\n");
        return;
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
    // the same key while the picture is hidden (car 2026-11-18): a show request may still be pending in the HMI plugin
    // (its retry after an ignored focus event), which must not pull Android Auto back over the iDrive screen chosen now
    if (!ev.keycode && ev.kind == 1 && ev.down && !ev.cancel) {
        int fd = open("/fs/ram3/nbt-aa-leave", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
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
    g_aaButton = wheelKey("NBT_AA_FAVORITE", 1);        // now only on (show request) / 0 = off
    if (g_auxButton > 8 || trace) g_auxButton = 0;   // a --trace copy (tests, boot.sh trace) never presses anything
    if (trace) g_aaButton = 0;
    g_playAux = wheelKey("NBT_PLAY_AUX", 1) != 0 && !trace;
    g_auxOpensAa = wheelKey("NBT_AUX_OPENS_AA", 1) != 0 && !trace;
    g_navOpensAa = wheelKey("NBT_NAV_OPENS_AA", 1) != 0 && !trace;
    g_navAudio = wheelKey("NBT_NAV_AUDIO", 1) != 0 && !trace;
    g_backGuard = wheelKey("NBT_BACK_GUARD", 1) != 0 && !trace;
    g_tiltGuard = wheelKey("NBT_TILT_GUARD", 1) != 0 && !trace;
    if (!most.addRoute(0xE5, 0x0F, 0xC06, 0xC, &onE5Periodic, NULL)) printf("[Knob] no room for the HMI address route\n");
    {
        const char* v = getenv("NBT_AUX_SOURCE");
        if (v && *v) g_auxSource = (unsigned)strtoul(v, 0, 16) & 0xFF;
        g_auxPause = wheelKey("NBT_AUX_PAUSE", 1) != 0 && !trace;   // a --trace copy never sends keys
        g_auxResume = wheelKey("NBT_AUX_RESUME", 1) != 0;
        g_wheelAuxOnly = wheelKey("NBT_WHEEL_AUX_ONLY", 1) != 0;
    g_listHoldMs = wheelKey("NBT_LIST_HOLD_S", 4) * 1000;
    }
    if (!most.addRoute(0x03, 0x00, 0xC03, 0xC, &onSource, NULL)) printf("[Knob] no room for the audio source route\n");
    if (!most.addRoute(0x03, 0x00, 0x053, nbt::most::kAny, &onSourceResult, NULL)) printf("[Knob] no room for the switch result route\n");
    if (!most.addRoute(0x03, 0x00, 0x054, nbt::most::kAny, &onSourceResult, NULL)) printf("[Knob] no room for the remove result route\n");
    {
        const char* v = getenv("NBT_PROMPT_CONN");             // hex, 0 = no prompts over another source
        g_promptConn = trace ? 0 : (v && *v ? (unsigned)strtoul(v, 0, 16) : 0xB8u);
    }
    unlink(kPromptReady);
    unlink(kSourceFile);
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
    bool subscribed = false;
    int counterAsks = 0;
    long long nextCounterAsk = 0;
    while (!g_stop) {
        usleep(20000);
        if (most.state() != nbt::most::kUp) subscribed = false;
        else if (!subscribed) subscribed = sourceNotification(most, true);   // answered with the current source
        // ask the controller for its counter (see g_selfDev): at start, again after 5 s and 30 s if no answer
        if (!trace && most.state() == nbt::most::kUp && counterAsks < 3 && nowMs() >= nextCounterAsk) {
            pthread_mutex_lock(&g_backMx);
            const bool known = g_counterKnown;
            pthread_mutex_unlock(&g_backMx);
            if (known) counterAsks = 3;
            else {
                g_selfDev = (uint16_t)most.deviceId();
                nbt::most::Msg q;
                q.txType = 0; q.varTag = 1; q.varVal = 0;
                q.dst = 0x3002; q.src = g_selfDev; q.fblock = 0xE5; q.inst = 0x01; q.fkt = 0xC20; q.opType = 0x1;
                q.hdrTag = 0xFFFFu;
                const bool ok = most.send(q);
                printf("[Knob] Get E5/01 C20 to the controller 3002 (counter): %s\n", ok ? "sent" : "FAILED");
                ++counterAsks;
                nextCounterAsk = nowMs() + (counterAsks == 1 ? 5000 : 25000);
            }
        }
        if (!trace && unlink("/fs/ram3/nbt-aa-hmi-nav") == 0) {
            pthread_mutex_lock(&g_backMx);
            g_navKeyPending = true;
            pthread_mutex_unlock(&g_backMx);
        }
        if (!trace) { sendGuardIfPending(most); sendBackIfPending(most); sendNavIfPending(most); }
        favouritesIfNeeded(most);
        playSwitchIfNeeded(most);
        navAudioIfNeeded(most);
        if (!trace) promptRouteIfNeeded(most);
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
    if (g_promptBuilt) { connectionCmd(most, 0x054, g_promptConn); unlink(kPromptReady); usleep(200000); }
    if (subscribed) { sourceNotification(most, false); usleep(200000); }
    printf("nbt_knob: end forwarded=%u dropped=%u\n", g_sent, g_dropped);
    fflush(stdout);
    _exit(0);   // the DSI library threads are not joined; leave without running their destructors
}
