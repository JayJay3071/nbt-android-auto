// h264_sps_timing: gives the phone's H.264 sequence parameter set (SPS) a frame rate.
//
// Why (car journal 2026-11-12): the phone's SPS (Baseline, level 3.1) has a VUI without timing info and the stream has
// no timestamps, so the stock player (Cinemo, raw H.264 over HTTP) plays it at a frame rate it assumes itself: never
// more than ~23-25 pictures/s were seen. While the navigation map moves the phone sends 30/s. The surplus piles up in the
// player's input buffer (the picture runs further and further behind) and once that buffer is full the player stops
// reading, fifo_http gives up after 2 s and the whole video chain is restarted: on the drive with navigation that
// happened every 64-66 s (17:07:40, 17:08:46, 17:09:52, 17:10:56, 17:12:01), and after five of them in a row the
// service ended the session (phone had to be replugged).
//
// The fix writes timing_info into the VUI (num_units_in_tick 1, time_scale 2 x fps, fixed_frame_rate_flag 1), so the
// player paces at fps pictures/s - faster than the phone ever sends - and plays every picture as soon as it is there.
// Everything else in the SPS stays bit for bit the same. An SPS that already has timing info, or one we cannot parse
// (scaling matrices), is left alone.
#ifndef NBT_H264_SPS_TIMING_HPP
#define NBT_H264_SPS_TIMING_HPP

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace nbt_sps {

struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos;
    bool bad;
    explicit BitReader(const std::vector<uint8_t>& v) : b(v), pos(0), bad(false) {}
    unsigned bit()
    {
        if (pos >= b.size() * 8) {
            bad = true;
            return 0;
        }
        const unsigned v = (b[pos >> 3] >> (7 - (pos & 7))) & 1;
        ++pos;
        return v;
    }
    unsigned u(int n)
    {
        unsigned v = 0;
        while (n-- > 0)
            v = (v << 1) | bit();
        return v;
    }
    unsigned ue() // se() has the same length, so it is skipped with ue() as well
    {
        int zeros = 0;
        while (!bad && !bit())
            if (++zeros > 31) {
                bad = true;
                return 0;
            }
        if (bad)
            return 0;
        return (unsigned)((1ull << zeros) - 1 + u(zeros));
    }
};

struct BitWriter {
    std::vector<uint8_t> b;
    size_t n;
    BitWriter() : n(0) {}
    void put(unsigned bit)
    {
        if (!(n & 7))
            b.push_back(0);
        if (bit)
            b.back() |= (uint8_t)(0x80 >> (n & 7));
        ++n;
    }
    void put(unsigned v, int bits)
    {
        while (bits-- > 0)
            put((v >> bits) & 1);
    }
    void copy(const std::vector<uint8_t>& from, size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
            put((from[i >> 3] >> (7 - (i & 7))) & 1);
    }
};

inline unsigned bit_at(const std::vector<uint8_t>& v, size_t i) { return (v[i >> 3] >> (7 - (i & 7))) & 1; }

// One SPS NAL unit (header byte first, no start code). Returns false (out untouched) if nothing was changed.
inline bool sps_with_timing(const uint8_t* nal, size_t n, unsigned fps, std::vector<uint8_t>& out)
{
    if (!nal || n < 4 || (nal[0] & 0x1f) != 7 || fps < 1 || fps > 240)
        return false;
    std::vector<uint8_t> rbsp; // emulation prevention bytes removed
    for (size_t i = 1, zeros = 0; i < n; ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(nal[i]);
        zeros = nal[i] ? 0 : zeros + 1;
    }
    BitReader r(rbsp);
    const unsigned profile = r.u(8);
    r.u(16); // constraint flags, level
    r.ue();  // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        if (r.ue() == 3) // chroma_format_idc
            r.u(1);
        r.ue();
        r.ue();
        r.u(1);
        if (r.u(1)) // seq_scaling_matrix_present_flag: not handled
            return false;
    }
    r.ue(); // log2_max_frame_num_minus4
    const unsigned poc_type = r.ue();
    if (poc_type == 0)
        r.ue();
    else if (poc_type == 1) {
        r.u(1);
        r.ue();
        r.ue();
        for (unsigned k = r.ue(); k > 0 && k < 256 && !r.bad; --k)
            r.ue();
    }
    r.ue();     // max_num_ref_frames
    r.u(1);     // gaps_in_frame_num_value_allowed_flag
    r.ue();     // pic_width_in_mbs_minus1
    r.ue();     // pic_height_in_map_units_minus1
    if (!r.u(1)) // frame_mbs_only_flag
        r.u(1);
    r.u(1); // direct_8x8_inference_flag
    if (r.u(1)) { // frame_cropping_flag
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    const size_t vui_at = r.pos;
    const bool vui = r.u(1) != 0;
    size_t timing_at = 0;
    if (vui) {
        if (r.u(1) && r.u(8) == 255) // aspect_ratio_info_present_flag, aspect_ratio_idc == Extended_SAR
            r.u(32);
        if (r.u(1)) // overscan_info_present_flag
            r.u(1);
        if (r.u(1)) { // video_signal_type_present_flag
            r.u(4);
            if (r.u(1))
                r.u(24);
        }
        if (r.u(1)) { // chroma_loc_info_present_flag
            r.ue();
            r.ue();
        }
        timing_at = r.pos;
        if (r.u(1)) // timing info already there
            return false;
    }
    if (r.bad)
        return false;
    // the rbsp stop bit is the last 1 bit; everything behind it is alignment
    size_t stop = rbsp.size() * 8;
    while (stop > r.pos && !bit_at(rbsp, stop - 1))
        --stop;
    if (stop <= r.pos)
        return false;
    --stop;

    BitWriter w;
    if (vui) {
        w.copy(rbsp, 0, timing_at);
        w.put(1);              // timing_info_present_flag
        w.put(1, 32);          // num_units_in_tick
        w.put(2 * fps, 32);    // time_scale: frame rate = time_scale / (2 x num_units_in_tick)
        w.put(1);              // fixed_frame_rate_flag
        w.copy(rbsp, timing_at + 1, stop);
    } else {
        w.copy(rbsp, 0, vui_at);
        w.put(1);              // vui_parameters_present_flag
        w.put(0);              // aspect_ratio_info_present_flag
        w.put(0);              // overscan_info_present_flag
        w.put(0);              // video_signal_type_present_flag
        w.put(0);              // chroma_loc_info_present_flag
        w.put(1);              // timing_info_present_flag
        w.put(1, 32);
        w.put(2 * fps, 32);
        w.put(1);
        w.put(0);              // nal_hrd_parameters_present_flag
        w.put(0);              // vcl_hrd_parameters_present_flag
        w.put(0);              // pic_struct_present_flag
        w.put(0);              // bitstream_restriction_flag
        w.copy(rbsp, vui_at + 1, stop);
    }
    w.put(1); // rbsp_stop_one_bit
    while (w.n & 7)
        w.put(0);

    std::vector<uint8_t> result;
    result.push_back(nal[0]);
    size_t zeros = 0;
    for (size_t i = 0; i < w.b.size(); ++i) {
        if (zeros >= 2 && w.b[i] <= 3) {
            result.push_back(3);
            zeros = 0;
        }
        result.push_back(w.b[i]);
        zeros = w.b[i] ? 0 : zeros + 1;
    }
    out.swap(result);
    return true;
}

// Annex-B buffer (codec config: SPS + PPS with start codes): every SPS gets timing info, the rest is copied.
// changed = true if an SPS was rewritten.
inline std::vector<uint8_t> annexb_with_timing(const uint8_t* p, size_t n, unsigned fps, bool& changed)
{
    changed = false;
    std::vector<size_t> codes; // positions of 00 00 01
    for (size_t k = 0; k + 2 < n; ++k)
        if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1) {
            codes.push_back(k);
            k += 2;
        }
    if (codes.empty())
        return std::vector<uint8_t>(p, p + n);
    std::vector<uint8_t> out(p, p + codes[0] + 3);
    for (size_t i = 0; i < codes.size(); ++i) {
        const size_t begin = codes[i] + 3;
        const size_t end = i + 1 < codes.size() ? codes[i + 1] : n;
        size_t body_end = end; // zero bytes in front of the next start code (4-byte form) stay where they are
        while (body_end > begin && p[body_end - 1] == 0)
            --body_end;
        std::vector<uint8_t> sps;
        if (body_end > begin && (p[begin] & 0x1f) == 7 && sps_with_timing(p + begin, body_end - begin, fps, sps)) {
            out.insert(out.end(), sps.begin(), sps.end());
            changed = true;
        } else
            out.insert(out.end(), p + begin, p + body_end);
        out.insert(out.end(), p + body_end, p + end);
        if (i + 1 < codes.size())
            out.insert(out.end(), p + end, p + end + 3);
    }
    return out;
}

} // namespace nbt_sps

#endif
