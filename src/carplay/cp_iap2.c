/* nbt_carplay: iAP2 over the carkit service: identification, authentication with the car's chip, CarPlay start.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * As nbt_carplay_probe (car 2026-11-23: identification accepted, authentication succeeded with the 2.0B chip,
 * CarPlayAvailability came), then as xcertplay's wired control client: PowerSourceUpdate, the subscriptions,
 * StartHID (media playback remote), and CarPlayStartSession (4301: our IPv6 address on the NCM link, the AirPlay port,
 * our MAC, our Ed25519 key, the source version) for each CarPlayAvailability (4300). The iPhone then connects to the
 * AirPlay receiver (cp_airplay.c). NowPlayingUpdate (5001) gives the playback state for nbt_knob
 * (/fs/ram3/nbt-aa-media: 2 playing, 3 paused).
 * Link layer: marker FF 55 02 00 EE 10 and a SYN from us; packets FF 5A, length, control (80 SYN, 40 ACK, 10 RST),
 * sequence, acknowledgement, session, header checksum, payload, payload checksum. Session 10 carries the control
 * messages: 40 40, length, id, parameters (length, id, data; groups nest).
 */
#include <stdio.h>
#include <string.h>
#include "cp.h"
#include "cp_lockdown.h"

enum { CONTROL = 10 };

static struct {
    struct stream* s;
    unsigned char in[140000];
    unsigned inn;
    unsigned sent_seq, last_rx, synced, peer_maxlen, peer_acks, normal;
    unsigned char csm[140000];
    unsigned csmn;
    int identified, authenticated, failed, started, carplay_sent;
    const struct iap2_carplay* cp;
} g_l;

static unsigned char csum(const unsigned char* b, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; ++i)
        s += b[i];
    return (unsigned char)(0x100 - (s & 0xff));
}

static int link_packet(unsigned control, unsigned seq, unsigned session, const unsigned char* p, unsigned n)
{
    static unsigned char f[70000];
    const unsigned len = 9 + (n ? n + 1 : 0);
    if (len > sizeof(f))
        return -1;
    f[0] = 0xff;
    f[1] = 0x5a;
    put16(f + 2, len);
    f[4] = (unsigned char)control;
    f[5] = (unsigned char)seq;
    f[6] = (unsigned char)g_l.last_rx;
    f[7] = (unsigned char)session;
    f[8] = csum(f, 8);
    if (n) {
        memcpy(f + 9, p, n);
        f[9 + n] = csum(p, n);
    }
    return stream_write(g_l.s, f, len);
}

static int csm_send(unsigned id, const unsigned char* params, unsigned n)
{
    static unsigned char m[70000];
    unsigned off = 0;
    const unsigned chunk = g_l.peer_maxlen > 10 ? g_l.peer_maxlen - 10 : 1000;
    if (n + 6 > 0xffff)
        return -1;
    m[0] = 0x40;
    m[1] = 0x40;
    put16(m + 2, n + 6);
    put16(m + 4, id);
    memcpy(m + 6, params, n);
    say("iap2: -> %04x (%u bytes)", id, n + 6);
    while (off < n + 6) {
        const unsigned k = n + 6 - off < chunk ? n + 6 - off : chunk;
        g_l.sent_seq = (g_l.sent_seq + 1) & 0xff;
        if (link_packet(0x40, g_l.sent_seq, CONTROL, m + off, k))
            return -1;
        off += k;
    }
    return 0;
}

/* ---- parameters ---- */

static unsigned prm(unsigned char* b, unsigned at, unsigned id, const void* data, unsigned n)
{
    put16(b + at, n + 4);
    put16(b + at + 2, id);
    if (n)
        memcpy(b + at + 4, data, n);
    return at + 4 + n;
}

static unsigned prm_str(unsigned char* b, unsigned at, unsigned id, const char* s)
{
    return prm(b, at, id, s, (unsigned)strlen(s) + 1);
}

static unsigned prm_u8(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    const unsigned char x = (unsigned char)v;
    return prm(b, at, id, &x, 1);
}

static unsigned prm_u16(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    unsigned char x[2];
    put16(x, v);
    return prm(b, at, id, x, 2);
}

static unsigned prm_u32(unsigned char* b, unsigned at, unsigned id, unsigned v)
{
    unsigned char x[4];
    put32(x, v);
    return prm(b, at, id, x, 4);
}

static unsigned prm_u16s(unsigned char* b, unsigned at, unsigned id, const unsigned short* v, unsigned count)
{
    unsigned char x[128];
    unsigned i;
    for (i = 0; i < count; ++i)
        put16(x + 2 * i, v[i]);
    return prm(b, at, id, x, 2 * count);
}

static const unsigned char* prm_find(const unsigned char* p, unsigned n, unsigned id, unsigned* len)
{
    unsigned at = 0;
    while (at + 4 <= n) {
        const unsigned l = get16(p + at), i = get16(p + at + 2);
        if (l < 4 || at + l > n)
            return 0;
        if (i == id) {
            *len = l - 4;
            return p + at + 4;
        }
        at += l;
    }
    return 0;
}

/* IdentificationInformation (1D01) as the probe's (accepted on the car): identity strings, the messages we send and
 * take, power, an external accessory protocol, English, the USB host transport (the NCM control interface) and a
 * media playback HID remote */
static int send_identification(void)
{
    static const unsigned short sent[] = {0x5000, 0x5002, 0x5200, 0x5203, 0xae00, 0xae02, 0x4157, 0x4159,
                                          0x4154, 0x4156, 0xae03, 0x4301, 0x6800, 0x6802, 0x6803};
    static const unsigned short taken[] = {0xea00, 0xea01, 0x5001, 0x5201, 0x5202, 0xae01, 0x4158, 0x4155, 0x4300,
                                           0x6801};
    unsigned char b[2048], g[256];
    unsigned at = 0, gn;
    at = prm_str(b, at, 0, "BMW NBT");
    at = prm_str(b, at, 1, "NBT");
    at = prm_str(b, at, 2, "BMW");
    at = prm_str(b, at, 3, "nbt-android-auto");
    at = prm_str(b, at, 4, "1.0");
    at = prm_str(b, at, 5, "1.0");
    at = prm_u16s(b, at, 6, sent, sizeof(sent) / sizeof(sent[0]));
    at = prm_u16s(b, at, 7, taken, sizeof(taken) / sizeof(taken[0]));
    at = prm_u8(b, at, 8, 2);
    at = prm_u16(b, at, 9, 20);
    gn = prm_u8(g, 0, 0, 1);
    gn = prm_str(g, gn, 1, "com.nbtaa.carplay");
    gn = prm_u8(g, gn, 2, 0);
    at = prm(b, at, 10, g, gn);
    at = prm_str(b, at, 12, "en");
    at = prm_str(b, at, 13, "en");
    gn = prm_u16(g, 0, 0, 0);
    gn = prm_str(g, gn, 1, "USBHostTransport");
    gn = prm(g, gn, 2, 0, 0);
    gn = prm_u8(g, gn, 3, g_l.cp->ncm_interface);
    gn = prm(g, gn, 4, 0, 0);
    at = prm(b, at, 16, g, gn);
    gn = prm_u16(g, 0, 0, 0);
    gn = prm_str(g, gn, 1, "Media Playback Remote");
    gn = prm_u8(g, gn, 2, 1);
    at = prm(b, at, 18, g, gn);
    return csm_send(0x1d01, b, at);
}

static const unsigned char MEDIA_REMOTE[] = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                                             0x95, 0x04, 0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb5, 0x09, 0xb6, 0x81, 0x02,
                                             0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0};

/* after authentication: power, subscriptions, the HID media remote (as xcertplay's wired control client) */
static void start_control(void)
{
    unsigned char b[512], g[64];
    unsigned at, gn;
    at = prm_u16(b, 0, 0, 1000);   /* 1000 mA for the iPhone */
    at = prm_u8(b, at, 1, 1);      /* charging */
    csm_send(0xae03, b, at);
    gn = prm(g, 0, 1, 0, 0);       /* NowPlaying: title, duration, album, artist, artwork id */
    gn = prm(g, gn, 4, 0, 0);
    gn = prm(g, gn, 6, 0, 0);
    gn = prm(g, gn, 12, 0, 0);
    gn = prm(g, gn, 26, 0, 0);
    at = prm(b, 0, 0, g, gn);
    gn = prm(g, 0, 0, 0, 0);       /* playback: status, elapsed, queue index, queue count, app name */
    gn = prm(g, gn, 1, 0, 0);
    gn = prm(g, gn, 2, 0, 0);
    gn = prm(g, gn, 3, 0, 0);
    gn = prm(g, gn, 7, 0, 0);
    at = prm(b, at, 1, g, gn);
    csm_send(0x5000, b, at);
    csm_send(0x5200, b, 0);
    at = prm(b, 0, 4, 0, 0);
    at = prm(b, at, 5, 0, 0);
    at = prm(b, at, 6, 0, 0);
    csm_send(0xae00, b, at);
    at = prm(b, 0, 0, 0, 0);
    at = prm(b, at, 4, 0, 0);
    at = prm(b, at, 5, 0, 0);
    csm_send(0x4157, b, at);
    at = prm(b, 0, 0, 0, 0);
    at = prm(b, at, 1, 0, 0);
    at = prm(b, at, 2, 0, 0);
    at = prm(b, at, 3, 0, 0);
    at = prm(b, at, 4, 0, 0);
    at = prm(b, at, 11, 0, 0);
    csm_send(0x4154, b, at);
    at = prm_u16(b, 0, 0, 0);
    at = prm_u16(b, at, 1, 2);
    at = prm_u16(b, at, 2, 1);
    at = prm(b, at, 4, MEDIA_REMOTE, sizeof(MEDIA_REMOTE));
    csm_send(0x6800, b, at);
    g_l.started = 1;
}

static void carplay_start(void)
{
    unsigned char b[512], g[128];
    unsigned at, gn;
    const struct iap2_carplay* cp = g_l.cp;
    gn = prm_str(g, 0, 0, cp->address);
    at = prm(b, 0, 0, g, gn);
    at = prm_u32(b, at, 2, cp->port);
    at = prm_str(b, at, 3, cp->mac);
    at = prm_str(b, at, 4, cp->public_key);
    at = prm_str(b, at, 5, cp->source_version);
    say("iap2: CarPlayStartSession: [%s]:%u, %s", cp->address, cp->port, cp->mac);
    csm_send(0x4301, b, at);
    g_l.carplay_sent++;
    cp_reached(STOP_IAP2);
}

static void now_playing(const unsigned char* p, unsigned n)
{
    unsigned len, l2;
    const unsigned char* g = prm_find(p, n, 1, &len);
    const unsigned char* st;
    if (!g || !(st = prm_find(g, len, 0, &l2)) || l2 < 1)
        return;
    say("iap2: playback state %u", st[0]);
    flag_set("/fs/ram3/nbt-aa-media", st[0] == 1 ? "2\n" : "3\n");
}

static void on_message(unsigned id, const unsigned char* p, unsigned n)
{
    unsigned char out[1400], sig[128];
    unsigned at, len, sl;
    const unsigned char* v;
    if (id != 0x5001 && id != 0xae01)
        say("iap2: <- %04x (%u bytes)", id, n + 6);
    switch (id) {
    case 0x1d00:
        send_identification();
        break;
    case 0x1d02:
        say("iap2: identification accepted");
        g_l.identified = 1;
        break;
    case 0x1d03: {
        unsigned a = 0;
        say("iap2: identification REJECTED");
        while (a + 4 <= n) {
            const unsigned l = get16(p + a);
            say("iap2:   parameter %u", get16(p + a + 2));
            if (l < 4)
                break;
            a += l;
        }
        g_l.failed = 1;
        break;
    }
    case 0xaa00: {
        const unsigned char* cert;
        unsigned cn;
        if (acp_cert(&cert, &cn)) {
            g_l.failed = 1;
            break;
        }
        at = prm(out, 0, 0, cert, cn);
        csm_send(0xaa01, out, at);
        break;
    }
    case 0xaa02:
        v = prm_find(p, n, 0, &len);
        if (!v || acp_sign(v, len, sig, &sl)) {
            g_l.failed = 1;
            break;
        }
        at = prm(out, 0, 0, sig, sl);
        csm_send(0xaa03, out, at);
        break;
    case 0xaa04:
        say("iap2: authentication FAILED");
        g_l.failed = 1;
        break;
    case 0xaa05:
        say("iap2: authentication succeeded");
        g_l.authenticated = 1;
        if (!g_l.started)
            start_control();
        break;
    case 0x4300:
        if ((v = prm_find(p, n, 0, &len)) != 0) {
            unsigned l2;
            const unsigned char* a = prm_find(v, len, 0, &l2);
            say("iap2: CarPlay available over USB: %s", a && l2 ? (a[0] ? "yes" : "no") : "?");
        }
        carplay_start();
        break;
    case 0x5001:
        now_playing(p, n);
        break;
    default:
        break;
    }
}

static void csm_in(const unsigned char* b, unsigned n)
{
    if (n > sizeof(g_l.csm) - g_l.csmn)
        g_l.csmn = 0;
    memcpy(g_l.csm + g_l.csmn, b, n);
    g_l.csmn += n;
    for (;;) {
        unsigned skip = 0, len;
        while (skip + 1 < g_l.csmn && !(g_l.csm[skip] == 0x40 && g_l.csm[skip + 1] == 0x40))
            ++skip;
        if (skip) {
            memmove(g_l.csm, g_l.csm + skip, g_l.csmn - skip);
            g_l.csmn -= skip;
        }
        if (g_l.csmn < 6)
            return;
        len = get16(g_l.csm + 2);
        if (len < 6) {
            memmove(g_l.csm, g_l.csm + 1, --g_l.csmn);
            continue;
        }
        if (g_l.csmn < len)
            return;
        on_message(get16(g_l.csm + 4), g_l.csm + 6, len - 6);
        memmove(g_l.csm, g_l.csm + len, g_l.csmn - len);
        g_l.csmn -= len;
    }
}

static void link_in(void)
{
    for (;;) {
        unsigned skip = 0, len, control, seq, session, pn;
        const unsigned char* pl;
        while (skip + 1 < g_l.inn && !(g_l.in[skip] == 0xff && g_l.in[skip + 1] == 0x5a)) {
            if (g_l.in[skip] == 0xff && skip + 6 <= g_l.inn && g_l.in[skip + 1] == 0x55) {
                skip += 6;   /* the iPhone's marker */
                continue;
            }
            ++skip;
        }
        if (skip) {
            memmove(g_l.in, g_l.in + skip, g_l.inn - skip);
            g_l.inn -= skip;
        }
        if (g_l.inn < 9)
            return;
        if (csum(g_l.in, 8) != g_l.in[8]) {
            memmove(g_l.in, g_l.in + 1, --g_l.inn);
            continue;
        }
        len = get16(g_l.in + 2);
        control = g_l.in[4];
        seq = g_l.in[5];
        session = g_l.in[7];
        if (len < 9) {
            memmove(g_l.in, g_l.in + 1, --g_l.inn);
            continue;
        }
        if (g_l.inn < len)
            return;
        pl = g_l.in + 9;
        pn = len > 9 ? len - 10 : 0;
        if (len > 9 && csum(pl, pn) != pl[pn])
            say("iap2: packet %u with a bad payload checksum (ignored)", seq);
        else if (control & 0x10) {
            say("iap2: the iPhone reset the link");
            g_l.failed = 1;
        } else {
            if (control & 0x80) {
                if (pn >= 10) {
                    g_l.peer_maxlen = get16(pl + 2);
                    g_l.peer_acks = pl[9];
                }
                g_l.last_rx = seq;
                g_l.synced = 1;
                link_packet(0x40, g_l.sent_seq, 0, 0, 0);
            }
            if ((control & 0x40) && !g_l.normal && g_l.synced) {
                g_l.normal = 1;
                say("iap2: link up");
            }
            if (control == 0x40 && pn) {
                if (seq == ((g_l.last_rx + 1) & 0xff)) {
                    g_l.last_rx = seq;
                    if (session == CONTROL)
                        csm_in(pl, pn);
                }
                if (g_l.peer_acks)
                    link_packet(0x40, g_l.sent_seq, 0, 0, 0);
            }
        }
        memmove(g_l.in, g_l.in + len, g_l.inn - len);
        g_l.inn -= len;
    }
}

int iap2_run(struct stream* s, const struct iap2_carplay* cp)
{
    static const unsigned char marker[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
    static const unsigned char syn[19] = {1, 4, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 10, 0, 2, 11, 2, 1, 12, 1, 2};
    const long long t0 = ms_now();
    int resent = 0;
    memset(&g_l, 0, sizeof(g_l));
    g_l.s = s;
    g_l.cp = cp;
    g_l.sent_seq = 99;
    if (acp_open())
        return -1;
    if (stream_write(s, marker, sizeof(marker)) || link_packet(0x80, g_l.sent_seq, 0, syn, sizeof(syn)))
        return -1;
    while (!g_stop && !g_l.failed && !usb_gone()) {
        unsigned char b[16384];
        const int r = stream_read_some(s, b, sizeof(b), 300);
        if (r < 0) {
            say("iap2: the carkit connection ended");
            break;
        }
        if (r > 0) {
            if ((unsigned)r > sizeof(g_l.in) - g_l.inn)
                g_l.inn = 0;
            memcpy(g_l.in + g_l.inn, b, (unsigned)r);
            g_l.inn += (unsigned)r;
            link_in();
        }
        if (!g_l.synced && !resent && ms_now() - t0 > 5000) {
            resent = 1;
            link_packet(0x80, g_l.sent_seq, 0, syn, sizeof(syn));
            say("iap2: SYN again");
        }
        if (!g_l.identified && ms_now() - t0 > 60000) {
            say("iap2: no identification within 60 s");
            break;
        }
    }
    say("iap2: over (identification %s, authentication %s, %d CarPlay start(s))",
        g_l.identified ? "accepted" : "not accepted", g_l.authenticated ? "succeeded" : "not succeeded",
        g_l.carplay_sent);
    flag_clear("/fs/ram3/nbt-aa-media");
    return g_l.authenticated ? 0 : -1;
}
