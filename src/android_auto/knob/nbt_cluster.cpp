// nbt_cluster - Android Auto turn-by-turn in the 6WB instrument cluster (version 2, 2026-11-08).
//
// The cluster draws the navigation graphics itself: the NBT remote-controls a graphics terminal in it over MOST
// (device 0x0101, FBlock 0x82 "HUDTerminal", inst 00; see docs/CLUSTER-HUDTERMINAL.md). Car-verified commands
// (OpType 6 StartResultAck, tx 4, payload = instance u16 + parameters):
//   204 [inst][0|1]                  visibility (0x17 = whole guidance area)
//   210 [inst][01 ISO-8859-15 text 00]  text: 0x1A street (top line, right-aligned, ~15 chars), 0x1B distance (big,
//                                    ~5 chars), 0x1C unit (small, ~3 chars)
//   21D [inst][item][03 filled | 02 line][n][n x (x i16, y i16)]   polygon / polyline (0x1F: junction-view plane, in
//                                    perspective, about 233 x 206, visible from y ~70 down)
//   21B [inst][colour 0..19]         colour of the following items
//   220 [inst]                       remove all items
//   227 [0x35][i32 value][i8 2]      distance bar
// Display logic (user, 2026-11-08, revised): bottom = the CURRENT step (AA step 0) as the big arrow, its distance as the
// bar (full from 1 km on, empty at the maneuver; no number). Top line = the FOLLOWING step (AA step 1): its street in
// 0x1A (the phone sends no distance for it, so 0x1B/0x1C stay empty).
// The iDrive navigation has priority: a HUDTerminal result that matches none of our own recent commands means the NBT
// draws a route itself; then nothing is sent until 20 s after its last activity.
// Reads /fs/ram3/nbt-aa-native/nav.state (written by the AA session from the phone's navigation status).
// usage: nbt_cluster [--seconds N]      Needs LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib (DSI library).
#include "most/client.hpp"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {
volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
long long nowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

const uint16_t kKombi = 0x0101;
const uint8_t kFb = 0x82;
const unsigned kIconA = 0x18, kIconB = 0x19;         // RoadIcon bitmap areas, 305 x 44 (the top line)
const unsigned kLanes = 0x23, kLanesAlt = 0x24;     // flat strip at the bottom, 283 x 40: lane row
const unsigned kArea = 0x17, kStreet = 0x1a, kDist = 0x1b, kUnit = 0x1c, kDraw = 0x1f, kOther = 0x20, kBar = 0x35;
const int kNear = 1000;                              // metres: the maneuver arrow only from here on
const uint8_t kColour = 0x02;                        // white in the palette test
const uint8_t kLaneGrey = 0x0e;                      // grey in the palette test (lanes not to take)
const uint8_t kLaneLine = 0x03;                      // lane outlines
const uint8_t kLine = 0x06;                          // outline width (219): 0x16 was far too thick (car 2026-11-08)
const char* const kNav = "/fs/ram3/nbt-aa-native/nav.state";
const char* const kAaState = "/fs/ram3/nbt-aa-state";

nbt::most::Client g_most;

// ---- own commands vs. the iDrive navigation's ----------------------------------------------------------------------
struct Pending { uint16_t fkt; uint8_t inst0, inst1; long long t; bool used; };
Pending g_pend[128];
unsigned g_pendNext = 0;
pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
volatile long long g_bmwUntil = 0;
volatile unsigned g_bmwResults = 0;

bool g_probe = false;                                // --bmp: log every HUDTerminal answer
void put16(std::vector<uint8_t>& b, int v) { b.push_back((uint8_t)((v >> 8) & 0xff)); b.push_back((uint8_t)(v & 0xff)); }
void send(uint16_t fkt, const std::vector<uint8_t>& data, uint32_t tx = 4) {
    nbt::most::Msg m;
    m.txType = tx; m.varTag = 1; m.varVal = 0;      // HUDTerminal: transmission type 4, TXControl 0 (as the NBT sets it)
    m.dst = kKombi; m.src = 0; m.fblock = kFb; m.inst = 0; m.fkt = fkt; m.opType = 6; m.hdrTag = 0xFFFFu;
    m.data = data;
    pthread_mutex_lock(&g_mx);
    Pending& p = g_pend[g_pendNext++ % 128];
    p.fkt = fkt; p.inst0 = data.size() > 0 ? data[0] : 0; p.inst1 = data.size() > 1 ? data[1] : 0; p.t = nowMs(); p.used = false;
    pthread_mutex_unlock(&g_mx);
    g_most.send(m);
    usleep(15000);                                   // a few ms between commands, like the NBT's own bursts
}
// results of the cluster go to the NBT's own client; we see them on the tap. One that matches none of our commands of
// the last 8 s = the iDrive navigation draws.
// --bench: time from sending an ImageAddBitmap to its answer
volatile unsigned g_benchInst = 0; volatile int g_benchOp = -1; volatile long long g_benchT = 0;
void onMsg(const nbt::most::View& m, nbt::most::MsgClass cls, void*) {
    if (g_benchInst && m.fblock == kFb && m.fkt == 0xC05) {
        if (cls == nbt::most::kTxStatus) printf("[bench] transmit status %u\n", (unsigned)m.varVal);
        else if (m.src == kKombi && m.len >= 2 && (unsigned)((m.data[0] << 8) | m.data[1]) == g_benchInst && g_benchOp < 0) {
            g_benchT = nowMs(); g_benchOp = m.op;
        }
    }
    if (g_probe && m.fblock == kFb && m.src == kKombi && (m.fkt >= 0xC00 || m.fkt == 0x300 || m.op != 0xD)) {
        char hex[100] = ""; size_t o = 0;
        for (size_t i = 0; i < m.len && i < 24; ++i) o += snprintf(hex + o, sizeof(hex) - o, "%02X ", m.data[i]);
        printf("[bmp] answer fkt %03X op %X tx %u len %u: %s\n", m.fkt, m.op, m.txType, (unsigned)m.len, hex);
    }
    if (m.fblock != kFb || m.src != kKombi || m.op != 0xD) return;
    const long long t = nowMs();
    bool ours = false;
    pthread_mutex_lock(&g_mx);
    for (unsigned i = 0; i < 128 && !ours; ++i) {
        Pending& p = g_pend[i];
        if (!p.used && p.fkt == m.fkt && t - p.t < 8000 && (m.len < 2 || (m.data[0] == p.inst0 && m.data[1] == p.inst1))) {
            p.used = true; ours = true;
        }
    }
    pthread_mutex_unlock(&g_mx);
    if (!ours) {
        // the iDrive navigation draws in bursts (polygons 21D, buffer swaps 20E, cursor C06, bar 227): only several foreign
        // drawing results within 3 s count (car 2026-11-08: single stray results made v2 yield wrongly)
        static long long first = 0; static unsigned count = 0;
        const bool drawing = m.fkt == 0x21D || m.fkt == 0x20E || m.fkt == 0xC06 || m.fkt == 0x227 || m.fkt == 0xC09;
        if (t - first > 3000) { first = t; count = 0; }
        if (drawing) ++count;
        static long long lastLog = 0;
        if (t - lastLog > 1000) {
            lastLog = t;
            printf("[cluster] foreign result fkt %03X len %u: %02X %02X %02X %02X (count %u)\n", m.fkt, (unsigned)m.len,
                   m.len > 0 ? m.data[0] : 0, m.len > 1 ? m.data[1] : 0, m.len > 2 ? m.data[2] : 0, m.len > 3 ? m.data[3] : 0, count);
        }
        if (count >= 4) { g_bmwUntil = t + 20000; ++g_bmwResults; }
    }
}

void visible(unsigned inst, bool on) { std::vector<uint8_t> b; put16(b, inst); b.push_back(on ? 1 : 0); send(0x204, b); }
void clearInst(unsigned inst) { std::vector<uint8_t> b; put16(b, inst); send(0x220, b); }
void colour(unsigned inst, uint8_t c) { std::vector<uint8_t> b; put16(b, inst); b.push_back(c); send(0x21B, b); }
void lineWidth(unsigned inst, uint8_t w) { std::vector<uint8_t> b; put16(b, inst); b.push_back(w); send(0x219, b); }
void bar(int value) {
    std::vector<uint8_t> b; put16(b, kBar);
    b.push_back(0); b.push_back(0); b.push_back((uint8_t)(value >> 8)); b.push_back((uint8_t)value);   // i32 value
    b.push_back(2); send(0x227, b);
}
// UTF-8 -> ISO-8859-15 (Latin-1 range directly, the euro sign, everything else '?')
std::string toIso(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (char)c; ++i; continue; }
        unsigned cp = 0; size_t n = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; } else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; } else { ++i; continue; }
        if (i + n > s.size()) break;
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        if (cp >= 0xA0 && cp <= 0xFF) o += (char)cp; else if (cp == 0x20AC) o += (char)0xA4; else o += '?';
    }
    return o;
}
void text(unsigned inst, const std::string& iso) {
    std::vector<uint8_t> b; put16(b, inst); b.push_back(1);          // 01 = ISO-8859-15 (car-verified, umlauts ok)
    for (size_t i = 0; i < iso.size(); ++i) b.push_back((uint8_t)iso[i]);
    b.push_back(0); send(0x210, b);
}

// ---- arrow geometry: outline of a thick path with an arrow head (plane 0x1F, y grows downwards) ----------------------
struct Pt { int x, y; };
unsigned g_item = 0x80;
unsigned g_inst = kDraw;                             // instance the polygons go to
void polyline(const std::vector<Pt>& p, int type) {
    std::vector<uint8_t> b; put16(b, g_inst); put16(b, g_item++); b.push_back((uint8_t)type); b.push_back((uint8_t)p.size());
    for (size_t i = 0; i < p.size(); ++i) { put16(b, p[i].x); put16(b, p[i].y); }
    send(0x21D, b);
}
const double kPi = 3.14159265358979;
const double kHalf = 10;                             // half shaft width
struct V { double x, y; };
V norm(V a) { double l = sqrt(a.x * a.x + a.y * a.y); V r = { l > 0 ? a.x / l : 0, l > 0 ? a.y / l : 0 }; return r; }
Pt P(double x, double y) { Pt p; p.x = (int)floor(x + 0.5); p.y = (int)floor(y + 0.5); return p; }
// path: centre line from the car to the arrow tip base; the head (length 26, half width 24) continues the last segment
void arrowOutline(const std::vector<V>& path, double half = kHalf, double headLen = 28, double headHalf = 25, int type = 2) {
    const size_t n = path.size();
    if (n < 2) return;
    // offset lines on both sides; at a joint the inner side meets in the miter point, the outer side is bevelled when
    // the turn is sharp (car 2026-11-08: a 135 degree miter shot far out and drew a big triangle)
    std::vector<V> sideA, sideB;                         // A = +normal side, B = -normal side
    for (size_t i = 0; i < n; ++i) {
        V d1 = i > 0 ? norm(V{path[i].x - path[i - 1].x, path[i].y - path[i - 1].y}) : norm(V{path[1].x - path[0].x, path[1].y - path[0].y});
        V d2 = i + 1 < n ? norm(V{path[i + 1].x - path[i].x, path[i + 1].y - path[i].y}) : d1;
        V n1 = { -d1.y, d1.x }, n2 = { -d2.y, d2.x };
        const double dot = d1.x * d2.x + d1.y * d2.y, cross = d1.x * d2.y - d1.y * d2.x;
        V m = norm(V{n1.x + n2.x, n1.y + n2.y});
        double c = m.x * n1.x + m.y * n1.y; if (c < 0.2) c = 0.2;
        const double k = half / c;
        V mA = { path[i].x + m.x * k, path[i].y + m.y * k }, mB = { path[i].x - m.x * k, path[i].y - m.y * k };
        if (i == 0 || i + 1 == n || dot > 0.5) { sideA.push_back(mA); sideB.push_back(mB); continue; }
        // sharp joint: cross > 0 = the path bends towards +normal... the outer side is the other one
        V a1 = { path[i].x + n1.x * half, path[i].y + n1.y * half }, a2 = { path[i].x + n2.x * half, path[i].y + n2.y * half };
        V b1 = { path[i].x - n1.x * half, path[i].y - n1.y * half }, b2 = { path[i].x - n2.x * half, path[i].y - n2.y * half };
        if (cross > 0) { sideA.push_back(mA); sideB.push_back(b1); sideB.push_back(b2); }     // B is outer
        else { sideA.push_back(a1); sideA.push_back(a2); sideB.push_back(mB); }                // A is outer
    }
    V d = norm(V{path[n - 1].x - path[n - 2].x, path[n - 1].y - path[n - 2].y}), nn = { -d.y, d.x };
    V tip = { path[n - 1].x + d.x * headLen, path[n - 1].y + d.y * headLen };
    std::vector<Pt> o;
    for (size_t i = 0; i < sideA.size(); ++i) o.push_back(P(sideA[i].x, sideA[i].y));
    o.push_back(P(path[n - 1].x + nn.x * headHalf, path[n - 1].y + nn.y * headHalf));
    o.push_back(P(tip.x, tip.y));
    o.push_back(P(path[n - 1].x - nn.x * headHalf, path[n - 1].y - nn.y * headHalf));
    for (size_t i = sideB.size(); i-- > 0;) o.push_back(P(sideB[i].x, sideB[i].y));
    if (type == 2) o.push_back(o[0]);                    // closed outline (filled polygons close themselves)
    polyline(o, type);
}V at(V o, double angDeg, double len) { double a = angDeg * kPi / 180; V p = { o.x + sin(a) * len, o.y - cos(a) * len }; return p; }
double g_dy = 0;                                     // big arrow moved up while a lane row is shown
const double kLift = 32;                             // everything raised (car 2026-11-08: still too low)
double Y(double y) { return y - g_dy - kLift; }
// the plane is drawn in perspective: vertical distances look about half as long as horizontal ones, so vertical
// components are stretched (car 2026-11-08: arrows looked squashed, the upper part vanished behind the horizon)
const double kStretch = 1.5;
V atS(V o, double angDeg, double len) { double a = angDeg * kPi / 180; V p = { o.x + sin(a) * len, o.y - cos(a) * len * kStretch }; return p; }
void turnArrow(double angDeg) {
    std::vector<V> p; p.push_back(V{116, Y(172)});
    if (angDeg == 0) { p.push_back(V{116, Y(78)}); arrowOutline(p); return; }
    V knee = { 116, Y(128) };
    p.push_back(knee);
    p.push_back(atS(knee, angDeg, 46));
    arrowOutline(p);
}
void uTurn(bool left) {
    int s = left ? -1 : 1;
    std::vector<V> p; p.push_back(V{116.0 - s * 22, Y(172)}); p.push_back(V{116.0 - s * 22, Y(96)});
    p.push_back(V{116.0 + s * 22, Y(96)}); p.push_back(V{116.0 + s * 22, Y(132)});
    arrowOutline(p);
}
void roundabout(double exitDeg) {
    V c = { 116, Y(116) };
    std::vector<Pt> ring;
    for (int i = 0; i <= 16; ++i) { V p = atS(c, i * 22.5, 24); ring.push_back(P(p.x, p.y)); }
    polyline(ring, 2);
    std::vector<Pt> inl; inl.push_back(P(116 - kHalf, Y(172))); inl.push_back(P(116 - kHalf, Y(116) + 24 * kStretch));
    polyline(inl, 2);
    std::vector<Pt> inr; inr.push_back(P(116 + kHalf, Y(172))); inr.push_back(P(116 + kHalf, Y(116) + 24 * kStretch));
    polyline(inr, 2);
    std::vector<V> ex; ex.push_back(atS(c, exitDeg, 24)); ex.push_back(atS(c, exitDeg, 44));
    arrowOutline(ex);
}void destination() {
    std::vector<Pt> pole; pole.push_back(P(104, Y(172))); pole.push_back(P(104, Y(90))); polyline(pole, 2);
    std::vector<Pt> flag; flag.push_back(P(104, Y(90))); flag.push_back(P(150, Y(102))); flag.push_back(P(104, Y(116)));
    flag.push_back(P(104, Y(90)));
    polyline(flag, 3);
}
// ---- lane row at the bottom of the plane: one small filled arrow per lane, the lane(s) to take bright ----------------
struct Lane { int shape; bool take; };
std::vector<Lane> parseLanes(const std::string& s) {      // "4h,1o,1o"
    std::vector<Lane> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(',', i); if (e == std::string::npos) e = s.size();
        if (e > i) { Lane l; l.shape = atoi(s.c_str() + i); l.take = s[e - 1] == 'h'; v.push_back(l); }
        i = e + 1;
    }
    return v;
}
// lane row in the flat strip 0x23 (283 x 40) right above the cluster's info line, where the iDrive navigation shows its
// lanes (car 2026-11-08, frame test). One lane: filled = a lane to take, outline only = the others (grey was invisible).
void smallArrow(double cx, double angDeg, bool uturnLeft, bool uturnRight, bool take) {
    const double base = 35, knee = 21;
    const int type = take ? 3 : 2;
    std::vector<V> p; p.push_back(V{cx, base});
    if (uturnLeft || uturnRight) {
        const double s = uturnLeft ? -1 : 1;
        p.push_back(V{cx, 14}); p.push_back(V{cx + s * 10, 14}); p.push_back(V{cx + s * 10, 20});
    } else if (angDeg == 0) p.push_back(V{cx, 14});
    else { V k = { cx, knee }; p.push_back(k); p.push_back(at(k, angDeg, 7)); }
    arrowOutline(p, 3, 8, 7, type);
}
void drawLanes(const std::vector<Lane>& lanes) {
    static const double ang[10] = { 0, 0, -45, 45, -90, 90, -135, 135, 0, 0 };
    const double step = 30, x0 = 141 - step * (lanes.size() - 1) / 2.0;
    g_inst = kLanes; g_item = 0x80;
    clearInst(kLanes); colour(kLanes, kColour); lineWidth(kLanes, kLaneLine);
    for (size_t i = 0; i < lanes.size() && i < 9; ++i) {
        int sh = lanes[i].shape; if (sh < 0 || sh > 9) sh = 1;
        smallArrow(x0 + i * step, ang[sh], sh == 8, sh == 9, lanes[i].take);
    }
    g_inst = kDraw;}// AA Maneuver TYPE_* -> drawing (big arrow)
void drawManeuver(int type, bool hasAngle, long long angle) {
    switch (type) {
        case 3: case 5: turnArrow(-45); break;               // keep / slight left
        case 4: case 6: turnArrow(45); break;                // keep / slight right
        case 7: turnArrow(-90); break;                       // turn left
        case 8: turnArrow(90); break;                        // turn right
        case 9: turnArrow(-135); break;                      // sharp left
        case 10: turnArrow(135); break;                      // sharp right
        case 11: uTurn(true); break;
        case 12: uTurn(false); break;
        case 13: case 15: case 17: case 19: case 21: case 23: case 25: case 27: turnArrow(-35); break;   // ramps, fork, merge left
        case 14: case 16: case 18: case 20: case 22: case 24: case 26: case 28: turnArrow(35); break;    // ... right
        case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 43: case 44: case 45: case 46: {
            // counter-clockwise (right-hand traffic) roundabouts: exit angle counted counter-clockwise from the entry
            double exitDeg = hasAngle ? 180.0 - (double)angle : 0.0;
            if (type == 32 || type == 33 || type == 43 || type == 44) exitDeg = hasAngle ? (double)angle - 180.0 : 0.0;
            roundabout(exitDeg);
            break;
        }
        case 39: case 40: case 41: case 42: destination(); break;
        default: turnArrow(0); break;                         // straight, depart, name change, unknown
    }
}

struct Nav { int status, type, unit, type2, speedLimit; long long angle, dist; bool hasAngle; long mtime; std::string distText, road, road2, lanes; };
bool readNav(Nav& n) {
    struct stat st;
    if (stat(kNav, &st) != 0) return false;
    FILE* f = fopen(kNav, "r");
    if (!f) return false;
    n = Nav(); n.status = n.type = n.unit = n.speedLimit = 0; n.type2 = -1; n.angle = 0; n.dist = -1; n.hasAngle = false; n.mtime = (long)st.st_mtime;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "status=", 7)) n.status = atoi(line + 7);
        else if (!strncmp(line, "type=", 5)) n.type = atoi(line + 5);
        else if (!strncmp(line, "angle=", 6)) n.angle = atoll(line + 6);
        else if (!strncmp(line, "has_angle=", 10)) n.hasAngle = atoi(line + 10) != 0;
        else if (!strncmp(line, "dist_m=", 7)) n.dist = atoll(line + 7);
        else if (!strncmp(line, "dist_text=", 10)) n.distText = line + 10;
        else if (!strncmp(line, "unit=", 5)) n.unit = atoi(line + 5);
        else if (!strncmp(line, "road=", 5)) n.road = line + 5;
        else if (!strncmp(line, "type2=", 6)) n.type2 = atoi(line + 6);
        else if (!strncmp(line, "road2=", 6)) n.road2 = line + 6;
        else if (!strncmp(line, "lanes=", 6)) n.lanes = line + 6;
    }
    fclose(f);
    return true;
}
bool aaRunning() {
    FILE* f = fopen(kAaState, "r");
    if (!f) return false;
    char buf[160]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    return strstr(buf, "state=running") != 0;
}
std::string unitText(int u) {
    switch (u) { case 1: return "m"; case 2: case 3: return "km"; case 4: case 5: return "mi"; case 6: return "ft"; case 7: return "yd"; default: return ""; }
}
std::string fitStreet(const std::string& isoIn) {  // the field shows about 15 characters and cuts on the left
    std::string iso = isoIn;
    const char* longForms[] = { "stra\xDF" "e", "Stra\xDF" "e", "strasse", "Strasse" };
    for (int i = 0; i < 4; ++i) {                    // "...straße" -> "...str."
        size_t p = iso.find(longForms[i]);
        if (p != std::string::npos) iso.replace(p, strlen(longForms[i]), i % 2 ? "Str." : "str.");
    }
    if (iso.size() <= 15) return iso;
    if (iso.compare(0, 9, "Richtung ") == 0) iso.erase(0, 9);   // "Richtung Rhönstr." -> "Rhönstr."
    if (iso.size() <= 15) return iso;
    size_t cut = iso.rfind(' ', 15);                 // whole words only
    if (cut == std::string::npos || cut < 6) cut = 14;
    std::string r = iso.substr(0, cut);
    while (!r.empty() && (r[r.size() - 1] == ' ' || r[r.size() - 1] == '-')) r.erase(r.size() - 1);
    return r;
}

// ---- road shields as bitmaps ------------------------------------------------------------------------------------------
// ImageAddBitmap C05 into the RoadIcon areas 0x18/0x19 = the whole top line, 305 x 44 (ImageGetSize result [inst]
// [01][w u16][h u16]; car 2026-11-08: a 49 x 44 picture came out as a striped band; the size must match exactly). Format NBT_RLE32 ("Bosch", file type 1),
// from the guidance viewer's CGVRLE32Compressor (NavigationGuidanceViewer_NV 0x80f0972): packets
//   [0x80 | n][A R G B]          n (2..127) times this pixel
//   [n][n x (A R G B)]           n (1..127) single pixels
// row after row without breaks, the stream zero-padded to a multiple of 8 bytes (always at least 1 byte of padding).
const int kIconW = 305, kIconH = 44;
const uint32_t kClear = 0x00000000, kBlue = 0xFF00579C, kYellow = 0xFFFFC000, kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
// byte order on the wire A B G R (car 2026-11-08: A R G B turned the blue shield brown)
void putArgb(std::vector<uint8_t>& o, uint32_t c) { o.push_back(c >> 24); o.push_back(c); o.push_back(c >> 8); o.push_back(c >> 16); }
void flushLit(std::vector<uint8_t>& o, std::vector<uint32_t>& lit) {
    if (lit.empty()) return;
    o.push_back((uint8_t)lit.size());
    for (size_t i = 0; i < lit.size(); ++i) putArgb(o, lit[i]);
    lit.clear();
}
std::vector<uint8_t> rle32(const std::vector<uint32_t>& px) {
    std::vector<uint8_t> o;
    std::vector<uint32_t> lit;
    size_t i = 0;
    while (i < px.size()) {
        size_t n = 1;
        while (i + n < px.size() && px[i + n] == px[i] && n < 127) ++n;
        if (n >= 2) { flushLit(o, lit); o.push_back((uint8_t)(0x80 | n)); putArgb(o, px[i]); }
        else { lit.push_back(px[i]); if (lit.size() == 127) flushLit(o, lit); }
        i += n;
    }
    flushLit(o, lit);
    const size_t pad = 8 - o.size() % 8;
    o.insert(o.end(), pad, 0);
    return o;
}
// ---- smooth drawing for the top-line picture (user 2026-11-10: the 5x7 pixel digits looked coarse) -------------------
// Everything is anti-aliased by coverage = distance to the shape edge. Edge pixels are blended over black and kept
// opaque (the cluster background behind the top line is black; partial alpha is not proven on the cluster).
uint32_t mixArgb(uint32_t a, uint32_t b, double t) {             // t = 0 -> a, 1 -> b
    if (t <= 0) return a; if (t >= 1) return b;
    uint32_t r = 0xFF000000;
    for (int sh = 0; sh < 24; sh += 8) {
        const double ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
    }
    return r;
}
void blendPx(std::vector<uint32_t>& px, int x, int y, uint32_t c, double cover) {
    if (x < 0 || x >= kIconW || y < 0 || y >= kIconH || cover <= 0) return;
    uint32_t& d = px[y * kIconW + x];
    const uint32_t under = (d >> 24) ? d : 0xFF000000;              // transparent counts as the black background
    d = mixArgb(under, c, cover > 1 ? 1 : cover);
}
double lineDist(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0; if (t < 0) t = 0; if (t > 1) t = 1;
    const double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}
// stroke digits in a 4 x 8 box (polylines, -1 separates strokes), modelled on the DIN road-sign figures
const double* strokes(char c, int& n) {
    static const double d0[] = { 1,0, 3,0, 4,1.2, 4,6.8, 3,8, 1,8, 0,6.8, 0,1.2, 1,0 };
    static const double d1[] = { 0.8,1.8, 2.6,0, 2.6,8 };
    static const double d2[] = { 0,1.6, 1,0, 3,0, 4,1.2, 4,2.9, 0,8, 4.2,8 };
    static const double d3[] = { 0,1.2, 1,0, 3,0, 4,1.1, 4,2.9, 3,3.9, 1.6,3.9, -1,-1, 3,3.9, 4,5, 4,6.9, 3,8, 1,8, 0,6.8 };
    static const double d4[] = { 3.2,8, 3.2,0, 0,5.6, 4.3,5.6 };
    static const double d5[] = { 4,0, 0.6,0, 0.2,3.7, 1.4,3.3, 3,3.3, 4,4.4, 4,6.9, 3,8, 1,8, 0,6.9 };
    static const double d6[] = { 3.6,0.2, 2.6,0, 1.4,0, 0,1.8, 0,6.8, 1,8, 3,8, 4,6.8, 4,5, 3,3.9, 1,3.9, 0,5 };
    static const double d7[] = { 0,0, 4.1,0, 1.4,8 };
    static const double d8[] = { 1,3.9, 0,2.9, 0,1.1, 1,0, 3,0, 4,1.1, 4,2.9, 3,3.9, 1,3.9, 0,5, 0,6.9, 1,8, 3,8, 4,6.9, 4,5, 3,3.9 };
    static const double d9[] = { 0.4,7.8, 1.4,8, 2.6,8, 4,6.2, 4,1.2, 3,0, 1,0, 0,1.2, 0,3, 1,4.1, 3,4.1, 4,3 };
    static const double* t[10] = { d0, d1, d2, d3, d4, d5, d6, d7, d8, d9 };
    static const int s[10] = { sizeof(d0), sizeof(d1), sizeof(d2), sizeof(d3), sizeof(d4), sizeof(d5), sizeof(d6), sizeof(d7),
                               sizeof(d8), sizeof(d9) };
    if (c < '0' || c > '9') { n = 0; return 0; }
    n = s[c - '0'] / (int)sizeof(double) / 2;
    return t[c - '0'];
}
// a number centred at (cx, cy), digit height h px, stroke width sw px
void drawNumber(std::vector<uint32_t>& px, const std::string& num, double cx, double cy, double h, double sw, uint32_t ink) {
    const double sc = h / 8.0, dw = 4 * sc, gap = sc * 1.6;
    const double total = num.size() * dw + (num.size() > 1 ? (num.size() - 1) * gap : 0);
    double x0 = cx - total / 2;
    for (size_t k = 0; k < num.size(); ++k, x0 += dw + gap) {
        int n = 0; const double* p = strokes(num[k], n);
        if (!p) continue;
        const int bx0 = (int)floor(x0 - sw), bx1 = (int)ceil(x0 + dw + sw), by0 = (int)floor(cy - h / 2 - sw), by1 = (int)ceil(cy + h / 2 + sw);
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                double best = 1e9;
                for (int i = 0; i + 1 < n; ++i) {
                    if (p[2 * i] < 0 || p[2 * i + 2] < 0) continue;
                    const double dd = lineDist(x + 0.5, y + 0.5, x0 + p[2 * i] * sc, cy - h / 2 + p[2 * i + 1] * sc,
                                               x0 + p[2 * i + 2] * sc, cy - h / 2 + p[2 * i + 3] * sc);
                    if (dd < best) best = dd;
                }
                blendPx(px, x, y, ink, sw / 2 + 0.5 - best);
            }
    }
}
// signed distance to a rounded rectangle (negative inside)
double roundRectDist(double x, double y, double x0, double y0, double x1, double y1, double r) {
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    const double qx = fabs(x - cx) - hx, qy = fabs(y - cy) - hy;
    const double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrt(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
}
// autobahn: white number on blue with a white rim; bundesstraße: black number on yellow with a black rim
std::vector<uint32_t> shield(bool autobahn, const std::string& number) {
    std::vector<uint32_t> px(kIconW * kIconH, kClear);
    const double x0 = 1.5, x1 = 53.5, y0 = 5.5, y1 = 38.5, r = 6;  // 52 x 33 sign at the left end of the line
    const uint32_t fill = autobahn ? kBlue : kYellow, ink = autobahn ? kWhite : kBlack;
    for (int y = 0; y < kIconH; ++y)
        for (int x = 0; x < 60; ++x) {
            const double d = roundRectDist(x + 0.5, y + 0.5, x0, y0, x1, y1, r);
            if (d > 0.5) continue;
            blendPx(px, x, y, ink, 0.5 - d);                         // outer edge of the rim
            const double di = roundRectDist(x + 0.5, y + 0.5, x0 + 2.2, y0 + 2.2, x1 - 2.2, y1 - 2.2, r - 2.2);
            if (di < 0.5) blendPx(px, x, y, fill, 0.5 - di);         // inside the rim
        }
    const double h = number.size() > 2 ? 17 : 19;
    drawNumber(px, number, (x0 + x1) / 2, (y0 + y1) / 2, h, number.size() > 2 ? 2.6 : 3.0, ink);
    return px;
}
// speed limit sign (red ring, white disc, black number) into an existing top-line picture, centre x = cx
const uint32_t kRed = 0xFFE01010;
void speedSign(std::vector<uint32_t>& px, int cx, int limit) {
    const double cy = 22, ro = 20.5, ri = 15.2;
    for (int y = 0; y < kIconH; ++y)
        for (int x = cx - 22; x <= cx + 22; ++x) {
            const double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            if (d > ro + 0.5) continue;
            blendPx(px, x, y, kRed, ro + 0.5 - d);
            if (d < ri + 0.5) blendPx(px, x, y, kWhite, ri + 0.5 - d);
        }
    char num[8]; snprintf(num, sizeof(num), "%d", limit);
    drawNumber(px, num, cx, cy, strlen(num) > 2 ? 13 : 15, strlen(num) > 2 ? 2.4 : 2.8, kBlack);
}
// the whole top-line picture: road shield at the left, speed limit sign next to it (either may be missing)
std::vector<uint32_t> topLine(const std::string& sh, int limit) {
    std::vector<uint32_t> px = sh.empty() ? std::vector<uint32_t>(kIconW * kIconH, kClear) : shield(sh[0] == 'A', sh.substr(1));
    if (limit > 0) speedSign(px, sh.empty() ? 23 : 80, limit);
    return px;
}// ImageAddBitmap, layout found on the car 2026-11-08 (ResultAck [inst][bitmapId][p5]; a wrong 4th byte = ErrorAck 06 04):
//   [inst u16][x u16][y u16][file type 1][1][0][bitmap id][RLE32 data]   on transmission type 0 (tx 4 gets no answer)
uint8_t g_bmpId = 0;
void addBitmap(unsigned inst, const std::vector<uint8_t>& rle) {
    std::vector<uint8_t> b; put16(b, inst); put16(b, 0); put16(b, 0);
    g_bmpId = (uint8_t)(g_bmpId % 200 + 1);
    b.push_back(1); b.push_back(1); b.push_back(0); b.push_back(g_bmpId);
    b.insert(b.end(), rle.begin(), rle.end());
    send(0xC05, b, 1);                                // tx 1 = asynchronous channel: tx 0 fails above ~6 KB (ErrorAck 0C)
}
// "A 66", "A66 Frankfurt", "Richtung B 40" -> "A66" / "B40"; "" if the road carries no motorway/federal road number
std::string roadShield(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != 'A' && s[i] != 'B') continue;
        if (i > 0 && (isalnum((unsigned char)s[i - 1]) || (unsigned char)s[i - 1] >= 0x80)) continue;
        size_t j = i + 1;
        if (j < s.size() && s[j] == ' ') ++j;
        size_t k = j;
        while (k < s.size() && k - j < 4 && isdigit((unsigned char)s[k])) ++k;
        if (k == j || k - j > 3) continue;
        if (k < s.size() && (isalnum((unsigned char)s[k]) || (unsigned char)s[k] >= 0x80)) continue;
        return std::string(1, s[i]) + s.substr(j, k - j);
    }
    return "";
}
// "A 66 Frankfurt" -> "Frankfurt" when the shield shows the number
std::string withoutShield(const std::string& road, const std::string& sh) {
    std::string spaced = sh.substr(0, 1) + " " + sh.substr(1);
    std::string r = road; size_t p = r.find(spaced); size_t l = spaced.size();
    if (p == std::string::npos) { p = r.find(sh); l = sh.size(); }
    if (p == std::string::npos) return road;
    r.erase(p, l);
    while (!r.empty() && (r[0] == ' ' || r[0] == '-' || r[0] == ',' || r[0] == '/')) r.erase(0, 1);
    while (!r.empty() && (r[r.size() - 1] == ' ' || r[r.size() - 1] == '-')) r.erase(r.size() - 1);
    return r;
}
// probe variants for the C05 parameter layout (the controller's records: u16 u16 u16 u8 u8 u8 u8 Stream)
void bmpProbe(int variant, unsigned inst, bool autobahn, const std::string& number, uint8_t id) {
    std::vector<uint8_t> rle = rle32(shield(autobahn, number));
    printf("[bmp] variant %d inst %02X %s %s: %u bytes RLE\n", variant, inst, autobahn ? "A" : "B", number.c_str(), (unsigned)rle.size());
    if (variant == 0) {                                 // ImageGetSize: u16 inst, u8 file type
        std::vector<uint8_t> b; put16(b, inst); b.push_back(1); send(0xC04, b); return;
    }
    std::vector<uint8_t> b; put16(b, inst);
    switch (variant) {
        case 1: case 2: put16(b, 0); put16(b, 0); b.push_back(id); b.push_back(1); b.push_back(kIconW); b.push_back(kIconH); break;
        case 3: case 4: put16(b, kIconW); put16(b, kIconH); b.push_back(id); b.push_back(1); b.push_back(0); b.push_back(0); break;
        case 5: case 6: put16(b, 0); put16(b, 0); b.push_back(id); b.push_back(1); b.push_back(kIconW); b.push_back(kIconH);
                put16(b, (int)rle.size()); break;
        // car 2026-11-08: variant 2 -> ErrorAck 06 (parameter out of range) 04 (4th parameter); tx 4 gets no answer
        case 8: put16(b, 0); put16(b, 0); b.push_back(1); b.push_back(id); b.push_back(kIconW); b.push_back(kIconH); break;
        case 10: put16(b, 0); put16(b, 0); b.push_back(1); b.push_back(1); b.push_back(kIconW); b.push_back(kIconH); break;
        case 12: put16(b, 0); put16(b, 0); b.push_back(0); b.push_back(1); b.push_back(kIconW); b.push_back(kIconH); break;
        case 14: put16(b, kIconW); put16(b, kIconH); b.push_back(1); b.push_back(1); b.push_back(0); b.push_back(0); break;
    }
    b.insert(b.end(), rle.begin(), rle.end());
    visible(kArea, true); visible(inst, true);
    send(0xC05, b, variant % 2 ? 4 : 0);                // odd: control channel (tx 4), even: tx 0 like the NBT's results
}
// ---- bitmap throughput benchmark (2026-11-09: could a map picture reach the cluster fast enough?) --------------------
// map-like test picture on a transparent background: detail 0 = route line + position mark, 1 = + six roads,
// 2 = + two filled areas; shift moves everything (a new frame)
double segDist(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0; if (t < 0) t = 0; if (t > 1) t = 1;
    const double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}
std::vector<uint32_t> mapPicture(int w, int h, int detail, int shift) {
    std::vector<uint32_t> px(w * h, kClear);
    const uint32_t road = 0xFFB0B0B0, route = 0xFF2A7FFF, park = 0xFF2E6B3A, water = 0xFF1E4F8F;
    const double s = shift;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t c = kClear;
            if (detail >= 2) {
                if ((x - 40 - s) * (x - 40 - s) + (y - 35) * (y - 35) < 28 * 28) c = park;
                if (x > w - 60 + s && y > h - 45) c = water;
            }
            if (detail >= 1) {
                static const double r[6][4] = { { 0, 30, 1, 60 }, { 0, 110, 1, 95 }, { 0.2, 0, 0.3, 1 }, { 0.75, 0, 0.6, 1 }, { 0, 150, 0.9, 0 }, { 0.5, 0, 1, 120 } };
                for (int i = 0; i < 6; ++i) {
                    const double ax = r[i][0] <= 1 ? r[i][0] * w : r[i][0], ay = r[i][1] <= 1 ? r[i][1] * h : r[i][1];
                    const double bx = r[i][2] <= 1 ? r[i][2] * w : r[i][2], by = r[i][3] <= 1 ? r[i][3] * h : r[i][3];
                    if (segDist(x, y, ax + s, ay, bx + s, by) < 1.5) c = road;
                }
            }
            if (segDist(x, y, w * 0.5 + s, h - 1, w * 0.5 + s, h * 0.55) < 3 || segDist(x, y, w * 0.5 + s, h * 0.55, w * 0.8 + s, h * 0.15) < 3) c = route;
            if (segDist(x, y, w * 0.5 + s, h - 14, w * 0.5 + s, h - 14) < 6) c = kWhite;
            px[y * w + x] = c;
        }
    return px;
}
// coarse vertical bars over the whole picture: a picture with many runs (size test)
std::vector<uint32_t> barsPicture(int w, int h, int period) {
    std::vector<uint32_t> px(w * h, kClear);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if ((x / period) % 2) px[y * w + x] = (y / 8) % 2 ? kYellow : kBlue;
    return px;
}
// sends one bitmap and waits for the cluster's answer; returns the time in ms, -1 = no answer
long benchSend(const char* label, unsigned inst, const std::vector<uint32_t>& px, uint32_t tx, long timeoutMs) {
    std::vector<uint8_t> rle = rle32(px);
    std::vector<uint8_t> b; put16(b, inst); put16(b, 0); put16(b, 0);
    g_bmpId = (uint8_t)(g_bmpId % 200 + 1);
    b.push_back(1); b.push_back(1); b.push_back(0); b.push_back(g_bmpId);
    b.insert(b.end(), rle.begin(), rle.end());
    if (b.size() > 60000) { printf("[bench] %s: %u bytes, too big, skipped\n", label, (unsigned)b.size()); return -1; }
    g_benchOp = -1; g_benchInst = inst;
    const long long t0 = nowMs();
    send(0xC05, b, tx);
    while (g_benchOp < 0 && nowMs() - t0 < timeoutMs && !g_stop) usleep(5000);
    const long ms = g_benchOp < 0 ? -1 : (long)(g_benchT - t0);
    if (ms < 0) printf("[bench] %s: inst %02X tx %u, %u bytes -> NO ANSWER in %ld ms\n", label, inst, (unsigned)tx, (unsigned)b.size(), timeoutMs);
    else printf("[bench] %s: inst %02X tx %u, %u bytes -> op %X after %ld ms (%ld bytes/s)\n", label, inst, (unsigned)tx,
                (unsigned)b.size(), g_benchOp, ms, ms > 0 ? (long)(b.size() * 1000 / ms) : 0);
    g_benchInst = 0;
    return g_benchOp == 0xD ? ms : -1;
}
void bench() {
    g_probe = true;
    visible(kArea, true); visible(kIconA, true);
    // 1) the known-good area 0x18 (305 x 44): small, medium and larger pictures on the control channel
    benchSend("top line, shield", kIconA, topLine("A66", 0), 0, 8000);
    benchSend("top line, shield + sign", kIconA, topLine("A66", 50), 0, 8000);
    benchSend("top line, bars 32", kIconA, barsPicture(kIconW, kIconH, 32), 0, 12000);
    benchSend("top line, bars 12", kIconA, barsPicture(kIconW, kIconH, 12), 0, 20000);
    // 2) the same small picture on the asynchronous (packet) channel
    benchSend("top line, shield, async", kIconA, topLine("A66", 0), 1, 4000);
    usleep(500000);
    benchSend("top line, shield again", kIconA, topLine("A66", 0), 0, 8000);
    // 3) does a drawing area (221 x 158) take a bitmap at all?
    const unsigned areas[2] = { 0x21, 0x1F };
    for (int a = 0; a < 2 && !g_stop; ++a) {
        clearInst(areas[a]); visible(areas[a], true);
        if (benchSend("map area, route only", areas[a], mapPicture(221, 158, 0, 0), 0, 15000) < 0) { visible(areas[a], false); continue; }
        usleep(1500000);
        benchSend("map area, route + roads", areas[a], mapPicture(221, 158, 1, 0), 0, 25000);
        usleep(1500000);
        // 4) three frames in a row = the frame rate a moving map would get
        const long long t0 = nowMs(); int ok = 0;
        for (int i = 1; i <= 3 && !g_stop; ++i) if (benchSend("map frame", areas[a], mapPicture(221, 158, 1, i * 6), 0, 25000) >= 0) ++ok;
        const long long dt = nowMs() - t0;
        printf("[bench] area %02X: %d of 3 frames in %lld ms = %.2f frames/s\n", areas[a], ok, dt, dt > 0 ? ok * 1000.0 / dt : 0.0);
        usleep(3000000);
        clearInst(areas[a]); visible(areas[a], false);
    }
    visible(kIconA, false); visible(kIconB, false); visible(kArea, false);
    printf("[bench] done\n");
}
// second run (car 2026-11-10): on tx 0 bitmaps above ~6 KB end with ErrorAck 0C (segmentation error) - same on the
// asynchronous channel (tx 1)? Map-area pictures of growing size, then three frames, all on tx 1.
void bench2() {
    g_probe = false;
    const unsigned a = 0x21;
    visible(kArea, true); clearInst(a); visible(a, true);
    benchSend("async, route only", a, mapPicture(221, 158, 0, 0), 1, 8000);
    usleep(800000);
    benchSend("async, route + roads", a, mapPicture(221, 158, 1, 0), 1, 15000);
    usleep(800000);
    benchSend("async, route + roads + areas", a, mapPicture(221, 158, 2, 0), 1, 15000);
    usleep(800000);
    benchSend("async, bars 16", a, barsPicture(221, 158, 16), 1, 20000);
    usleep(800000);
    const long long t0 = nowMs(); int ok = 0;
    for (int i = 1; i <= 3 && !g_stop; ++i) if (benchSend("async map frame", a, mapPicture(221, 158, 1, i * 6), 1, 15000) >= 0) ++ok;
    const long long dt = nowMs() - t0;
    printf("[bench] async: %d of 3 frames in %lld ms = %.2f frames/s\n", ok, dt, dt > 0 ? ok * 1000.0 / dt : 0.0);
    usleep(2000000);
    clearInst(a); visible(a, false); visible(kArea, false);
    printf("[bench] done\n");
}
// --mapdemo N (user request 2026-11-10: see what a map in the cluster looks like): the simple test map in the middle
// area 0x21, moving, as fast as the cluster takes it (asynchronous channel), for N seconds
void mapDemo(long seconds, unsigned a) {
    // exact area sizes (ImageGetSize): a bitmap must match them
    const int w = a == 0x1D || a == 0x1E ? 145 : a == 0x23 || a == 0x24 ? 283 : 221;
    const int h = a == 0x1D || a == 0x1E ? 44 : a == 0x23 || a == 0x24 ? 40 : 158;
    if (a == 0x1F) visible(0x20, false);
    if (a == 0x21) visible(0x22, false);
    visible(kArea, true); clearInst(a); visible(a, true);
    printf("[mapdemo] area %02X %dx%d\n", a, w, h);
    const long long t0 = nowMs(); int frames = 0, shift = 0;
    while (!g_stop && nowMs() - t0 < seconds * 1000LL) {
        if (benchSend("map demo", a, mapPicture(w, h, 2, shift), 1, 5000) >= 0) ++frames;
        shift = (shift + 4) % 60;
    }
    const long long dt = nowMs() - t0;
    printf("[mapdemo] %d frames in %lld ms = %.2f frames/s\n", frames, dt, dt > 0 ? frames * 1000.0 / dt : 0.0);
    clearInst(a); visible(a, false); visible(kArea, false);
}
}

int main(int argc, char** argv) {
    long seconds = 0;
    bool demo = false, frames = false, doBench = false, doBench2 = false, doMapDemo = false; unsigned mapInst = 0x21;
    int bmpVariant = -1; unsigned bmpInst = 0x18; std::string bmpRoad = "A66";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--frames")) frames = true;   // test frames in the drawing areas 0x1D, 0x21, 0x23
        else if (!strcmp(argv[i], "--bench")) doBench = true;
        else if (!strcmp(argv[i], "--bench2")) doBench2 = true;
        else if (!strcmp(argv[i], "--mapdemo")) doMapDemo = true;   // with --seconds
        else if (!strcmp(argv[i], "--mapinst") && i + 1 < argc) mapInst = (unsigned)strtoul(argv[++i], 0, 16);   // bitmap throughput test (car parked, no route)
        else if (!strcmp(argv[i], "--demo")) demo = true;   // fixed test data instead of the phone (with --seconds)
        else if (!strcmp(argv[i], "--bmp") && i + 3 < argc) {   // --bmp VARIANT INST(hex) ROAD   e.g. --bmp 1 18 A66
            bmpVariant = atoi(argv[++i]); bmpInst = (unsigned)strtoul(argv[++i], 0, 16); bmpRoad = argv[++i];
        }
        else { fprintf(stderr, "usage: %s [--seconds N] | --bmp VARIANT INST ROAD\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, onSignal); signal(SIGINT, onSignal); signal(SIGPIPE, SIG_IGN);
    if (!g_most.setTap(&onMsg, NULL) || !g_most.start("NBTPROJ_CLUSTER")) return 3;
    for (int w = 0; w < 50 && g_most.state() != nbt::most::kUp && !g_stop; ++w) usleep(100000);
    if (g_most.state() != nbt::most::kUp) { printf("[cluster] MOST link not up\n"); return 3; }
    printf("[cluster] v2 up, own device 0x%04X\n", g_most.deviceId());
    if (doBench) { bench(); fflush(stdout); _exit(0); }
    if (doBench2) { bench2(); fflush(stdout); _exit(0); }
    if (doMapDemo) { mapDemo(seconds ? seconds : 40, mapInst); fflush(stdout); _exit(0); }
    if (frames) {
        // frame around each area (sizes from ImageGetSize) + a mark: 0x21 an X, 0x23 a middle line, 0x1D nothing inside
        g_probe = true;
        struct F { unsigned inst, w, h; int mark; } fs[3] = { { 0x21, 221, 158, 1 }, { 0x23, 283, 40, 2 }, { 0x1D, 145, 44, 0 } };
        visible(kArea, true);
        for (int i = 0; i < 3; ++i) {
            const F& f = fs[i];
            g_inst = f.inst; g_item = 0x80;
            clearInst(f.inst); colour(f.inst, kColour); lineWidth(f.inst, kLine);
            // frame 5 px inside (car 2026-11-08: lines on the very edge are cut off) + 1/2/3 filled squares in the middle
            const int m = 5;
            std::vector<Pt> r; r.push_back(P(m, m)); r.push_back(P(f.w - 1 - m, m)); r.push_back(P(f.w - 1 - m, f.h - 1 - m));
            r.push_back(P(m, f.h - 1 - m)); r.push_back(P(m, m));
            polyline(r, 2);
            const int count = f.mark == 1 ? 1 : f.mark == 2 ? 2 : 3, sz = 12;
            for (int q = 0; q < count; ++q) {
                const int cx = (int)f.w / 2 + (q - (count - 1) / 2.0) * (sz + 8), cy = (int)f.h / 2;
                std::vector<Pt> s; s.push_back(P(cx - sz / 2, cy - sz / 2)); s.push_back(P(cx + sz / 2, cy - sz / 2));
                s.push_back(P(cx + sz / 2, cy + sz / 2)); s.push_back(P(cx - sz / 2, cy + sz / 2));
                polyline(s, 3);
            }
            visible(f.inst, true);
        }
        printf("[frames] drawn, holding %ld s\n", seconds ? seconds : 60);
        for (long i = 0; i < (seconds ? seconds : 60) * 10 && !g_stop; ++i) usleep(100000);
        for (int i = 0; i < 3; ++i) { clearInst(fs[i].inst); visible(fs[i].inst, false); }
        visible(kArea, false);
        printf("[frames] done\n"); fflush(stdout); _exit(0);
    }
    if (bmpVariant >= 0) {
        g_probe = true;
        const bool autobahn = bmpRoad.size() > 1 && bmpRoad[0] == 'A';
        bmpProbe(bmpVariant, bmpInst, autobahn, bmpRoad.substr(1), (uint8_t)(0x40 + bmpVariant));
        for (int i = 0; i < 40 && !g_stop; ++i) usleep(100000);
        printf("[bmp] done\n"); fflush(stdout); _exit(0);
    }

    const long long t0 = nowMs();
    bool shown = false, barShown = false;
    int drawn = -1000; long long drawnAngle = -999; std::string drawnLanes;
    std::string shownStreet = "\x01", shownDist = "\x01", shownUnit = "\x01", shownShield;
    int lastBar = -1; unsigned lastBmw = 0;
    while (!g_stop && (!seconds || nowMs() - t0 < seconds * 1000LL)) {
        const long long t = nowMs();
        Nav n;
        bool haveNav;
        if (demo) {                                       // keep right onto the motorway, 2 of 4 lanes, counting down
            n = Nav(); haveNav = true; n.mtime = (long)time(0);
            n.status = 1; n.type = 4; n.hasAngle = false; n.angle = 0; n.unit = 1; n.type2 = -1;
            n.dist = 800 - ((t - t0) / 1000 * 25) % 800;
            char d[16]; snprintf(d, sizeof(d), "%lld", n.dist); n.distText = d;
            n.road = ((t - t0) / 20000) % 2 ? "B 40 Fulda" : "A 66 Frankfurt";   // shield changes every 20 s
            n.lanes = "1o,1o,3h,3h"; n.speedLimit = 50;
        } else haveNav = readNav(n);
        const bool fresh = haveNav && time(0) - n.mtime < 15;   // the phone updates the position about every second while it guides
        const bool bmw = t < g_bmwUntil;
        if (g_bmwResults != lastBmw) { if (!bmw || shown) printf("[cluster] iDrive navigation draws: yielding\n"); lastBmw = g_bmwResults; }
        const bool want = !bmw && fresh && (n.status == 1 || n.status == 3) && (demo || aaRunning());
        if (want && !shown) {
            printf("[cluster] show (AA navigation active)\n");
            visible(kOther, false); clearInst(kDraw);
            visible(kLanesAlt, false); clearInst(kLanes);
            visible(kArea, true); visible(kDraw, true); visible(kLanes, true);
            visible(kStreet, true); visible(kDist, true); visible(kUnit, true);
            shown = true; drawn = -1000; lastBar = -1; barShown = false; drawnLanes = "\x01";
            shownStreet = shownDist = shownUnit = "\x01"; shownShield.clear();
        }
        if (want && shown) {
            const int big = n.type;                       // the current step, always (bar = its distance)
            const long long ang = n.hasAngle ? n.angle : -999;
            if (big != drawn || ang != drawnAngle || n.lanes != drawnLanes) {
                printf("[cluster] big arrow: type %d (%lld m), next: type %d '%s'\n", big, n.dist, n.type2, n.road2.c_str());
                clearInst(kDraw); g_item = 0x80;
                colour(kDraw, kColour); lineWidth(kDraw, kLine);
                std::vector<Lane> lanes = parseLanes(n.lanes);
                g_dy = 0;                                 // lanes have their own strip now
                drawManeuver(big, n.hasAngle, n.angle);
                if (n.lanes != drawnLanes) {
                    if (!lanes.empty()) { printf("[cluster] lanes %s\n", n.lanes.c_str()); drawLanes(lanes); }
                    else clearInst(kLanes);
                }
                drawn = big; drawnAngle = ang; drawnLanes = n.lanes;
            }
            // top line: street and distance of the current step (user 2026-11-08: the following step later)
            std::string street = fitStreet(toIso(n.road));
            std::string dist = n.distText;
            // the cluster's big distance font has no comma (car 2026-11-10: "5,4" showed as "5 4"), the point works
            for (size_t i = 0; i < dist.size(); ++i) if (dist[i] == ',') dist[i] = '.';
            if (dist.size() > 5) dist = dist.substr(0, 5);
            std::string unit = unitText(n.unit);
            // road shield (A = blue, B = yellow) in both RoadIcon areas when the step's road carries a number
            std::string sh = roadShield(n.road);
            if (sh.empty()) sh = roadShield(n.road2);
            // speed limit: Android Auto sends none; only the demo has one so far (sign drawn into the same picture)
            char key[32]; snprintf(key, sizeof(key), "%s|%d", sh.c_str(), n.speedLimit);
            if (key != shownShield) {
                if (!sh.empty() || n.speedLimit > 0) {
                    std::vector<uint8_t> rle = rle32(topLine(sh, n.speedLimit));
                    printf("[cluster] top-line picture %s (%u bytes)\n", key, (unsigned)rle.size());
                    addBitmap(kIconA, rle); addBitmap(kIconB, rle);
                    visible(kIconA, true); visible(kIconB, true);
                } else { visible(kIconA, false); visible(kIconB, false); }
                shownShield = key;
            }
            if (!sh.empty()) street = fitStreet(toIso(withoutShield(n.road, sh)));
            if (street != shownStreet) { text(kStreet, street); shownStreet = street; }
            if (dist != shownDist) { text(kDist, dist); shownDist = dist; }
            if (unit != shownUnit) { text(kUnit, unit); shownUnit = unit; }
            // distance bar only from 1 km on (user 2026-11-10), empty at the maneuver
            if (n.dist >= 0 && n.dist < kNear) {
                const int v = (int)(n.dist * 255 / kNear);
                if (lastBar < 0 || abs(v - lastBar) >= 4) { bar(v); lastBar = v; }
                if (!barShown) { visible(kBar, true); barShown = true; }
            } else if (barShown) { visible(kBar, false); barShown = false; lastBar = -1; }
        }
        if (!want && shown) {
            if (!bmw) {
                printf("[cluster] hide\n");
                clearInst(kDraw); text(kStreet, ""); text(kDist, ""); text(kUnit, "");
                visible(kIconA, false); visible(kIconB, false); clearInst(kLanes); visible(kLanes, false);
                visible(kBar, false); visible(kArea, false);
            } else printf("[cluster] handing the cluster to the iDrive navigation\n");
            shown = false;
        }
        usleep(250000);
    }
    if (shown && !(nowMs() < g_bmwUntil)) {
        clearInst(kDraw); text(kStreet, ""); text(kDist, ""); text(kUnit, ""); visible(kIconA, false); visible(kIconB, false);
        clearInst(kLanes); visible(kLanes, false); visible(kBar, false); visible(kArea, false);
        usleep(300000);
    }
    printf("[cluster] end\n");
    fflush(stdout);
    _exit(0);
}
