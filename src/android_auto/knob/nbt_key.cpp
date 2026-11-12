// nbt_key - TEST TOOL: send an iDrive controller key to the HMI as if the controller had sent it (2026-11-12).
//
// Why: in the Android Auto split-screen mode a tilt right moves the iDrive focus into the split screen, and the plugin's
// focus event does not bring it back quickly. A BACK pressed right after fixes it (user, car 2026-11-12). nbt_knob only
// observes, so iDrive cannot be kept from seeing the tilt; the idea is to answer it with a BACK.
//
// How (NOT car-verified yet): the controller (FBlock 0xE5, instance 03) reports its hard keys as a full state,
// fkt 0xC47 PushButtonsState, op Status (0xC), 7 bytes, one per key: MENU BACK OPTION COM MEDIA NAV MAP (1 = pressed;
// layout from ref/car/ccs_decoder.hpp, car-verified for reading). This tool sends that message twice through the
// DSYSMOSTServer (BACK pressed, 120 ms later all released). Open questions this tool answers:
//   - does the MOST server deliver a message we send to our own head unit to its local receivers (the HMI)?
//     --watch shows every E5 message with src/dst; our own sent message showing up there means it does
//   - does the HMI accept a controller status that does not come from the controller's address?
//
//   nbt_key --watch [N]          print every E5 message for N s (default 15): learn the controller's src/dst
//   nbt_key back [--dst HEX] [--src HEX] [--c4f]
//                                send BACK; dst default = our own device id (the head unit); --src fakes the sender
//                                (default 0 = filled with our id by the client); --c4f also sends 0xC4F (8 bytes, [7]=7)
// Needs LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib (DSI library).
#include "most/client.hpp"
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

void onE5(const nbt::most::View& m, void*) {
    printf("%6lld ms  E5/%02X fkt=%03X op=%X src=%04X dst=%04X len=%u:", nowMs() - g_t0, m.inst, m.fkt, m.op, m.src, m.dst,
           (unsigned)m.len);
    for (size_t i = 0; i < m.len && i < 16; ++i) printf(" %02x", m.data[i]);
    printf("\n");
}

nbt::most::Msg keyState(uint16_t dst, uint16_t src, uint16_t fkt, bool back) {
    nbt::most::Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;      // as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = src; m.fblock = 0xE5; m.inst = 0x03; m.fkt = fkt; m.opType = 0xC; m.hdrTag = 0xFFFFu;
    m.data.assign(fkt == 0xC4F ? 8 : 7, 0);
    if (back) m.data[1] = 1;                       // slot 1 = BACK
    if (fkt == 0xC4F) m.data[7] = 7;
    return m;
}

int usage() {
    printf("usage: nbt_key --watch [seconds] | nbt_key back [--dst HEX] [--src HEX] [--c4f]\n");
    return 1;
}
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return usage();
    bool watch = !strcmp(argv[1], "--watch"), back = !strcmp(argv[1], "back"), c4f = false;
    int seconds = 15;
    unsigned dst = 0, src = 0;
    if (!watch && !back) return usage();
    for (int i = 2; i < argc; ++i) {
        if (watch && i == 2 && argv[i][0] != '-') seconds = atoi(argv[i]);
        else if (!strcmp(argv[i], "--dst") && i + 1 < argc) dst = (unsigned)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--src") && i + 1 < argc) src = (unsigned)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--c4f")) c4f = true;
        else return usage();
    }

    static nbt::most::Client most;
    // the same device controller keys nbt_knob registers, so the E5 messages are delivered to us
    static const uint16_t keys[] = { 0xE510, 0xE503, 0xE505, 0xE512, 0xE514, 0xE501, 0xE502 };
    if (!most.addRoute(0xE5, nbt::most::kAny, nbt::most::kAny, nbt::most::kAny, &onE5, NULL)) return 3;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) most.addDevCtrl(keys[i]);
    g_t0 = nowMs();
    if (!most.start("NBTPROJ_KEY")) { printf("DSI start failed\n"); return 3; }
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) { printf("MOST link not up\n"); return 3; }
    printf("our device id %04X\n", most.deviceId());

    if (back) {
        const uint16_t d = (uint16_t)(dst ? dst : most.deviceId());
        printf("sending BACK as E5/03 fkt C47%s to %04X, src %04X\n", c4f ? " + C4F" : "", d, src);
        bool ok = most.send(keyState(d, (uint16_t)src, 0xC47, true));
        if (c4f) ok = most.send(keyState(d, (uint16_t)src, 0xC4F, true)) && ok;
        usleep(120000);
        ok = most.send(keyState(d, (uint16_t)src, 0xC47, false)) && ok;
        if (c4f) ok = most.send(keyState(d, (uint16_t)src, 0xC4F, false)) && ok;
        printf("send %s; E5 messages seen in the next 2 s:\n", ok ? "ok" : "FAILED");
        seconds = 2;
    }
    const long long end = nowMs() + seconds * 1000LL;
    while (!g_stop && nowMs() < end) usleep(100000);
    fflush(stdout);
    _exit(0);   // as the other MOST tools: no DSI teardown races
}
