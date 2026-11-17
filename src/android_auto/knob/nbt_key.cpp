// nbt_key - TEST TOOL: send an iDrive controller key to the HMI as if the controller had sent it (2026-11-12).
//
// Why: in the Android Auto split-screen mode a tilt right moves the iDrive focus into the split screen, and the plugin's
// focus event does not bring it back quickly. A BACK pressed right after fixes it (user, car 2026-11-12). nbt_knob only
// observes, so iDrive cannot be kept from seeing the tilt; the idea is to answer it with a BACK.
//
// This car's controller speaks the older "CCS" protocol (car 2026-11-12, nbt_key --watch), not C46/C47:
//   E5/01 fkt C20 PushButton, op Status, src 3002 -> dst 2025 (the HMI's MOST client), 7 bytes, one event per message:
//     [0] sender 01 [1] receiver 1D [2] message counter (+1 per C20 message) [3] 1 pressed / 0 released
//     [4] tilt (2 = right) [5] cluster: 00 hard keys / 1D tilt [6] sensor: hard key bit (02 = BACK) / 01 for tilts
//   seen: BACK  01 1d 67 01 00 00 02 / 01 1d 68 00 00 00 02     tilt right  01 1d 69 01 02 1d 01 / 01 1d 6a 00 00 1d 01
// The first try (C47 to our own device id 0x2065, sender filled with ours) did nothing.
// Now the message is a copy of the controller's: same src (3002) and dst (2025) as the controller's last C20, its
// counter continued. Open: does the MOST server deliver a message to a local client (2025) when it comes from another
// local client, and does the HMI accept a faked sender.
//
//   nbt_key --watch [N]     print every E5 message for N s (default 15)
//   nbt_key back [N]        wait up to N s (default 20) for a tilt right; when it is released, send BACK (pressed,
//                           100 ms later released) with the controller's addresses and the next counters.
//   nbt_key source [N]      2026-11-13 TEST: ask the audio source master for the current source and print every
//                           FBlock 03 message for N s (default 5). Source status as seen on the car: 100E -> HMI,
//                           03/00 fkt C03 op Status, 5 bytes, byte 3 = source (BB AUX, C7 Bluetooth, 01 radio). Sends
//                           (a) Get 03/00 C03 and (b) a notification registration 03/00 fkt 001 {01, our id, 0C 03},
//                           both to 100E from our own device id, so the answer comes back to us. Reads only.
//   nbt_key cm <fkt> <op> [hex bytes...]   2026-11-17 TEST: send one ConnectionMaster message (03/00) to 100E from our
//                           own device id and print every FBlock 03 answer for 2 s. MOST error answers (op F) are
//                           decoded (05 = wrong length, 06 = parameter n wrong, ...). For finding the source switch
//                           command: the HMI sends 03/00 fkt 053 op 6 (StartResultAck) to 100E on every switch.
// Needs LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib (DSI library).
#include "most/client.hpp"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
long long nowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
long long g_t0 = 0;

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
bool g_seen = false;            // a C20 from the controller was seen
uint16_t g_src = 0, g_dst = 0;  // its addresses
uint8_t g_counter = 0;          // its last counter
bool g_tiltRightDown = false;
volatile bool g_trigger = false;   // tilt right released: send BACK now

void onE5(const nbt::most::View& m, void*) {
    printf("%6lld ms  E5/%02X fkt=%03X op=%X src=%04X dst=%04X len=%u:", nowMs() - g_t0, m.inst, m.fkt, m.op, m.src, m.dst,
           (unsigned)m.len);
    for (size_t i = 0; i < m.len && i < 16; ++i) printf(" %02x", m.data[i]);
    printf("\n");
    if (m.fkt != 0xC20 || m.inst != 0x01 || m.op != 0xC || m.len < 7) return;
    pthread_mutex_lock(&g_mx);
    if (m.data[2] != 0xFF) { g_seen = true; g_src = m.src; g_dst = m.dst; g_counter = m.data[2]; }
    if (m.data[5] == 0x1D && m.data[3] == 1 && m.data[4] == 2) g_tiltRightDown = true;
    else if (m.data[5] == 0x1D && m.data[3] == 0 && g_tiltRightDown) { g_tiltRightDown = false; g_trigger = true; }
    pthread_mutex_unlock(&g_mx);
}

void onFb03(const nbt::most::View& m, void*) {
    printf("%6lld ms  03/%02X fkt=%03X op=%X src=%04X dst=%04X len=%u:", nowMs() - g_t0, m.inst, m.fkt, m.op, m.src, m.dst,
           (unsigned)m.len);
    for (size_t i = 0; i < m.len && i < 16; ++i) printf(" %02x", m.data[i]);
    if (m.fkt == 0xC03 && m.len >= 5) printf("   <- source %02X%s", m.data[3], m.data[3] == 0xBB ? " (AUX)" : "");
    if (m.op == 0xF && m.len >= 1) {
        static const char* const names[] = { "?", "FBlock not available", "instance not available", "function not available",
                                             "op type not available", "invalid length", "parameter wrong/out of range",
                                             "parameter not available" };
        printf("   <- ERROR %02X %s", m.data[0], m.data[0] < 8 ? names[m.data[0]] : "");
        if ((m.data[0] == 6 || m.data[0] == 7) && m.len >= 2) printf(" (parameter %u)", m.data[1]);
    }
    printf("\n");
}

nbt::most::Msg fb03Msg(uint16_t src, uint16_t fkt, uint8_t op, const uint8_t* d, size_t n) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;
    m.dst = 0x100E; m.src = src; m.fblock = 0x03; m.inst = 0x00; m.fkt = fkt; m.opType = op; m.hdrTag = 0xFFFFu;
    m.data.assign(d, d + n);
    return m;
}

uint8_t nextCounter(uint8_t c) { c = (uint8_t)(c + 1); return c == 0xFF ? 0 : c; }   // 0xFF = "ignore" (firmware)

nbt::most::Msg backMsg(uint16_t src, uint16_t dst, uint8_t counter, bool down) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = src; m.fblock = 0xE5; m.inst = 0x01; m.fkt = 0xC20; m.opType = 0xC; m.hdrTag = 0xFFFFu;
    const uint8_t d[7] = { 0x01, 0x1D, counter, (uint8_t)(down ? 1 : 0), 0x00, 0x00, 0x02 };
    m.data.assign(d, d + 7);
    return m;
}

int usage() {
    printf("usage: nbt_key --watch [seconds] | nbt_key back [seconds] | nbt_key source [seconds] |"
           " nbt_key cm <fkt hex> <op hex> [data hex bytes...]\n");
    return 1;
}
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return usage();
    const bool cm = !strcmp(argv[1], "cm");
    const bool watch = !strcmp(argv[1], "--watch"), back = !strcmp(argv[1], "back"), source = !strcmp(argv[1], "source");
    if (!watch && !back && !source && !cm) return usage();
    if (cm ? argc < 4 || argc > 4 + 32 : argc > 3) return usage();
    const int seconds = cm ? 2 : argc == 3 ? atoi(argv[2]) : (watch ? 15 : source ? 5 : 20);

    static nbt::most::Client most;
    // the same device controller keys nbt_knob registers, so the E5 messages are delivered to us
    static const uint16_t keys[] = { 0xE510, 0xE503, 0xE505, 0xE512, 0xE514, 0xE501, 0xE502 };
    if (source || cm) {
        if (!most.addRoute(0x03, nbt::most::kAny, nbt::most::kAny, nbt::most::kAny, &onFb03, NULL)) return 3;
    } else if (!most.addRoute(0xE5, nbt::most::kAny, nbt::most::kAny, nbt::most::kAny, &onE5, NULL)) return 3;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) most.addDevCtrl(keys[i]);
    g_t0 = nowMs();
    if (!most.start("NBTPROJ_KEY")) { printf("DSI start failed\n"); return 3; }
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) { printf("MOST link not up\n"); return 3; }
    printf("our device id %04X%s\n", most.deviceId(), back ? "; tilt the controller right now" : "");

    if (source) {
        const uint16_t me = (uint16_t)most.deviceId();
        printf("Get 03/00 C03 -> 100E: %s\n", most.send(fb03Msg(me, 0xC03, 0x1, NULL, 0)) ? "sent" : "FAILED");
        usleep(500000);
        const uint8_t reg[5] = { 0x01, (uint8_t)(me >> 8), (uint8_t)me, 0x0C, 0x03 };
        printf("notification 03/00 001 {01 %04X 0C03} -> 100E: %s\n", me,
               most.send(fb03Msg(me, 0x001, 0x0, reg, 5)) ? "sent" : "FAILED");
    }
    if (cm) {
        const unsigned fkt = (unsigned)strtoul(argv[2], 0, 16) & 0xFFF, op = (unsigned)strtoul(argv[3], 0, 16) & 0xF;
        uint8_t d[32];
        size_t n = 0;
        for (int i = 4; i < argc; ++i) d[n++] = (uint8_t)strtoul(argv[i], 0, 16);
        printf("03/00 fkt=%03X op=%X len=%u -> 100E:", fkt, op, (unsigned)n);
        for (size_t i = 0; i < n; ++i) printf(" %02x", d[i]);
        printf("  %s\n", most.send(fb03Msg((uint16_t)most.deviceId(), (uint16_t)fkt, (uint8_t)op, d, n)) ? "sent" : "FAILED");
    }
    const long long end = nowMs() + seconds * 1000LL;
    while (!g_stop && nowMs() < end) {
        if (back && g_trigger) {
            pthread_mutex_lock(&g_mx);
            g_trigger = false;
            const uint16_t src = g_src, dst = g_dst;
            const uint8_t c1 = nextCounter(g_counter), c2 = nextCounter(c1);
            g_counter = c2;
            pthread_mutex_unlock(&g_mx);
            printf("tilt right released: sending BACK %04X -> %04X, counters %02x %02x\n", src, dst, c1, c2);
            const bool ok1 = most.send(backMsg(src, dst, c1, true));
            usleep(100000);
            const bool ok2 = most.send(backMsg(src, dst, c2, false));
            printf("send %s / %s\n", ok1 ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
            usleep(1500000);
            break;
        }
        usleep(20000);
    }
    if (back && !g_seen) printf("no controller C20 message seen\n");
    fflush(stdout);
    _exit(0);   // as the other MOST tools: no DSI teardown races
}
