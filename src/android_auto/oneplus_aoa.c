/* oneplus_aoa: the main() of the oneplus_native program (the name is historical: it started as a probe for the
 * developer's OnePlus 7 Pro; it works with any Android phone now). Bounded: one given bus/device, never a device-wide scan.
 * native_service calls it as --switch-aa <bus> <device> and --capture-video <bus> <device> (the live session, built
 * with -DNBT_AA_CAPTURE, see aa_video_capture.cpp).
 * Control-transfer implementation adapted from NBT-AAD usb_direct.cpp.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
 * --query sends GET_PROTOCOL only. --switch-aa also sends identity + START once.
 * --version sends one AA version request and reads its response, without TLS.
 * --tls-inspect adds one fresh ClientHello, records the public server flight,
 * then closes. It never completes TLS or sends AUTH_COMPLETE/application data.
 * No SET_AUDIO_MODE, reset, configuration change, driver stop or retry.
 */
#include <sys/usbdi.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#ifdef NBT_AA_CAPTURE
#include "aa_video_capture.h"
#endif
#ifdef NBT_AA_TLS_HANDSHAKE
#include "tls_usb_probe.h"
struct tls_usb_context {
    struct usbd_urb* urb;
    struct usbd_pipe *input, *output;
    unsigned char* dma;
};
#endif

static int window_active(void) {
    static const char* const paths[] = {
        "/opt/mm/bin/aoap-switch", "/fs/sda0/opt/mm/bin/aoap-switch",
        "/opt/mm/bin/devc-serusb-aoap", "/fs/sda0/opt/mm/bin/devc-serusb-aoap",
        "/opt/share/bin/io-fs-media", "/fs/sda0/opt/share/bin/io-fs-media"
    };
    unsigned i;
    if (geteuid() != 0) return 0;
    for (i = 0; i < 6; ++i) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) { close(fd); return 0; }
        if (errno != EACCES) return 0;
    }
    return 1;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static usbd_device_instance_t selected;
static unsigned target_bus, target_device, found, gone;
static unsigned target_vendor = 0x22d9, target_product = 0x2764;
static void inserted(struct usbd_connection* c, usbd_device_instance_t* d) {
    (void)c;
    if (d->path != target_bus || d->devno != target_device ||
        d->ident.vendor != target_vendor || d->ident.device != target_product || d->iface != 0) return;
    pthread_mutex_lock(&lock);
    selected = *d; ++found;
    pthread_mutex_unlock(&lock);
}
static void removed(struct usbd_connection* c, usbd_device_instance_t* d) {
    (void)c;
    pthread_mutex_lock(&lock);
    if (found && d->path == selected.path && d->devno == selected.devno &&
        d->generation == selected.generation) gone = 1;
    pthread_mutex_unlock(&lock);
}
static int number(const char* text) {
    char* end; long n;
    if (!*text) return -1;
    for (end = (char*)text; *end; ++end) if (*end < '0' || *end > '9') return -1;
    errno = 0; n = strtol(text, &end, 10);
    return errno || *end || n < 0 || n > 127 ? -1 : (int)n;
}
static int request(struct usbd_urb* urb, struct usbd_pipe* pipe, void* buffer,
                   unsigned request_id, unsigned index, unsigned size, int input,
                   _Uint32t* actual) {
    _Uint32t status = 0;
    int rc;
    usbd_setup_vendor(urb, input ? URB_DIR_IN | URB_SHORT_XFER_OK : size ? URB_DIR_OUT : URB_DIR_NONE,
        request_id, USB_TYPE_VENDOR | USB_RECIPIENT_DEVICE |
        (input ? USB_DIRECTION_HOST : USB_DIRECTION_DEVICE), 0, index, size ? buffer : NULL, size);
    rc = usbd_io(urb, pipe, NULL, NULL, 1000);
    *actual = 0; usbd_urb_status(urb, &status, actual);
    printf("AOA_REQUEST id=%u index=%u result=%d status=%08x bytes=%u\n",
           request_id, index, rc, (unsigned)status, (unsigned)*actual);
    return rc == EOK && (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP;
}
static int version_response(const unsigned char* b, unsigned n, int verbose) {
    unsigned size, major, minor, status;
    if (n < 12 || b[0] != 0 || b[1] != 3 || b[4] != 0 || b[5] != 2) return 0;
    size = (b[2] << 8) | b[3];
    if (size < 8 || size > n - 4) return 0;
    major = (b[6] << 8) | b[7]; minor = (b[8] << 8) | b[9]; status = (b[10] << 8) | b[11];
    if (verbose) printf("AA_VERSION_RESPONSE major=%u minor=%u status=%u\n", major, minor, status);
    return major == 1 && status == 0;
}
struct completion {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int done;
    _Uint32t status, actual;
};
static void completed(struct usbd_urb* urb, struct usbd_pipe* pipe, void* context) {
    struct completion* c = (struct completion*)context;
    (void)pipe;
    pthread_mutex_lock(&c->mutex);
    usbd_urb_status(urb, &c->status, &c->actual);
    c->done = 1;
    pthread_cond_signal(&c->condition);
    pthread_mutex_unlock(&c->mutex);
}
static int await_completion(struct completion* c, unsigned milliseconds) {
    struct timespec deadline;
    int rc = 0, done;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&c->mutex);
    while (!c->done && (!rc || rc == EINTR)) rc = pthread_cond_timedwait(&c->condition, &c->mutex, &deadline);
    done = c->done;
    pthread_mutex_unlock(&c->mutex);
    return done;
}
static int bulk_transfer(struct usbd_urb* urb, struct usbd_pipe* pipe, unsigned char* buffer,
                         unsigned size, int input, _Uint32t* status, _Uint32t* actual) {
    struct completion c;
    int rc;
    memset(&c, 0, sizeof(c));
    pthread_mutex_init(&c.mutex, NULL); pthread_cond_init(&c.condition, NULL);
    rc = usbd_setup_bulk(urb, input ? URB_DIR_IN | URB_SHORT_XFER_OK : URB_DIR_OUT, buffer, size);
    if (rc == EOK) rc = usbd_io(urb, pipe, completed, &c, 2000);
    if (rc == EOK && !await_completion(&c, 3000)) {
        puts("AA_BULK_CALLBACK_TIMEOUT_ABORTING_NO_RETRY");
        usbd_abort_pipe(pipe);
        if (!await_completion(&c, 1000)) {
            /* Do not free buffers/URBs still referenced by the USB callback. */
            puts("AA_BULK_ABORT_UNCONFIRMED_EXITING_NO_RETRY");
            _exit(6);
        }
        rc = ETIMEDOUT;
    }
    *status = c.status; *actual = c.actual;
    pthread_cond_destroy(&c.condition); pthread_mutex_destroy(&c.mutex);
    return rc;
}
static void* signal_test_completion(void* context) {
    struct completion* c = (struct completion*)context;
    usleep(10000);
    pthread_mutex_lock(&c->mutex); c->done = 1;
    pthread_cond_signal(&c->condition); pthread_mutex_unlock(&c->mutex);
    return NULL;
}
#ifdef NBT_AA_CAPTURE
struct capture_usb {
    struct usbd_urb* urb[2];
    struct usbd_pipe* pipe[2];
    unsigned char* dma[2];
};
static int capture_transfer(void* context,int input,unsigned char* data,unsigned* size) {
    struct capture_usb* c=(struct capture_usb*)context;
    _Uint32t status,actual;int rc;
    if(input<0 || input>1 || !*size || *size>65536)return 0;
    if(!input)memcpy(c->dma[0],data,*size);
    rc=bulk_transfer(c->urb[input],c->pipe[input],c->dma[input],*size,input,&status,&actual);
    if(rc!=EOK || (status&USBD_URB_STATUS_MASK)!=USBD_STATUS_CMP || actual>*size)return 0;
    if(input)memcpy(data,c->dma[1],actual);
    *size=actual;return 1;
}
#endif
static int completion_selftest(void) {
    struct completion c;
    pthread_t thread;
    int ok;
    memset(&c, 0, sizeof(c));
    pthread_mutex_init(&c.mutex, NULL); pthread_cond_init(&c.condition, NULL);
    ok = !await_completion(&c, 5);
    if (pthread_create(&thread, NULL, signal_test_completion, &c) != 0) return 0;
    ok = await_completion(&c, 1000) && ok;
    pthread_join(thread, NULL);
    ok = await_completion(&c, 5) && ok; /* Completion arrived before waiting. */
    pthread_cond_destroy(&c.condition); pthread_mutex_destroy(&c.mutex);
    return ok;
}
#ifdef NBT_AA_TLS_HANDSHAKE
static int tls_transfer(void* context, int input, unsigned char* data, unsigned* size) {
    struct tls_usb_context* c = (struct tls_usb_context*)context;
    _Uint32t status, actual;
    int rc;
    if (*size > 65536) return 0;
    if (!input) memcpy(c->dma,data,*size);
    rc=bulk_transfer(c->urb,input ? c->input : c->output,c->dma,*size,input,&status,&actual);
    printf("AA_TLS_USB direction=%s result=%d status=%08x bytes=%u\n",input ? "in" : "out",rc,(unsigned)status,(unsigned)actual);
    if (rc != EOK || (status & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP || actual>*size) return 0;
    if (input) memcpy(data,c->dma,actual);
    *size=actual; return 1;
}
#endif
static unsigned char hello[4096];
static unsigned hello_size;
static int valid_hello(const unsigned char* b, unsigned n) {
    unsigned record, handshake;
    if (n < 44 || n > sizeof(hello) || b[0] != 22 || b[1] != 3 || b[2] > 3 || b[5] != 1) return 0;
    record = (b[3] << 8) | b[4];
    handshake = (b[6] << 16) | (b[7] << 8) | b[8];
    return record == n - 5 && handshake == n - 9 && b[9] == 3 && b[10] == 3;
}
static int load_hello(void) {
    const char* path = getenv("NBT_AA_CLIENT_HELLO");
    FILE* f;
    int extra;
    if (!path || !(f = fopen(path, "rb"))) return 0;
    hello_size = fread(hello, 1, sizeof(hello), f);
    extra = fgetc(f);
    if (ferror(f)) { fclose(f); return 0; }
    fclose(f);
    return extra == EOF && valid_hello(hello, hello_size);
}
static int tls_inspect(struct usbd_urb* urb, struct usbd_pipe* input,
                       struct usbd_pipe* output, unsigned char* buffer) {
    _Uint32t status, actual;
    unsigned n, i, total = 0;
    int rc;
    buffer[0] = 0; buffer[1] = 3;
    buffer[2] = (hello_size + 2) >> 8; buffer[3] = (hello_size + 2) & 255;
    buffer[4] = 0; buffer[5] = 3;
    memcpy(buffer + 6, hello, hello_size);
    rc = bulk_transfer(urb, output, buffer, hello_size + 6, 0, &status, &actual);
    printf("AA_TLS_HELLO_WRITE result=%d status=%08x bytes=%u\n", rc, (unsigned)status, (unsigned)actual);
    if (rc != EOK || (status & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP || actual != hello_size + 6) return 3;
    /* Collect at most 64 KiB / eight transfers, with the existing two-second
       USB timeout and process alarm. A timeout after data ends capture only;
       it is not a successful handshake. Never resend the ClientHello. */
    for (n = 0; n < 8 && total < 65536; ++n) {
        rc = bulk_transfer(urb, input, buffer, 65536 - total, 1, &status, &actual);
        printf("AA_TLS_FLIGHT_READ result=%d status=%08x bytes=%u\n", rc, (unsigned)status, (unsigned)actual);
        if (rc != EOK || (status & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP) break;
        if (!actual || actual > 65536 - total) break;
        printf("AA_TLS_PUBLIC_FLIGHT_HEX=");
        for (i = 0; i < actual; ++i) printf("%02x", buffer[i]);
        putchar('\n'); total += actual;
    }
    printf("AA_TLS_CAPTURE_END bytes=%u authenticated=0 no_auth_complete=1 no_projection=1\n", total);
    return total ? 0 : 3;
}
static int version_probe(struct usbd_device* device, const usbd_device_instance_t* instance, int inspect_tls) {
    struct usbd_desc_node *iface = NULL, *ep = NULL;
    usbd_descriptors_t *ed, *bin = NULL, *bout = NULL;
    struct usbd_pipe *input = NULL, *output = NULL;
    struct usbd_urb* urb = NULL;
    unsigned char* buffer = NULL;
    const unsigned char version[] = {0, 3, 0, 8, 0, 1, 0, 1, 0, 7, 0, 0};
    _Uint32t status, actual;
    int i, rc, result = 3;
    if (!usbd_interface_descriptor(device, instance->config, instance->iface, instance->alternate, &iface)) goto done;
    for (i = 0; (ed = usbd_parse_descriptors(device, iface, USB_DESC_ENDPOINT, i, &ep)); ++i) {
        if ((ed->endpoint.bmAttributes & 3) != USB_ATTRIB_BULK) continue;
        if (ed->endpoint.bEndpointAddress & USB_ENDPOINT_IN) bin = ed; else bout = ed;
    }
    if (!bin || !bout) goto done;
    if (usbd_open_pipe(device, bin, &input) != EOK) { input = NULL; goto done; }
    if (usbd_open_pipe(device, bout, &output) != EOK) { output = NULL; goto done; }
#ifdef NBT_AA_CAPTURE
    if(inspect_tls==4) {
        struct capture_usb c;memset(&c,0,sizeof(c));c.pipe[0]=output;c.pipe[1]=input;
        for(i=0;i<2;++i) {c.urb[i]=usbd_alloc_urb(NULL);c.dma[i]=(unsigned char*)usbd_alloc(65536);}
        if(c.urb[0] && c.urb[1] && c.dma[0] && c.dma[1])
            result=nbt_video_capture_run(capture_transfer,&c,getenv("NBT_AA_CAPTURE_PREFIX"));
        for(i=0;i<2;++i) {if(c.urb[i])usbd_free_urb(c.urb[i]);if(c.dma[i])usbd_free(c.dma[i]);}
        goto done;
    }
#endif
    urb = usbd_alloc_urb(NULL); buffer = (unsigned char*)usbd_alloc(65536);
    if (!urb || !buffer) goto done;
    memcpy(buffer, version, sizeof(version));
    rc = bulk_transfer(urb, output, buffer, sizeof(version), 0, &status, &actual);
    printf("AA_VERSION_WRITE result=%d status=%08x bytes=%u\n", rc, (unsigned)status, (unsigned)actual);
    if (rc != EOK || (status & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP || actual != sizeof(version)) goto done;
    rc = bulk_transfer(urb, input, buffer, 65536, 1, &status, &actual);
    printf("AA_VERSION_READ result=%d status=%08x bytes=%u\n", rc, (unsigned)status, (unsigned)actual);
    if (rc == EOK && (status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP && actual <= 65536 &&
        version_response(buffer, actual, 1)) {
        puts("AA_USB_VERSION_OK no_tls_no_projection=1"); result = 0;
        if (inspect_tls == 1) result = tls_inspect(urb, input, output, buffer);
#ifdef NBT_AA_TLS_HANDSHAKE
        if (inspect_tls >= 2) {
            struct tls_usb_context c;
            c.urb=urb; c.input=input; c.output=output; c.dma=buffer;
            result=nbt_tls_probe_run(tls_transfer,&c,inspect_tls == 3);
        }
#endif
    }
done:
    if (urb) usbd_free_urb(urb);
    if (buffer) usbd_free(buffer);
    if (input) usbd_close_pipe(input);
    if (output) usbd_close_pipe(output);
    return result;
}
int main(int argc, char** argv) {
    usbd_connect_parm_t params;
    usbd_funcs_t callbacks;
    usbd_device_ident_t ident;
    struct usbd_connection* connection = NULL;
    struct usbd_device* device = NULL;
    struct usbd_pipe* pipe = NULL;
    struct usbd_urb* urb = NULL;
    struct usbd_desc_node* node = NULL;
    usbd_descriptors_t* descriptor;
    unsigned char* buffer = NULL;
    usbd_device_instance_t instance;
    _Uint32t actual;
    int bus, dev, switching, versioning = 0, inspecting = 0, rc, result = 3;
    unsigned matches, removed_already, protocol, i;
    static const char* const identity[] = {
        "Android", "Android Auto", "Android Auto", "2.0.1", "", "NBT-AA-TEST"
    };
#ifdef NBT_AA_TLS_HANDSHAKE
    if (argc == 2 && !strcmp(argv[1], "--tls-preflight")) {
        alarm(20);
        rc=nbt_tls_probe_init(); nbt_tls_probe_free();
        puts(rc ? "AA_TLS_PREFLIGHT_OK no_usb=1" : "AA_TLS_PREFLIGHT_FAILED no_usb=1");
        return rc ? 0 : 2;
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "--selftest")) {
        unsigned char b[] = {0, 3, 0, 8, 0, 2, 0, 1, 0, 7, 0, 0}; unsigned n;
        for (n = 0; n < sizeof(b); ++n) if (version_response(b, n, 0)) return 1;
        if (!version_response(b, sizeof(b), 0)) return 1;
        b[11] = 1; if (version_response(b, sizeof(b), 0)) return 1; b[11] = 0;
        b[7] = 2; if (version_response(b, sizeof(b), 0)) return 1; b[7] = 1;
        b[3] = 9; if (version_response(b, sizeof(b), 0)) return 1;
        if (!completion_selftest()) return 1;
        memset(hello, 0, 44); hello[0] = 22; hello[1] = 3; hello[2] = 1;
        hello[4] = 39; hello[5] = 1; hello[8] = 35; hello[9] = 3; hello[10] = 3;
        if (!valid_hello(hello, 44) || valid_hello(hello, 43)) return 1;
        hello[5] = 2; if (valid_hello(hello, 44)) return 1;
        puts("AA_VERSION_PARSER_OK truncated refused incompatible oversized completion-wait timeout"); return 0;
    }
    if (argc != 4 || ((switching = !strcmp(argv[1], "--switch-aa")) == 0 &&
#ifdef NBT_AA_CAPTURE
        (inspecting = !strcmp(argv[1], "--capture-video") ? 4 : 0) == 0 &&
#endif
#ifdef NBT_AA_TLS_HANDSHAKE
        (inspecting = !strcmp(argv[1], "--discover") ? 3 : 0) == 0 &&
        (inspecting = !strcmp(argv[1], "--tls-handshake") ? 2 : 0) == 0 &&
#endif
        (inspecting = !strcmp(argv[1], "--tls-inspect")) == 0 &&
        (versioning = !strcmp(argv[1], "--version")) == 0 && strcmp(argv[1], "--query")) ||
        (bus = number(argv[2])) < 0 || (dev = number(argv[3])) < 1) {
        fputs("Usage: oneplus_aoa --query|--switch-aa|--version|--tls-inspect bus device\n", stderr); return 2;
    }
    alarm(inspecting==4 ? (getenv("NBT_AA_LIVE_FIFO") ? 0 : 75) : 20); setvbuf(stdout, NULL, _IOLBF, 0);
    if ((switching || versioning || inspecting) && !window_active()) {
        puts("AOA_SWITCH_REFUSED_NO_ACCESSORY_WINDOW"); return 3;
    }
    if (inspecting == 1 && !load_hello()) { puts("AA_TLS_INVALID_CLIENT_HELLO"); return 2; }
#ifdef NBT_AA_TLS_HANDSHAKE
    if ((inspecting == 2 || inspecting == 3) && !nbt_tls_probe_init()) return 2;
#endif
    target_bus = bus; target_device = dev;
    /* other phones: NBT_PHONE_ID=vvvv:pppp (the phone's normal USB identity, set by native_service from phones.txt),
     * NBT_AOA_PRODUCT=2d01 when the phone enumerates as accessory+ADB (USB debugging on) */
    {
        const char *id = getenv("NBT_PHONE_ID"), *aoa = getenv("NBT_AOA_PRODUCT");
        unsigned v, p;
        if (id && sscanf(id, "%4x:%4x", &v, &p) == 2 && v && p) { target_vendor = v; target_product = p; }
        if (versioning || inspecting) {
            target_vendor = 0x18d1;
            target_product = aoa && !strcmp(aoa, "2d01") ? 0x2d01 : 0x2d00;
        }
    }
    memset(&params, 0, sizeof(params)); memset(&callbacks, 0, sizeof(callbacks));
    ident.vendor = target_vendor; ident.device = target_product;
    ident.dclass = ident.subclass = ident.protocol = USBD_CONNECT_WILDCARD;
    callbacks.nentries = _USBDI_NFUNCS; callbacks.insertion = inserted; callbacks.removal = removed;
    params.vusb = USB_VERSION; params.vusbd = USBD_VERSION; params.ident = &ident;
    params.funcs = &callbacks; params.connect_wait = 1;
    rc = usbd_connect(&params, &connection);
    if (rc != EOK) { printf("AOA_CONNECT_FAILED code=%d\n", rc); goto done; }
    sleep(1);
    pthread_mutex_lock(&lock);
    matches = found; removed_already = gone; instance = selected;
    pthread_mutex_unlock(&lock);
    if (matches != 1 || removed_already) {
        printf("AOA_TARGET_REFUSED matches=%u removed=%u expected=%04x:%04x bus=%u device=%u\n",
               matches, removed_already, target_vendor, target_product, target_bus, target_device); goto done;
    }
    rc = usbd_attach(connection, &instance, 0, &device);
    if (rc != EOK) { device = NULL; printf("AOA_ATTACH_FAILED code=%d\n", rc); goto done; }
    if (versioning || inspecting) { result = version_probe(device, &instance, inspecting); goto done; }
    descriptor = usbd_parse_descriptors(device, NULL, USB_DESC_DEVICE, 0, &node);
    if (!descriptor || usbd_open_pipe(device, descriptor, &pipe) != EOK) { pipe = NULL; goto done; }
    urb = usbd_alloc_urb(NULL); buffer = (unsigned char*)usbd_alloc(256);
    if (!urb || !buffer) goto done;
    if (!request(urb, pipe, buffer, 51, 0, 2, 1, &actual) || actual != 2) goto done;
    protocol = buffer[0] | (buffer[1] << 8);
    if (protocol != 1 && protocol != 2) { puts("AOA_PROTOCOL_UNSUPPORTED"); goto done; }
    printf("AOA_PROTOCOL_OK version=%u identity=%04x:%04x bus=%u device=%u\n", protocol, target_vendor, target_product,
           target_bus, target_device);
    if (!switching) { result = 0; goto done; }
    for (i = 0; i < 6; ++i) {
        size_t size = strlen(identity[i]) + 1;
        memcpy(buffer, identity[i], size);
        if (!request(urb, pipe, buffer, 52, i, size, 0, &actual)) goto done;
    }
    /* START can cause immediate re-enumeration. Never retry an ambiguous result. */
    result = request(urb, pipe, buffer, 53, 0, 0, 0, &actual) ? 0 : 4;
    puts(result ? "AOA_START_OUTCOME_UNKNOWN_INSPECT_USB_NO_RETRY" : "AOA_START_SENT_INSPECT_USB");
done:
#ifdef NBT_AA_TLS_HANDSHAKE
    nbt_tls_probe_free();
#endif
    if (urb) usbd_free_urb(urb);
    if (buffer) usbd_free(buffer);
    if (pipe) usbd_close_pipe(pipe);
    if (device) usbd_detach(device);
    if (connection) usbd_disconnect(connection);
    alarm(0);
    printf("AOA_PROBE_END result=%d\n", result);
    return result;
}
