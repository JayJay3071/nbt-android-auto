/* nbt_usb_reset: makes one Apple device on the USB register again (a port reset: the head unit sees it leave and come
 * back, exactly like pulling and plugging it).
 *
 * Why (car 2026-11-14): after an iPhone is plugged in, the HMI's media application first sends the iPod driver the
 * message 0x80 and then waits for the device to come back fresh; only on that second arrival does it ask the driver
 * everything and list the source. Our iPod stand-in (nbt_aa_ipod) has no iPhone to talk into a restart, so it calls
 * this program for the iPhone in the snap-in adapter.
 *
 * Usage: nbt_usb_reset <bus> <device> [reconfig [1..4]]      only vendor 0x05ac (Apple) is ever touched
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/usbdi.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* The iPhone shows three interfaces: 0 and 1 (class 01, sound) are in use by the head unit's sound driver, 2 (class 03,
 * control) is where the stock iPod driver would sit - free while our stand-in runs in its place. Attaching through a
 * busy interface is refused (error 16, car 2026-11-14); so every interface is remembered and tried, the control one
 * first. */
static usbd_device_instance_t seen[8];
static unsigned target_bus, target_device, found;

static void inserted(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    if (d->path != target_bus || d->devno != target_device || d->ident.vendor != 0x05ac)
        return;
    pthread_mutex_lock(&lock);
    if (found < 8)
        seen[found++] = *d;
    pthread_mutex_unlock(&lock);
}

static void removed(struct usbd_connection* c, usbd_device_instance_t* d)
{
    (void)c;
    (void)d;
}

int main(int argc, char** argv)
{
    usbd_connect_parm_t params;
    usbd_funcs_t callbacks;
    usbd_device_ident_t ident;
    struct usbd_connection* connection = NULL;
    struct usbd_device* device = NULL;
    usbd_device_instance_t instance;
    int rc, i;

    if (argc < 3)
        return 2;
    target_bus = (unsigned)atoi(argv[1]);
    target_device = (unsigned)atoi(argv[2]);
    alarm(15);
    memset(&ident, 0, sizeof(ident));
    ident.vendor = 0x05ac;
    ident.device = USBD_CONNECT_WILDCARD;
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
        printf("USB_RESET connect failed code=%d\n", rc);
        return 1;
    }
    for (i = 0; i < 15; ++i) /* the interfaces are reported one after the other: wait for all of them */
        usleep(100000);
    if (!found) {
        printf("USB_RESET no Apple device at bus %u device %u\n", target_bus, target_device);
        usbd_disconnect(connection);
        return 3;
    }
    rc = EBUSY;
    for (i = 0; i < 2 * (int)found && !device; ++i) { /* first pass: control interfaces (class 03), second: the rest */
        const unsigned k = (unsigned)i % found;
        const int control = seen[k].ident.dclass == 0x03;
        if ((i < (int)found) != control)
            continue;
        instance = seen[k];
        rc = usbd_attach(connection, &instance, 0, &device);
        printf("USB_RESET attach through interface %u (class %02x): code=%d\n", (unsigned)instance.iface,
               (unsigned)instance.ident.dclass, rc);
        if (rc != EOK)
            device = NULL;
    }
    if (!device) {
        usbd_disconnect(connection);
        return 1;
    }
    if (argc > 3 && !strcmp(argv[3], "reconfig")) {
        /* A plain reset is carried out but nobody is told (car 2026-11-16: reset code 0, the USB launcher saw no
         * removal). Switching the iPhone to its first USB configuration takes the iPod interfaces away, which the
         * launcher does see; on the arrival that follows it selects configuration 2 again by itself, as it does
         * after a real plug-in ("Selecting usb configuration: 2"). */
        const int wanted = argc > 4 ? atoi(argv[4]) : 1;
        rc = wanted >= 1 && wanted <= 4 ? usbd_select_config(device, (_Uint8t)wanted) : EINVAL;
        printf("USB_RESET bus %u device %u: configuration %d selected, code=%d\n", target_bus, target_device, wanted, rc);
    } else {
        rc = usbd_reset_device(device);
        printf("USB_RESET bus %u device %u product %04x: reset code=%d\n", target_bus, target_device, instance.ident.device, rc);
    }
    usbd_detach(device);
    usbd_disconnect(connection);
    return rc == EOK ? 0 : 1;
}
