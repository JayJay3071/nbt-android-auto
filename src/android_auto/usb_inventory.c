/* Original NBT diagnostic: observe USB insertion/removal notifications only.
 * No attach, endpoints, control transfers, mode switches or driver signals.
 * API: QNX 6.5 usbd_connect/usbd_disconnect documentation.
 */
#include <sys/usbdi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned insertions;

static void report(const char *event, const usbd_device_instance_t *dev) {
    printf("USB_%s bus=%u device=%u vendor=%04x product=%04x class=%02x\n",
           event, (unsigned)dev->path, (unsigned)dev->devno,
           (unsigned)dev->ident.vendor, (unsigned)dev->ident.device,
           (unsigned)dev->ident.dclass);
}

static void inserted(struct usbd_connection *connection, usbd_device_instance_t *dev) {
    (void)connection;
    pthread_mutex_lock(&lock);
    ++insertions;
    report("INSERT", dev);
    pthread_mutex_unlock(&lock);
}

static void removed(struct usbd_connection *connection, usbd_device_instance_t *dev) {
    (void)connection;
    pthread_mutex_lock(&lock);
    report("REMOVE", dev);
    pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv) {
    struct usbd_connection *connection = NULL;
    usbd_connect_parm_t params;
    usbd_funcs_t funcs;
    usbd_device_ident_t interest;
    long seconds = 3;
    unsigned seen;
    int rc;
    if (argc > 2) return 2;
    if (argc == 2) {
        char *end;
        errno = 0;
        seconds = strtol(argv[1], &end, 10);
        if (errno || !argv[1][0] || *end || seconds < 1 || seconds > 30) {
            fprintf(stderr, "Usage: usb_inventory [seconds: 1..30]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Bound library connection/disconnection waits as well as observation. */
    alarm((unsigned)seconds + 10);
    memset(&params, 0, sizeof(params));
    memset(&funcs, 0, sizeof(funcs));
    interest.vendor = interest.device = interest.dclass = interest.subclass =
        interest.protocol = USBD_CONNECT_WILDCARD;
    funcs.nentries = _USBDI_NFUNCS;
    funcs.insertion = inserted;
    funcs.removal = removed;
    params.vusb = USB_VERSION;
    params.vusbd = USBD_VERSION;
    params.ident = &interest;
    params.funcs = &funcs;
    params.connect_wait = 1;
    printf("USB_OBSERVE_BEGIN seconds=%ld no_device_claim=1\n", seconds);
    rc = usbd_connect(&params, &connection);
    if (rc != EOK) {
        fprintf(stderr, "USB_STACK_UNAVAILABLE code=%d message=%s\n", rc, strerror(rc));
        return 3;
    }
    sleep((unsigned)seconds);
    rc = usbd_disconnect(connection);
    pthread_mutex_lock(&lock);
    seen = insertions;
    pthread_mutex_unlock(&lock);
    printf("USB_OBSERVE_END insertion_events=%u disconnect_code=%d\n", seen, rc);
    alarm(0);
    return rc == EOK ? 0 : 4;
}
