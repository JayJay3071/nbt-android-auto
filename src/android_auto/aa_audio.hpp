// aa_audio: optional Android Auto sound over the Android Auto connection (part of oneplus_native).
//
// Active when NBT_AUDIO_MODE=browser (native_service's default since 2026-11-15; bt-audio switches back). Then the NBT-AAD core's
// BrowserAudio (upstream milestone-7/src/audio/browser_audio.*) takes the phone's media (ch 9), guidance (ch 8) and
// system-audio (ch 16) PCM, mixes it and plays it through the car's browser audio front end
// (/opt/mm/lib/libasound/libasound.so -> dev-libasound -> A2I input_stereo_3). Plan and open points:
// docs/AA-AUDIO-PLAN.md.
//
// Routing (making the car actually play input_stereo_3): BrowserAudio asks for ConnectionMaster connection
// NBT_BROWSER_CONN (0xb8) through an `audioconn`-compatible program (NBT_AUDIOCONN). The original NBT has no audioconn:
// knob/nbt_audioconn.cpp (installed as audioconn) sends the ConnectionMaster request HmiMain3D uses (FBlock 03 Fkt 053).
#ifndef NBT_AA_AUDIO_HPP
#define NBT_AA_AUDIO_HPP

namespace nbt {
class AASession;
}

namespace aa_audio {

// Before session.start(): when browser mode is on, creates the backend and registers the session's audio callbacks.
// Returns true when audio is active for this session.
bool setup(nbt::AASession& session);
// After session.stop() (the session thread has ended): ends the session for the backend and shuts it down.
void shutdown();

} // namespace aa_audio

#endif
