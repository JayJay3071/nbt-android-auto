/* Stand-in for libvdpau's vdpau_x11.h: FFmpeg's configure (run once on Linux, see scripts/ffmpeg_qnx.py) wants both
 * VDPAU headers before it enables its VDPAU code, and libavcodec/vdpau.h includes this one. nbt_decoder uses FFmpeg's VDPAU interface for the car's own
 * hardware H.264 decoder (libnmsdk), there is no X11 and no libvdpau. */
#include "vdpau.h"
