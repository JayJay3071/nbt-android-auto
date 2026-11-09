// Bounded Annex-B NAL splitter for a future original-NBT video backend.
// Feed only video payload AFTER AA message/timestamp removal. Not a decoder,
// SPS validator, access-unit assembler or hardware-readiness check.
#ifndef NBT_H264_ANNEXB_HPP
#define NBT_H264_ANNEXB_HPP
#include <stddef.h>
#include <stdint.h>
#include <vector>
namespace nbt {
class AnnexBInput {
public:
    typedef bool (*NalCallback)(void*, const uint8_t*, size_t);
    AnnexBInput(size_t limit, NalCallback callback, void* context)
        : limit_(limit), callback_(callback), context_(context) { reset(); }
    void reset() { nal_.clear(); zeros_=0; started_=false; finished_=false;
        failed_=!limit_ || limit_>2*1024*1024 || !callback_; }
    bool feed(const uint8_t* data, size_t size) {
        if (failed_ || finished_ || (!data && size)) return fail();
        for (size_t i=0;i<size;++i) {
            uint8_t b=data[i];
            if (!b) {
                // A full-size NAL may be followed by another start code.
                // Keep delimiter zeros outside the payload limit until known.
                if (zeros_>=limit_+3) return fail();
                ++zeros_; continue;
            }
            if (b==1 && zeros_>=2) {
                if (started_ && !emit()) return false;
                started_=true; zeros_=0; continue;
            }
            if (!started_ || zeros_>limit_-nal_.size() ||
                nal_.size()+zeros_>=limit_) return fail();
            nal_.insert(nal_.end(),zeros_,0); zeros_=0;
            nal_.push_back(b);
        }
        return true;
    }
    bool finish() {
        if (failed_ || finished_ || !started_) return fail();
        // Annex-B trailing_zero_8bits are framing, not NAL payload.
        zeros_=0; finished_=true; return emit();
    }
    bool failed() const { return failed_; }
private:
    bool fail() { failed_=true; nal_.clear(); zeros_=0; return false; }
    bool emit() {
        if (nal_.empty() || (nal_[0]&128) || !(nal_[0]&31) || (nal_[0]&31)>=24) return fail();
        // The callback receives one NAL without its start code. The pointer is
        // valid only during the call; callback failure ends this stream.
        if (!callback_(context_,&nal_[0],nal_.size())) return fail();
        nal_.clear(); return true;
    }
    size_t limit_, zeros_;
    NalCallback callback_; void* context_;
    bool started_, finished_, failed_;
    std::vector<uint8_t> nal_;
    AnnexBInput(const AnnexBInput&);
    AnnexBInput& operator=(const AnnexBInput&);
};
}
#endif
