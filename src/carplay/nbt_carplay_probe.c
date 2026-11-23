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
    unsigned seq, ack;                       /* multiplexor frame counters */
    unsigned tseq, tack, sport, connected;   /* the TCP connection */
    unsigned char data[16384];               /* TCP payload received, not yet taken */
    unsigned datan;
} g_mux;

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

static int mux_send_raw(const unsigned char* data, unsigned n)
{
    _Uint32t status = 0, actual = 0;
    int rc;
    if (n > 16384)
        return -1;
    memcpy(g_mux.bout, data, n);
    usbd_setup_bulk(g_mux.uout, URB_DIR_OUT, g_mux.bout, n);
    rc = usbd_io(g_mux.uout, g_mux.out, 0, 0, 2000);
    usbd_urb_status(g_mux.uout, &status, &actual);
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
        usbd_setup_bulk(g_mux.uin, URB_DIR_IN | URB_SHORT_XFER_OK, g_mux.bin, 16384);
        const int rc = usbd_io(g_mux.uin, g_mux.in, 0, 0, (unsigned)(until - now < 200 ? 200 : until - now));
        usbd_urb_status(g_mux.uin, &status, &actual);
        if (rc != EOK && actual == 0) {
            if (rc == ETIMEDOUT)
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

static int tcp_send(unsigned flags, const unsigned char* payload, unsigned n)
{
    static unsigned char t[20 + 16000];
    if (n > 16000)
        return -1;
    memset(t, 0, 20);
    put16(t, g_mux.sport);
    put16(t + 2, LOCKDOWN_PORT);
    put32(t + 4, g_mux.tseq);
    put32(t + 8, g_mux.tack);
    t[12] = 0x50;
    t[13] = (unsigned char)flags;
    put16(t + 14, 512);
    memcpy(t + 20, payload, n);
    if (mux_send(MUX_TCP, t, 20 + n))
        return -1;
    g_mux.tseq += n;
    return 0;
}

/* frames until one TCP segment for us has been handled; 1 = handled, 0 = timeout, -1 = error / reset */
static int tcp_poll(unsigned ms)
{
    static unsigned char f[70000];
    unsigned n;
    for (;;) {
        const int r = mux_frame(f, &n, ms);
        if (r <= 0)
            return r;
        const unsigned proto = get32(f);
        if (proto != MUX_TCP || n < 16 + 20) {
            say("mux: frame protocol %u, %u bytes (ignored)", proto, n);
            continue;
        }
        const unsigned char* t = f + 16;
        const unsigned hl = (t[12] >> 4) * 4, flags = t[13];
        if (get16(t + 2) != g_mux.sport || hl < 20 || 16 + hl > n)
            continue;
        if (flags & TCP_RST) {
            say("tcp: reset by the iPhone");
            return -1;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            g_mux.tseq += 1;
            g_mux.tack = get32(t + 4) + 1;
            g_mux.connected = 1;
            tcp_send(TCP_ACK, 0, 0);
            return 1;
        }
        const unsigned len = n - 16 - hl;
        if (len) {
            if (len > sizeof(g_mux.data) - g_mux.datan)
                return -1;
            memcpy(g_mux.data + g_mux.datan, t + hl, len);
            g_mux.datan += len;
            g_mux.tack += len;
            tcp_send(TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            g_mux.tack += 1;
            tcp_send(TCP_ACK, 0, 0);
            say("tcp: closed by the iPhone");
            return -1;
        }
        return 1;
    }
}

/* one lockdown request (an XML dictionary body), its answer printed */
static int lockdown_ask(const char* body)
{
    static const char head[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD "
                               "PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist "
                               "version=\"1.0\">\n<dict>\n<key>Label</key>\n<string>nbt</string>\n";
    static const char tail[] = "</dict>\n</plist>\n";
    char msg[2048];
    const int len = snprintf(msg + 4, sizeof(msg) - 4, "%s%s%s", head, body, tail);
    unsigned i, want, t;
    if (len <= 0 || len >= (int)sizeof(msg) - 4)
        return -1;
    put32((unsigned char*)msg, (unsigned)len);
    if (tcp_send(TCP_ACK, (const unsigned char*)msg, 4 + (unsigned)len))
        return -1;
    for (t = 0; t < 50; ++t) {
        if (g_mux.datan >= 4) {
            want = get32(g_mux.data);
            if (want > sizeof(g_mux.data) - 4) {
                say("lockdown: answer of %u bytes is too long", want);
                return -1;
            }
            if (g_mux.datan >= 4 + want)
                break;
        }
        if (tcp_poll(200) < 0)
            return -1;
    }
    if (g_mux.datan < 4 || g_mux.datan < 4 + get32(g_mux.data)) {
        say("lockdown: no answer");
        return -1;
    }
    want = get32(g_mux.data);
    printf("      answer: ");
    for (i = 4; i < 4 + want; ++i) {   /* the XML on one line */
        const char c = (char)g_mux.data[i];
        if (c != '\n' && c != '\t' && c != '\r')
            putchar(c);
    }
    putchar('\n');
    memmove(g_mux.data, g_mux.data + 4 + want, g_mux.datan - 4 - want);
    g_mux.datan -= 4 + want;
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

static int run_lockdown(void)
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
    g_mux.sport = 1 + (unsigned)(time(0) % 1000);
    g_mux.tseq = g_mux.tack = 0;
    if (tcp_send(TCP_SYN, 0, 0))
        goto fail;
    for (i = 0; i < 25 && !g_mux.connected; ++i)
        if (tcp_poll(200) < 0)
            goto fail;
    if (!g_mux.connected) {
        say("tcp: no answer from lockdown (port %u)", LOCKDOWN_PORT);
        goto fail;
    }
    say("lockdown: connected");
    say("lockdown: QueryType");
    lockdown_ask("<key>Request</key>\n<string>QueryType</string>\n");
    say("lockdown: GetValue DeviceName");
    lockdown_ask("<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>DeviceName</string>\n");
    say("lockdown: GetValue ProductVersion");
    lockdown_ask("<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>ProductVersion</string>\n");
    say("lockdown: GetValue ProductType");
    lockdown_ask("<key>Request</key>\n<string>GetValue</string>\n<key>Key</key>\n<string>ProductType</string>\n");
    tcp_send(TCP_FIN | TCP_ACK, 0, 0);
    usbd_disconnect(conn);
    say("lockdown: done (unplug the iPhone to bring it back to normal)");
    return 0;
fail:
    usbd_disconnect(conn);
    say("lockdown: stopped");
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
        return run_lockdown();
    fprintf(stderr, "usage: nbt_carplay_probe acp | usb | usb carplay | lockdown\n");
    return 2;
}
