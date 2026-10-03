#include "femto_voice.h"

#include <cstring>
#include "esp_log.h"
#include "session.hpp"

namespace {
femto::AudioCores3 s_audio;
femto::Session *s_session = nullptr;
}  // namespace

extern "C" esp_err_t femto_voice_start(const char *backend_url, int i2c_port, int volume, const char *request_data_json)
{
    if (s_session) return ESP_OK;
    esp_err_t err = s_audio.init(i2c_port, volume);
    if (err != ESP_OK) return err;
    // Lives for the program's lifetime (libpeer keeps pointers into it).
    s_session = new femto::Session(backend_url ? backend_url : "", s_audio);
    if (request_data_json) s_session->setRequestData(request_data_json);
    s_session->start();
    return ESP_OK;
}

extern "C" femto_voice_state_t femto_voice_state(void) { return s_session ? s_session->state() : FEMTO_VOICE_IDLE; }
extern "C" float femto_voice_level(void) { return s_session ? s_session->level() : 0.0f; }
extern "C" float femto_voice_mic_level(void) { return s_session ? s_session->micLevel() : 0.0f; }
extern "C" void femto_voice_wake(void) { if (s_session) s_session->wake(); }
extern "C" void femto_voice_set_volume(int volume) { s_audio.setVolume(volume); }

extern "C" size_t femto_voice_next_event(char *out, size_t cap)
{
    std::string ev;
    if (!s_session || !s_session->nextEvent(ev) || cap == 0) return 0;
    const size_t n = ev.size() < cap ? ev.size() : cap;
    memcpy(out, ev.data(), n);
    return n;
}
