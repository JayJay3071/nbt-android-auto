#ifndef NBT_AA_VIDEO_CAPTURE_H
#define NBT_AA_VIDEO_CAPTURE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One reader and one writer run concurrently. Transfer must bound its waits
 * and use separate buffers/URBs for each direction. Returns 1 on success. */
typedef int (*nbt_capture_transfer)(void*, int input, unsigned char*, unsigned*);
int nbt_video_capture_run(nbt_capture_transfer transfer, void* context, const char* prefix);
#ifdef __cplusplus
}
#endif
#endif
