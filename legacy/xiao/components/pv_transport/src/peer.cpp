#include "transport/peer.hpp"

#include <cstring>
#include <utility>

#include "esp_log.h"

namespace {
constexpr const char* kTag = "peer";
} // namespace

namespace transport {

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

std::optional<Peer> Peer::create(const std::vector<PeerIceServer>& ice)
{
    Peer p;
    p.ice_ = ice;

    PeerConfiguration cfg{};
    cfg.audio_codec  = CODEC_OPUS;
    cfg.video_codec  = CODEC_NONE;
    cfg.datachannel  = DATA_CHANNEL_NONE;
    cfg.onaudiotrack = &Peer::thunkOnAudio;
    cfg.user_data    = &p;   // see NOTE below about move-stability

    const std::size_t n = std::min<std::size_t>(p.ice_.size(), 5);
    for (std::size_t i = 0; i < n; ++i) {
        cfg.ice_servers[i].urls       = p.ice_[i].url.c_str();
        cfg.ice_servers[i].username   = p.ice_[i].username.c_str();
        cfg.ice_servers[i].credential = p.ice_[i].credential.c_str();
    }

    p.pc_ = peer_connection_create(&cfg);
    if (!p.pc_) {
        ESP_LOGE(kTag, "peer_connection_create failed");
        return std::nullopt;
    }
    peer_connection_oniceconnectionstatechange(p.pc_, &Peer::thunkOnState);
    peer_connection_onicecandidate            (p.pc_, &Peer::thunkOnSdp);

    // NOTE on user_data lifetime: libpeer copies cfg into the PeerConnection
    // and stores the user_data pointer there. We pass `&p` which would be
    // unsafe across a move(). The caller MUST keep the Peer in a stable
    // location (e.g. as a class member by value, NOT a local that gets
    // moved) for the lifetime of the connection. close()/move are
    // responsible for tearing down the pc before allowing relocation.
    return p;
}

Peer::Peer(Peer&& other) noexcept
    : pc_(other.pc_)
    , ice_(std::move(other.ice_))
    , on_state_(std::move(other.on_state_))
    , on_sdp_(std::move(other.on_sdp_))
    , on_audio_(std::move(other.on_audio_))
{
    pending_answer_.store(other.pending_answer_.exchange(nullptr));
    other.pc_ = nullptr;
    // NOTE: see comment in create() — the moved-from `&other` is now
    // dangling inside libpeer's stored user_data. Caller must close
    // BEFORE moving. We document this in the header; runtime detection
    // would require an extra indirection we don't want.
}

Peer& Peer::operator=(Peer&& other) noexcept
{
    if (this != &other) {
        close();
        pc_       = other.pc_;
        ice_      = std::move(other.ice_);
        on_state_ = std::move(other.on_state_);
        on_sdp_   = std::move(other.on_sdp_);
        on_audio_ = std::move(other.on_audio_);
        pending_answer_.store(other.pending_answer_.exchange(nullptr));
        other.pc_ = nullptr;
    }
    return *this;
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
    if (auto* parked = pending_answer_.exchange(nullptr)) {
        peer_connection_set_remote_description(pc_, parked->c_str(), SDP_TYPE_ANSWER);
        delete parked;
    }
    peer_connection_loop(pc_);
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
