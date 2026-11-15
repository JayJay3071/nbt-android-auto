// nbt_audioconn - an `audioconn`-compatible route tool for the ORIGINAL BMW HU_NBT, which has no audioconn.
//
// NBT-AAD's BrowserAudio (oneplus_native, browser audio mode) runs `audioconn -b 0x<conn>` when Android Auto starts
// playing and `audioconn -r 0x<conn>` after silence; exit status 0 = granted. On NBT Evo audioconn is a stock program.
// Here the same request is sent the way HmiMain3D itself sends it (offline analysis 2026-11-15, docs/AA-AUDIO-PLAN.md):
//
//   ConnectionMaster FBlock 0x03 (address and instance from the DSYSMOSTServer registry)
//     Fkt 0x053 BuildHighSyncConnection   OpType 0x6 StartResultAck   payload big-endian { u16 SenderHandle, u16 ConnID }
//     Fkt 0x054 RemoveHighSyncConnection  OpType 0x6 StartResultAck   same payload
//     reply: same Fkt, OpType 0xD ResultAck { u16 SenderHandle } = granted; OpType 0x9 ErrorAck / 0xF Error = refused
//     Fkt 0xC03 HighSyncConnectionTable   OpType 0x1 Get (read-only, used by -l)
//   HmiMain3D: proxy ctor 0x8b838fc, request builders 0x8b832e6 (053) / 0x8b83258 (054), table EAudioSource -> ConnID at
//   0x9879224 (src 0x53 -> 0xB8 front, 0x54 -> 0xB9, 0x55 -> 0xBA), getConnIDForAudioSource 0x899c068.
//
// Connection 0xB8 is the browser audio connection (NBT Evo: BrowserFront, A2I input_stereo_3). It is an interrupting
// source: while it is built the radio / Bluetooth source is paused. native_service therefore removes it after every
// session even when oneplus_native died without doing so (route state file, see below).
//
// usage: nbt_audioconn -b 0x<conn>      build    exit 0 granted, 1 refused, 3 no answer, 4 no ConnectionMaster / link
//        nbt_audioconn -r 0x<conn>      remove   same exit codes
//        nbt_audioconn -l               print the ConnectionMaster's connection table (Get 0xC03, read-only)
// Only the browser connections 0xB8, 0xB9 and 0xBA are accepted (exit 2 otherwise): this tool cannot be used to switch
// arbitrary car audio connections. NBT_AUDIOCONN_ANY=1 lifts that limit for investigation.
//
// State files in the runtime directory (RAM, gone after a reboot):
//   audio-route.built    a build was granted and no remove has been granted since (native_service cleans up with it)
//   audio-route.failed   the last build was refused or not answered: native_service keeps Bluetooth audio for the next
//                        sessions until the next reboot (or until the file is removed)
// Needs no LD_LIBRARY_PATH: the DSI library is opened by absolute path (ref/dsi_client.cpp).
#include "most/client.hpp"
#include "most/connmaster.hpp"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

namespace cm = nbt::most::connmaster;
const char* const kRuntimeDir = "/fs/ram3/nbt-aa-native";
const uint8_t kFBlockConnMaster = cm::kFBlock;
const uint16_t kFktBuild = cm::kFktBuild, kFktRemove = cm::kFktRemove, kFktTable = cm::kFktTable;

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---- registry: where the ConnectionMaster lives ------------------------------------------------------------------
volatile unsigned g_cmAddr = 0, g_cmInst = 0, g_regSeen = 0;
void onInfo(uint32_t id, const uint8_t* p, uint32_t len, void*) {
    if (id != nbt::most::kAttrRegistry || len < 4) return;
    uint32_t n = nbt::most::rdLe32(p);
    unsigned addr = 0, inst = 0;
    for (uint32_t i = 0; i < n && 4 + i * 4 + 4 <= len; ++i) {
        unsigned a = nbt::most::rdLe16(p + 4 + i * 4), key = nbt::most::rdLe16(p + 6 + i * 4);
        if ((key >> 8) == kFBlockConnMaster && !addr) { addr = a; inst = key & 0xff; }
    }
    g_cmAddr = addr;
    g_cmInst = inst;
    g_regSeen = 1;
}

// ---- replies from FBlock 03 to us --------------------------------------------------------------------------------
volatile unsigned g_myDev = 0;
volatile int g_wantFkt = -1, g_wantHandle = -1;
volatile int g_result = 0;          // 0 waiting, 1 ResultAck, -1 error
uint8_t g_errBytes[8];
volatile unsigned g_errLen = 0;
bool g_list = false;

void onMsg(const nbt::most::View& m, nbt::most::MsgClass, void*) {
    if (m.fblock != kFBlockConnMaster) return;
    if (g_list && m.fkt == kFktTable && m.op == 0xC) {
        printf("table status (%u bytes):", (unsigned)m.len);
        for (size_t i = 0; i < m.len && i < 200; ++i) printf("%s%02x", i % 5 ? " " : "  |", m.data[i]);
        printf("\n");
        if (!g_myDev || m.dst == g_myDev) g_result = 1;
        return;
    }
    if (g_myDev && m.dst != g_myDev) return;
    if ((int)m.fkt != g_wantFkt) return;
    if (m.op == cm::kOpResultAck) {
        int h = cm::resultAckHandle(m.data, m.len);
        if (g_wantHandle >= 0 && h >= 0 && h != g_wantHandle) {
            printf("ResultAck for another handle 0x%04x (ours 0x%04x): ignored\n", (unsigned)h, (unsigned)g_wantHandle);
            return;
        }
        g_result = 1;
    } else if (m.op == cm::kOpErrorAck || m.op == cm::kOpError) {
        g_errLen = m.len < sizeof(g_errBytes) ? (unsigned)m.len : (unsigned)sizeof(g_errBytes);
        memcpy(g_errBytes, m.data, g_errLen);
        g_result = -1;
    } else {
        printf("reply fkt=%03X op=%X len=%u (not final)\n", m.fkt, m.op, (unsigned)m.len);
    }
}

void stateFile(const char* name, bool present, const char* text) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", kRuntimeDir, name);
    if (!present) { unlink(path); return; }
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", text);
    fclose(f);
}

int usage(const char* me) {
    fprintf(stderr, "usage: %s -b 0x<conn> | -r 0x<conn> | -l\n", me);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char op = 0;
    unsigned long conn = 0;
    if (argc == 2 && !strcmp(argv[1], "-l")) op = 'l';
    else if (argc == 3 && (!strcmp(argv[1], "-b") || !strcmp(argv[1], "-r"))) {
        char* end = 0;
        op = argv[1][1];
        conn = strtoul(argv[2], &end, 0);
        if (end == argv[2] || *end || conn == 0 || conn > 0xffff) return usage(argv[0]);
    } else return usage(argv[0]);
    const char* any = getenv("NBT_AUDIOCONN_ANY");
    if (op != 'l' && !(any && !strcmp(any, "1")) && !cm::isBrowserConn(conn)) {
        fprintf(stderr, "connection 0x%lx refused: only the browser connections 0xb8..0xba are allowed\n", conn);
        return 2;
    }
    g_list = op == 'l';
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);

    static nbt::most::Client most;
    if (!most.setTap(&onMsg, NULL) || !most.setInfoTap(&onInfo, NULL)) return 4;
    if (!most.start("NBTPROJ_AUDIOCONN")) return 4;
    for (int w = 0; w < 50 && most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (most.state() != nbt::most::kUp) {
        printf("MOST link not up: nothing sent\n");
        if (op == 'b') stateFile("audio-route.failed", true, "MOST link not up");
        fflush(stdout); _exit(4);
    }
    for (int w = 0; w < 30 && !g_regSeen && !g_stop; ++w) usleep(100000);
    if (!g_cmAddr) {
        printf("ConnectionMaster FBlock 03 not in the MOST registry%s: nothing sent\n", g_regSeen ? "" : " (no registry)");
        if (op == 'b') stateFile("audio-route.failed", true, "FBlock 03 not in registry");
        fflush(stdout); _exit(4);
    }
    g_myDev = most.deviceId();
    printf("ConnectionMaster 03/%02X at 0x%04X, own device 0x%04X\n", g_cmInst, g_cmAddr, g_myDev);

    if (op == 'l') {
        g_wantFkt = kFktTable;
        most.send(cm::tableGet((uint16_t)g_cmAddr, (uint8_t)g_cmInst));
        for (long long t = nowMs() + 2000; nowMs() < t && !g_result && !g_stop;) usleep(50000);
        fflush(stdout);
        _exit(g_result ? 0 : 3);
    }

    const uint16_t fkt = op == 'b' ? kFktBuild : kFktRemove;
    const unsigned handle = 0x4E00u | (unsigned)(getpid() & 0xff);   // own range, away from the HMI's 0xB1xx / counters
    g_wantFkt = fkt;
    g_wantHandle = (int)handle;
    printf("%s connection 0x%lx (fkt %03X, handle 0x%04X)\n", op == 'b' ? "build" : "remove", conn, fkt, handle);
    if (!most.send(cm::connectionRequest(op == 'b', (uint16_t)g_cmAddr, (uint8_t)g_cmInst, (uint16_t)handle,
                                         (uint16_t)conn))) {
        if (op == 'b') stateFile("audio-route.failed", true, "send failed");
        fflush(stdout); _exit(4);
    }
    for (long long t = nowMs() + 3000; nowMs() < t && !g_result && !g_stop;) usleep(50000);

    char why[64];
    int rc;
    if (g_result == 1) {
        printf("granted\n");
        rc = 0;
    } else if (g_result == -1) {
        int n = snprintf(why, sizeof(why), "refused:");
        for (unsigned i = 0; i < g_errLen && n < (int)sizeof(why) - 4; ++i) n += snprintf(why + n, sizeof(why) - n, " %02x", g_errBytes[i]);
        printf("%s\n", why);
        rc = 1;
    } else {
        snprintf(why, sizeof(why), "no answer within 3 s");
        printf("%s\n", why);
        rc = 3;
    }
    if (op == 'b') {
        if (rc == 0) { stateFile("audio-route.built", true, argv[2]); stateFile("audio-route.failed", false, 0); }
        else stateFile("audio-route.failed", true, why);
    } else if (rc == 0) {
        stateFile("audio-route.built", false, 0);
    }
    fflush(stdout);
    _exit(rc);
}
