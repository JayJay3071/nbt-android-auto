// Host test for most/connmaster.hpp: the exact MOST frames nbt_audioconn sends to the HU_NBT ConnectionMaster.
// Build and run on any host (no QNX, no car):
//   cd src/android_auto/knob && c++ -std=c++11 -Iref tests/test_connmaster.cpp -o test_connmaster && ./test_connmaster
// Expected values come from the HmiMain3D analysis (docs/AA-AUDIO-PLAN.md): FBlock 03, Fkt 053/054, OpType 6,
// big-endian payload { u16 handle, u16 connId }.
#include "most/connmaster.hpp"
#include <stdio.h>
#include <string.h>

namespace cm = nbt::most::connmaster;
using nbt::most::Msg;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool same(const std::vector<uint8_t>& v, const uint8_t* e, size_t n) {
    if (v.size() != n) { printf("  size %u != %u\n", (unsigned)v.size(), (unsigned)n); }
    bool ok = v.size() == n && !memcmp(&v[0], e, n);
    if (!ok) {
        printf("  got     :"); for (size_t i = 0; i < v.size(); ++i) printf(" %02x", v[i]); printf("\n");
        printf("  expected:"); for (size_t i = 0; i < n; ++i) printf(" %02x", e[i]); printf("\n");
    }
    return ok;
}

int main() {
    // build 0xb8 to ConnectionMaster at 0x100E inst 1, handle 0x4E2A
    Msg b = cm::connectionRequest(true, 0x100E, 0x01, 0x4E2A, cm::kConnBrowserFront);
    CHECK(b.fblock == 0x03 && b.inst == 0x01 && b.fkt == 0x053 && b.opType == 0x6 && b.dst == 0x100E);
    const uint8_t buildPayload[] = {0x4E, 0x2A, 0x00, 0xB8};
    CHECK(same(b.data, buildPayload, sizeof(buildPayload)));

    Msg r = cm::connectionRequest(false, 0x100E, 0x01, 0x4E2B, 0xB8);
    CHECK(r.fkt == 0x054 && r.opType == 0x6);
    const uint8_t removePayload[] = {0x4E, 0x2B, 0x00, 0xB8};
    CHECK(same(r.data, removePayload, sizeof(removePayload)));

    // the whole DSYSMOSTServer MessageType frame (little-endian header, msg.hpp layout) with src/tag as send() fills them
    b.src = 0x0123; b.hdrTag = 7;
    std::vector<uint8_t> f = nbt::most::buildMessage(b);
    const uint8_t frame[] = {
        0x00, 0x00, 0x00, 0x00,   // txType 0
        0x01, 0x00, 0x00, 0x00,   // varTag 1 (TXControl)
        0x00, 0x00, 0x00, 0x00,   // varVal 0
        0x0E, 0x10,               // dst 0x100E
        0x23, 0x01,               // src 0x0123
        0x03, 0x01,               // FBlock 03, inst 1
        0x53, 0x00,               // fkt 0x053
        0x06, 0x00, 0x00, 0x00,   // opType 6 + pad
        0x07, 0x00, 0x00, 0x00,   // hdrTag
        0x04, 0x00, 0x00, 0x00,   // payload length 4
        0x4E, 0x2A, 0x00, 0xB8    // payload, big-endian
    };
    CHECK(same(f, frame, sizeof(frame)));

    // round trip through the receive parser
    nbt::most::View v = nbt::most::View();
    CHECK(nbt::most::parseView(&f[0], f.size(), v));
    CHECK(v.fblock == 0x03 && v.fkt == 0x053 && v.op == 0x6 && v.len == 4 && v.data[3] == 0xB8);

    Msg t = cm::tableGet(0x100E, 1);
    CHECK(t.fkt == 0xC03 && t.opType == 0x1 && t.data.empty());

    const uint8_t ack[] = {0x4E, 0x2A};
    CHECK(cm::resultAckHandle(ack, 2) == 0x4E2A);
    CHECK(cm::resultAckHandle(ack, 1) == -1);

    CHECK(cm::isBrowserConn(0xB8) && cm::isBrowserConn(0xBA));
    CHECK(!cm::isBrowserConn(0xC7) && !cm::isBrowserConn(0x36) && !cm::isBrowserConn(0));

    printf(failures ? "test_connmaster: %d FAILED\n" : "test_connmaster: all passed%.0d\n", failures);
    return failures ? 1 : 0;
}
