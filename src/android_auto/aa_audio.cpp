// aa_audio: optional Android Auto sound over the Android Auto connection. See aa_audio.hpp.
//
// Wiring, as in NBT-AAD's main.cpp (browser mode), for one session per process:
//   AASession audio frame / stream state (session thread) --> BrowserAudio::onFrame / onStreamState
//   BrowserAudio consumed bytes / underflow (mixer thread)  --> AASession::postAudioConsumed / postAudioUnderflow
// With deferred acks (NBT_AA_DEFER_ACKS, default on) the phone is paced by the car's audio clock: a frame is acked once
// the mixer has taken it, or after NBT_AA_ACK_FORCE_MS at the latest, so no device / no route never stalls the session.
#include "aa_audio.hpp"
#include "aa_session.hpp"
#include "audio/audio_acks.hpp"
#include "audio/browser_audio.hpp"
#include "audio/config.hpp"
#include "audio/mic_uplink.hpp"
#include <stdio.h>

namespace {

nbt::BrowserAudio* g_audio = 0;

void on_frame(uint8_t channel, const uint8_t* pcm, size_t len, uint64_t pts, void*)
{
    if (g_audio)
        g_audio->onFrame(channel, pcm, len, pts);
}

void on_state(uint8_t channel, bool started, uint32_t rate, uint32_t channels, void*)
{
    if (g_audio)
        g_audio->onStreamState(channel, started, rate, channels);
}

void on_consumed(uint8_t channel, size_t bytes, void* session)
{
    static_cast<nbt::AASession*>(session)->postAudioConsumed(channel, bytes);
}

void on_underflow(uint8_t channel, void* session)
{
    static_cast<nbt::AASession*>(session)->postAudioUnderflow(channel);
}

} // namespace

namespace aa_audio {

bool setup(nbt::AASession& session)
{
    if (!nbt::browserAudioEnabled()) {
        printf("AA_AUDIO off (Bluetooth; NBT_AUDIO_MODE=%s)\n", nbt::audioModeName(nbt::audioMode()));
        return false;
    }
    const nbt::BrowserConfig cfg = nbt::readBrowserConfig();
    g_audio = new nbt::BrowserAudio();
    if (!g_audio->init(cfg))
        printf("AA_AUDIO WARNING: libasound shim %s not loaded: Android Auto audio will be silent\n", cfg.shim_path);
    if (cfg.defer_acks) {
        g_audio->setConsumedCallback(on_consumed, &session);
        g_audio->setAckWindow(nbt::AudioAcks::window());
    }
    session.setDeferredAudioAcks(cfg.defer_acks);
    if (cfg.underflow_reports)
        g_audio->setUnderflowCallback(on_underflow, &session);
    session.setUnderflowReports(cfg.underflow_reports);
    session.setAudioFrameCallback(on_frame, 0);
    session.setAudioStateCallback(on_state, 0);
    g_audio->start();
    g_audio->onSessionStarted();
    printf("AA_AUDIO browser mode: connection 0x%x%s, acks %s, underflow reports %s\n", (unsigned)cfg.conn,
           cfg.conn ? "" : " (not routed: plays only if the car already listens to input_stereo_3)",
           cfg.defer_acks ? "once played" : "on arrival", cfg.underflow_reports ? "on" : "off");
    return true;
}

void shutdown()
{
    if (!g_audio)
        return;
    g_audio->cancelPending();
    g_audio->onSessionEnded();
    g_audio->stop();
    delete g_audio;
    g_audio = 0;
    printf("AA_AUDIO stopped\n");
}

} // namespace aa_audio

// ---- link stubs ------------------------------------------------------------------------------------------------------
// AudioBackend's default startMic()/stopMic() (upstream audio/backend.hpp) reference MicUplink, which links the car's
// A2I client library directly. oneplus_native never starts the car microphone, so these two definitions replace
// upstream's mic_uplink.cpp / a2i_stream.cpp instead of adding liba2i_client.so as a load-time dependency.
bool nbt::MicUplink::start()
{
    printf("AA_AUDIO car microphone not supported\n");
    return false;
}

void nbt::A2ICaptureStream::stop() {}
