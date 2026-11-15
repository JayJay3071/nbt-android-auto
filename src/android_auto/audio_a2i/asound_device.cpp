// NBT-Project replacement of NBT-AAD milestone-7 src/audio/asound_device.cpp: BrowserAudio's output device writes to the
// original HU_NBT's A2I endpoint input_stereo_3 instead of the (missing) libasound shim. See asound_device.hpp.
// Based on: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto),
// PolyForm-Noncommercial-1.0.0. A2I protocol as in NBT-AAD audio/a2i_stream.cpp and experiments/audio/nbt_a2i_beep.c
// (car-verified 2026-11-15: 3 beeps heard on input_stereo_3 with connection 0xb8 built).
#include "audio/asound_device.hpp"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace nbt {

namespace {

// liba2i_client.so configuration (76 bytes, layout from NBT-AAD include/a2i_client.h). mode = number of packages
// (sloginfo on the car: "uNumberOfPackages: 2" for mode 2).
#pragma pack(push, 4)
struct A2iCfg {
    uint32_t magic, direction, field_08, mode, buffer_size, channels, bytes_per_sample, sample_rate, field_20;
    const char* endpoint_name;
    uint32_t channel_type, field_2c, sample_bits, field_34, timeout_ms, reserved[4];
};
#pragma pack(pop)

enum { F_OPEN = 0, F_CLOSE, F_NOTIFY, F_GETBUF, F_WRITE, F_ERR, F_COUNT };
const char* const kFnNames[F_COUNT] = {
    "A2I_Client_Open", "A2I_Client_Close", "A2I_Client_GetNotification", "A2I_Client_GetBuffer",
    "A2I_Client_Out_Write", "A2I_Client_GetErrorDescription"
};
typedef int (*FnOpen)(void**, const A2iCfg*);
typedef int (*FnClose)(void**);
typedef int (*FnNotify)(void*);
typedef int (*FnGetBuf)(void*, void**, size_t*);
typedef int (*FnWrite)(void*, const void*, size_t, uint32_t);
typedef const char* (*FnErr)(int);

const uint32_t kCookieLegacy = 0x05555555u;   // the library starts the stream itself on the 2nd Out_Write
const int kNoBuffer = 0x14;                   // GetBuffer: no free package right now
const unsigned kWaitMs = 1000;                // a package must come free within this time, else the write fails

const char* envOr(const char* name, const char* def) {
    const char* v = getenv(name);
    return v && *v ? v : def;
}

uint32_t envU(const char* name, uint32_t def) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    char* end = 0;
    unsigned long x = strtoul(v, &end, 0);
    return (end != v && !*end) ? (uint32_t)x : def;
}

uint64_t monoMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000);
}

} // namespace

const uint32_t AsoundDevice::kChannels;

AsoundDevice::AsoundDevice()
    : want_rate_(48000), force_rate_(0), lib_(NULL), pcm_(NULL), rate_(0), frag_bytes_(0), cur_(NULL), cur_size_(0),
      cur_fill_(0), handed_(0), write_errors_(0), prepares_(0), writes_(0), writes_blocked_(0), lead_min_(0),
      lead_max_(0), lead_seen_(false) {
    memset(fn_, 0, sizeof(fn_));
}

AsoundDevice::~AsoundDevice() {}

void AsoundDevice::configure(const char*, uint32_t want_rate, uint32_t force_rate, const char*, int, int) {
    want_rate_ = want_rate;
    force_rate_ = force_rate;
}

bool AsoundDevice::load() {
    if (lib_) return true;
    const char* path = envOr("NBT_A2I_LIB", "/opt/sys/lib/liba2i_client.so");
    lib_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        printf("[Browser] could not load %s: %s\n", path, dlerror());
        return false;
    }
    for (int i = 0; i < F_COUNT; ++i) {
        fn_[i] = dlsym(lib_, kFnNames[i]);
        if (!fn_[i] && i != F_ERR) {
            printf("[Browser] %s has no %s\n", path, kFnNames[i]);
            dlclose(lib_);
            lib_ = NULL;
            return false;
        }
    }
    printf("[Browser] A2I output: %s loaded, endpoint %s\n", path, envOr("NBT_A2I_ENDPOINT", "input_stereo_3"));
    return true;
}

void AsoundDevice::unload() {
    if (lib_) { dlclose(lib_); lib_ = NULL; }
}

bool AsoundDevice::open() {
    if (pcm_) return true;
    if (!lib_ && !load()) return false;
    // 2 packages like the car-proven beep test (after the start every package waits for a notification); 4096 bytes
    // each = 1024 frames = 21 ms, 42 ms queued
    uint32_t buf = envU("NBT_A2I_BUF", 4096), packages = envU("NBT_A2I_PACKAGES", 2);
    if (buf < 32 || buf > 65536) buf = 4096;
    while (buf & (buf - 1)) buf &= buf - 1;   // a power of two (liba2i check_parameter)
    if (packages < 1 || packages > 256) packages = 2;
    if (openWith(buf, packages)) return true;
    if (buf == 1024 && packages == 2) return false;
    printf("[Browser] trying the car-proven A2I setup: 1024-byte packages x2\n");
    return openWith(1024, 2);
}

bool AsoundDevice::openWith(uint32_t package_bytes, uint32_t packages) {
    const char* ep = envOr("NBT_A2I_ENDPOINT", "input_stereo_3");
    const uint32_t rate = force_rate_ ? force_rate_ : 48000;   // the endpoint was proven at 48 kHz
    A2iCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0x02000000; cfg.direction = 0; cfg.field_08 = 1; cfg.mode = packages; cfg.buffer_size = package_bytes;
    cfg.channels = kChannels; cfg.bytes_per_sample = 2; cfg.sample_rate = rate; cfg.endpoint_name = ep;
    cfg.channel_type = 1; cfg.sample_bits = 0; cfg.field_34 = kCookieLegacy; cfg.timeout_ms = 2000;
    void* h = NULL;
    int rc = ((FnOpen)fn_[F_OPEN])(&h, &cfg);
    FnErr err = (FnErr)fn_[F_ERR];
    if (rc || !h) {
        printf("[Browser] A2I_Client_Open('%s', %u Hz, %u bytes x%u) failed: %d %s\n", ep, rate, package_bytes,
               packages, rc, err ? err(rc) : "");
        return false;
    }
    pcm_ = h;
    rate_ = rate;
    frag_bytes_ = package_bytes;
    cur_ = NULL;
    cur_size_ = cur_fill_ = 0;
    handed_ = 0;
    printf("[Browser] A2I output open: %s, %u Hz x%u, %u-byte packages x%u (%u ms)\n", ep, rate, kChannels,
           package_bytes, packages, (unsigned)(package_bytes * packages / (kChannels * 2) * 1000u / rate));
    if (want_rate_ && want_rate_ != rate)
        printf("[Browser] note: configured rate %u Hz, A2I runs at %u Hz (the mixer resamples)\n", want_rate_, rate);
    return true;
}

// The first two packages are taken without a notification (the library starts the stream on the 2nd Out_Write, legacy
// cookie); afterwards GetNotification says when a queued package has been played and can be filled again.
bool AsoundDevice::nextPackage() {
    const uint64_t deadline = monoMs() + kWaitMs;
    bool notified = handed_ < 2;
    for (;;) {
        if (!notified) {
            if (((FnNotify)fn_[F_NOTIFY])(pcm_) == 0) notified = true;
            else if (monoMs() >= deadline) { ++prepares_; return false; }
            else { usleep(1000); continue; }
        }
        void* b = NULL;
        size_t n = 0;
        int rc = ((FnGetBuf)fn_[F_GETBUF])(pcm_, &b, &n);
        if (rc == 0 && b && n) {
            cur_ = (uint8_t*)b;
            cur_size_ = n;
            cur_fill_ = 0;
            return true;
        }
        if (monoMs() >= deadline) { ++prepares_; return false; }
        if (rc != kNoBuffer || handed_ >= 2) notified = handed_ < 2;   // wait for the next notification
        usleep(rc == kNoBuffer ? 2000 : 1000);
    }
}

bool AsoundDevice::flushPackage() {
    int rc = ((FnWrite)fn_[F_WRITE])(pcm_, cur_, cur_size_, 0);
    cur_ = NULL;
    cur_size_ = cur_fill_ = 0;
    if (rc) {
        ++write_errors_;
        FnErr err = (FnErr)fn_[F_ERR];
        if (write_errors_ <= 5 || write_errors_ % 500 == 0)
            printf("[Browser] A2I Out_Write failed: %d %s (%u so far)\n", rc, err ? err(rc) : "", write_errors_);
        return false;
    }
    if (++handed_ == 2) printf("[Browser] A2I stream started (2 packages queued)\n");
    return true;
}

bool AsoundDevice::write(const int16_t* buf, size_t frames) {
    if (!pcm_) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
    size_t left = frames * kChannels * 2;
    while (left > 0) {
        if (!cur_ && !nextPackage()) return false;
        size_t n = cur_size_ - cur_fill_;
        if (n > left) n = left;
        memcpy(cur_ + cur_fill_, p, n);
        cur_fill_ += n;
        p += n;
        left -= n;
        if (cur_fill_ == cur_size_ && !flushPackage()) return false;
    }
    return true;
}

void AsoundDevice::close() {
    if (!pcm_) return;
    if (cur_) {   // hand back what we hold, padded with silence (never keep a library buffer across Close)
        memset(cur_ + cur_fill_, 0, cur_size_ - cur_fill_);
        flushPackage();
    }
    ((FnClose)fn_[F_CLOSE])(&pcm_);
    pcm_ = NULL;
    printf("[Browser] A2I output closed (%u packages written)\n", handed_);
}

} // namespace nbt
