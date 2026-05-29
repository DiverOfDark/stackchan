#include "transport/peer.hpp"

#include <cstring>
#include <utility>

#include "esp_log.h"

namespace {
constexpr const char* kTag = "peer";
} // namespace

namespace transport {

esp_err_t Peer::initLibpeerOnce()
{
    static bool inited = false;
    if (inited) return ESP_OK;
    if (peer_init() != 0) {
        ESP_LOGE(kTag, "peer_init failed (libsrtp / usrsctp init)");
        return ESP_FAIL;
    }
    inited = true;
    return ESP_OK;
}

PeerState fromLibpeer(PeerConnectionState s)
{
    switch (s) {
    case PEER_CONNECTION_NEW:          return PeerState::New;
    case PEER_CONNECTION_CHECKING:     return PeerState::Checking;
    case PEER_CONNECTION_CONNECTED:    return PeerState::Connected;
    case PEER_CONNECTION_COMPLETED:    return PeerState::Completed;
    case PEER_CONNECTION_FAILED:       return PeerState::Failed;
    case PEER_CONNECTION_DISCONNECTED: return PeerState::Disconnected;
    case PEER_CONNECTION_CLOSED:       return PeerState::Closed;
    }
    return PeerState::Failed;
}

std::unique_ptr<Peer> Peer::create(const std::vector<PeerIceServer>& ice)
{
    auto p   = std::unique_ptr<Peer>(new Peer);
    p->ice_  = ice;

    PeerConfiguration cfg{};
    // G.711 µ-law @ 8 kHz, not Opus: Opus's >120 KB SILK scratch only fits in
    // PSRAM here and ran ~1.5× real time, stalling audio capture. G.711 encode/
    // decode are free, so the pipeline stays real time. libpeer drives the
    // PCMU SDP + RTP (PT 0, 8 kHz clock); the app does the µ-law companding.
    cfg.audio_codec  = CODEC_PCMU;
    cfg.video_codec  = CODEC_NONE;
    cfg.datachannel  = DATA_CHANNEL_NONE;
    cfg.onaudiotrack = &Peer::thunkOnAudio;
    cfg.user_data    = p.get();   // unique_ptr keeps the address stable

    const std::size_t n = std::min<std::size_t>(p->ice_.size(), 5);
    for (std::size_t i = 0; i < n; ++i) {
        cfg.ice_servers[i].urls       = p->ice_[i].url.c_str();
        cfg.ice_servers[i].username   = p->ice_[i].username.c_str();
        cfg.ice_servers[i].credential = p->ice_[i].credential.c_str();
    }

    p->pc_ = peer_connection_create(&cfg);
    if (!p->pc_) {
        ESP_LOGE(kTag, "peer_connection_create failed");
        return nullptr;
    }
    peer_connection_oniceconnectionstatechange(p->pc_, &Peer::thunkOnState);
    peer_connection_onicecandidate            (p->pc_, &Peer::thunkOnSdp);
    return p;
}

Peer::~Peer()
{
    close();
    // Drain any in-flight pending answer.
    if (auto* p = pending_answer_.exchange(nullptr)) delete p;
}

void Peer::setOnStateChange(OnStateChange cb) { on_state_  = std::move(cb); }
void Peer::setOnLocalSdp   (OnLocalSdp    cb) { on_sdp_    = std::move(cb); }
void Peer::setOnAudio      (OnInboundAudio cb) { on_audio_ = std::move(cb); }

const char* Peer::createOffer()
{
    return pc_ ? peer_connection_create_offer(pc_) : nullptr;
}

void Peer::publishAnswer(std::string sdp)
{
    auto* heap = new std::string(std::move(sdp));
    auto* old  = pending_answer_.exchange(heap);
    delete old;
}

void Peer::tick()
{
    if (!pc_) return;
    // Order matters: pump libpeer FIRST (drains the UDP queue, advances
    // ICE/DTLS state), THEN apply any pending SDP answer. Doing
    // set_remote_description before the loop on tick #N+1 — at the
    // moment DTLS keys aren't derived yet — can let a stray packet
    // hit srtp_unprotect with srtp_in == NULL and crash.
    peer_connection_loop(pc_);
    if (auto* parked = pending_answer_.exchange(nullptr)) {
        peer_connection_set_remote_description(pc_, parked->c_str(), SDP_TYPE_ANSWER);
        delete parked;
    }
}

int Peer::sendAudio(const uint8_t* opus, std::size_t bytes)
{
    if (!pc_ || !opus) return -1;
    return peer_connection_send_audio(pc_, opus, bytes);
}

void Peer::close()
{
    if (!pc_) return;
    peer_connection_close(pc_);
    peer_connection_destroy(pc_);
    pc_ = nullptr;
}

PeerState Peer::currentState() const
{
    return pc_ ? fromLibpeer(peer_connection_get_state(pc_)) : PeerState::Closed;
}

// ---------- libpeer C-callback thunks ------------------------------------

void Peer::thunkOnState(PeerConnectionState s, void* ud)
{
    auto* self = static_cast<Peer*>(ud);
    if (self && self->on_state_) self->on_state_(fromLibpeer(s));
}

void Peer::thunkOnSdp(char* sdp, void* ud)
{
    auto* self = static_cast<Peer*>(ud);
    if (self && self->on_sdp_ && sdp) self->on_sdp_(std::string(sdp));
}

void Peer::thunkOnAudio(uint8_t* d, std::size_t n, void* ud)
{
    auto* self = static_cast<Peer*>(ud);
    if (self && self->on_audio_) self->on_audio_(d, n);
}

} // namespace transport
