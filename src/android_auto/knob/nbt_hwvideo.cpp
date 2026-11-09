// nbt_hwvideo - show the Android Auto picture through the HARDWARE video path of the original NBT (vehicle-proven
// 2026-11-07 with a test clip: full screen in front of iDrive, 23 fps, 27 % CPU, no texture uploads).
//
// The decoder (NmeCmdLine -vd NmeDeviceScreen with the stock id, Cinemo output 1) renders into the window
// CinemoVideo1_TaskA on the Intel side. This program then does what the HMI's video page does, plus two things the HMI
// does only on its own video page:
//   1. MOST to VideoApp (sequence captured from the HMI, build/video-trace-20261107):
//        D7/01 060 StartResult {h, 16, 01}   BuildVideoConnection source 0x16 -> sink CID
//        D8/01 C05 Set {16 00}               CurrentVideoSource
//        D8/01 C03 Set {0,0,800,480}         VideoViewport
//        D8/01 C01 StartResult {h}           StartVideo -> dev-nvideocapture captures the Intel output (800x480)
//        D8/01 C04 Set {0,0,1280,480}        ScreenViewPort -> WAL window VIDEO (0x41c) 1280x480
//   2. DisplayManager: CinemoVideo1_TaskA visible, size 800 x (480*480/rows) so that the phone's content rows (0..rows-1)
//      fill the 480 captured lines (rows=300: 800x768, the GPU then scales 800->1280: 1.6 in both directions)
//   3. WAL: setVisible(0x41c, 1) and setZOrder(0x41c -> 1) (a SMALLER z is in front; the HMI window 0x100 has 256)
// While /fs/ram3/nbt-aa-hidden exists (iDrive page on screen) the video window is hidden, without stopping anything.
// SIGTERM: stop sequence (D8 C02 StopVideo, D7 061 RemoveVideoConnection, D8 C05 {00 00}) and DisplayManager invisible.
//
// usage: nbt_hwvideo [--rows N]      (N = content rows of the 800x480 phone canvas, default 300; 480 = whole frame)
// Needs LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib (DSI and WAL libraries).
#include "most/client.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
long long nowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

nbt::most::Msg req(uint16_t dst, uint8_t fb, uint16_t fkt, uint8_t op, const uint8_t* d, size_t n) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // eTransmissionTXControl, as the vendor client sends (vehicle-tested)
    m.dst = dst; m.src = 0; m.fblock = fb; m.inst = 1; m.fkt = fkt; m.opType = op; m.hdrTag = 0xFFFFu;
    m.data.assign(d, d + n);
    return m;
}
void be16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// --- WAL (HMI interface: window visibility and z-order) ---
typedef struct { unsigned long id; unsigned short z; unsigned short pad; } OrderTypeDef;
void* g_hmi = 0;
void (*g_setz)(void*, const OrderTypeDef*, unsigned long) = 0;
void (*g_setv)(void*, unsigned long, int) = 0;
bool walInit() {
    void* lib = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { printf("[hw] WAL dlopen: %s\n", dlerror()); return false; }
    void* (*create)() = (void* (*)())dlsym(lib, "_ZN3wal15WalHmiInterface9createPtrEv");
    g_setz = (void (*)(void*, const OrderTypeDef*, unsigned long))dlsym(lib, "_ZN3wal15WalHmiInterface9setZOrderEPKNS0_12OrderTypeDefEm");
    g_setv = (void (*)(void*, unsigned long, int))dlsym(lib, "_ZN3wal15WalHmiInterface10setVisibleEmb");
    if (!create || !g_setz || !g_setv) { printf("[hw] WAL symbols missing\n"); return false; }
    g_hmi = create();
    return g_hmi != 0;
}
// With a narrower viewport (1:1 picture in the middle) the iDrive HMI window (WAL 256) would show left and right of the
// picture: it is made invisible while Android Auto is in front, so the sides are black (car 2026-11-08, user-confirmed).
// Restored on every hide, on exit, on fatal signals, and by "nbt_hwvideo --hmi-visible" (native_service after a session).
const unsigned long kHmiWindow = 256;
bool g_blankHmi = false;
void walShow(bool on) {
    if (!g_hmi) return;
    if (on) { OrderTypeDef o = { 0x41c, 1, 0 }; g_setz(g_hmi, &o, 1); }
    if (!on && g_blankHmi) g_setv(g_hmi, kHmiWindow, 1);   // HMI back first, then the picture away
    g_setv(g_hmi, 0x41c, on ? 1 : 0);
    if (on && g_blankHmi) g_setv(g_hmi, kHmiWindow, 0);
}
void onFatal(int sig) {
    if (g_hmi && g_setv) g_setv(g_hmi, kHmiWindow, 1);    // never leave iDrive invisible
    signal(sig, SIG_DFL);
    _exit(128 + sig);
}
// --- DisplayManager (Intel side) ---
void dm(const char* cmd) {
    int fd = open("/dev/DisplayManager:0", O_WRONLY);
    if (fd < 0) { printf("[hw] DisplayManager open failed\n"); return; }
    char line[80]; int n = snprintf(line, sizeof(line), "CinemoVideo1_TaskA,%s;", cmd);
    if (write(fd, line, (size_t)n) != n) printf("[hw] DisplayManager write failed: %s\n", line);
    close(fd);
}
// VideoApp's device addresses change with every start (car 2026-11-07: 0x2007/0x2008 before an iDrive restart, no
// answer after it). They are taken from the server's registry attribute 0xC0000001: u32 count, then count entries of
// {u16 address, u16 fblock << 8 | inst}.
volatile unsigned g_addrD7 = 0, g_addrD8 = 0;
bool g_printAll = false;   // --registry-all: print every FBlock of the registry (read-only investigation)
void onInfo(uint32_t id, const uint8_t* p, uint32_t len, void*) {
    if (id != 0xC0000001u || len < 4) return;
    uint32_t n = nbt::most::rdLe32(p);
    for (uint32_t i = 0; i < n && 4 + i * 4 + 4 <= len; ++i) {
        unsigned addr = nbt::most::rdLe16(p + 4 + i * 4), key = nbt::most::rdLe16(p + 6 + i * 4);
        if (key == 0xD701) g_addrD7 = addr;
        if (key == 0xD801) g_addrD8 = addr;
    }
    printf("[hw] registry: %u entries, D7/01 at 0x%04X, D8/01 at 0x%04X\n", (unsigned)n, g_addrD7, g_addrD8);
    for (uint32_t i = 0; i < n && i < (g_printAll ? 512u : 64u) && 4 + i * 4 + 4 <= len; ++i) {
        unsigned addr = nbt::most::rdLe16(p + 4 + i * 4), key = nbt::most::rdLe16(p + 6 + i * 4);
        if (g_printAll || ((key >> 8) >= 0xD0 && (key >> 8) <= 0xDF)) printf("[hw]   %04X = %02X/%02X\n", addr, key >> 8, key & 0xff);
    }
}
// replies of VideoApp to OUR requests (dst = our device): ResultAck (op D) of BuildVideoConnection / StartVideo, errors
// (op 9 ErrorAck, op F Error), and the CurrentVideoSource status we ask for every 5 s
volatile int g_ack060 = 0, g_ackC01 = 0, g_err = 0, g_curSrc = -1;
volatile unsigned g_myDev = 0;
void onReply(const nbt::most::View& m, nbt::most::MsgClass, void*) {
    if (m.fblock != 0xD7 && m.fblock != 0xD8) return;
    if (m.fblock == 0xD8 && m.fkt == 0xC05 && m.op == 0xC && m.len >= 1 && (!g_myDev || m.dst == g_myDev)) g_curSrc = m.data[0];
    if (g_myDev && m.dst != g_myDev) return;
    if (m.op == 0xD || m.op == 0xF || m.op == 0x9) {
        printf("[hw] reply %02X fkt=%03X op=%X len=%u%s\n", m.fblock, m.fkt, m.op, (unsigned)m.len, m.op == 0xD ? "" : " (ERROR)");
        if (m.op == 0xD && m.fblock == 0xD7 && m.fkt == 0x060) g_ack060 = 1;
        if (m.op == 0xD && m.fblock == 0xD8 && m.fkt == 0xC01) g_ackC01 = 1;
        if (m.op != 0xD) g_err++;
    }
}
void dmShow(bool on, int height) {
    char s[32];
    if (on) {
        snprintf(s, sizeof(s), "s,800,%d", height); dm(s);
        dm("p,0,0"); dm("a,255"); dm("v,1");
    } else dm("v,0");
}
}

int main(int argc, char** argv) {
    int rows = 300;
    int vp[4] = {0, 0, 1280, 480};                       // ScreenViewPort x,y,w,h on the 1280x480 display (stock: x,y,w,h,
                                                         // e.g. 213,0,853,480 for 16:9 DVD); --viewport 240,0,800,480 = 1:1
    bool registryOnly = false, keepHmi = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--registry")) registryOnly = true;   // read-only: print the addresses and exit
        else if (!strcmp(argv[i], "--registry-all")) { registryOnly = true; g_printAll = true; }
        else if (!strcmp(argv[i], "--keep-hmi")) keepHmi = true;        // narrower viewport, iDrive stays visible beside it
        else if (!strcmp(argv[i], "--hmi-visible")) {                   // repair: make the iDrive HMI window visible
            if (!walInit()) return 3;
            g_setv(g_hmi, kHmiWindow, 1);
            printf("[hw] HMI window %lu visible\n", kHmiWindow);
            return 0;
        }
        else if (!strcmp(argv[i], "--viewport") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d", &vp[0], &vp[1], &vp[2], &vp[3]) != 4 || vp[0] < 0 || vp[1] < 0 ||
                vp[2] < 160 || vp[3] < 120 || vp[0] + vp[2] > 1280 || vp[1] + vp[3] > 480) return 2;
        }
        else { fprintf(stderr, "usage: %s [--rows 120..480] [--viewport x,y,w,h]\n", argv[0]); return 2; }
    }
    if (rows < 120 || rows > 480) return 2;
    const int height = 480 * 480 / rows;                 // 300 -> 768: content rows fill the captured 480 lines
    g_blankHmi = vp[2] < 1280 && !keepHmi;
    signal(SIGSEGV, onFatal); signal(SIGBUS, onFatal); signal(SIGABRT, onFatal); signal(SIGFPE, onFatal); signal(SIGILL, onFatal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);

    static nbt::most::Client most;
    // the client needs one consumer to start: a tap that only counts the VideoApp replies to us
    if (!most.setTap(&onReply, NULL) || !most.setInfoTap(&onInfo, NULL)) return 3;
    if (!most.start("NBTPROJ_HWVIDEO")) return 3;
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) { printf("[hw] MOST link not up\n"); return 3; }
    for (int w = 0; w < 30 && (!g_addrD7 || !g_addrD8) && !g_stop; ++w) usleep(100000);
    const uint16_t aD7 = (uint16_t)(g_addrD7 ? g_addrD7 : 0x2008), aD8 = (uint16_t)(g_addrD8 ? g_addrD8 : 0x2007);
    printf("[hw] VideoConnectionMaster D7 -> 0x%04X, VideoSink D8 -> 0x%04X%s\n", aD7, aD8,
           g_addrD7 && g_addrD8 ? "" : " (registry incomplete: old defaults)");
    if (registryOnly) { fflush(stdout); _exit(g_addrD7 && g_addrD8 ? 0 : 4); }
    if (!walInit()) printf("[hw] WAL control unavailable: picture may stay behind iDrive\n");

    g_myDev = most.deviceId();
    unsigned h = 0x0101 + (unsigned)(getpid() & 0x3f) * 8;   // sender handles, a fresh pair for every attempt
    uint8_t b[8];
    // Start, robust (car 2026-11-07: after an unclean session end the picture never came back until a reboot):
    //   1. clear a connection a previous run may have left in VideoApp (StopVideo + RemoveVideoConnection, harmless
    //      when there is none), 2. build + start, 3. no ResultAck for both within 1.5 s -> try again (3 attempts)
    for (int attempt = 1; attempt <= 3 && !g_stop; ++attempt) {
        be16(b, h); most.send(req(aD8, 0xD8, 0xC02, 0x6, b, 2)); usleep(250000);
        be16(b, h); b[2] = 0x16; b[3] = 0x01; most.send(req(aD7, 0xD7, 0x061, 0x6, b, 4)); usleep(400000);
        g_ack060 = g_ackC01 = 0; ++h;
        be16(b, h); b[2] = 0x16; b[3] = 0x01; most.send(req(aD7, 0xD7, 0x060, 0x6, b, 4)); usleep(300000);
        b[0] = 0x16; b[1] = 0; most.send(req(aD8, 0xD8, 0xC05, 0x0, b, 2)); usleep(150000);
        be16(b, 0); be16(b + 2, 0); be16(b + 4, 800); be16(b + 6, 480); most.send(req(aD8, 0xD8, 0xC03, 0x0, b, 8)); usleep(150000);
        be16(b, h); most.send(req(aD8, 0xD8, 0xC01, 0x6, b, 2));
        for (int w = 0; w < 15 && !(g_ack060 && g_ackC01) && !g_stop; ++w) usleep(100000);
        be16(b, (uint16_t)vp[0]); be16(b + 2, (uint16_t)vp[1]); be16(b + 4, (uint16_t)vp[2]); be16(b + 6, (uint16_t)vp[3]); most.send(req(aD8, 0xD8, 0xC04, 0x0, b, 8));
        printf("[hw] start attempt %d: build %s, StartVideo %s\n", attempt, g_ack060 ? "ok" : "NO ACK", g_ackC01 ? "ok" : "NO ACK");
        if (g_ack060 && g_ackC01) break;
        ++h;
        usleep(2000000);
    }
    printf("[hw] video connection requested (rows %d -> DisplayManager 800x%d, viewport %d,%d,%d,%d)\n", rows, height,
           vp[0], vp[1], vp[2], vp[3]);
    // apply display settings repeatedly during the first 12 s (the Cinemo window and the WAL video window appear only
    // when the decoder renders and the capture runs), then whenever the Android Auto page appears or disappears
    const long long t0 = nowMs();
    int shown = -1;
    long long nextApply = t0 + 1000, hiddenSince = 0, nextSrcCheck = t0 + 8000;
    while (!g_stop) {
        const long long t = nowMs();
        // hide only when the page has been away for 400 ms (BACK / side panels are put right back by the plugin)
        bool visible = access("/fs/ram3/nbt-aa-hidden", F_OK) != 0;
        if (visible) hiddenSince = 0;
        else if (!hiddenSince) hiddenSince = t;
        if (!visible && shown == 1 && t - hiddenSince < 400) visible = true;
        if (visible && t >= nextSrcCheck) {
            nextSrcCheck = t + 5000;
            if (g_curSrc >= 0 && g_curSrc != 0x16) {
                printf("[hw] VideoSink source is 0x%02X, not ours: starting the video connection again\n", g_curSrc);
                g_curSrc = -1;
                be16(b, ++h); b[2] = 0x16; b[3] = 0x01; most.send(req(aD7, 0xD7, 0x060, 0x6, b, 4)); usleep(300000);
                b[0] = 0x16; b[1] = 0; most.send(req(aD8, 0xD8, 0xC05, 0x0, b, 2)); usleep(150000);
                be16(b, h); most.send(req(aD8, 0xD8, 0xC01, 0x6, b, 2)); usleep(500000);
                be16(b, (uint16_t)vp[0]); be16(b + 2, (uint16_t)vp[1]); be16(b + 4, (uint16_t)vp[2]); be16(b + 6, (uint16_t)vp[3]); most.send(req(aD8, 0xD8, 0xC04, 0x0, b, 8));
                shown = -1;                                  // re-apply DisplayManager and WAL settings
            }
            most.send(req(aD8, 0xD8, 0xC05, 0x1, b, 0));     // Get CurrentVideoSource (answer -> g_curSrc)
        }
        if ((int)visible != shown || (t - t0 < 12000 && t >= nextApply)) {
            if ((int)visible != shown) printf("[hw] %s\n", visible ? "show (Android Auto page)" : "hide (iDrive page)");
            dmShow(visible, height);
            walShow(visible);
            shown = visible;
            nextApply = t + 2000;
        }
        usleep(100000);
    }
    // stop sequence
    walShow(false);
    dmShow(false, height);
    be16(b, h + 1); most.send(req(aD8, 0xD8, 0xC02, 0x6, b, 2)); usleep(300000);
    be16(b, h + 1); b[2] = 0x16; b[3] = 0x01; most.send(req(aD7, 0xD7, 0x061, 0x6, b, 4)); usleep(300000);
    b[0] = 0; b[1] = 0; most.send(req(aD8, 0xD8, 0xC05, 0x0, b, 2)); usleep(300000);
    printf("[hw] video connection removed\n");
    _exit(0);
}
