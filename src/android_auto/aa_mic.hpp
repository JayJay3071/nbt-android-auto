// aa_mic: the car's microphone to the phone (Google Assistant, voice search), 2026-11-20.
//
// The phone opens the microphone over the Android Auto connection (channel 14, offered by the NBT-AAD service
// discovery; AASession answers the request and sends what postMicData gets). While it is open, a thread captures from
// the car's audio manager through the A2I client (liba2i_client, loaded with dlopen as in aa_media_out):
//   endpoint /output_multich_0, 22050 Hz, 4 channels, 16-bit (the microphone bus of the plain NBT; the NBT Evo has an
//   echo-cancelled /output_mono_AirPlayUL instead, which this NBT does not offer)
// and sends the phone 16 kHz mono 16-bit: channels 0 and 1 averaged, interpolated to 16 kHz, amplified, 16 ms chunks.
// No echo cancellation: the phone turns its own sound down while the Assistant listens.
//
// Environment (set by native_service or by hand):
//   NBT_MIC=0            microphone off (the phone's request is still answered, it just gets no sound)
//   NBT_MIC_GAIN=n       amplification 1..16 (default 4)
//   NBT_MIC_ENDPOINT, NBT_MIC_RATE, NBT_MIC_CHANNELS   another A2I endpoint and its format
#ifndef AA_MIC_HPP
#define AA_MIC_HPP

namespace nbt {
class AASession;
namespace mic {

// Loads the A2I library, starts the capture thread and registers for the phone's microphone requests. False if the
// microphone is off or the library is not available (the session then answers the requests without sound).
bool start(AASession& session);
// Ends a running capture and the thread. Safe to call when start() failed or was not called. Before session.stop().
void stop();

} // namespace mic
} // namespace nbt

#endif // AA_MIC_HPP
