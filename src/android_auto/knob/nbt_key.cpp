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
    printf("usage: nbt_key --watch [seconds] | nbt_key back [seconds]\n");
    return 1;
}
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);
    if (argc < 2 || argc > 3) return usage();
    const bool watch = !strcmp(argv[1], "--watch"), back = !strcmp(argv[1], "back");
    if (!watch && !back) return usage();
    const int seconds = argc == 3 ? atoi(argv[2]) : (watch ? 15 : 20);

    static nbt::most::Client most;
    // the same device controller keys nbt_knob registers, so the E5 messages are delivered to us
    static const uint16_t keys[] = { 0xE510, 0xE503, 0xE505, 0xE512, 0xE514, 0xE501, 0xE502 };
    if (!most.addRoute(0xE5, nbt::most::kAny, nbt::most::kAny, nbt::most::kAny, &onE5, NULL)) return 3;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) most.addDevCtrl(keys[i]);
    g_t0 = nowMs();
    if (!most.start("NBTPROJ_KEY")) { printf("DSI start failed\n"); return 3; }
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) { printf("MOST link not up\n"); return 3; }
    printf("our device id %04X%s\n", most.deviceId(), back ? "; tilt the controller right now" : "");

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
