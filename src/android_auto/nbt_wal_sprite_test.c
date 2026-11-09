/*
 * nbt_wal_sprite_test - minimal, bounded test of the WAL sprite interface of the stock window server (WalServer).
 *
 * The HMI itself uses sprites (for example the mouse cursor). A sprite is a raw RGBA bitmap (width*height*4 bytes)
 * that an application uploads (createSprite), assigns to a WAL window (assignSpriteToWindow), positions
 * (setSpritePosition), scales (setSpriteSize) and shows (setSpriteVisible). The client library libwal_nvidia.so
 * talks to the server through /dev/WAL; no EGL/KD graphics are initialised in this process.
 *
 * usage: nbt_wal_sprite_test <window> <x> <y> <w> <h> <seconds>
 *   window   0x100 (HMI), 0x200 (ASSIST) or 0x50 (MOUSE)
 *   w,h      w<=1280, h<=480, w*h*4 <= 1.6 MB     seconds 1..30
 * The sprite is opaque mid-grey with a white border and four coloured corner squares: red top-left, green top-right,
 * blue bottom-left, yellow bottom-right (checks coverage, channel order and opacity).
 * The library is loaded with dlopen() from /opt/sys/lib; symbols are resolved by their mangled C++ names and called
 * with the x86 member function convention (this pointer as first stack argument).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *(*create_ptr_t)(void);
typedef void (*create_sprite_t)(void *self, unsigned long id, const void *data, unsigned long w, unsigned long h);
typedef void (*destroy_sprite_t)(void *self, unsigned long id);
typedef void (*assign_t)(void *self, unsigned long sprite, unsigned long window);
typedef void (*position_t)(void *self, unsigned long id, long x, long y);
typedef void (*size_t2)(void *self, unsigned long id, unsigned long w, unsigned long h);
typedef void (*visible_t)(void *self, unsigned long id, int visible);

static void *need(void *h, const char *name)
{
    void *p = dlsym(h, name);
    if (!p) { fprintf(stderr, "missing symbol %s\n", name); exit(3); }
    return p;
}

int main(int argc, char **argv)
{
    unsigned long window, w, h;
    long x, y;
    int seconds;
    const unsigned long ID = 0x7a01;
    unsigned char *pix;
    unsigned long i, j;
    void *lib, *wal;
    create_ptr_t create_ptr;
    create_sprite_t create_sprite;
    destroy_sprite_t destroy_sprite;
    assign_t assign;
    position_t set_pos;
    size_t2 set_size;
    visible_t set_visible;

    if (argc != 7) { fprintf(stderr, "usage: %s <window 0x100|0x50> <x> <y> <w> <h> <seconds>\n", argv[0]); return 2; }
    window = strtoul(argv[1], NULL, 0);
    x = strtol(argv[2], NULL, 0);
    y = strtol(argv[3], NULL, 0);
    w = strtoul(argv[4], NULL, 0);
    h = strtoul(argv[5], NULL, 0);
    seconds = atoi(argv[6]);
    /* One sprite must stay below 1.6 MB: a 1280x480 sprite (2.4 MB) blocked the client in the WAL write buffer. */
    if ((window != 0x100 && window != 0x200 && window != 0x50) || w < 1 || w > 1280 || h < 1 || h > 480 ||
        w * h * 4 > 1600000 || seconds < 1 || seconds > 30) {
        fprintf(stderr, "arguments out of the allowed range\n");
        return 2;
    }
    pix = malloc(w * h * 4);
    if (!pix) return 3;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            unsigned char *p = pix + (j * w + i) * 4;
            unsigned long m = (w < 120 || h < 120) ? 8 : 60;
            if (i < 4 || j < 4 || i >= w - 4 || j >= h - 4) { p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255; }
            else if (i < m && j < m) { p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255; }
            else if (i >= w - m && j < m) { p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 255; }
            else if (i < m && j >= h - m) { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
            else if (i >= w - m && j >= h - m) { p[0] = 255; p[1] = 255; p[2] = 0; p[3] = 255; }
            else { p[0] = 128; p[1] = 128; p[2] = 128; p[3] = 255; }
        }
    }
    lib = dlopen("/opt/sys/lib/libwal_nvidia.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 3; }
    create_ptr = (create_ptr_t)need(lib, "_ZN3wal23WalApplicationInterface9createPtrEv");
    create_sprite = (create_sprite_t)need(lib, "_ZN3wal23WalApplicationInterface12createSpriteEmPKvmm");
    destroy_sprite = (destroy_sprite_t)need(lib, "_ZN3wal23WalApplicationInterface13destroySpriteEm");
    assign = (assign_t)need(lib, "_ZN3wal23WalApplicationInterface20assignSpriteToWindowEmm");
    set_pos = (position_t)need(lib, "_ZN3wal23WalApplicationInterface17setSpritePositionEmll");
    set_size = (size_t2)need(lib, "_ZN3wal23WalApplicationInterface13setSpriteSizeEmmm");
    set_visible = (visible_t)need(lib, "_ZN3wal23WalApplicationInterface16setSpriteVisibleEmb");

    wal = create_ptr();
    if (!wal) { fprintf(stderr, "createPtr returned NULL\n"); return 3; }
    fprintf(stderr, "connected, creating sprite %lu (%lux%lu) for window 0x%lx at %ld,%ld\n", ID, w, h, window, x, y);
    create_sprite(wal, ID, pix, w, h);
    assign(wal, ID, window);
    set_pos(wal, ID, x, y);
    set_size(wal, ID, w, h);
    set_visible(wal, ID, 1);
    fprintf(stderr, "sprite shown for %d s\n", seconds);
    sleep((unsigned)seconds);
    set_visible(wal, ID, 0);
    destroy_sprite(wal, ID);
    usleep(300000);
    fprintf(stderr, "sprite removed\n");
    free(pix);
    return 0;
}
