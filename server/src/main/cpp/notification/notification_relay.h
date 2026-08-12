#ifndef NOTIFICATION_RELAY_H
#define NOTIFICATION_RELAY_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>

namespace scrcpy {

// Notification relay (M1): bridges the companion APK and the PC channel.
//
//   companion APK --(TCP 127.0.0.1:29748, 4B BE length + tc::Message)--> relay
//   relay --(raw envelope bytes)--> WebSocket (g_wsServer) --> PC
//   PC --(kNotificationControl)--> relay --(framed)--> companion APK
//
// Security: the first frame on a new TCP connection must be
// kNotificationHandshake whose token matches /data/local/tmp/mivox_agent_token
// (written by PC via adb, 0600). If the token file is missing/unreadable the
// relay rejects every connection (logged, never crashes).
//
// Threading model (mirrors PocoWebsocketServer's lifetime rules):
//   Accept thread (relay-lifetime): accept(); a new connection closes and
//                                   replaces the previous one (single client).
//   Client thread (per connection): blocking frame read -> verify/forward.
// Stop() closes the listen + client sockets to unblock both threads, then
// joins them (accept thread joins the client thread on exit).
class NotificationRelay {
public:
    // Returns false when there is no PC-side channel; bytes are then dropped
    // (the relay logs such drops, throttled).
    using SendToPcHandler = std::function<bool(const uint8_t* data, size_t len)>;

    static constexpr int kDefaultPort = 29748;
    static constexpr size_t kMaxFrameSize = 64 * 1024;  // single frame cap
    static constexpr const char* kTokenFilePath = "/data/local/tmp/mivox_agent_token";

    NotificationRelay();
    ~NotificationRelay();

    bool Start(int port = kDefaultPort);
    void Stop();
    bool IsRunning() const;

    // True once an agent APK is connected AND handshake-verified.
    bool HasAgent() const;

    // PC -> agent: prepend the 4-byte BE length header and write to the agent
    // socket. Thread-safe (called from the WebSocket work thread). Returns
    // false when no verified agent is connected.
    bool SendControl(const uint8_t* data, size_t len);

    void SetSendToPcHandler(SendToPcHandler handler);

private:
    void AcceptLoop();
    void ClientLoop(Poco::Net::StreamSocket socket);

    bool ReadExact(Poco::Net::StreamSocket& socket, uint8_t* buf, size_t len);
    bool ReadFrame(Poco::Net::StreamSocket& socket, std::vector<uint8_t>& out);
    bool VerifyHandshake(const uint8_t* data, size_t len, std::string& agentVersionOut);
    std::string ReadTokenFile();

    // Caller must hold m_clientMutex.
    void CloseClientLocked();

    std::atomic<bool> m_running{false};

    std::unique_ptr<Poco::Net::ServerSocket> m_listenSocket;
    std::thread m_acceptThread;
    std::thread m_clientThread;

    mutable std::mutex m_clientMutex;
    std::optional<Poco::Net::StreamSocket> m_clientSocket;
    std::atomic<bool> m_clientAuthed{false};

    SendToPcHandler m_sendToPc;

    // Throttled-drop counters (log 1st, then every 60th).
    std::atomic<uint64_t> m_dropNoPcCount{0};
    std::atomic<uint64_t> m_dropBadTypeCount{0};
};

}  // namespace scrcpy

#endif  // NOTIFICATION_RELAY_H
