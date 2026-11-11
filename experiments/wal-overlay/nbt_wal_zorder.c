/*
 * nbt_wal_zorder - set the WAL z-order (and optionally visibility) of compositor windows, e.g. bring the hardware video
 * window VIDEO (0x41c) in front of the HMI window (0x100). Prototype 2026-11-07, small control messages only (no
 * textures): WalHmiInterface::setZOrder sends {len, 0x40000041, count, {id, z}...} (decoded from libwal_nvidia).
 * The WalServer logs every change as "zorder changed <id>: <old> => <new>", so the previous values are in sloginfo.
 *
 * usage: nbt_wal_zorder [--hold S] [--visible ID 0|1] ID:Z [ID:Z ...]     (ID and Z decimal or 0x hex)
 *   --hold S   keep the client connected S seconds (0..600, default 0) before exiting
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { unsigned long id; unsigned short z; unsigned short pad; } OrderTypeDef;   /* 8 bytes, as in the lib */
typedef void *(*create_t)(void);
typedef void (*zorder_t)(void *self, const OrderTypeDef *order, unsigned long count);
typedef void (*visible_t)(void *self, unsigned long id, int visible);

int main(int argc, char **argv)
{
    OrderTypeDef order[16];
    unsigned long n = 0, vis_id = 0;
    int vis = -1, hold = 0, i;
    void *lib, *hmi;
    create_t create;
    zorder_t setz;
    visible_t setv;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--visible") && i + 2 < argc) { vis_id = strtoul(argv[i + 1], 0, 0); vis = atoi(argv[i + 2]); i += 2; }
        else {
            char *c = strchr(argv[i], ':');
            if (!c || n >= 16) { fprintf(stderr, "usage: %s [--hold S] [--visible ID 0|1] ID:Z ...\n", argv[0]); return 2; }
            order[n].id = strtoul(argv[i], 0, 0);
            order[n].z = (unsigned short)strtoul(c + 1, 0, 0);
            order[n].pad = 0;
            n++;
        }
    }
    if (hold < 0 || hold > 600 || (!n && vis < 0)) return 2;
    lib = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    create = (create_t)dlsym(lib, "_ZN3wal15WalHmiInterface9createPtrEv");
    setz = (zorder_t)dlsym(lib, "_ZN3wal15WalHmiInterface9setZOrderEPKNS0_12OrderTypeDefEm");
    setv = (visible_t)dlsym(lib, "_ZN3wal15WalHmiInterface10setVisibleEmb");
    if (!create || !setz || !setv) { fprintf(stderr, "symbols missing\n"); return 3; }
    hmi = create();
    if (!hmi) { fprintf(stderr, "WalHmiInterface::createPtr returned NULL\n"); return 3; }
    sleep(1);   /* let the client register with the server */
    if (n) {
        setz(hmi, order, n);
        for (i = 0; i < (int)n; i++) printf("zorder %lu (0x%lx) -> %u\n", order[i].id, order[i].id, order[i].z);
    }
    if (vis >= 0) { setv(hmi, vis_id, vis); printf("visible %lu -> %d\n", vis_id, vis); }
    fflush(stdout);
    if (hold) sleep((unsigned)hold);
    else sleep(1);
    _exit(0);   /* the library's threads are not joined */
}
