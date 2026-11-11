/*
 * nbt_wal_sprite_bench2 - persistent-sprite frame update through the WAL sprite interface.
 *
 * The WalServer keeps a sprite object per id: creating a sprite with an id that already exists replaces only the texture
 * contents (it keeps window assignment, position, size and visibility). So after one-time setup every frame is a single
 * createSprite() call - no destroy and no re-assign, hence no flicker gap. The displayed size (setSpriteSize) is
 * independent of the texture size, the GPU scales.
 *
 * usage: nbt_wal_sprite_bench2 <window 0x100|0x200> <tex-w> <tex-h> <disp-w> <disp-h> <disp-x> <disp-y> <fps 1..60> <seconds 1..20> <bg 0|1>
 *   tex-w*tex-h*4 <= 1.6 MB (a 2.4 MB sprite blocked the client in the WAL write buffer)
 *   bg=1 adds a black background sprite covering 1280x480 behind the picture
 * Pattern: dark gradient, moving white bar, corner squares (red TL, green TR, blue BL, yellow BR).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void put(unsigned char *p, unsigned char r, unsigned char g, unsigned char b)
{
    p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

static void draw(unsigned char *pix, unsigned long w, unsigned long h, unsigned long frame)
{
    unsigned long i, j, bar = (frame * 12) % w, m = w < 200 ? 8 : 40;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            unsigned char *p = pix + (j * w + i) * 4;
            unsigned char g = (unsigned char)(24 + (i * 60) / w);
            if (i >= bar && i < bar + 6) put(p, 255, 255, 255);
            else if (i < m && j < m) put(p, 255, 0, 0);
            else if (i >= w - m && j < m) put(p, 0, 255, 0);
            else if (i < m && j >= h - m) put(p, 0, 0, 255);
            else if (i >= w - m && j >= h - m) put(p, 255, 255, 0);
            else put(p, g, g, g);
        }
}

int main(int argc, char **argv)
{
    unsigned long window, tw, th, dw, dh, frame = 0;
    long dx, dy;
    int fps, seconds, bg;
    const unsigned long ID = 0x7a21, BGID = 0x7a20;
    unsigned char *pix, black[16 * 16 * 4];
    void *lib, *wal;
    create_ptr_t create_ptr;
    create_sprite_t create_sprite;
    destroy_sprite_t destroy_sprite;
    assign_t assign;
    position_t set_pos;
    size_t2 set_size;
    visible_t set_visible;
    double t0, next, period, sum = 0, mx = 0, end;

    if (argc != 11) {
        fprintf(stderr, "usage: %s <window 0x100|0x200> <tex-w> <tex-h> <disp-w> <disp-h> <disp-x> <disp-y> <fps> <seconds> <bg 0|1>\n", argv[0]);
        return 2;
    }
    window = strtoul(argv[1], NULL, 0);
    tw = strtoul(argv[2], NULL, 0);
    th = strtoul(argv[3], NULL, 0);
    dw = strtoul(argv[4], NULL, 0);
    dh = strtoul(argv[5], NULL, 0);
    dx = strtol(argv[6], NULL, 0);
    dy = strtol(argv[7], NULL, 0);
    fps = atoi(argv[8]);
    seconds = atoi(argv[9]);
    bg = atoi(argv[10]);
    if ((window != 0x100 && window != 0x200) || tw < 16 || th < 16 || tw * th * 4 > 1600000 || dw < 16 || dw > 2000 || dh < 16 || dh > 1000 ||
        fps < 1 || fps > 60 || seconds < 1 || seconds > 20 || (bg != 0 && bg != 1)) {
        fprintf(stderr, "arguments out of the allowed range\n");
        return 2;
    }
    pix = malloc(tw * th * 4);
    if (!pix) return 3;
    memset(black, 0, sizeof(black));
    {
        int k;
        for (k = 0; k < 16 * 16; k++) black[k * 4 + 3] = 255;
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
    fprintf(stderr, "bench2 texture %lux%lu shown %lux%lu at %ld,%ld, %d fps, %d s, bg=%d, window 0x%lx\n", tw, th, dw, dh, dx, dy, fps, seconds, bg, window);

    if (bg) {
        create_sprite(wal, BGID, black, 16, 16);
        assign(wal, BGID, window);
        set_pos(wal, BGID, 0, 0);
        set_size(wal, BGID, 1280, 480);
        set_visible(wal, BGID, 1);
    }
    draw(pix, tw, th, 0);
    create_sprite(wal, ID, pix, tw, th);
    assign(wal, ID, window);
    set_pos(wal, ID, dx, dy);
    set_size(wal, ID, dw, dh);
    set_visible(wal, ID, 1);

    period = 1000.0 / fps;
    t0 = now_ms();
    next = t0;
    end = t0 + seconds * 1000.0;
    while (now_ms() < end) {
        double a, b;
        draw(pix, tw, th, frame);
        a = now_ms();
        create_sprite(wal, ID, pix, tw, th);
        b = now_ms();
        sum += b - a;
        if (b - a > mx) mx = b - a;
        frame++;
        next += period;
        if (next > now_ms()) usleep((useconds_t)((next - now_ms()) * 1000.0));
        else if (now_ms() - next > 500.0) next = now_ms();
    }
    {
        double total = now_ms() - t0;
        fprintf(stderr, "frames=%lu time=%.2f s fps=%.2f createSprite avg=%.2f ms max=%.2f ms\n",
                frame, total / 1000.0, frame * 1000.0 / total, sum / (frame ? frame : 1), mx);
    }
    set_visible(wal, ID, 0);
    destroy_sprite(wal, ID);
    if (bg) { set_visible(wal, BGID, 0); destroy_sprite(wal, BGID); }
    usleep(300000);
    free(pix);
    fprintf(stderr, "done\n");
    return 0;
}
