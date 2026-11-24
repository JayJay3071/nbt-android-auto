/* nbt_hub_port: switches one port of the snap-in adapter's USB hub off and on again, so that the device behind it
 * (the iPhone that stands in for our iPod source) leaves the bus and registers again - what pulling it out and
 * putting it back does by hand.
 *
 * Why (car 2026-11-14/16): the head unit lists the iPod source only after the Apple device has come back once
 * (after the first contact the media application asks for a restart and waits for the device to re-appear), and at
 * a cold start the stock driver takes the iPhone before our service runs. Resetting the iPhone itself was refused
 * (it is in use by the USB launcher: attach error 16). The hub in front of it is asked instead, with the standard
 * hub requests "port power off / on".
 *
 * Only the hub of the snap-in adapter is ever touched: vendor 0x0424, product 0x2514.
 * Usage: nbt_hub_port <bus> <hub device number> <port> [status | list]
 *        "list": print every device the USB stack reports to a client, touch nothing
 *        without "status": port status, power off, 2 s, power on, port status
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/usbdi.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static usbd_device_instance_t selected;
static unsigned target_bus, target_device, found;
static int listing; /* "list": only print what the USB stack reports to a client */

static void inserted(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    if (listing) {
        printf("HUB_PORT sees bus %u device %u: %04x:%04x class %02x interface %u\n", (unsigned)d->path, (unsigned)d->devno,
               (unsigned)d->ident.vendor, (unsigned)d->ident.device, (unsigned)d->ident.dclass, (unsigned)d->iface);
        return;
    }
    if (d->path != target_bus || d->devno != target_device || d->ident.vendor != 0x0424 || d->ident.device != 0x2514)
        return;
    pthread_mutex_lock(&lock);
    if (!found) {
        selected = *d;
        found = 1;
    }
    pthread_mutex_unlock(&lock);
}

static void removed(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    (void)d;
}

/* hub class request to a port: request 1 = clear feature, 3 = set feature, 0 = get status (4 bytes back) */
static int port_request(struct usbd_urb* urb, struct usbd_pipe* pipe, unsigned request, unsigned feature, unsigned port,
                        unsigned char* answer)
{
    _Uint32t status = 0, actual = 0;
    int rc;
    if (answer)
        usbd_setup_vendor(urb, URB_DIR_IN | URB_SHORT_XFER_OK, request, USB_TYPE_CLASS | USB_RECIPIENT_OTHER | USB_DIRECTION_HOST,
                          feature, port, answer, 4);
    else
        usbd_setup_vendor(urb, URB_DIR_NONE, request, USB_TYPE_CLASS | USB_RECIPIENT_OTHER | USB_DIRECTION_DEVICE, feature,
                          port, NULL, 0);
    rc = usbd_io(urb, pipe, NULL, NULL, 2000);
    usbd_urb_status(urb, &status, &actual);
    printf("HUB_PORT request %u feature %u port %u: result %d status %08x bytes %u\n", request, feature, port, rc,
           (unsigned)status, (unsigned)actual);
    return rc;
}

int main(int argc, char** argv)
{
    usbd_connect_parm_t params;
    usbd_funcs_t callbacks;
    usbd_device_ident_t ident;
    struct usbd_connection* connection = NULL;
    struct usbd_device* device = NULL;
    struct usbd_pipe* pipe = NULL;
    struct usbd_urb* urb = NULL;
    struct usbd_desc_node* node;
    usbd_descriptors_t* descriptor;
    usbd_device_instance_t instance;
    unsigned char* answer = NULL;
    unsigned port;
    int rc, i, result = 1, only_status;

    if (argc < 4)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    target_bus = (unsigned)atoi(argv[1]);
    target_device = (unsigned)atoi(argv[2]);
    port = (unsigned)atoi(argv[3]);
    only_status = argc > 4 && !strcmp(argv[4], "status");
    listing = argc > 4 && !strcmp(argv[4], "list");
    if (port < 1 || port > 4)
        return 2;
    alarm(25);
    memset(&ident, 0, sizeof(ident));
    ident.vendor = listing ? USBD_CONNECT_WILDCARD : 0x0424;
    ident.device = listing ? USBD_CONNECT_WILDCARD : 0x2514;
    ident.dclass = USBD_CONNECT_WILDCARD;
    ident.subclass = USBD_CONNECT_WILDCARD;
    ident.protocol = USBD_CONNECT_WILDCARD;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.nentries = _USBDI_NFUNCS;
    callbacks.insertion = inserted;
    callbacks.removal = removed;
    memset(&params, 0, sizeof(params));
    params.vusb = USB_VERSION;
    params.vusbd = USBD_VERSION;
    params.ident = &ident;
    params.funcs = &callbacks;
    params.connect_wait = 1;
    rc = usbd_connect(&params, &connection);
    if (rc != EOK) {
        printf("HUB_PORT connect failed code=%d\n", rc);
        return 1;
    }
    for (i = 0; i < 20 && !found; ++i)
        usleep(100000);
    pthread_mutex_lock(&lock);
    instance = selected;
    pthread_mutex_unlock(&lock);
    if (listing)
        goto done;
    if (!found) {
        /* Hubs are not announced to clients (car 2026-11-16: only the devices behind them are). The stack still lets
         * a client attach to one by its place on the bus, as the "usb" utility does. */
        memset(&instance, 0, sizeof(instance));
        instance.path = target_bus;
        instance.devno = target_device;
        instance.generation = 0;
        instance.ident.vendor = USBD_CONNECT_WILDCARD;
        instance.ident.device = USBD_CONNECT_WILDCARD;
        instance.ident.dclass = USBD_CONNECT_WILDCARD;
        instance.ident.subclass = USBD_CONNECT_WILDCARD;
        instance.ident.protocol = USBD_CONNECT_WILDCARD;
        instance.config = USBD_CONNECT_WILDCARD;
        instance.iface = USBD_CONNECT_WILDCARD;
        instance.alternate = USBD_CONNECT_WILDCARD;
    }
    rc = usbd_attach(connection, &instance, 0, &device);
    if (rc != EOK) {
        device = NULL;
        printf("HUB_PORT attach failed code=%d\n", rc);
        goto done;
    }
    descriptor = usbd_parse_descriptors(device, NULL, USB_DESC_DEVICE, 0, &node);
    /* the safety check: this must be the hub of the snap-in adapter and nothing else */
    if (!descriptor || descriptor->device.idVendor != 0x0424 || descriptor->device.idProduct != 0x2514) {
        printf("HUB_PORT bus %u device %u is not the snap-in hub (%04x:%04x)\n", target_bus, target_device,
               descriptor ? (unsigned)descriptor->device.idVendor : 0, descriptor ? (unsigned)descriptor->device.idProduct : 0);
        goto done;
    }
    if (usbd_open_pipe(device, descriptor, &pipe) != EOK) {
        pipe = NULL;
        printf("HUB_PORT no control pipe\n");
        goto done;
    }
    urb = usbd_alloc_urb(NULL);
    answer = (unsigned char*)usbd_alloc(64);
    if (!urb || !answer)
        goto done;
    memset(answer, 0, 64);
    if (port_request(urb, pipe, 0, 0, port, answer) == EOK)
        printf("HUB_PORT status before: %02x %02x %02x %02x\n", answer[0], answer[1], answer[2], answer[3]);
    if (only_status) {
        result = 0;
        goto done;
    }
    rc = port_request(urb, pipe, 1, 8, port, NULL); /* clear PORT_POWER */
    if (rc != EOK)
        goto done;
    sleep(2);
    rc = port_request(urb, pipe, 3, 8, port, NULL); /* set PORT_POWER */
    sleep(1);
    memset(answer, 0, 64);
    if (port_request(urb, pipe, 0, 0, port, answer) == EOK)
        printf("HUB_PORT status after: %02x %02x %02x %02x\n", answer[0], answer[1], answer[2], answer[3]);
    result = rc == EOK ? 0 : 1;
done:
    if (answer)
        usbd_free(answer);
    if (urb)
        usbd_free_urb(urb);
    if (pipe)
        usbd_close_pipe(pipe);
    if (device)
        usbd_detach(device);
    if (connection)
        usbd_disconnect(connection);
    printf("HUB_PORT end result=%d\n", result);
    return result;
}
