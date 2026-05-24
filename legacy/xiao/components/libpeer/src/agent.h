#ifndef AGENT_H_
#define AGENT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base64.h"
#include "ice.h"
#include "socket.h"
#include "stun.h"
#include "utils.h"

#ifndef AGENT_MAX_DESCRIPTION
#define AGENT_MAX_DESCRIPTION 40960
#endif

#ifndef AGENT_MAX_CANDIDATES
#define AGENT_MAX_CANDIDATES 10
#endif

#ifndef AGENT_MAX_CANDIDATE_PAIRS
#define AGENT_MAX_CANDIDATE_PAIRS 100
#endif

typedef enum AgentState {

  AGENT_STATE_GATHERING_ENDED = 0,
  AGENT_STATE_GATHERING_STARTED,
  AGENT_STATE_GATHERING_COMPLETED,

} AgentState;

typedef enum AgentMode {

  AGENT_MODE_CONTROLLED = 0,
  AGENT_MODE_CONTROLLING

} AgentMode;

typedef struct Agent Agent;

struct Agent {
  char remote_ufrag[ICE_UFRAG_LENGTH + 1];
  char remote_upwd[ICE_UPWD_LENGTH + 1];

  char local_ufrag[ICE_UFRAG_LENGTH + 1];
  char local_upwd[ICE_UPWD_LENGTH + 1];

  IceCandidate local_candidates[AGENT_MAX_CANDIDATES];
  IceCandidate remote_candidates[AGENT_MAX_CANDIDATES];

  int local_candidates_count;
  int remote_candidates_count;

  UdpSocket udp_sockets[2];

  Address host_addr;
  int b_host_addr;
  uint64_t binding_request_time;
  AgentState state;

  AgentMode mode;

  IceCandidatePair candidate_pairs[AGENT_MAX_CANDIDATE_PAIRS];
  IceCandidatePair* selected_pair;
  IceCandidatePair* nominated_pair;

  int candidate_pairs_num;
  int use_candidate;
  uint32_t transaction_id[3];

  /* --- TURN client state (set by agent_create_turn_addr after a
   * successful Allocate response; consumed when sending CreatePermission,
   * Send indications, and Refresh) ---
   *
   * turn_server_addr — where to send TURN messages (NOT the relayed
   *                    address; that lives in local_candidates as a
   *                    RELAY-type candidate).
   * turn_username / turn_credential — long-term cred used for MESSAGE-
   *                    INTEGRITY on every TURN request after Allocate.
   *                    Allocate itself does an unauthenticated probe first
   *                    and the server returns realm+nonce in the 401
   *                    response, which is then folded into the retry. We
   *                    cache them on the Agent so subsequent
   *                    CreatePermission / Refresh / etc. don't have to
   *                    re-do the challenge dance every time.
   * turn_realm / turn_nonce — server-provided long-term-cred parameters.
   *                    Nonce can rotate (server returns 438 with a fresh
   *                    nonce); CreatePermission code handles that.
   * turn_allocated — true after Allocate succeeded and a RELAY candidate
   *                  was added to local_candidates.
   * turn_permission_addr / turn_permission_set — the peer address we last
   *                    asked for CreatePermission on, and whether the
   *                    server acked it. We only support one active
   *                    permission at a time (matches our single
   *                    nominated_pair model). */
  Address turn_server_addr;
  char turn_username[128];
  char turn_credential[64];
  char turn_realm[64];
  char turn_nonce[64];
  bool turn_allocated;
  Address turn_permission_addr;
  bool turn_permission_set;
};

void agent_gather_candidate(Agent* agent, const char* urls, const char* username, const char* credential);

void agent_create_ice_credential(Agent* agent);

void agent_get_local_description(Agent* agent, char* description, int length);

int agent_send(Agent* agent, const uint8_t* buf, int len);

int agent_recv(Agent* agent, uint8_t* buf, int len);

void agent_set_remote_description(Agent* agent, char* description);

int agent_select_candidate_pair(Agent* agent);

int agent_connectivity_check(Agent* agent);

void agent_clear_candidates(Agent* agent);

int agent_create(Agent* agent);

void agent_destroy(Agent* agent);

void agent_update_candidate_pairs(Agent* agent);

/* Ask the TURN server to permit the given peer address to send to us
 * through our allocation. Must be called before any Send indication to
 * that peer (RFC 5766 §9). Returns 0 on success, -1 on failure.
 * Safe to call repeatedly with the same address — re-issues the permission
 * (each one is valid for 5 min; this also acts as a refresh). */
int agent_turn_set_permission(Agent* agent, Address* peer_addr);

#endif  // AGENT_H_
