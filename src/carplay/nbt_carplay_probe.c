/* nbt_carplay_probe: first steps towards wired CarPlay on the NBT (2026-11-23).
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 * (license and the section 7 permission for the head unit's libraries: NOTICE.md, "GPLv3 parts")
 *
 *   nbt_carplay_probe acp           the car's Apple authentication chip: version, certificate, one test signature
 *   nbt_carplay_probe usb           wait for an iPhone, print what it offers over USB (changes nothing)
 *   nbt_carplay_probe usb carplay   the same, then ask the iPhone for its CarPlay USB configuration and print what
 *                                   it offers after that (the iPhone re-connects; unplug it to undo)
 *
 * The chip: I2C bus 0, dev-i2c node /dev/i2c/i2c_0/acp (/opt/sys/etc/i2c-conf_x86.cfg). A register is read by
 * writing its number, then reading; written by writing its number followed by the data. The chip sleeps between
 * transfers and does not answer the first one: every transfer is retried. Run "acp" without an iPhone plugged in,
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

static int acp_read(unsigned reg, unsigned char* out, unsigned n)
{
    const unsigned char r = (unsigned char)reg;
    int tries;
    for (tries = 0; tries < 20; ++tries) {
        if (acp_write(&r, 1) == 0 && read(g_acp, out, n) == (ssize_t)n)
            return 0;
        usleep(1000);
    }
    say("acp: read of register 0x%02x (%u bytes) failed: %s", reg, n, strerror(errno));
    return -1;
}

static int acp_set(unsigned reg, const unsigned char* data, unsigned n)
{
    unsigned char b[1 + 128];
    if (n > 128)
        return -1;
    b[0] = (unsigned char)reg;
    memcpy(b + 1, data, n);
    return acp_write(b, n + 1);
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
    if (acp_read(0x00, b, 4) || acp_read(0x04, id, 4))
        return 1;
    say("acp: device version 0x%02x (%s), firmware 0x%02x, protocol %u.%u, device id %02x %02x %02x %02x", b[0],
        b[0] == 0x03 ? "2.0B" : b[0] == 0x05 ? "2.0C" : b[0] == 0x07 ? "3.0" : "?", b[1], b[2], b[3], id[0], id[1],
        id[2], id[3]);
    if (!memcmp(id, b, 4)) {
        say("acp: register 0x04 read back register 0x00: this driver does not take the register number as a write;"
            " stopping");
        return 1;
    }
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
    char* s = index ? usbd_string(dev, (_Uint8)index, 0) : 0;
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
        usbd_configuration_descriptor_t* c = usbd_configuration_descriptor(dev, (_Uint8)cfg, &cn);
        unsigned mux = 0, ncm = 0;
        if (!c)
            continue;
        say("usb:   configuration %u \"%s\": %u interfaces, %u mA", c->bConfigurationValue,
            text(dev, c->iConfiguration), c->bNumInterfaces, c->MaxPower * 2);
        for (ifc = 0; ifc < 16; ++ifc)
            for (alt = 0; alt < 4; ++alt) {
                usbd_interface_descriptor_t* i =
                    usbd_interface_descriptor(dev, (_Uint8)cfg, (_Uint8)ifc, (_Uint8)alt, &in);
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
    fprintf(stderr, "usage: nbt_carplay_probe acp | usb | usb carplay\n");
    return 2;
}
