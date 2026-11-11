/*
 * nbt_wal_win - move / resize / crop WAL compositor windows (experiment 2026-11-08: Android Auto on the right, the
 * iDrive split screen on the left). Small control messages only, like nbt_wal_zorder (no textures).
 * Symbols from libwal_nvidia.so (wal::WalHmiInterface): moveWindow(id,x,y), setWindowPositionAndSize(id,x,y,w,h),
 * setViewport(id,x,y,w,h), resetViewport(id), setVisible(id,b). The WalServer logs the changes in sloginfo.
 *
 * usage: nbt_wal_win CMD [CMD ...]       (numbers decimal or 0x hex; commands run in order, 0.2 s apart)
 *   move ID X Y          vp ID X Y W H        pos ID X Y W H        reset ID        vis ID 0|1        hold S
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *(*create_t)(void);
typedef void (*move_t)(void *self, unsigned long id, unsigned long x, unsigned long y);
typedef void (*rect_t)(void *self, unsigned long id, unsigned long x, unsigned long y, unsigned long w, unsigned long h);
typedef void (*reset_t)(void *self, unsigned long id);
typedef void (*visible_t)(void *self, unsigned long id, int visible);

static unsigned long num(const char *s) { return strtoul(s, 0, 0); }

int main(int argc, char **argv)
{
    void *lib, *hmi;
    create_t create;
    move_t mv;
    rect_t vp, pos;
    reset_t reset;
    visible_t vis;
    int i;
    if (argc < 2) { fprintf(stderr, "usage: %s move ID X Y | vp ID X Y W H | pos ID X Y W H | reset ID | vis ID 0|1 | hold S ...\n", argv[0]); return 2; }
    lib = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    create = (create_t)dlsym(lib, "_ZN3wal15WalHmiInterface9createPtrEv");
    mv = (move_t)dlsym(lib, "_ZN3wal15WalHmiInterface10moveWindowEmmm");
    vp = (rect_t)dlsym(lib, "_ZN3wal15WalHmiInterface11setViewportEmmmmm");
    pos = (rect_t)dlsym(lib, "_ZN3wal15WalHmiInterface24setWindowPositionAndSizeEmmmmm");
    reset = (reset_t)dlsym(lib, "_ZN3wal15WalHmiInterface13resetViewportEm");
    vis = (visible_t)dlsym(lib, "_ZN3wal15WalHmiInterface10setVisibleEmb");
    if (!create || !mv || !vp || !pos || !reset || !vis) { fprintf(stderr, "symbols missing\n"); return 3; }
    hmi = create();
    if (!hmi) { fprintf(stderr, "createPtr returned NULL\n"); return 3; }
    sleep(1);   /* let the client register with the server */
    for (i = 1; i < argc; ) {
        const char *c = argv[i];
        if (!strcmp(c, "move") && i + 3 < argc) {
            mv(hmi, num(argv[i + 1]), num(argv[i + 2]), num(argv[i + 3]));
            printf("move %s -> %s,%s\n", argv[i + 1], argv[i + 2], argv[i + 3]); i += 4;
        } else if ((!strcmp(c, "vp") || !strcmp(c, "pos")) && i + 5 < argc) {
            (c[0] == 'v' ? vp : pos)(hmi, num(argv[i + 1]), num(argv[i + 2]), num(argv[i + 3]), num(argv[i + 4]), num(argv[i + 5]));
            printf("%s %s -> %s,%s %sx%s\n", c, argv[i + 1], argv[i + 2], argv[i + 3], argv[i + 4], argv[i + 5]); i += 6;
        } else if (!strcmp(c, "reset") && i + 1 < argc) {
            reset(hmi, num(argv[i + 1])); printf("reset viewport %s\n", argv[i + 1]); i += 2;
        } else if (!strcmp(c, "vis") && i + 2 < argc) {
            vis(hmi, num(argv[i + 1]), atoi(argv[i + 2])); printf("visible %s -> %s\n", argv[i + 1], argv[i + 2]); i += 3;
        } else if (!strcmp(c, "hold") && i + 1 < argc) {
            int s = atoi(argv[i + 1]); fflush(stdout); if (s > 0 && s <= 600) sleep((unsigned)s); i += 2;
        } else { fprintf(stderr, "bad command at '%s'\n", c); _exit(2); }
        fflush(stdout);
        usleep(200000);
    }
    sleep(1);
    _exit(0);   /* the library's threads are not joined */
}
