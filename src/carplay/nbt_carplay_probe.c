/* nbt_carplay_probe: first steps towards wired CarPlay on the NBT (2026-11-23).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 * (license and the section 7 permission for the head unit's libraries: NOTICE.md, "GPLv3 parts")
 *
 *   nbt_carplay_probe acp           the car's Apple authentication chip: version, certificate, one test signature
 *   nbt_carplay_probe usb           wait for an iPhone, print what it offers over USB (changes nothing)
 *   nbt_carplay_probe usb carplay   the same, then ask the iPhone for its CarPlay USB configuration and print what
 *                                   it offers after that (the iPhone re-connects; unplug it to undo)
 *   nbt_carplay_probe lockdown      CarPlay configuration, the USB multiplexor in it, a connection to the iPhone's
 *                                   lockdown service and a few questions that need no pairing (no "Trust" prompt)
 *   nbt_carplay_probe carkit        pairing (the iPhone asks "Trust"; record in /fs/ram3/carplay-pair), a TLS
 *                                   session, the service com.apple.carkit.service, what it sends first
 *
 * The chip: I2C bus 0, dev-i2c node /dev/i2c/i2c_0/acp (/opt/sys/etc/i2c-conf_x86.cfg). A plain read from a fresh
 * open returns the registers from 0 (car 2026-11-23, dd). How this driver addresses other registers is not known, so
 * "acp" first tries two ways on register 4 (device id, 00 00 02 00) and keeps the one that works: (1) write the
 * register number, wait, read (raw I2C, as LIVI / xcertplay; car 2026-11-23: write and an immediate read gave
 * Input/output error 20 times); (2) the file offset as the register number (pread / pwrite). The chip sleeps between
 * transfers and may not answer the first one: transfers are retried. Run "acp" without an iPhone plugged in,
 * so the stock iPod stack does not use the chip at the same time. The certificate goes to /fs/ram3/acp-cert.der.
 *
 * USB, as in LIVI / xcertplay (GPL-3.0, github.com/shilapi/xcertplay): a vendor request 0x52 (IN, value 0, index 4,
 * 1 byte) makes the iPhone re-connect with an extra configuration that has the Apple USB multiplexor (ff/fe/02) and
 * a CDC NCM network function (02/0d): the configuration wired CarPlay runs over when the car's USB port can only be
 * the host (as the NBT's). Here it is only asked for and printed; nothing is selected or opened on it.
 */
#include <sys/usbdi.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"

#define ACP_NODE "/dev/i2c/i2c_0/acp"
#define CERT_FILE "/fs/ram3/acp-cert.der"
#define APPLE 0x05ac

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
    struct timespec ts;
    va_list ap;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("%5ld.%03ld ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void hex(const char* what, const unsigned char* b, unsigned n)
{
    unsigned i;
    printf("      %s (%u bytes):", what, n);
    for (i = 0; i < n; ++i)
        printf("%s%02x", i % 32 ? " " : "\n        ", b[i]);
    putchar('\n');
}

/* ---- the authentication chip ---- */

static int g_acp = -1;

/* one transfer, retried while the chip sleeps (it does not acknowledge the first one) */
static int g_offsets;   /* 1: the file offset is the register number (pread / pwrite) */

static int acp_write(const unsigned char* data, unsigned n)
{
    int tries;
    for (tries = 0; tries < 50; ++tries) {
        if (write(g_acp, data, n) == (ssize_t)n)
            return 0;
        usleep(1000);
    }
    say("acp: write of register 0x%02x failed: %s", data[0], strerror(errno));
    return -1;
}

/* way 1: the register number written, then read after a pause (retried without writing it again) */
static int acp_read_raw(unsigned reg, unsigned char* out, unsigned n)
{
    const unsigned char r = (unsigned char)reg;
    int tries, k;
    for (tries = 0; tries < 5; ++tries) {
        if (acp_write(&r, 1) != 0)
            return -1;
        for (k = 0; k < 20; ++k) {
            usleep(1000);
            if (read(g_acp, out, n) == (ssize_t)n)
                return 0;
        }
    }
    return -1;
}

/* way 2: the file offset as the register number */
static int acp_read_offset(unsigned reg, unsigned char* out, unsigned n)
{
    int tries;
    for (tries = 0; tries < 50; ++tries) {
        if (pread(g_acp, out, n, (off_t)reg) == (ssize_t)n)
            return 0;
        usleep(1000);
    }
    return -1;
}

static int acp_read(unsigned reg, unsigned char* out, unsigned n)
{
    if ((g_offsets ? acp_read_offset : acp_read_raw)(reg, out, n) == 0)
        return 0;
    say("acp: read of register 0x%02x (%u bytes) failed: %s", reg, n, strerror(errno));
    return -1;
}

static int acp_set(unsigned reg, const unsigned char* data, unsigned n)
{
    unsigned char b[1 + 128];
    int tries;
    if (n > 128)
        return -1;
    if (g_offsets) {
        for (tries = 0; tries < 50; ++tries) {
            if (pwrite(g_acp, data, n, (off_t)reg) == (ssize_t)n)
                return 0;
            usleep(1000);
        }
        say("acp: write of register 0x%02x failed: %s", reg, strerror(errno));
        return -1;
    }
    b[0] = (unsigned char)reg;
    memcpy(b + 1, data, n);
    return acp_write(b, n + 1);
}

/* which way addresses registers here: register 4 must read 00 00 02 00 (register 0 reads 03 01 02 00) */
static int acp_find_way(unsigned char* first4)
{
    unsigned char b[4];
    ssize_t r = read(g_acp, first4, 4);
    say("acp: plain read from offset 0: %s%02x %02x %02x %02x", r == 4 ? "" : "failed, ", first4[0], first4[1],
        first4[2], first4[3]);
    if (r != 4) {
        say("acp: %s", strerror(errno));
        return -1;
    }
    memset(b, 0xee, 4);
    if (acp_read_raw(0x04, b, 4) == 0) {
        say("acp: way 1 (write the register, then read): register 4 = %02x %02x %02x %02x", b[0], b[1], b[2], b[3]);
        if (b[0] == 0 && b[1] == 0 && b[2] == 2 && b[3] == 0)
            return 0;
    } else
        say("acp: way 1 (write the register, then read): %s", strerror(errno));
    memset(b, 0xee, 4);
    if (acp_read_offset(0x04, b, 4) == 0) {
        say("acp: way 2 (file offset = register): register 4 = %02x %02x %02x %02x", b[0], b[1], b[2], b[3]);
        if (b[0] == 0 && b[1] == 0 && b[2] == 2 && b[3] == 0) {
            g_offsets = 1;
            return 0;
        }
    } else
        say("acp: way 2 (file offset = register): %s", strerror(errno));
    say("acp: neither way reads register 4 as expected; stopping");
    return -1;
}

static int run_acp(void)
{
    unsigned char b[1280], id[4], len[2];
    unsigned n, i, status = 0;
    FILE* f;
    /* a fixed test challenge: 20 bytes (a SHA-1 digest's size, what an iPhone sends to a 2.0 chip) */
    static const unsigned char challenge[20] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                                0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13};
    g_acp = open(ACP_NODE, O_RDWR);
    if (g_acp < 0) {
        say("acp: %s: %s", ACP_NODE, strerror(errno));
        return 1;
    }
    if (acp_find_way(b) || acp_read(0x04, id, 4))
        return 1;
    say("acp: device version 0x%02x (%s), firmware 0x%02x, protocol %u.%u, device id %02x %02x %02x %02x", b[0],
        b[0] == 0x03 ? "2.0B" : b[0] == 0x05 ? "2.0C" : b[0] == 0x07 ? "3.0" : "?", b[1], b[2], b[3], id[0], id[1],
        id[2], id[3]);
    if (acp_read(0x30, len, 2))
        return 1;
    n = (unsigned)len[0] << 8 | len[1];
    say("acp: certificate %u bytes", n);
    if (n == 0 || n > sizeof(b))
        return 1;
    for (i = 0; i * 128 < n; ++i)
        if (acp_read(0x31 + i, b + i * 128, n - i * 128 < 128 ? n - i * 128 : 128))
            return 1;
    hex("certificate, start", b, n < 64 ? n : 64);
    if (b[0] != 0x30)
        say("acp: this does not start like a certificate (0x30): the register reads may be wrong");
    f = fopen(CERT_FILE, "wb");
    if (f && fwrite(b, 1, n, f) == n && fclose(f) == 0)
        say("acp: certificate written to %s", CERT_FILE);
    else
        say("acp: %s could not be written", CERT_FILE);

    /* one signature: challenge length, challenge, start; then poll the status until "signature ready" (0x10) */
    len[0] = 0;
    len[1] = sizeof(challenge);
    if (acp_set(0x20, len, 2) || acp_set(0x21, challenge, sizeof(challenge)))
        return 1;
    b[0] = 1;
    if (acp_set(0x10, b, 1))
        return 1;
    for (i = 0; i < 100; ++i) {
        usleep(20000);
        if (acp_read(0x10, b, 1) == 0) {
            status = b[0];
            if ((status & 0x70) == 0x10 || (status & 0x80))
                break;
        }
    }
    say("acp: status after signing 0x%02x (%s)", status,
        (status & 0x70) == 0x10 ? "signature ready" : status & 0x80 ? "error" : "no result");
    if ((status & 0x70) != 0x10) {
        if (acp_read(0x05, b, 1) == 0)
            say("acp: error code 0x%02x", b[0]);
        return 1;
    }
    if (acp_read(0x11, len, 2))
        return 1;
    n = (unsigned)len[0] << 8 | len[1];
    if (n == 0 || n > 128 || acp_read(0x12, b, n))
        return 1;
    hex("challenge", challenge, sizeof(challenge));
    hex("signature", b, n);
    say("acp: OK");
    return 0;
}

/* ---- USB ---- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static usbd_device_instance_t g_inst[32];
static unsigned g_count, g_removed;

static void inserted(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    pthread_mutex_lock(&g_lock);
    say("usb: inserted %04x:%04x bus %u device %u generation %u, configuration %u interface %u alternate %u",
        (unsigned)d->ident.vendor, (unsigned)d->ident.device, (unsigned)d->path, (unsigned)d->devno,
        (unsigned)d->generation, (unsigned)d->config, (unsigned)d->iface, (unsigned)d->alternate);
    if (g_count < 32)
        g_inst[g_count++] = *d;
    pthread_mutex_unlock(&g_lock);
}

static void removed(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    pthread_mutex_lock(&g_lock);
    say("usb: removed %04x:%04x bus %u device %u interface %u", (unsigned)d->ident.vendor, (unsigned)d->ident.device,
        (unsigned)d->path, (unsigned)d->devno, (unsigned)d->iface);
    ++g_removed;
    pthread_mutex_unlock(&g_lock);
}

static const char* iface_name(unsigned c, unsigned s, unsigned p)
{
    if (c == 0xff && s == 0xfe && p == 0x02)
        return "Apple USB multiplexor";
    if (c == 0xff && s == 0xfd && p == 0x01)
        return "Apple USB Ethernet";
    if (c == 0xff && s == 0xf0)
        return "iAP";
    if (c == 0x02 && s == 0x0d)
        return "CDC NCM control";
    if (c == 0x0a)
        return "CDC data";
    if (c == 0x06)
        return "PTP (still image)";
    if (c == 0x01)
        return "audio";
    if (c == 0x03)
        return "HID";
    return "";
}

static const char* text(struct usbd_device* dev, unsigned index)
{
    char* s = index ? usbd_string(dev, (unsigned char)index, 0) : 0;
    return s ? s : "";
}

/* every configuration the iPhone reports, with its interfaces and endpoints */
static void describe(struct usbd_device* dev)
{
    struct usbd_desc_node *dn, *cn, *in, *en;
    usbd_device_descriptor_t* d = usbd_device_descriptor(dev, &dn);
    usbd_descriptors_t* e;
    unsigned cfg, ifc, alt, k, carplay = 0;
    if (!d) {
        say("usb: no device descriptor");
        return;
    }
    say("usb: device %04x:%04x \"%s\" \"%s\", USB %x.%02x, %u configurations", d->idVendor, d->idProduct,
        text(dev, d->iManufacturer), text(dev, d->iProduct), d->bcdUSB >> 8, d->bcdUSB & 0xff, d->bNumConfigurations);
    for (cfg = 1; cfg <= 16; ++cfg) {
        usbd_configuration_descriptor_t* c = usbd_configuration_descriptor(dev, (unsigned char)cfg, &cn);
        unsigned mux = 0, ncm = 0;
        if (!c)
            continue;
        say("usb:   configuration %u \"%s\": %u interfaces, %u mA", c->bConfigurationValue,
            text(dev, c->iConfiguration), c->bNumInterfaces, c->MaxPower * 2);
        for (ifc = 0; ifc < 16; ++ifc)
            for (alt = 0; alt < 4; ++alt) {
                usbd_interface_descriptor_t* i =
                    usbd_interface_descriptor(dev, (unsigned char)cfg, (unsigned char)ifc, (unsigned char)alt, &in);
                if (!i)
                    continue;
                printf("        interface %u alt %u: %02x/%02x/%02x %s \"%s\", endpoints", i->bInterfaceNumber,
                       i->bAlternateSetting, i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol,
                       iface_name(i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol),
                       text(dev, i->iInterface));
                for (k = 0; (e = usbd_parse_descriptors(dev, in, USB_DESC_ENDPOINT, (int)k, &en)) != 0; ++k)
                    printf(" %02x(%s %u)", e->endpoint.bEndpointAddress,
                           (e->endpoint.bmAttributes & 3) == 2 ? "bulk" : (e->endpoint.bmAttributes & 3) == 3 ? "int"
                           : (e->endpoint.bmAttributes & 3) == 1                                         ? "iso"
                                                                                                         : "ctl",
                           e->endpoint.wMaxPacketSize & 0x7ff);
                putchar('\n');
                if (i->bInterfaceClass == 0xff && i->bInterfaceSubClass == 0xfe && i->bInterfaceProtocol == 0x02)
                    mux = 1;
                if (i->bInterfaceClass == 0x02 && i->bInterfaceSubClass == 0x0d)
                    ncm = 1;
            }
        if (mux && ncm)
            carplay = cfg;
    }
    if (carplay)
        say("usb: configuration %u has the USB multiplexor and NCM: the CarPlay configuration", carplay);
    else
        say("usb: no configuration with both the USB multiplexor and NCM");
}

/* wait up to `seconds` for Apple interfaces to show up (more than `after` of them), then 1 s for the rest */
static int wait_apple(unsigned after, unsigned seconds)
{
    unsigned t, n;
    for (t = 0; t < seconds * 10; ++t) {
        pthread_mutex_lock(&g_lock);
        n = g_count;
        pthread_mutex_unlock(&g_lock);
        if (n > after) {
            sleep(1);
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

/* attach to one of the iPhone's interfaces (from `first` on); the stock iPod stack may hold some of them */
static struct usbd_device* attach_any(struct usbd_connection* conn, unsigned first, usbd_device_instance_t* used)
{
    unsigned i, n;
    pthread_mutex_lock(&g_lock);
    n = g_count;
    pthread_mutex_unlock(&g_lock);
    for (i = first; i < n; ++i) {
        struct usbd_device* dev = 0;
        usbd_device_instance_t inst;
        int rc;
        pthread_mutex_lock(&g_lock);
        inst = g_inst[i];
        pthread_mutex_unlock(&g_lock);
        rc = usbd_attach(conn, &inst, 0, &dev);
        say("usb: attach to interface %u: %s", (unsigned)inst.iface, rc == EOK ? "OK" : strerror(rc));
        if (rc == EOK) {
            *used = inst;
            return dev;
        }
    }
    return 0;
}

static int carplay_request(struct usbd_device* dev)
{
    struct usbd_desc_node* node;
    usbd_descriptors_t* d = usbd_parse_descriptors(dev, 0, USB_DESC_DEVICE, 0, &node);
    struct usbd_pipe* pipe = 0;
    struct usbd_urb* urb = usbd_alloc_urb(0);
    unsigned char* buf = (unsigned char*)usbd_alloc(8);
    _Uint32t status = 0, actual = 0;
    int rc = -1;
    if (!d || !urb || !buf || usbd_open_pipe(dev, d, &pipe) != EOK) {
        say("usb: no control pipe");
        goto out;
    }
    memset(buf, 0, 8);
    usbd_setup_vendor(urb, URB_DIR_IN | URB_SHORT_XFER_OK, 0x52,
                      USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE | USB_DIRECTION_HOST, 0, 4, buf, 1);
    rc = usbd_io(urb, pipe, 0, 0, 1000);
    usbd_urb_status(urb, &status, &actual);
    say("usb: CarPlay request (vendor 0x52, index 4): result %d, status %08x, %u byte(s) back: %02x", rc,
        (unsigned)status, (unsigned)actual, buf[0]);
out:
    if (pipe)
        usbd_close_pipe(pipe);
    if (urb)
        usbd_free_urb(urb);
    if (buf)
        usbd_free(buf);
    return rc;
}

static int run_usb(int carplay)
{
    usbd_connect_parm_t parm;
    usbd_funcs_t funcs;
    usbd_device_ident_t ident;
    struct usbd_connection* conn = 0;
    struct usbd_device* dev;
    usbd_device_instance_t used;
    unsigned before;
    int rc;
    memset(&parm, 0, sizeof(parm));
    memset(&funcs, 0, sizeof(funcs));
    ident.vendor = APPLE;
    ident.device = ident.dclass = ident.subclass = ident.protocol = USBD_CONNECT_WILDCARD;
    funcs.nentries = _USBDI_NFUNCS;
    funcs.insertion = inserted;
    funcs.removal = removed;
    parm.vusb = USB_VERSION;
    parm.vusbd = USBD_VERSION;
    parm.ident = &ident;
    parm.funcs = &funcs;
    parm.connect_wait = 1;
    rc = usbd_connect(&parm, &conn);
    if (rc != EOK) {
        say("usb: connect to the USB stack failed: %s", strerror(rc));
        return 1;
    }
    say("usb: waiting up to 60 s for an iPhone (plug it in now if it is not)");
    if (wait_apple(0, 60)) {
        say("usb: no Apple device");
        usbd_disconnect(conn);
        return 1;
    }
    dev = attach_any(conn, 0, &used);
    if (!dev) {
        say("usb: every interface is held by another driver (the stock iPod stack?)");
        usbd_disconnect(conn);
        return 1;
    }
    describe(dev);
    if (!carplay) {
        usbd_detach(dev);
        usbd_disconnect(conn);
        say("usb: done");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    before = g_count;
    pthread_mutex_unlock(&g_lock);
    rc = carplay_request(dev);
    usbd_detach(dev);
    say("usb: waiting up to 15 s for the iPhone to re-connect");
    if (wait_apple(before, 15)) {
        say("usb: it did not re-connect (%u removal(s) seen)", g_removed);
        usbd_disconnect(conn);
        return 1;
    }
    dev = attach_any(conn, before, &used);
    if (dev) {
        describe(dev);
        usbd_detach(dev);
    }
    usbd_disconnect(conn);
    say("usb: done (unplug the iPhone to bring it back to normal)");
    return 0;
}

/* ---- lockdown over the USB multiplexor (usbmux version 2, as LIVI / xcertplay) ----
 * Car 2026-11-23: after the vendor request the iPhone 15 Pro Max offers 6 configurations; 5 and 6 have the multiplexor
 * (ff/fe/02, bulk 04 out / 85 in) and NCM; 6 also Apple USB Ethernet. Frames: big-endian {protocol, length (whole
 * frame), magic 0xfeedface, our sequence (16 bit), the last sequence received (16 bit)}, then the payload. Start:
 * {0, 20, 2, 0, 0} raw (version 2), the iPhone answers the same; then a "setup" frame (protocol 2, one byte 7).
 * Protocol 6 carries a small TCP: a 20-byte header {source port, destination port, seq, ack, 0x50, flags, window 512,
 * 0, 0}; lockdown listens on 62078. Lockdown messages: a 4-byte big-endian length, then an XML plist. */

enum { MUX_TCP = 6, TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_ACK = 0x10, LOCKDOWN_PORT = 62078 };

static struct {
    struct usbd_pipe *in, *out;
    struct usbd_urb *uin, *uout;
    unsigned char *bin, *bout;
    unsigned char rx[70000];
    unsigned rxn;
    unsigned seq, ack;   /* multiplexor frame counters */
    unsigned next_port;  /* our next TCP source port */
} g_mux;

/* one TCP connection over the multiplexor (lockdown, a service) */
struct tcpc {
    unsigned sport, dport, tseq, tack, connected, closed;
    unsigned char data[131072];   /* received, not yet taken */
    unsigned datan;
};
static struct tcpc g_tc[3];

static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
static unsigned get16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned get32(const unsigned char* p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

/* one bulk transfer, waited for: usbd_io queues it and returns at once (car 2026-11-23: a send "succeeded" with 0
 * bytes, status 0); the completion callback says when it is done. Returns usbd_io's result or ETIMEDOUT. */
struct xfer_done {
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
    _Uint32t status, actual;
};
static void xfer_cb(struct usbd_urb* urb, struct usbd_pipe* pipe, void* arg)
{
    struct xfer_done* x = (struct xfer_done*)arg;
    (void)pipe;
    pthread_mutex_lock(&x->m);
    usbd_urb_status(urb, &x->status, &x->actual);
    x->done = 1;
    pthread_cond_signal(&x->c);
    pthread_mutex_unlock(&x->m);
}
static int xfer_wait(struct xfer_done* x, unsigned ms)
{
    struct timespec t;
    int r = 0, done;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&x->m);
    while (!x->done && (r == 0 || r == EINTR))
        r = pthread_cond_timedwait(&x->c, &x->m, &t);
    done = x->done;
    pthread_mutex_unlock(&x->m);
    return done;
}
static int bulk(struct usbd_urb* urb, struct usbd_pipe* pipe, unsigned char* buf, unsigned n, int in, unsigned ms,
                _Uint32t* status, _Uint32t* actual)
{
    static struct xfer_done x = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};
    int rc;
    x.done = 0;
    x.status = x.actual = 0;
    usbd_setup_bulk(urb, in ? URB_DIR_IN | URB_SHORT_XFER_OK : URB_DIR_OUT, buf, n);
    rc = usbd_io(urb, pipe, xfer_cb, &x, ms);
    if (rc == EOK && !xfer_wait(&x, ms + 1000)) {
        usbd_abort_pipe(pipe);
        if (!xfer_wait(&x, 1000)) {   /* the stack still owns the buffer: do not touch it again */
            say("usb: a transfer did not end after its abort; stopping");
            _exit(3);
        }
        rc = ETIMEDOUT;
    }
    *status = x.status;
    *actual = x.actual;
    return rc;
}

static int mux_send_raw(const unsigned char* data, unsigned n)
{
    _Uint32t status = 0, actual = 0;
    int rc;
    if (n > 16384)
        return -1;
    memcpy(g_mux.bout, data, n);
    rc = bulk(g_mux.uout, g_mux.out, g_mux.bout, n, 0, 2000, &status, &actual);
    if (rc != EOK || actual != n) {
        say("mux: send of %u bytes failed: result %d, status %08x, %u sent", n, rc, (unsigned)status, (unsigned)actual);
        return -1;
    }
    return 0;
}

static int mux_send(unsigned protocol, const unsigned char* payload, unsigned n)
{
    static unsigned char f[16 + 16384];
    if (n > 16384 - 16)
        return -1;
    put32(f, protocol);
    put32(f + 4, 16 + n);
    put32(f + 8, 0xfeedfaceu);
    put16(f + 12, g_mux.seq);
    put16(f + 14, g_mux.ack);
    memcpy(f + 16, payload, n);
    g_mux.seq = (g_mux.seq + 1) & 0xffff;
    return mux_send_raw(f, 16 + n);
}

/* one whole frame into f (its length in *n), waiting up to ms; 1 = got one, 0 = timeout, -1 = error */
static int mux_frame(unsigned char* f, unsigned* n, unsigned ms)
{
    long long until = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    until = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000 + ms;
    for (;;) {
        if (g_mux.rxn >= 8) {
            const unsigned len = get32(g_mux.rx + 4);
            if (len < 8 || len > sizeof(g_mux.rx)) {
                say("mux: bad frame length %u", len);
                return -1;
            }
            if (g_mux.rxn >= len) {
                memcpy(f, g_mux.rx, len);
                *n = len;
                memmove(g_mux.rx, g_mux.rx + len, g_mux.rxn - len);
                g_mux.rxn -= len;
                if (len >= 16)
                    g_mux.ack = get16(f + 12);
                return 1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        if (now >= until)
            return 0;
        _Uint32t status = 0, actual = 0;
        const unsigned left = (unsigned)(until - now);
        const int rc = bulk(g_mux.uin, g_mux.in, g_mux.bin, 16384, 1, left < 200 ? 200 : left > 1000 ? 1000 : left,
                            &status, &actual);
        if (actual == 0) {
            if (rc == ETIMEDOUT || rc == EOK)   /* nothing within the time (a timed-out transfer ends with a status) */
                continue;
            say("mux: read failed: result %d, status %08x", rc, (unsigned)status);
            return -1;
        }
        if (actual > sizeof(g_mux.rx) - g_mux.rxn)
            return -1;
        memcpy(g_mux.rx + g_mux.rxn, g_mux.bin, actual);
        g_mux.rxn += actual;
    }
}

static long long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int tcp_send(struct tcpc* c, unsigned flags, const unsigned char* payload, unsigned n)
{
    static unsigned char t[20 + 16000];
    if (n > 16000)
        return -1;
    memset(t, 0, 20);
    put16(t, c->sport);
    put16(t + 2, c->dport);
    put32(t + 4, c->tseq);
    put32(t + 8, c->tack);
    t[12] = 0x50;
    t[13] = (unsigned char)flags;
    put16(t + 14, 512);
    if (n)
        memcpy(t + 20, payload, n);
    if (mux_send(MUX_TCP, t, 20 + n))
        return -1;
    c->tseq += n;
    return 0;
}

/* one frame, handed to its connection; 1 = a frame, 0 = timeout, -1 = the multiplexor failed */
static int tcp_poll(unsigned ms)
{
    static unsigned char f[70000];
    unsigned n, k;
    const int r = mux_frame(f, &n, ms);
    if (r <= 0)
        return r;
    if (get32(f) != MUX_TCP || n < 16 + 20) {
        say("mux: frame protocol %u, %u bytes (ignored)", get32(f), n);
        return 1;
    }
    const unsigned char* t = f + 16;
    const unsigned hl = (t[12] >> 4) * 4u, flags = t[13], port = get16(t + 2);
    struct tcpc* c = 0;
    for (k = 0; k < sizeof(g_tc) / sizeof(g_tc[0]); ++k)
        if (g_tc[k].sport == port && !g_tc[k].closed)
            c = &g_tc[k];
    if (!c || hl < 20 || 16 + hl > n)
        return 1;   /* an old connection's leftovers (car 2026-11-23: one before the version answer) */
    if (flags & TCP_RST) {
        say("tcp %u: reset by the iPhone", c->dport);
        c->closed = 2;
        return 1;
    }
    if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
        c->tseq += 1;
        c->tack = get32(t + 4) + 1;
        c->connected = 1;
        tcp_send(c, TCP_ACK, 0, 0);
        return 1;
    }
    const unsigned len = n - 16 - hl;
    if (len) {
        if (len > sizeof(c->data) - c->datan) {
            say("tcp %u: receive buffer full", c->dport);
            c->closed = 2;
            return 1;
        }
        memcpy(c->data + c->datan, t + hl, len);
        c->datan += len;
        c->tack += len;
        tcp_send(c, TCP_ACK, 0, 0);
    }
    if (flags & TCP_FIN) {
        c->tack += 1;
        tcp_send(c, TCP_ACK, 0, 0);
        c->closed = 1;
    }
    return 1;
}

static struct tcpc* tcp_open(unsigned slot, unsigned port)
{
    struct tcpc* c = &g_tc[slot];
    memset(c, 0, sizeof(*c));
    c->sport = g_mux.next_port++;
    c->dport = port;
    if (tcp_send(c, TCP_SYN, 0, 0))
        return 0;
    const long long until = ms_now() + 5000;
    while (!c->connected && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return 0;
    if (!c->connected) {
        say("tcp: port %u %s", port, c->closed ? "refused" : "did not answer");
        c->closed = 2;
        return 0;
    }
    return c;
}

static void tcp_close(struct tcpc* c)
{
    if (c && c->connected && !c->closed)
        tcp_send(c, TCP_FIN | TCP_ACK, 0, 0);
    if (c)
        c->closed = 1;
}

/* exactly n bytes of the connection's data, waiting up to ms */
static int tcp_take(struct tcpc* c, unsigned char* out, unsigned n, unsigned ms)
{
    const long long until = ms_now() + ms;
    while (c->datan < n && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return -1;
    if (c->datan < n)
        return -1;
    memcpy(out, c->data, n);
    memmove(c->data, c->data + n, c->datan - n);
    c->datan -= n;
    return 0;
}

/* ---- a byte stream: a TCP connection, TLS on it once a session or service asks for it ---- */

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

struct stream {
    struct tcpc* c;
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    struct tcpc* c = (struct tcpc*)ctx;
    const unsigned n = len > 16000 ? 16000 : (unsigned)len;
    if (c->closed)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return tcp_send(c, TCP_ACK, buf, n) ? MBEDTLS_ERR_NET_SEND_FAILED : (int)n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    struct tcpc* c = (struct tcpc*)ctx;
    const long long until = ms_now() + 8000;
    unsigned n;
    while (!c->datan && !c->closed && ms_now() < until)
        if (tcp_poll(200) < 0)
            return MBEDTLS_ERR_NET_RECV_FAILED;
    if (!c->datan)
        return c->closed ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_SSL_TIMEOUT;
    n = c->datan < len ? c->datan : (unsigned)len;
    memcpy(buf, c->data, n);
    memmove(c->data, c->data + n, c->datan - n);
    c->datan -= n;
    return (int)n;
}

static void tls_error(const char* what, int r)
{
    char e[160];
    mbedtls_strerror(r, e, sizeof(e));
    say("tls: %s: -0x%04x %s", what, (unsigned)-r, e);
}

/* TLS on the stream as lockdown wants it: the pair record's root certificate and key as our identity, the iPhone's
 * certificate not checked (as libimobiledevice and xcertplay) */
static int stream_tls(struct stream* s, mbedtls_x509_crt* cert, mbedtls_pk_context* key)
{
    int r;
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) {
        tls_error("config", r);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    r = mbedtls_ssl_conf_own_cert(&s->conf, cert, key);
    if (!r)
        r = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (r) {
        tls_error("setup", r);
        return -1;
    }
    mbedtls_ssl_set_bio(&s->ssl, s->c, bio_send, bio_recv, 0);
    while ((r = mbedtls_ssl_handshake(&s->ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_error("handshake", r);
            return -1;
        }
    s->tls = 1;
    say("tls: %s, %s", mbedtls_ssl_get_version(&s->ssl), mbedtls_ssl_get_ciphersuite(&s->ssl));
    return 0;
}

static int stream_write(struct stream* s, const unsigned char* b, unsigned n)
{
    while (n) {
        int r;
        if (!s->tls) {
            const unsigned k = n > 16000 ? 16000 : n;
            if (tcp_send(s->c, TCP_ACK, b, k))
                return -1;
            b += k;
            n -= k;
            continue;
        }
        r = mbedtls_ssl_write(&s->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) {
            tls_error("write", r);
            return -1;
        }
        b += r;
        n -= (unsigned)r;
    }
    return 0;
}

static int stream_read(struct stream* s, unsigned char* b, unsigned n, unsigned ms)
{
    if (!s->tls)
        return tcp_take(s->c, b, n, ms);
    while (n) {
        const int r = mbedtls_ssl_read(&s->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) {
            if (r != MBEDTLS_ERR_SSL_TIMEOUT)
                tls_error("read", r);
            return -1;
        }
        b += r;
        n -= (unsigned)r;
    }
    return 0;
}

/* ---- lockdown: plist requests ---- */

static char g_answer[65536];

/* the value after <key>key</key>: a string, integer or data element's text, or "true" / "false" */
static int plist_value(const char* xml, const char* key, char* out, unsigned size)
{
    char k[96];
    const char *p, *e;
    snprintf(k, sizeof(k), "<key>%s</key>", key);
    p = strstr(xml, k);
    if (!p)
        return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
        ++p;
    if (!strncmp(p, "<true/>", 7) || !strncmp(p, "<false/>", 8)) {
        snprintf(out, size, "%s", p[1] == 't' ? "true" : "false");
        return 1;
    }
    p = strchr(p, '>');
    if (!p)
        return 0;
    ++p;
    e = strchr(p, '<');
    if (!e || (unsigned)(e - p) >= size)
        return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}

/* one request (the dictionary's entries after Label), its answer in g_answer, printed shortened */
static int lockdown_request(struct stream* s, const char* body)
{
    static char msg[16384];
    static const char head[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD "
                               "PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist "
                               "version=\"1.0\">\n<dict>\n<key>Label</key>\n<string>nbt</string>\n";
    unsigned char len[4];
    unsigned n, i, j;
    const int m = snprintf(msg + 4, sizeof(msg) - 4, "%s%s</dict>\n</plist>\n", head, body);
    if (m <= 0 || m >= (int)sizeof(msg) - 4)
        return -1;
    put32((unsigned char*)msg, (unsigned)m);
    if (stream_write(s, (const unsigned char*)msg, 4 + (unsigned)m) || stream_read(s, len, 4, 10000)) {
        say("lockdown: no answer");
        return -1;
    }
    n = get32(len);
    if (n >= sizeof(g_answer) || stream_read(s, (unsigned char*)g_answer, n, 10000)) {
        say("lockdown: answer of %u bytes not read", n);
        return -1;
    }
    g_answer[n] = 0;
    {   /* printed: from <dict> on, on one line, long values cut */
        const char* p = strstr(g_answer, "<dict>");
        char line[700];
        for (i = 0, j = 0; p && p[i] && j < sizeof(line) - 1; ++i)
            if (p[i] != '\n' && p[i] != '\t' && p[i] != '\r')
                line[j++] = p[i];
        line[j] = 0;
        printf("      answer: %s%s\n", line, p && p[i] ? " ..." : "");
    }
    return 0;
}

static int lockdown_error(void)
{
    char e[128];
    if (!plist_value(g_answer, "Error", e, sizeof(e)))
        return 0;
    say("lockdown: error %s", e);
    return 1;
}

/* ---- the pair record (RAM, /fs/ram3/carplay-pair: the iPhone asks "Trust" again after each boot of the car) ----
 * As libimobiledevice / xcertplay make it: a root key and certificate (CA), a host key and certificate and a
 * certificate for the iPhone's own key (DevicePublicKey), all RSA-2048, empty names, serial 0, SHA-256, signed by the
 * root; HostID and SystemBUID are UUIDs. TLS with lockdown uses the root certificate and key. */
#define PAIR_DIR "/fs/ram3/carplay-pair"

static struct {
    char host_id[40], buid[40];
    char root_key[4096], root_cert[4096], host_key[4096], host_cert[4096], device_cert[4096];
    int have;
} g_pair;

static int file_get(const char* name, char* out, unsigned size)
{
    char path[128];
    FILE* f;
    size_t n;
    snprintf(path, sizeof(path), PAIR_DIR "/%s", name);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return n ? 0 : -1;
}

static int file_put(const char* name, const char* data, unsigned n)
{
    char path[128];
    FILE* f;
    snprintf(path, sizeof(path), PAIR_DIR "/%s", name);
    f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        if (f)
            fclose(f);
        return -1;
    }
    return fclose(f);
}

static int pair_load(void)
{
    g_pair.have = !file_get("host_id", g_pair.host_id, sizeof(g_pair.host_id)) &&
                  !file_get("buid", g_pair.buid, sizeof(g_pair.buid)) &&
                  !file_get("root_key.pem", g_pair.root_key, sizeof(g_pair.root_key)) &&
                  !file_get("root_cert.pem", g_pair.root_cert, sizeof(g_pair.root_cert)) &&
                  !file_get("paired", g_answer, 16);
    return g_pair.have ? 0 : -1;
}

static void uuid(char* out)
{
    unsigned char r[16];
    mbedtls_ctr_drbg_random(&g_drbg, r, sizeof(r));
    r[6] = (unsigned char)((r[6] & 0x0f) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3f) | 0x80);
    sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", r[0], r[1], r[2], r[3], r[4],
            r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* one certificate: subject's key, signed by the root key; ca = the root's own, ski = with a subject key identifier */
static int make_cert(mbedtls_pk_context* subject, mbedtls_pk_context* root, int ca, int ski, char* pem, unsigned size)
{
    mbedtls_x509write_cert crt;
    static const unsigned char serial[1] = {0};
    static const unsigned char bc_ca[] = {0x30, 0x03, 0x01, 0x01, 0xff}, bc_leaf[] = {0x30, 0x00};
    static const unsigned char ku[] = {0x03, 0x02, 0x05, 0xa0};   /* digitalSignature, keyEncipherment */
    int r;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, subject);
    mbedtls_x509write_crt_set_issuer_key(&crt, root);
    r = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial, 1);
    if (!r)
        r = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
    if (!r)
        r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_BASIC_CONSTRAINTS,
                                                MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS), 1,
                                                ca ? bc_ca : bc_leaf, ca ? sizeof(bc_ca) : sizeof(bc_leaf));
    if (!r && ski) {
        unsigned char der[600], v[22];
        unsigned char* end = der + sizeof(der);
        const int n = mbedtls_pk_write_pubkey(&end, der, subject);   /* PKCS#1, written backwards from the end */
        if (n <= 0)
            r = -1;
        else {
            v[0] = 0x04;
            v[1] = 20;
            r = mbedtls_sha1(der + sizeof(der) - n, (size_t)n, v + 2);
            if (!r)
                r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER,
                                                        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_KEY_IDENTIFIER), 0, v,
                                                        sizeof(v));
        }
    }
    if (!r && !ca)
        r = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_KEY_USAGE),
                                                1, ku, sizeof(ku));
    if (!r)
        r = mbedtls_x509write_crt_pem(&crt, (unsigned char*)pem, size, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_x509write_crt_free(&crt);
    if (r)
        tls_error("certificate", r);
    return r ? -1 : 0;
}

static int gen_key(mbedtls_pk_context* k, char* pem, unsigned size)
{
    const long long t0 = ms_now();
    int r = mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (!r)
        r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*k), mbedtls_ctr_drbg_random, &g_drbg, 2048, 65537);
    if (!r && pem)
        r = mbedtls_pk_write_key_pem(k, (unsigned char*)pem, size);
    if (r)
        tls_error("RSA key", r);
    else
        say("pair: RSA-2048 key made in %lld ms", ms_now() - t0);
    return r ? -1 : 0;
}

/* the record for the iPhone's key (PEM, PKCS#1) */
static int pair_make(const char* device_pem)
{
    mbedtls_pk_context root, host, device;
    int r = -1;
    mbedtls_pk_init(&root);
    mbedtls_pk_init(&host);
    mbedtls_pk_init(&device);
    if (mbedtls_pk_parse_public_key(&device, (const unsigned char*)device_pem, strlen(device_pem) + 1)) {
        say("pair: the iPhone's key could not be read");
        goto out;
    }
    if (gen_key(&root, g_pair.root_key, sizeof(g_pair.root_key)) ||
        gen_key(&host, g_pair.host_key, sizeof(g_pair.host_key)) ||
        make_cert(&root, &root, 1, 0, g_pair.root_cert, sizeof(g_pair.root_cert)) ||
        make_cert(&host, &root, 0, 0, g_pair.host_cert, sizeof(g_pair.host_cert)) ||
        make_cert(&device, &root, 0, 1, g_pair.device_cert, sizeof(g_pair.device_cert)))
        goto out;
    uuid(g_pair.host_id);
    r = 0;
out:
    mbedtls_pk_free(&root);
    mbedtls_pk_free(&host);
    mbedtls_pk_free(&device);
    return r;
}

static void pem_base64(const char* pem, char* out, unsigned size)
{
    size_t n = 0;
    if (mbedtls_base64_encode((unsigned char*)out, size, &n, (const unsigned char*)pem, strlen(pem)))
        n = 0;
    out[n] = 0;
}

/* lockdown on a new connection (slot 0) */
static int lockdown_connect(struct stream* s)
{
    memset(s, 0, sizeof(*s));
    s->c = tcp_open(0, LOCKDOWN_PORT);
    return s->c ? 0 : -1;
}

/* pairing: the iPhone asks "Trust this computer?"; until it is answered, Pair says PairingDialogResponsePending and
 * is asked again (a new connection each time, as xcertplay), for up to 2 minutes */
static int pair(void)
{
    struct stream s;
    char value[4096], device_pem[2048], body[16384], dev64[3000], host64[3000], root64[3000];
    size_t n = 0;
    long long until;
    if (!g_pair.buid[0])
        uuid(g_pair.buid);
    if (lockdown_connect(&s))
        return -1;
    snprintf(body, sizeof(body),
             "<key>Request</key>\n<string>SetValue</string>\n<key>Key</key>\n<string>UntrustedHostBUID</string>\n"
             "<key>Value</key>\n<string>%s</string>\n",
             g_pair.buid);
    if (lockdown_request(&s, body))
        return -1;
    if (lockdown_request(&s, "<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n"
                             "<string>DevicePublicKey</string>\n") ||
        !plist_value(g_answer, "Value", value, sizeof(value)) ||
        mbedtls_base64_decode((unsigned char*)device_pem, sizeof(device_pem) - 1, &n, (const unsigned char*)value,
                              strlen(value))) {
        say("pair: no DevicePublicKey");
        return -1;
    }
    device_pem[n] = 0;
    tcp_close(s.c);
    say("pair: making the pair record (two RSA-2048 keys)");
    if (pair_make(device_pem))
        return -1;
    pem_base64(g_pair.device_cert, dev64, sizeof(dev64));
    pem_base64(g_pair.host_cert, host64, sizeof(host64));
    pem_base64(g_pair.root_cert, root64, sizeof(root64));
    snprintf(body, sizeof(body),
             "<key>PairRecord</key>\n<dict>\n<key>DeviceCertificate</key>\n<data>%s</data>\n"
             "<key>HostCertificate</key>\n<data>%s</data>\n<key>HostID</key>\n<string>%s</string>\n"
             "<key>RootCertificate</key>\n<data>%s</data>\n<key>SystemBUID</key>\n<string>%s</string>\n</dict>\n"
             "<key>Request</key>\n<string>Pair</string>\n<key>ProtocolVersion</key>\n<string>2</string>\n"
             "<key>PairingOptions</key>\n<dict>\n<key>ExtendedPairingErrors</key>\n<true/>\n</dict>\n",
             dev64, host64, g_pair.host_id, root64, g_pair.buid);
    say("pair: asking the iPhone (answer \"Trust\" on it, with the passcode if it asks)");
    until = ms_now() + 120000;
    while (ms_now() < until) {
        if (lockdown_connect(&s))
            return -1;
        if (lockdown_request(&s, body))
            return -1;
        tcp_close(s.c);
        if (!plist_value(g_answer, "Error", value, sizeof(value))) {
            say("pair: paired");
            mkdir(PAIR_DIR, 0700);
            if (file_put("host_id", g_pair.host_id, strlen(g_pair.host_id)) ||
                file_put("buid", g_pair.buid, strlen(g_pair.buid)) ||
                file_put("root_key.pem", g_pair.root_key, strlen(g_pair.root_key)) ||
                file_put("root_cert.pem", g_pair.root_cert, strlen(g_pair.root_cert)) ||
                file_put("host_key.pem", g_pair.host_key, strlen(g_pair.host_key)) ||
                file_put("host_cert.pem", g_pair.host_cert, strlen(g_pair.host_cert)) ||
                file_put("device_cert.pem", g_pair.device_cert, strlen(g_pair.device_cert)) ||
                (plist_value(g_answer, "EscrowBag", value, sizeof(value)) &&
                 file_put("escrow_bag.b64", value, strlen(value))) ||
                file_put("paired", "1\n", 2))
                say("pair: the record could not be written to %s (pairing again next time)", PAIR_DIR);
            g_pair.have = 1;
            return 0;
        }
        if (strcmp(value, "PairingDialogResponsePending")) {
            say("pair: refused: %s", value);
            return -1;
        }
        sleep(1);
    }
    say("pair: no answer on the iPhone within 2 minutes");
    return -1;
}

static int pair_identity(mbedtls_x509_crt* cert, mbedtls_pk_context* key)
{
    int r = mbedtls_x509_crt_parse(cert, (const unsigned char*)g_pair.root_cert, strlen(g_pair.root_cert) + 1);
    if (!r)
        r = mbedtls_pk_parse_key(key, (const unsigned char*)g_pair.root_key, strlen(g_pair.root_key) + 1, 0, 0,
                                 mbedtls_ctr_drbg_random, &g_drbg);
    if (r)
        tls_error("pair record", r);
    return r ? -1 : 0;
}

/* pairing if needed, a TLS session, the carkit service on its own connection (slot 1); first bytes printed */
static int run_carkit(void)
{
    struct stream s, svc;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    char body[512], value[256];
    unsigned port, i;
    int r;
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    if (pair_load() == 0)
        say("pair: record from %s (HostID %s)", PAIR_DIR, g_pair.host_id);
    else if (pair())
        return -1;
    if (pair_identity(&cert, &key) || lockdown_connect(&s))
        return -1;
    snprintf(body, sizeof(body),
             "<key>Request</key>\n<string>StartSession</string>\n<key>HostID</key>\n<string>%s</string>\n"
             "<key>SystemBUID</key>\n<string>%s</string>\n",
             g_pair.host_id, g_pair.buid);
    if (lockdown_request(&s, body) || lockdown_error())
        return -1;
    if (plist_value(g_answer, "EnableSessionSSL", value, sizeof(value)) && !strcmp(value, "true")) {
        if (stream_tls(&s, &cert, &key))
            return -1;
    } else
        say("lockdown: session without TLS");
    if (lockdown_request(&s, "<key>Request</key>\n<string>StartService</string>\n<key>Service</key>\n"
                             "<string>com.apple.carkit.service</string>\n") ||
        lockdown_error() || !plist_value(g_answer, "Port", value, sizeof(value)))
        return -1;
    port = (unsigned)atoi(value);
    const int ssl = plist_value(g_answer, "EnableServiceSSL", value, sizeof(value)) && !strcmp(value, "true");
    say("carkit: port %u%s", port, ssl ? ", TLS" : "");
    memset(&svc, 0, sizeof(svc));
    svc.c = tcp_open(1, port);
    if (!svc.c || (ssl && stream_tls(&svc, &cert, &key)))
        return -1;
    say("carkit: connected; what the iPhone sends in the first 5 s:");
    {
        unsigned char b[256];
        unsigned got = 0;
        const long long until = ms_now() + 5000;
        while (got < sizeof(b) && ms_now() < until) {
            if (ssl) {
                r = mbedtls_ssl_read(&svc.ssl, b + got, sizeof(b) - got);
                if (r > 0)
                    got += (unsigned)r;
                else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_TIMEOUT)
                    break;
            } else {
                if (svc.c->datan) {
                    const unsigned k = svc.c->datan < sizeof(b) - got ? svc.c->datan : sizeof(b) - got;
                    tcp_take(svc.c, b + got, k, 0);
                    got += k;
                } else if (tcp_poll(200) < 0 || svc.c->closed)
                    break;
            }
        }
        hex("carkit data", b, got);
    }
    for (i = 0; i < 3; ++i)
        tcp_close(&g_tc[i]);
    say("carkit: done");
    return 0;
}

/* the configuration with the multiplexor and NCM (the highest), and the multiplexor's interface in it */
static int find_carplay(struct usbd_device* dev, unsigned* cfg_out, unsigned* ifc_out)
{
    struct usbd_desc_node* node;
    unsigned cfg, ifc, found = 0;
    for (cfg = 1; cfg <= 16; ++cfg) {
        unsigned mux = 99, ncm = 0;
        if (!usbd_configuration_descriptor(dev, (unsigned char)cfg, &node))
            continue;
        for (ifc = 0; ifc < 16; ++ifc) {
            usbd_interface_descriptor_t* i =
                usbd_interface_descriptor(dev, (unsigned char)cfg, (unsigned char)ifc, 0, &node);
            if (!i)
                continue;
            if (i->bInterfaceClass == 0xff && i->bInterfaceSubClass == 0xfe && i->bInterfaceProtocol == 0x02)
                mux = ifc;
            if (i->bInterfaceClass == 0x02 && i->bInterfaceSubClass == 0x0d)
                ncm = 1;
        }
        if (mux != 99 && ncm) {
            *cfg_out = cfg;
            *ifc_out = mux;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static int open_mux_pipes(struct usbd_device* dev, unsigned cfg, unsigned ifc)
{
    struct usbd_desc_node *in_node, *ep_node;
    usbd_descriptors_t *e, *bin = 0, *bout = 0;
    int k, rc;
    if (!usbd_interface_descriptor(dev, (unsigned char)cfg, (unsigned char)ifc, 0, &in_node))
        return -1;
    for (k = 0; (e = usbd_parse_descriptors(dev, in_node, USB_DESC_ENDPOINT, k, &ep_node)) != 0; ++k)
        if ((e->endpoint.bmAttributes & 3) == 2) {
            if (e->endpoint.bEndpointAddress & 0x80)
                bin = e;
            else
                bout = e;
        }
    if (!bin || !bout)
        return -1;
    rc = usbd_open_pipe(dev, bin, &g_mux.in);
    if (rc == EOK)
        rc = usbd_open_pipe(dev, bout, &g_mux.out);
    say("mux: pipes %02x in / %02x out: %s", bin->endpoint.bEndpointAddress, bout->endpoint.bEndpointAddress,
        rc == EOK ? "open" : strerror(rc));
    return rc == EOK ? 0 : -1;
}

static int run_lockdown(int carkit)
{
    usbd_connect_parm_t parm;
    usbd_funcs_t funcs;
    usbd_device_ident_t ident;
    struct usbd_connection* conn = 0;
    struct usbd_device *dev, *dev2;
    usbd_device_instance_t used;
    struct usbd_desc_node* node;
    usbd_device_descriptor_t* d;
    unsigned before, cfg = 0, ifc = 0, i, n;
    static unsigned char f[70000];
    int rc;
    memset(&parm, 0, sizeof(parm));
    memset(&funcs, 0, sizeof(funcs));
    ident.vendor = APPLE;
    ident.device = ident.dclass = ident.subclass = ident.protocol = USBD_CONNECT_WILDCARD;
    funcs.nentries = _USBDI_NFUNCS;
    funcs.insertion = inserted;
    funcs.removal = removed;
    parm.vusb = USB_VERSION;
    parm.vusbd = USBD_VERSION;
    parm.ident = &ident;
    parm.funcs = &funcs;
    parm.connect_wait = 1;
    rc = usbd_connect(&parm, &conn);
    if (rc != EOK) {
        say("usb: connect to the USB stack failed: %s", strerror(rc));
        return 1;
    }
    say("usb: waiting up to 60 s for an iPhone (plug it in now if it is not)");
    if (wait_apple(0, 60) || !(dev = attach_any(conn, 0, &used)))
        goto fail;
    d = usbd_device_descriptor(dev, &node);
    if (find_carplay(dev, &cfg, &ifc)) {   /* not yet: ask for the CarPlay configurations */
        pthread_mutex_lock(&g_lock);
        before = g_count;
        pthread_mutex_unlock(&g_lock);
        carplay_request(dev);
        usbd_detach(dev);
        if (wait_apple(before, 15) || !(dev = attach_any(conn, before, &used)) || find_carplay(dev, &cfg, &ifc)) {
            say("usb: no CarPlay configuration after the request");
            goto fail;
        }
        d = usbd_device_descriptor(dev, &node);
    }
    say("usb: %u configurations; CarPlay configuration %u, multiplexor interface %u", d ? d->bNumConfigurations : 0,
        cfg, ifc);
    /* the multiplexor's pipes must be opened through its own interface's attachment (car 2026-11-23: through
     * interface 0's, every transfer failed with ENOENT). After the configuration is selected the USB stack reports
     * its interfaces (on the next connection at the latest: the iPhone stays in it until unplugged). */
    if (used.config != cfg) {
        pthread_mutex_lock(&g_lock);
        before = g_count;
        pthread_mutex_unlock(&g_lock);
        rc = usbd_select_config(dev, (unsigned char)cfg);
        say("usb: select configuration %u: %s", cfg, rc == EOK ? "OK" : strerror(rc));
    } else
        before = 0;
    dev2 = 0;
    for (i = 0; i < 50 && !dev2; ++i) {
        unsigned k;
        pthread_mutex_lock(&g_lock);
        n = g_count;
        pthread_mutex_unlock(&g_lock);
        for (k = before; k < n && !dev2; ++k) {
            usbd_device_instance_t inst;
            pthread_mutex_lock(&g_lock);
            inst = g_inst[k];
            pthread_mutex_unlock(&g_lock);
            if (inst.iface != ifc || inst.config != cfg)
                continue;
            rc = usbd_attach(conn, &inst, 0, &dev2);
            say("usb: attach to interface %u of configuration %u: %s", (unsigned)inst.iface, (unsigned)inst.config,
                rc == EOK ? "OK" : strerror(rc));
            if (rc != EOK)
                dev2 = 0;
        }
        if (!dev2)
            usleep(100000);
    }
    if (!dev2) {
        say("usb: the multiplexor interface was not reported yet: run the command again (the iPhone stays in "
            "configuration %u until unplugged)", cfg);
        goto fail;
    }
    if (open_mux_pipes(dev2, cfg, ifc))
        goto fail;
    g_mux.uin = usbd_alloc_urb(0);
    g_mux.uout = usbd_alloc_urb(0);
    g_mux.bin = (unsigned char*)usbd_alloc(16384);
    g_mux.bout = (unsigned char*)usbd_alloc(16384);
    if (!g_mux.uin || !g_mux.uout || !g_mux.bin || !g_mux.bout)
        goto fail;

    {   /* version 2 */
        unsigned char v[20];
        memset(v, 0, sizeof(v));
        put32(v + 4, 20);
        put32(v + 8, 2);
        if (mux_send_raw(v, 20))
            goto fail;
        for (;;) {
            rc = mux_frame(f, &n, 5000);
            if (rc <= 0) {
                say("mux: no version answer");
                goto fail;
            }
            if (get32(f) == 0 && n == 20) {
                say("mux: version %u accepted", get32(f + 8));
                break;
            }
            say("mux: frame protocol %u, %u bytes before the version answer (ignored)", get32(f), n);
        }
        const unsigned char setup = 7;
        if (mux_send(2, &setup, 1))
            goto fail;
    }
    g_mux.next_port = 1 + (unsigned)(time(0) % 1000) * 16;
    if (carkit) {
        rc = run_carkit();
    } else {
        struct stream s;
        rc = lockdown_connect(&s);
        if (!rc) {
            say("lockdown: connected");
            say("lockdown: QueryType");
            lockdown_request(&s, "<key>Request</key>\n<string>QueryType</string>\n");
            say("lockdown: GetValue DeviceName");
            lockdown_request(&s, "<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>DeviceName</string>\n");
            say("lockdown: GetValue ProductVersion");
            lockdown_request(&s, "<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>ProductVersion</string>\n");
            say("lockdown: GetValue ProductType");
            lockdown_request(&s, "<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>ProductType</string>\n");
            tcp_close(s.c);
        }
    }
    usbd_disconnect(conn);
    say("%s: %s (unplug the iPhone to bring it back to normal)", carkit ? "carkit" : "lockdown", rc ? "stopped" : "done");
    return rc ? 1 : 0;
fail:
    usbd_disconnect(conn);
    say("%s: stopped", carkit ? "carkit" : "lockdown");
    return 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    alarm(120);
    if (argc == 2 && !strcmp(argv[1], "acp"))
        return run_acp();
    if (argc == 2 && !strcmp(argv[1], "usb"))
        return run_usb(0);
    if (argc == 3 && !strcmp(argv[1], "usb") && !strcmp(argv[2], "carplay"))
        return run_usb(1);
    if (argc == 2 && !strcmp(argv[1], "lockdown"))
        return run_lockdown(0);
    if (argc == 2 && !strcmp(argv[1], "carkit")) {
        int r;
        alarm(300);   /* the "Trust" answer may take a while */
        mbedtls_entropy_init(&g_entropy);
        mbedtls_ctr_drbg_init(&g_drbg);
        psa_crypto_init();
        r = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)"nbt-carplay", 11);
        if (r) {
            tls_error("random", r);
            return 1;
        }
        return run_lockdown(1);
    }
    fprintf(stderr, "usage: nbt_carplay_probe acp | usb | usb carplay | lockdown | carkit\n");
    return 2;
}
