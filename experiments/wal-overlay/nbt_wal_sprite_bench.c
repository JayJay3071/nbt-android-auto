/*
 * nbt_wal_sprite_bench - measure how fast full frames can be pushed through the WAL sprite interface.
 *
 * Every frame is a new RGBA sprite (w*h*4 bytes) that replaces the previous one: the new sprite is created, assigned to
 * the window, positioned, sized and shown, then the previous sprite is destroyed (two sprite ids alternate, so there is
 * always one visible sprite). The pattern is a dark gradient with a moving white bar and four coloured corner squares:
 * red top-left, green top-right, blue bottom-left, yellow bottom-right (checks full coverage and channel order).
 *
 * usage: nbt_wal_sprite_bench <window 0x100|0x50> <w> <h> <fps 1..60> <seconds 1..20>
 *   w 16..1280, h 16..480
 * Prints achieved frames per second, average and maximum time of the createSprite call. Nothing is written anywhere else.
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
    unsigned long i, j, bar = (frame * 12) % w;
    unsigned long m = w < 200 ? 8 : 40;
    for (j = 0; j < h; j++) {
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
}

int main(int argc, char **argv)
{
    unsigned long window, w, h, frame = 0;
    int fps, seconds;
    const unsigned long IDS[2] = { 0x7a11, 0x7a12 };
    unsigned char *pix;
    void *lib, *wal;
    create_ptr_t create_ptr;
    create_sprite_t create_sprite;
    destroy_sprite_t destroy_sprite;
    assign_t assign;
    position_t set_pos;
    size_t2 set_size;
    visible_t set_visible;
    double t0, next, period, create_sum = 0, create_max = 0, end;

    if (argc != 6) { fprintf(stderr, "usage: %s <window 0x100|0x50> <w> <h> <fps> <seconds>\n", argv[0]); return 2; }
    window = strtoul(argv[1], NULL, 0);
    w = strtoul(argv[2], NULL, 0);
    h = strtoul(argv[3], NULL, 0);
    fps = atoi(argv[4]);
    seconds = atoi(argv[5]);
    if ((window != 0x100 && window != 0x50) || w < 16 || w > 1280 || h < 16 || h > 480 || fps < 1 || fps > 60 || seconds < 1 || seconds > 20) {
        fprintf(stderr, "arguments out of the allowed range\n");
        return 2;
    }
    pix = malloc(w * h * 4);
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
    fprintf(stderr, "bench %lux%lu at %d fps for %d s on window 0x%lx\n", w, h, fps, seconds, window);

    period = 1000.0 / fps;
    t0 = now_ms();
    next = t0;
    end = t0 + seconds * 1000.0;
    while (now_ms() < end) {
        unsigned long id = IDS[frame & 1], prev = IDS[(frame + 1) & 1];
        double a, b;
        draw(pix, w, h, frame);
        a = now_ms();
        create_sprite(wal, id, pix, w, h);
        b = now_ms();
        assign(wal, id, window);
        set_pos(wal, id, 0, 0);
        set_size(wal, id, w, h);
        set_visible(wal, id, 1);
        if (frame > 0) destroy_sprite(wal, prev);
        create_sum += b - a;
        if (b - a > create_max) create_max = b - a;
        frame++;
        next += period;
        if (next > now_ms()) usleep((useconds_t)((next - now_ms()) * 1000.0));
        else if (now_ms() - next > 500.0) next = now_ms();
    }
    {
        double total = now_ms() - t0;
        fprintf(stderr, "frames=%lu time=%.2f s fps=%.2f createSprite avg=%.2f ms max=%.2f ms\n",
                frame, total / 1000.0, frame * 1000.0 / total, create_sum / (frame ? frame : 1), create_max);
    }
    set_visible(wal, IDS[(frame + 1) & 1], 0);
    set_visible(wal, IDS[frame & 1], 0);
    destroy_sprite(wal, IDS[(frame + 1) & 1]);
    destroy_sprite(wal, IDS[frame & 1]);
    usleep(300000);
    free(pix);
    fprintf(stderr, "done\n");
    return 0;
}
