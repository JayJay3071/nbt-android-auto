/*
 * nbt_wal_sprite_multi - find out which WAL window draws sprites on top of the HMI.
 *
 * Creates one fully opaque 150x150 sprite per listed WAL window id and shows them side by side for a few seconds.
 * Colours, left to right: red (0x100 HMI), green (0x200 ASSIST), blue (0x201 Map), yellow (0x202), magenta (0x207),
 * cyan (0x310 BROWSER_BON), white (0x330 BROWSER_BON_ENT). A black 4 px border marks every square.
 * Which squares are visible and how opaque they look tells which window is drawn above the HMI content.
 *
 * usage: nbt_wal_sprite_multi <seconds 1..20>
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
    static const unsigned long windows[7] = { 0x100, 0x200, 0x201, 0x202, 0x207, 0x310, 0x330 };
    static const unsigned char colors[7][3] = { {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0}, {255,0,255}, {0,255,255}, {255,255,255} };
    const unsigned long W = 150, H = 150;
    unsigned char *pix;
    void *lib, *wal;
    create_ptr_t create_ptr;
    create_sprite_t create_sprite;
    destroy_sprite_t destroy_sprite;
    assign_t assign;
    position_t set_pos;
    size_t2 set_size;
    visible_t set_visible;
    int seconds, k;
    unsigned long i, j;

    if (argc != 2) { fprintf(stderr, "usage: %s <seconds 1..20>\n", argv[0]); return 2; }
    seconds = atoi(argv[1]);
    if (seconds < 1 || seconds > 20) { fprintf(stderr, "seconds out of range\n"); return 2; }
    pix = malloc(W * H * 4);
    if (!pix) return 3;
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
    for (k = 0; k < 7; k++) {
        unsigned long id = 0x7b00 + (unsigned long)k;
        for (j = 0; j < H; j++)
            for (i = 0; i < W; i++) {
                unsigned char *p = pix + (j * W + i) * 4;
                if (i < 4 || j < 4 || i >= W - 4 || j >= H - 4) { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 255; }
                else { p[0] = colors[k][0]; p[1] = colors[k][1]; p[2] = colors[k][2]; p[3] = 255; }
            }
        create_sprite(wal, id, pix, W, H);
        assign(wal, id, windows[k]);
        set_pos(wal, id, 20 + k * 170, 100);
        set_size(wal, id, W, H);
        set_visible(wal, id, 1);
        fprintf(stderr, "sprite %lx -> window 0x%lx at x=%d\n", id, windows[k], 20 + k * 170);
        usleep(100000);
    }
    sleep((unsigned)seconds);
    for (k = 0; k < 7; k++) { set_visible(wal, 0x7b00 + (unsigned long)k, 0); destroy_sprite(wal, 0x7b00 + (unsigned long)k); }
    usleep(300000);
    free(pix);
    fprintf(stderr, "done\n");
    return 0;
}
