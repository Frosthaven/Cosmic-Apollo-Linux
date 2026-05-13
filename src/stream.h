/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);

    /**
     * @brief Lock-free read of the running_sessions atomic counter.
     *        Safe to call from videoThread / OutputRestoreGuard while
     *        rtsp_server_t::clear() holds _session_slots.lock(), where
     *        rtsp_stream::session_count() would deadlock because it
     *        internally tries to reacquire the same lock.
     *        Counter is incremented at session::start, decremented at
     *        the end of session::join (after videoThread.join()), so
     *        during the destructor's run our own session is still
     *        counted — `active_session_count() > 1` means "another
     *        client is still streaming besides me".
     */
    unsigned int active_session_count();
  }  // namespace session
}  // namespace stream
