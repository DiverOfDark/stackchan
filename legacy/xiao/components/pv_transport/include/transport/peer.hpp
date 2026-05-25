#pragma once

// RAII C++17 wrapper around libpeer's PeerConnection.
//
// Owns:
//   * the PeerConnection* (destroyed in ~Peer)
//   * the ICE-server backing strings (libpeer keeps const char* into
//     them — must outlive the connection; lives here)
//   * the atomic SDP handoff (on_local_sdp publishes, the main loop
//     consumes; this class owns the std::atomic<std::string*> instead
//     of leaving the race surface scattered)
//
// Exposes:
//   * std::function callbacks for state-change, local-SDP-ready, and
//     inbound audio packets. The C void* userdata juggling stays
//     inside this class.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "esp_err.h"
extern "C" {
#include "peer.h"
}

namespace transport {

struct PeerIceServer {
    std::string url;
    std::string username;
    std::string credential;
};

enum class PeerState : uint8_t {
    New, Checking, Connected, Completed, Failed, Disconnected, Closed,
};
PeerState fromLibpeer(PeerConnectionState s);

class Peer {
public:
    using OnStateChange    = std::function<void(PeerState)>;
    using OnLocalSdp       = std::function<void(std::string)>;
    using OnInboundAudio   = std::function<void(const uint8_t*, std::size_t)>;

    // Process-global libpeer init (initialises libsrtp + usrsctp).
    // Must be called once before any Peer::create(). Idempotent.
    static esp_err_t initLibpeerOnce();

    // Factory. Returns a unique_ptr so the address of *this stays
    // stable for the lifetime of the libpeer connection — libpeer
    // stores `Peer*` in its config.user_data and dereferences it on
    // every callback. A move-able Peer would invalidate that pointer
    // the first time it gets relocated; std::unique_ptr keeps the
    // object pinned in place.
    static std::unique_ptr<Peer> create(const std::vector<PeerIceServer>& ice);

    // Non-copyable, non-movable for the same reason — only the
    // unique_ptr moves; the underlying object stays put.
    Peer(const Peer&)            = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&)                 = delete;
    Peer& operator=(Peer&&)      = delete;
    ~Peer();

    void setOnStateChange(OnStateChange cb);
    void setOnLocalSdp   (OnLocalSdp    cb);
    void setOnAudio      (OnInboundAudio cb);

    // Build the local SDP offer; libpeer gathers synchronously and
    // calls the on_local_sdp callback with the full SDP.
    const char* createOffer();

    // Park the remote SDP answer for delivery to libpeer on the main
    // loop. Safe to call from the on_local_sdp callback thread —
    // atomic_exchange under the hood.
    void publishAnswer(std::string sdp);

    // Called from main_loop_task each tick: feeds any parked answer
    // into libpeer, then pumps peer_connection_loop().
    void tick();

    // Send a bare Opus payload (libpeer handles RTP framing + SRTP).
    // Returns the libpeer return code (≥0 on success).
    int sendAudio(const uint8_t* opus, std::size_t bytes);

    // Close + destroy the peer connection. The Peer is reusable after
    // this only via move-assignment from a fresh create().
    void close();

    PeerState currentState() const;

private:
    Peer() = default;

    // libpeer-facing static thunks.
    static void thunkOnState(PeerConnectionState s, void* ud);
    static void thunkOnSdp  (char* sdp, void* ud);
    static void thunkOnAudio(uint8_t* d, std::size_t n, void* ud);

    PeerConnection*           pc_ = nullptr;
    std::vector<PeerIceServer> ice_;        // backing strings — must outlive pc_
    std::atomic<std::string*> pending_answer_{nullptr};

    OnStateChange  on_state_;
    OnLocalSdp     on_sdp_;
    OnInboundAudio on_audio_;
};

} // namespace transport
