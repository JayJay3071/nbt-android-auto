// aa_media_out: Android Auto sound into one io-amp input of the NBT (2026-11-18).
//
// Car 2026-11-18 (nbt_key cm 053 + nbt_a2i_beep): ConnectionMaster connection f6 is a main audio source the amplifier
// builds without a medium; iDrive keeps it selected, shows no player for it and does nothing on the steering wheel
// rocker, and it plays io-amp input input_stereo_1. So Android Auto music (channel 9) goes there and nbt_knob makes f6
// the Android Auto audio source (switch to it on play / NAV in Android Auto, pause the phone when it is left).
//
// The phone's streams are mixed into one 48 kHz 16-bit stereo stream (the format the A2I beep used):
//   ch 9  media      48 kHz stereo
//   ch 8  guidance   48 kHz mono (NBT_GUIDANCE_RATE), navigation prompts
//   ch 16 system     16 kHz mono (if the phone uses it)
// Mono is doubled, other rates are interpolated to 48 kHz. Media is turned down while a prompt or system sound plays.
// Each stream waits for a small reserve before it plays (and again after running dry), and media is cut back when the
// phone has queued more than ~0.5 s, so the delay stays short.
//
// Flow control: the session acks the phone's audio frames only once they are played (AASession deferred acks); this
// module reports what left its queues through the consumed callback, so the phone sends at the speed the car plays.
//
// The A2I client library is loaded with dlopen (/opt/sys/lib/liba2i_client.so, as nbt_a2i_beep): nothing extra is
// needed to build. A writer thread fills A2I buffers as the input asks for them (silence while nothing plays).
#ifndef AA_MEDIA_OUT_HPP
#define AA_MEDIA_OUT_HPP

#include <stddef.h>
#include <stdint.h>

namespace nbt {
namespace mediaout {

// Opens the A2I input (e.g. "input_stereo_1") and starts the writer thread. False if the library or the input is not
// available (then nothing else here does anything).
bool start(const char* endpoint);
// Called from the writer thread with the phone bytes (in the stream's own format) that were played or dropped, for the
// deferred acks (AASession::postAudioConsumed). Set before start().
typedef void (*ConsumedFn)(uint8_t channel, size_t bytes, void* user);
void setConsumedCallback(ConsumedFn fn, void* user);
// Stops the writer thread and closes the input. Safe to call when start() failed or was not called.
void stop();
// AASession callbacks (session thread). Never block.
void onAudio(uint8_t channel, const uint8_t* pcm, size_t len);
void onState(uint8_t channel, bool started, uint32_t rate, uint32_t channels);

} // namespace mediaout
} // namespace nbt

#endif // AA_MEDIA_OUT_HPP
