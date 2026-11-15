// NBT-Project replacement of NBT-AAD milestone-7 src/audio/asound_device.hpp (copied over it by scripts/build_all.py).
// Based on: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto),
// PolyForm-Noncommercial-1.0.0. Same class and public interface, so browser_audio.cpp is unchanged.
//
// The ORIGINAL HU_NBT has no libasound shim / dev-libasound (car 2026-11-15: "Library cannot be found"). Its audio inputs
// are A2I endpoints of dev-ioamp-router. Car-verified 2026-11-15: with ConnectionMaster connection 0xb8 built, PCM written
// to A2I "input_stereo_3" (48 kHz, 16-bit stereo, legacy cookie 0x05555555) is heard on the speakers. So this device
// writes the mixer's blocks straight into that endpoint through /opt/sys/lib/liba2i_client.so (dlopen'ed).
// Environment: NBT_A2I_ENDPOINT (default input_stereo_3), NBT_A2I_LIB (default /opt/sys/lib/liba2i_client.so),
//              NBT_A2I_BUF (bytes per package, default 4096) and NBT_A2I_PACKAGES (default 2); if the open with those is
//              refused, the car-proven 1024 x 2 is used.
#ifndef AUDIO_ASOUND_DEVICE_HPP
#define AUDIO_ASOUND_DEVICE_HPP

#include <stdint.h>
#include <stddef.h>

namespace nbt {

// Unchanged from upstream (used by tests / config): the output rate to ask for.
inline uint32_t pickDeviceRate(uint32_t want, uint32_t force, int min_rate, int max_rate) {
    if (force) return force;
    if (max_rate > 0 && ((int64_t)want > max_rate || (min_rate > 0 && (int64_t)want < min_rate)))
        return (uint32_t)max_rate;
    return want;
}

class AsoundDevice {
public:
    static const uint32_t kChannels = 2;   // stereo, interleaved

    AsoundDevice();
    ~AsoundDevice();

    // Same signature as upstream. shim_path / pcm are ignored (A2I); want_rate / force_rate pick the rate (48000 unless
    // forced); frag / frags are ignored (NBT_A2I_BUF / NBT_A2I_PACKAGES).
    void configure(const char* shim_path, uint32_t want_rate, uint32_t force_rate, const char* pcm, int frag,
                   int frags);
    bool load();     // dlopen liba2i_client and look up its entry points (already loaded: true)
    void unload();

    bool open();     // A2I_Client_Open of the endpoint. Already open: true.
    void close();    // pads and hands back a half-filled package, then A2I_Client_Close
    bool isOpen() const { return pcm_ != NULL; }
    uint32_t rate() const { return rate_; }
    uint32_t fragBytes() const { return frag_bytes_; }   // one A2I package = one mixer block

    // Copies frames of interleaved stereo into A2I packages; blocks (GetNotification) while every package is queued.
    bool write(const int16_t* buf, size_t frames);
    uint32_t writeErrors() const { return write_errors_; }
    uint32_t prepares() const { return prepares_; }

    void noteWrite(bool blocked) { ++writes_; if (blocked) ++writes_blocked_; }
    void noteLead(int64_t lead_ms) {
        if (!lead_seen_ || lead_ms < lead_min_) lead_min_ = lead_ms;
        if (!lead_seen_ || lead_ms > lead_max_) lead_max_ = lead_ms;
        lead_seen_ = true;
    }
    uint32_t writes() const { return writes_; }
    uint32_t writesBlocked() const { return writes_blocked_; }
    bool leadSeen() const { return lead_seen_; }
    int64_t leadMin() const { return lead_min_; }
    int64_t leadMax() const { return lead_max_; }
    void resetWaitRoom() { writes_ = writes_blocked_ = 0; lead_seen_ = false; }

private:
    AsoundDevice(const AsoundDevice&);
    AsoundDevice& operator=(const AsoundDevice&);

    bool openWith(uint32_t package_bytes, uint32_t packages);
    bool nextPackage();          // waits for and takes a free A2I package into cur_
    bool flushPackage();         // Out_Write of cur_

    uint32_t want_rate_;
    uint32_t force_rate_;

    void* lib_;
    void* fn_[8];
    void* pcm_;                  // A2I client handle
    uint32_t rate_;
    uint32_t frag_bytes_;

    uint8_t* cur_;               // package from GetBuffer being filled, NULL = none
    size_t cur_size_;
    size_t cur_fill_;
    uint32_t handed_;            // packages written since open (the library auto-starts the stream on the 2nd)

    uint32_t write_errors_;
    uint32_t prepares_;          // here: waits for a free package that timed out
    uint32_t writes_, writes_blocked_;
    int64_t lead_min_, lead_max_;
    bool lead_seen_;
};

} // namespace nbt

#endif // AUDIO_ASOUND_DEVICE_HPP
