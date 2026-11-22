/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu. GNU GPL version 3 with the additional permission in NOTICE.md (GPLv3 parts). */
/* nbt_hwdec.h: the car's hardware H.264 decoder (libnmsdk) behind FFmpeg's VDPAU interface, for nbt_decoder.c.
 *
 * FFmpeg (hwaccel h264_vdpau) parses the stream, keeps the reference pictures and hands every picture over as
 * VdpPictureInfoH264 plus its slices (each after a 00 00 01 start code). The car's library takes the same information
 * in its own layout (nvh264bspPictureDecode) through INvVideo::VideoDecode and copies it, field by field, into its
 * VDPAU-like decoder (h264BSP2NVVA in libnmsdk.so, NVIDIA's NVVA H.264 decoders for G82/G98/GT212 video engines).
 * The offsets below are the ones h264BSP2NVVA reads (libnmsdk.so of 2014, 0x34d90), checked by running that function
 * in an emulator on what hw_bsp_fill() writes (2026-11-22).
 */
#ifndef NBT_HWDEC_H
#define NBT_HWDEC_H

#include <stdint.h>
#include <string.h>
#include <vdpau/vdpau.h>

enum {
    BSP_SIZE = 0x600,           /* the library reads up to 0x4e8 */
    BSP_FLAGS = 0x10,           /* bit 0 reference picture, bit 2 field picture, bit 3 bottom field */
    BSP_SCALING = 0x30,         /* != 0: own scaling matrices (not used here: 0 = flat, the only case we hand over) */
    BSP_LOG2_MAX_FRAME_NUM = 0x154, BSP_POC_TYPE = 0x158, BSP_LOG2_MAX_POC_LSB = 0x15c, BSP_DELTA_POC_ZERO = 0x160,
    BSP_FRAME_MBS_ONLY = 0x170, BSP_MBAFF = 0x174, BSP_DIRECT_8X8 = 0x178, BSP_CABAC = 0x180, BSP_POC_PRESENT = 0x184,
    BSP_REF_L0 = 0x1fc, BSP_REF_L1 = 0x200, BSP_WEIGHTED_PRED = 0x204, BSP_WEIGHTED_BIPRED = 0x208,
    BSP_PIC_INIT_QP = 0x20c, BSP_CHROMA_QP = 0x210, BSP_DEBLOCK_PRESENT = 0x214, BSP_CONSTRAINED_INTRA = 0x218,
    BSP_REDUNDANT_PIC_CNT = 0x21c, BSP_TRANSFORM_8X8 = 0x220, BSP_SECOND_CHROMA_QP = 0x348,
    BSP_POC0 = 0x354, BSP_POC1 = 0x358,
    BSP_REFS = 0x35c, BSP_REF_SIZE = 0x18,   /* per entry: +0 index into the reference table (8 bytes per entry, the
                                                surface first), +4 bit 0 top / bit 1 bottom field is a reference
                                                (0 = entry unused), +8 long-term (byte), +0xc frame_idx, +0x10/+0x14 POC */
    BSP_NUM_REF_FRAMES = 0x4dc, BSP_FRAME_NUM = 0x4e0, BSP_SLICES = 0x4e4,
};

static void bsp_int(uint8_t* b, int off, int32_t v) { memcpy(b + off, &v, 4); }

/* 1 if the 4x4 and 8x8 scaling lists are all flat (16): the only case the hand-over below covers */
static int hw_flat_scaling(const VdpPictureInfoH264* p)
{
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 16; ++j)
            if (p->scaling_lists_4x4[i][j] != 16)
                return 0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 64; ++j)
            if (p->scaling_lists_8x8[i][j] != 16)
                return 0;
    return 1;
}

/* VdpPictureInfoH264 -> nvh264bspPictureDecode; row_of(surface) gives a reference's row in the reference table, < 0
 * for one that is not known (returns -1 then) */
static int hw_bsp_fill(uint8_t* b, const VdpPictureInfoH264* p, int (*row_of)(VdpVideoSurface))
{
    memset(b, 0, BSP_SIZE);
    bsp_int(b, BSP_FLAGS, (p->is_reference ? 1 : 0) | (p->field_pic_flag ? 4 : 0) | (p->bottom_field_flag ? 8 : 0));
    bsp_int(b, BSP_LOG2_MAX_FRAME_NUM, p->log2_max_frame_num_minus4);
    bsp_int(b, BSP_POC_TYPE, p->pic_order_cnt_type);
    bsp_int(b, BSP_LOG2_MAX_POC_LSB, p->log2_max_pic_order_cnt_lsb_minus4);
    bsp_int(b, BSP_DELTA_POC_ZERO, p->delta_pic_order_always_zero_flag);
    bsp_int(b, BSP_FRAME_MBS_ONLY, p->frame_mbs_only_flag);
    bsp_int(b, BSP_MBAFF, p->mb_adaptive_frame_field_flag);
    bsp_int(b, BSP_DIRECT_8X8, p->direct_8x8_inference_flag);
    bsp_int(b, BSP_CABAC, p->entropy_coding_mode_flag);
    bsp_int(b, BSP_POC_PRESENT, p->pic_order_present_flag);
    bsp_int(b, BSP_REF_L0, p->num_ref_idx_l0_active_minus1);
    bsp_int(b, BSP_REF_L1, p->num_ref_idx_l1_active_minus1);
    bsp_int(b, BSP_WEIGHTED_PRED, p->weighted_pred_flag);
    bsp_int(b, BSP_WEIGHTED_BIPRED, p->weighted_bipred_idc);
    bsp_int(b, BSP_PIC_INIT_QP, p->pic_init_qp_minus26);
    bsp_int(b, BSP_CHROMA_QP, p->chroma_qp_index_offset);
    bsp_int(b, BSP_DEBLOCK_PRESENT, p->deblocking_filter_control_present_flag);
    bsp_int(b, BSP_CONSTRAINED_INTRA, p->constrained_intra_pred_flag);
    bsp_int(b, BSP_REDUNDANT_PIC_CNT, p->redundant_pic_cnt_present_flag);
    bsp_int(b, BSP_TRANSFORM_8X8, p->transform_8x8_mode_flag);
    bsp_int(b, BSP_SECOND_CHROMA_QP, p->second_chroma_qp_index_offset);
    bsp_int(b, BSP_POC0, p->field_order_cnt[0]);
    bsp_int(b, BSP_POC1, p->field_order_cnt[1]);
    for (int i = 0; i < 16; ++i) {
        const VdpReferenceFrameH264* r = &p->referenceFrames[i];
        uint8_t* e = b + BSP_REFS + BSP_REF_SIZE * i;
        const int flags = (r->top_is_reference ? 1 : 0) | (r->bottom_is_reference ? 2 : 0);
        if (r->surface == VDP_INVALID_HANDLE || !flags)
            continue;
        const int row = row_of(r->surface);
        if (row < 0)
            return -1;
        bsp_int(e, 0, row);
        bsp_int(e, 4, flags);
        e[8] = r->is_long_term ? 1 : 0;
        bsp_int(e, 0xc, r->frame_idx);
        bsp_int(e, 0x10, r->field_order_cnt[0]);
        bsp_int(e, 0x14, r->field_order_cnt[1]);
    }
    bsp_int(b, BSP_NUM_REF_FRAMES, p->num_ref_frames);
    bsp_int(b, BSP_FRAME_NUM, p->frame_num);
    bsp_int(b, BSP_SLICES, (int32_t)p->slice_count);
    return 0;
}

#endif /* NBT_HWDEC_H */
